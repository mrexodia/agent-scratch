// Mouse wiggler for the Waveshare RP2040-One.
//
// BOOT button:
//   long press  (>= LONG_PRESS_MS) - enable / disable wiggling
//   short press                    - cycle mode (LED flashes the new mode's colour)
//
// Modes:
//   MODE_SQUARE    - 1 px square, one step per SQUARE_STEP_MS. Net movement is
//                    zero, so the computer stays usable while it runs.
//   MODE_REALISTIC - every few seconds a quick, slightly curved swipe across
//                    the screen, sometimes with an overshoot and correction.
//                    Never clicks.

#include <math.h>

#include "pico/stdlib.h"
#include "pico/rand.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "tusb.h"

#include "ws2812.pio.h"

#define LED_PIN                 PICO_DEFAULT_WS2812_PIN
#define LED_BRIGHTNESS          32
#define MODE_FLASH_MS           500

#define DEBOUNCE_MS             30
#define LONG_PRESS_MS           700

#define SQUARE_STEP_MS          1000
#define SQUARE_SIZE             1

// Realistic mode: pause between gestures, and the area (in mouse counts,
// relative to where the cursor was when the mode started) targets are picked in.
#define GESTURE_PAUSE_MIN_MS    3000
#define GESTURE_PAUSE_MAX_MS    25000
#define GESTURE_BURST_CHANCE    0.25f
#define AREA_HALF_WIDTH         700
#define AREA_HALF_HEIGHT        400
#define MIN_SWIPE               80

typedef enum { MODE_SQUARE, MODE_REALISTIC, MODE_COUNT } wiggle_mode_t;

static const uint8_t mode_colour[MODE_COUNT][3] = {
    [MODE_SQUARE]    = {0, LED_BRIGHTNESS, 0},  // green
    [MODE_REALISTIC] = {0, 0, LED_BRIGHTNESS},  // blue
};

static bool enabled;
static wiggle_mode_t mode = MODE_SQUARE;

static inline bool ms_reached(uint32_t now, uint32_t t) {
    return (int32_t)(now - t) >= 0;
}

// --- Random helpers --------------------------------------------------------

static float frand(void) {
    return (float)(get_rand_32() >> 8) * (1.0f / 16777216.0f);
}

static float frange(float lo, float hi) {
    return lo + (hi - lo) * frand();
}

static uint32_t urange(uint32_t lo, uint32_t hi) {
    return lo + get_rand_32() % (hi - lo + 1);
}

// --- LED ---------------------------------------------------------------------

static PIO led_pio = pio0;
static uint led_sm;
static uint32_t led_shown = UINT32_MAX;
static uint32_t led_flash_until;

static void led_init(void) {
    uint offset = pio_add_program(led_pio, &ws2812_program);
    led_sm = pio_claim_unused_sm(led_pio, true);
    ws2812_program_init(led_pio, led_sm, offset, LED_PIN, 800000, false);
}

static void led_update(uint32_t now) {
    uint32_t grb = 0;
    if (enabled || !ms_reached(now, led_flash_until)) {
        const uint8_t *c = mode_colour[mode];
        grb = ((uint32_t)c[1] << 16) | ((uint32_t)c[0] << 8) | c[2];
    }
    if (grb != led_shown) {
        led_shown = grb;
        pio_sm_put_blocking(led_pio, led_sm, grb << 8);
    }
}

// --- Mouse -------------------------------------------------------------------

static bool mouse_move(int8_t dx, int8_t dy) {
    if (!tud_hid_ready()) return false;
    uint8_t report[4] = {0, (uint8_t)dx, (uint8_t)dy, 0};
    // In boot protocol the host expects the 3-byte boot mouse report.
    uint16_t len = tud_hid_get_protocol() == HID_PROTOCOL_BOOT ? 3 : 4;
    return tud_hid_report(0, report, len);
}

// --- Square mode -------------------------------------------------------------

static const int8_t square_steps[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
static uint square_idx;
static uint32_t square_next_ms;

static void square_reset(uint32_t now) {
    square_idx = 0;
    square_next_ms = now;
}

static void square_task(uint32_t now) {
    if (!ms_reached(now, square_next_ms)) return;
    if (!mouse_move(square_steps[square_idx][0] * SQUARE_SIZE,
                    square_steps[square_idx][1] * SQUARE_SIZE)) return;
    square_idx = (square_idx + 1) % 4;
    square_next_ms = now + SQUARE_STEP_MS;
}

// --- Realistic mode ----------------------------------------------------------

// One stroke: a quadratic Bezier from (0,0) via (cx,cy) to (dx,dy), traversed
// with a minimum-jerk velocity profile (slow start, fast middle, slow end).
typedef struct {
    float dx, dy;
    float cx, cy;
    uint32_t duration_ms;
    uint32_t pause_ms;  // wait before this stroke starts
} stroke_t;

#define MAX_STROKES 3
static stroke_t strokes[MAX_STROKES];
static int stroke_count, stroke_idx;
static uint32_t stroke_start_ms;
static int32_t stroke_sent_x, stroke_sent_y;
static int32_t pos_x, pos_y;  // estimated cursor offset from the start point
static uint32_t gesture_next_ms;

static void add_stroke(float dx, float dy, uint32_t pause_ms) {
    if (stroke_count >= MAX_STROKES) return;
    float len = hypotf(dx, dy);
    if (len < 1.0f) return;
    float bend = frange(-0.2f, 0.2f) * len;
    stroke_t *s = &strokes[stroke_count++];
    s->dx = dx;
    s->dy = dy;
    s->cx = dx * 0.5f - dy / len * bend;
    s->cy = dy * 0.5f + dx / len * bend;
    s->duration_ms = 80 + (uint32_t)(len * frange(0.3f, 0.6f));
    if (s->duration_ms > 800) s->duration_ms = 800;
    s->pause_ms = pause_ms;
}

static void start_stroke(uint32_t now) {
    stroke_start_ms = now + strokes[stroke_idx].pause_ms;
    stroke_sent_x = stroke_sent_y = 0;
}

static void plan_gesture(uint32_t now) {
    float dx = 0, dy = 0;
    for (int tries = 0; tries < 8; tries++) {
        dx = frange(-AREA_HALF_WIDTH, AREA_HALF_WIDTH) - (float)pos_x;
        dy = frange(-AREA_HALF_HEIGHT, AREA_HALF_HEIGHT) - (float)pos_y;
        if (hypotf(dx, dy) >= MIN_SWIPE) break;
    }

    stroke_count = stroke_idx = 0;
    if (frand() < 0.3f) {
        // Overshoot the target, then pull back to it.
        float k = frange(1.04f, 1.15f);
        add_stroke(dx * k, dy * k, 0);
        add_stroke(dx * (1.0f - k) + frange(-4, 4), dy * (1.0f - k) + frange(-4, 4),
                   urange(60, 250));
    } else {
        add_stroke(dx, dy, 0);
    }
    if (frand() < 0.4f) {
        // Small follow-up adjustment.
        add_stroke(frange(-25, 25), frange(-25, 25), urange(150, 700));
    }
    start_stroke(now);
}

static void realistic_reset(uint32_t now) {
    stroke_count = stroke_idx = 0;
    pos_x = pos_y = 0;
    gesture_next_ms = now + 500;
}

static void realistic_task(uint32_t now) {
    if (stroke_idx >= stroke_count) {
        if (!ms_reached(now, gesture_next_ms)) return;
        plan_gesture(now);
        if (stroke_count == 0) return;
    }

    const stroke_t *s = &strokes[stroke_idx];
    if (!ms_reached(now, stroke_start_ms) || !tud_hid_ready()) return;

    float t = (float)(now - stroke_start_ms) / (float)s->duration_ms;
    if (t > 1.0f) t = 1.0f;
    float e = t * t * t * (10.0f + t * (-15.0f + 6.0f * t));
    float u = 1.0f - e;
    float x = 2.0f * u * e * s->cx + e * e * s->dx;
    float y = 2.0f * u * e * s->cy + e * e * s->dy;
    if (t < 1.0f) {  // a little hand tremor
        x += frange(-0.7f, 0.7f);
        y += frange(-0.7f, 0.7f);
    }

    int32_t ddx = (int32_t)lroundf(x) - stroke_sent_x;
    int32_t ddy = (int32_t)lroundf(y) - stroke_sent_y;
    ddx = ddx > 127 ? 127 : ddx < -127 ? -127 : ddx;
    ddy = ddy > 127 ? 127 : ddy < -127 ? -127 : ddy;

    if (ddx || ddy) {
        if (!mouse_move((int8_t)ddx, (int8_t)ddy)) return;
        stroke_sent_x += ddx;
        stroke_sent_y += ddy;
        pos_x += ddx;
        pos_y += ddy;
    }

    if (t >= 1.0f && stroke_sent_x == lroundf(s->dx) && stroke_sent_y == lroundf(s->dy)) {
        if (++stroke_idx < stroke_count) {
            start_stroke(now);
        } else if (frand() < GESTURE_BURST_CHANCE) {
            gesture_next_ms = now + urange(400, 2500);
        } else {
            gesture_next_ms = now + urange(GESTURE_PAUSE_MIN_MS, GESTURE_PAUSE_MAX_MS);
        }
    }
}

// --- BOOT button -------------------------------------------------------------

// The BOOT button pulls the flash chip-select low. Reading it means briefly
// releasing CS, so this must run from RAM with interrupts disabled
// (from pico-examples/picoboard/button).
static bool __no_inline_not_in_flash_func(get_bootsel_button)(void) {
    const uint CS_PIN_INDEX = 1;
    uint32_t flags = save_and_disable_interrupts();

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 1000; ++i);
    bool pressed = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);
    return pressed;
}

static void reset_mode(uint32_t now) {
    square_reset(now);
    realistic_reset(now);
}

static void on_long_press(uint32_t now) {
    enabled = !enabled;
    led_flash_until = now;
    reset_mode(now);
}

static void on_short_press(uint32_t now) {
    mode = (mode + 1) % MODE_COUNT;
    led_flash_until = now + MODE_FLASH_MS;
    reset_mode(now);
}

static void button_task(uint32_t now) {
    static uint32_t next_poll_ms, raw_since_ms, press_ms;
    static bool raw_last, stable, long_fired;

    if (!ms_reached(now, next_poll_ms)) return;
    next_poll_ms = now + 10;

    bool raw = get_bootsel_button();
    if (raw != raw_last) {
        raw_last = raw;
        raw_since_ms = now;
    }
    if (raw != stable && now - raw_since_ms >= DEBOUNCE_MS) {
        stable = raw;
        if (stable) {
            press_ms = now;
            long_fired = false;
        } else if (!long_fired) {
            on_short_press(now);
        }
    }
    // Fire the long press while still held, so the LED reacts immediately.
    if (stable && !long_fired && now - press_ms >= LONG_PRESS_MS) {
        long_fired = true;
        on_long_press(now);
    }
}

// -----------------------------------------------------------------------------

int main(void) {
    led_init();
    tusb_init();

    while (true) {
        tud_task();
        uint32_t now = to_ms_since_boot(get_absolute_time());

        button_task(now);

        // Don't move (and so never wake the host) while it is suspended.
        if (enabled && tud_mounted() && !tud_suspended()) {
            if (mode == MODE_SQUARE) square_task(now);
            else realistic_task(now);
        }

        led_update(now);
    }
}
