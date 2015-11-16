/*
 *  Setup code for SAMA5 Evaluation Kits with Device Tree support
 *
 *  Copyright (C) 2013 Atmel,
 *                2013 Ludovic Desroches <ludovic.desroches@atmel.com>
 *
 * Licensed under GPLv2 or later.
 */

#include <linux/types.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/micrel_phy.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/clk-provider.h>
#include <linux/phy.h>
#include <linux/delay.h>
#include <linux/wl12xx.h>

#include <asm/setup.h>
#include <asm/irq.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/irq.h>

#include "generic.h"

static int ksz8081_phy_fixup(struct phy_device *phy)
{
	int value;

	value = phy_read(phy, 0x16);
	value &= ~0x20;
	phy_write(phy, 0x16, value);

	return 0;
}

static void __init sama5_dt_device_init(void)
{
	if (of_machine_is_compatible("atmel,sama5d4ek") &&
	   IS_ENABLED(CONFIG_PHYLIB)) {
		phy_register_fixup_for_id("fc028000.etherne:00",
						ksz8081_phy_fixup);
	}

	if (of_machine_is_compatible("atmel,sama5d3xmb") &&
	   IS_ENABLED(CONFIG_PHYLIB)) {
		phy_register_fixup_for_uid(PHY_ID_KSZ8051, MICREL_PHY_ID_MASK,
						ksz8081_phy_fixup);
	}

	of_platform_populate(NULL, of_default_bus_match_table, NULL, NULL);
	sam5d3_pm_init();
}

static struct wl12xx_platform_data sama5d4_wl12xx_wlan_data __initdata = {
	.irq			= -1,
	.wlan_pwr.gpio	= -1,
	.wlan_en.gpio	= -1,
};

static int wl18xx_set_gpio_output(struct wl12xx_gpio_data *pin, int value)
{
	int ret;

	if (!gpio_is_valid(pin->gpio)) {
		pr_err("wlan gpio %d is not valid\n", pin->gpio);
		return -ENODEV;
	}

	if (value)
		value = !!!pin->alow;
	else
		value = !!pin->alow;

	ret = gpio_direction_output(pin->gpio, value);
	if (ret < 0) {
		pr_err("wlan gpio %d output error: %d\n", pin->gpio, ret);
		return ret;
	}

	return 0;
}

static int wl18xx_set_power(int power_on)
{
	struct wl12xx_platform_data *pdata = wl12xx_get_platform_data();
	int ret;

	pr_debug("wl18xx power %s\n", power_on ? "on" : "off");

	if (IS_ERR(pdata)) {
		ret = PTR_ERR(pdata);
		pr_err("wlan platform data is missing: %d\n", ret);
		return ret;
	}

	if (power_on) {
		/* Power up sequence */
		ret = wl18xx_set_gpio_output(&pdata->wlan_pwr, 1);
		if (ret < 0)
			return ret;

		udelay(120);

		ret = wl18xx_set_gpio_output(&pdata->wlan_en, 1);
		if (ret < 0)
			return ret;

	} else {
		/* Power down sequence */
		ret = wl18xx_set_gpio_output(&pdata->wlan_en, 0);
		if (ret < 0)
			return ret;

		udelay(120);

		ret = wl18xx_set_gpio_output(&pdata->wlan_pwr, 0);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static void __init wl18xx_get_of_gpio(struct device_node *np, const char *of_name,
	struct wl12xx_gpio_data *data)
{
	int gpio_pin;
	enum of_gpio_flags gpio_flags;

	gpio_pin = of_get_named_gpio_flags(np, of_name, 0, &gpio_flags);
	if (gpio_is_valid(gpio_pin)) {
		if (gpio_request(gpio_pin, of_name) >= 0) {
			data->gpio = gpio_pin;
			data->alow = gpio_flags & OF_GPIO_ACTIVE_LOW;
		} else {
			pr_err("wlan gpio %s (%d) request failed\n", of_name, gpio_pin);
		}
	} else {
		pr_err("wlan gpio %s is not valid: %d\n", of_name, gpio_pin);
	}
}

static void __init wl18xx_get_of_irq(struct device_node *np, struct wl12xx_platform_data *data)
{
	int irq;

	irq = irq_of_parse_and_map(np, 0);
	if (irq >= 0)
		data->irq = irq;
	else
		pr_err("wlan irq map failed: %d\n", irq);
}

static void __init sama5_dt_device_init_late(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "ti,wilink8");
	if (np) {
		if (of_device_is_available(np)) {
			/* Extract wlcore properties form device tree */
			wl18xx_get_of_gpio(np, "wlan-pwr-gpio", &sama5d4_wl12xx_wlan_data.wlan_pwr);
			wl18xx_get_of_gpio(np, "wlan-en-gpio", &sama5d4_wl12xx_wlan_data.wlan_en);
			wl18xx_get_of_irq(np, &sama5d4_wl12xx_wlan_data);
			sama5d4_wl12xx_wlan_data.set_power = wl18xx_set_power;

			pr_debug("wl18xx wlan-pwr-gpio=%d, wlan-en-gpio=%d, wlan-irq=%d\n",
				sama5d4_wl12xx_wlan_data.wlan_pwr.gpio,
				sama5d4_wl12xx_wlan_data.wlan_en.gpio,
				sama5d4_wl12xx_wlan_data.irq);

			/* Set legacy platform data */
			if (wl12xx_set_platform_data(&sama5d4_wl12xx_wlan_data))
				pr_err("can't set wl12xx platform data\n");

			/* Perform power up sequence */
			wl18xx_set_power(0);
			wl18xx_set_power(1);
		}
	}
	of_node_put(np);
}

static const char *sama5_dt_board_compat[] __initconst = {
	"atmel,sama5",
	NULL
};

DT_MACHINE_START(sama5_dt, "Atmel SAMA5")
	/* Maintainer: Atmel */
	.map_io		= at91_map_io,
	.init_early	= at91_dt_initialize,
	.init_machine	= sama5_dt_device_init,
	.dt_compat	= sama5_dt_board_compat,
MACHINE_END

static void __init sama5d4_dt_device_init(void)
{
	of_platform_populate(NULL, of_default_bus_match_table, NULL, NULL);
	sam5d4_pm_init();
}

static const char *sama5_alt_dt_board_compat[] __initconst = {
	"atmel,sama5d4",
	NULL
};

DT_MACHINE_START(sama5_alt_dt, "Atmel SAMA5")
	/* Maintainer: Atmel */
	.map_io		= at91_alt_map_io,
	.init_early	= at91_dt_initialize,
	.init_machine	= sama5d4_dt_device_init,
	.dt_compat	= sama5_alt_dt_board_compat,
	.l2c_aux_mask	= ~0UL,
	.init_late = sama5_dt_device_init_late,
MACHINE_END
