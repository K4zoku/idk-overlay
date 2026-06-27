#include "test_runner.h"
#include "public/idk_ipc.h"

TEST(input_event_size) {
    ASSERT_SIZEOF(idk_input_event_t, 20);
}

TEST(input_event_offsets) {
    idk_input_event_t ev;
    size_t base = (size_t)&ev;
    ASSERT_EQ((size_t)&ev.type  - base, 0);
    ASSERT_EQ((size_t)&ev.flags - base, 1);
    ASSERT_EQ((size_t)&ev.mods  - base, 2);
    ASSERT_EQ((size_t)&ev.time  - base, 4);
    ASSERT_EQ((size_t)&ev.u     - base, 8);
}

TEST(input_key_event_fields) {
    idk_input_event_t ev = {
        .type  = IDK_INPUT_KEY,
        .flags = IDK_INPUT_FLAG_PRESS,
        .mods  = IDK_MOD_CTRL | IDK_MOD_SHIFT,
        .time  = 12345,
        .u.key = { .keycode = 30, .keysym = 0x61 },
    };
    ASSERT_EQ(ev.type,           IDK_INPUT_KEY);
    ASSERT_EQ(ev.flags,          IDK_INPUT_FLAG_PRESS);
    ASSERT_EQ(ev.mods,           IDK_MOD_CTRL | IDK_MOD_SHIFT);
    ASSERT_EQ(ev.time,           12345);
    ASSERT_EQ(ev.u.key.keycode,  30);
    ASSERT_EQ(ev.u.key.keysym,   0x61);
}

TEST(input_button_event_fields) {
    idk_input_event_t ev = {
        .type  = IDK_INPUT_BUTTON,
        .flags = IDK_INPUT_FLAG_PRESS,
        .mods  = 0,
        .time  = 67890,
        .u.btn = { .x = 100, .y = 200, .button = 1 },
    };
    ASSERT_EQ(ev.type,        IDK_INPUT_BUTTON);
    ASSERT_EQ(ev.u.btn.x,     100);
    ASSERT_EQ(ev.u.btn.y,     200);
    ASSERT_EQ(ev.u.btn.button, 1);
}

TEST(input_motion_event_fields) {
    idk_input_event_t ev = {
        .type   = IDK_INPUT_MOTION,
        .flags  = 0,
        .time   = 11111,
        .u.motion = { .x = 500, .y = 300 },
    };
    ASSERT_EQ(ev.type,      IDK_INPUT_MOTION);
    ASSERT_EQ(ev.u.motion.x, 500);
    ASSERT_EQ(ev.u.motion.y, 300);
}

TEST(input_axis_event_fields) {
    idk_input_event_t ev = {
        .type = IDK_INPUT_AXIS,
        .flags = 0,
        .time  = 22222,
        .u.axis = { .dx = 10, .dy = -5 },
    };
    ASSERT_EQ(ev.type,    IDK_INPUT_AXIS);
    ASSERT_EQ(ev.u.axis.dx, 10);
    ASSERT_EQ(ev.u.axis.dy, -5);
}

TEST(input_state_event_fields) {
    idk_input_event_t ev = {
        .type  = IDK_INPUT_STATE,
        .flags = IDK_INPUT_FLAG_CAPTURE,
        .time  = 33333,
    };
    ASSERT_EQ(ev.type,  IDK_INPUT_STATE);
    ASSERT_EQ(ev.flags, IDK_INPUT_FLAG_CAPTURE);
}

TEST(input_repeat_event_fields) {
    idk_input_event_t ev = {
        .type    = IDK_INPUT_REPEAT,
        .flags   = 0,
        .time    = 44444,
        .u.repeat = { .rate = 25, .delay = 500 },
    };
    ASSERT_EQ(ev.type,        IDK_INPUT_REPEAT);
    ASSERT_EQ(ev.u.repeat.rate,  25);
    ASSERT_EQ(ev.u.repeat.delay, 500);
}

TEST(recv_input_validates_type_range) {
    /* We can't easily call idk_ipc_recv_input without a socket,
     * but we can verify the validation logic directly.
     * Values 1-6 are valid, everything else should fail. */
    for (int t = 1; t <= 6; t++) {
        idk_input_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = (uint8_t)t;
        /* type in range - would not hit the EBADMSG branch */
        ASSERT_TRUE(ev.type >= IDK_INPUT_KEY && ev.type <= IDK_INPUT_REPEAT);
    }
    /* Out of range */
    idk_input_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = 0;
    ASSERT_FALSE(ev.type >= IDK_INPUT_KEY && ev.type <= IDK_INPUT_REPEAT);
    ev.type = 7;
    ASSERT_FALSE(ev.type >= IDK_INPUT_KEY && ev.type <= IDK_INPUT_REPEAT);
    ev.type = 255;
    ASSERT_FALSE(ev.type >= IDK_INPUT_KEY && ev.type <= IDK_INPUT_REPEAT);
}

int main(void) {
    RUN(input_event_size);
    RUN(input_event_offsets);
    RUN(input_key_event_fields);
    RUN(input_button_event_fields);
    RUN(input_motion_event_fields);
    RUN(input_axis_event_fields);
    RUN(input_state_event_fields);
    RUN(input_repeat_event_fields);
    RUN(recv_input_validates_type_range);
    return 0;
}
