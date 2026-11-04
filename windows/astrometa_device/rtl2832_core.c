#include "astrometa_device.h"
#include <stdio.h>

#define CMD_DEMOD_RD     0x0000
#define CMD_DEMOD_WR     0x0010
#define CMD_USB_RD       0x0100
#define CMD_USB_WR       0x0110

#define SYS_DEMOD_CTL    0x3000
#define SYS_DEMOD_CTL1   0x300B

bool rtl2832_ctrl(astrometa_device_t *dev, uint16_t value, uint16_t index, void *buf, uint16_t len, bool write) {
    if (!dev || !dev->hWinUsb) return false;
    WINUSB_SETUP_PACKET pkt = {0};
    pkt.RequestType = write ? 0x40 : 0xC0;
    pkt.Request = 0;
    pkt.Value = value;
    pkt.Index = index;
    pkt.Length = len;
    ULONG transferred = 0;
    return WinUsb_ControlTransfer(dev->hWinUsb, pkt, (PUCHAR)buf, len, &transferred, NULL);
}

bool rtl2832_rd_reg(astrometa_device_t *dev, uint16_t reg, uint8_t *val) {
    uint16_t index = (reg < 0x3000) ? CMD_USB_RD : CMD_SYS_RD;
    return rtl2832_ctrl(dev, reg, index, val, 1, false);
}

bool rtl2832_wr_reg(astrometa_device_t *dev, uint16_t reg, uint8_t val) {
    uint16_t index = (reg < 0x3000) ? CMD_USB_WR : CMD_SYS_WR;
    return rtl2832_ctrl(dev, reg, index, &val, 1, true);
}

bool rtl2832_wr_reg_mask(astrometa_device_t *dev, uint16_t reg, uint8_t val, uint8_t mask) {
    uint8_t tmp = 0;
    if (mask != 0xff) {
        if (!rtl2832_rd_reg(dev, reg, &tmp)) return false;
        val &= mask;
        tmp &= ~mask;
        val |= tmp;
    }
    return rtl2832_wr_reg(dev, reg, val);
}

bool rtl2832_open_gate(astrometa_device_t *dev) {
    uint8_t gate_open = 0x18;
    return rtl2832_ctrl(dev, 0x0120, CMD_DEMOD_WR | 1, &gate_open, 1, true);
}

bool rtl2832_close_gate(astrometa_device_t *dev) {
    uint8_t gate_close = 0x10;
    return rtl2832_ctrl(dev, 0x0120, CMD_DEMOD_WR | 1, &gate_close, 1, true);
}

bool rtl2832_i2c_read(astrometa_device_t *dev, uint8_t i2c_addr, uint8_t reg, uint8_t *buf, uint16_t len) {
    uint16_t value = (reg << 8) | (i2c_addr << 1);
    return rtl2832_ctrl(dev, value, CMD_I2C_RD, buf, len, false);
}

bool rtl2832_i2c_write(astrometa_device_t *dev, uint8_t i2c_addr, uint8_t reg, const uint8_t *buf, uint16_t len) {
    /* Upstream rtl28xxu: Method 2 old I2C up to 22 bytes per transfer */
    uint16_t value = (reg << 8) | (i2c_addr << 1);
    return rtl2832_ctrl(dev, value, CMD_I2C_WR, (void *)buf, len, true);
}

bool rtl2832_init(astrometa_device_t *dev) {
    /* 1. Bridge & Power Init matching test_r820t_port */
    rtl2832_wr_reg_mask(dev, SYS_GPIO_DIR,    0x00, 0x40);
    rtl2832_wr_reg_mask(dev, SYS_GPIO_OUT_EN, 0x48, 0x48);

    /* Tuner power: GPIO3=1, GPIO4=0 */
    rtl2832_wr_reg_mask(dev, SYS_GPIO_DIR,     0x00, 0x18);
    rtl2832_wr_reg_mask(dev, SYS_GPIO_OUT_EN,  0x18, 0x18);
    rtl2832_wr_reg_mask(dev, SYS_GPIO_OUT_VAL, 0x08, 0x18);

    /* 2. Suspend off */
    rtl2832_wr_reg_mask(dev, SYS_DEMOD_CTL1, 0x00, 0x10);

    /* 3. Enable demod PLL (0x80) and disable reset (0x20), keep internal ADC off (0xA0) */
    rtl2832_wr_reg_mask(dev, SYS_DEMOD_CTL, 0xA0, 0xE0);

    /* 4. Power cycle reset slave demod on GPIO0 */
    rtl2832_wr_reg_mask(dev, SYS_GPIO_OUT_VAL, 0x00, 0x01);
    rtl2832_wr_reg_mask(dev, SYS_GPIO_OUT_EN,  0x00, 0x01);
    Sleep(50);
    rtl2832_wr_reg_mask(dev, SYS_GPIO_OUT_VAL, 0x01, 0x01);
    rtl2832_wr_reg_mask(dev, SYS_GPIO_DIR,     0x00, 0x01);
    rtl2832_wr_reg_mask(dev, SYS_GPIO_OUT_EN,  0x01, 0x01);
    Sleep(50);

    /* 5. Open I2C repeater gate */
    rtl2832_open_gate(dev);

    /* 6. Configure USB EPA FIFO and DMA mode */
    uint8_t sysctl = 0;
    rtl2832_ctrl(dev, 0x2000, CMD_USB_RD, &sysctl, 1, false);
    sysctl |= 0x09; /* enable DMA and Full Packet Mode */
    rtl2832_ctrl(dev, 0x2000, CMD_USB_WR, &sysctl, 1, true);

    uint8_t epa_maxpkt[4] = {0x00, 0x02, 0x00, 0x00}; /* 512 bytes */
    rtl2832_ctrl(dev, 0x2158, CMD_USB_WR, epa_maxpkt, 4, true);

    uint8_t epa_fifo[4] = {0x14, 0x00, 0x00, 0x00};
    rtl2832_ctrl(dev, 0x2160, CMD_USB_WR, epa_fifo, 4, true);

    uint8_t epa_ctl[2] = {0x00, 0x00};
    rtl2832_ctrl(dev, 0x2148, CMD_USB_WR, epa_ctl, 2, true);

    return true;
}

bool rtl2832_set_streaming(astrometa_device_t *dev, bool enable) {
    if (enable) {
        /* 1. Suspend off */
        rtl2832_wr_reg_mask(dev, SYS_DEMOD_CTL1, 0x00, 0x10);

        /* 2. Enable demod PLL (0x80), enable TS DMA to EPA (0x40), and disable reset (0x20) */
        rtl2832_wr_reg_mask(dev, SYS_DEMOD_CTL, 0xE0, 0xE0);

        /* 3. Configure USB EPA FIFO and DMA mode */
        uint8_t sysctl = 0;
        rtl2832_ctrl(dev, 0x2000, CMD_USB_RD, &sysctl, 1, false);
        sysctl |= 0x09; /* enable DMA and Full Packet Mode */
        rtl2832_ctrl(dev, 0x2000, CMD_USB_WR, &sysctl, 1, true);

        uint8_t epa_maxpkt[4] = {0x00, 0x02, 0x00, 0x00}; /* 512 bytes */
        rtl2832_ctrl(dev, 0x2158, CMD_USB_WR, epa_maxpkt, 4, true);

        uint8_t epa_fifo[4] = {0x14, 0x00, 0x00, 0x00};
        rtl2832_ctrl(dev, 0x2160, CMD_USB_WR, epa_fifo, 4, true);

        /* 4. Slave TS Routing matching Linux rtl2832_slave_ts_ctrl */
        /* Page 1, 0x01: DVBT_SOFT_RST = 0 (bit 2 = 0), keep I2C repeater open (bits 4,3 = 0x18) */
        uint8_t reg101 = 0x18;
        rtl2832_ctrl(dev, 0x0120, CMD_DEMOD_WR | 1, &reg101, 1, true);

        /* Page 1, 0x0C: IF_AGC Manual & Max Val */
        uint8_t ts_conf[2] = {0x5F, 0xFF};
        rtl2832_ctrl(dev, 0x0C20, CMD_DEMOD_WR | 1, ts_conf, 2, true);

        /* Page 0, 0x21: PIP_ON = 1 (bit 3 = 0x08) */
        uint8_t reg21 = 0;
        rtl2832_ctrl(dev, 0x2120, CMD_DEMOD_RD | 0, &reg21, 1, false);
        reg21 |= 0x08;
        rtl2832_ctrl(dev, 0x2120, CMD_DEMOD_WR | 0, &reg21, 1, true);

        /* Page 0, 0xBC: External slave TS clock/sync settings (0x18) */
        uint8_t bc = 0x18;
        rtl2832_ctrl(dev, 0xBC20, CMD_DEMOD_WR | 0, &bc, 1, true);

        /* Page 1, 0x92: Slave TS multiplexer routing */
        uint8_t ts_mux[3] = {0x7F, 0xF7, 0xFF};
        rtl2832_ctrl(dev, 0x9220, CMD_DEMOD_WR | 1, ts_mux, 3, true);

        /* 5. Clear EPA stall and reset */
        uint8_t epa[2] = {0x00, 0x00};
        rtl2832_ctrl(dev, 0x2148, CMD_USB_WR, epa, 2, true);

        log_dbg("rtl2832_set_streaming: Slave TS routing applied!\n");
    } else {
        /* Set EPA stall and reset */
        uint8_t epa[2] = {0x10, 0x02};
        rtl2832_ctrl(dev, 0x2148, CMD_USB_WR, epa, 2, true);
    }
    return true;
}
