#include "astrometa_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint64_t v4l2_std_id;

#define pr_fmt(fmt) fmt
#define module_param(a, b, c)
#define MODULE_PARM_DESC(a, b)
#define tuner_dbg(fmt, ...)
#define tuner_info(fmt, ...)
#define tuner_warn(fmt, ...)
#define tuner_err(fmt, ...)
#define pr_info(fmt, ...)

#define usleep_range(min, max) Sleep(((min) + 999) / 1000)
#define msleep(ms) Sleep(ms)
#define kzalloc(sz, flags) calloc(1, sz)
#define kfree(p) free(p)
#define GFP_KERNEL 0

void log_dbg(const char *fmt, ...);
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define EREMOTEIO 121
#define EINVAL 22
#define ENOMEM 12
#define ENODEV 19
#define MHz 1000000

#define swap(a, b) do { __typeof__(a) _tmp = (a); (a) = (b); (b) = _tmp; } while (0)

#define EXPORT_SYMBOL_GPL(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_AUTHOR(x)
#define MODULE_LICENSE(x)

enum v4l2_tuner_type {
    V4L2_TUNER_RADIO = 1,
    V4L2_TUNER_ANALOG_TV = 2,
    V4L2_TUNER_DIGITAL_TV = 3,
    V4L2_TUNER_SDR = 4,
    V4L2_TUNER_RF = 5,
};

#define V4L2_STD_SECAM_LC 0
#define V4L2_STD_MN 0
#define V4L2_STD_PAL_M 0
#define V4L2_STD_NTSC 0
#define DEFINE_MUTEX(name) struct mutex name = {0}
#define mutex_lock(m)
#define mutex_unlock(m)
#define mutex_init(m)
struct mutex { int dummy; };
struct list_head { struct list_head *next, *prev; };

enum fe_delivery_system {
    SYS_UNDEFINED,
    SYS_DVBC_ANNEX_A,
    SYS_DVBC_ANNEX_B,
    SYS_DVBT,
    SYS_DSS,
    SYS_DVBS,
    SYS_DVBS2,
    SYS_DVBH,
    SYS_ISDBT,
    SYS_ISDBS,
    SYS_ISDBC,
    SYS_ATSC,
    SYS_ATSCMH,
    SYS_DTMB,
    SYS_CMMB,
    SYS_DAB,
    SYS_DVBT2,
    SYS_TURBO,
    SYS_DVBC_ANNEX_C,
};

static uint8_t bitrev8(uint8_t b) {
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

static astrometa_device_t *g_current_dev = NULL;

struct tuner_i2c_props { u8 addr; void *adap; };

static inline int tuner_i2c_xfer_send(struct tuner_i2c_props *props, char *buf, int len) {
    uint16_t val = ((u8)buf[0] << 8) | (props->addr << 1);
    for (int retry = 0; retry < 10; retry++) {
        rtl2832_open_gate(g_current_dev);
        if (rtl2832_ctrl(g_current_dev, val, CMD_I2C_WR, &buf[1], (uint16_t)(len - 1), true)) {
            return len;
        }
        Sleep(5);
    }
    return -EREMOTEIO;
}

static inline int tuner_i2c_xfer_send_recv(struct tuner_i2c_props *props,
                                          char *obuf, int olen, char *ibuf, int ilen) {
    uint16_t val = ((u8)obuf[0] << 8) | (props->addr << 1);
    for (int retry = 0; retry < 10; retry++) {
        rtl2832_open_gate(g_current_dev);
        if (rtl2832_ctrl(g_current_dev, val, CMD_I2C_RD, ibuf, (uint16_t)ilen, false)) {
            return ilen;
        }
        Sleep(5);
    }
    return -EREMOTEIO;
}

struct dvb_frontend;
struct analog_parameters { v4l2_std_id std; u32 frequency; };

struct dvb_tuner_info {
    char name[32];
    u32 frequency_min_hz;
    u32 frequency_max_hz;
    u32 frequency_step_hz;
    u32 bandwidth_min;
    u32 bandwidth_max;
    u32 bandwidth_step;
};

struct dvb_tuner_ops {
    struct dvb_tuner_info info;
    void (*release)(struct dvb_frontend *fe);
    int (*init)(struct dvb_frontend *fe);
    int (*sleep)(struct dvb_frontend *fe);
    int (*set_params)(struct dvb_frontend *fe);
    int (*set_analog_params)(struct dvb_frontend *fe, struct analog_parameters *p);
    int (*get_frequency)(struct dvb_frontend *fe, u32 *frequency);
    int (*get_bandwidth)(struct dvb_frontend *fe, u32 *bandwidth);
    int (*get_if_frequency)(struct dvb_frontend *fe, u32 *frequency);
    int (*get_rf_strength)(struct dvb_frontend *fe, u16 *strength);
};

struct dtv_frontend_properties {
    u32 frequency;
    u32 symbol_rate;
    u32 bandwidth_hz;
    u32 delivery_system;
};

struct dvb_frontend_ops {
    struct dvb_tuner_ops tuner_ops;
    int (*i2c_gate_ctrl)(struct dvb_frontend *fe, int enable);
    int (*read_signal_strength)(struct dvb_frontend *fe, u16 *strength);
};

struct dvb_frontend {
    struct dvb_frontend_ops ops;
    struct dtv_frontend_properties dtv_property_cache;
    void *tuner_priv;
};

#define hybrid_tuner_request_state(TYPE, STATE, LIST, ADAP, ADDR, NAME) \
    ({ (STATE) = (TYPE *)kzalloc(sizeof(TYPE), GFP_KERNEL); \
       (STATE)->i2c_props.addr = (ADDR); \
       (STATE)->i2c_props.adap = (ADAP); \
       1; })
#define hybrid_tuner_release_state(STATE) do { if (STATE) kfree(STATE); } while (0)

static int dummy_i2c_gate_ctrl(struct dvb_frontend *fe, int enable) {
    (void)fe;
    if (enable && g_current_dev) rtl2832_open_gate(g_current_dev);
    return 0;
}

struct i2c_adapter { int dummy; };

#define IS_REACHABLE(x) 1
#define CONFIG_MEDIA_TUNER_R820T 1
#include "r820t.h"

#define __func__ "r820t"
#include "r820t.c"

static struct dvb_frontend g_fe = {0};
static bool g_r820t_attached = false;
static const struct r820t_config g_r820t_cfg = {
    .i2c_addr = 0x3a,
    .xtal = 16000000,
    .rafael_chip = CHIP_R828D,
    .max_i2c_msg_len = 2,
    .use_diplexer = false,
    .use_predetect = false
};

bool r820t_device_init(astrometa_device_t *dev) {
    g_current_dev = dev;
    no_imr_cal = 1;

    if (!g_r820t_attached) {
        memset(&g_fe, 0, sizeof(g_fe));
        g_fe.ops.i2c_gate_ctrl = dummy_i2c_gate_ctrl;

        rtl2832_open_gate(dev);
        struct dvb_frontend *ret_fe = r820t_attach(&g_fe, NULL, &g_r820t_cfg);
        rtl2832_close_gate(dev);
        if (!ret_fe) return false;
        g_r820t_attached = true;
    }

    if (g_fe.ops.tuner_ops.init) {
        rtl2832_open_gate(dev);
        int rc = g_fe.ops.tuner_ops.init(&g_fe);
        rtl2832_close_gate(dev);
        if (rc < 0) return false;
    }

    return true;
}

bool r820t_device_set_freq(astrometa_device_t *dev, uint32_t freq_hz) {
    g_current_dev = dev;
    rtl2832_open_gate(dev);
    if (!g_r820t_attached) {
        if (!r820t_device_init(dev)) return false;
    }

    g_fe.dtv_property_cache.frequency = freq_hz;
    g_fe.dtv_property_cache.bandwidth_hz = 8000000;
    g_fe.dtv_property_cache.delivery_system = SYS_DVBC_ANNEX_A;

    if (g_fe.ops.tuner_ops.set_params) {
        int rc = g_fe.ops.tuner_ops.set_params(&g_fe);
        log_dbg("r820t_device_set_freq: set_params ret=%d\n", rc);
        rtl2832_close_gate(dev);
        return (rc >= 0);
    }
    rtl2832_close_gate(dev);
    return false;
}
