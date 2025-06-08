#define DT_DRV_COMPAT zmk_behavior_indicator_led_strip

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>

#include <zmk_indicator_led_strip/indicator_led_strip.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct behavior_config {
    bool toggle;
};

static int behavior_init(const struct device *dev) { return 0; }

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
#if IS_ENABLED(CONFIG_INDICATOR_LED_STRIP)
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_config *cfg = dev->config;

    if (cfg->toggle) {
        indicator_led_strip_toggle();
    }

#endif // IS_ENABLED(CONFIG_INDICATOR_LED_STRIP)

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_indicator_led_strip_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

#define INST_DEFINE(n)                                     \
    static struct behavior_config behavior_config_##n = {   \
        .toggle = DT_INST_PROP(n, toggle)                   \
    };                                                      \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_init, NULL, NULL, &behavior_config_##n,    \
                        POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,          \
                        &behavior_indicator_led_strip_driver_api);

DT_INST_FOREACH_STATUS_OKAY(INST_DEFINE)
