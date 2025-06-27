#include <zephyr/device.h>
#include <zephyr/devicetree.h>
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
#include <zmk/workqueue.h>
#include <zmk/keymap.h>

#include <zmk_indicator_led_strip/indicator_led_strip.h>

#define BASE_BRIGHTNESS_R 0.006f
#define BASE_BRIGHTNESS_G 0.005f
#define BASE_BRIGHTNESS_B 0.0064f

#define POWER_INDEX 0
#define BLUET_INDEX 1
#define LAYER_INDEX 2

#define POWER_COLOR_100  ((RGB){r: 0,   g: 1.0, b: 0.3})
#define POWER_COLOR_90   ((RGB){r: 0,   g: 1.0, b: 0.3})
#define POWER_COLOR_80   ((RGB){r: 0,   g: 1.0, b: 0.3})
#define POWER_COLOR_70   ((RGB){r: 0,   g: 1.0, b: 0.3})
#define POWER_COLOR_60   ((RGB){r: 0,   g: 1.0, b: 0.3})
#define POWER_COLOR_50   ((RGB){r: 0,   g: 1.0, b: 0.3})
#define POWER_COLOR_40   ((RGB){r: 0,   g: 1.0, b: 0.3})
#define POWER_COLOR_30   ((RGB){r: 0,   g: 1.0, b: 0})
#define POWER_COLOR_20   ((RGB){r: 0,   g: 1.0, b: 0})
#define POWER_COLOR_10   ((RGB){r: 1.0, g: 0,   b: 0})
#define POWER_COLOR_0    ((RGB){r: 1.0, g: 0,   b: 0})

#define BLUET_COLOR_ADVERT ((RGB){r: 0, g: 0, b: 1.0})
#define BLUET_COLOR_DISCON ((RGB){r: 0, g: 0, b: 0})
#define BLUET_COLOR_P0  ((RGB){r: 1.0, g: 0.3, b: 0})
#define BLUET_COLOR_P1  ((RGB){r: 0,   g: 0.8, b: 1.0})
#define BLUET_COLOR_P2  ((RGB){r: 0.8, g: 0.8, b: 0})
#define BLUET_COLOR_PX  ((RGB){r: 1.0, g: 1.0, b: 1.0})

#define LAYER_COLOR_L0 ((RGB){r: 1.0, g: 0,   b: 0})
#define LAYER_COLOR_L1 ((RGB){r: 0.4, g: 0,   b: 1.0})
#define LAYER_COLOR_L2 ((RGB){r: 0,   g: 1.0, b: 0})
#define LAYER_COLOR_L3 ((RGB){r: 0,   g: 0.4, b: 1.0})
#define LAYER_COLOR_LX ((RGB){r: 0,   g: 0,   b: 0})

#define BREATHE_LENGTH 5000

#define BLINK_BRIGHTNESS 0.5f
#define BLINK_OFF_LENGTH 2000
#define BLINK_ON_LENGTH 20

#define ACTIVE_MODE BREATHE
#define IDLE_MODE BLINK

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
    BREATHE,
    BLINK
};

typedef struct {
    float r;
    float g;
    float b;
} RGB;

static const struct device *led_strip;
static RGB indicators[STRIP_NUM_PIXELS];
static RGB fractions[STRIP_NUM_PIXELS];
static enum indicator_mode state_mode;
static uint32_t animation_counter = 0;

#if IS_ENABLED(CONFIG_INDICATOR_LED_STRIP_EXT_POWER)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

//// Ext-Power Operation

inline static void ext_power_on() {
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
}

inline static void ext_power_off() {
#if IS_ENABLED(CONFIG_INDICATOR_LED_STRIP_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_disable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to disable EXT_POWER: %d", rc);
        }
    }
#endif
}

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

inline static void scale_rgb(const RGB *const rgb, RGB *out_rgb, float rate) {
    out_rgb->r = rgb->r * rate;
    out_rgb->g = rgb->g * rate;
    out_rgb->b = rgb->b * rate;
}

static void indicator_tick(struct k_work *work) {
    RGB *target = indicators;
    RGB out_rgb[STRIP_NUM_PIXELS];

    if (state_mode == BLINK) {
        const uint32_t blink_time = animation_counter % (BLINK_OFF_LENGTH + BLINK_ON_LENGTH);
        float brightness;
        if (blink_time < BLINK_OFF_LENGTH) {
            if (blink_time == 0) ext_power_off();
            brightness = 0;
        } else {
            if (blink_time == BLINK_OFF_LENGTH) ext_power_on();
            brightness = BLINK_BRIGHTNESS;
        }
        scale_rgb(&indicators[POWER_INDEX], &out_rgb[POWER_INDEX], brightness);
        scale_rgb(&indicators[BLUET_INDEX], &out_rgb[BLUET_INDEX], brightness);
        scale_rgb(&indicators[LAYER_INDEX], &out_rgb[LAYER_INDEX], brightness);
        target = out_rgb;
    } else if (state_mode == BREATHE) {
        #define M_PI_2 6.2831853f
        float breathe_rate = (cosf(
            M_PI_2 * ((float)(animation_counter % BREATHE_LENGTH) / (float)BREATHE_LENGTH)
        ) + 1.1f) * 0.45f;
        scale_rgb(&indicators[POWER_INDEX], &out_rgb[POWER_INDEX], breathe_rate);
        scale_rgb(&indicators[BLUET_INDEX], &out_rgb[BLUET_INDEX], breathe_rate);
        scale_rgb(&indicators[LAYER_INDEX], &out_rgb[LAYER_INDEX], breathe_rate);
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

    animation_counter++;
}

K_WORK_DEFINE(indicator_work, indicator_tick);

static void indicator_timer_cb(struct k_timer *timer) {
    if (state_mode == OFF) return;
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &indicator_work);
}

K_TIMER_DEFINE(indicator_timer, indicator_timer_cb, NULL);

//// Toggle

static void animation_on(void) {
    // 4MHzで1bit ws2812の1bitはspiの8bitだから1usかかる計算
    // 24*3+8(reset)で更新一回 80us なのでタイマー周期1msあたりから試してもいいかも
    k_timer_start(&indicator_timer, K_NO_WAIT, K_MSEC(1));
    animation_counter = 0;

    k_sleep(K_MSEC(3));
}

static void animation_off_task(struct k_work *work) {
    struct led_rgb offpixels[STRIP_NUM_PIXELS];
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        offpixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
    }
    led_strip_update_rgb(led_strip, offpixels, STRIP_NUM_PIXELS);
}
K_WORK_DEFINE(animation_off_work, animation_off_task);

static void animation_off(void) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &animation_off_work);
    k_timer_stop(&indicator_timer);
}

int indicator_led_strip_toggle(void) {
    if (!led_strip) return -ENODEV;

    switch (state_mode) {
    case OFF:
        state_mode = ACTIVE_MODE;
        animation_on();
        ext_power_on();
    default:
        state_mode = OFF;
        ext_power_off();
        animation_off();
    }
    return 0;
}

//// Battery Listener

#define SET_INDICATOR_COLOR(CL, FL, VAL) \
    if (CL > VAL && VAL >= FL) { \
    const float rate_ = (VAL - FL) / 10.0f; \
    indicators[POWER_INDEX].r = POWER_COLOR_##FL.r + (POWER_COLOR_##CL.r - POWER_COLOR_##FL.r) * rate_; \
    indicators[POWER_INDEX].g = POWER_COLOR_##FL.g + (POWER_COLOR_##CL.g - POWER_COLOR_##FL.g) * rate_; \
    indicators[POWER_INDEX].b = POWER_COLOR_##FL.b + (POWER_COLOR_##CL.b - POWER_COLOR_##FL.b) * rate_; }

static int battery_listener_cb(const zmk_event_t *eh) {
    if (state_mode == OFF) return 0;

    int16_t rate = (eh != NULL)
        ? as_zmk_battery_state_changed(eh)->state_of_charge
        : zmk_battery_state_of_charge();
    // avoid value "100"
    rate = rate >= 100 ? 99 : rate;

    SET_INDICATOR_COLOR(100, 90, rate) else
    SET_INDICATOR_COLOR(90, 80, rate) else
    SET_INDICATOR_COLOR(80, 70, rate) else
    SET_INDICATOR_COLOR(60, 50, rate) else
    SET_INDICATOR_COLOR(50, 40, rate) else
    SET_INDICATOR_COLOR(40, 30, rate) else
    SET_INDICATOR_COLOR(30, 20, rate) else
    SET_INDICATOR_COLOR(20, 10, rate) else
    SET_INDICATOR_COLOR(10, 0, rate)

    return 0;
}

ZMK_LISTENER(indicator_battery_listener, battery_listener_cb);
ZMK_SUBSCRIPTION(indicator_battery_listener, zmk_battery_state_changed);

//// Bluetooth Listener

static int bluetooth_listener_cb(const zmk_event_t *eh) {
    if (state_mode == OFF) return 0;

    if (zmk_ble_active_profile_is_connected()) {
        switch (zmk_ble_active_profile_index()) {
            case 0: indicators[BLUET_INDEX] = BLUET_COLOR_P0; break;
            case 1: indicators[BLUET_INDEX] = BLUET_COLOR_P1; break;
            case 2: indicators[BLUET_INDEX] = BLUET_COLOR_P2; break;
            default: indicators[BLUET_INDEX] = BLUET_COLOR_PX; break;
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

//// Layer Listener

static int layer_listener_cb(const zmk_event_t *eh) {
    if (state_mode == OFF) return 0;

    switch (zmk_keymap_highest_layer_active()) {
    case 0: indicators[LAYER_INDEX] = LAYER_COLOR_L0; break;
    case 1: indicators[LAYER_INDEX] = LAYER_COLOR_L1; break;
    case 2: indicators[LAYER_INDEX] = LAYER_COLOR_L2; break;
    case 3: indicators[LAYER_INDEX] = LAYER_COLOR_L3; break;
    default: indicators[LAYER_INDEX] = LAYER_COLOR_LX; break;
    }
    return 0;
}

ZMK_LISTENER(indicator_layer_listener, layer_listener_cb);
ZMK_SUBSCRIPTION(indicator_layer_listener, zmk_layer_state_changed);

//// Activity State Listener

static int activity_state_listener_cb(const zmk_event_t *eh) {
    switch (zmk_activity_get_state()) {
    case ZMK_ACTIVITY_ACTIVE:
        if (state_mode != ACTIVE_MODE) animation_counter = 0;
        state_mode = ACTIVE_MODE;
        if (IDLE_MODE == OFF) animation_on();
        ext_power_on();
        break;
    case ZMK_ACTIVITY_IDLE:
        if (IDLE_MODE == OFF) animation_off();
        if (state_mode != IDLE_MODE) animation_counter = 0;
        state_mode = IDLE_MODE;
        break;
    default:
        break;
    }
    return 0;
}

ZMK_LISTENER(indicator_activity_state_listener, activity_state_listener_cb);
ZMK_SUBSCRIPTION(indicator_activity_state_listener, zmk_activity_state_changed);

//// Initialization

static int indicator_led_strip_init(void) {
    led_strip = DEVICE_DT_GET(STRIP_CHOSEN);

    state_mode = OFF;

    indicators[POWER_INDEX] = POWER_COLOR_0;
    indicators[BLUET_INDEX] = BLUET_COLOR_DISCON;
    indicators[LAYER_INDEX] = LAYER_COLOR_LX;

    return 0;
}

SYS_INIT(indicator_led_strip_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

extern void indicator_led_strip_init_lazy(void) {
    if (IS_ENABLED(CONFIG_INDICATOR_LED_STRIP_ON_START)) {
        state_mode = ACTIVE_MODE;
        animation_on();
        ext_power_on();
    }

    k_sleep(K_MSEC(200));

    battery_listener_cb(NULL);
    layer_listener_cb(NULL);
    bluetooth_listener_cb(NULL);
}

K_THREAD_DEFINE(indicator_led_strip_init_thread, 1024, indicator_led_strip_init_lazy, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);
