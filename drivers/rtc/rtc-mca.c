/* rtc-mca.c - Real time clock device driver for MCA on ConnectCore modules
 * Copyright (C) 2016 - 2018  Digi International
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mfd/mca-common/core.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>

#define MCA_DRVNAME_RTC		"mca-rtc"

#define CLOCK_DATA_LEN	(MCA_RTC_COUNT_SEC - MCA_RTC_COUNT_YEAR_L + 1)
#define ALARM_DATA_LEN	(MCA_RTC_ALARM_SEC - MCA_RTC_ALARM_YEAR_L + 1)

#ifdef CONFIG_OF
enum mca_rtc_type {
	CC6UL_MCA_RTC,
	CC8X_MCA_RTC,
};

struct mca_rtc_data {
	enum mca_rtc_type devtype;
};
#endif

struct mca_rtc {
	struct rtc_device *rtc_dev;
	struct mca_drv *mca;
	int irq_alarm;
	bool alarm_enabled;
};

enum {
	DATA_YEAR_L,
	DATA_YEAR_H,
	DATA_MONTH,
	DATA_DAY,
	DATA_HOUR,
	DATA_MIN,
	DATA_SEC,
};

static void mca_data_to_tm(u8 *data, struct rtc_time *tm)
{
	/* conversion from MCA RTC to struct time is month-1 and year-1900 */
	tm->tm_year = (((data[DATA_YEAR_H] &
			 MCA_RTC_YEAR_H_MASK) << 8) |
		       (data[DATA_YEAR_L] & MCA_RTC_YEAR_L_MASK)) -
		       1900;
	tm->tm_mon  = (data[DATA_MONTH] & MCA_RTC_MONTH_MASK) - 1;
	tm->tm_mday = (data[DATA_DAY]   & MCA_RTC_DAY_MASK);
	tm->tm_hour = (data[DATA_HOUR]  & MCA_RTC_HOUR_MASK);
	tm->tm_min  = (data[DATA_MIN]   & MCA_RTC_MIN_MASK);
	tm->tm_sec  = (data[DATA_SEC]   & MCA_RTC_SEC_MASK);
}

static void mca_tm_to_data(struct rtc_time *tm, u8 *data)
{
	/* conversion from struct time to MCA RTC is year+1900 */
	data[DATA_YEAR_L]  &= (u8)~MCA_RTC_YEAR_L_MASK;
	data[DATA_YEAR_H]  &= (u8)~MCA_RTC_YEAR_H_MASK;
	data[DATA_YEAR_L]  |= (tm->tm_year + 1900) &
			      MCA_RTC_YEAR_L_MASK;
	data[DATA_YEAR_H]  |= ((tm->tm_year + 1900) >> 8) &
			       MCA_RTC_YEAR_H_MASK;

	/* conversion from struct time to MCA RTC is month+1 */
	data[DATA_MONTH] &= ~MCA_RTC_MONTH_MASK;
	data[DATA_MONTH] |= (tm->tm_mon + 1) & MCA_RTC_MONTH_MASK;

	data[DATA_DAY]   &= ~MCA_RTC_DAY_MASK;
	data[DATA_DAY]   |= tm->tm_mday & MCA_RTC_DAY_MASK;

	data[DATA_HOUR]  &= ~MCA_RTC_HOUR_MASK;
	data[DATA_HOUR]  |= tm->tm_hour & MCA_RTC_HOUR_MASK;

	data[DATA_MIN]   &= ~MCA_RTC_MIN_MASK;
	data[DATA_MIN]   |= tm->tm_min & MCA_RTC_MIN_MASK;

	data[DATA_SEC]   &= ~MCA_RTC_SEC_MASK;
	data[DATA_SEC]   |= tm->tm_sec & MCA_RTC_SEC_MASK;
}

static int mca_rtc_stop_alarm(struct device *dev)
{
	struct mca_rtc *rtc = dev_get_drvdata(dev);

	return regmap_update_bits(rtc->mca->regmap, MCA_RTC_CONTROL,
				  MCA_RTC_ALARM_EN, 0);
}

static int mca_rtc_start_alarm(struct device *dev)
{
	struct mca_rtc *rtc = dev_get_drvdata(dev);

	return regmap_update_bits(rtc->mca->regmap, MCA_RTC_CONTROL,
				  MCA_RTC_ALARM_EN,
				  MCA_RTC_ALARM_EN);
}

static int mca_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct mca_rtc *rtc = dev_get_drvdata(dev);
	u8 data[CLOCK_DATA_LEN] = { [0 ... (CLOCK_DATA_LEN - 1)] = 0 };
	int ret;

	ret = regmap_bulk_read(rtc->mca->regmap, MCA_RTC_COUNT_YEAR_L,
			       data, CLOCK_DATA_LEN);
	if (ret < 0) {
		dev_err(dev, "Failed to read RTC time data: %d\n", ret);
		return ret;
	}

	mca_data_to_tm(data, tm);
	return rtc_valid_tm(tm);
}

static int mca_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct mca_rtc *rtc = dev_get_drvdata(dev);
	u8 data[CLOCK_DATA_LEN] = { [0 ... (CLOCK_DATA_LEN - 1)] = 0 };
	int ret;

	mca_tm_to_data(tm, data);

	ret = regmap_bulk_write(rtc->mca->regmap, MCA_RTC_COUNT_YEAR_L,
				data, CLOCK_DATA_LEN);
	if (ret < 0) {
		dev_err(dev, "Failed to set RTC time data: %d\n", ret);
		return ret;
	}

	return ret;
}

/*
 * The MCA RTC alarm expires (triggers the irq) when the RTC time matches the
 * value programmed in the alarm register and the RTC counter increments.
 * This means, one second after the programmed value. To correct this, the
 * alarm value is adjusted when it is being written/read, decrementing/incremen-
 * ting the value by 1 second.
 */
static void mca_rtc_adjust_alarm_time(struct rtc_wkalrm *alrm, bool inc)
{
	unsigned long time;

	rtc_tm_to_time(&alrm->time, &time);
	time = inc ? time + 1 : time - 1;
	rtc_time_to_tm(time, &alrm->time);
}

static int mca_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct mca_rtc *rtc = dev_get_drvdata(dev);
	u8 data[CLOCK_DATA_LEN] = { [0 ... (CLOCK_DATA_LEN - 1)] = 0 };
	int ret;
	unsigned int val;

	ret = regmap_bulk_read(rtc->mca->regmap, MCA_RTC_ALARM_YEAR_L,
			       data, ALARM_DATA_LEN);
	if (ret < 0)
		return ret;

	mca_data_to_tm(data, &alrm->time);
	mca_rtc_adjust_alarm_time(alrm, true);

	/* Enable status */
	ret = regmap_read(rtc->mca->regmap, MCA_RTC_CONTROL, &val);
	if (ret < 0)
		return ret;

	/* Pending status */
	ret = regmap_read(rtc->mca->regmap, MCA_IRQ_STATUS_0, &val);
	if (ret < 0)
		return ret;
	alrm->pending = (val & MCA_RTC_ALARM) ? 1 : 0;

	return 0;
}

static int mca_rtc_alarm_irq_enable(struct device *dev,
					  unsigned int enabled)
{
	struct mca_rtc *rtc = dev_get_drvdata(dev);
	int ret;

	if (enabled) {
		ret = mca_rtc_start_alarm(dev);
		if (ret != 0) {
			dev_err(dev, "Failed to enable alarm IRQ (%d)\n", ret);
			goto exit_alarm_irq;
		}
		rtc->alarm_enabled = 1;
	} else {
		ret = mca_rtc_stop_alarm(dev);
		if (ret != 0) {
			dev_err(dev, "Failed to disable alarm IRQ (%d)\n", ret);
			goto exit_alarm_irq;
		}
		rtc->alarm_enabled = 0;
	}

exit_alarm_irq:
	return ret;
}

static int mca_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct mca_rtc *rtc = dev_get_drvdata(dev);
	u8 data[CLOCK_DATA_LEN] = { [0 ... (CLOCK_DATA_LEN - 1)] = 0 };
	int ret;

	mca_rtc_adjust_alarm_time(alrm, false);
	mca_tm_to_data(&alrm->time, data);

	ret = regmap_bulk_write(rtc->mca->regmap, MCA_RTC_ALARM_YEAR_L,
				data, ALARM_DATA_LEN);
	if (ret < 0)
		return ret;

	return mca_rtc_alarm_irq_enable(dev, alrm->enabled);
}

static irqreturn_t mca_alarm_event(int irq, void *data)
{
	struct mca_rtc *rtc = data;

	rtc_update_irq(rtc->rtc_dev, 1, RTC_IRQF | RTC_AF);

	return IRQ_HANDLED;
}

static const struct rtc_class_ops mca_rtc_ops = {
	.read_time = mca_rtc_read_time,
	.set_time = mca_rtc_set_time,
	.read_alarm = mca_rtc_read_alarm,
	.set_alarm = mca_rtc_set_alarm,
	.alarm_irq_enable = mca_rtc_alarm_irq_enable,
};

static int mca_rtc_probe(struct platform_device *pdev)
{
	struct mca_drv *mca = dev_get_drvdata(pdev->dev.parent);
	struct mca_rtc *rtc;
	const struct mca_rtc_data *devdata =
				   of_device_get_match_data(&pdev->dev);
	struct device_node *np = NULL;
	int ret = 0;

	if (!mca || !mca->dev->parent->of_node)
		return -EPROBE_DEFER;

	rtc = devm_kzalloc(&pdev->dev, sizeof *rtc, GFP_KERNEL);
	if (!rtc)
		return -ENOMEM;

	platform_set_drvdata(pdev, rtc);
	device_init_wakeup(&pdev->dev, 1);
	rtc->mca = mca;

	/* Find entry in device-tree */
	if (mca->dev->of_node) {
		const char * compatible = pdev->dev.driver->
				    of_match_table[devdata->devtype].compatible;

		/*
		 * Return silently if RTC node does not exist
		 * or if it is disabled
		 */
		np = of_find_compatible_node(mca->dev->of_node, NULL, compatible);
		if (!np) {
			ret = -ENODEV;
			goto err;
		}
		if (!of_device_is_available(np)) {
			ret = -ENODEV;
			goto err;
		}
	}

	/* Enable RTC hardware */
	ret = regmap_update_bits(mca->regmap, MCA_RTC_CONTROL,
				 MCA_RTC_EN, MCA_RTC_EN);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to enable RTC.\n");
		goto err;
	}

	/* Register RTC device */
	rtc->rtc_dev = rtc_device_register(dev_name(&pdev->dev), &pdev->dev,
					   &mca_rtc_ops, THIS_MODULE);
	if (IS_ERR(rtc->rtc_dev)) {
		dev_err(&pdev->dev, "Failed to register RTC device: %ld\n",
			PTR_ERR(rtc->rtc_dev));
		ret = PTR_ERR(rtc->rtc_dev);
		goto err;
	}

	/*
	 * Register interrupts. Complain on errors but let device
	 * to be registered at least for date/time.
	 */
	rtc->irq_alarm = platform_get_irq_byname(pdev,
						 MCA_IRQ_RTC_ALARM_NAME);
	ret = devm_request_threaded_irq(&pdev->dev, rtc->irq_alarm, NULL,
					mca_alarm_event,
					IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					MCA_IRQ_RTC_ALARM_NAME, rtc);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request %s IRQ. (%d)\n",
			MCA_IRQ_RTC_ALARM_NAME, rtc->irq_alarm);
		rtc->irq_alarm = -ENXIO;
	}

	return 0;

err:
	rtc = NULL;
	return ret;
}

static int mca_rtc_remove(struct platform_device *pdev)
{
	struct mca_rtc *rtc = platform_get_drvdata(pdev);

	if (rtc->irq_alarm >= 0)
		devm_free_irq(&pdev->dev, rtc->irq_alarm, rtc);

	rtc_device_unregister(rtc->rtc_dev);
	return 0;
}

#ifdef CONFIG_PM
static int mca_rtc_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mca_rtc *rtc = platform_get_drvdata(pdev);
	int ret;

	if (!device_may_wakeup(&pdev->dev) && rtc->alarm_enabled) {
		/* Disable the alarm irq to avoid unwanted wakeups */
		ret = mca_rtc_stop_alarm(&pdev->dev);
		if (ret < 0)
			dev_err(&pdev->dev, "Failed to disable RTC Alarm\n");
	}

	return 0;
}

static int mca_rtc_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mca_rtc *rtc = platform_get_drvdata(pdev);
	int ret;

	if (!device_may_wakeup(&pdev->dev) && rtc->alarm_enabled) {
		/* Enable the alarm irq, just in case it was disabled suspending */
		ret = mca_rtc_start_alarm(&pdev->dev);
		if (ret < 0)
			dev_err(&pdev->dev, "Failed to restart RTC Alarm\n");
	}

	return 0;
}

static const struct dev_pm_ops mca_rtc_pm_ops = {
	.suspend	= mca_rtc_suspend,
	.resume		= mca_rtc_resume,
	.poweroff	= mca_rtc_suspend,
};
#endif

#ifdef CONFIG_OF
static struct mca_rtc_data mca_rtc_devdata[] = {
	[CC6UL_MCA_RTC] = {
		.devtype = CC6UL_MCA_RTC,
	},
	[CC8X_MCA_RTC] = {
		.devtype = CC8X_MCA_RTC,
	},
};

static const struct of_device_id mca_rtc_dt_ids[] = {
	{ .compatible = "digi,mca-cc6ul-rtc",
	  .data = &mca_rtc_devdata[CC6UL_MCA_RTC]},
	{ .compatible = "digi,mca-cc8x-rtc",
	  .data = &mca_rtc_devdata[CC8X_MCA_RTC]},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mca_rtc_dt_ids);
#endif

static struct platform_driver mca_rtc_driver = {
	.probe		= mca_rtc_probe,
	.remove		= mca_rtc_remove,
	.driver		= {
		.name	= MCA_DRVNAME_RTC,
		.owner	= THIS_MODULE,
#ifdef CONFIG_PM
		.pm	= &mca_rtc_pm_ops,
#endif
#ifdef CONFIG_OF
		.of_match_table = mca_rtc_dt_ids,
#endif
	},
};

static int __init mca_rtc_init(void)
{
	return platform_driver_register(&mca_rtc_driver);
}
module_init(mca_rtc_init);

static void __exit mca_rtc_exit(void)
{
	platform_driver_unregister(&mca_rtc_driver);
}
module_exit(mca_rtc_exit);

/* Module information */
MODULE_AUTHOR("Digi International Inc.");
MODULE_DESCRIPTION("Real time clock device driver for MCA of ConnectCore Modules");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" MCA_DRVNAME_RTC);
