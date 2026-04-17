#ifndef SENSOR_HUB_H
#define SENSOR_HUB_H

#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/property.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "sensor_hub_ioctl.h"

#define SH_EVT_Q_SIZE 64

struct sh_core;
struct sh_endpoint;

struct sh_endpoint_ops {
	int (*refresh)(struct sh_core *hub, struct sh_endpoint *endpoint);
	int (*apply_cfg)(struct sh_core *hub,
			 struct sh_endpoint *endpoint,
			 const struct sh_sensor_cfg *cfg);
	int (*dispatch)(struct sh_core *hub,
			struct sh_endpoint *endpoint,
			const struct sh_action_req *req);
	void (*remove)(struct sh_core *hub, struct sh_endpoint *endpoint);
};

struct sh_endpoint {
	__u32 id;
	__u32 type;
	__u32 direction;
	__u32 caps;
	char name[SH_NAME_LEN];

	struct sh_sensor_value value;
	struct sh_sensor_cfg cfg;

	const struct sh_endpoint_ops *ops;
	void *priv;
	bool registered;
};

struct sh_event_queue {
	struct sh_event events[SH_EVT_Q_SIZE];
	u32 head;
	u32 tail;
	u32 count;
	u64 dropped;

	spinlock_t lock;
	wait_queue_head_t wq;
};

struct sh_core {
	struct device *dev;
	struct miscdevice miscdev;
	struct mutex lock; /* sensor registry / config / snapshot */

	struct sh_event_queue q;

	struct sh_endpoint endpoints[SH_MAX_SENSORS];
	u32 endpoint_count;

	u64 event_seq;
	u64 snapshot_seq;
};

/* core helpers */
struct sh_endpoint *sh_find_endpoint(struct sh_core *hub, u32 id);
struct sh_endpoint *sh_register_endpoint(struct sh_core *hub,
					 const struct sh_endpoint *tmpl);
void sh_unregister_endpoint(struct sh_core *hub, struct sh_endpoint *endpoint);
int sh_push_event(struct sh_core *hub, struct sh_event *evt);
void sh_init_value(struct sh_endpoint *endpoint,
		   struct sh_sensor_value *val,
		   u32 flags,
		   u32 nvalues);
void sh_emit_event(struct sh_core *hub,
		   struct sh_endpoint *endpoint,
		   u32 evt_type,
		   u32 code,
		   u32 evt_flags,
		   const struct sh_sensor_value *val);
void sh_update_endpoint_value(struct sh_core *hub,
			      struct sh_endpoint *endpoint,
			    const struct sh_sensor_value *val);
void sh_fill_snapshot(struct sh_core *hub, struct sh_snapshot *snap);

/* submodule entrypoints */
int sh_pir_register(struct sh_core *hub);
void sh_pir_unregister(struct sh_core *hub);

int sh_sht20_register(struct sh_core *hub);
void sh_sht20_unregister(struct sh_core *hub);

int sh_buzzer_register(struct sh_core *hub);
void sh_buzzer_unregister(struct sh_core *hub);

#endif
