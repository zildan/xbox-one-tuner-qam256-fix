#include "unified_device.h"
#include <setupapi.h>
#include <stdio.h>
#include <ctype.h>

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

static bool find_device_path(device_model_t model, char *out_path, size_t out_len) {
    const GUID *guids[] = { &GUID_DEVINTERFACE_ZADIG, &GUID_DEVINTERFACE_USBDevice, &GUID_DEVINTERFACE_USB_DEVICE };

    for (int g = 0; g < 3; g++) {
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
                bool is_xbox = stristr_match(det->DevicePath, "vid_045e") && stristr_match(det->DevicePath, "pid_02d5");
                bool is_astro = stristr_match(det->DevicePath, "vid_15f4") && stristr_match(det->DevicePath, "pid_0131") &&
                                (stristr_match(det->DevicePath, "mi_00") || g == 0);

                if ((model == DEVICE_MODEL_XBOX_ONE || model == DEVICE_MODEL_AUTO) && is_xbox) {
                    strncpy(out_path, det->DevicePath, out_len - 1);
                    free(det);
                    SetupDiDestroyDeviceInfoList(hInfo);
                    return true;
                }
                if ((model == DEVICE_MODEL_ASTROMETA || model == DEVICE_MODEL_AUTO) && is_astro) {
                    strncpy(out_path, det->DevicePath, out_len - 1);
                    free(det);
                    SetupDiDestroyDeviceInfoList(hInfo);
                    return true;
                }
            }
            free(det);
        }
        SetupDiDestroyDeviceInfoList(hInfo);
    }
    return false;
}

bool unified_usb_open(unified_device_t *dev, device_model_t target_model) {
    char path[1024] = {0};
    if (!find_device_path(target_model, path, sizeof(path))) {
        log_dbg("unified_usb_open: No supported device found (target=%d)\n", (int)target_model);
        return false;
    }

    if (stristr_match(path, "vid_045e")) {
        dev->model = DEVICE_MODEL_XBOX_ONE;
    } else if (stristr_match(path, "vid_15f4")) {
        dev->model = DEVICE_MODEL_ASTROMETA;
    } else {
        dev->model = target_model;
    }

    log_dbg("unified_usb_open: Opening %s (Model: %s)\n", path,
            dev->model == DEVICE_MODEL_XBOX_ONE ? "Xbox One" : "Astrometa");

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

    /* Configure default EP mapping */
    if (dev->model == DEVICE_MODEL_XBOX_ONE) {
        dev->ep_in = 0x82; /* DiB0700 TS bulk in endpoint */
    } else {
        dev->ep_in = 0x81; /* RTL2832 TS bulk in endpoint */
    }
    dev->max_packet_size = 512;

    /* Enumerate endpoints from USB descriptor */
    USB_INTERFACE_DESCRIPTOR ifDesc;
    if (WinUsb_QueryInterfaceSettings(dev->hWinUsb, 0, &ifDesc)) {
        for (int i = 0; i < ifDesc.bNumEndpoints; i++) {
            WINUSB_PIPE_INFORMATION pipeInfo;
            if (WinUsb_QueryPipe(dev->hWinUsb, 0, (UCHAR)i, &pipeInfo)) {
                if (pipeInfo.PipeType == UsbdPipeTypeBulk && (pipeInfo.PipeId & 0x80)) {
                    dev->ep_in = pipeInfo.PipeId;
                    dev->max_packet_size = pipeInfo.MaximumPacketSize;
                    log_dbg("Discovered Bulk IN EP: 0x%02X (max packet: %u)\n", dev->ep_in, (unsigned)dev->max_packet_size);
                    break;
                }
            }
        }
    }

    /* Configure WinUSB pipe policies for high-speed streaming */
    UCHAR raw_io = TRUE;
    WinUsb_SetPipePolicy(dev->hWinUsb, dev->ep_in, RAW_IO, sizeof(raw_io), &raw_io);
    ULONG timeout = 1000;
    WinUsb_SetPipePolicy(dev->hWinUsb, dev->ep_in, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);

    return true;
}

void unified_usb_close(unified_device_t *dev) {
    if (dev->hWinUsb) {
        WinUsb_Free(dev->hWinUsb);
        dev->hWinUsb = NULL;
    }
    if (dev->hDev && dev->hDev != INVALID_HANDLE_VALUE) {
        CloseHandle(dev->hDev);
        dev->hDev = INVALID_HANDLE_VALUE;
    }
    dev->is_initialized = false;
    dev->model = DEVICE_MODEL_NONE;
}

int xbox_usb_control_msg(unified_device_t *dev, uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index, void *data, uint16_t size, int timeout) {
    if (!dev || !dev->hWinUsb) return -1;
    WINUSB_SETUP_PACKET pkt = {0};
    pkt.RequestType = request_type;
    pkt.Request = request;
    pkt.Value = value;
    pkt.Index = index;
    pkt.Length = size;
    ULONG transferred = 0;
    BOOL ok = WinUsb_ControlTransfer(dev->hWinUsb, pkt, (PUCHAR)data, size, &transferred, NULL);
    return ok ? (int)transferred : -1;
}


