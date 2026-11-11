/* lht: 「押した瞬間にレイヤーON / 単独タップなら最後にキーを1つ送る」ビヘイビア
 * ZMK の hold-tap は「未確定のホールドタップは同時に1個」という制約があり、
 * 無変換を押したままスペースを押すとスペース側の判定が始まらない。
 * それを避けるため、レイヤー系キーをこのビヘイビアに置き換える。
 */
#define DT_DRV_COMPAT zmk_behavior_lht

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/keymap.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define LHT_MAX_POS 96
#define LHT_QUICK_TAP_MS 200  /* 2回目の押下を保持 → 連打 */
#define LHT_TAP_WINDOW_MS 300 /* これ以内に離して他キー無し → タップとしてキー送出 */

struct lht_pos_state {
    int64_t press_time;
    int64_t last_release;
    bool layer_on;
    bool repeating;
    bool used;
};

static struct lht_pos_state lht_states[LHT_MAX_POS];

static void lht_send_key(struct zmk_behavior_binding_event event, uint32_t keycode, bool press,
                         uint32_t wait) {
    struct zmk_behavior_binding kp = {
        .behavior_dev = DEVICE_DT_NAME(DT_NODELABEL(kp)),
        .param1 = keycode,
        .param2 = 0,
    };
    zmk_behavior_queue_add(&event, kp, press, wait);
}

static int lht_pressed(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    if (event.position < 0 || event.position >= LHT_MAX_POS) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    struct lht_pos_state *st = &lht_states[event.position];
    int64_t now = k_uptime_get();

    if (st->last_release != 0 && (now - st->last_release) < LHT_QUICK_TAP_MS) {
        /* 素早い2回目 → レイヤーに入らず、キーを押しっぱなし（＝連打）にする */
        st->repeating = true;
        st->layer_on = false;
        lht_send_key(event, binding->param2, true, 0);
        LOG_DBG("lht repeat pos %d", event.position);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    st->press_time = now;
    st->layer_on = true;
    st->used = false;
    st->repeating = false;
    zmk_keymap_layer_activate(binding->param1);
    LOG_DBG("lht layer %d on (pos %d)", binding->param1, event.position);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int lht_released(struct zmk_behavior_binding *binding,
                        struct zmk_behavior_binding_event event) {
    if (event.position < 0 || event.position >= LHT_MAX_POS) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    struct lht_pos_state *st = &lht_states[event.position];
    int64_t now = k_uptime_get();

    if (st->repeating) {
        lht_send_key(event, binding->param2, false, 0);
        st->repeating = false;
        st->last_release = now;
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (st->layer_on) {
        zmk_keymap_layer_deactivate(binding->param1);
        st->layer_on = false;
    }

    if (!st->used && st->press_time != 0 && (now - st->press_time) < LHT_TAP_WINDOW_MS) {
        /* 他のキーが押されていなければ、タップとしてキーを送る */
        lht_send_key(event, binding->param2, true, 5);
        lht_send_key(event, binding->param2, false, 0);
        LOG_DBG("lht tap pos %d", event.position);
    }

    st->press_time = 0;
    st->used = false;
    st->last_release = now;
    return ZMK_BEHAVIOR_OPAQUE;
}

/* ホールド中に他のキーが入力されたら「レイヤーを使用した」とみなす */
static int lht_keycode_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    for (int i = 0; i < LHT_MAX_POS; i++) {
        if (lht_states[i].layer_on) {
            lht_states[i].used = true;
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(lht_listener, lht_keycode_listener);
ZMK_SUBSCRIPTION(lht_listener, zmk_keycode_state_changed);

static const struct behavior_driver_api lht_driver_api = {
    .binding_pressed = lht_pressed,
    .binding_released = lht_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &lht_driver_api);

#endif
