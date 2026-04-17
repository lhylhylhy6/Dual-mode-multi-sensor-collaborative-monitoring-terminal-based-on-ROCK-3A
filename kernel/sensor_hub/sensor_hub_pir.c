#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "sensor_hub.h"

struct sh_pir_priv {
	struct gpio_desc *gpiod;
	int irq;
	s64 last_irq_ns;
	struct sh_endpoint *endpoint;
};

static irqreturn_t sh_pir_irq_thread(int irq, void *data)
{
	struct sh_core *hub = data;
	struct sh_endpoint *endpoint = sh_find_endpoint(hub, SH_SENSOR_PIR);
	struct sh_pir_priv *priv;
	struct sh_sensor_value val;
	s64 now;
	int level;
	u32 debounce_ms;

	if (!endpoint)
		return IRQ_HANDLED;

	priv = endpoint->priv;
	if (!priv || !endpoint->cfg.enabled)
		return IRQ_HANDLED;

	now = ktime_get_ns();
	debounce_ms = endpoint->cfg.debounce_ms;

	if (debounce_ms &&
	    priv->last_irq_ns &&
	    now - priv->last_irq_ns < (s64)debounce_ms * 1000000LL) {
		return IRQ_HANDLED;
	}

	level = gpiod_get_value_cansleep(priv->gpiod);
	if (level < 0)
		return IRQ_HANDLED;

	priv->last_irq_ns = now;

	sh_init_value(endpoint, &val,
		      SH_FLAG_ENABLED | SH_FLAG_VALID | SH_FLAG_ONLINE,
		      1);
	val.timestamp_ns = now;
	val.values[0] = level;

	sh_update_endpoint_value(hub, endpoint, &val);
	sh_emit_event(hub, endpoint, SH_EVT_SENSOR,
		      level ? SH_CODE_TRIGGER : SH_CODE_FALLING,
		      val.flags, &val);

	return IRQ_HANDLED;
}

static int sh_pir_refresh(struct sh_core *hub, struct sh_endpoint *endpoint)
{
	struct sh_pir_priv *priv = endpoint->priv;
	struct sh_sensor_value val;
	int level;

	level = gpiod_get_value_cansleep(priv->gpiod);
	if (level < 0)
		return level;

	sh_init_value(endpoint, &val,
		      (endpoint->cfg.enabled ? SH_FLAG_ENABLED : 0) |
		      SH_FLAG_VALID | SH_FLAG_ONLINE,
		      1);
	val.values[0] = level;

	sh_update_endpoint_value(hub, endpoint, &val);
	return 0;
}

static int sh_pir_apply_cfg(struct sh_core *hub,
			    struct sh_endpoint *endpoint,
			    const struct sh_sensor_cfg *cfg)
{
	struct sh_sensor_value val;

	mutex_lock(&hub->lock);
	endpoint->cfg.enabled = cfg->enabled;
	endpoint->cfg.debounce_ms = cfg->debounce_ms;
	mutex_unlock(&hub->lock);

	val = endpoint->value;
	val.flags &= ~SH_FLAG_ENABLED;
	if (cfg->enabled)
		val.flags |= SH_FLAG_ENABLED;
	val.timestamp_ns = ktime_get_ns();
	sh_update_endpoint_value(hub, endpoint, &val);
	sh_emit_event(hub, endpoint, SH_EVT_CONFIG,
		      cfg->enabled ? SH_CODE_ENABLE : SH_CODE_DISABLE,
		      val.flags, &val);
	return 0;
}

static void sh_pir_remove_sensor(struct sh_core *hub, struct sh_endpoint *endpoint)
{
	(void)hub;
	(void)endpoint;
}

static const struct sh_endpoint_ops sh_pir_ops = {
	.refresh   = sh_pir_refresh,
	.apply_cfg = sh_pir_apply_cfg,
	.remove    = sh_pir_remove_sensor,
};

int sh_pir_register(struct sh_core *hub)
{
	struct sh_pir_priv *priv;
	struct sh_endpoint tmpl = { 0 };
	struct sh_endpoint *endpoint;
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
	tmpl.direction = SH_DIR_INPUT;
	tmpl.caps = SH_CAP_REFRESH | SH_CAP_CFG | SH_CAP_EVENT;
	strscpy(tmpl.name, "pir", sizeof(tmpl.name));
	tmpl.cfg.id = SH_SENSOR_PIR;
	tmpl.cfg.enabled = 1;
	tmpl.cfg.debounce_ms = 200;
	tmpl.ops = &sh_pir_ops;
	tmpl.priv = priv;

	endpoint = sh_register_endpoint(hub, &tmpl);
	if (!endpoint)
		return -ENOMEM;

	priv->endpoint = endpoint;

	level = gpiod_get_value_cansleep(priv->gpiod);
	if (level < 0)
		level = 0;

	sh_init_value(endpoint, &val,
		      SH_FLAG_ENABLED | SH_FLAG_VALID | SH_FLAG_ONLINE,
		      1);
	val.values[0] = level;

	sh_update_endpoint_value(hub, endpoint, &val);

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
	sh_unregister_endpoint(hub, sh_find_endpoint(hub, SH_SENSOR_PIR));
}
