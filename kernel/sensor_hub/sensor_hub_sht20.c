#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "sensor_hub.h"

#define SHT20_CMD_TEMP_NOHOLD 0xF3
#define SHT20_CMD_HUMI_NOHOLD 0xF5

struct sh_sht20_priv {
	struct sh_core *hub;
	struct i2c_adapter *adap;
	struct i2c_client *client;
	struct delayed_work work;
	struct mutex io_lock;

	u32 bus_num;
	u16 addr;

	struct sh_endpoint *endpoint;
};

static int sh_sht20_do_sample(struct sh_sht20_priv *priv, s32 *temp_mc, s32 *humi_mrh)
{
	u8 cmd;
	u8 buf[3];
	u16 raw;
	int ret;

	cmd = SHT20_CMD_TEMP_NOHOLD;
	ret = i2c_master_send(priv->client, &cmd, 1);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;

	msleep(100);

	ret = i2c_master_recv(priv->client, buf, 3);
	if (ret != 3)
		return ret < 0 ? ret : -EIO;

	raw = (((u16)buf[0] << 8) | buf[1]) & ~0x0003;
	*temp_mc = -46850 + div_s64((s64)175720 * raw, 65536);

	cmd = SHT20_CMD_HUMI_NOHOLD;
	ret = i2c_master_send(priv->client, &cmd, 1);
	if (ret != 1)
		return ret < 0 ? ret : -EIO;

	msleep(50);

	ret = i2c_master_recv(priv->client, buf, 3);
	if (ret != 3)
		return ret < 0 ? ret : -EIO;

	raw = (((u16)buf[0] << 8) | buf[1]) & ~0x0003;
	*humi_mrh = -6000 + div_s64((s64)125000 * raw, 65536);

	return 0;
}

static int sh_sht20_refresh(struct sh_core *hub, struct sh_endpoint *endpoint)
{
	struct sh_sht20_priv *priv = endpoint->priv;
	struct sh_sensor_value val;
	s32 temp_mc, humi_mrh;
	int ret;

	mutex_lock(&priv->io_lock);
	ret = sh_sht20_do_sample(priv, &temp_mc, &humi_mrh);
	mutex_unlock(&priv->io_lock);
	if (ret)
		return ret;

	sh_init_value(endpoint, &val,
		      (endpoint->cfg.enabled ? SH_FLAG_ENABLED : 0) |
		      SH_FLAG_VALID | SH_FLAG_ONLINE,
		      2);
	val.values[0] = temp_mc;
	val.values[1] = humi_mrh;

	sh_update_endpoint_value(hub, endpoint, &val);
	return 0;
}

static void sh_sht20_workfn(struct work_struct *work)
{
	struct sh_sht20_priv *priv =
		container_of(to_delayed_work(work), struct sh_sht20_priv, work);
	struct sh_endpoint *endpoint = priv->endpoint;

	if (!endpoint || !endpoint->cfg.enabled)
		return;

	sh_sht20_refresh(priv->hub, endpoint);

	if (endpoint->cfg.enabled && endpoint->cfg.period_ms)
		schedule_delayed_work(&priv->work,
				      msecs_to_jiffies(endpoint->cfg.period_ms));
}

static int sh_sht20_apply_cfg(struct sh_core *hub,
			      struct sh_endpoint *endpoint,
			      const struct sh_sensor_cfg *cfg)
{
	struct sh_sht20_priv *priv = endpoint->priv;
	struct sh_sensor_value val;

	mutex_lock(&hub->lock);
	endpoint->cfg.enabled = cfg->enabled;
	endpoint->cfg.period_ms = cfg->period_ms ? cfg->period_ms : 1000;
	memcpy(endpoint->cfg.params, cfg->params, sizeof(endpoint->cfg.params));
	memcpy(endpoint->cfg.thresh_hi, cfg->thresh_hi, sizeof(endpoint->cfg.thresh_hi));
	memcpy(endpoint->cfg.thresh_lo, cfg->thresh_lo, sizeof(endpoint->cfg.thresh_lo));
	mutex_unlock(&hub->lock);

	cancel_delayed_work_sync(&priv->work);
	if (endpoint->cfg.enabled)
		schedule_delayed_work(&priv->work, 0);

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

static void sh_sht20_remove_sensor(struct sh_core *hub, struct sh_endpoint *endpoint)
{
	struct sh_sht20_priv *priv = endpoint->priv;

	cancel_delayed_work_sync(&priv->work);

	if (priv->client)
		i2c_unregister_device(priv->client);

	if (priv->adap)
		i2c_put_adapter(priv->adap);
}

static const struct sh_endpoint_ops sh_sht20_ops = {
	.refresh   = sh_sht20_refresh,
	.apply_cfg = sh_sht20_apply_cfg,
	.remove    = sh_sht20_remove_sensor,
};

int sh_sht20_register(struct sh_core *hub)
{
	struct sh_sht20_priv *priv;
	struct sh_endpoint tmpl = { 0 };
	struct sh_endpoint *endpoint;
	struct i2c_board_info info = { };
	struct sh_sensor_value val = { 0 };
	u32 bus_num = 2;
	u32 addr = 0x40;
	int ret;

	device_property_read_u32(hub->dev, "sht20-bus-num", &bus_num);
	device_property_read_u32(hub->dev, "sht20-addr", &addr);

	priv = devm_kzalloc(hub->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hub = hub;
	priv->bus_num = bus_num;
	priv->addr = addr;
	mutex_init(&priv->io_lock);

	priv->adap = i2c_get_adapter(priv->bus_num);
	if (!priv->adap)
		return -ENODEV;

	strscpy(info.type, "sht20", I2C_NAME_SIZE);
	info.addr = priv->addr;

	priv->client = i2c_new_client_device(priv->adap, &info);
	if (IS_ERR(priv->client)) {
		ret = PTR_ERR(priv->client);
		priv->client = NULL;
		i2c_put_adapter(priv->adap);
		priv->adap = NULL;
		return ret;
	}

	memset(&tmpl, 0, sizeof(tmpl));
	tmpl.id = SH_SENSOR_SHT20;
	tmpl.type = SH_TYPE_ENV;
	tmpl.direction = SH_DIR_INPUT;
	tmpl.caps = SH_CAP_REFRESH | SH_CAP_CFG;
	strscpy(tmpl.name, "sht20", sizeof(tmpl.name));
	tmpl.cfg.id = SH_SENSOR_SHT20;
	tmpl.cfg.enabled = 1;
	tmpl.cfg.period_ms = 1000;
	tmpl.ops = &sh_sht20_ops;
	tmpl.priv = priv;

	endpoint = sh_register_endpoint(hub, &tmpl);
	if (!endpoint)
		return -ENOMEM;

	priv->endpoint = endpoint;
	INIT_DELAYED_WORK(&priv->work, sh_sht20_workfn);

	sh_init_value(endpoint, &val, SH_FLAG_ENABLED, 2);
	val.values[0] = 0;
	val.values[1] = 0;

	sh_update_endpoint_value(hub, endpoint, &val);

	ret = sh_sht20_refresh(hub, endpoint);
	if (ret)
		dev_warn(hub->dev, "initial SHT20 refresh failed: %d\n", ret);

	schedule_delayed_work(&priv->work,
			      msecs_to_jiffies(endpoint->cfg.period_ms));

	dev_info(hub->dev, "SHT20 registered on i2c-%u addr=0x%02x\n",
		 priv->bus_num, priv->addr);
	return 0;
}

void sh_sht20_unregister(struct sh_core *hub)
{
	sh_unregister_endpoint(hub, sh_find_endpoint(hub, SH_SENSOR_SHT20));
}
