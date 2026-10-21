#include "wiggle.h"

#include <math.h>
#include <stddef.h>

static inline bool ms_reached(uint32_t now, uint32_t t) {
    return (int32_t)(now - t) >= 0;
}

// --- Random helpers ----------------------------------------------------------

static float frand(wiggle_t *w) {
    return (float)(w->rand32() >> 8) * (1.0f / 16777216.0f);
}

static float frange(wiggle_t *w, float lo, float hi) {
    return lo + (hi - lo) * frand(w);
}

static uint32_t urange(wiggle_t *w, uint32_t lo, uint32_t hi) {
    return lo + w->rand32() % (hi - lo + 1);
}

// --- Square mode -------------------------------------------------------------

static const int8_t square_steps[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};

static bool square_poll(wiggle_t *w, uint32_t now, int8_t *dx, int8_t *dy) {
    if (!ms_reached(now, w->square_next_ms)) return false;
    *dx = (int8_t)(square_steps[w->square_idx][0] * WIGGLE_SQUARE_SIZE);
    *dy = (int8_t)(square_steps[w->square_idx][1] * WIGGLE_SQUARE_SIZE);
    w->square_idx = (w->square_idx + 1) % 4;
    w->square_next_ms = now + WIGGLE_SQUARE_STEP_MS;
    return true;
}

// --- Realistic mode ----------------------------------------------------------

static void add_stroke(wiggle_t *w, float dx, float dy, uint32_t pause_ms) {
    if (w->stroke_count >= WIGGLE_MAX_STROKES) return;
    float len = hypotf(dx, dy);
    if (len < 1.0f) return;
    float bend = frange(w, -0.2f, 0.2f) * len;
    wiggle_stroke_t *s = &w->strokes[w->stroke_count++];
    s->dx = dx;
    s->dy = dy;
    s->cx = dx * 0.5f - dy / len * bend;
    s->cy = dy * 0.5f + dx / len * bend;
    s->duration_ms = 80 + (uint32_t)(len * frange(w, 0.3f, 0.6f));
    if (s->duration_ms > 800) s->duration_ms = 800;
    s->pause_ms = pause_ms;
}

static void start_stroke(wiggle_t *w, uint32_t now) {
    w->stroke_start_ms = now + w->strokes[w->stroke_idx].pause_ms;
    w->stroke_sent_x = w->stroke_sent_y = 0;
}

static void plan_gesture(wiggle_t *w, uint32_t now) {
    float dx = 0, dy = 0;
    for (int tries = 0; tries < 8; tries++) {
        dx = frange(w, -WIGGLE_AREA_HALF_WIDTH, WIGGLE_AREA_HALF_WIDTH) - (float)w->pos_x;
        dy = frange(w, -WIGGLE_AREA_HALF_HEIGHT, WIGGLE_AREA_HALF_HEIGHT) - (float)w->pos_y;
        if (hypotf(dx, dy) >= WIGGLE_MIN_SWIPE) break;
    }

    w->stroke_count = w->stroke_idx = 0;
    if (frand(w) < 0.3f) {
        // Overshoot the target, then pull back to it.
        float k = frange(w, 1.04f, 1.15f);
        add_stroke(w, dx * k, dy * k, 0);
        add_stroke(w, dx * (1.0f - k) + frange(w, -4, 4), dy * (1.0f - k) + frange(w, -4, 4),
                   urange(w, 60, 250));
    } else {
        add_stroke(w, dx, dy, 0);
    }
    if (frand(w) < 0.4f) {
        // Small follow-up adjustment.
        add_stroke(w, frange(w, -25, 25), frange(w, -25, 25), urange(w, 150, 700));
    }
    start_stroke(w, now);
}

static bool realistic_poll(wiggle_t *w, uint32_t now, int8_t *out_dx, int8_t *out_dy) {
    if (w->stroke_idx >= w->stroke_count) {
        if (!ms_reached(now, w->gesture_next_ms)) return false;
        plan_gesture(w, now);
        if (w->stroke_count == 0) return false;
    }

    const wiggle_stroke_t *s = &w->strokes[w->stroke_idx];
    if (!ms_reached(now, w->stroke_start_ms)) return false;

    // Minimum-jerk velocity profile: slow start, fast middle, slow end.
    float t = (float)(now - w->stroke_start_ms) / (float)s->duration_ms;
    if (t > 1.0f) t = 1.0f;
    float e = t * t * t * (10.0f + t * (-15.0f + 6.0f * t));
    float u = 1.0f - e;
    float x = 2.0f * u * e * s->cx + e * e * s->dx;
    float y = 2.0f * u * e * s->cy + e * e * s->dy;
    if (t < 1.0f) {  // a little hand tremor
        x += frange(w, -0.7f, 0.7f);
        y += frange(w, -0.7f, 0.7f);
    }

    int32_t ddx = (int32_t)lroundf(x) - w->stroke_sent_x;
    int32_t ddy = (int32_t)lroundf(y) - w->stroke_sent_y;
    ddx = ddx > 127 ? 127 : ddx < -127 ? -127 : ddx;
    ddy = ddy > 127 ? 127 : ddy < -127 ? -127 : ddy;

    w->stroke_sent_x += ddx;
    w->stroke_sent_y += ddy;
    w->pos_x += ddx;
    w->pos_y += ddy;

    if (t >= 1.0f && w->stroke_sent_x == lroundf(s->dx) && w->stroke_sent_y == lroundf(s->dy)) {
        if (++w->stroke_idx < w->stroke_count) {
            start_stroke(w, now);
        } else if (frand(w) < WIGGLE_GESTURE_BURST_CHANCE) {
            w->gesture_next_ms = now + urange(w, 400, 2500);
        } else {
            w->gesture_next_ms = now + urange(w, WIGGLE_GESTURE_PAUSE_MIN_MS, WIGGLE_GESTURE_PAUSE_MAX_MS);
        }
    }

    if (!ddx && !ddy) return false;
    *out_dx = (int8_t)ddx;
    *out_dy = (int8_t)ddy;
    return true;
}

// --- Public API --------------------------------------------------------------

void wiggle_init(wiggle_t *w, uint32_t (*rand32)(void)) {
    *w = (wiggle_t){.rand32 = rand32};
}

void wiggle_start(wiggle_t *w, wiggle_mode_t mode, uint32_t now_ms) {
    uint32_t (*rand32)(void) = w->rand32;
    *w = (wiggle_t){.rand32 = rand32, .mode = mode};
    w->square_next_ms = now_ms;
    w->gesture_next_ms = now_ms + 500;
}

bool wiggle_poll(wiggle_t *w, uint32_t now_ms, int8_t *dx, int8_t *dy) {
    switch (w->mode) {
    case WIGGLE_MODE_SQUARE:    return square_poll(w, now_ms, dx, dy);
    case WIGGLE_MODE_REALISTIC: return realistic_poll(w, now_ms, dx, dy);
    default:                    return false;
    }
}
