#ifndef XBOX_DEVICE_H
#define XBOX_DEVICE_H

#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <stdint.h>
#include <stdbool.h>

#define XBOX_TUNER_VID 0x045E
#define XBOX_TUNER_PID 0x02D5

#define DIB0700_FIRMWARE_NAME "dvb-usb-dib0700-1.20.fw"
#define MN88472_FIRMWARE_NAME "dvb-demod-mn88472-02.fw"

typedef struct {
    HANDLE hDev;
    WINUSB_INTERFACE_HANDLE hWinUsb;
    uint8_t ep_in;
    uint32_t max_packet_size;
    bool is_initialized;
    bool is_streaming;
    HANDLE hStreamThread;
    HANDLE hStopEvent;
    
    /* Stats */
    int signal_strength;
    int signal_quality;
    bool has_lock;
} xbox_device_t;

extern xbox_device_t g_xbox_dev;

/* WinUSB functions */
bool xbox_usb_open(xbox_device_t *dev);
void xbox_usb_close(xbox_device_t *dev);
int  xbox_usb_control_msg(xbox_device_t *dev, uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index, void *data, uint16_t size, int timeout);

/* DiB0700 bridge functions */
bool dib0700_init(xbox_device_t *dev);
int  dib0700_ctrl_wr(xbox_device_t *dev, uint8_t *tx, uint8_t txlen);
int  dib0700_ctrl_rd(xbox_device_t *dev, uint8_t *tx, uint8_t txlen, uint8_t *rx, uint8_t rxlen);
int  dib0700_i2c_write(xbox_device_t *dev, uint8_t addr, const uint8_t *buf, int len);
int  dib0700_i2c_read(xbox_device_t *dev, uint8_t addr, uint8_t reg, uint8_t *buf, int len);
bool dib0700_set_gpio(xbox_device_t *dev, uint8_t gpio, uint8_t dir, uint8_t val);

/* Demodulator & Tuner functions */
bool mn88472_init_device(xbox_device_t *dev);
bool mn88472_tune_dvbc(xbox_device_t *dev, uint32_t freq_hz, uint32_t symbol_rate_ks, int qam_mode);
bool mn88472_read_status(xbox_device_t *dev, int *strength, int *snr, bool *locked);

#endif /* XBOX_DEVICE_H */
