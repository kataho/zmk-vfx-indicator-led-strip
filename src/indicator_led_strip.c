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
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/workqueue.h>
#include <zmk/keymap.h>
#include <zmk/split/bluetooth/peripheral.h>

#include <zmk_indicator_led_strip/indicator_led_strip.h>

#define BASE_BRIGHTNESS_R 0.006f
#define BASE_BRIGHTNESS_G 0.005f
#define BASE_BRIGHTNESS_B 0.0064f

#define POWER_INDEX 0
#define BLUET_INDEX 1
#define LAYER_INDEX 2

#define POWER_COLOR_FULL   ((RGB){r: 0,   g: 1.0, b: 0.3})
#define POWER_COLOR_HALF   ((RGB){r: 0,   g: 1.0, b: 0})
#define POWER_COLOR_EMPTY  ((RGB){r: 1.0, g: 0,   b: 0})

#define BLUET_COLOR_ADVERT ((RGB){r: 0, g: 0, b: 1.0})
#define BLUET_COLOR_DISCON ((RGB){r: 0, g: 0, b: 0})
#define BLUET_COLOR_P0 ((RGB){r: 1.0, g: 0.3, b: 0})
#define BLUET_COLOR_P1 ((RGB){r: 0,   g: 0.8, b: 1.0})
#define BLUET_COLOR_P2 ((RGB){r: 0.8, g: 0.8, b: 0})
#define BLUET_COLOR_DEFAULT ((RGB){r: 1.0, g: 1.0, b: 1.0})

#define LAYER_COLOR_L0 ((RGB){r: 1.0, g: 0,   b: 0})
#define LAYER_COLOR_L1 ((RGB){r: 0.4, g: 0,   b: 1.0})
#define LAYER_COLOR_L2 ((RGB){r: 0,   g: 1.0, b: 0})
#define LAYER_COLOR_L3 ((RGB){r: 0,   g: 0.4, b: 1.0})
#define LAYER_COLOR_NULL ((RGB){r: 0, g: 0, b: 0})

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_indicator_led_strip)
#error "A zmk,indicator-led-strip chosen node must be declared"
#endif

#define STRIP_CHOSEN DT_CHOSEN(zmk_indicator_led_strip)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

#ifdef POWER_INDEX
#if POWER_INDEX >= STRIP_NUM_PIXELS
#error "POWER_INDEX out of strip length"
#endif
#endif

#ifdef BLUET_INDEX
#if BLUET_INDEX >= STRIP_NUM_PIXELS
#error "BLUET_INDEX out of strip length"
#endif
#endif

#ifdef LAYER_INDEX
#if LAYER_INDEX >= STRIP_NUM_PIXELS
#error "LAYER_INDEX out of strip length"
#endif
#endif

enum indicator_mode {
    OFF = 0,
    SOLID,
    BREATHE
};

struct indicator_led_strip_state {
    enum indicator_mode mode;
};

typedef struct {
    float r;
    float g;
    float b;
} RGB;

static const struct device *led_strip;
static RGB indicators[STRIP_NUM_PIXELS];
static RGB fractions[STRIP_NUM_PIXELS];
static struct indicator_led_strip_state state;

#if IS_ENABLED(CONFIG_INDICATOR_LED_STRIP_EXT_POWER)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

//// Timer setup for tick

static struct led_rgb convert_to_led_rgb(const RGB *const rgb, RGB *frac) {
    RGB multiplied = (RGB){
        r: rgb->r * 255.0f * BASE_BRIGHTNESS_R + frac->r,
        g: rgb->g * 255.0f * BASE_BRIGHTNESS_G + frac->g,
        b: rgb->b * 255.0f * BASE_BRIGHTNESS_B + frac->b
    };
    RGB real_ret = (RGB){
        r: floorf(multiplied.r),
        g: floorf(multiplied.g),
        b: floorf(multiplied.b)
    };
    frac->r = multiplied.r - real_ret.r;
    frac->g = multiplied.g - real_ret.g;
    frac->b = multiplied.b - real_ret.b;
    return (struct led_rgb){
        r: (uint8_t)real_ret.r,
        g: (uint8_t)real_ret.g,
        b: (uint8_t)real_ret.b
    };
}

static void apply_breathe_effect(const RGB *const rgb, RGB *out_rgb) {
    static uint32_t counter = 0;
    const uint32_t freq = 20000;
    const float M_PI_2 = 6.2831853f;
    float breathe_rate = (sinf(M_PI_2 * ((float)(counter % freq) / (float)freq)) + 1.1f) * 0.45f ;
    out_rgb->r = rgb->r * breathe_rate;
    out_rgb->g = rgb->g * breathe_rate;
    out_rgb->b = rgb->b * breathe_rate;
    counter++;
}

static void indicator_tick(struct k_work *work) {
    RGB *target = indicators;
    RGB out_rgb[STRIP_NUM_PIXELS];
    if (state.mode == BREATHE) {
        apply_breathe_effect(&indicators[POWER_INDEX], &out_rgb[POWER_INDEX]);
        apply_breathe_effect(&indicators[BLUET_INDEX], &out_rgb[BLUET_INDEX]);
        apply_breathe_effect(&indicators[LAYER_INDEX], &out_rgb[LAYER_INDEX]);
        target = out_rgb;
    }
    struct led_rgb pixels[STRIP_NUM_PIXELS];
    pixels[POWER_INDEX] = convert_to_led_rgb(&target[POWER_INDEX], &fractions[POWER_INDEX]);
    pixels[BLUET_INDEX] = convert_to_led_rgb(&target[BLUET_INDEX], &fractions[BLUET_INDEX]);
    pixels[LAYER_INDEX] = convert_to_led_rgb(&target[LAYER_INDEX], &fractions[LAYER_INDEX]);
    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }
}

K_WORK_DEFINE(indicator_work, indicator_tick);

static void indicator_timer_cb(struct k_timer *timer) {
    if (state.mode == OFF) return;
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &indicator_work);
}

K_TIMER_DEFINE(indicator_timer, indicator_timer_cb, NULL);

//// Toggle

static int indicator_on(void) {
    if (!led_strip) return -ENODEV;

#if IS_ENABLED(CONFIG_INDICATOR_LED_STRIP_EXT_POWER)
    if (ext_power != NULL && device_is_ready(ext_power)) {
        int rc = ext_power_enable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to enable EXT_POWER: %d", rc);
        }
    } else {
        LOG_ERR("ext power device is not ready");
    }
#endif

    state.mode = IS_ENABLED(CONFIG_INDICATOR_LED_STRIP_BREATHE) ? BREATHE : SOLID;
    /* 4MHzで1bit ws2812の1bitはspiの8bitだから1usかかる計算
    24*3+8(reset)で更新一回 80us なのでタイマー周期1msあたりから試してもいいかもね */
    k_timer_start(&indicator_timer, K_NO_WAIT, K_MSEC(1));

    return 0;
}

static void indicator_off_task(struct k_work *work) {
    struct led_rgb offpixels[STRIP_NUM_PIXELS];
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        offpixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
    }
    led_strip_update_rgb(led_strip, offpixels, STRIP_NUM_PIXELS);
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
    state.mode = OFF;

    return 0;
}

int indicator_led_strip_toggle(void) {
    switch (state.mode) {
    case OFF:
        state.mode = IS_ENABLED(CONFIG_INDICATOR_LED_STRIP_BREATHE) ? BREATHE : SOLID;
        return indicator_on();
    case SOLID:
    case BREATHE:
        return indicator_off();
    }
    return 0;
}

//// Listeners

static int battery_listener_cb(const zmk_event_t *eh) {
    if (state.mode == OFF) return 0;

    float rate = ((eh != NULL)
        ? as_zmk_battery_state_changed(eh)->state_of_charge
        : zmk_battery_state_of_charge()
    ) / 100.0f;

    if (rate > 0.5f) {
        const float upper_rate = (rate - 0.5f) / 0.5f;
        indicators[POWER_INDEX].r = POWER_COLOR_HALF.r + (POWER_COLOR_FULL.r - POWER_COLOR_HALF.r) * upper_rate;
        indicators[POWER_INDEX].g = POWER_COLOR_HALF.g + (POWER_COLOR_FULL.g - POWER_COLOR_HALF.g) * upper_rate;
        indicators[POWER_INDEX].b = POWER_COLOR_HALF.b + (POWER_COLOR_FULL.b - POWER_COLOR_HALF.b) * upper_rate;
    } else {
        const float lower_rate = rate / 0.5f;
        indicators[POWER_INDEX].r = POWER_COLOR_EMPTY.r + (POWER_COLOR_HALF.r - POWER_COLOR_EMPTY.r) * lower_rate;
        indicators[POWER_INDEX].g = POWER_COLOR_EMPTY.g + (POWER_COLOR_HALF.g - POWER_COLOR_EMPTY.g) * lower_rate;
        indicators[POWER_INDEX].b = POWER_COLOR_EMPTY.b + (POWER_COLOR_HALF.b - POWER_COLOR_EMPTY.b) * lower_rate;
    }

    return 0;
}

ZMK_LISTENER(indicator_battery_listener, battery_listener_cb);
ZMK_SUBSCRIPTION(indicator_battery_listener, zmk_battery_state_changed);

static int bluetooth_listener_cb(const zmk_event_t *eh) {
    if (state.mode == OFF) return 0;

    uint8_t profile_index = zmk_ble_active_profile_index();
    if (zmk_ble_active_profile_is_connected()) {
        switch (profile_index) {
        case 0: indicators[BLUET_INDEX] = BLUET_COLOR_P0; break;
        case 1: indicators[BLUET_INDEX] = BLUET_COLOR_P1; break;
        case 2: indicators[BLUET_INDEX] = BLUET_COLOR_P2; break;
        default: indicators[BLUET_INDEX] = BLUET_COLOR_DEFAULT; break;
        }
    } else if (zmk_ble_active_profile_is_open()) {
        indicators[BLUET_INDEX] = BLUET_COLOR_ADVERT;
    } else {
        indicators[BLUET_INDEX] = BLUET_COLOR_DISCON;
    }
    return 0;
}

ZMK_LISTENER(indicator_bluetooth_listener, bluetooth_listener_cb);
ZMK_SUBSCRIPTION(indicator_bluetooth_listener, zmk_ble_active_profile_changed);

static int layer_listener_cb(const zmk_event_t *eh) {
    if (state.mode == OFF) return 0;

    switch (zmk_keymap_highest_layer_active()) {
    case 0: indicators[LAYER_INDEX] = LAYER_COLOR_L0; break;
    case 1: indicators[LAYER_INDEX] = LAYER_COLOR_L1; break;
    case 2: indicators[LAYER_INDEX] = LAYER_COLOR_L2; break;
    case 3: indicators[LAYER_INDEX] = LAYER_COLOR_L3; break;
    default: indicators[LAYER_INDEX] = LAYER_COLOR_NULL; break;
    }
    return 0;
}

ZMK_LISTENER(indicator_layer_listener, layer_listener_cb);
ZMK_SUBSCRIPTION(indicator_layer_listener, zmk_layer_state_changed);

#if IS_ENABLED(CONFIG_INDICATOR_LED_STRIP_OFF_IDLE)

static int activity_state_listener_cb(const zmk_event_t *eh) {
    switch (zmk_activity_get_state()) {
    case ZMK_ACTIVITY_ACTIVE:
        indicator_on();
        break;
    case ZMK_ACTIVITY_IDLE:
        indicator_off();
        break;
    default:
        break;
    }
    return 0;
}

ZMK_LISTENER(indicator_activity_state_listener, activity_state_listener_cb);
ZMK_SUBSCRIPTION(indicator_activity_state_listener, zmk_activity_state_changed);

#endif

//// Initialization

extern void indicator_led_strip_init(void) {
    led_strip = DEVICE_DT_GET(STRIP_CHOSEN);

    state = (struct indicator_led_strip_state){
        mode : OFF
    };

    indicators[POWER_INDEX] = POWER_COLOR_EMPTY;
    indicators[BLUET_INDEX] = BLUET_COLOR_DISCON;
    indicators[LAYER_INDEX] = LAYER_COLOR_NULL;

    if (IS_ENABLED(CONFIG_INDICATOR_LED_STRIP_ON_START)) {
        indicator_on();
    }

    k_sleep(K_MSEC(200));
    battery_listener_cb(NULL);
    layer_listener_cb(NULL);
    bluetooth_listener_cb(NULL);
}

K_THREAD_DEFINE(indicator_led_strip_init_tid, 1024, indicator_led_strip_init, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);
