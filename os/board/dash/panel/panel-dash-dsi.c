// SPDX-License-Identifier: GPL-2.0-only
/*
 * S3XY Dash panel: 480x1920 MIPI-DSI, 4 lanes, RGB888, video mode with sync pulses.
 * The mode is the one the stock unit runs (read from its TCON/DSI registers, our own numbers).
 * Init: power, an optional DCS list from the DT ("dash,init"), exit sleep, display on.
 * No DCS reads: a panel that never answers one (the bus turned round to it and not handed back) leaves the DSI
 * host's lane busy, and the video does not start until the next off/on, so a replacement panel that does not do
 * reads would stay black. (The unit's panel, while it answered, read 0x38 at DCS 0xDA.)
 *
 * dash,init: a byte list of records <len cmd args...>; len counts cmd + args; a record
 * <0xff ms> sleeps ms milliseconds instead. Example: [ 02 b0 04  ff 0a ] = write B0 04, wait 10 ms.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct dash_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *supply;
	struct gpio_desc *reset_gpio;
	const u8 *init;
	int init_len;
};

static inline struct dash_panel *to_dash_panel(struct drm_panel *panel)
{
	return container_of(panel, struct dash_panel, panel);
}

static void dash_panel_send_init(struct dash_panel *ctx, struct mipi_dsi_multi_context *dsi_ctx)
{
	int i = 0;

	while (i < ctx->init_len && !dsi_ctx->accum_err) {
		u8 n = ctx->init[i];

		if (n == 0xff && i + 1 < ctx->init_len) {
			mipi_dsi_msleep(dsi_ctx, ctx->init[i + 1]);
			i += 2;
			continue;
		}
		if (n == 0 || i + 1 + n > ctx->init_len) {
			dev_err(&ctx->dsi->dev, "dash,init: bad record at byte %d\n", i);
			dsi_ctx->accum_err = -EINVAL;
			break;
		}
		mipi_dsi_dcs_write_buffer_multi(dsi_ctx, &ctx->init[i + 1], n);
		i += 1 + n;
	}
}

static int dash_panel_prepare(struct drm_panel *panel)
{
	struct dash_panel *ctx = to_dash_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	int ret;

	ret = regulator_enable(ctx->supply);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "failed to enable power supply %d\n", ret);
		return ret;
	}
	msleep(20);

	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		usleep_range(10000, 11000);
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		msleep(120);
	}

	dash_panel_send_init(ctx, &dsi_ctx);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);

	if (dsi_ctx.accum_err)
		dev_err(&ctx->dsi->dev, "panel init failed: %d\n", dsi_ctx.accum_err);
	return dsi_ctx.accum_err;
}

static int dash_panel_unprepare(struct drm_panel *panel)
{
	struct dash_panel *ctx = to_dash_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	if (ctx->reset_gpio)
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_disable(ctx->supply);
	return 0;
}

/* Stock timings: HFP 50, HSA 20, HBP 50; VFP 16, VSA 5, VBP 55; 60 Hz */
static const struct drm_display_mode dash_panel_mode = {
	.clock = 600 * 1996 * 60 / 1000,
	.hdisplay = 480,
	.hsync_start = 480 + 50,
	.hsync_end = 480 + 50 + 20,
	.htotal = 480 + 50 + 20 + 50,
	.vdisplay = 1920,
	.vsync_start = 1920 + 16,
	.vsync_end = 1920 + 16 + 5,
	.vtotal = 1920 + 16 + 5 + 55,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int dash_panel_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &dash_panel_mode);
}

static const struct drm_panel_funcs dash_panel_funcs = {
	.prepare = dash_panel_prepare,
	.unprepare = dash_panel_unprepare,
	.get_modes = dash_panel_get_modes,
};

static int dash_panel_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct dash_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct dash_panel, panel, &dash_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(ctx->supply))
		return dev_err_probe(dev, PTR_ERR(ctx->supply), "Failed to get power regulator\n");

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio), "Failed to get reset-gpios\n");

	ctx->init = of_get_property(dev->of_node, "dash,init", &ctx->init_len);
	if (!ctx->init)
		ctx->init_len = 0;

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE | MIPI_DSI_MODE_LPM;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}
	return 0;
}

static void dash_panel_remove(struct mipi_dsi_device *dsi)
{
	struct dash_panel *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id dash_panel_of_match[] = {
	{ .compatible = "dash,s3xy-panel" },
	{ }
};
MODULE_DEVICE_TABLE(of, dash_panel_of_match);

static struct mipi_dsi_driver dash_panel_driver = {
	.driver = {
		.name = "panel-dash-dsi",
		.of_match_table = dash_panel_of_match,
	},
	.probe = dash_panel_probe,
	.remove = dash_panel_remove,
};
module_mipi_dsi_driver(dash_panel_driver);

MODULE_DESCRIPTION("S3XY Dash 480x1920 MIPI-DSI panel");
MODULE_LICENSE("GPL");
