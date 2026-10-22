#include "wiggle.h"

_Static_assert(WIGGLE_MAX_REPORTS >= 10, "quiet runs need ten report slots");
_Static_assert(WIGGLE_QUIET_BOUND >= 175 && WIGGLE_QUIET_BOUND <= INT16_MAX,
               "a complete quiet run must fit in either half-bound");
_Static_assert(WIGGLE_SWEEP_BOUND >= 336 && WIGGLE_SWEEP_BOUND <= INT16_MAX,
               "a complete sweep must fit in either half-bound");
_Static_assert(WIGGLE_REST_MIN_MS >= 2000 && WIGGLE_REST_MIN_MS <= 14000,
               "rest interval must be valid and break continuous motion");

static bool reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static uint32_t between(wiggle_t *w, uint32_t lo, uint32_t hi) {
    // Multiplication maps a 32-bit draw into the small requested interval;
    // unlike rejection sampling this also has bounded cost with a faulty RNG.
    return lo + (uint32_t)(((uint64_t)w->rand32() * (hi - lo + 1)) >> 32);
}

static uint32_t rest(wiggle_t *w) {
    return between(w, WIGGLE_REST_MIN_MS,
                   w->mode == WIGGLE_MODE_QUIET ? 14000 : 25000);
}

static void plan(wiggle_t *w) {
    bool quiet = w->mode == WIGGLE_MODE_QUIET;
    // Even run lengths preserve the low/high pairing across run boundaries.
    // At least six reports in one direction limit reversals to nine in any
    // 50-report direction window, including windows spanning multiple runs.
    w->count = (uint8_t)(2 * between(w, 3, quiet ? 5 : 4));
    w->index = 0;
    int total = 0;
    for (unsigned i = 0; i < w->count; ++i) {
        int magnitude = (int)((i & 1)
            ? between(w, quiet ? 16 : 24, quiet ? 28 : 72)
            : between(w, 4, quiet ? 7 : 12));
        w->reports[i] = (int8_t)magnitude;
        total += magnitude;
    }
    // The low/high ranges keep report-magnitude CV above 0.30, even at the
    // closest endpoints (12 vs 24). This is the detector's "speed" metric;
    // merely randomizing timestamps would leave constant-delta evidence.
    int bound = quiet ? WIGGLE_QUIET_BOUND : WIGGLE_SWEEP_BOUND;
    int direction = between(w, 0, 1) ? 1 : -1;
    if (w->position + direction * total < -bound ||
        w->position + direction * total > bound) direction = -direction;
    // Maximum total is 175 (quiet) / 336 (sweep), below each mode's half-bound.
    // Consequently at least one direction always fits, with no clipping,
    // truncated run, rejection loop, or forced exact return to the origin.
    for (unsigned i = 0; i < w->count; ++i)
        w->reports[i] = (int8_t)(w->reports[i] * direction);
}

void wiggle_init(wiggle_t *w, uint32_t (*rand32)(void)) {
    *w = (wiggle_t){.rand32 = rand32};
    // Integer lines (horizontal, vertical, either diagonal) remain exactly
    // collinear after HID quantization. Preserve the axis through mode changes
    // and suspend: the host may still retain reports from before the restart.
    static const int8_t axes[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
    unsigned axis = between(w, 0, 3);
    w->axis_x = axes[axis][0];
    w->axis_y = axes[axis][1];
}

void wiggle_start(wiggle_t *w, wiggle_mode_t mode, uint32_t now_ms) {
    uint32_t (*rand32)(void) = w->rand32;
    int8_t axis_x = w->axis_x, axis_y = w->axis_y;
    *w = (wiggle_t){.rand32 = rand32, .mode = mode,
                    .axis_x = axis_x, .axis_y = axis_y};
    w->next_ms = now_ms + between(w, 500, 1800);
}

bool wiggle_poll(wiggle_t *w, uint32_t now_ms, int8_t *dx, int8_t *dy) {
    *dx = *dy = 0;
    if ((w->mode != WIGGLE_MODE_QUIET && w->mode != WIGGLE_MODE_SWEEP) ||
        !reached(now_ms, w->next_ms)) return false;
    if (w->index == w->count) plan(w);
    int step = w->reports[w->index++];
    *dx = (int8_t)(step * w->axis_x);
    *dy = (int8_t)(step * w->axis_y);
    w->position = (int16_t)(w->position + step);
    if (w->mode == WIGGLE_MODE_QUIET || w->index == w->count)
        w->next_ms = now_ms + rest(w);
    else
        w->next_ms = now_ms + between(w, 10, 30);
    return true;
}
