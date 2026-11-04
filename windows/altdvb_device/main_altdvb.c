#include "xbox_device.h"
#include "altdvb_interface.h"
#include <stdio.h>
#include <process.h>

static STR_CB_PROC g_stream_cb = NULL;
static MSG_CB_PROC g_msg_cb = NULL;
static struct TUNE_DATA g_last_tune = {0};

/* Adaptive Signal Polling State */
static volatile DWORD g_last_tune_time = 0;
static volatile bool g_is_locked = false;
static int g_last_strength = 0;
static int g_last_snr = 0;
static DWORD g_last_stat_query = 0;
static volatile uint64_t g_ts_packet_count = 0;

/* High-Throughput Asynchronous Ring Buffer Pipeline: 16 x 48128 bytes */
#define NUM_BUFFERS 16
#define TS_BUFFER_SIZE 48128 /* 256 * 188 = 94 * 512 */

typedef struct {
    OVERLAPPED ov;
    uint8_t buffer[TS_BUFFER_SIZE];
    ULONG read_bytes;
} stream_buf_t;

static unsigned __stdcall StreamThreadProc(void *arg) {
    xbox_device_t *dev = (xbox_device_t *)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    stream_buf_t *bufs = (stream_buf_t *)malloc(NUM_BUFFERS * sizeof(stream_buf_t));
    if (!bufs) return 1;

    /* Pre-arm all 16 overlapped transfers into the Windows USB kernel queue */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        memset(&bufs[i].ov, 0, sizeof(OVERLAPPED));
        bufs[i].ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        bufs[i].read_bytes = 0;
        WinUsb_ReadPipe(dev->hWinUsb, dev->ep_in, bufs[i].buffer, TS_BUFFER_SIZE, &bufs[i].read_bytes, &bufs[i].ov);
    }

    uint8_t leftover[188];
    ULONG leftover_len = 0;
    bool synced = false;
    int cur = 0;

    while (dev->is_streaming) {
        HANDLE wait_pair[2] = { dev->hStopEvent, bufs[cur].ov.hEvent };
        DWORD wait_res = WaitForMultipleObjects(2, wait_pair, FALSE, INFINITE);
        if (wait_res == WAIT_OBJECT_0) {
            /* Stop event signaled */
            break;
        }

        if (wait_res == WAIT_OBJECT_0 + 1) {
            ULONG transferred = 0;
            if (WinUsb_GetOverlappedResult(dev->hWinUsb, &bufs[cur].ov, &transferred, FALSE) && transferred > 0) {
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
                        } else {
                            synced = false;
                        }
                        leftover_len = 0;
                    }
                }

                /* If not synced, hunt for 3 consecutive 0x47 sync bytes */
                if (!synced) {
                    leftover_len = 0;
                    for (ULONG k = chunk_pos; k + 376 < transferred; k++) {
                        if (chunk[k] == 0x47 && chunk[k + 188] == 0x47 && chunk[k + 376] == 0x47) {
                            chunk_pos = k;
                            synced = true;
                            break;
                        }
                    }
                }

                /* Stream full 188-byte packets */
                if (synced) {
                    while (chunk_pos + 188 <= transferred) {
                        if (chunk[chunk_pos] == 0x47) {
                            if (g_stream_cb) g_stream_cb(&chunk[chunk_pos]);
                            chunk_pos += 188;
                        } else {
                            synced = false;
                            break;
                        }
                    }

                    /* Save any trailing incomplete packet bytes for the next buffer */
                    if (synced && chunk_pos < transferred) {
                        leftover_len = transferred - chunk_pos;
                        if (leftover_len < 188) {
                            memcpy(leftover, &chunk[chunk_pos], leftover_len);
                        } else {
                            leftover_len = 0;
                        }
                    } else if (!synced) {
                        leftover_len = 0;
                    }
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
    }
    free(bufs);
    return 0;
}

static void StartStreaming(xbox_device_t *dev) {
    if (!dev->is_streaming && dev->is_initialized) {
        dev->is_streaming = true;
        dib0700_set_streaming(dev, 1);
        dev->hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        dev->hStreamThread = (HANDLE)_beginthreadex(NULL, 0, StreamThreadProc, dev, 0, NULL);
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
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
    if (!data && (msg_id != DEVICE_CONFIG)) {
        return AltxDVB_OK;
    }

    switch (msg_id) {
    case DRIVER_DESCRIPTION: {
        struct DRIVER_DATA *drv = (struct DRIVER_DATA *)data;
        memset(drv, 0, sizeof(struct DRIVER_DATA));
        strncpy(drv->author, "zildan & Community", 127);
        strncpy(drv->url, "https://github.com/zildan/xbox-one-tuner-qam256-fix", 127);
        strncpy(drv->description, "Microsoft Xbox One Digital TV Tuner (QAM-256 0-Drop Fix)", 127);
        drv->extra = 0;
        return AltxDVB_OK;
    }

    case DEVICE_DESCRIPTION: {
        struct DEVICE_DATA *dev_data = (struct DEVICE_DATA *)data;
        memset(dev_data, 0, sizeof(struct DEVICE_DATA));
        dev_data->dev_cnt = 1;
        dev_data->device[0].dev_type = DVBC | AVAILABLE;
        strncpy(dev_data->device[0].dev_name, "Xbox One DVB-C Tuner (WinUSB QAM-256 Fix)", 127);
        strncpy(dev_data->device[0].dev_id, "0", 127);
        return AltxDVB_OK;
    }

    case DEVICE_NAME: {
        struct DEVICE_NAME_DATA *d = (struct DEVICE_NAME_DATA *)data;
        memset(d, 0, sizeof(struct DEVICE_NAME_DATA));
        strncpy(d->description, "Xbox One DVB-C Tuner", 127);
        return AltxDVB_OK;
    }

    case OPEN_DEVICE: {
        if (!xbox_usb_open(&g_xbox_dev)) {
            return AltxDVB_ERR;
        }

        if (!dib0700_init(&g_xbox_dev)) {
            xbox_usb_close(&g_xbox_dev);
            return AltxDVB_ERR;
        }

        if (!mn88472_init_device(&g_xbox_dev)) {
            xbox_usb_close(&g_xbox_dev);
            return AltxDVB_ERR;
        }

        g_xbox_dev.is_initialized = true;
        StartStreaming(&g_xbox_dev);
        return AltxDVB_OK;
    }

    case CLOSE_DEVICE: {
        if (g_xbox_dev.is_streaming) {
            g_xbox_dev.is_streaming = false;
            dib0700_set_streaming(&g_xbox_dev, 0);
            SetEvent(g_xbox_dev.hStopEvent);
            WaitForSingleObject(g_xbox_dev.hStreamThread, 1000);
            CloseHandle(g_xbox_dev.hStreamThread);
            CloseHandle(g_xbox_dev.hStopEvent);
            g_xbox_dev.hStreamThread = NULL;
            g_xbox_dev.hStopEvent = NULL;
        }
        xbox_usb_close(&g_xbox_dev);
        return AltxDVB_OK;
    }

    case STREAM_CALLBACK: {
        g_stream_cb = (STR_CB_PROC)data;
        StartStreaming(&g_xbox_dev);
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

        uint32_t sr_ks = tune->symbol_rate;
        if (sr_ks == 0) sr_ks = 6900;

        /* Reset lock status on new tune so AltDVB scanner detects 0 -> 1 lock transition */
        g_is_locked = false;
        g_last_snr = 0;
        g_last_stat_query = 0;
        g_last_tune_time = GetTickCount();

        mn88472_tune_dvbc(&g_xbox_dev, freq_hz, sr_ks, tune->modulation);
        StartStreaming(&g_xbox_dev);
        return AltxDVB_OK;
    }

    case SIGNAL_STATISTICS: {
        struct SIGNAL_STATISTICS_DATA *sig = (struct SIGNAL_STATISTICS_DATA *)data;
        DWORD now = GetTickCount();

        /* Adaptive polling policy:
         * - If unlocked or tuned recently (< 3000ms): poll hardware every 100ms
         *   so that scanning and channel changes detect lock immediately (within 500ms).
         * - Once solidly locked and streaming: completely STOP polling I2C registers!
         *   Every EP0 control transfer stalls the DiB0700 microcontroller's TS FIFO
         *   on high-bitrate QAM-256 streams (~50.87 Mbps), causing intermittent dropouts.
         */
        DWORD poll_interval = g_is_locked ? 400 : 150;
        if (now - g_last_stat_query >= poll_interval || g_last_stat_query == 0) {
            g_last_stat_query = now;
            bool locked_now = false;
            int str_now = 0, snr_now = 0;
            mn88472_read_status(&g_xbox_dev, &str_now, &snr_now, &locked_now);

            g_is_locked = locked_now;
            g_last_strength = str_now;
            g_last_snr = snr_now;
        }

        sig->strength = (short int)g_last_strength;
        sig->quality = (short int)g_last_snr;
        sig->flags = g_is_locked ? 0x1F : 0x00;
        return AltxDVB_OK;
    }

    case DEVICE_WHOLE_TRANSP: {
        struct WHOLE_TRANSPONDER_DATA *w = (struct WHOLE_TRANSPONDER_DATA *)data;
        w->OpensWholeTransponder = 0x1;
        return AltxDVB_OK;
    }

    case PID_FILTER: {
        return AltxDVB_OK;
    }

    case DISEQC_SUPPORT: {
        struct DISEQC_SUPPORT_DATA *d = (struct DISEQC_SUPPORT_DATA *)data;
        d->DiSEqC_Support = DISEQC_NONE;
        return AltxDVB_OK;
    }

    case DEVICE_CONFIG: {
        return AltxDVB_OK;
    }

    default:
        return AltxDVB_OK;
    }
}
