/*
 * DRM driver for Multi-Inno MI0283QT panels
 *
 * Copyright 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include <drm/drm_fb_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/tinydrm/mipi-dbi.h>
#include <drm/tinydrm/tinydrm-helpers.h>
#include <video/mipi_display.h>

#define ST77XX_MADCTL_MY  0x80
#define ST77XX_MADCTL_MX  0x40
#define ST77XX_MADCTL_MV  0x20
#define ST77XX_MADCTL_ML  0x10
#define ST77XX_MADCTL_BGR 0x08
#define ST77XX_MADCTL_RGB 0x00

static void st7789vada_enable(struct drm_simple_display_pipe *pipe,
			    struct drm_crtc_state *crtc_state,
			    struct drm_plane_state *plane_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);
	u8 addr_mode;
	int ret;

   printk(KERN_INFO "Hello, world.\n");
	DRM_DEBUG_KMS("\n");

	ret = mipi_dbi_poweron_conditional_reset(mipi);
	if (ret < 0)
		return;
	if (ret == 1)
		goto out_enable;

	mipi_dbi_command(mipi, MIPI_DCS_SET_DISPLAY_OFF);

	mipi_dbi_command(mipi, MIPI_DCS_SOFT_RESET);
	msleep(150);
	mipi_dbi_command(mipi, MIPI_DCS_EXIT_SLEEP_MODE);
	msleep(10);
	mipi_dbi_command(mipi, MIPI_DCS_SET_PIXEL_FORMAT, 0x55); // 16 bit color
	msleep(10);
	mipi_dbi_command(mipi, MIPI_DCS_SET_ADDRESS_MODE, 0);
	mipi_dbi_command(mipi, MIPI_DCS_SET_COLUMN_ADDRESS, 0, 0, 0, 240);
	mipi_dbi_command(mipi, MIPI_DCS_SET_PAGE_ADDRESS, 0, 0, 320>>8, 320&0xFF);
	mipi_dbi_command(mipi, MIPI_DCS_ENTER_INVERT_MODE); // odd hack, displays are inverted
	mipi_dbi_command(mipi, MIPI_DCS_ENTER_NORMAL_MODE);
	msleep(10);
	mipi_dbi_command(mipi, MIPI_DCS_SET_DISPLAY_ON);
	msleep(10);

out_enable:
	/* The PiTFT (ili9340) has a hardware reset circuit that
	 * resets only on power-on and not on each reboot through
	 * a gpio like the rpi-display does.
	 * As a result, we need to always apply the rotation value
	 * regardless of the display "on/off" state.
	 */
	switch (mipi->rotation) {
	default:
		addr_mode = 0;
		break;
	case 90:
		addr_mode = ST77XX_MADCTL_MV;
		break;
	case 180:
		addr_mode = ST77XX_MADCTL_MY;
		break;
	case 270:
		addr_mode = ST77XX_MADCTL_MV | ST77XX_MADCTL_MY |
			    ST77XX_MADCTL_MX;
		break;
	}
	mipi_dbi_command(mipi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);

	mipi_dbi_command(mipi, MIPI_DCS_SET_DISPLAY_ON);

	mipi_dbi_enable_flush(mipi, crtc_state, plane_state);

	backlight_enable(mipi->backlight);
}

static const struct drm_simple_display_pipe_funcs st7789vada_pipe_funcs = {
	.enable = st7789vada_enable,
	.disable = mipi_dbi_pipe_disable,
	.update = tinydrm_display_pipe_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

static const struct drm_display_mode st7789vada_mode = {
	TINYDRM_MODE(240, 240, 58, 43),
};

DEFINE_DRM_GEM_CMA_FOPS(st7789vada_fops);

static struct drm_driver st7789vada_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
				  DRIVER_ATOMIC,
	.fops			= &st7789vada_fops,
	TINYDRM_GEM_DRIVER_OPS,
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "mi0283qt",
	.desc			= "Multi-Inno MI0283QT",
	.date			= "20160614",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id st7789vada_of_match[] = {
	{ .compatible = "multi-inno,mi0283qt" },
	{},
};
MODULE_DEVICE_TABLE(of, st7789vada_of_match);

static const struct spi_device_id st7789vada_id[] = {
	{ "mi0283qt", 0 },
	{ },
};
MODULE_DEVICE_TABLE(spi, st7789vada_id);

static int st7789vada_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct mipi_dbi *mipi;
	struct gpio_desc *dc;
	u32 rotation = 0;
	int ret;

	mipi = devm_kzalloc(dev, sizeof(*mipi), GFP_KERNEL);
	if (!mipi)
		return -ENOMEM;

	mipi->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(mipi->reset)) {
		DRM_DEV_ERROR(dev, "Failed to get gpio 'reset'\n");
		return PTR_ERR(mipi->reset);
	}

	dc = devm_gpiod_get_optional(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc)) {
		DRM_DEV_ERROR(dev, "Failed to get gpio 'dc'\n");
		return PTR_ERR(dc);
	}

	mipi->regulator = devm_regulator_get(dev, "power");
	if (IS_ERR(mipi->regulator))
		return PTR_ERR(mipi->regulator);

	mipi->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(mipi->backlight))
		return PTR_ERR(mipi->backlight);

	device_property_read_u32(dev, "rotation", &rotation);

	ret = mipi_dbi_spi_init(spi, mipi, dc);
	if (ret)
		return ret;

	ret = mipi_dbi_init(&spi->dev, mipi, &st7789vada_pipe_funcs,
			    &st7789vada_driver, &st7789vada_mode, rotation);
	if (ret)
		return ret;

	spi_set_drvdata(spi, mipi);

	return devm_tinydrm_register(&mipi->tinydrm);
}

static void st7789vada_shutdown(struct spi_device *spi)
{
	struct mipi_dbi *mipi = spi_get_drvdata(spi);

	tinydrm_shutdown(&mipi->tinydrm);
}

static int __maybe_unused st7789vada_pm_suspend(struct device *dev)
{
	struct mipi_dbi *mipi = dev_get_drvdata(dev);

	return drm_mode_config_helper_suspend(mipi->tinydrm.drm);
}

static int __maybe_unused st7789vada_pm_resume(struct device *dev)
{
	struct mipi_dbi *mipi = dev_get_drvdata(dev);

	drm_mode_config_helper_resume(mipi->tinydrm.drm);

	return 0;
}

static const struct dev_pm_ops st7789vada_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(st7789vada_pm_suspend, st7789vada_pm_resume)
};

static struct spi_driver st7789vada_spi_driver = {
	.driver = {
		.name = "mi0283qt",
		.owner = THIS_MODULE,
		.of_match_table = st7789vada_of_match,
		.pm = &st7789vada_pm_ops,
	},
	.id_table = st7789vada_id,
	.probe = st7789vada_probe,
	.shutdown = st7789vada_shutdown,
};
module_spi_driver(st7789vada_spi_driver);

MODULE_DESCRIPTION("Multi-Inno MI0283QT DRM driver");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_LICENSE("GPL");
