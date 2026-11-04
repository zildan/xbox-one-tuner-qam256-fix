#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define CMD_DEMOD_RD     0x0000
#define CMD_DEMOD_WR     0x0010
#define CMD_USB_RD       0x0100
#define CMD_USB_WR       0x0110
#define CMD_SYS_RD       0x0200
#define CMD_SYS_WR       0x0210
#define CMD_I2C_RD       0x0300
#define CMD_I2C_WR       0x0310

static const GUID GUID_DEVINTERFACE_USBDevice = 
{ 0x88bae032, 0x5a81, 0x49f0, { 0xbc, 0x3d, 0xa4, 0xff, 0x13, 0x82, 0x16, 0xd6 } };

static const GUID GUID_DEVINTERFACE_ZADIG = 
{ 0xdee824ef, 0x729b, 0x4a0e, { 0x9c, 0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17 } };

static const GUID GUID_DEVINTERFACE_0BDA = 
{ 0x52a4315e, 0x9ec0, 0x419d, { 0x84, 0x52, 0x37, 0x55, 0x95, 0x89, 0x88, 0xa9 } };

bool rtl_ctrl(WINUSB_INTERFACE_HANDLE hWinUsb, uint16_t value, uint16_t index, void *buf, uint16_t len, bool write) {
    WINUSB_SETUP_PACKET pkt = {0};
    pkt.RequestType = write ? 0x40 : 0xC0;
    pkt.Request = 0;
    pkt.Value = value;
    pkt.Index = index;
    pkt.Length = len;
    ULONG transferred = 0;
    return WinUsb_ControlTransfer(hWinUsb, pkt, (PUCHAR)buf, len, &transferred, NULL);
}

bool rtl_rd_reg(WINUSB_INTERFACE_HANDLE hWinUsb, uint16_t reg, uint8_t *val) {
    uint16_t index = (reg < 0x3000) ? CMD_USB_RD : CMD_SYS_RD;
    return rtl_ctrl(hWinUsb, reg, index, val, 1, false);
}

bool rtl_wr_reg(WINUSB_INTERFACE_HANDLE hWinUsb, uint16_t reg, uint8_t val) {
    uint16_t index = (reg < 0x3000) ? CMD_USB_WR : CMD_SYS_WR;
    return rtl_ctrl(hWinUsb, reg, index, &val, 1, true);
}

bool rtl_i2c_read(WINUSB_INTERFACE_HANDLE hWinUsb, uint8_t i2c_addr, uint8_t reg, uint8_t *buf, uint16_t len) {
    uint16_t value = (reg << 8) | (i2c_addr << 1);
    return rtl_ctrl(hWinUsb, value, CMD_I2C_RD, buf, len, false);
}

bool rtl_i2c_write(WINUSB_INTERFACE_HANDLE hWinUsb, uint8_t i2c_addr, uint8_t reg, const uint8_t *buf, uint16_t len) {
    uint16_t value = (reg << 8) | (i2c_addr << 1);
    return rtl_ctrl(hWinUsb, value, CMD_I2C_WR, (void *)buf, len, true);
}

int main(void) {
    printf("=== Realtek RTL2832 Hardware Probe & I2C Scanner ===\n\n");

    char target_path[1024] = {0};
    const GUID *guids[] = { &GUID_DEVINTERFACE_0BDA, &GUID_DEVINTERFACE_USBDevice, &GUID_DEVINTERFACE_ZADIG };

    for (int g = 0; g < 3 && !target_path[0]; g++) {
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
                printf("Found WinUSB device: %s\n", det->DevicePath);
                if (strstr(det->DevicePath, "vid_0bda") || strstr(det->DevicePath, "vid_15f4") ||
                    strstr(det->DevicePath, "VID_0BDA") || strstr(det->DevicePath, "VID_15F4")) {
                    strncpy(target_path, det->DevicePath, sizeof(target_path) - 1);
                    free(det);
                    break;
                }
            }
            free(det);
        }
        SetupDiDestroyDeviceInfoList(hInfo);
    }

    if (!target_path[0]) {
        /* Direct fallback */
        strncpy(target_path, "\\\\?\\USB#VID_0BDA&PID_2838&MI_00#6&22bccd7b&0&0000#{52a4315e-9ec0-419d-8452-3755958988a9}", sizeof(target_path) - 1);
        printf("Using direct fallback path: %s\n", target_path);
    }

    printf("\nOpening: %s\n", target_path);
    HANDLE hDev = CreateFileA(target_path, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (hDev == INVALID_HANDLE_VALUE) {
        printf("CreateFileA failed, err=%u\n", (unsigned)GetLastError());
        return 1;
    }

    WINUSB_INTERFACE_HANDLE hWinUsb = NULL;
    if (!WinUsb_Initialize(hDev, &hWinUsb)) {
        printf("WinUsb_Initialize failed, err=%u\n", (unsigned)GetLastError());
        CloseHandle(hDev);
        return 1;
    }
    printf("WinUSB initialized successfully!\n\n");

    /* Read basic RTL2832 registers */
    uint8_t sys0 = 0, sys1 = 0;
    rtl_rd_reg(hWinUsb, 0x3000, &sys0);
    rtl_rd_reg(hWinUsb, 0x300B, &sys1);
    printf("RTL2832 SYS_DEMOD_CTL (0x3000): 0x%02X\n", sys0);
    printf("RTL2832 SYS_DEMOD_CTL1 (0x300B): 0x%02X\n", sys1);

    /* Turn off suspend, enable demod clock and PLL */
    rtl_wr_reg(hWinUsb, 0x300B, sys1 & ~0x10);
    rtl_wr_reg(hWinUsb, 0x3000, 0xE0);

    /* Open I2C repeater gate (0x101 = 0x18) */
    uint8_t gate = 0x18;
    rtl_ctrl(hWinUsb, 0x0120, CMD_DEMOD_WR | 1, &gate, 1, true);
    rtl_ctrl(hWinUsb, 0x0101, CMD_DEMOD_WR | 1, &gate, 1, true);

    /* Dump EEPROM (I2C addr 0x50, 8-bit 0xA0) */
    printf("\n--- Reading 24C02 / 24C16 EEPROM (I2C 0x50) ---\n");
    uint8_t eeprom[64] = {0};
    if (rtl_i2c_read(hWinUsb, 0x50, 0x00, eeprom, 32)) {
        printf("EEPROM (first 32 bytes):\n  ");
        for (int i = 0; i < 32; i++) {
            printf("%02X ", eeprom[i]);
            if ((i + 1) % 16 == 0 && i < 31) printf("\n  ");
        }
        printf("\n");
        /* Decode standard RTL EEPROM VID/PID */
        if (eeprom[0] == 0x28 && eeprom[1] == 0x32) {
            uint16_t vid = eeprom[2] | (eeprom[3] << 8);
            uint16_t pid = eeprom[4] | (eeprom[5] << 8);
            printf("EEPROM Header '28 32' -> Configured VID:PID = %04X:%04X\n", vid, pid);
        }
    } else {
        printf("EEPROM not responding or unpopulated.\n");
    }

    /* I2C Full Bus Scan */
    printf("\n--- Scanning I2C Bus (Addresses 0x08 - 0x77) ---\n");
    int found_count = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        uint8_t data = 0;
        if (rtl_i2c_read(hWinUsb, addr, 0x00, &data, 1)) {
            printf("  -> I2C Device found at 0x%02X (8-bit 0x%02X) - Reg 0x00 = 0x%02X\n", addr, addr << 1, data);
            found_count++;
        }
    }
    printf("Total I2C devices responding to Reg 0x00: %d\n", found_count);

    /* Specific Chip Inspections */
    printf("\n--- Detailed Chip Inspections ---\n");

    /* 1. Panasonic Demodulator check (0x18 or 0x1C) */
    uint8_t chip_id = 0;
    if (rtl_i2c_read(hWinUsb, 0x18, 0xFF, &chip_id, 1)) {
        printf("[DEMOD] Responding at I2C 0x18 (Bank 0): Chip ID = 0x%02X\n", chip_id);
        if (chip_id == 0x02) printf("  ==> Identified as PANASONIC MN88472 (DVB-T/T2/C Rev. 1, Parallel TS)!\n");
    } else {
        printf("[DEMOD] I2C 0x18 (MN88472): No response\n");
    }

    if (rtl_i2c_read(hWinUsb, 0x1C, 0xFF, &chip_id, 1)) {
        printf("[DEMOD] Responding at I2C 0x1C: Chip ID = 0x%02X\n", chip_id);
        if (chip_id == 0x03) printf("  ==> Identified as PANASONIC MN88473 (DVB-T/T2/C Rev. 2, Serial TS)!\n");
    } else {
        printf("[DEMOD] I2C 0x1C (MN88473): No response\n");
    }

    /* 2. Sony Demodulator check (0x6C / 0x6D) */
    uint8_t sony_id = 0;
    if (rtl_i2c_read(hWinUsb, 0x6C, 0x00, &sony_id, 1) || rtl_i2c_read(hWinUsb, 0x6D, 0x00, &sony_id, 1)) {
        printf("[DEMOD] Responding at I2C 0x6C/0x6D: Reg 0 = 0x%02X\n", sony_id);
        printf("  ==> Identified as SONY CXD2837ER (DVB-T/T2)!\n");
    } else {
        printf("[DEMOD] I2C 0x6C/0x6D (Sony CXD2837ER): No response\n");
    }

    /* 3. Rafael Micro Tuner check (0x34 or 0x3A) */
    uint8_t r820_id = 0;
    if (rtl_i2c_read(hWinUsb, 0x3A, 0x00, &r820_id, 1)) {
        printf("[TUNER] Responding at I2C 0x3A: Reg 0 = 0x%02X\n", r820_id);
        printf("  ==> Identified as Rafael Micro R828D / R820T!\n");
    } else if (rtl_i2c_read(hWinUsb, 0x34, 0x00, &r820_id, 1)) {
        printf("[TUNER] Responding at I2C 0x34: Reg 0 = 0x%02X\n", r820_id);
        printf("  ==> Identified as Rafael Micro R820T (alternate address)!\n");
    } else {
        printf("[TUNER] I2C 0x34/0x3A (Rafael Micro): No response\n");
    }

    /* 4. Fitipower Tuner check (0x10) */
    uint8_t fc_id = 0;
    if (rtl_i2c_read(hWinUsb, 0x10, 0x00, &fc_id, 1)) {
        printf("[TUNER] Responding at I2C 0x10: Reg 0 = 0x%02X\n", fc_id);
        printf("  ==> Identified as Fitipower FC0012 / FC0013!\n");
    }

    /* 5. Elonics E4000 check (0x64 / 0x65) */
    uint8_t e4k_id = 0;
    if (rtl_i2c_read(hWinUsb, 0x64, 0x00, &e4k_id, 1) || rtl_i2c_read(hWinUsb, 0x65, 0x00, &e4k_id, 1)) {
        printf("[TUNER] Responding at I2C 0x64/0x65: Reg 0 = 0x%02X\n", e4k_id);
        printf("  ==> Identified as Elonics E4000!\n");
    }

    WinUsb_Free(hWinUsb);
    CloseHandle(hDev);
    printf("\n=== Probe Complete ===\n");
    return 0;
}
