#include <linux/kernel.h>
#include <linux/pwm.h>
#include <linux/slab.h>

#include "sensor_hub.h"

#define SH_BUZZER_DEFAULT_FREQ_HZ        2400U
#define SH_BUZZER_DEFAULT_DUTY_PERMILLE   500U
#define SH_BUZZER_DEFAULT_DURATION_MS     180U

struct sh_buzzer_priv {
	struct sh_core *hub;
	struct pwm_device *pwm;
	struct delayed_work stop_work;
	struct sh_endpoint *endpoint;
	u32 freq_hz;
	u32 duty_permille;
	u32 duration_ms;
	bool active;
};

static u32 sh_buzzer_sanitize_duty(u32 duty_permille)
{
	if (!duty_permille || duty_permille > 1000U)
		return SH_BUZZER_DEFAULT_DUTY_PERMILLE;

	return duty_permille;
}

static u32 sh_buzzer_sanitize_freq(u32 freq_hz)
{
	if (!freq_hz)
		return SH_BUZZER_DEFAULT_FREQ_HZ;

	return freq_hz;
}

static u32 sh_buzzer_sanitize_duration(u32 duration_ms)
{
	if (!duration_ms)
		return SH_BUZZER_DEFAULT_DURATION_MS;

	return duration_ms;
}

static int sh_buzzer_apply_pwm(struct sh_buzzer_priv *priv,
			       bool enable,
			       u32 freq_hz,
			       u32 duty_permille)
{
	struct pwm_state state;
	u64 period_ns;
	u64 duty_ns;

	if (!priv->pwm)
		return -ENODEV;

	pwm_init_state(priv->pwm, &state);

	if (!enable) {
		state.enabled = false;
		return pwm_apply_state(priv->pwm, &state);
	}

	if (!freq_hz)
		return -EINVAL;

	duty_permille = sh_buzzer_sanitize_duty(duty_permille);
	period_ns = DIV_ROUND_CLOSEST_ULL(1000000000ULL, freq_hz);
	duty_ns = DIV_ROUND_CLOSEST_ULL(period_ns * duty_permille, 1000ULL);
	if (!duty_ns)
		duty_ns = period_ns / 2;

	state.period = period_ns;
	state.duty_cycle = duty_ns;
	state.enabled = true;

	return pwm_apply_state(priv->pwm, &state);
}

static void sh_buzzer_build_value(struct sh_buzzer_priv *priv,
				  struct sh_sensor_value *val)
{
	u32 flags = SH_FLAG_VALID | SH_FLAG_ONLINE;

	if (priv->endpoint->cfg.enabled)
		flags |= SH_FLAG_ENABLED;
	if (priv->active)
		flags |= SH_FLAG_ACTIVE;

	sh_init_value(priv->endpoint, val, flags, 4);
	val->values[0] = priv->active ? 1 : 0;
	val->values[1] = (s32)priv->freq_hz;
	val->values[2] = (s32)priv->duration_ms;
	val->values[3] = (s32)priv->duty_permille;
}

static void sh_buzzer_commit_state(struct sh_buzzer_priv *priv,
				   u32 evt_type,
				   u32 code)
{
	struct sh_sensor_value val;

	sh_buzzer_build_value(priv, &val);
	sh_update_endpoint_value(priv->hub, priv->endpoint, &val);

	if (code)
		sh_emit_event(priv->hub, priv->endpoint, evt_type, code,
			      val.flags, &val);
}

static int __sh_buzzer_stop(struct sh_buzzer_priv *priv,
			    bool emit_event,
			    u32 evt_type,
			    u32 code)
{
	int ret;

	ret = sh_buzzer_apply_pwm(priv, false, 0, 0);
	if (ret)
		return ret;

	priv->active = false;
	sh_buzzer_commit_state(priv, evt_type,
			       emit_event ? code : SH_CODE_NONE);
	return 0;
}

static int sh_buzzer_stop(struct sh_buzzer_priv *priv, bool emit_event)
{
	cancel_delayed_work_sync(&priv->stop_work);
	return __sh_buzzer_stop(priv, emit_event,
				SH_EVT_OUTPUT, SH_CODE_OUTPUT_OFF);
}

static void sh_buzzer_stop_workfn(struct work_struct *work)
{
	struct sh_buzzer_priv *priv =
		container_of(to_delayed_work(work), struct sh_buzzer_priv, stop_work);

	__sh_buzzer_stop(priv, true, SH_EVT_OUTPUT, SH_CODE_OUTPUT_OFF);
}

static int sh_buzzer_apply_cfg(struct sh_core *hub,
			       struct sh_endpoint *endpoint,
			       const struct sh_sensor_cfg *cfg)
{
	struct sh_buzzer_priv *priv = endpoint->priv;
	int ret;

	mutex_lock(&hub->lock);
	endpoint->cfg.enabled = cfg->enabled;
	memcpy(endpoint->cfg.params, cfg->params, sizeof(endpoint->cfg.params));
	mutex_unlock(&hub->lock);

	priv->freq_hz = sh_buzzer_sanitize_freq((u32)endpoint->cfg.params[0]);
	priv->duty_permille = sh_buzzer_sanitize_duty((u32)endpoint->cfg.params[1]);
	priv->duration_ms = sh_buzzer_sanitize_duration((u32)endpoint->cfg.params[2]);

	if (!cfg->enabled) {
		cancel_delayed_work_sync(&priv->stop_work);
		if (priv->active) {
			ret = __sh_buzzer_stop(priv, false, SH_EVT_OUTPUT,
					       SH_CODE_OUTPUT_OFF);
			if (ret)
				return ret;
		}

		sh_buzzer_commit_state(priv, SH_EVT_CONFIG, SH_CODE_DISABLE);
		return 0;
	}

	cancel_delayed_work_sync(&priv->stop_work);
	if (priv->active) {
		ret = __sh_buzzer_stop(priv, false, SH_EVT_OUTPUT,
				       SH_CODE_OUTPUT_OFF);
		if (ret)
			return ret;
	}

	sh_buzzer_commit_state(priv, SH_EVT_CONFIG, SH_CODE_ENABLE);
	return 0;
}

static int sh_buzzer_dispatch(struct sh_core *hub,
			      struct sh_endpoint *endpoint,
			      const struct sh_action_req *req)
{
	struct sh_buzzer_priv *priv = endpoint->priv;
	u32 freq_hz;
	u32 duty_permille;
	u32 duration_ms;
	int ret;

	(void)hub;

	if (!endpoint->cfg.enabled)
		return -EACCES;

	switch (req->action) {
	case SH_ACTION_STOP:
		return sh_buzzer_stop(priv, true);

	case SH_ACTION_ALERT:
	case SH_ACTION_PULSE:
		freq_hz = sh_buzzer_sanitize_freq((u32)endpoint->cfg.params[0]);
		duty_permille = sh_buzzer_sanitize_duty((u32)endpoint->cfg.params[1]);
		duration_ms = sh_buzzer_sanitize_duration((u32)endpoint->cfg.params[2]);

		if (req->nvalues > 0 && req->values[0] > 0)
			freq_hz = (u32)req->values[0];
		if (req->nvalues > 1 && req->values[1] > 0)
			duty_permille = (u32)req->values[1];
		if (req->duration_ms)
			duration_ms = req->duration_ms;

		freq_hz = sh_buzzer_sanitize_freq(freq_hz);
		duty_permille = sh_buzzer_sanitize_duty(duty_permille);
		duration_ms = sh_buzzer_sanitize_duration(duration_ms);

		cancel_delayed_work_sync(&priv->stop_work);

		ret = sh_buzzer_apply_pwm(priv, true, freq_hz, duty_permille);
		if (ret)
			return ret;

		priv->freq_hz = freq_hz;
		priv->duty_permille = duty_permille;
		priv->duration_ms = duration_ms;
		priv->active = true;
		sh_buzzer_commit_state(priv, SH_EVT_OUTPUT,
				       req->action == SH_ACTION_ALERT ?
				       SH_CODE_ALERT : SH_CODE_OUTPUT_ON);

		if (duration_ms)
			schedule_delayed_work(&priv->stop_work,
					      msecs_to_jiffies(duration_ms));
		return 0;
	default:
		return -EINVAL;
	}
}

static void sh_buzzer_remove(struct sh_core *hub, struct sh_endpoint *endpoint)
{
	struct sh_buzzer_priv *priv = endpoint->priv;

	(void)hub;

	if (!priv)
		return;

	sh_buzzer_stop(priv, false);
}

static const struct sh_endpoint_ops sh_buzzer_ops = {
	.apply_cfg = sh_buzzer_apply_cfg,
	.dispatch  = sh_buzzer_dispatch,
	.remove    = sh_buzzer_remove,
};

int sh_buzzer_register(struct sh_core *hub)
{
	struct sh_buzzer_priv *priv;
	struct sh_endpoint tmpl = { 0 };
	struct sh_endpoint *endpoint;

	priv = devm_kzalloc(hub->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pwm = devm_pwm_get(hub->dev, "buzzer");
	if (IS_ERR(priv->pwm)) {
		dev_warn(hub->dev,
			 "PWM buzzer unavailable (%ld), skip output endpoint\n",
			 PTR_ERR(priv->pwm));
		priv->pwm = NULL;
		return 0;
	}

	if (!priv->pwm) {
		dev_info(hub->dev, "no PWM buzzer configured, skip output endpoint\n");
		return 0;
	}

	priv->hub = hub;
	INIT_DELAYED_WORK(&priv->stop_work, sh_buzzer_stop_workfn);

	device_property_read_u32(hub->dev, "buzzer-default-freq-hz", &priv->freq_hz);
	device_property_read_u32(hub->dev, "buzzer-default-duty-permille",
				 &priv->duty_permille);
	device_property_read_u32(hub->dev, "buzzer-default-duration-ms",
				 &priv->duration_ms);

	priv->freq_hz = sh_buzzer_sanitize_freq(priv->freq_hz);
	priv->duty_permille = sh_buzzer_sanitize_duty(priv->duty_permille);
	priv->duration_ms = sh_buzzer_sanitize_duration(priv->duration_ms);

	tmpl.id = SH_SENSOR_BUZZER;
	tmpl.type = SH_TYPE_PWM;
	tmpl.direction = SH_DIR_OUTPUT;
	tmpl.caps = SH_CAP_CFG | SH_CAP_ACTION;
	strscpy(tmpl.name, "buzzer", sizeof(tmpl.name));
	tmpl.cfg.id = SH_SENSOR_BUZZER;
	tmpl.cfg.enabled = 1;
	tmpl.cfg.params[0] = (s32)priv->freq_hz;
	tmpl.cfg.params[1] = (s32)priv->duty_permille;
	tmpl.cfg.params[2] = (s32)priv->duration_ms;
	tmpl.ops = &sh_buzzer_ops;
	tmpl.priv = priv;

	endpoint = sh_register_endpoint(hub, &tmpl);
	if (!endpoint)
		return -ENOMEM;

	priv->endpoint = endpoint;
	priv->active = false;
	sh_buzzer_commit_state(priv, SH_EVT_CONFIG, SH_CODE_ENABLE);

	dev_info(hub->dev,
		 "PWM buzzer registered, default=%uHz duty=%u/1000 duration=%ums\n",
		 priv->freq_hz, priv->duty_permille, priv->duration_ms);
	return 0;
}

void sh_buzzer_unregister(struct sh_core *hub)
{
	sh_unregister_endpoint(hub, sh_find_endpoint(hub, SH_SENSOR_BUZZER));
}
