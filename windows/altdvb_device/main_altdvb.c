#include "xbox_device.h"
#include "altdvb_interface.h"
#include <stdio.h>
#include <process.h>

static STR_CB_PROC g_stream_cb = NULL;
static MSG_CB_PROC g_msg_cb = NULL;

static unsigned __stdcall StreamThreadProc(void *arg) {
    xbox_device_t *dev = (xbox_device_t *)arg;
    uint8_t buffer[64 * 188]; /* Multiple of TS packet */
    ULONG read_bytes = 0;

    /* Start Streaming on DiB0700 */
    uint8_t b[4] = { 0x09, 0x00, 0x00, 0x01 }; /* REQUEST_ENABLE_VIDEO_STREAM */
    dib0700_ctrl_wr(dev, b, 4);

    while (dev->is_streaming) {
        if (WaitForSingleObject(dev->hStopEvent, 0) == WAIT_OBJECT_0) {
            break;
        }

        BOOL res = WinUsb_ReadPipe(dev->hWinUsb, dev->ep_in, buffer, sizeof(buffer), &read_bytes, NULL);
        if (res && read_bytes > 0 && g_stream_cb != NULL) {
            /* Pass packets of 188 bytes to AltDVB callback */
            for (ULONG i = 0; i + 188 <= read_bytes; i += 188) {
                if (buffer[i] == 0x47) { /* Valid MPEG-TS Sync Byte */
                    g_stream_cb(&buffer[i]);
                }
            }
        }
    }

    /* Stop Streaming on DiB0700 */
    uint8_t b_stop[4] = { 0x09, 0x00, 0x00, 0x00 };
    dib0700_ctrl_wr(dev, b_stop, 4);

    return 0;
}

int __declspec(dllexport) __stdcall DvbDeviceControl(int msg_id, char *data) {
    switch (msg_id) {
    case DRIVER_DESCRIPTION: {
        struct DRIVER_DATA *drv = (struct DRIVER_DATA *)data;
        strcpy(drv->author, "zildan & Community");
        strcpy(drv->url, "https://github.com/zildan/xbox-one-tuner-qam256-fix");
        strcpy(drv->description, "Microsoft Xbox One Digital TV Tuner (QAM-256 0-Drop Fix)");
        drv->extra = 0;
        return AltxDVB_OK;
    }

    case DEVICE_DESCRIPTION: {
        struct DEVICE_DATA *dev_data = (struct DEVICE_DATA *)data;
        dev_data->dev_cnt = 1;
        dev_data->device[0].dev_type = DVBC | DVBT | AVAILABLE;
        strcpy(dev_data->device[0].dev_name, "Xbox One DVB-C/T/T2 Tuner (WinUSB)");
        strcpy(dev_data->device[0].dev_id, "VID_045E&PID_02D5");
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
        uint32_t freq_hz = tune->frequency * 1000;
        uint32_t sr_ks = tune->symbol_rate;
        if (sr_ks == 0) sr_ks = 6900;

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
        sig->flags = locked ? 0x1F : 0x00; /* 0x1F = LOCKED */
        return AltxDVB_OK;
    }

    case DEVICE_WHOLE_TRANSP: {
        struct WHOLE_TRANSPONDER_DATA *w = (struct WHOLE_TRANSPONDER_DATA *)data;
        w->OpensWholeTransponder = 1; /* Transmit full TS transponder stream */
        return AltxDVB_OK;
    }

    case PID_FILTER: {
        return AltxDVB_OK; /* Hardware delivers whole stream */
    }

    case DISEQC_SUPPORT: {
        struct DISEQC_SUPPORT_DATA *d = (struct DISEQC_SUPPORT_DATA *)data;
        d->DiSEqC_Support = DISEQC_NONE;
        return AltxDVB_OK;
    }

    default:
        return AltxDVB_OK;
    }
}
