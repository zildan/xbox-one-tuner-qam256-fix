#include "xbox_device.h"
#include <stdio.h>
#include <stdlib.h>

#define REQUEST_I2C_READ       0x02
#define REQUEST_I2C_WRITE      0x03
#define REQUEST_JUMPRAM        0x08
#define REQUEST_SET_CLOCK      0x0B
#define REQUEST_SET_GPIO       0x0C
#define REQUEST_ENABLE_VIDEO   0x0F
#define REQUEST_SET_I2C_PARAM  0x10
#define REQUEST_GET_VERSION    0x15

#define GPIO6_PIN  8
#define GPIO10_PIN 15

int dib0700_ctrl_wr(xbox_device_t *dev, uint8_t *tx, uint8_t txlen) {
    int ret = xbox_usb_control_msg(dev,
                                   0x40, /* USB_TYPE_VENDOR | USB_DIR_OUT */
                                   tx[0],
                                   0, 0,
                                   tx, txlen, 1000);
    return (ret == txlen) ? 0 : -1;
}

int dib0700_ctrl_rd(xbox_device_t *dev, uint8_t *tx, uint8_t txlen, uint8_t *rx, uint8_t rxlen) {
    uint16_t value = ((txlen - 2) << 8) | tx[1];
    uint16_t index = 0;
    if (txlen > 2)
        index |= (tx[2] << 8);
    if (txlen > 3)
        index |= tx[3];

    int status = xbox_usb_control_msg(dev,
                                      0xC0, /* USB_TYPE_VENDOR | USB_DIR_IN */
                                      tx[0],
                                      value, index,
                                      rx, rxlen, 1000);
    return status;
}

int dib0700_i2c_write(xbox_device_t *dev, uint8_t addr, const uint8_t *buf, int len) {
    uint8_t tx[64];
    if (len + 2 > (int)sizeof(tx)) return -1;

    tx[0] = REQUEST_I2C_WRITE;
    tx[1] = addr << 1;
    memcpy(&tx[2], buf, len);

    return dib0700_ctrl_wr(dev, tx, len + 2);
}

int dib0700_i2c_read(xbox_device_t *dev, uint8_t addr, uint8_t reg, uint8_t *buf, int len) {
    uint8_t tx[4];
    uint8_t rx[64];
    if (len > (int)sizeof(rx)) return -1;

    tx[0] = REQUEST_I2C_READ;
    tx[1] = (addr << 1) | 0x01;
    tx[2] = reg;

    int ret = dib0700_ctrl_rd(dev, tx, 3, rx, len);
    if (ret <= 0) return -1;

    memcpy(buf, rx, len);
    return 0;
}

bool dib0700_set_gpio(xbox_device_t *dev, uint8_t gpio, uint8_t dir, uint8_t val) {
    uint8_t b[3];
    b[0] = REQUEST_SET_GPIO;
    b[1] = gpio;
    b[2] = ((dir & 0x01) << 7) | ((val & 0x01) << 6);
    return (dib0700_ctrl_wr(dev, b, 3) == 0);
}

static bool dib0700_set_clock(xbox_device_t *dev, uint8_t en_pll, uint8_t pll_src,
                              uint8_t pll_range, uint8_t clock_gpio3, uint16_t pll_prediv,
                              uint16_t pll_loopdiv) {
    uint8_t b[10];
    b[0] = REQUEST_SET_CLOCK;
    b[1] = (en_pll << 7) | (pll_src << 6) | (pll_range << 5) | (clock_gpio3 << 4);
    b[2] = (pll_prediv >> 8) & 0xff;
    b[3] = pll_prediv & 0xff;
    b[4] = (pll_loopdiv >> 8) & 0xff;
    b[5] = pll_loopdiv & 0xff;
    b[6] = 0;
    b[7] = 0;
    b[8] = 0;
    b[9] = 0;
    return (dib0700_ctrl_wr(dev, b, 10) == 0);
}

static bool dib0700_ctrl_clock(xbox_device_t *dev, uint32_t clk_mhz, uint8_t clock_gpio3) {
    if (clk_mhz == 72) {
        return dib0700_set_clock(dev, 1, 0, 1, clock_gpio3, 2, 24);
    }
    return false;
}

static bool dib0700_set_i2c_speed(xbox_device_t *dev, uint16_t speed_khz) {
    uint8_t b[8];
    uint16_t divider;

    if (speed_khz == 0) return false;

    b[0] = REQUEST_SET_I2C_PARAM;
    divider = (uint16_t)(30000 / speed_khz);
    b[1] = 0;
    b[2] = (uint8_t)(divider >> 8);
    b[3] = (uint8_t)(divider & 0xff);
    divider = (uint16_t)(72000 / speed_khz);
    b[4] = (uint8_t)(divider >> 8);
    b[5] = (uint8_t)(divider & 0xff);
    b[6] = (uint8_t)(divider >> 8);
    b[7] = (uint8_t)(divider & 0xff);

    return (dib0700_ctrl_wr(dev, b, 8) == 0);
}

/* Parse and load DiB0700 USB firmware from Intel HEX format */
static bool dib0700_download_firmware(xbox_device_t *dev) {
    char fw_path[MAX_PATH];
    FILE *fp = NULL;

    if (g_module_dir[0] != '\0') {
        snprintf(fw_path, sizeof(fw_path), "%s%s", g_module_dir, DIB0700_FIRMWARE_NAME);
        fp = fopen(fw_path, "rb");
    }
    if (!fp) {
        snprintf(fw_path, sizeof(fw_path), "Devices/%s", DIB0700_FIRMWARE_NAME);
        fp = fopen(fw_path, "rb");
    }
    if (!fp) {
        snprintf(fw_path, sizeof(fw_path), "%s", DIB0700_FIRMWARE_NAME);
        fp = fopen(fw_path, "rb");
    }
    if (!fp) {
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *fw_data = (uint8_t *)malloc(fsize);
    if (!fw_data) {
        fclose(fp);
        return false;
    }
    fread(fw_data, 1, fsize, fp);
    fclose(fp);

    /* Test if already warm (running firmware) */
    uint8_t ver_buf[16] = {0};
    int ret = xbox_usb_control_msg(dev, 0xC0, REQUEST_GET_VERSION, 0, 0, ver_buf, 16, 500);
    if (ret > 0) {
        free(fw_data);
        return true;
    }

    /* Cold State: Download Intel Hex Records through Bulk Pipe 0x01 */
    int pos = 0;
    uint8_t buf[260];
    ULONG written = 0;
    int records = 0;

    while (pos < fsize) {
        uint8_t *b = &fw_data[pos];
        uint8_t len = b[0];
        if (pos + len + 4 >= fsize) break;

        uint32_t addr = b[1] | (b[2] << 8);
        uint8_t type = b[3];
        if (type == 0x04) {
            addr |= (b[4] << 24) | (b[5] << 16);
        }

        buf[0] = len;
        buf[1] = (addr >> 8) & 0xff;
        buf[2] = addr & 0xff;
        buf[3] = type;
        memcpy(&buf[4], &b[4], len);
        buf[4 + len] = b[4 + len]; /* chk */

        BOOL wres = WinUsb_WritePipe(dev->hWinUsb, 0x01, buf, len + 5, &written, NULL);
        if (!wres) {
            free(fw_data);
            return false;
        }
        records++;
        pos += len + 5;
    }
    free(fw_data);

    /* JumpRAM to 0x70000000 to start DiB0700 CPU */
    uint8_t jmp[8];
    jmp[0] = REQUEST_JUMPRAM;
    jmp[1] = 0;
    jmp[2] = 0;
    jmp[3] = 0;
    jmp[4] = 0x70;
    jmp[5] = 0x00;
    jmp[6] = 0x00;
    jmp[7] = 0x00;
    BOOL jres = WinUsb_WritePipe(dev->hWinUsb, 0x01, jmp, 8, &written, NULL);
    if (!jres) {
        return false;
    }

    Sleep(500);

    /* Verify DiB0700 transitioned to warm state */
    memset(ver_buf, 0, sizeof(ver_buf));
    ret = xbox_usb_control_msg(dev, 0xC0, REQUEST_GET_VERSION, 0, 0, ver_buf, 16, 1000);
    return (ret > 0);
}

bool dib0700_init(xbox_device_t *dev) {
    /* 0. Cold boot firmware load if tuner was unplugged / power cycled */
    dib0700_download_firmware(dev);

    /* 1. Set Internal Clock to 72 MHz (Our QAM-256 Fix!) */
    dib0700_ctrl_clock(dev, 72, 1);

    /* 2. Set I2C speed to 1000 kHz Fast Mode Plus (Our QAM-256 Fix!) */
    dib0700_set_i2c_speed(dev, 1000);

    /* 2. Frontend Power Enable via GPIO6 (Physical Pin 8) */
    dib0700_set_gpio(dev, GPIO6_PIN, 1, 0);
    Sleep(30);
    dib0700_set_gpio(dev, GPIO6_PIN, 1, 1);
    Sleep(30);

    /* 3. Reset Demod via GPIO10 (Physical Pin 15) */
    dib0700_set_gpio(dev, GPIO10_PIN, 1, 1);
    Sleep(30);
    dib0700_set_gpio(dev, GPIO10_PIN, 1, 0);
    Sleep(30);
    dib0700_set_gpio(dev, GPIO10_PIN, 1, 1);
    Sleep(30);

    return true;
}

int dib0700_set_streaming(xbox_device_t *dev, int onoff) {
    uint8_t buf[4];
    buf[0] = REQUEST_ENABLE_VIDEO; /* 0x0F */
    buf[1] = (onoff != 0) ? 0x10 : 0x00;
    buf[2] = (onoff != 0) ? 0x01 : 0x00; /* Channel 0, slave mode */
    buf[3] = 0x00;
    return dib0700_ctrl_wr(dev, buf, 4);
}
