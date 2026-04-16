#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "sensor_hub.h"

struct sh_pir_priv {
	struct gpio_desc *gpiod;
	int irq;
	s64 last_irq_ns;
	struct sh_sensor *sensor;
};

static irqreturn_t sh_pir_irq_thread(int irq, void *data)
{
	struct sh_core *hub = data;
	struct sh_sensor *sensor = sh_find_sensor(hub, SH_SENSOR_PIR);
	struct sh_pir_priv *priv;
	struct sh_sensor_value val;
	struct sh_event evt;
	s64 now;
	int level;
	u32 debounce_ms;

	if (!sensor)
		return IRQ_HANDLED;

	priv = sensor->priv;
	if (!priv || !sensor->cfg.enabled)
		return IRQ_HANDLED;

	now = ktime_get_ns();
	debounce_ms = sensor->cfg.debounce_ms;

	if (debounce_ms &&
	    priv->last_irq_ns &&
	    now - priv->last_irq_ns < (s64)debounce_ms * 1000000LL) {
		return IRQ_HANDLED;
	}

	level = gpiod_get_value_cansleep(priv->gpiod);
	if (level < 0)
		return IRQ_HANDLED;

	priv->last_irq_ns = now;

	memset(&val, 0, sizeof(val));
	val.id = SH_SENSOR_PIR;
	val.type = SH_TYPE_BINARY;
	val.flags = SH_FLAG_ENABLED | SH_FLAG_VALID | SH_FLAG_ONLINE;
	val.nvalues = 1;
	val.timestamp_ns = now;
	val.values[0] = level;

	sh_update_sensor_value(hub, sensor, &val);

	memset(&evt, 0, sizeof(evt));
	evt.type = SH_EVT_SENSOR;
	evt.sensor_id = SH_SENSOR_PIR;
	evt.code = level ? SH_CODE_TRIGGER : SH_CODE_FALLING;
	evt.flags = SH_FLAG_VALID;
	evt.nvalues = 1;
	evt.timestamp_ns = now;
	evt.values[0] = level;

	sh_push_event(hub, &evt);

	return IRQ_HANDLED;
}

static int sh_pir_refresh(struct sh_core *hub, struct sh_sensor *sensor)
{
	struct sh_pir_priv *priv = sensor->priv;
	struct sh_sensor_value val;
	int level;

	level = gpiod_get_value_cansleep(priv->gpiod);
	if (level < 0)
		return level;

	memset(&val, 0, sizeof(val));
	val.id = SH_SENSOR_PIR;
	val.type = SH_TYPE_BINARY;
	val.flags = SH_FLAG_ENABLED | SH_FLAG_VALID | SH_FLAG_ONLINE;
	val.nvalues = 1;
	val.timestamp_ns = ktime_get_ns();
	val.values[0] = level;

	sh_update_sensor_value(hub, sensor, &val);
	return 0;
}

static int sh_pir_apply_cfg(struct sh_core *hub,
			    struct sh_sensor *sensor,
			    const struct sh_sensor_cfg *cfg)
{
	mutex_lock(&hub->lock);
	sensor->cfg.enabled = cfg->enabled;
	sensor->cfg.debounce_ms = cfg->debounce_ms;
	mutex_unlock(&hub->lock);
	return 0;
}

static void sh_pir_remove_sensor(struct sh_core *hub, struct sh_sensor *sensor)
{
}

static const struct sh_sensor_ops sh_pir_ops = {
	.refresh   = sh_pir_refresh,
	.apply_cfg = sh_pir_apply_cfg,
	.remove    = sh_pir_remove_sensor,
};

int sh_pir_register(struct sh_core *hub)
{
	struct sh_pir_priv *priv;
	struct sh_sensor tmpl = { 0 };
	struct sh_sensor *sensor;
	struct sh_sensor_value val;
	int ret, level;

	priv = devm_kzalloc(hub->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->gpiod = devm_gpiod_get(hub->dev, "pir", GPIOD_IN);
	if (IS_ERR(priv->gpiod))
		return PTR_ERR(priv->gpiod);

	priv->irq = gpiod_to_irq(priv->gpiod);
	if (priv->irq < 0)
		return priv->irq;

	memset(&tmpl, 0, sizeof(tmpl));
	tmpl.id = SH_SENSOR_PIR;
	tmpl.type = SH_TYPE_BINARY;
	strscpy(tmpl.name, "pir", sizeof(tmpl.name));
	tmpl.cfg.id = SH_SENSOR_PIR;
	tmpl.cfg.enabled = 1;
	tmpl.cfg.debounce_ms = 200;
	tmpl.ops = &sh_pir_ops;
	tmpl.priv = priv;

	sensor = sh_register_sensor(hub, &tmpl);
	if (!sensor)
		return -ENOMEM;

	priv->sensor = sensor;

	level = gpiod_get_value_cansleep(priv->gpiod);
	if (level < 0)
		level = 0;

	memset(&val, 0, sizeof(val));
	val.id = SH_SENSOR_PIR;
	val.type = SH_TYPE_BINARY;
	val.flags = SH_FLAG_ENABLED | SH_FLAG_VALID | SH_FLAG_ONLINE;
	val.nvalues = 1;
	val.timestamp_ns = ktime_get_ns();
	val.values[0] = level;

	sh_update_sensor_value(hub, sensor, &val);

	ret = devm_request_threaded_irq(hub->dev,
					priv->irq,
					NULL,
					sh_pir_irq_thread,
					IRQF_TRIGGER_RISING |
					IRQF_TRIGGER_FALLING |
					IRQF_ONESHOT,
					"sensor_hub_pir",
					hub);
	if (ret)
		return ret;

	dev_info(hub->dev, "PIR registered, irq=%d\n", priv->irq);
	return 0;
}

void sh_pir_unregister(struct sh_core *hub)
{
}