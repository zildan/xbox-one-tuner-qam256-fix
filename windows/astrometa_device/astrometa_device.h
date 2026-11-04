#ifndef ASTROMETA_DEVICE_H
#define ASTROMETA_DEVICE_H

#include <windows.h>
#include <winusb.h>
#include <stdint.h>
#include <stdbool.h>

/* Astrometa USB ID */
#define ASTROMETA_VID 0x15f4
#define ASTROMETA_PID 0x0131

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

typedef struct {
    HANDLE hDev;
    WINUSB_INTERFACE_HANDLE hWinUsb;
    UCHAR ep_in;
    ULONG max_packet_size;
    bool is_streaming;
    bool is_initialized;
    HANDLE hStreamThread;
    HANDLE hStopEvent;
    CRITICAL_SECTION lock;
} astrometa_device_t;

/* Low-level USB / RTL2832 prototypes */
bool astrometa_usb_open(astrometa_device_t *dev);
void astrometa_usb_close(astrometa_device_t *dev);

bool rtl2832_ctrl(astrometa_device_t *dev, uint16_t value, uint16_t index, void *buf, uint16_t len, bool write);
bool rtl2832_wr_reg_mask(astrometa_device_t *dev, uint16_t reg, uint8_t val, uint8_t mask);
bool rtl2832_rd_reg(astrometa_device_t *dev, uint16_t reg, uint8_t *val);
bool rtl2832_wr_reg(astrometa_device_t *dev, uint16_t reg, uint8_t val);

bool rtl2832_i2c_read(astrometa_device_t *dev, uint8_t i2c_addr, uint8_t reg, uint8_t *buf, uint16_t len);
bool rtl2832_i2c_write(astrometa_device_t *dev, uint8_t i2c_addr, uint8_t reg, const uint8_t *buf, uint16_t len);

bool rtl2832_init(astrometa_device_t *dev);
bool rtl2832_open_gate(astrometa_device_t *dev);
bool rtl2832_close_gate(astrometa_device_t *dev);
bool rtl2832_set_streaming(astrometa_device_t *dev, bool enable);

/* MN88473 Demodulator prototypes */
bool mn88473_init(astrometa_device_t *dev);
bool mn88473_tune_dvbc(astrometa_device_t *dev, uint32_t freq_khz, uint32_t sym_rate_ks, int qam_mode);
bool mn88473_get_status(astrometa_device_t *dev, bool *locked, int *strength, int *snr);

/* Rafael Micro R820T / R828D Tuner prototypes */
bool r820t_device_init(astrometa_device_t *dev);
bool r820t_device_set_freq(astrometa_device_t *dev, uint32_t freq_hz);

#endif /* ASTROMETA_DEVICE_H */
