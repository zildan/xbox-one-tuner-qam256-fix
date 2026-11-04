#include "unified_device.h"
#include "altdvb_interface.h"
#include <stdio.h>
#include <stdarg.h>
#include <float.h>

void log_dbg(const char *fmt, ...) {
    FILE *f = fopen("D:\\temporare\\unified_debug.log", "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
}

static STR_CB_PROC g_stream_cb = NULL;
static MSG_CB_PROC g_msg_cb = NULL;
static struct TUNE_DATA g_last_tune = {0};

/* Adaptive Signal Polling State */
static volatile DWORD g_last_tune_time = 0;
static volatile bool g_is_locked = false;
static int g_last_strength = 0;
static int g_last_snr = 0;
static DWORD g_last_stat_query = 0;

unified_device_t g_unified_dev = {0};
HINSTANCE g_hModule = NULL;
char g_module_dir[MAX_PATH] = {0};

#define NUM_BUFFERS 16
#define TS_BUFFER_SIZE 24064 /* 128 * 188 = 47 * 512, vendor BULK_STAGESIZE */

typedef struct {
    OVERLAPPED ov;
    uint8_t *buffer;
    ULONG read_bytes;
} stream_buf_t;

static inline void clear_fpu(void) {
    __asm__ __volatile__ ("fnclex");
    _clear87();
    _control87(0x133F, 0xFFFF);
}

static unsigned __stdcall StreamThreadProc(void *arg) {
    clear_fpu();
    unified_device_t *dev = (unified_device_t *)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    log_dbg("StreamThread: ENTERING (Model=%s, EP=0x%02X)\n",
            dev->model == DEVICE_MODEL_XBOX_ONE ? "Xbox One" : "Astrometa", dev->ep_in);

    WinUsb_ResetPipe(dev->hWinUsb, dev->ep_in);

    stream_buf_t bufs[NUM_BUFFERS];
    for (int i = 0; i < NUM_BUFFERS; i++) {
        memset(&bufs[i], 0, sizeof(stream_buf_t));
        bufs[i].buffer = (uint8_t *)_aligned_malloc(TS_BUFFER_SIZE, 4096);
        if (!bufs[i].buffer) {
            log_dbg("StreamThread: _aligned_malloc failed for buffer %d!\n", i);
            return 1;
        }
        memset(bufs[i].buffer, 0, TS_BUFFER_SIZE);
        bufs[i].ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        bufs[i].read_bytes = 0;

        BOOL ok = WinUsb_ReadPipe(dev->hWinUsb, dev->ep_in, bufs[i].buffer, TS_BUFFER_SIZE, &bufs[i].read_bytes, &bufs[i].ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                log_dbg("StreamThread: initial WinUsb_ReadPipe[%d] failed, err=%u\n", i, err);
            }
        }
    }

    uint8_t leftover[256];
    ULONG leftover_len = 0;
    bool synced = false;
    int cur = 0;
    int loop_cnt = 0;
    int pkt_count = 0;

    log_dbg("StreamThread: armed 16 buffers, entering streaming loop\n");

    while (dev->is_streaming) {
        HANDLE wait_pair[2] = { dev->hStopEvent, bufs[cur].ov.hEvent };
        DWORD wait_res = WaitForMultipleObjects(2, wait_pair, FALSE, 200);
        if (wait_res == WAIT_OBJECT_0) {
            log_dbg("StreamThread: StopEvent signaled, exiting\n");
            break;
        }

        if (wait_res == WAIT_TIMEOUT) {
            loop_cnt++;
            if (loop_cnt >= 5) {
                /* Cable disconnected or stream interrupted: reset sync for clean recovery */
                synced = false;
                leftover_len = 0;
            }
            if (loop_cnt % 15 == 1) {
                log_dbg("StreamThread: WAIT_TIMEOUT on buf[%d]\n", cur);
            }
            continue;
        }

        if (wait_res == WAIT_OBJECT_0 + 1) {
            loop_cnt = 0;
            ULONG transferred = 0;
            BOOL bRes = WinUsb_GetOverlappedResult(dev->hWinUsb, &bufs[cur].ov, &transferred, FALSE);
            if (!bRes) {
                DWORD err = GetLastError();
                log_dbg("StreamThread: GetOverlappedResult err=%u\n", err);
            } else if (transferred > 0) {
                uint8_t *chunk = bufs[cur].buffer;
                ULONG chunk_pos = 0;

                /* Complete leftover packet from previous buffer */
                if (synced && leftover_len > 0) {
                    ULONG need = 188 - leftover_len;
                    if (transferred >= need) {
                        memcpy(leftover + leftover_len, chunk, need);
                        chunk_pos += need;
                        if (leftover[0] == 0x47) {
                            if (g_stream_cb) g_stream_cb(leftover);
                            pkt_count++;
                        } else {
                            synced = false;
                        }
                        leftover_len = 0;
                    }
                }

                /* Hunt for 2 consecutive 0x47 sync bytes if not synced */
                if (!synced) {
                    leftover_len = 0;
                    for (ULONG k = chunk_pos; k + 188 < transferred; k++) {
                        if (chunk[k] == 0x47 && chunk[k + 188] == 0x47) {
                            chunk_pos = k;
                            synced = true;
                            if (pkt_count == 0) {
                                log_dbg("StreamThread: Initial SYNC ACQUIRED at offset %u!\n", k);
                            }
                            break;
                        }
                    }
                }

                /* Stream full 188-byte packets with internal resync */
                if (synced) {
                    while (chunk_pos + 188 <= transferred) {
                        if (chunk[chunk_pos] == 0x47) {
                            if (g_stream_cb) g_stream_cb(&chunk[chunk_pos]);
                            pkt_count++;
                            chunk_pos += 188;
                        } else if (chunk_pos + 188 < transferred && chunk[chunk_pos + 188] == 0x47) {
                            /* Isolated corrupt/padding packet; next is already aligned */
                            chunk_pos += 188;
                        } else {
                            /* Fast internal re-hunt within the same buffer */
                            bool recovered = false;
                            for (ULONG k = chunk_pos + 1; k + 188 <= transferred; k++) {
                                if (chunk[k] == 0x47 && chunk[k + 188] == 0x47) {
                                    chunk_pos = k;
                                    recovered = true;
                                    break;
                                }
                            }
                            if (!recovered) {
                                synced = false;
                                break;
                            }
                        }
                    }

                    if (synced && chunk_pos < transferred) {
                        leftover_len = transferred - chunk_pos;
                        if (leftover_len < sizeof(leftover)) {
                            memcpy(leftover, &chunk[chunk_pos], leftover_len);
                        } else {
                            leftover_len = 0;
                        }
                    } else if (!synced) {
                        leftover_len = 0;
                    }
                }
            }

            /* Re-arm bufs[cur] at the tail of the USB pipeline */
            HANDLE hEv = bufs[cur].ov.hEvent;
            ResetEvent(hEv);
            memset(&bufs[cur].ov, 0, sizeof(OVERLAPPED));
            bufs[cur].ov.hEvent = hEv;
            bufs[cur].read_bytes = 0;
            WinUsb_ReadPipe(dev->hWinUsb, dev->ep_in, bufs[cur].buffer, TS_BUFFER_SIZE, &bufs[cur].read_bytes, &bufs[cur].ov);

            cur = (cur + 1) % NUM_BUFFERS;
        }
    }

    CancelIo(dev->hDev);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (bufs[i].ov.hEvent) {
            WaitForSingleObject(bufs[i].ov.hEvent, 50);
            CloseHandle(bufs[i].ov.hEvent);
        }
        if (bufs[i].buffer) {
            _aligned_free(bufs[i].buffer);
        }
    }

    log_dbg("StreamThread: loop finished, total streamed=%d packets\n", pkt_count);
    return 0;
}

static void StartStreaming(unified_device_t *dev) {
    if (dev->is_streaming) return;
    if (!dev->is_initialized || !g_stream_cb) return;

    log_dbg("StartStreaming: Starting TS streaming for model %d\n", (int)dev->model);
    dev->is_streaming = true;
    dev->hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (dev->model == DEVICE_MODEL_XBOX_ONE) {
        dib0700_set_streaming(dev, 1);
    } else {
        rtl2832_set_streaming(dev, true);
    }

    dev->hStreamThread = (HANDLE)_beginthreadex(NULL, 0, StreamThreadProc, dev, 0, NULL);
}

static void StopStreaming(unified_device_t *dev) {
    if (!dev->is_streaming) return;

    log_dbg("StopStreaming: Stopping TS streaming...\n");
    dev->is_streaming = false;

    if (dev->model == DEVICE_MODEL_XBOX_ONE) {
        dib0700_set_streaming(dev, 0);
    } else {
        rtl2832_set_streaming(dev, false);
    }

    if (dev->hStopEvent) {
        SetEvent(dev->hStopEvent);
    }

    if (dev->hStreamThread) {
        WaitForSingleObject(dev->hStreamThread, 1500);
        CloseHandle(dev->hStreamThread);
        dev->hStreamThread = NULL;
    }

    if (dev->hStopEvent) {
        CloseHandle(dev->hStopEvent);
        dev->hStopEvent = NULL;
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        clear_fpu();
        InitializeCriticalSection(&g_unified_dev.lock);
        g_hModule = hinstDLL;
        char path[MAX_PATH] = {0};
        if (GetModuleFileNameA(hinstDLL, path, sizeof(path))) {
            char *last_slash = strrchr(path, '\\');
            if (last_slash) {
                *(last_slash + 1) = '\0';
                strncpy(g_module_dir, path, sizeof(g_module_dir) - 1);
            }
        }
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        StopStreaming(&g_unified_dev);
        unified_usb_close(&g_unified_dev);
        DeleteCriticalSection(&g_unified_dev.lock);
    }
    return TRUE;
}

int __declspec(dllexport) __stdcall DvbDeviceControl(int msg_id, char *data) {
    clear_fpu();
    if (!data && (msg_id != DEVICE_CONFIG)) {
        return AltxDVB_OK;
    }

    switch (msg_id) {
    case DRIVER_DESCRIPTION: {
        struct DRIVER_DATA *drv = (struct DRIVER_DATA *)data;
        memset(drv, 0, sizeof(struct DRIVER_DATA));
        strncpy(drv->author, "zildan & Community", 127);
        strncpy(drv->url, "https://git.gbc.xyz/zil/Drivere-driver-tuner-xbox--si-astrometa", 127);
        strncpy(drv->description, "Unified DVB-C USB Tuner Driver (Xbox One & Astrometa Stick)", 127);
        drv->extra = 0;
        return AltxDVB_OK;
    }

    case DEVICE_DESCRIPTION: {
        struct DEVICE_DATA *dev_data = (struct DEVICE_DATA *)data;
        memset(dev_data, 0, sizeof(struct DEVICE_DATA));

        /* Probe connected hardware */
        char path_xbox[256] = {0};
        char path_astro[256] = {0};

        /* Always present both slots for clean selection in AltDVB */
        dev_data->dev_cnt = 2;

        dev_data->device[0].dev_type = DVBC | AVAILABLE;
        strncpy(dev_data->device[0].dev_name, "Xbox One DVB-C Tuner (DiB0700/MN88472/TDA18250)", 127);
        strncpy(dev_data->device[0].dev_id, "XBOX", 127);

        dev_data->device[1].dev_type = DVBC | AVAILABLE;
        strncpy(dev_data->device[1].dev_name, "Astrometa DVB-T2/C Stick (RTL2832P/MN88473/R828D)", 127);
        strncpy(dev_data->device[1].dev_id, "ASTROMETA", 127);

        return AltxDVB_OK;
    }

    case DEVICE_NAME: {
        struct DEVICE_NAME_DATA *d = (struct DEVICE_NAME_DATA *)data;
        memset(d, 0, sizeof(struct DEVICE_NAME_DATA));
        if (g_unified_dev.model == DEVICE_MODEL_XBOX_ONE) {
            strncpy(d->description, "Xbox One DVB-C Tuner", 127);
        } else if (g_unified_dev.model == DEVICE_MODEL_ASTROMETA) {
            strncpy(d->description, "Astrometa DVB-T2/C Stick", 127);
        } else {
            strncpy(d->description, "Unified DVB-C USB Tuner", 127);
        }
        return AltxDVB_OK;
    }

    case OPEN_DEVICE: {
        struct OPEN_DEVICE_DATA *open_data = (struct OPEN_DEVICE_DATA *)data;
        device_model_t target = DEVICE_MODEL_AUTO;

        if (open_data) {
            if (strstr(open_data->dev_id, "XBOX")) target = DEVICE_MODEL_XBOX_ONE;
            else if (strstr(open_data->dev_id, "ASTROMETA")) target = DEVICE_MODEL_ASTROMETA;
        }

        log_dbg("OPEN_DEVICE: Target model = %d (ID=%s)\n", (int)target, open_data ? open_data->dev_id : "NULL");

        if (!unified_usb_open(&g_unified_dev, target)) {
            log_dbg("OPEN_DEVICE: unified_usb_open failed!\n");
            return AltxDVB_ERR;
        }

        if (g_unified_dev.model == DEVICE_MODEL_XBOX_ONE) {
            if (!dib0700_init(&g_unified_dev)) {
                log_dbg("OPEN_DEVICE: dib0700_init failed!\n");
                unified_usb_close(&g_unified_dev);
                return AltxDVB_ERR;
            }
            if (!mn88472_init_device(&g_unified_dev)) {
                log_dbg("OPEN_DEVICE: mn88472_init_device failed!\n");
                unified_usb_close(&g_unified_dev);
                return AltxDVB_ERR;
            }
        } else if (g_unified_dev.model == DEVICE_MODEL_ASTROMETA) {
            if (!rtl2832_init(&g_unified_dev)) {
                log_dbg("OPEN_DEVICE: rtl2832_init failed!\n");
                unified_usb_close(&g_unified_dev);
                return AltxDVB_ERR;
            }
            if (!mn88473_init(&g_unified_dev)) {
                log_dbg("OPEN_DEVICE: mn88473_init failed!\n");
                unified_usb_close(&g_unified_dev);
                return AltxDVB_ERR;
            }
            if (!r820t_device_init(&g_unified_dev)) {
                log_dbg("OPEN_DEVICE: r820t_device_init failed!\n");
            }
        }

        g_unified_dev.is_initialized = true;
        StartStreaming(&g_unified_dev);
        return AltxDVB_OK;
    }

    case CLOSE_DEVICE: {
        StopStreaming(&g_unified_dev);
        unified_usb_close(&g_unified_dev);
        return AltxDVB_OK;
    }

    case STREAM_CALLBACK: {
        g_stream_cb = (STR_CB_PROC)data;
        StartStreaming(&g_unified_dev);
        return AltxDVB_OK;
    }

    case MESSAGE_CALLBACK: {
        g_msg_cb = (MSG_CB_PROC)data;
        return AltxDVB_OK;
    }

    case TUNE: {
        struct TUNE_DATA *tune = (struct TUNE_DATA *)data;
        memcpy(&g_last_tune, tune, sizeof(struct TUNE_DATA));

        uint32_t freq_hz;
        if (tune->frequency < 10000) {
            freq_hz = tune->frequency * 1000000;
        } else if (tune->frequency < 10000000) {
            freq_hz = tune->frequency * 1000;
        } else {
            freq_hz = tune->frequency;
        }

        uint32_t sym_rate_ks = tune->symbol_rate;
        if (sym_rate_ks == 0) sym_rate_ks = 6900;
        if (sym_rate_ks > 100000) sym_rate_ks /= 1000;

        int qam_mode = 2; /* Default 256-QAM */
        if (tune->modulation == 1) qam_mode = 0; /* 64-QAM */

        log_dbg("TUNE: freq=%u Hz, sr=%u kS/s, qam=%d, model=%d\n",
                freq_hz, sym_rate_ks, qam_mode, (int)g_unified_dev.model);

        EnterCriticalSection(&g_unified_dev.lock);

        if (g_unified_dev.model == DEVICE_MODEL_XBOX_ONE) {
            mn88472_tune_dvbc(&g_unified_dev, freq_hz, sym_rate_ks, tune->modulation);
        } else if (g_unified_dev.model == DEVICE_MODEL_ASTROMETA) {
            r820t_device_set_freq(&g_unified_dev, freq_hz);
            mn88473_tune_dvbc(&g_unified_dev, freq_hz / 1000, sym_rate_ks, qam_mode);
        }

        g_last_tune_time = GetTickCount();
        g_is_locked = false;
        LeaveCriticalSection(&g_unified_dev.lock);
        return AltxDVB_OK;
    }

    case SIGNAL_STATISTICS: {
        clear_fpu();
        struct SIGNAL_STATISTICS_DATA *sig = (struct SIGNAL_STATISTICS_DATA *)data;
        if (!sig) { clear_fpu(); return AltxDVB_OK; }
        DWORD now = GetTickCount();

        /* Continuously poll signal (400ms when locked, 150ms when searching) to support cable hot-plug */
        DWORD poll_interval = g_is_locked ? 400 : 150;
        if (now - g_last_stat_query >= poll_interval || g_last_stat_query == 0) {
            g_last_stat_query = now;
            bool locked_now = false;
            int str_now = 0, snr_now = 0;
            EnterCriticalSection(&g_unified_dev.lock);
            if (g_unified_dev.model == DEVICE_MODEL_XBOX_ONE) {
                mn88472_read_status(&g_unified_dev, &str_now, &snr_now, &locked_now);
            } else if (g_unified_dev.model == DEVICE_MODEL_ASTROMETA) {
                mn88473_get_status(&g_unified_dev, &locked_now, &str_now, &snr_now);
            }
            LeaveCriticalSection(&g_unified_dev.lock);

            g_is_locked = locked_now;
            g_last_strength = str_now;
            g_last_snr = snr_now;
        }

        sig->strength = (short int)g_last_strength;
        sig->quality = (short int)g_last_snr;
        sig->flags = g_is_locked ? 0x1F : 0x00;
        clear_fpu();
        return AltxDVB_OK;
    }

    case PID_FILTER: {
        return AltxDVB_OK;
    }

    case DEVICE_WHOLE_TRANSP: {
        return AltxDVB_OK;
    }

    case DEVICE_CONFIG: {
        MessageBoxA(NULL,
                    "Unified DVB-C USB Tuner Driver\n"
                    "Supported hardware:\n"
                    " - Microsoft Xbox One Tuner (045E:02D5)\n"
                    " - Astrometa DVB-T2/C Stick (15F4:0131)\n\n"
                    "Status: Production 0-Drop WinUSB Pipeline\n"
                    "Author: zildan & Community",
                    "Unified DVB-C Driver", MB_OK | MB_ICONINFORMATION);
        return AltxDVB_OK;
    }

    default:
        return AltxDVB_OK;
    }
}
