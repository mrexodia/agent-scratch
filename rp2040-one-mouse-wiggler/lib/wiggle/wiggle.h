// Mouse movement generators for the wiggler.
//
// Plain C99 with no hardware dependencies, so the same code runs on the
// RP2040 and in host-side tests (see jiggler-detection-test/).
//
// Usage: call wiggle_start() when wiggling starts or the mode changes, then
// call wiggle_poll() whenever the host can accept a mouse report. When it
// returns true, send a report with the returned dx/dy.

#ifndef WIGGLE_H
#define WIGGLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Square mode: one step every WIGGLE_SQUARE_STEP_MS around a square of
// WIGGLE_SQUARE_SIZE counts. Net movement is zero.
#ifndef WIGGLE_SQUARE_STEP_MS
#define WIGGLE_SQUARE_STEP_MS        1000
#endif
#ifndef WIGGLE_SQUARE_SIZE
#define WIGGLE_SQUARE_SIZE           2
#endif

// Realistic mode: pause between gestures, chance of a quick follow-up gesture,
// and the area (in mouse counts, relative to where the cursor was when the
// mode started) that targets are picked in.
#ifndef WIGGLE_GESTURE_PAUSE_MIN_MS
#define WIGGLE_GESTURE_PAUSE_MIN_MS  3000
#endif
#ifndef WIGGLE_GESTURE_PAUSE_MAX_MS
#define WIGGLE_GESTURE_PAUSE_MAX_MS  25000
#endif
#ifndef WIGGLE_GESTURE_BURST_CHANCE
#define WIGGLE_GESTURE_BURST_CHANCE  0.25f
#endif
#ifndef WIGGLE_AREA_HALF_WIDTH
#define WIGGLE_AREA_HALF_WIDTH       700
#endif
#ifndef WIGGLE_AREA_HALF_HEIGHT
#define WIGGLE_AREA_HALF_HEIGHT      400
#endif
#ifndef WIGGLE_MIN_SWIPE
#define WIGGLE_MIN_SWIPE             80
#endif

typedef enum {
    WIGGLE_MODE_SQUARE,
    WIGGLE_MODE_REALISTIC,
    WIGGLE_MODE_COUNT
} wiggle_mode_t;

#define WIGGLE_MAX_STROKES 3

// One stroke: a quadratic Bezier from (0,0) via (cx,cy) to (dx,dy).
typedef struct {
    float dx, dy;
    float cx, cy;
    uint32_t duration_ms;
    uint32_t pause_ms;  // wait before this stroke starts
} wiggle_stroke_t;

typedef struct {
    uint32_t (*rand32)(void);
    wiggle_mode_t mode;

    // Square mode
    unsigned square_idx;
    uint32_t square_next_ms;

    // Realistic mode
    wiggle_stroke_t strokes[WIGGLE_MAX_STROKES];
    int stroke_count, stroke_idx;
    uint32_t stroke_start_ms;
    int32_t stroke_sent_x, stroke_sent_y;
    int32_t pos_x, pos_y;  // estimated cursor offset from the start point
    uint32_t gesture_next_ms;
} wiggle_t;

// rand32 must return uniformly distributed 32-bit random numbers.
void wiggle_init(wiggle_t *w, uint32_t (*rand32)(void));

// (Re)start generating movement in the given mode.
void wiggle_start(wiggle_t *w, wiggle_mode_t mode, uint32_t now_ms);

// Returns true and sets *dx/*dy if a movement report should be sent now.
bool wiggle_poll(wiggle_t *w, uint32_t now_ms, int8_t *dx, int8_t *dy);

#ifdef __cplusplus
}
#endif

#endif
