#ifndef UNIFIED_DEVICE_H
#define UNIFIED_DEVICE_H

#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <stdint.h>
#include <stdbool.h>

/* USB VIDs and PIDs */
#define XBOX_TUNER_VID     0x045E
#define XBOX_TUNER_PID     0x02D5

#define ASTROMETA_VID      0x15F4
#define ASTROMETA_PID      0x0131

#define DIB0700_FIRMWARE_NAME "dvb-usb-dib0700-1.20.fw"
#define MN88472_FIRMWARE_NAME "dvb-demod-mn88472-02.fw"

/* RTL2832 Command Opcodes */
#define CMD_SYS_RD       0x0200
#define CMD_SYS_WR       0x0210
#define CMD_I2C_RD       0x0300
#define CMD_I2C_WR       0x0310
#define CMD_I2C_DA_RD    0x0600
#define CMD_I2C_DA_WR    0x0610

/* RTL2832 System Registers */
#define SYS_GPIO_OUT_VAL 0x3001
#define SYS_GPIO_IN_VAL  0x3002
#define SYS_GPIO_OUT_EN  0x3003
#define SYS_GPIO_DIR     0x3004

/* I2C Addresses */
#define I2C_ADDR_MN88473_BANK0 0x18
#define I2C_ADDR_MN88473_BANK1 0x1a
#define I2C_ADDR_MN88473_BANK2 0x1c
#define I2C_ADDR_R820T         0x3a

typedef enum {
    DEVICE_MODEL_NONE = 0,
    DEVICE_MODEL_AUTO = 0,
    DEVICE_MODEL_XBOX_ONE = 1,
    DEVICE_MODEL_ASTROMETA = 2
} device_model_t;

typedef struct unified_device {
    device_model_t model;
    HANDLE hDev;
    WINUSB_INTERFACE_HANDLE hWinUsb;
    UCHAR ep_in;
    ULONG max_packet_size;
    bool is_streaming;
    bool is_initialized;
    HANDLE hStreamThread;
    HANDLE hStopEvent;
    CRITICAL_SECTION lock;

    /* Signal stats */
    int signal_strength;
    int signal_quality;
    bool has_lock;
} unified_device_t;

typedef struct unified_device xbox_device_t;
typedef struct unified_device astrometa_device_t;

extern unified_device_t g_unified_dev;
extern char g_module_dir[MAX_PATH];
extern HINSTANCE g_hModule;

void log_dbg(const char *fmt, ...);

/* Unified USB Layer */
bool unified_usb_open(unified_device_t *dev, device_model_t target_model);
void unified_usb_close(unified_device_t *dev);
int  xbox_usb_control_msg(unified_device_t *dev, uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index, void *data, uint16_t size, int timeout);

/* RTL2832 Bridge prototypes */
bool rtl2832_ctrl(unified_device_t *dev, uint16_t value, uint16_t index, void *buf, uint16_t len, bool write);
bool rtl2832_wr_reg_mask(unified_device_t *dev, uint16_t reg, uint8_t val, uint8_t mask);
bool rtl2832_rd_reg(unified_device_t *dev, uint16_t reg, uint8_t *val);
bool rtl2832_wr_reg(unified_device_t *dev, uint16_t reg, uint8_t val);
bool rtl2832_i2c_read(unified_device_t *dev, uint8_t i2c_addr, uint8_t reg, uint8_t *buf, uint16_t len);
bool rtl2832_i2c_write(unified_device_t *dev, uint8_t i2c_addr, uint8_t reg, const uint8_t *buf, uint16_t len);
bool rtl2832_init(unified_device_t *dev);
bool rtl2832_open_gate(unified_device_t *dev);
bool rtl2832_close_gate(unified_device_t *dev);
bool rtl2832_set_streaming(unified_device_t *dev, bool enable);

/* DiB0700 Bridge prototypes */
bool dib0700_init(unified_device_t *dev);
int  dib0700_ctrl_wr(unified_device_t *dev, uint8_t *tx, uint8_t txlen);
int  dib0700_ctrl_rd(unified_device_t *dev, uint8_t *tx, uint8_t txlen, uint8_t *rx, uint8_t rxlen);
int  dib0700_i2c_write(unified_device_t *dev, uint8_t addr, const uint8_t *buf, int len);
int  dib0700_i2c_read(unified_device_t *dev, uint8_t addr, uint8_t reg, uint8_t *buf, int len);
bool dib0700_set_gpio(unified_device_t *dev, uint8_t gpio, uint8_t dir, uint8_t val);
int  dib0700_set_streaming(unified_device_t *dev, int onoff);

/* Demodulator & Tuner prototypes */
bool mn88472_init_device(unified_device_t *dev);
bool mn88472_tune_dvbc(unified_device_t *dev, uint32_t freq_hz, uint32_t symbol_rate_ks, int qam_mode);
bool mn88472_read_status(unified_device_t *dev, int *strength, int *snr, bool *locked);

bool tda18250_init(unified_device_t *dev);
bool tda18250_tune_dvbc(unified_device_t *dev, uint32_t freq_hz);

bool mn88473_init(unified_device_t *dev);
bool mn88473_tune_dvbc(unified_device_t *dev, uint32_t freq_khz, uint32_t sym_rate_ks, int qam_mode);
bool mn88473_get_status(unified_device_t *dev, bool *locked, int *strength, int *snr);

bool r820t_device_init(unified_device_t *dev);
bool r820t_device_set_freq(unified_device_t *dev, uint32_t freq_hz);

#endif /* UNIFIED_DEVICE_H */
