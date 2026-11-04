#include "xbox_device.h"
#include <stdio.h>
#include <math.h>

#define MN88472_ADDR_B0 0x18
#define MN88472_ADDR_B1 0x1A
#define MN88472_ADDR_B2 0x1C

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
    char fw_path[MAX_PATH];
    FILE *fp = NULL;

    if (g_module_dir[0] != '\0') {
        snprintf(fw_path, sizeof(fw_path), "%s%s", g_module_dir, MN88472_FIRMWARE_NAME);
        fp = fopen(fw_path, "rb");
    }
    if (!fp) {
        snprintf(fw_path, sizeof(fw_path), "Devices/%s", MN88472_FIRMWARE_NAME);
        fp = fopen(fw_path, "rb");
    }
    if (!fp) {
        snprintf(fw_path, sizeof(fw_path), "%s", MN88472_FIRMWARE_NAME);
        fp = fopen(fw_path, "rb");
    }
    if (!fp) {
        return false;
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
    uint8_t st = 0;
    int r = dib0700_i2c_read(dev, MN88472_ADDR_B0, 0xf5, &st, 1);
    if (r == 0 && !(st & 0x01)) {
        free(fw_data);
        return true;
    }

    /* Put demod in firmware download mode */
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

    /* Finalize firmware download: transition CPU from download mode to running */
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xf5, 0x00);
    Sleep(50);

    /* Verify demod status */
    st = 0;
    dib0700_i2c_read(dev, MN88472_ADDR_B0, 0xf5, &st, 1);
    return !(st & 0x01);
}

bool mn88472_init_device(xbox_device_t *dev) {
    /* 1. Power up sequence */
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x05, 0x00);
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x0b, 0x00);
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x0c, 0x00);

    /* 2. Download Demod Firmware */
    if (!mn88472_load_firmware(dev)) {
        return false;
    }

    /* 3. Configure Parallel TS Mode & Variable TS Clock (Our QAM-256 Fix!) */
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x08, 0x00); /* Bank 2, Reg 0x08: Parallel TS Mode */
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xd9, 0xe3); /* Bank 0, Reg 0xD9: Variable TS Clock */

    /* 4. Initialize TDA18250 Tuner */
    if (!tda18250_init(dev)) {
        return false;
    }

    return true;
}

/* Tune DVB-C (Annex A, DIGI Romania 8MHz / QAM-256) */
bool mn88472_tune_dvbc(xbox_device_t *dev, uint32_t freq_hz, uint32_t symbol_rate_ks, int qam_mode) {
    (void)symbol_rate_ks;
    (void)qam_mode;

    /* 1. Tune TDA18250 Silicon Tuner */
    tda18250_tune_dvbc(dev, freq_hz);

    /* 2. Ensure TS mode & variable clock settings */
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x08, 0x00);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xd9, 0xe3);

    /* 3. Set Delivery System to DVB-C Annex A (0x04) */
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x00, 0x66);
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x01, 0x00);
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x02, 0x01);
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x03, 0x04);
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x04, 0x00);

    /* 4. Set Intermediate Frequency (IF 4950 kHz = 0x3DCDB0) */
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x10, 0x3d);
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x11, 0xcd);
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0x12, 0xb0);

    /* 5. Set DVB-C Bank 0 Equalizer registers & high-bitrate bandwidth/TS timing */
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0x46, 0x00);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xae, 0x00);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xb0, 0x00);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xb4, 0x00);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xcd, 0x17);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xd4, 0x09);
    mn88472_write_reg(dev, MN88472_ADDR_B0, 0xd6, 0x46); /* High-bitrate timing (0x46) */

    /* 6. Reset Demodulator FSM to start locking */
    mn88472_write_reg(dev, MN88472_ADDR_B2, 0xf8, 0x9f);

    return true;
}

/* Fast Atomic Status & Lock & SNR Reading */
bool mn88472_read_status(xbox_device_t *dev, int *strength, int *snr, bool *locked) {
    /* Read Lock Status (Bank 1, Reg 0x84) */
    uint8_t lock_st = mn88472_read_reg(dev, MN88472_ADDR_B1, 0x84);
    *locked = ((lock_st & 0x0f) >= 0x08);

    /* Signal Strength (Bank 2, Reg 0x8e..0x8f) */
    uint8_t str_b[2] = {0};
    dib0700_i2c_read(dev, MN88472_ADDR_B2, 0x8e, &str_b[0], 1);
    dib0700_i2c_read(dev, MN88472_ADDR_B2, 0x8f, &str_b[1], 1);
    uint32_t raw_str = (str_b[0] << 8) | str_b[1];
    *strength = (raw_str > 60000) ? 100 : (raw_str * 100 / 60000);

    if (!(*locked)) {
        *snr = 0;
        return true;
    }

    /* CNR / SNR (Bank 1, Reg 0xa1..0xa4) */
    uint8_t cnr_b[4] = {0};
    dib0700_i2c_read(dev, MN88472_ADDR_B1, 0xa1, cnr_b, 4);
    uint32_t sig = (cnr_b[0] << 8) | cnr_b[1];
    uint32_t noi = (cnr_b[2] << 8) | cnr_b[3];

    if (noi > 0 && sig > 0) {
        int snr_db = (int)(10.0 * 0.43429 * (log((double)sig / (double)noi) + 2.079));
        if (snr_db < 0) snr_db = 0;
        if (snr_db > 40) snr_db = 40;
        *snr = snr_db * 100 / 36;
    } else {
        *snr = 85;
    }

    return true;
}
