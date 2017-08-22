/*
 * linux/drivers/develed/pwm-ssc.c
 *
 * PWM-generated signals for SSC peripheral
 *
 * Copyright @ 2017 Develer Srl
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/err.h>
#include <linux/pwm.h>
#include <linux/sysfs.h>

#define NSEC_IN_SEC	1000000000

enum pwm_ssc_signals {
	PWM_SSC_CLOCK	= 0,
	PWM_SSC_SYNC,
	PWM_SSC_STAMP,
	PWM_SSC_SIGNALS_NUM /* guard */
};

static const char *pwm_ssc_signal_names[PWM_SSC_SIGNALS_NUM] = {
	"develed,clk-pwm",
	"develed,sync-pwm",
	"develed,stamp-pwm",
};

struct pwm_ssc_params {
	struct pwm_device	*pwm;
	unsigned int		channel;
	unsigned int		period;
	unsigned int		duty;
	unsigned int		polarity;
};

struct pwm_ssc_stamp {
	s64	timestamp;
	u64	id;
} __attribute__((packed));

struct pwm_ssc_data {
	struct pwm_ssc_params	params[PWM_SSC_SIGNALS_NUM];
	struct pwm_ssc_stamp	last_stamp;
	spinlock_t		lock;
};

static int pwm_ssc_enable(struct device *dev, struct pwm_ssc_data *priv);

static ssize_t pwm_ssc_timestamp_show(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	struct pwm_ssc_data *priv = dev_get_drvdata(dev);

	spin_lock_irq(&priv->lock);
	memcpy(buf, &priv->last_stamp, sizeof(struct pwm_ssc_stamp));
	spin_unlock_irq(&priv->lock);

	return sizeof(struct pwm_ssc_stamp);
}

static ssize_t pwm_ssc_frequency_show(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	struct pwm_ssc_data *priv = dev_get_drvdata(dev);

	return sprintf(buf, "%d", NSEC_IN_SEC / priv->params[PWM_SSC_CLOCK].period);
}

static ssize_t pwm_ssc_enable_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t size)
{
	struct pwm_ssc_data *priv = dev_get_drvdata(dev);

	if (pwm_ssc_enable(dev, priv))
		return -EIO;

	return size;
}

static DEVICE_ATTR(timestamp, S_IRUGO, pwm_ssc_timestamp_show, NULL);
static DEVICE_ATTR(frequency, S_IRUGO, pwm_ssc_frequency_show, NULL);
static DEVICE_ATTR(enable,    S_IWUSR, NULL, pwm_ssc_enable_store);

static struct attribute *pwm_ssc_attributes[] = {
	&dev_attr_timestamp.attr,
	&dev_attr_frequency.attr,
	&dev_attr_enable.attr,
	NULL
};

static const struct attribute_group pwm_ssc_attr_group = {
	.attrs	= pwm_ssc_attributes,
};

static void pwm_ssc_capture_cb(void *args)
{
	struct pwm_ssc_data *priv = args;
	struct timeval now;

	do_gettimeofday(&now);
	priv->last_stamp.timestamp = timeval_to_ns(&now);
	priv->last_stamp.id++;
}

static int pwm_ssc_add(struct device *dev, const char *pwm_label,
		       struct pwm_ssc_params *params)
{
	int ret;
	u32 props[4];

	ret = of_property_read_u32_array(dev->of_node, pwm_label, props, ARRAY_SIZE(props));
	if (ret) {
		dev_err(dev, "could not read property %s: %d\n", pwm_label, ret);
		return ret;
	}

	params->channel = props[0];
	params->period = props[1];
	params->duty = props[2];
	params->polarity = props[3];

	params->pwm = pwm_request(params->channel, pwm_label);
	if (IS_ERR(params->pwm)) {
		dev_err(dev, "could not request PWM%d: %d\n", params->channel, ret);
		return PTR_ERR(params->pwm);
	}

	ret = pwm_config(params->pwm, params->duty, params->period);
	if (ret) {
		dev_err(dev, "could not config PWM%d: %d\n", params->channel, ret);
		goto exit_free;
	}

	ret = pwm_set_polarity(params->pwm, params->polarity);
	if (ret) {
		dev_err(dev, "could not set PWM%d polarity\n", params->channel);
		goto exit_free;
	}

	dev_info(dev, "%s: chan=%d, period=%d, duty=%d\n", pwm_label,
			params->channel, params->period, params->duty);

	return 0;

exit_free:
	pwm_free(params->pwm);
	return ret;
}

static int pwm_ssc_create(struct device *dev, struct pwm_ssc_data *priv)
{
	int i;
	int ret;

	for (i = 0; i < PWM_SSC_SIGNALS_NUM; i++) {
		ret = pwm_ssc_add(dev, pwm_ssc_signal_names[i], &priv->params[i]);
		if (ret)
			goto release_all;
	}

	pwm_set_capture(priv->params[PWM_SSC_STAMP].pwm, pwm_ssc_capture_cb, priv);

	return 0;

release_all:
	for (i -= 1; i >= 0; i--)
		pwm_free(priv->params[i].pwm);

	return ret;
}

static int pwm_ssc_enable(struct device *dev, struct pwm_ssc_data *priv)
{
	int ret;

	ret = pwm_sync(priv->params[PWM_SSC_CLOCK].pwm);
	if (ret)
		dev_err(dev, "could not sync PWM channels\n");

	return ret;
}

static void pwm_ssc_cleanup(struct pwm_ssc_data *priv)
{
	int i;

	for (i = 0; i < PWM_SSC_SIGNALS_NUM; i++)
		pwm_free(priv->params[i].pwm);
}

static int pwm_ssc_probe(struct platform_device *pdev)
{
	struct pwm_ssc_data *priv;
	int ret = 0;

	priv = devm_kzalloc(&pdev->dev, sizeof(struct pwm_ssc_data), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	ret = sysfs_create_group(&pdev->dev.kobj, &pwm_ssc_attr_group);
	if (ret) {
		dev_err(&pdev->dev, "failed to create sysfs\n");
		return ret;
	}

	spin_lock_init(&priv->lock);

	priv->last_stamp.timestamp = -1;
	priv->last_stamp.id = 0;

	ret = pwm_ssc_create(&pdev->dev, priv);
	if (ret)
		return ret;

	ret = pwm_ssc_enable(&pdev->dev, priv);
	if (ret)
		goto err_cleanup;

	platform_set_drvdata(pdev, priv);

	return 0;

err_cleanup:
	pwm_ssc_cleanup(priv);
	return ret;
}

static int pwm_ssc_remove(struct platform_device *pdev)
{
	struct pwm_ssc_data *priv = platform_get_drvdata(pdev);

	pwm_ssc_cleanup(priv);

	return 0;
}

static const struct of_device_id of_pwm_ssc_match[] = {
	{ .compatible = "develed,pwm-ssc" },
	{},
};
MODULE_DEVICE_TABLE(of, of_pwm_ssc_match);

static struct platform_driver pwm_ssc_driver = {
	.probe		= pwm_ssc_probe,
	.remove		= pwm_ssc_remove,
	.driver		= {
		.name	= "pwm-ssc",
		.owner	= THIS_MODULE,
		.of_match_table = of_pwm_ssc_match,
	},
};

module_platform_driver(pwm_ssc_driver);

MODULE_AUTHOR("Pietro Lorefice <pietro@develer.com>");
MODULE_DESCRIPTION("PWM-generated signals for SSC peripheral");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:pwm-ssc");
