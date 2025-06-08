#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <math.h>
#include <stdlib.h>

#include <zephyr/drivers/led_strip.h>
#include <drivers/ext_power.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/workqueue.h>
#include <zmk/keymap.h>
#include <zmk/split/bluetooth/peripheral.h>

#include <zmk_indicator_led_strip/indicator_led_strip.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_indicator_led_strip)
#error "A zmk,indicator-led-strip chosen node must be declared"
#endif

#define STRIP_CHOSEN DT_CHOSEN(zmk_indicator_led_strip)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

struct indicator_led_strip_state {
    bool on;
};

static const struct device *led_strip;
static struct led_rgb pixels[STRIP_NUM_PIXELS];
static struct indicator_led_strip_state state;

#if IS_ENABLED(CONFIG_INDICATOR_LED_STRIP_EXT_POWER)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

//// Timer setup for ticks

/* 4MHzで1bit ws2812の1bitはspiの8bitだから1usかかる計算
24*3+8(reset)で更新一回 80us なのでタイマー周期1msあたりから試してもいいかもね */
#define TIMER_PERIOD K_MSEC(16)

static void indicator_tick(struct k_work *work) {
    //
}

K_WORK_DEFINE(indicator_work, indicator_tick);

static void indicator_timer_cb(struct k_timer *timer) {
    if (!state.on) { return; }
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &indicator_work);
}

K_TIMER_DEFINE(indicator_timer, indicator_timer_cb, NULL);

//// Toggle

static int indicator_on(void) {
    if (!led_strip) return -ENODEV;

#if IS_ENABLED(CONFIG_INDICATOR_LED_STRIP_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_enable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to enable EXT_POWER: %d", rc);
        }
    }
#endif

    state.on = true;
    k_timer_start(&indicator_timer, K_NO_WAIT, TIMER_PERIOD);

    return 0;
}

static void indicator_off_task(struct k_work *work) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
    }
    led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
}
K_WORK_DEFINE(indicator_off_work, indicator_off_task);

static int indicator_off(void) {
    if (!led_strip) return -ENODEV;

#if IS_ENABLED(CONFIG_INDICATOR_LED_STRIP_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_disable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to disable EXT_POWER: %d", rc);
        }
    }
#endif

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &indicator_off_work);
    k_timer_stop(&indicator_timer);
    state.on = false;

    return 0;
}

int indicator_led_strip_toggle(void) {
    return state.on ? indicator_off() : indicator_on();
}

//// Initialization

static int indicator_init(void) {
    led_strip = DEVICE_DT_GET(STRIP_CHOSEN);

#if IS_ENABLED(CONFIG_INDICATOR_LED_STRIP_EXT_POWER)
    if (!device_is_ready(ext_power)) {
        LOG_ERR("ext power device \"%s\" is not ready", ext_power->name);
        return -ENODEV;
    }
#endif

    state = (struct indicator_led_strip_state){
        on : IS_ENABLED(CONFIG_INDICATOR_LED_STRIP_ON_START)
    };

    if (state.on) {
        indicator_on();
    }

    return 0;
}

SYS_INIT(indicator_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
