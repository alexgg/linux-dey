/*
 * Driver for the IMX SNVS ON/OFF Power Key
 * Copyright (C) 2015 Freescale Semiconductor, Inc. All Rights Reserved.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

#define SNVS_LPSR_REG	0x4C	/* LP Status Register */
#define SNVS_LPCR_REG	0x38	/* LP Control Register */
#define SNVS_HPSR_REG	0x14
#define SNVS_HPSR_BTN	BIT(6)
#define SNVS_LPSR_SPO	BIT(18)
#define SNVS_LPCR_DEP_EN BIT(5)

#define DEBOUNCE_TIME 30
#define REPEAT_INTERVAL 60

struct pwrkey_drv_data {
	struct regmap *snvs;
	int irq;
	int keycode;
	int keystate;  /* 1:pressed */
	int wakeup;
	struct timer_list check_timer;
	struct input_dev *input;
};

static void imx_imx_snvs_check_for_events(unsigned long data)
{
	struct pwrkey_drv_data *pdata = (struct pwrkey_drv_data *) data;
	struct input_dev *input = pdata->input;
	u32 state;

	regmap_read(pdata->snvs, SNVS_HPSR_REG, &state);
	state = state & SNVS_HPSR_BTN ? 1 : 0;

	/* only report new event if status changed */
	if (state ^ pdata->keystate) {
		pdata->keystate = state;
		input_event(input, EV_KEY, pdata->keycode, state);
		input_sync(input);
		pm_relax(pdata->input->dev.parent);
	}

	/* repeat check if pressed long */
	if (state) {
		mod_timer(&pdata->check_timer,
			  jiffies + msecs_to_jiffies(REPEAT_INTERVAL));
	}
}

static irqreturn_t imx_snvs_pwrkey_interrupt(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	struct pwrkey_drv_data *pdata = platform_get_drvdata(pdev);
	u32 lp_status;

	pm_wakeup_event(pdata->input->dev.parent, 0);

	regmap_read(pdata->snvs, SNVS_LPSR_REG, &lp_status);
	if (lp_status & SNVS_LPSR_SPO)
		mod_timer(&pdata->check_timer, jiffies + msecs_to_jiffies(DEBOUNCE_TIME));

	/* clear SPO status */
	regmap_write(pdata->snvs, SNVS_LPSR_REG, SNVS_LPSR_SPO);

	return IRQ_HANDLED;
}

static void imx_snvs_pwrkey_act(void *pdata)
{
	struct pwrkey_drv_data *pd = pdata;

	del_timer_sync(&pd->check_timer);
}

static int imx_snvs_pwrkey_probe(struct platform_device *pdev)
{
	struct pwrkey_drv_data *pdata = NULL;
	struct input_dev *input = NULL;
	struct device_node *np;
	int error;

	/* Get SNVS register Page */
	np = pdev->dev.of_node;
	if (!np)
		return -ENODEV;

	pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->snvs = syscon_regmap_lookup_by_phandle(np, "regmap");
	if (IS_ERR(pdata->snvs)) {
		dev_err(&pdev->dev, "Can't get snvs syscon\n");
		return PTR_ERR(pdata->snvs);
	}

	if (of_property_read_u32(np, "linux,keycode", &pdata->keycode)) {
		pdata->keycode = KEY_POWER;
		dev_warn(&pdev->dev, "KEY_POWER without setting in dts\n");
	}

	pdata->wakeup = of_property_read_bool(np, "wakeup-source");

	pdata->irq = platform_get_irq(pdev, 0);
	if (pdata->irq < 0) {
		dev_err(&pdev->dev, "no irq defined in platform data\n");
		return -EINVAL;
	}

	regmap_update_bits(pdata->snvs, SNVS_LPCR_REG, SNVS_LPCR_DEP_EN, SNVS_LPCR_DEP_EN);

	/* clear the unexpected interrupt before driver ready */
	regmap_write(pdata->snvs, SNVS_LPSR_REG, SNVS_LPSR_SPO);

	setup_timer(&pdata->check_timer,
		    imx_imx_snvs_check_for_events, (unsigned long) pdata);

	input = devm_input_allocate_device(&pdev->dev);
	if (!input) {
		dev_err(&pdev->dev, "failed to allocate the input device\n");
		return -ENOMEM;
	}

	input->name = pdev->name;
	input->phys = "snvs-pwrkey/input0";
	input->id.bustype = BUS_HOST;

	input_set_capability(input, EV_KEY, pdata->keycode);

	/* input customer action to cancel release timer */
	error = devm_add_action(&pdev->dev, imx_snvs_pwrkey_act, pdata);
	if (error) {
		dev_err(&pdev->dev, "failed to register remove action\n");
		return error;
	}

	pdata->input = input;
	platform_set_drvdata(pdev, pdata);

	error = devm_request_irq(&pdev->dev, pdata->irq,
			       imx_snvs_pwrkey_interrupt,
			       0, pdev->name, pdev);

	if (error) {
		dev_err(&pdev->dev, "interrupt not available.\n");
		return error;
	}

	error = input_register_device(input);
	if (error < 0) {
		dev_err(&pdev->dev, "failed to register input device\n");
		return error;
	}

	device_init_wakeup(&pdev->dev, pdata->wakeup);

	error = devm_request_irq(&pdev->dev, pdata->irq,
			       imx_snvs_pwrkey_interrupt,
			       0, pdev->name, pdev);

	if (error) {
		dev_err(&pdev->dev, "interrupt not available.\n");
		input_unregister_device(input);
		return error;
	}

	return 0;
}

static int __maybe_unused imx_snvs_pwrkey_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct pwrkey_drv_data *pdata = platform_get_drvdata(pdev);

	if (device_may_wakeup(&pdev->dev))
		enable_irq_wake(pdata->irq);

	return 0;
}

static int __maybe_unused imx_snvs_pwrkey_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct pwrkey_drv_data *pdata = platform_get_drvdata(pdev);

	if (device_may_wakeup(&pdev->dev))
		disable_irq_wake(pdata->irq);

	return 0;
}

static const struct of_device_id imx_snvs_pwrkey_ids[] = {
	{ .compatible = "fsl,sec-v4.0-pwrkey" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx_snvs_pwrkey_ids);

static SIMPLE_DEV_PM_OPS(imx_snvs_pwrkey_pm_ops, imx_snvs_pwrkey_suspend,
				imx_snvs_pwrkey_resume);

static struct platform_driver imx_snvs_pwrkey_driver = {
	.driver = {
		.name = "snvs_pwrkey",
		.pm     = &imx_snvs_pwrkey_pm_ops,
		.of_match_table = imx_snvs_pwrkey_ids,
	},
	.probe = imx_snvs_pwrkey_probe,
};
module_platform_driver(imx_snvs_pwrkey_driver);

MODULE_AUTHOR("Freescale Semiconductor");
MODULE_DESCRIPTION("i.MX snvs power key Driver");
MODULE_LICENSE("GPL");
