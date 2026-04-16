#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "sensor_hub.h"

static int sh_q_pop(struct sh_core *hub, struct sh_event *evt)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&hub->q.lock, flags);

	if (!hub->q.count) {
		ret = -ENOENT;
		goto out;
	}

	*evt = hub->q.events[hub->q.tail];
	hub->q.tail = (hub->q.tail + 1) % SH_EVT_Q_SIZE;
	hub->q.count--;

out:
	spin_unlock_irqrestore(&hub->q.lock, flags);
	return ret;
}

int sh_push_event(struct sh_core *hub, struct sh_event *evt)
{
	unsigned long flags;

	spin_lock_irqsave(&hub->q.lock, flags);

	if (hub->q.count == SH_EVT_Q_SIZE) {
		hub->q.tail = (hub->q.tail + 1) % SH_EVT_Q_SIZE;
		hub->q.count--;
		hub->q.dropped++;
	}

	evt->seq = ++hub->event_seq;
	hub->q.events[hub->q.head] = *evt;
	hub->q.head = (hub->q.head + 1) % SH_EVT_Q_SIZE;
	hub->q.count++;

	spin_unlock_irqrestore(&hub->q.lock, flags);

	wake_up_interruptible(&hub->q.wq);
	return 0;
}
EXPORT_SYMBOL_GPL(sh_push_event);

struct sh_sensor *sh_find_sensor(struct sh_core *hub, u32 id)
{
	u32 i;

	for (i = 0; i < hub->sensor_count; i++) {
		if (hub->sensors[i].registered && hub->sensors[i].id == id)
			return &hub->sensors[i];
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(sh_find_sensor);

struct sh_sensor *sh_register_sensor(struct sh_core *hub,
				     const struct sh_sensor *tmpl)
{
	struct sh_sensor *dst;

	if (hub->sensor_count >= SH_MAX_SENSORS)
		return NULL;

	dst = &hub->sensors[hub->sensor_count++];
	*dst = *tmpl;
	dst->registered = true;

	return dst;
}
EXPORT_SYMBOL_GPL(sh_register_sensor);

void sh_update_sensor_value(struct sh_core *hub,
			    struct sh_sensor *sensor,
			    const struct sh_sensor_value *val)
{
	mutex_lock(&hub->lock);
	sensor->value = *val;
	hub->snapshot_seq++;
	mutex_unlock(&hub->lock);
}
EXPORT_SYMBOL_GPL(sh_update_sensor_value);

void sh_fill_snapshot(struct sh_core *hub, struct sh_snapshot *snap)
{
	u32 i, out = 0;

	memset(snap, 0, sizeof(*snap));
	snap->version = SH_API_VERSION;
	snap->seq = hub->snapshot_seq;
	snap->dropped_events = hub->q.dropped;
	snap->timestamp_ns = ktime_get_ns();

	mutex_lock(&hub->lock);

	for (i = 0; i < hub->sensor_count && out < SH_MAX_SENSORS; i++) {
		if (!hub->sensors[i].registered)
			continue;
		snap->items[out++] = hub->sensors[i].value;
	}

	snap->count = out;
	mutex_unlock(&hub->lock);
}
EXPORT_SYMBOL_GPL(sh_fill_snapshot);

static int sh_open(struct inode *inode, struct file *file)
{
	struct miscdevice *mdev = file->private_data;
	struct sh_core *hub = container_of(mdev, struct sh_core, miscdev);

	file->private_data = hub;
	return 0;
}

static ssize_t sh_read(struct file *file, char __user *buf,
		       size_t len, loff_t *ppos)
{
	struct sh_core *hub = file->private_data;
	size_t copied = 0;
	struct sh_event evt;
	int ret;

	if (len < sizeof(struct sh_event))
		return -EINVAL;

	if (len % sizeof(struct sh_event))
		return -EINVAL;

	while (copied + sizeof(struct sh_event) <= len) {
		ret = sh_q_pop(hub, &evt);
		if (ret == -ENOENT) {
			if (copied)
				break;

			if (file->f_flags & O_NONBLOCK)
				return -EAGAIN;

			ret = wait_event_interruptible(hub->q.wq, hub->q.count > 0);
			if (ret)
				return ret;
			continue;
		}

		if (copy_to_user(buf + copied, &evt, sizeof(evt)))
			return copied ? copied : -EFAULT;

		copied += sizeof(evt);
	}

	return copied;
}

static __poll_t sh_poll(struct file *file, poll_table *wait)
{
	struct sh_core *hub = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &hub->q.wq, wait);

	if (hub->q.count > 0)
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static long sh_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct sh_core *hub = file->private_data;
	void __user *argp = (void __user *)arg;
	struct sh_hub_info info;
	struct sh_snapshot snap;
	struct sh_refresh_req req;
	struct sh_sensor_cfg cfg;
	struct sh_sensor *sensor;

	switch (cmd) {
	case SH_IOC_GET_INFO:
		memset(&info, 0, sizeof(info));
		info.version = SH_API_VERSION;
		info.sensor_count = hub->sensor_count;
		info.queue_size = SH_EVT_Q_SIZE;
		info.queue_depth = hub->q.count;
		info.event_seq = hub->event_seq;
		info.snapshot_seq = hub->snapshot_seq;

		if (copy_to_user(argp, &info, sizeof(info)))
			return -EFAULT;
		return 0;

	case SH_IOC_GET_SNAPSHOT:
		sh_fill_snapshot(hub, &snap);
		if (copy_to_user(argp, &snap, sizeof(snap)))
			return -EFAULT;
		return 0;

	case SH_IOC_CLR_EVENTS:{
		unsigned long flags;

		spin_lock_irqsave(&hub->q.lock, flags);
		hub->q.head = 0;
		hub->q.tail = 0;
		hub->q.count = 0;
		spin_unlock_irqrestore(&hub->q.lock, flags);
		return 0;
	}


	case SH_IOC_FORCE_REFRESH:
		if (copy_from_user(&req, argp, sizeof(req)))
			return -EFAULT;

		sensor = sh_find_sensor(hub, req.id);
		if (!sensor || !sensor->ops || !sensor->ops->refresh)
			return -EINVAL;

		return sensor->ops->refresh(hub, sensor);

	case SH_IOC_GET_SENSOR_CFG:
		if (copy_from_user(&cfg, argp, sizeof(cfg)))
			return -EFAULT;

		sensor = sh_find_sensor(hub, cfg.id);
		if (!sensor)
			return -EINVAL;

		cfg = sensor->cfg;
		if (copy_to_user(argp, &cfg, sizeof(cfg)))
			return -EFAULT;
		return 0;

	case SH_IOC_SET_SENSOR_CFG:
		if (copy_from_user(&cfg, argp, sizeof(cfg)))
			return -EFAULT;

		sensor = sh_find_sensor(hub, cfg.id);
		if (!sensor || !sensor->ops || !sensor->ops->apply_cfg)
			return -EINVAL;

		return sensor->ops->apply_cfg(hub, sensor, &cfg);

	default:
		return -ENOTTY;
	}

}

static const struct file_operations sh_fops = {
	.owner          = THIS_MODULE,
	.open           = sh_open,
	.read           = sh_read,
	.poll           = sh_poll,
	.unlocked_ioctl = sh_ioctl,
	.llseek         = no_llseek,
};

static int sh_probe(struct platform_device *pdev)
{
	struct sh_core *hub;
	int ret;

	hub = devm_kzalloc(&pdev->dev, sizeof(*hub), GFP_KERNEL);
	if (!hub)
		return -ENOMEM;

	hub->dev = &pdev->dev;
	mutex_init(&hub->lock);
	spin_lock_init(&hub->q.lock);
	init_waitqueue_head(&hub->q.wq);

	hub->miscdev.minor = MISC_DYNAMIC_MINOR;
	hub->miscdev.name = "sensor_hub";
	hub->miscdev.fops = &sh_fops;
	hub->miscdev.parent = &pdev->dev;

	platform_set_drvdata(pdev, hub);

	ret = misc_register(&hub->miscdev);
	if (ret)
		return ret;

	ret = sh_pir_register(hub);
	if (ret)
		goto err_misc;

	ret = sh_sht20_register(hub);
	if (ret)
		goto err_pir;

	dev_info(&pdev->dev, "sensor_hub probed\n");
	return 0;

err_pir:
	sh_pir_unregister(hub);
err_misc:
	misc_deregister(&hub->miscdev);
	return ret;
}

static int sh_remove(struct platform_device *pdev)
{
	struct sh_core *hub = platform_get_drvdata(pdev);
	u32 i;

	sh_sht20_unregister(hub);
	sh_pir_unregister(hub);

	for (i = 0; i < hub->sensor_count; i++) {
		if (hub->sensors[i].registered &&
		    hub->sensors[i].ops &&
		    hub->sensors[i].ops->remove) {
			hub->sensors[i].ops->remove(hub, &hub->sensors[i]);
		}
	}

	misc_deregister(&hub->miscdev);
	return 0;
}

static const struct of_device_id sh_of_match[] = {
	{ .compatible = "poozoo,sensor-hub", },
	{ }
};
MODULE_DEVICE_TABLE(of, sh_of_match);

static struct platform_driver sh_driver = {
	.probe  = sh_probe,
	.remove = sh_remove,
	.driver = {
		.name = "sensor_hub",
		.of_match_table = sh_of_match,
	},
};

module_platform_driver(sh_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenAI + lhy");
MODULE_DESCRIPTION("Unified sensor hub driver for PIR + SHT20");