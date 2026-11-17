/*
 * input -- every way of holding a controller input, merged into what Stella
 * sees.
 *
 * The keyboard, the Controllers window's mouse, a timed pulse (a driving
 * wheel's notch, the menus' Select and Reset), and each gamepad's buttons and
 * its stick are SEPARATE sources of a target (A2600_SRC_* in
 * session_internal.h). A target is held while any source holds it, so a key
 * released while a pad still holds the same fire button leaves the button
 * down, and two pads on one port do not cancel each other. Opposed pairs on
 * the same controller (up and down, left and right, a paddle's two turn keys,
 * the driving wheel's two ways) cancel, as on the real hardware where the
 * contacts cannot close both ways: the console sees neither.
 *
 * The analog controllers (the paddles, the driving wheel) are positions, not
 * holds: the last source to write one owns it. A gamepad stick writes only
 * when its value changes, so a still pad never fights a mouse drag, and a pad
 * that goes away recentres what it last moved.
 *
 * input_mtx is a leaf lock (see session_internal.h): the calls that hand the
 * merged result to Stella (it only queues an event) are made under it, so two
 * threads can never reorder a press and its release.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "compat.h"
#include "input.h"

/* The actions whose two directions cannot both be held. */
static const int k_pairs[5][2] = {
    { A2600_ACT_JOY_UP, A2600_ACT_JOY_DOWN },
    { A2600_ACT_JOY_LEFT, A2600_ACT_JOY_RIGHT },
    { A2600_ACT_PADDLE_A_DEC, A2600_ACT_PADDLE_A_INC },
    { A2600_ACT_PADDLE_B_DEC, A2600_ACT_PADDLE_B_INC },
    { A2600_ACT_DRIVE_CCW, A2600_ACT_DRIVE_CW },
};

static void set_src(uint32_t *word, uint32_t src, int down)
{
    if (down)
        *word |= src;
    else
        *word &= ~src;
}

/* ---- what Stella sees ------------------------------------------------------------ */

/* input_mtx held. */
static unsigned desired_locked(const struct a2600session *s, int port)
{
    unsigned mask = 0;
    int a, p;

    for (a = 0; a < A2600_ACT_PER_PORT; a++)
        if (s->src[A2600_TARGET_PORT(port, a)])
            mask |= 1u << a;
    for (p = 0; p < 5; p++) {
        const unsigned lo = 1u << k_pairs[p][0], hi = 1u << k_pairs[p][1];
        if ((mask & lo) && (mask & hi))
            mask &= ~(lo | hi);
    }
    return mask;
}

/* Tells Stella what changed on a port. While the machine is stopped nothing is
 * sent and nothing is remembered as sent, so input_resync can hand it all over
 * when it starts. */
static void push_port_locked(struct a2600session *s, int port)
{
    unsigned want, diff;
    int a;

    if (!s->running)
        return;
    want = desired_locked(s, port);
    diff = want ^ s->sent_acts[port];
    for (a = 0; a < A2600_ACT_PER_PORT; a++)
        if (diff & (1u << a))
            session_machine_event(s, port, a, (want >> a) & 1u);
    s->sent_acts[port] = want;
}

static void push_switches_locked(struct a2600session *s)
{
    static const int k_momentary[2] = { A2600_SW_SELECT, A2600_SW_RESET };
    int i;

    if (!s->running)
        return;
    for (i = 0; i < 2; i++) {
        const int sw = k_momentary[i];
        const uint8_t want = s->src[A2600_TARGET_SWITCH(sw)] != 0;
        if (want != s->sent_sw[i]) {
            s->sent_sw[i] = want;
            session_machine_switch(s, sw, want);
        }
    }
}

static void push_all_locked(struct a2600session *s)
{
    push_port_locked(s, 0);
    push_port_locked(s, 1);
    push_switches_locked(s);
}

void input_init(struct a2600session *s)
{
    int port, axis;

    pthread_mutex_init(&s->input_mtx, NULL);
    pthread_cond_init(&s->pulse_cond, NULL);
    for (port = 0; port < 2; port++)
        for (axis = 0; axis < 3; axis++)
            s->analog_owner[port][axis] = 0;
}

void input_destroy(struct a2600session *s)
{
    pthread_cond_destroy(&s->pulse_cond);
    pthread_mutex_destroy(&s->input_mtx);
}

void input_resync(struct a2600session *s)
{
    int port, axis;

    pthread_mutex_lock(&s->input_mtx);
    /* the pulses were counted in the old machine's frames: over */
    memset(s->pulse_until, 0, sizeof s->pulse_until);
    {
        int t;
        for (t = 0; t < A2600_TARGET_COUNT; t++)
            s->src[t] &= ~A2600_SRC_PULSE;
    }
    memset(s->sent_acts, 0, sizeof s->sent_acts);
    memset(s->sent_sw, 0, sizeof s->sent_sw);
    push_all_locked(s);
    for (port = 0; port < 2; port++)
        for (axis = 0; axis < 3; axis++)
            if (s->running && s->analog_val[port][axis])
                session_machine_analog(s, port, axis == 2 ? -1 : axis,
                                       s->analog_val[port][axis]);
    pthread_mutex_unlock(&s->input_mtx);
}

/* ---- holds ----------------------------------------------------------------------- */

void input_press(struct a2600session *s, uint32_t src, int target, int down)
{
    int sw, was;

    if (!s || target < 0 || target >= A2600_TARGET_COUNT)
        return;

    /* a system action is not a held input: it runs on the press. The pad
     * thread (and the keyboard's path) must not run it themselves: it may stop
     * the machine in the debugger or reboot it, so it is the UI thread's. */
    if (target >= A2600_TARGET_SYSACT(0)) {
        if (!down)
            return;
        if (src == A2600_SRC_POINTER)
            a2600session_sysaction(s, target - A2600_TARGET_SYSACT(0));
        else
            a2600session_sysaction_post(s, target - A2600_TARGET_SYSACT(0));
        return;
    }

    pthread_mutex_lock(&s->input_mtx);
    if (target < 2 * A2600_ACT_PER_PORT) {
        set_src(&s->src[target], src, down);
        push_port_locked(s, target / A2600_ACT_PER_PORT);
        pthread_mutex_unlock(&s->input_mtx);
        return;
    }
    sw = target - 2 * A2600_ACT_PER_PORT;
    if (sw == A2600_SW_SELECT || sw == A2600_SW_RESET) {
        set_src(&s->src[target], src, down);
        push_switches_locked(s);
        pthread_mutex_unlock(&s->input_mtx);
        return;
    }
    /* a toggling switch (Color/B&W, the difficulties) flips on each source's
     * press; the source bit only makes a repeat of that same press a no-op */
    was = (s->src[target] & src) != 0;
    set_src(&s->src[target], src, down);
    pthread_mutex_unlock(&s->input_mtx);
    if (down && !was)
        a2600session_switch_set(s, sw, !a2600session_switch_get(s, sw));
}

void input_port_set(struct a2600session *s, uint32_t src, int port, unsigned mask)
{
    int a;

    if (!s || port < 0 || port > 1)
        return;
    pthread_mutex_lock(&s->input_mtx);
    for (a = 0; a < A2600_ACT_PER_PORT; a++)
        set_src(&s->src[A2600_TARGET_PORT(port, a)], src, (mask >> a) & 1u);
    push_port_locked(s, port);
    pthread_mutex_unlock(&s->input_mtx);
}

void input_release_sources(struct a2600session *s, uint32_t srcmask)
{
    int t;

    if (!s)
        return;
    pthread_mutex_lock(&s->input_mtx);
    for (t = 0; t < A2600_TARGET_SYSACT(0); t++)
        s->src[t] &= ~srcmask;
    push_all_locked(s);
    pthread_mutex_unlock(&s->input_mtx);
}

void input_stop(struct a2600session *s)
{
    pthread_mutex_lock(&s->input_mtx);
    if (s->pulse_thread_up) {
        s->pulse_quit = 1;
        pthread_cond_signal(&s->pulse_cond);
        pthread_mutex_unlock(&s->input_mtx);
        pthread_join(s->pulse_thread, NULL);
        pthread_mutex_lock(&s->input_mtx);
        s->pulse_thread_up = 0;
        s->pulse_quit = 0;
    }
    {
        int t;
        memset(s->pulse_until, 0, sizeof s->pulse_until);
        for (t = 0; t < A2600_TARGET_COUNT; t++)
            s->src[t] &= ~A2600_SRC_PULSE;
    }
    pthread_mutex_unlock(&s->input_mtx);
}

/* ---- gamepads -------------------------------------------------------------------- */

void input_pad_clear(struct a2600session *s, int owner)
{
    int t, port, axis;
    const uint32_t srcmask = A2600_SRC_PAD(owner) | A2600_SRC_STICK(owner);

    if (!s || owner < 0 || owner >= A2600_MAX_PADS)
        return;
    pthread_mutex_lock(&s->input_mtx);
    for (t = 0; t < A2600_TARGET_SYSACT(0); t++)
        s->src[t] &= ~srcmask;
    push_all_locked(s);
    /* an analog controller this pad was the last to move goes back to centre,
     * so an unplugged stick cannot leave a paddle (or the wheel) deflected */
    for (port = 0; port < 2; port++)
        for (axis = 0; axis < 3; axis++)
            if (s->analog_owner[port][axis] == A2600_SRC_STICK(owner)) {
                s->analog_owner[port][axis] = 0;
                if (s->analog_val[port][axis]) {
                    s->analog_val[port][axis] = 0;
                    if (s->running)
                        session_machine_analog(s, port, axis == 2 ? -1 : axis, 0);
                }
            }
    pthread_mutex_unlock(&s->input_mtx);
}

/* ---- analog positions ------------------------------------------------------------- */

void input_analog(struct a2600session *s, uint32_t src, int port, int axis, int value)
{
    if (!s || port < 0 || port > 1 || axis < 0 || axis > 2)
        return;
    if (value < -32767) value = -32767;
    if (value > 32767) value = 32767;
    pthread_mutex_lock(&s->input_mtx);
    s->analog_owner[port][axis] = src;
    if (value != s->analog_val[port][axis]) {
        s->analog_val[port][axis] = value;
        if (s->running)
            session_machine_analog(s, port, axis == 2 ? -1 : axis, value);
    }
    pthread_mutex_unlock(&s->input_mtx);
}

void input_analog_end(struct a2600session *s, uint32_t src, int port, int axis)
{
    if (!s || port < 0 || port > 1 || axis < 0 || axis > 2)
        return;
    pthread_mutex_lock(&s->input_mtx);
    if (s->analog_owner[port][axis] == src)
        s->analog_owner[port][axis] = 0;
    pthread_mutex_unlock(&s->input_mtx);
}

int input_analog_get(struct a2600session *s, int port, int axis)
{
    int value = 0;

    if (!s || port < 0 || port > 1 || axis < 0 || axis > 2)
        return 0;
    pthread_mutex_lock(&s->input_mtx);
    value = s->analog_val[port][axis];
    pthread_mutex_unlock(&s->input_mtx);
    return value;
}

/* ---- what is held ------------------------------------------------------------------ */

unsigned input_held(struct a2600session *s, int port)
{
    unsigned mask;

    if (!s || port < 0 || port > 1)
        return 0;
    pthread_mutex_lock(&s->input_mtx);
    mask = desired_locked(s, port);
    pthread_mutex_unlock(&s->input_mtx);
    return mask;
}

int input_switch_held(struct a2600session *s, int sw)
{
    int held;

    if (!s || sw < 0 || sw >= A2600_SW_COUNT)
        return 0;
    if (sw != A2600_SW_SELECT && sw != A2600_SW_RESET)
        return s->switch_state[sw];       /* a toggle: where it is */
    pthread_mutex_lock(&s->input_mtx);
    held = s->src[A2600_TARGET_SWITCH(sw)] != 0;
    pthread_mutex_unlock(&s->input_mtx);
    return held;
}

/* ---- timed pulses ------------------------------------------------------------------ */

/* Lets each pulse go once the machine has run its frames. Waits on pulse_cond
 * while there is none. */
static void *pulse_main(void *arg)
{
    struct a2600session *s = arg;

    pthread_mutex_lock(&s->input_mtx);
    while (!s->pulse_quit) {
        int active = 0, t;

        for (t = 0; t < A2600_TARGET_COUNT; t++)
            active |= s->pulse_until[t] != 0;
        if (!active) {
            pthread_cond_wait(&s->pulse_cond, &s->input_mtx);
            continue;
        }
        pthread_mutex_unlock(&s->input_mtx);
        a2600_sleep_ms(2);
        pthread_mutex_lock(&s->input_mtx);
        {
            const uint64_t now = a2600session_frame_count(s);
            int changed = 0;

            for (t = 0; t < A2600_TARGET_COUNT; t++)
                if (s->pulse_until[t] && now >= s->pulse_until[t]) {
                    s->pulse_until[t] = 0;
                    s->src[t] &= ~A2600_SRC_PULSE;
                    changed = 1;
                }
            if (changed)
                push_all_locked(s);
        }
    }
    pthread_mutex_unlock(&s->input_mtx);
    return NULL;
}

void input_pulse(struct a2600session *s, int target, int frames)
{
    int other = -1;

    if (!s || !s->running || frames <= 0)
        return;
    /* only the targets a pulse makes sense for: a momentary console switch, or
     * a notch of the driving wheel */
    if (target >= 2 * A2600_ACT_PER_PORT) {
        const int sw = target - 2 * A2600_ACT_PER_PORT;
        if (sw != A2600_SW_SELECT && sw != A2600_SW_RESET)
            return;
    } else if (target % A2600_ACT_PER_PORT == A2600_ACT_DRIVE_CCW) {
        other = target + 1;
    } else if (target % A2600_ACT_PER_PORT == A2600_ACT_DRIVE_CW) {
        other = target - 1;
    } else {
        return;
    }

    pthread_mutex_lock(&s->input_mtx);
    if (!s->pulse_thread_up) {
        s->pulse_quit = 0;
        if (pthread_create(&s->pulse_thread, NULL, pulse_main, s) != 0) {
            pthread_mutex_unlock(&s->input_mtx);
            return;
        }
        s->pulse_thread_up = 1;
    }
    /* a notch the other way ends the one before it */
    if (other >= 0) {
        s->pulse_until[other] = 0;
        s->src[other] &= ~A2600_SRC_PULSE;
    }
    s->pulse_until[target] = a2600session_frame_count(s) + (uint64_t)frames;
    s->src[target] |= A2600_SRC_PULSE;
    push_all_locked(s);
    pthread_cond_signal(&s->pulse_cond);
    pthread_mutex_unlock(&s->input_mtx);
}
