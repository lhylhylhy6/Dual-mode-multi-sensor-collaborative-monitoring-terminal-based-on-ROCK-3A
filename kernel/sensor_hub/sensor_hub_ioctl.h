#ifndef SENSOR_HUB_IOCTL_H
#define SENSOR_HUB_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * sensor_hub shared uapi header
 *
 * Design goals:
 * 1) One device node: /dev/sensor_hub
 * 2) Unified event stream for trigger mode
 * 3) Unified snapshot/status for monitor mode
 * 4) Easy extension for future sensors
 *
 * Notes:
 * - Do NOT use float in kernel/user shared structures.
 * - Environmental values use scaled integers:
 *   temperature: milli-degree Celsius (mC)
 *   humidity   : milli-percent RH (m%RH)
 *
 * Example:
 *   23.74 C   -> 23740
 *   61.67 %RH -> 61670
 */

/* =========================
 * API version / basic limits
 * ========================= */
#define SH_API_VERSION            0x00010000U

#define SH_MAX_VALUES             4
#define SH_MAX_SENSORS            16
#define SH_NAME_LEN               32
#define SH_RESERVED_WORDS         4

/* =========================
 * Sensor IDs
 * ========================= */
#define SH_SENSOR_NONE            0U
#define SH_SENSOR_PIR             1U
#define SH_SENSOR_SHT20           2U

/* Reserved future examples */
#define SH_SENSOR_LIGHT           3U
#define SH_SENSOR_SMOKE           4U
#define SH_SENSOR_DOOR            5U

/* Custom sensor ID range */
#define SH_SENSOR_CUSTOM_BASE     1000U

/* =========================
 * Sensor types
 * ========================= */
#define SH_TYPE_UNKNOWN           0U
#define SH_TYPE_BINARY            1U   /* 0/1 state, e.g. PIR, door switch */
#define SH_TYPE_ENV               2U   /* environmental, e.g. temp/humidity */
#define SH_TYPE_ANALOG            3U
#define SH_TYPE_COUNTER           4U
#define SH_TYPE_CUSTOM            100U

/* =========================
 * Common flags
 * ========================= */
#define SH_FLAG_ENABLED           (1U << 0)
#define SH_FLAG_VALID             (1U << 1)
#define SH_FLAG_ONLINE            (1U << 2)
#define SH_FLAG_ALARM_HI          (1U << 3)
#define SH_FLAG_ALARM_LO          (1U << 4)
#define SH_FLAG_ERROR             (1U << 5)

/* =========================
 * Event types
 * ========================= */
#define SH_EVT_NONE               0U
#define SH_EVT_SENSOR             1U   /* normal sensor event */
#define SH_EVT_CONFIG             2U   /* config changed */
#define SH_EVT_ERROR              3U   /* driver/sensor error */
#define SH_EVT_OVERFLOW           4U   /* event queue overflow */

/* =========================
 * Event codes
 * ========================= */

/* Generic / common codes */
#define SH_CODE_NONE              0U
#define SH_CODE_SAMPLE            1U   /* sample/update finished */
#define SH_CODE_TRIGGER           2U   /* trigger action */
#define SH_CODE_RISING            3U   /* rising edge / active */
#define SH_CODE_FALLING           4U   /* falling edge / inactive */
#define SH_CODE_ENABLE            5U
#define SH_CODE_DISABLE           6U
#define SH_CODE_REFRESH           7U   /* force refresh */
#define SH_CODE_ERROR             8U
#define SH_CODE_QUEUE_DROPPED     9U

/* Sensor-specific semantics:
 *
 * PIR:
 *   values[0] = current level (0/1)
 *
 * SHT20:
 *   values[0] = temperature in mC
 *   values[1] = humidity in m%RH
 */

/* =========================
 * Snapshot / value model
 * ========================= */

/*
 * Latest value of one sensor.
 * Used inside sh_snapshot.items[].
 */
struct sh_sensor_value {
    __u32 id;                           /* SH_SENSOR_* */
    __u32 type;                         /* SH_TYPE_* */
    __u32 flags;                        /* SH_FLAG_* */
    __u32 nvalues;                      /* number of valid entries in values[] */

    __s64 timestamp_ns;                 /* last update time */

    __s32 values[SH_MAX_VALUES];        /* scaled integer values */
    __u32 reserved[SH_RESERVED_WORDS];
};

/*
 * Full latest-state snapshot for all registered sensors.
 * Used by monitor mode.
 */
struct sh_snapshot {
    __u32 version;                      /* SH_API_VERSION */
    __u32 count;                        /* valid items in items[] */

    __u64 seq;                          /* snapshot sequence */
    __u64 dropped_events;               /* total dropped events in queue */
    __s64 timestamp_ns;                 /* snapshot generation time */

    struct sh_sensor_value items[SH_MAX_SENSORS];
};

/* =========================
 * Event model
 * ========================= */

/*
 * One event record returned by read().
 * read() may return 1 or more whole sh_event records.
 */
struct sh_event {
    __u64 seq;                          /* event sequence number */

    __u32 type;                         /* SH_EVT_* */
    __u32 sensor_id;                    /* SH_SENSOR_* */

    __u32 code;                         /* SH_CODE_* */
    __u32 flags;                        /* SH_FLAG_* or event-specific flags */

    __u32 nvalues;                      /* number of valid values[] entries */
    __u32 reserved0;

    __s64 timestamp_ns;                 /* event time */
    __s32 values[SH_MAX_VALUES];

    __u32 reserved[SH_RESERVED_WORDS];
};

/* =========================
 * Configuration model
 * ========================= */

/*
 * Unified per-sensor config.
 *
 * Different sensors use different fields:
 *
 * PIR:
 *   enabled
 *   debounce_ms
 *
 * SHT20:
 *   enabled
 *   period_ms
 *   thresh_hi[0] = temp_hi_mC     (optional)
 *   thresh_lo[0] = temp_lo_mC     (optional)
 *   thresh_hi[1] = humi_hi_mRH    (optional)
 *   thresh_lo[1] = humi_lo_mRH    (optional)
 */
struct sh_sensor_cfg {
    __u32 id;                           /* SH_SENSOR_* */
    __u32 enabled;                      /* 0/1 */

    __u32 period_ms;                    /* polling period for periodic sensors */
    __u32 debounce_ms;                  /* debounce for edge sensors */

    __s32 thresh_hi[SH_MAX_VALUES];     /* high thresholds */
    __s32 thresh_lo[SH_MAX_VALUES];     /* low thresholds */

    __u32 reserved[SH_RESERVED_WORDS];
};

/*
 * Used by FORCE_REFRESH.
 * timeout_ms can be 0 to use driver default.
 */
struct sh_refresh_req {
    __u32 id;                           /* SH_SENSOR_* */
    __u32 timeout_ms;                   /* 0 = default */
    __u32 reserved[SH_RESERVED_WORDS];
};

/*
 * Basic driver metadata.
 */
struct sh_hub_info {
    __u32 version;                      /* SH_API_VERSION */
    __u32 sensor_count;                 /* currently registered sensors */
    __u32 queue_size;                   /* event queue capacity */
    __u32 queue_depth;                  /* current queued events */

    __u64 event_seq;                    /* latest event seq */
    __u64 snapshot_seq;                 /* latest snapshot seq */

    __u32 reserved[SH_RESERVED_WORDS];
};

/* =========================
 * read()/poll() semantics
 * ========================= */

/*
 * read(fd, buf, size):
 *   - returns one or more whole struct sh_event objects
 *   - size should be multiple of sizeof(struct sh_event)
 *   - poll() readable when at least one event exists
 *
 * monitor mode:
 *   - use ioctl(SH_IOC_GET_SNAPSHOT)
 *
 * trigger mode:
 *   - use poll()/read() for PIR events
 *   - optionally ioctl(SH_IOC_FORCE_REFRESH, SHT20)
 *   - then ioctl(SH_IOC_GET_SNAPSHOT)
 */

/* =========================
 * ioctl definitions
 * ========================= */

#define SH_IOC_MAGIC              'S'

/* Get driver metadata */
#define SH_IOC_GET_INFO           _IOR(SH_IOC_MAGIC, 0x01, struct sh_hub_info)

/* Get current all-sensor snapshot */
#define SH_IOC_GET_SNAPSHOT       _IOR(SH_IOC_MAGIC, 0x02, struct sh_snapshot)

/* Clear pending event queue */
#define SH_IOC_CLR_EVENTS         _IO(SH_IOC_MAGIC,  0x03)

/* Force one sensor refresh, e.g. SHT20 sample-now */
#define SH_IOC_FORCE_REFRESH      _IOW(SH_IOC_MAGIC, 0x04, struct sh_refresh_req)

/* Get one sensor config */
#define SH_IOC_GET_SENSOR_CFG     _IOWR(SH_IOC_MAGIC, 0x05, struct sh_sensor_cfg)

/* Set one sensor config */
#define SH_IOC_SET_SENSOR_CFG     _IOW(SH_IOC_MAGIC, 0x06, struct sh_sensor_cfg)

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_HUB_IOCTL_H */