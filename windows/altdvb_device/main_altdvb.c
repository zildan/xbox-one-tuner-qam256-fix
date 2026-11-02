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

    FILE *finit = fopen("D:\\temporare\\altdvb_stream.log", "a");
    if (finit) {
        fprintf(finit, "StreamThreadProc: ENTERED! ep_in=0x%02X, cb=%p\n", dev->ep_in, g_stream_cb);
        fclose(finit);
    }

    stream_buf_t *bufs = (stream_buf_t *)malloc(NUM_BUFFERS * sizeof(stream_buf_t));
    if (!bufs) return 1;

    HANDLE wait_handles[NUM_BUFFERS + 1];
    wait_handles[0] = dev->hStopEvent;

    /* Pre-arm all 16 overlapped transfers into the Windows USB kernel queue */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        memset(&bufs[i].ov, 0, sizeof(OVERLAPPED));
        bufs[i].ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        wait_handles[i + 1] = bufs[i].ov.hEvent;
        bufs[i].read_bytes = 0;

        BOOL res = WinUsb_ReadPipe(dev->hWinUsb, dev->ep_in, bufs[i].buffer, TS_BUFFER_SIZE, &bufs[i].read_bytes, &bufs[i].ov);
        if (!res && GetLastError() != ERROR_IO_PENDING) {
            FILE *ferr = fopen("D:\\temporare\\altdvb_stream.log", "a");
            if (ferr) {
                fprintf(ferr, "StreamThreadProc: initial WinUsb_ReadPipe [%d] failed, err=%lu\n", i, GetLastError());
                fclose(ferr);
            }
        }
    }

    static bool first_buf_logged = false;

    while (dev->is_streaming) {
        DWORD wait_res = WaitForMultipleObjects(NUM_BUFFERS + 1, wait_handles, FALSE, INFINITE);
        if (wait_res == WAIT_OBJECT_0) {
            /* Stop event signaled */
            break;
        }

        if (wait_res >= (WAIT_OBJECT_0 + 1) && wait_res < (WAIT_OBJECT_0 + 1 + NUM_BUFFERS)) {
            int idx = (int)(wait_res - (WAIT_OBJECT_0 + 1));
            ULONG transferred = 0;

            if (WinUsb_GetOverlappedResult(dev->hWinUsb, &bufs[idx].ov, &transferred, FALSE)) {
                if (transferred > 0) {
                    if (!first_buf_logged) {
                        first_buf_logged = true;
                        FILE *fb = fopen("D:\\temporare\\altdvb_stream.log", "a");
                        if (fb) {
                            fprintf(fb, "StreamThreadProc: First TS buffer! transferred=%lu bytes, byte0=0x%02X, cb=%p\n",
                                    transferred, bufs[idx].buffer[0], g_stream_cb);
                            fclose(fb);
                        }
                    }

                    uint8_t *p = bufs[idx].buffer;
                    ULONG offset = 0;
                    if (p[0] != 0x47) {
                        /* Validate packet boundary using 2-packet TS sync validation */
                        for (ULONG k = 0; k < 188 && (k + 188) < transferred; k++) {
                            if (p[k] == 0x47 && p[k + 188] == 0x47) {
                                offset = k;
                                break;
                            }
                        }
                    }

                    if (g_stream_cb != NULL) {
                        for (ULONG i = offset; i + 188 <= transferred; i += 188) {
                            if (p[i] == 0x47) {
                                g_stream_cb(&p[i]);
                            }
                        }
                    }
                }
            }

            /* Immediately re-arm the completed buffer to keep the USB hardware pipeline full */
            HANDLE hEv = bufs[idx].ov.hEvent;
            ResetEvent(hEv);
            memset(&bufs[idx].ov, 0, sizeof(OVERLAPPED));
            bufs[idx].ov.hEvent = hEv;
            bufs[idx].read_bytes = 0;
            WinUsb_ReadPipe(dev->hWinUsb, dev->ep_in, bufs[idx].buffer, TS_BUFFER_SIZE, &bufs[idx].read_bytes, &bufs[idx].ov);
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
        FILE *fs = fopen("D:\\temporare\\altdvb_stream.log", "a");
        if (fs) {
            fprintf(fs, "StartStreaming: dib0700_set_streaming(1) OK! thread launched! cb=%p\n", g_stream_cb);
            fclose(fs);
        }
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

static const char* msg_name(int id) {
    switch (id) {
    case 0x10000001: return "DRIVER_DESCRIPTION";
    case 0x10000002: return "DEVICE_DESCRIPTION";
    case 0x10000003: return "MESSAGE_CALLBACK";
    case 0x10000004: return "OPEN_DEVICE";
    case 0x10000005: return "CLOSE_DEVICE";
    case 0x10000006: return "DEVICE_NAME";
    case 0x10000007: return "PID_FILTER";
    case 0x10000008: return "STREAM_CALLBACK";
    case 0x10000009: return "TUNE";
    case 0x1000000A: return "SIGNAL_STATISTICS";
    case 0x1000000B: return "DEVICE_CONFIG";
    case 0x1000000C: return "DISEQC_COMMAND";
    case 0x1000000D: return "DISEQC_SUPPORT";
    case 0x1000000F: return "DEVICE_WHOLE_TRANSP";
    default: return "OTHER";
    }
}

int __declspec(dllexport) __stdcall DvbDeviceControl(int msg_id, char *data) {
    if (!data && (msg_id != DEVICE_CONFIG)) {
        return AltxDVB_OK;
    }

    if (msg_id != SIGNAL_STATISTICS) {
        FILE *fmsg = fopen("D:\\temporare\\altdvb_msgs.log", "a");
        if (fmsg) {
            fprintf(fmsg, "MSG: %s (0x%08X), data=%p\n", msg_name(msg_id), msg_id, data);
            fclose(fmsg);
        }
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
        FILE *f = fopen("D:\\temporare\\altdvb_init.log", "w");
        if (f) {
            fprintf(f, "OPEN_DEVICE started. Module dir: '%s'\n", g_module_dir);
            fclose(f);
        }

        if (!xbox_usb_open(&g_xbox_dev)) {
            f = fopen("D:\\temporare\\altdvb_init.log", "a");
            if (f) { fprintf(f, "xbox_usb_open FAILED\n"); fclose(f); }
            return AltxDVB_ERR;
        }

        f = fopen("D:\\temporare\\altdvb_init.log", "a");
        if (f) { fprintf(f, "xbox_usb_open OK. Initializing DiB0700 bridge...\n"); fclose(f); }

        if (!dib0700_init(&g_xbox_dev)) {
            f = fopen("D:\\temporare\\altdvb_init.log", "a");
            if (f) { fprintf(f, "dib0700_init FAILED\n"); fclose(f); }
            xbox_usb_close(&g_xbox_dev);
            return AltxDVB_ERR;
        }

        f = fopen("D:\\temporare\\altdvb_init.log", "a");
        if (f) { fprintf(f, "dib0700_init OK. Initializing MN88472...\n"); fclose(f); }

        if (!mn88472_init_device(&g_xbox_dev)) {
            f = fopen("D:\\temporare\\altdvb_init.log", "a");
            if (f) { fprintf(f, "mn88472_init_device FAILED\n"); fclose(f); }
            xbox_usb_close(&g_xbox_dev);
            return AltxDVB_ERR;
        }

        f = fopen("D:\\temporare\\altdvb_init.log", "a");
        if (f) { fprintf(f, "mn88472_init_device OK!\n"); fclose(f); }

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

        FILE *f = fopen("D:\\temporare\\altdvb_tune.log", "a");
        if (f) {
            fprintf(f, "TUNE called: raw_freq=%d -> freq_hz=%u, SR=%u, mod=%d (lock reset)\n",
                    tune->frequency, freq_hz, sr_ks, tune->modulation);
            fclose(f);
        }

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
         * - Once solidly locked and streaming: throttle to 1000ms to eliminate
         *   EP0 I2C traffic that stalls the DiB0700 TS FIFO during 50 Mbps playback.
         */
        DWORD poll_interval = (g_is_locked && (now - g_last_tune_time > 3000)) ? 1000 : 100;

        if (now - g_last_stat_query >= poll_interval || g_last_stat_query == 0) {
            g_last_stat_query = now;
            bool locked_now = false;
            int str_now = 0, snr_now = 0;
            mn88472_read_status(&g_xbox_dev, &str_now, &snr_now, &locked_now);

            g_is_locked = locked_now;
            g_last_strength = str_now;
            g_last_snr = snr_now;

            if (now - g_last_tune_time < 3000) {
                FILE *f = fopen("D:\\temporare\\altdvb_tune.log", "a");
                if (f) {
                    fprintf(f, "STAT: str=%d%% snr=%d%% locked=%d flags=0x%02X\n",
                            g_last_strength, g_last_snr, g_is_locked, g_is_locked ? 0x1F : 0x00);
                    fclose(f);
                }
            }
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
        struct PID_FILTER_DATA *pf = (struct PID_FILTER_DATA *)data;
        FILE *fmsg = fopen("D:\\temporare\\altdvb_msgs.log", "a");
        if (fmsg && pf) {
            fprintf(fmsg, "PID_FILTER: op=%d, pid=0x%04X (%d)\n", pf->operation, pf->pid, pf->pid);
            fclose(fmsg);
        }
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
