#include "xbox_device.h"
#include <stdio.h>

#define REQUEST_I2C_READ       0x02
#define REQUEST_I2C_WRITE      0x03
#define REQUEST_SET_USB_XFER_LEN 0x07
#define REQUEST_ENABLE_VIDEO_STREAM 0x09
#define REQUEST_SET_CLOCK      0x0B
#define REQUEST_SET_GPIO       0x0D
#define REQUEST_SET_I2C_SPEED  0x14

int dib0700_ctrl_wr(xbox_device_t *dev, uint8_t *tx, uint8_t txlen) {
    int ret = xbox_usb_control_msg(dev,
                                   0x40, /* USB_TYPE_VENDOR | USB_DIR_OUT */
                                   tx[0],
                                   0, 0,
                                   tx, txlen, 1000);
    return (ret == txlen) ? 0 : -1;
}

int dib0700_ctrl_rd(xbox_device_t *dev, uint8_t *tx, uint8_t txlen, uint8_t *rx, uint8_t rxlen) {
    int ret = xbox_usb_control_msg(dev,
                                   0x40, /* USB_TYPE_VENDOR | USB_DIR_OUT */
                                   tx[0],
                                   0, 0,
                                   tx, txlen, 1000);
    if (ret != txlen) return -1;

    ret = xbox_usb_control_msg(dev,
                               0xC0, /* USB_TYPE_VENDOR | USB_DIR_IN */
                               tx[0],
                               0, 0,
                               rx, rxlen, 1000);
    return ret;
}

/* Atomic I2C Write via REQUEST_I2C_WRITE */
int dib0700_i2c_write(xbox_device_t *dev, uint8_t addr, const uint8_t *buf, int len) {
    uint8_t tx[64];
    if (len + 2 > sizeof(tx)) return -1;

    tx[0] = REQUEST_I2C_WRITE;
    tx[1] = addr << 1;
    memcpy(&tx[2], buf, len);

    return dib0700_ctrl_wr(dev, tx, len + 2);
}

/* Atomic Combined I2C Write+Read via REQUEST_I2C_READ (Our QAM-256 Fix!) */
int dib0700_i2c_read(xbox_device_t *dev, uint8_t addr, uint8_t reg, uint8_t *buf, int len) {
    uint8_t tx[4];
    uint8_t rx[64];
    if (len > sizeof(rx)) return -1;

    tx[0] = REQUEST_I2C_READ;
    tx[1] = (addr << 1) | 0x01;
    tx[2] = reg;

    int ret = dib0700_ctrl_rd(dev, tx, 3, rx, len);
    if (ret <= 0) return -1;

    memcpy(buf, rx, len);
    return 0;
}

bool dib0700_set_gpio(xbox_device_t *dev, uint8_t gpio, uint8_t dir, uint8_t val) {
    uint8_t b[4];
    b[0] = REQUEST_SET_GPIO;
    b[1] = gpio;
    b[2] = ((dir & 0x01) << 7) | ((val & 0x01) << 6);
    b[3] = 0;
    return (dib0700_ctrl_wr(dev, b, 4) == 0);
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

/* 72 MHz Clock Configuration */
static bool dib0700_ctrl_clock(xbox_device_t *dev, uint32_t clk_mhz, uint8_t clock_gpio3) {
    if (clk_mhz == 72) {
        return dib0700_set_clock(dev, 1, 0, 1, clock_gpio3, 2, 24);
    }
    return false;
}

/* 1000 kHz Fast I2C Speed Configuration */
static bool dib0700_set_i2c_speed(xbox_device_t *dev, uint16_t speed_khz) {
    uint8_t b[4];
    uint16_t divider;

    if (speed_khz >= 1000) divider = 1;
    else if (speed_khz >= 400) divider = 2;
    else divider = 8;

    b[0] = REQUEST_SET_I2C_SPEED;
    b[1] = 0;
    b[2] = (divider >> 8) & 0xff;
    b[3] = divider & 0xff;
    return (dib0700_ctrl_wr(dev, b, 4) == 0);
}

bool dib0700_init(xbox_device_t *dev) {
    /* 1. Set Internal Clock to 72 MHz (Our QAM-256 Fix!) */
    dib0700_ctrl_clock(dev, 72, 1);

    /* 2. Set I2C speed to 1000 kHz Fast Mode (Our QAM-256 Fix!) */
    dib0700_set_i2c_speed(dev, 1000);

    /* 3. Frontend Power Enable via GPIO6 */
    dib0700_set_gpio(dev, 6, 1, 0); /* GPIO6 OUT 0 */
    Sleep(30);
    dib0700_set_gpio(dev, 6, 1, 1); /* GPIO6 OUT 1 */
    Sleep(30);

    /* 4. Reset Demod via GPIO10 */
    dib0700_set_gpio(dev, 10, 1, 0); /* GPIO10 OUT 0 */
    Sleep(30);
    dib0700_set_gpio(dev, 10, 1, 1); /* GPIO10 OUT 1 */
    Sleep(30);

    /* 5. Set MPEG-TS Packet Buffer Size */
    uint8_t b[4];
    b[0] = REQUEST_SET_USB_XFER_LEN;
    b[1] = 0;
    b[2] = 0;
    b[3] = 21; /* 21 * 188 = 3948 bytes (standard USB buffer) */
    dib0700_ctrl_wr(dev, b, 4);

    return true;
}
