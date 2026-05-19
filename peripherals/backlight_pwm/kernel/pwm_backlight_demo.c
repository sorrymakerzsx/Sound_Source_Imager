// SPDX-License-Identifier: GPL-2.0
/*
 * 简化版 PWM 背光驱动示例。
 *
 * 说明：
 * ATK RK3588 板卡已经可以直接复用内核通用 pwm-backlight 驱动；
 */

#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/slab.h>

struct zsx_pwm_bl {
    struct pwm_device *pwm;
    struct backlight_device *bl_dev;
    struct gpio_desc *enable_gpio;
    u32 max_brightness;
    u32 period_ns;
};

static int zsx_pwm_bl_update_status(struct backlight_device *bl)
{
    struct zsx_pwm_bl *ctx = bl_get_data(bl);
    struct pwm_state state;
    unsigned int brightness = bl->props.brightness;

    pwm_get_state(ctx->pwm, &state);
    state.period = ctx->period_ns;
    state.enabled = brightness > 0;
    state.duty_cycle = (u64)brightness * ctx->period_ns / ctx->max_brightness;
    pwm_apply_state(ctx->pwm, &state);

    if (ctx->enable_gpio)
        gpiod_set_value_cansleep(ctx->enable_gpio, state.enabled ? 1 : 0);
    return 0;
}

static const struct backlight_ops zsx_pwm_bl_ops = {
    .update_status = zsx_pwm_bl_update_status,
};

static int zsx_pwm_bl_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct zsx_pwm_bl *ctx;
    struct backlight_properties props;
    u32 default_brightness = 128;
    int ret;

    ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    ctx->pwm = devm_pwm_get(dev, NULL);
    if (IS_ERR(ctx->pwm))
        return PTR_ERR(ctx->pwm);

    ctx->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->enable_gpio))
        return PTR_ERR(ctx->enable_gpio);

    ctx->max_brightness = 255;
    ctx->period_ns = 25000;
    of_property_read_u32(dev->of_node, "max-brightness", &ctx->max_brightness);
    of_property_read_u32(dev->of_node, "default-brightness-level", &default_brightness);
    of_property_read_u32(dev->of_node, "pwm-period-ns", &ctx->period_ns);

    memset(&props, 0, sizeof(props));
    props.type = BACKLIGHT_RAW;
    props.max_brightness = ctx->max_brightness;

    ctx->bl_dev = devm_backlight_device_register(dev, dev_name(dev), dev, ctx,
                                                 &zsx_pwm_bl_ops, &props);
    if (IS_ERR(ctx->bl_dev))
        return PTR_ERR(ctx->bl_dev);

    ctx->bl_dev->props.brightness = min_t(u32, default_brightness, ctx->max_brightness);
    platform_set_drvdata(pdev, ctx);

    ret = backlight_update_status(ctx->bl_dev);
    if (ret)
        dev_err(dev, "failed to apply initial backlight state: %d\n", ret);
    return ret;
}

static const struct of_device_id zsx_pwm_bl_of_match[] = {
    { .compatible = "zsx,pwm-backlight-demo" },
    { }
};
MODULE_DEVICE_TABLE(of, zsx_pwm_bl_of_match);

static struct platform_driver zsx_pwm_bl_driver = {
    .probe = zsx_pwm_bl_probe,
    .driver = {
        .name = "zsx-pwm-backlight-demo",
        .of_match_table = zsx_pwm_bl_of_match,
    },
};
module_platform_driver(zsx_pwm_bl_driver);

MODULE_DESCRIPTION("ZSX PWM backlight demo driver");
MODULE_LICENSE("GPL");
