#include "astrometa_device.h"
#include <setupapi.h>
#include <stdio.h>
#include <ctype.h>

void log_dbg(const char *fmt, ...);

static const GUID GUID_DEVINTERFACE_ZADIG = 
{ 0xdee824ef, 0x729b, 0x4a0e, { 0x9c, 0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17 } };

static const GUID GUID_DEVINTERFACE_USBDevice = 
{ 0x88bae032, 0x5a81, 0x49f0, { 0xbc, 0x3d, 0xa4, 0xff, 0x13, 0x82, 0x16, 0xd6 } };

static const GUID GUID_DEVINTERFACE_USB_DEVICE = 
{ 0xA5DCBF10, 0x6530, 0x11D2, { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED } };

static bool stristr_match(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && (tolower((unsigned char)*h) == tolower((unsigned char)*n))) {
            h++;
            n++;
        }
        if (!*n) return true;
    }
    return false;
}

bool astrometa_usb_open(astrometa_device_t *dev) {
    char path[1024] = {0};
    bool found = false;

    /* Enumerate using Zadig WinUSB GUID first, then standard classes */
    const GUID *guids[] = { &GUID_DEVINTERFACE_ZADIG, &GUID_DEVINTERFACE_USBDevice, &GUID_DEVINTERFACE_USB_DEVICE };
    for (int g = 0; g < 3 && !found; g++) {
        HDEVINFO hInfo = SetupDiGetClassDevsA(guids[g], NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (hInfo == INVALID_HANDLE_VALUE) continue;

        SP_DEVICE_INTERFACE_DATA ifData;
        ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

        for (int i = 0; SetupDiEnumDeviceInterfaces(hInfo, NULL, guids[g], i, &ifData); i++) {
            ULONG reqLen = 0;
            SetupDiGetDeviceInterfaceDetailA(hInfo, &ifData, NULL, 0, &reqLen, NULL);
            if (reqLen == 0) continue;

            PSP_DEVICE_INTERFACE_DETAIL_DATA_A det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(reqLen);
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
            if (SetupDiGetDeviceInterfaceDetailA(hInfo, &ifData, det, reqLen, NULL, NULL)) {
                log_dbg("Enum candidate: %s\n", det->DevicePath);
                if (stristr_match(det->DevicePath, "vid_15f4") && stristr_match(det->DevicePath, "pid_0131") &&
                    (stristr_match(det->DevicePath, "mi_00") || g == 0)) {
                    strncpy(path, det->DevicePath, sizeof(path) - 1);
                    found = true;
                    free(det);
                    break;
                }
            }
            free(det);
        }
        SetupDiDestroyDeviceInfoList(hInfo);
    }

    if (!found) {
        /* Fallback directly to the known instance path */
        strncpy(path, "\\\\?\\USB#VID_15F4&PID_0131&MI_00#6&348360e0&0&0000#{dee824ef-729b-4a0e-9c14-b7117d33a817}", sizeof(path) - 1);
        log_dbg("Using direct fallback path: %s\n", path);
    } else {
        log_dbg("Using enumerated path: %s\n", path);
    }

    dev->hDev = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (dev->hDev == INVALID_HANDLE_VALUE) {
        log_dbg("CreateFileA failed, err=%u\n", (unsigned)GetLastError());
        return false;
    }

    if (!WinUsb_Initialize(dev->hDev, &dev->hWinUsb)) {
        log_dbg("WinUsb_Initialize failed, err=%u\n", (unsigned)GetLastError());
        CloseHandle(dev->hDev);
        dev->hDev = INVALID_HANDLE_VALUE;
        return false;
    }

    /* Configure WinUSB endpoint settings */
    USB_INTERFACE_DESCRIPTOR ifDesc;
    dev->ep_in = 0x81; /* RTL2832 MPEG-TS bulk endpoint */
    dev->max_packet_size = 512;

    if (WinUsb_QueryInterfaceSettings(dev->hWinUsb, 0, &ifDesc)) {
        for (int i = 0; i < ifDesc.bNumEndpoints; i++) {
            WINUSB_PIPE_INFORMATION pipeInfo;
            if (WinUsb_QueryPipe(dev->hWinUsb, 0, (UCHAR)i, &pipeInfo)) {
                if (pipeInfo.PipeId == 0x81) {
                    dev->ep_in = pipeInfo.PipeId;
                    dev->max_packet_size = pipeInfo.MaximumPacketSize;
                }
            }
        }
    }

    /* Configure pipe policies */
    UCHAR raw_io = TRUE;
    WinUsb_SetPipePolicy(dev->hWinUsb, dev->ep_in, RAW_IO, sizeof(raw_io), &raw_io);

    UCHAR auto_clear = TRUE;
    WinUsb_SetPipePolicy(dev->hWinUsb, dev->ep_in, AUTO_CLEAR_STALL, sizeof(auto_clear), &auto_clear);

    ULONG timeout = 3000;
    WinUsb_SetPipePolicy(dev->hWinUsb, dev->ep_in, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);

    return true;
}

void astrometa_usb_close(astrometa_device_t *dev) {
    if (dev->hWinUsb) {
        WinUsb_Free(dev->hWinUsb);
        dev->hWinUsb = NULL;
    }
    if (dev->hDev != INVALID_HANDLE_VALUE && dev->hDev != NULL) {
        CloseHandle(dev->hDev);
        dev->hDev = INVALID_HANDLE_VALUE;
    }
}
