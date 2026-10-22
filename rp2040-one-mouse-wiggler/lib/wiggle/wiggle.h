// Hardware-independent movement engine shared by firmware and host tests.
#ifndef WIGGLE_H
#define WIGGLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Persisted mode numbers must not change. The legacy names remain source aliases;
// mode 0 no longer traces a square, and mode 1 no longer simulates curved swipes.
typedef enum {
    WIGGLE_MODE_QUIET = 0,
    WIGGLE_MODE_SWEEP = 1,
    WIGGLE_MODE_COUNT = 2,
    WIGGLE_MODE_SQUARE = WIGGLE_MODE_QUIET,
    WIGGLE_MODE_REALISTIC = WIGGLE_MODE_SWEEP
} wiggle_mode_t;

#define WIGGLE_QUIET_BOUND 192
#define WIGGLE_SWEEP_BOUND 400
#define WIGGLE_MAX_REPORTS 10
#define WIGGLE_REST_MIN_MS 3200

// One fixed, randomly chosen line per wiggle_init (normally once at boot).
// Coordinates are raw mouse counts, not pixels. Position is bounded relative
// to each wiggle_start; there is no screen position feedback. Restarts preserve
// the axis because host histories can retain reports from before the restart.
typedef struct {
    uint32_t (*rand32)(void);
    wiggle_mode_t mode;
    int8_t axis_x, axis_y;
    int16_t position;
    int8_t reports[WIGGLE_MAX_REPORTS];
    uint8_t count, index;
    uint32_t next_ms;
} wiggle_t;

// rand32 must be non-NULL and return uniformly distributed 32-bit random numbers.
void wiggle_init(wiggle_t *w, uint32_t (*rand32)(void));

// Discard any old run and restart with a fresh bounded origin and initial rest.
void wiggle_start(wiggle_t *w, wiggle_mode_t mode, uint32_t now_ms);

// Call only when a report can be accepted. True consumes exactly one report.
// The caller must retain/retry it if the transport rejects it (see firmware).
// False leaves dx and dy at zero. Late polls never produce catch-up bursts.
bool wiggle_poll(wiggle_t *w, uint32_t now_ms, int8_t *dx, int8_t *dy);

#ifdef __cplusplus
}
#endif
#endif
