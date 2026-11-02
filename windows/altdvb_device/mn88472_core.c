#include "xbox_device.h"
#include <stdio.h>
#include <math.h>

#define MN88472_ADDR_B0 0x18
#define MN88472_ADDR_B1 0x1A
#define MN88472_ADDR_B2 0x1C
#define TDA18250_ADDR   0x60

static bool mn88472_write_reg(xbox_device_t *dev, uint8_t bank_addr, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return (dib0700_i2c_write(dev, bank_addr, buf, 2) == 0);
}

static uint8_t mn88472_read_reg(xbox_device_t *dev, uint8_t bank_addr, uint8_t reg) {
    uint8_t val = 0;
    dib0700_i2c_read(dev, bank_addr, reg, &val, 1);
    return val;
}

static bool mn88472_load_firmware(xbox_device_t *dev) {
    FILE *fp = fopen(MN88472_FIRMWARE_NAME, "rb");
    if (!fp) {
        /* Try relative path or subfolder */
        fp = fopen("Devices/" MN88472_FIRMWARE_NAME, "rb");
        if (!fp) return false;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *fw_data = (uint8_t *)malloc(size);
    if (!fw_data) {
        fclose(fp);
        return false;
    }

    fread(fw_data, 1, size, fp);
    fclose(fp);

    /* Check if already warm */
    uint8_t st = mn88472_read_reg(dev, MN88472_ADDR_B0, 0xf5);
    if (!(st & 0x01)) {
        free(fw_data);
        return true; /* Already running */
    }

    /* Start firmware download */
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xf5, 0x03);

    int rem = size;
    int offset = 0;
    while (rem > 0) {
        int len = (rem > 21) ? 21 : rem;
        uint8_t chunk[32];
        chunk[0] = 0xf6;
        memcpy(&chunk[1], &fw_data[offset], len);
        dib0700_i2c_write(dev, MN88472_ADDR_B0, chunk, len + 1);
        offset += len;
        rem -= len;
    }

    free(fw_data);
    Sleep(50);
    return true;
}

bool mn88472_init_device(xbox_device_t *dev) {
    /* 1. Power up sequences */
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x05, 0x00);
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x0b, 0x00);
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x0c, 0x00);

    /* 2. Firmware Download */
    if (!mn88472_load_firmware(dev)) {
        return false;
    }

    /* 3. Common Demod Initialization */
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0x04, 0x3e);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0x05, 0xae);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0x0b, 0x30);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0x0c, 0x43);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0x0d, 0xbc);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0x0e, 0x5c);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0x0f, 0x00);

    /* 4. TS Pacing: VARIABLE_TS_CLOCK & Parallel Mode (Our QAM-256 Fix!) */
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xd9, 0xe1); /* Parallel TS */
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xda, 0x01); /* Variable TS Clock */

    return true;
}

/* NXP TDA18250 Tuning Routine */
static bool tda18250_tune(xbox_device_t *dev, uint32_t freq_hz) {
    uint8_t buf[16];
    uint32_t f_lo = freq_hz + 5000000; /* 5 MHz IF */

    buf[0] = 0x00;
    buf[1] = 0x01;
    dib0700_i2c_write(dev, TDA18250_ADDR, buf, 2);

    /* Calculate Divider for TDA18250 */
    uint32_t n = f_lo / 1000;
    buf[0] = 0x20;
    buf[1] = (n >> 16) & 0x07;
    buf[2] = (n >> 8) & 0xff;
    buf[3] = n & 0xff;
    dib0700_i2c_write(dev, TDA18250_ADDR, buf, 4);

    Sleep(20);
    return true;
}

/* Tune DVB-C (Annex A, DIGI / Cable QAM-256 / QAM-64) */
bool mn88472_tune_dvbc(xbox_device_t *dev, uint32_t freq_hz, uint32_t symbol_rate_ks, int qam_mode) {
    /* 1. Tune RF front-end */
    tda18250_tune(dev, freq_hz);

    /* 2. Configure MN88472 Delivery System to DVB-C (0x04) */
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x00, 0x66);
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x01, 0x00);
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x02, 0x01);
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x03, 0x04); /* SYS_DVBC_ANNEX_A */
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x04, 0x00);

    /* Intermediate Frequency (IF = 5.0 MHz) */
    uint64_t if_val = ((uint64_t)5000000 * 0x1000000) / 20500000;
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x10, (if_val >> 16) & 0xff);
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x11, (if_val >> 8) & 0xff);
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x12, if_val & 0xff);

    /* DVB-C Equalizer and Bank 0 settings */
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xb4, 0x00);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xcd, 0x17);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xd4, 0x09);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xd6, 0x48);

    /* Symbol Rate (e.g. 6900 kS/s) */
    if (symbol_rate_ks == 0) symbol_rate_ks = 6900;
    uint32_t sr_reg = ((uint64_t)symbol_rate_ks * 1000 * 0x1000000) / 20500000;
    mn88472_write_reg(dev, MN88472_ADDR_B1, 0x01, (sr_reg >> 16) & 0xff);
    mn88472_write_reg(dev, MN88472_ADDR_B1, 0x02, (sr_reg >> 8) & 0xff);
    mn88472_write_reg(dev, MN88472_ADDR_B1, 0x03, sr_reg & 0xff);

    /* Reset Demodulator FSM */
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0xf8, 0x9f);

    return true;
}

/* Fast Atomic Status & SNR Reading */
bool mn88472_read_status(xbox_device_t *dev, int *strength, int *snr, bool *locked) {
    /* Check Lock (Bank 1, Reg 0x84) */
    uint8_t lock_st = mn88472_read_reg(dev, MN88472_ADDR_B1, 0x84);
    *locked = ((lock_st & 0x0f) >= 0x08);

    if (!(*locked)) {
        *strength = 20;
        *snr = 0;
        return true;
    }

    /* Signal Strength (Bank 2, Reg 0x8e) */
    uint8_t str_b[2] = {0};
    dib0700_i2c_read(dev, MN88472_ADDR_B2, 0x8e, &str_b[0], 1);
    dib0700_i2c_read(dev, MN88472_ADDR_B2, 0x8f, &str_b[1], 1);
    uint32_t raw_str = (str_b[0] << 8) | str_b[1];
    *strength = (raw_str > 60000) ? 100 : (raw_str * 100 / 60000);

    /* SNR / CNR (Bank 1, Reg 0xa1) */
    uint8_t cnr_b[4] = {0};
    dib0700_i2c_read(dev, MN88472_ADDR_B1, 0xa1, cnr_b, 4);
    uint32_t sig = (cnr_b[0] << 8) | cnr_b[1];
    uint32_t noi = (cnr_b[2] << 8) | cnr_b[3];

    if (noi > 0 && sig > 0) {
        /* Approximate dB from ratio */
        int snr_db = (int)(10.0 * 0.43429 * (log((double)sig / (double)noi) + 2.079));
        if (snr_db < 0) snr_db = 0;
        if (snr_db > 40) snr_db = 40;
        *snr = snr_db * 100 / 36; /* Scaled to 0-100% for AltDVB */
    } else {
        *snr = 85; /* Fallback default good lock */
    }

    return true;
}
