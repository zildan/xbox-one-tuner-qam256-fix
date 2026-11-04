#include "xbox_device.h"
#include <stdio.h>

#define TDA18250_I2C_ADDR 0x60

#define R00_ID1      0x00
#define R06_POWER    0x06
#define R08_IRQ1     0x08
#define R0A_IRQ3     0x0a
#define R0C_AGC11    0x0c
#define R0F_AGC14    0x0f
#define R10_LT1      0x10
#define R12_AGC21    0x12
#define R13_AGC22    0x13
#define R14_AGC23    0x14
#define R16_AGC25    0x16
#define R17_AGC31    0x17
#define R1A_AGCK     0x1a
#define R1F_RF_BPF   0x1f
#define R20_IR_MIX   0x20
#define R25_POWER2   0x25
#define R26_IF       0x26
#define R27_RF1      0x27
#define R2A_MSM1     0x2a
#define R2B_MSM2     0x2b
#define R30_RSSI2    0x30
#define R34_MD1      0x34
#define R39_SD5      0x39
#define R46_CPUMP    0x46
#define R4D_XTALFLX1 0x4d
#define R4F_XTALFLX3 0x4f
#define R58_AGC2_UP1 0x58
#define R5A_H3H5     0x5a

#define TDA18250_IRQ_CAL     0x81
#define TDA18250_IRQ_HW_INIT 0x82
#define TDA18250_IRQ_TUNE    0x88

static bool tda18250_write_reg(xbox_device_t *dev, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return (dib0700_i2c_write(dev, TDA18250_I2C_ADDR, buf, 2) == 0);
}

static uint8_t tda18250_read_reg(xbox_device_t *dev, uint8_t reg) {
    uint8_t val = 0;
    dib0700_i2c_read(dev, TDA18250_I2C_ADDR, reg, &val, 1);
    return val;
}

static bool tda18250_write_bits(xbox_device_t *dev, uint8_t reg, uint8_t mask, uint8_t val) {
    uint8_t cur = tda18250_read_reg(dev, reg);
    cur = (cur & ~mask) | (val & mask);
    return tda18250_write_reg(dev, reg, cur);
}

static bool tda18250_wait_for_irq(xbox_device_t *dev, uint8_t irq, int maxwait_ms) {
    for (int i = 0; i < maxwait_ms / 10; i++) {
        uint8_t st = tda18250_read_reg(dev, R08_IRQ1);
        if ((st & irq) == irq) {
            return true;
        }
        Sleep(10);
    }
    return false;
}

bool tda18250_init(xbox_device_t *dev) {
    uint8_t id[3] = {0};
    dib0700_i2c_read(dev, TDA18250_I2C_ADDR, R00_ID1, id, 3);

    if (id[0] != 0xc7 && id[0] != 0x47) {
        return false;
    }

    /* 1. Power up to Normal mode */
    tda18250_write_bits(dev, R06_POWER, 0x07, 0x00);
    tda18250_write_bits(dev, R25_POWER2, 0xc0, 0xc0);
    Sleep(20);

    /* 2. Program 34 standard init registers from Linux kernel driver */
    static const uint8_t init_regs[][2] = {
        { 0x0c, 0xc7 }, { 0x0d, 0x5d }, { 0x0e, 0x40 }, { 0x0f, 0x0e },
        { 0x10, 0x47 }, { 0x11, 0x4e }, { 0x12, 0x26 }, { 0x13, 0x60 },
        { 0x18, 0x37 }, { 0x19, 0x09 }, { 0x1a, 0x00 }, { 0x1e, 0x29 },
        { 0x1f, 0x06 }, { 0x20, 0xc6 }, { 0x21, 0x00 }, { 0x2c, 0x75 },
        { 0x2d, 0x06 }, { 0x2e, 0x07 }, { 0x30, 0x0e }, { 0x31, 0x00 },
        { 0x39, 0x00 }, { 0x3b, 0x55 }, { 0x3c, 0xa7 }, { 0x3f, 0x85 },
        { 0x40, 0x87 }, { 0x41, 0xc0 }, { 0x43, 0x40 }, { 0x44, 0xc0 },
        { 0x46, 0x0c }, { 0x47, 0x64 }, { 0x4b, 0x30 }, { 0x59, 0x05 },
        { 0x5b, 0x07 }, { 0x5c, 0x00 }
    };
    for (int i = 0; i < (int)(sizeof(init_regs)/sizeof(init_regs[0])); i++) {
        tda18250_write_reg(dev, init_regs[i][0], init_regs[i][1]);
    }

    /* 3. Program 27 MHz Crystal registers */
    static const uint8_t xtal_27mhz[5] = { 0x69, 0x78, 0x8d, 0x80, 0x1b };
    for (int i = 0; i < 5; i++) {
        tda18250_write_reg(dev, R4D_XTALFLX1 + i, xtal_27mhz[i]);
    }

    /* 4. Enable loopthrough */
    tda18250_write_bits(dev, R10_LT1, 0x80, 0x00);

    /* 5. Trigger HW Init calibration and wait for IRQ 0x82 */
    tda18250_write_reg(dev, R0A_IRQ3, TDA18250_IRQ_HW_INIT);
    tda18250_write_reg(dev, R2A_MSM1, 0x70);
    tda18250_write_reg(dev, R2B_MSM2, 0x01);
    tda18250_wait_for_irq(dev, TDA18250_IRQ_HW_INIT, 500);

    /* 6. Trigger Tuner calibration and wait for IRQ 0x81 */
    tda18250_write_reg(dev, R0A_IRQ3, TDA18250_IRQ_CAL);
    tda18250_write_reg(dev, R2A_MSM1, 0x02);
    tda18250_write_reg(dev, R2B_MSM2, 0x01);
    tda18250_wait_for_irq(dev, TDA18250_IRQ_CAL, 500);

    /* 7. Power up LNA (Clear bit 7 of R0C_AGC11) */
    tda18250_write_bits(dev, R0C_AGC11, 0x80, 0x00);

    return true;
}

/* Full Linux Kernel TDA18250 DVB-C 8MHz (4950 kHz IF, 27 MHz XTAL) */
bool tda18250_tune_dvbc(xbox_device_t *dev, uint32_t freq_hz) {
    static const uint8_t regs[16] = {
        0x22, 0x23, 0x24, 0x21, 0x0d, 0x0c, 0x0f, 0x14,
        0x0e, 0x12, 0x58, 0x59, 0x1a, 0x19, 0x1e, 0x30
    };
    static const uint8_t masks[16] = {
        0x77, 0xff, 0xff, 0x87, 0xf0, 0x78, 0x07, 0xe0,
        0x60, 0x0f, 0x60, 0x0f, 0x33, 0x30, 0x80, 0x06
    };
    static const uint8_t dvbc_8[16] = {
        0x53, 0x03, 0x88, 0x82, 0x50, 0x00, 0x06, 0x60,
        0x40, 0x0e, 0x60, 0x05, 0x33, 0x10, 0x00, 0x04
    };

    uint32_t freq_khz = freq_hz / 1000;
    uint32_t if_khz = 4950; /* Xbox One Tuner IF frequency */

    /* 1. Delivery system dependent registers */
    for (int i = 0; i < 16; i++) {
        tda18250_write_bits(dev, regs[i], masks[i], dvbc_8[i]);
    }

    /* 2. Set IF: 4950 / 50 = 99 (0x63) */
    tda18250_write_reg(dev, R26_IF, 99);

    /* 3. Set AGC parameters */
    tda18250_write_bits(dev, R1F_RF_BPF, 0x87, 0x06);
    tda18250_write_reg(dev, R5A_H3H5, 0x00);
    tda18250_write_bits(dev, R0C_AGC11, 0x07, (freq_hz < 100000000) ? 2 : 3);

    uint8_t utmp  = (freq_hz < 320000000) ? 16 : 18;
    uint8_t utmp2 = (freq_hz < 320000000) ? 18 : 20;
    tda18250_write_bits(dev, R58_AGC2_UP1, 0x1f, utmp2 + 8);
    tda18250_write_bits(dev, R13_AGC22, 0x1f, utmp);
    tda18250_write_bits(dev, R14_AGC23, 0x1f, utmp2);
    tda18250_write_bits(dev, R16_AGC25, 0xf8, 90);
    tda18250_write_bits(dev, R12_AGC21, 0x60, (freq_hz > 800000000) ? 0x40 : 0x20);
    tda18250_write_reg(dev, R17_AGC31, (7 << 4) | 12);
    tda18250_write_bits(dev, R20_IR_MIX, 0x06, ((freq_hz > 320000000) && (freq_hz < 600000000)) ? 0x02 : 0x04);
    tda18250_write_bits(dev, R16_AGC25, 0x03, (freq_hz < 600000000) ? 0 : 3);
    tda18250_write_bits(dev, R0F_AGC14, 0x3f, 0x0c);

    tda18250_write_bits(dev, R1A_AGCK, 0x03, 0x01);
    tda18250_write_bits(dev, R14_AGC23, 0x40, 0x00);

    /* 4. Set frequency registers (3 bytes, kHz) */
    tda18250_write_reg(dev, R27_RF1, (freq_khz >> 16) & 0xff);
    tda18250_write_reg(dev, R27_RF1 + 1, (freq_khz >> 8) & 0xff);
    tda18250_write_reg(dev, R27_RF1 + 2, freq_khz & 0xff);

    /* 5. Clear IRQ & trigger 1st tune stage */
    tda18250_write_reg(dev, R0A_IRQ3, TDA18250_IRQ_TUNE);
    tda18250_write_reg(dev, R2A_MSM1, 0x01);
    tda18250_write_reg(dev, R2B_MSM2, 0x01);
    tda18250_wait_for_irq(dev, TDA18250_IRQ_TUNE, 500);

    /* 6. Dynamic PLL calculation from calibrated R34_MD1 */
    uint8_t uval = tda18250_read_reg(dev, R34_MD1);
    uint32_t exp = (uval & 0x70) >> 4;
    if (exp > 5) exp = 0;
    uint32_t lopd = 1 << (exp - 1);
    uint32_t scale = uval & 0x0f;
    uint32_t fvco = lopd * scale * (freq_khz + if_khz);

    uint8_t rdiv, ndiv, icp;
    if (fvco < 6643000) {
        rdiv = 2; ndiv = 0; icp = 0x05;
    } else if (fvco < 6811000) {
        rdiv = 2; ndiv = 0; icp = 0x06;
    } else {
        rdiv = 3; ndiv = 1; icp = 0x02;
    }

    tda18250_write_bits(dev, R4F_XTALFLX3, 0xe0, (rdiv << 6) | (ndiv << 5));

    /* 7. Clear IRQ & trigger 2nd tune stage */
    tda18250_write_reg(dev, R0A_IRQ3, TDA18250_IRQ_TUNE);
    tda18250_write_bits(dev, R46_CPUMP, 0x07, 0x00);
    tda18250_write_bits(dev, R39_SD5, 0x03, 0x00);
    tda18250_write_reg(dev, R2A_MSM1, 0x01);
    tda18250_write_reg(dev, R2B_MSM2, 0x01);
    tda18250_wait_for_irq(dev, TDA18250_IRQ_TUNE, 500);

    /* 8. Post-tune settling and restore AGC */
    Sleep(20);
    tda18250_write_bits(dev, R2B_MSM2, 0x04, 0x04);
    Sleep(20);
    tda18250_write_bits(dev, R1A_AGCK, 0x03, 0x03);
    tda18250_write_bits(dev, R14_AGC23, 0x40, 0x40);
    tda18250_write_bits(dev, R46_CPUMP, 0x07, icp);

    return true;
}
