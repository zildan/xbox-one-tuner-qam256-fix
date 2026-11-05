#include "astrometa_device.h"
#include "altdvb_interface.h"
#include <stdio.h>
#include <stdarg.h>
#include <float.h>

void log_dbg(const char *fmt, ...) {
    FILE *f = fopen("D:\\temporare\\astrometa_debug.log", "a");
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
static volatile int g_raw_dump_bytes = 0;

static astrometa_device_t g_dev = {0};
static HINSTANCE g_hModule = NULL;
static char g_module_dir[MAX_PATH] = {0};

/* Ring Buffer Pipeline: 16 x 24064 bytes (128 * 188 = 47 * 512, vendor BULK_STAGESIZE) */
#define NUM_BUFFERS 16
#define TS_BUFFER_SIZE 24064

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
    astrometa_device_t *dev = (astrometa_device_t *)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    log_dbg("StreamThread: ENTERING, ep=0x%02X\n", dev->ep_in);

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
    int pkt_stride = 188;
    int sync_mode = 0; /* 0: standard 0x47, 1: inverted 0xB8 */
    int cur = 0;
    int loop_cnt = 0;
    int pkt_count = 0;

    log_dbg("StreamThread: armed 16 buffers, starting loop\n");

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
                log_dbg("StreamThread: WAIT_TIMEOUT waiting for buf[%d] (no data from EP 0x81)\n", cur);
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

                /* Raw stream dumping disabled for production performance */
                #if 0
                if (g_raw_dump_bytes < 1024 * 1024 && synced) {
                    FILE *rf = fopen("D:\\temporare\\astrometa_raw_stream.bin", g_raw_dump_bytes == 0 ? "wb" : "ab");
                    if (rf) {
                        fwrite(chunk, 1, transferred, rf);
                        fclose(rf);
                        g_raw_dump_bytes += transferred;
                    }
                }
                #endif

                if (!synced && (++loop_cnt % 5 == 1)) {
                    log_dbg("StreamThread: got %u bytes, searching sync... first 16: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                            transferred, chunk[0], chunk[1], chunk[2], chunk[3], chunk[4], chunk[5], chunk[6], chunk[7],
                            chunk[8], chunk[9], chunk[10], chunk[11], chunk[12], chunk[13], chunk[14], chunk[15]);
                }

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
                            pkt_stride = 188;
                            log_dbg("StreamThread: SYNC ACQUIRED at offset %u!\n", k);
                            break;
                        }
                    }
                }

                /* Stream full 188-byte packets with internal resynchronization */
                if (synced) {
                    while (chunk_pos + 188 <= transferred) {
                        if (chunk[chunk_pos] == 0x47) {
                            if (g_stream_cb) g_stream_cb(&chunk[chunk_pos]);
                            pkt_count++;
                            chunk_pos += 188;
                        } else if (chunk_pos + 188 < transferred && chunk[chunk_pos + 188] == 0x47) {
                            /* Isolated corrupt/padding packet; next packet is already aligned */
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

                if (++loop_cnt % 100 == 1 && synced) {
                    log_dbg("StreamThread: alive, mode=%d stride=%d streamed %d packets\n", sync_mode, pkt_stride, pkt_count);
                }
            }

            /* Immediately re-arm bufs[cur] at the tail of the USB pipeline */
            HANDLE hEv = bufs[cur].ov.hEvent;
            ResetEvent(hEv);
            memset(&bufs[cur].ov, 0, sizeof(OVERLAPPED));
            bufs[cur].ov.hEvent = hEv;
            bufs[cur].read_bytes = 0;
            WinUsb_ReadPipe(dev->hWinUsb, dev->ep_in, bufs[cur].buffer, TS_BUFFER_SIZE, &bufs[cur].read_bytes, &bufs[cur].ov);

            cur = (cur + 1) % NUM_BUFFERS;
        }
    }

    /* Cancel any remaining pending I/O and free events */
    CancelIo(dev->hDev);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (bufs[i].ov.hEvent) {
            CloseHandle(bufs[i].ov.hEvent);
        }
        if (bufs[i].buffer) {
            _aligned_free(bufs[i].buffer);
        }
    }
    return 0;
}

static void StartStreaming(astrometa_device_t *dev) {
    if (!dev->is_streaming && dev->is_initialized) {
        dev->is_streaming = true;
        rtl2832_set_streaming(dev, true);
        WinUsb_ResetPipe(dev->hWinUsb, dev->ep_in);
        dev->hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        dev->hStreamThread = (HANDLE)_beginthreadex(NULL, 0, StreamThreadProc, dev, 0, NULL);
    }
}

static void StopStreaming(astrometa_device_t *dev) {
    if (dev->is_streaming) {
        dev->is_streaming = false;
        rtl2832_set_streaming(dev, false);
        SetEvent(dev->hStopEvent);
        WaitForSingleObject(dev->hStreamThread, 1000);
        CloseHandle(dev->hStreamThread);
        CloseHandle(dev->hStopEvent);
        dev->hStreamThread = NULL;
        dev->hStopEvent = NULL;
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        _control87(0x133F, 0xFFFF); /* Mask all FPU exceptions to prevent Delphi crashes */
        g_hModule = hinstDLL;
        char path[MAX_PATH] = {0};
        if (GetModuleFileNameA(hinstDLL, path, sizeof(path))) {
            char *last_slash = strrchr(path, '\\');
            if (last_slash) {
                *(last_slash + 1) = '\0';
                strncpy(g_module_dir, path, sizeof(g_module_dir) - 1);
            }
        }
    }
    return TRUE;
}

int __declspec(dllexport) __stdcall DvbDeviceControl(int msg_id, char *data) {
    clear_fpu();
    log_dbg("DvbDeviceControl msg_id=%d\n", msg_id);
    if (!data && (msg_id != DEVICE_CONFIG)) {
        clear_fpu();
        return AltxDVB_OK;
    }

    switch (msg_id) {
    case DRIVER_DESCRIPTION: {
        log_dbg("DRIVER_DESCRIPTION\n");
        struct DRIVER_DATA *drv = (struct DRIVER_DATA *)data;
        memset(drv, 0, sizeof(struct DRIVER_DATA));
        strncpy(drv->author, "Community & zildan", 127);
        strncpy(drv->url, "https://github.com", 127);
        strncpy(drv->description, "Astrometa DVB-T2/C USB Stick (WinUSB Native)", 127);
        drv->extra = 0;
        clear_fpu();
        return AltxDVB_OK;
    }

    case DEVICE_DESCRIPTION: {
        log_dbg("DEVICE_DESCRIPTION\n");
        struct DEVICE_DATA *dev_data = (struct DEVICE_DATA *)data;
        memset(dev_data, 0, sizeof(struct DEVICE_DATA));
        dev_data->dev_cnt = 1;
        dev_data->device[0].dev_type = DVBC | AVAILABLE;
        strncpy(dev_data->device[0].dev_name, "Astrometa DVB-C Tuner (RTL2832+MN88473+R828D)", 127);
        strncpy(dev_data->device[0].dev_id, "0", 127);
        clear_fpu();
        return AltxDVB_OK;
    }

    case DEVICE_NAME: {
        log_dbg("DEVICE_NAME\n");
        struct DEVICE_NAME_DATA *d = (struct DEVICE_NAME_DATA *)data;
        memset(d, 0, sizeof(struct DEVICE_NAME_DATA));
        strncpy(d->description, "Astrometa DVB-C Tuner", 127);
        clear_fpu();
        return AltxDVB_OK;
    }

    case OPEN_DEVICE: {
        log_dbg("OPEN_DEVICE begin\n");
        if (!astrometa_usb_open(&g_dev)) {
            log_dbg("OPEN_DEVICE: astrometa_usb_open failed!\n");
            clear_fpu();
            return AltxDVB_ERR;
        }
        log_dbg("OPEN_DEVICE: usb_open OK\n");

        if (!rtl2832_init(&g_dev)) {
            log_dbg("OPEN_DEVICE: rtl2832_init failed!\n");
            astrometa_usb_close(&g_dev);
            clear_fpu();
            return AltxDVB_ERR;
        }
        log_dbg("OPEN_DEVICE: rtl2832_init OK\n");

        if (!mn88473_init(&g_dev)) {
            log_dbg("OPEN_DEVICE: mn88473_init failed!\n");
            astrometa_usb_close(&g_dev);
            clear_fpu();
            return AltxDVB_ERR;
        }
        log_dbg("OPEN_DEVICE: mn88473_init OK\n");

        if (!r820t_device_init(&g_dev)) {
            log_dbg("OPEN_DEVICE: r820t_device_init failed!\n");
            astrometa_usb_close(&g_dev);
            clear_fpu();
            return AltxDVB_ERR;
        }
        log_dbg("OPEN_DEVICE: r820t_device_init OK\n");

        g_dev.is_initialized = true;
        log_dbg("OPEN_DEVICE OK\n");
        clear_fpu();
        return AltxDVB_OK;
    }

    case CLOSE_DEVICE: {
        StopStreaming(&g_dev);
        astrometa_usb_close(&g_dev);
        g_dev.is_initialized = false;
        clear_fpu();
        return AltxDVB_OK;
    }

    case STREAM_CALLBACK: {
        g_stream_cb = (STR_CB_PROC)data;
        clear_fpu();
        return AltxDVB_OK;
    }

    case MESSAGE_CALLBACK: {
        g_msg_cb = (MSG_CB_PROC)data;
        clear_fpu();
        return AltxDVB_OK;
    }

    case TUNE: {
        clear_fpu();
        struct TUNE_DATA *tune = (struct TUNE_DATA *)data;
        if (!tune) { clear_fpu(); return AltxDVB_OK; }
        log_dbg("TUNE: freq=%d sr=%d mod=%d bw=%d fec=%d\n",
                tune->frequency, tune->symbol_rate, tune->modulation, tune->bandwidth, tune->fec);
        memcpy(&g_last_tune, tune, sizeof(struct TUNE_DATA));

        if (tune->frequency <= 0) {
            log_dbg("TUNE: frequency <= 0, skipping tune\n");
            clear_fpu();
            return AltxDVB_OK;
        }

        /* Cleanly stop any existing stream thread before reprogramming hardware */
        StopStreaming(&g_dev);

        uint32_t freq_hz;
        if (tune->frequency < 10000) {
            freq_hz = tune->frequency * 1000000;
        } else if (tune->frequency < 10000000) {
            freq_hz = tune->frequency * 1000;
        } else {
            freq_hz = tune->frequency;
        }

        uint32_t sr_ks = tune->symbol_rate;
        if (sr_ks == 0) sr_ks = 6900;

        g_is_locked = false;
        g_raw_dump_bytes = 0;
        g_last_snr = 0;
        g_last_strength = 0;
        g_last_stat_query = 0;
        g_last_tune_time = GetTickCount();

        /* 1. Tune Rafael Micro R828D Tuner */
        log_dbg("TUNE: r820t_device_set_freq %u Hz\n", freq_hz);
        r820t_device_set_freq(&g_dev, freq_hz);

        /* 2. Configure Panasonic MN88473 Demodulator */
        log_dbg("TUNE: mn88473_tune_dvbc\n");
        mn88473_tune_dvbc(&g_dev, freq_hz, sr_ks, tune->modulation);

        /* 3. Enable Slave TS routing in RTL2832 */
        log_dbg("TUNE: rtl2832_set_streaming\n");
        rtl2832_set_streaming(&g_dev, true);

        if (g_stream_cb) {
            log_dbg("TUNE: StartStreaming\n");
            StartStreaming(&g_dev);
        }
        log_dbg("TUNE OK\n");
        clear_fpu();
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
            mn88473_get_status(&g_dev, &locked_now, &str_now, &snr_now);

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

    case DEVICE_WHOLE_TRANSP: {
        struct WHOLE_TRANSPONDER_DATA *w = (struct WHOLE_TRANSPONDER_DATA *)data;
        w->OpensWholeTransponder = 0x1;
        clear_fpu();
        return AltxDVB_OK;
    }

    case PID_FILTER: {
        clear_fpu();
        return AltxDVB_OK;
    }

    case DISEQC_SUPPORT: {
        struct DISEQC_SUPPORT_DATA *d = (struct DISEQC_SUPPORT_DATA *)data;
        d->DiSEqC_Support = DISEQC_NONE;
        clear_fpu();
        return AltxDVB_OK;
    }

    case DEVICE_CONFIG: {
        clear_fpu();
        return AltxDVB_OK;
    }

    default:
        clear_fpu();
        return AltxDVB_OK;
    }
}
