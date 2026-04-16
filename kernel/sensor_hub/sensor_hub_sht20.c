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

	u32 bus_num;
	u16 addr;

	struct sh_sensor *sensor;
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

static int sh_sht20_refresh(struct sh_core *hub, struct sh_sensor *sensor)
{
	struct sh_sht20_priv *priv = sensor->priv;
	struct sh_sensor_value val;
	//struct sh_event evt;
	s32 temp_mc, humi_mrh;
	int ret;

	ret = sh_sht20_do_sample(priv, &temp_mc, &humi_mrh);
	if (ret)
		return ret;

	memset(&val, 0, sizeof(val));
	val.id = SH_SENSOR_SHT20;
	val.type = SH_TYPE_ENV;
	val.flags = SH_FLAG_ENABLED | SH_FLAG_VALID | SH_FLAG_ONLINE;
	val.nvalues = 2;
	val.timestamp_ns = ktime_get_ns();
	val.values[0] = temp_mc;
	val.values[1] = humi_mrh;

	sh_update_sensor_value(hub, sensor, &val);

/* 	memset(&evt, 0, sizeof(evt));
	evt.type = SH_EVT_SENSOR;
	evt.sensor_id = SH_SENSOR_SHT20;
	evt.code = SH_CODE_SAMPLE;
	evt.flags = SH_FLAG_VALID;
	evt.nvalues = 2;
	evt.timestamp_ns = val.timestamp_ns;
	evt.values[0] = temp_mc;
	evt.values[1] = humi_mrh;

	sh_push_event(hub, &evt); */
	return 0;
}

static void sh_sht20_workfn(struct work_struct *work)
{
	struct sh_sht20_priv *priv =
		container_of(to_delayed_work(work), struct sh_sht20_priv, work);
	struct sh_sensor *sensor = priv->sensor;

	if (!sensor || !sensor->cfg.enabled)
		return;

	sh_sht20_refresh(priv->hub, sensor);

	if (sensor->cfg.enabled && sensor->cfg.period_ms)
		schedule_delayed_work(&priv->work,
				      msecs_to_jiffies(sensor->cfg.period_ms));
}

static int sh_sht20_apply_cfg(struct sh_core *hub,
			      struct sh_sensor *sensor,
			      const struct sh_sensor_cfg *cfg)
{
	struct sh_sht20_priv *priv = sensor->priv;

	mutex_lock(&hub->lock);
	sensor->cfg.enabled = cfg->enabled;
	sensor->cfg.period_ms = cfg->period_ms ? cfg->period_ms : 1000;
	memcpy(sensor->cfg.thresh_hi, cfg->thresh_hi, sizeof(sensor->cfg.thresh_hi));
	memcpy(sensor->cfg.thresh_lo, cfg->thresh_lo, sizeof(sensor->cfg.thresh_lo));
	mutex_unlock(&hub->lock);

	cancel_delayed_work_sync(&priv->work);
	if (sensor->cfg.enabled)
		schedule_delayed_work(&priv->work, 0);

	return 0;
}

static void sh_sht20_remove_sensor(struct sh_core *hub, struct sh_sensor *sensor)
{
	struct sh_sht20_priv *priv = sensor->priv;

	cancel_delayed_work_sync(&priv->work);

	if (priv->client)
		i2c_unregister_device(priv->client);

	if (priv->adap)
		i2c_put_adapter(priv->adap);
}

static const struct sh_sensor_ops sh_sht20_ops = {
	.refresh   = sh_sht20_refresh,
	.apply_cfg = sh_sht20_apply_cfg,
	.remove    = sh_sht20_remove_sensor,
};

int sh_sht20_register(struct sh_core *hub)
{
	struct sh_sht20_priv *priv;
	struct sh_sensor tmpl = { 0 };
	struct sh_sensor *sensor;
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
	strscpy(tmpl.name, "sht20", sizeof(tmpl.name));
	tmpl.cfg.id = SH_SENSOR_SHT20;
	tmpl.cfg.enabled = 1;
	tmpl.cfg.period_ms = 1000;
	tmpl.ops = &sh_sht20_ops;
	tmpl.priv = priv;

	sensor = sh_register_sensor(hub, &tmpl);
	if (!sensor)
		return -ENOMEM;

	priv->sensor = sensor;
	INIT_DELAYED_WORK(&priv->work, sh_sht20_workfn);

	val.id = SH_SENSOR_SHT20;
	val.type = SH_TYPE_ENV;
	val.flags = SH_FLAG_ENABLED;
	val.nvalues = 2;
	val.timestamp_ns = ktime_get_ns();
	val.values[0] = 0;
	val.values[1] = 0;

	sh_update_sensor_value(hub, sensor, &val);

	ret = sh_sht20_refresh(hub, sensor);
	if (ret)
		dev_warn(hub->dev, "initial SHT20 refresh failed: %d\n", ret);

	schedule_delayed_work(&priv->work,
			      msecs_to_jiffies(sensor->cfg.period_ms));

	dev_info(hub->dev, "SHT20 registered on i2c-%u addr=0x%02x\n",
		 priv->bus_num, priv->addr);
	return 0;
}

void sh_sht20_unregister(struct sh_core *hub)
{
}