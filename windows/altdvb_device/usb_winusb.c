#include "xbox_device.h"
#include <stdio.h>
#include <initguid.h>

/* Standard USB Device GUID or WinUSB Generic GUID */
DEFINE_GUID(GUID_DEVINTERFACE_USB_DEVICE, 0xA5DCBF10L, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);
/* Custom WinUSB Device GUID often used with Zadig */
DEFINE_GUID(GUID_DEVINTERFACE_WINUSB,     0xdee824ef, 0x729b, 0x4a0e, 0x9c, 0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17);

xbox_device_t g_xbox_dev = {0};
char g_module_dir[MAX_PATH] = {0};
HINSTANCE g_hModule = NULL;

bool xbox_usb_open(xbox_device_t *dev) {
    if (dev->hWinUsb != NULL) return true;
    
    HDEVINFO hDevInfo;
    SP_DEVICE_INTERFACE_DATA devIntData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA detailData = NULL;
    DWORD reqSize = 0;
    bool found = false;
    char path[MAX_PATH] = {0};

    const GUID *guids[] = { &GUID_DEVINTERFACE_WINUSB, &GUID_DEVINTERFACE_USB_DEVICE };

    for (int g = 0; g < 2 && !found; g++) {
        hDevInfo = SetupDiGetClassDevs(guids[g], NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (hDevInfo == INVALID_HANDLE_VALUE) continue;

        devIntData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, guids[g], i, &devIntData); i++) {
            SetupDiGetDeviceInterfaceDetail(hDevInfo, &devIntData, NULL, 0, &reqSize, NULL);
            detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(reqSize);
            if (!detailData) continue;
            detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

            if (SetupDiGetDeviceInterfaceDetail(hDevInfo, &devIntData, detailData, reqSize, NULL, NULL)) {
                /* Check for VID_045E & PID_02D5 */
                char *str = detailData->DevicePath;
                if ((strstr(str, "vid_045e") || strstr(str, "VID_045E")) &&
                    (strstr(str, "pid_02d5") || strstr(str, "PID_02D5"))) {
                    strncpy(path, detailData->DevicePath, sizeof(path) - 1);
                    found = true;
                    free(detailData);
                    break;
                }
            }
            free(detailData);
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }

    if (!found) {
        return false;
    }

    dev->hDev = CreateFile(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (dev->hDev == INVALID_HANDLE_VALUE) {
        return false;
    }

    if (!WinUsb_Initialize(dev->hDev, &dev->hWinUsb)) {
        CloseHandle(dev->hDev);
        dev->hDev = INVALID_HANDLE_VALUE;
        return false;
    }

    /* Configure WinUSB endpoint settings */
    USB_INTERFACE_DESCRIPTOR ifDesc;
    dev->ep_in = 0x82; /* DiB0700 MPEG-TS bulk endpoint */
    dev->max_packet_size = 512;
    if (WinUsb_QueryInterfaceSettings(dev->hWinUsb, 0, &ifDesc)) {
        for (int i = 0; i < ifDesc.bNumEndpoints; i++) {
            WINUSB_PIPE_INFORMATION pipeInfo;
            if (WinUsb_QueryPipe(dev->hWinUsb, 0, (UCHAR)i, &pipeInfo)) {
                if (pipeInfo.PipeId == 0x82) {
                    dev->ep_in = pipeInfo.PipeId;
                    dev->max_packet_size = pipeInfo.MaximumPacketSize;
                }
            }
        }
    }

    /* Set pipe policy for EP 0x01 (Firmware download) */
    ULONG out_timeout = 2000;
    WinUsb_SetPipePolicy(dev->hWinUsb, 0x01, PIPE_TRANSFER_TIMEOUT, sizeof(out_timeout), &out_timeout);

    /* Disable RAW_IO to enable WinUSB multi-packet buffering */
    UCHAR rawIO = FALSE;
    WinUsb_SetPipePolicy(dev->hWinUsb, dev->ep_in, RAW_IO, sizeof(rawIO), &rawIO);

    UCHAR auto_clear = TRUE;
    WinUsb_SetPipePolicy(dev->hWinUsb, dev->ep_in, AUTO_CLEAR_STALL, sizeof(auto_clear), &auto_clear);

    ULONG timeout = 3000;
    WinUsb_SetPipePolicy(dev->hWinUsb, dev->ep_in, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);

    return true;
}

void xbox_usb_close(xbox_device_t *dev) {
    if (dev->hWinUsb) {
        WinUsb_Free(dev->hWinUsb);
        dev->hWinUsb = NULL;
    }
    if (dev->hDev != INVALID_HANDLE_VALUE && dev->hDev != NULL) {
        CloseHandle(dev->hDev);
        dev->hDev = INVALID_HANDLE_VALUE;
    }
    dev->is_initialized = false;
    dev->is_streaming = false;
}

int xbox_usb_control_msg(xbox_device_t *dev, uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index, void *data, uint16_t size, int timeout) {
    if (!dev->hWinUsb) return -1;

    WINUSB_SETUP_PACKET setup;
    setup.RequestType = request_type;
    setup.Request = request;
    setup.Value = value;
    setup.Index = index;
    setup.Length = size;

    ULONG transferred = 0;
    BOOL res;

    if (request_type & 0x80) {
        /* Device to Host (Read) */
        res = WinUsb_ControlTransfer(dev->hWinUsb, setup, (PUCHAR)data, size, &transferred, NULL);
    } else {
        /* Host to Device (Write) */
        res = WinUsb_ControlTransfer(dev->hWinUsb, setup, (PUCHAR)data, size, &transferred, NULL);
    }

    if (!res) {
        return -1;
    }
    return (int)transferred;
}
