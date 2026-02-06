// SPDX-License-Identifier: GPL-2.0
/*
 * VeriSilicon Watchdog driver.
 *
 * Copyright (C) 2016 VeriSilicon, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/watchdog.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/miscdevice.h>
#include <linux/clocksource.h>
#include <linux/clockchips.h>
#include <linux/regmap.h>
#include <linux/reboot.h>

/* Register offset */
#define WDT_IDREV    (0x00)
#define WDT_CTRL     (0x10)
#define WDT_RESTART  (0x14)
#define WDT_WEN      (0x18)
#define WDT_ST       (0x1c)

/* Bit definitions */
#define WDT_ENABLE_BIT           0x00000001
#define WDT_CLKSEL_BIT           0x00000002
#define WDT_INTEN_BIT            0x00000004
#define WDT_RSTEN_BIT            0x00000008
#define WDT_INTTIME_BIT          0x00000070
#define WDT_RSTTIME_BIT          0x00000700

/* Register I/O operations */
#define WDT_REG_WRITE(_addr, _val)   writel(_val, (wdt->reg_base)+(_addr))
#define WDT_REG_READ(_addr)   (readl((wdt->reg_base)+(_addr)))

/* Watchdog parameters */
#define ATCWDT200_WP_NUM      0x5aa55aa5
#define ATCWDT200_RESTART_NUM 0xcafecafe

#define FSL_WATCHDOG_ATBOOT		    (0)
#define FSL_WATCHDOG_DEFAULT_TIME	(15)
#define FSL_WDT_NAME    "fsl-wdt"


static bool nowayout	= WATCHDOG_NOWAYOUT;
static int tmr_margin;
static int tmr_atboot	= FSL_WATCHDOG_ATBOOT;
static int soft_noboot;
static int debug = 1;

module_param(tmr_margin,  int, 0);
module_param(tmr_atboot,  int, 0);
module_param(nowayout,   bool, 0);
module_param(soft_noboot, int, 0);
module_param(debug,	  int, 0);

MODULE_PARM_DESC(tmr_margin,
		 "Watchdog tmr_margin in seconds. (default="
		 __MODULE_STRING(FSL_WATCHDOG_DEFAULT_TIME) ")");
MODULE_PARM_DESC(tmr_atboot,
		 "Watchdog is started at boot time if set to 1, default="
		 __MODULE_STRING(FSL_WATCHDOG_ATBOOT));
MODULE_PARM_DESC(nowayout,
		 "Watchdog cannot be stopped once started (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");
MODULE_PARM_DESC(soft_noboot,
		 "Watchdog action, set to 1 to ignore reboots, 0 to reboot (default 0)");
MODULE_PARM_DESC(debug, "Watchdog debug, set to >1 for debug (default 0)");

/* Watchdog device structure */
struct fsl_wdt {
	struct device               *dev;
	struct clk                  *clock;
	void __iomem                *reg_base;
	int                         irq;
	spinlock_t                  lock;
	struct notifier_block	    restart_handler;
	struct watchdog_device      wdt_device;
};

/* Watchdog stop register operations */
static void __fsl_wdt_stop(struct fsl_wdt *wdt)
{
	unsigned int wtcon;

	wtcon = WDT_REG_READ(WDT_CTRL);
	wtcon &= ~WDT_ENABLE_BIT;
	WDT_REG_WRITE(WDT_WEN, ATCWDT200_WP_NUM);
	WDT_REG_WRITE(WDT_CTRL, wtcon);
}

/* Feed watchdog */
static int fsl_wdt_keepalive(struct watchdog_device *wdd)
{
	unsigned int wtcon;
	struct fsl_wdt *wdt = watchdog_get_drvdata(wdd);

	spin_lock(&wdt->lock);
	__fsl_wdt_stop(wdt);

	wtcon = WDT_REG_READ(WDT_CTRL);

	wtcon &= ~(WDT_CLKSEL_BIT | WDT_RSTTIME_BIT);
	wtcon |= (WDT_ENABLE_BIT | WDT_INTEN_BIT | WDT_RSTEN_BIT | WDT_INTTIME_BIT);

	WDT_REG_WRITE(WDT_WEN, ATCWDT200_WP_NUM); /* write enable */
	WDT_REG_WRITE(WDT_RESTART, ATCWDT200_RESTART_NUM);

	WDT_REG_WRITE(WDT_WEN, ATCWDT200_WP_NUM); /* write enable */
	WDT_REG_WRITE(WDT_CTRL, wtcon);

	spin_unlock(&wdt->lock);

	return 0;
}

/* Start watchdog */
static int fsl_wdt_start(struct watchdog_device *wdd)
{
	unsigned int wtcon;
	struct fsl_wdt *wdt = watchdog_get_drvdata(wdd);

	spin_lock(&wdt->lock);

	__fsl_wdt_stop(wdt);

	wtcon = WDT_REG_READ(WDT_CTRL);

	wtcon &= ~(WDT_CLKSEL_BIT | WDT_RSTTIME_BIT);
	wtcon |= (WDT_ENABLE_BIT | WDT_INTEN_BIT | WDT_RSTEN_BIT | WDT_INTTIME_BIT);

	WDT_REG_WRITE(WDT_WEN, ATCWDT200_WP_NUM); /* write enable */
	WDT_REG_WRITE(WDT_RESTART, ATCWDT200_RESTART_NUM);

	WDT_REG_WRITE(WDT_WEN, ATCWDT200_WP_NUM); /* write enable */
	WDT_REG_WRITE(WDT_CTRL, wtcon);

	spin_unlock(&wdt->lock);

	return 0;
}

/* Stop watchdog */
static int fsl_wdt_stop(struct watchdog_device *wdd)
{
	struct fsl_wdt *wdt = watchdog_get_drvdata(wdd);

	spin_lock(&wdt->lock);
	__fsl_wdt_stop(wdt);
	spin_unlock(&wdt->lock);

	return 0;
}

/* Watchdog related structures */
#define OPTIONS (WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE)

static const struct watchdog_info fsl_wdt_ident = {
	.options          =     OPTIONS,
	.firmware_version =	0,
	.identity         =	FSL_WDT_NAME,
};

static struct watchdog_ops fsl_wdt_ops = {
	.owner = THIS_MODULE,
	.start = fsl_wdt_start,
	.stop = fsl_wdt_stop,
	.ping = fsl_wdt_keepalive,
};

static struct watchdog_device fsl_wdd = {
	.info = &fsl_wdt_ident,
	.ops = &fsl_wdt_ops,
	.timeout = FSL_WATCHDOG_DEFAULT_TIME,
};

/* Interrupt handler */
static irqreturn_t fsl_wdt_irq(int irqno, void *param)
{
	struct fsl_wdt *wdt = platform_get_drvdata(param);

	dev_info(wdt->dev, "watchdog timer expired (irq)\n");

	fsl_wdt_keepalive(&wdt->wdt_device);
	return IRQ_HANDLED;
}

/* Restart system (reboot) */
static int fsl_wdt_restart(struct notifier_block *this, unsigned long mode, void *cmd)
{
	unsigned int wtcon;
	struct fsl_wdt *wdt = container_of(this, struct fsl_wdt, restart_handler);

	spin_lock(&wdt->lock);

	__fsl_wdt_stop(wdt);

	wtcon = WDT_REG_READ(WDT_CTRL);

	wtcon &= ~(WDT_CLKSEL_BIT | WDT_RSTTIME_BIT);
	wtcon |= (WDT_ENABLE_BIT | WDT_INTEN_BIT | WDT_RSTEN_BIT | WDT_INTTIME_BIT);

	WDT_REG_WRITE(WDT_WEN, ATCWDT200_WP_NUM); /* write enable */
	WDT_REG_WRITE(WDT_RESTART, ATCWDT200_RESTART_NUM);

	WDT_REG_WRITE(WDT_WEN, ATCWDT200_WP_NUM); /* write enable */
	WDT_REG_WRITE(WDT_CTRL, wtcon);

	spin_unlock(&wdt->lock);

	mdelay(1000);

	return NOTIFY_DONE;
}

/* Probe when registered */
static int fsl_wdt_probe(struct platform_device *pdev)
{
	struct device_node *dn = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct fsl_wdt *wdt;
	static struct clock_event_device __percpu *wdt_evt;
	int ret;
	struct clk *wdt_clk;

	unsigned int wtcon;

	if (!dn) {
		dev_err(dev, "of_node is NULL.\n");
		return -ENODEV;
	}

	/* Memory allocate */
	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	wdt->dev = dev;
	spin_lock_init(&wdt->lock);
	wdt->wdt_device = fsl_wdd;

	/* Get irq */
	wdt->irq = platform_get_irq(pdev, 0);
	if (wdt->irq < 0) {
		dev_err(dev, "no irq resource specified\n");
		ret = -ENOENT;
		goto err;
	}

	/* Get reg_base */
	wdt->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wdt->reg_base)) {
		ret = PTR_ERR(wdt->reg_base);
		goto err;
	}

	/* Get clk */
	wdt_clk = devm_clk_get(&pdev->dev, NULL);
	if (!IS_ERR(wdt_clk)) {
		ret = clk_prepare_enable(wdt_clk);
		if (ret)
			goto err;
	} else {
		pr_warn("watchdog-timer: clk not found\n");
		ret = -EINVAL;
		goto err;
	}

	wdt->clock = wdt_clk;

	wdt_evt = alloc_percpu(struct clock_event_device);
	if (!wdt_evt) {
		pr_warn("watchdog-timer: can't allocate memory\n");
		ret = -ENOMEM;
		goto err;
	}

	/* Request irq */
	ret = devm_request_irq(&pdev->dev, wdt->irq, fsl_wdt_irq, 0,
			       dev_name(&pdev->dev), wdt);
	if (ret) {
		pr_warn("watchdog: can't register interrupt %d (%d)\n",
			wdt->irq, ret);
		goto err;
	}

	watchdog_set_drvdata(&wdt->wdt_device, wdt);
	platform_set_drvdata(pdev, wdt);

	/* Register restart handler */
	wdt->restart_handler.notifier_call = fsl_wdt_restart;
	wdt->restart_handler.priority = 128;
	ret = register_restart_handler(&wdt->restart_handler);
	if (ret)
		pr_err("cannot register restart handler, %d\n", ret);

	watchdog_register_device(&wdt->wdt_device);

	return 0;
err:
	return ret;
}

/* Remove watchdog */
static int fsl_wdt_remove(struct platform_device *dev)
{
	struct fsl_wdt *wdt = platform_get_drvdata(dev);

	watchdog_unregister_device(&wdt->wdt_device);

	clk_disable_unprepare(wdt->clock);
	wdt->clock = NULL;

	return 0;
}

/* Shutdown watchdog */
static void fsl_wdt_shutdown(struct platform_device *dev)
{
	struct fsl_wdt *wdt = platform_get_drvdata(dev);

	fsl_wdt_stop(&wdt->wdt_device);
}

#ifdef CONFIG_PM_SLEEP
static int fsl_wdt_suspend(struct device *dev)
{
	return 0;
}

static int fsl_wdt_resume(struct device *dev)
{
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(fsl_wdt_pm_ops, fsl_wdt_suspend, fsl_wdt_resume);

#ifdef CONFIG_OF
static const struct of_device_id fsl_wdt_match[] = {
	{ .compatible = "riscv,fsl-wdt" },
	{ },
};
MODULE_DEVICE_TABLE(of, fsl_wdt_match);
#endif

static struct platform_driver fsl_wdt_driver = {
	.probe		= fsl_wdt_probe,
	.remove		= fsl_wdt_remove,
	.shutdown	= fsl_wdt_shutdown,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= FSL_WDT_NAME,
		.pm	    = &fsl_wdt_pm_ops,
		.of_match_table = of_match_ptr(fsl_wdt_match),
	},
};

module_platform_driver(fsl_wdt_driver);

MODULE_AUTHOR("Liu Pengfei <pfliu@fislink.com>");
MODULE_DESCRIPTION("Fislink Watchdog Device Driver");
MODULE_LICENSE("GPL v2");
