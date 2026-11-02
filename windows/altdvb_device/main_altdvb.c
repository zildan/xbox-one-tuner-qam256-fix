#include "xbox_device.h"
#include "altdvb_interface.h"
#include <stdio.h>
#include <process.h>

static STR_CB_PROC g_stream_cb = NULL;
static MSG_CB_PROC g_msg_cb = NULL;
static struct TUNE_DATA g_last_tune = {0};

/* High-Throughput Asynchronous Ring Buffer Pipeline: 8 x 48128 bytes */
#define NUM_BUFFERS 8
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

    HANDLE wait_handles[NUM_BUFFERS + 1];
    wait_handles[0] = dev->hStopEvent;

    /* Pre-arm all 8 overlapped transfers into the Windows USB kernel queue */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        memset(&bufs[i].ov, 0, sizeof(OVERLAPPED));
        bufs[i].ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        wait_handles[i + 1] = bufs[i].ov.hEvent;
        bufs[i].read_bytes = 0;

        BOOL res = WinUsb_ReadPipe(dev->hWinUsb, dev->ep_in, bufs[i].buffer, TS_BUFFER_SIZE, &bufs[i].read_bytes, &bufs[i].ov);
        if (!res && GetLastError() != ERROR_IO_PENDING) {
            /* Error queuing */
        }
    }

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
                if (transferred > 0 && g_stream_cb != NULL) {
                    uint8_t *p = bufs[idx].buffer;
                    for (ULONG i = 0; i + 188 <= transferred; i += 188) {
                        if (p[i] == 0x47) {
                            g_stream_cb(&p[i]);
                        }
                    }
                }
            }

            /* Immediately re-arm the completed buffer to keep the USB hardware pipeline full */
            ResetEvent(bufs[idx].ov.hEvent);
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
        return AltxDVB_OK;
    }

    case CLOSE_DEVICE: {
        if (g_xbox_dev.is_streaming) {
            g_xbox_dev.is_streaming = false;
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
        if (g_stream_cb && !g_xbox_dev.is_streaming && g_xbox_dev.is_initialized) {
            g_xbox_dev.is_streaming = true;
            g_xbox_dev.hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            g_xbox_dev.hStreamThread = (HANDLE)_beginthreadex(NULL, 0, StreamThreadProc, &g_xbox_dev, 0, NULL);
        }
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

        FILE *f = fopen("D:\\temporare\\altdvb_tune.log", "a");
        if (f) {
            fprintf(f, "TUNE called: raw_freq=%d -> freq_hz=%u, SR=%u, mod=%d\n",
                    tune->frequency, freq_hz, sr_ks, tune->modulation);
            fclose(f);
        }

        mn88472_tune_dvbc(&g_xbox_dev, freq_hz, sr_ks, tune->modulation);
        return AltxDVB_OK;
    }

    case SIGNAL_STATISTICS: {
        struct SIGNAL_STATISTICS_DATA *sig = (struct SIGNAL_STATISTICS_DATA *)data;
        int strength = 0, snr = 0;
        bool locked = false;

        mn88472_read_status(&g_xbox_dev, &strength, &snr, &locked);
        sig->strength = (short int)strength;
        sig->quality = (short int)snr;
        sig->flags = locked ? 0x1F : 0x00;

        static int stat_log_cnt = 0;
        if ((stat_log_cnt++ % 5) == 0) {
            FILE *f = fopen("D:\\temporare\\altdvb_tune.log", "a");
            if (f) {
                fprintf(f, "STAT: strength=%d%%, snr=%d%%, locked=%d, flags=0x%02x\n",
                        strength, snr, locked, sig->flags);
                fclose(f);
            }
        }
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
