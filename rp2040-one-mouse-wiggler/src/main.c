// Mouse wiggler for the Waveshare RP2040-One.
//
// BOOT button:
//   long press  (>= LONG_PRESS_MS) - enable / disable wiggling
//   short press                    - cycle mode (LED flashes the new mode's colour)
//
// Enabled state and mode are saved to flash and restored on power-up.
//
// Modes (movement generation lives in lib/wiggle):
//   square    - 2 px square, one step per second. Net movement is zero, so the
//               computer stays usable while it runs.
//   realistic - every few seconds a quick, slightly curved swipe across the
//               screen, sometimes with an overshoot and correction. Never clicks.

#include <string.h>

#include "pico/stdlib.h"
#include "pico/rand.h"
#include "hardware/flash.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "tusb.h"

#include "ws2812.pio.h"
#include "wiggle.h"

#define LED_PIN                 PICO_DEFAULT_WS2812_PIN
#define LED_BRIGHTNESS          32
#define MODE_FLASH_MS           500

#define DEBOUNCE_MS             30
#define LONG_PRESS_MS           700

static const uint8_t mode_colour[WIGGLE_MODE_COUNT][3] = {
    [WIGGLE_MODE_SQUARE]    = {0, LED_BRIGHTNESS, 0},  // green
    [WIGGLE_MODE_REALISTIC] = {0, 0, LED_BRIGHTNESS},  // blue
};

static bool enabled;
static wiggle_mode_t mode = WIGGLE_MODE_SQUARE;
static wiggle_t wiggler;

static inline bool ms_reached(uint32_t now, uint32_t t) {
    return (int32_t)(now - t) >= 0;
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
    uint8_t report[4] = {0, (uint8_t)dx, (uint8_t)dy, 0};
    // In boot protocol the host expects the 3-byte boot mouse report.
    uint16_t len = tud_hid_get_protocol() == HID_PROTOCOL_BOOT ? 3 : 4;
    return tud_hid_report(0, report, len);
}

// --- Saved settings ----------------------------------------------------------

// The last flash sector holds a log of settings records, one per 256-byte
// page. Each save programs the next blank page, and the sector is only erased
// once all 16 pages are used, which keeps flash wear low. On boot the last
// valid record wins.

#define SETTINGS_OFFSET   (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define SETTINGS_SLOTS    (FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE)
#define SETTINGS_MAGIC    0x57494747u  // "WIGG"
#define SAVE_DELAY_MS     2000         // let quick mode cycling settle first

typedef struct {
    uint32_t magic;
    uint8_t enabled;
    uint8_t mode;
    uint8_t check;  // enabled ^ mode ^ 0xA5
    uint8_t reserved;
} settings_t;

static int settings_next_slot;
static bool settings_dirty;
static uint32_t settings_save_ms;

static const settings_t *settings_slot(int slot) {
    return (const settings_t *)(XIP_BASE + SETTINGS_OFFSET + slot * FLASH_PAGE_SIZE);
}

static bool settings_valid(const settings_t *s) {
    return s->magic == SETTINGS_MAGIC && s->mode < WIGGLE_MODE_COUNT && s->enabled <= 1 &&
           s->check == (uint8_t)(s->enabled ^ s->mode ^ 0xA5);
}

static void settings_load(void) {
    settings_next_slot = SETTINGS_SLOTS;  // no blank page: erase on next save
    for (int i = 0; i < SETTINGS_SLOTS; i++) {
        const settings_t *s = settings_slot(i);
        if (settings_valid(s)) {
            enabled = s->enabled;
            mode = (wiggle_mode_t)s->mode;
        } else if (s->magic == 0xFFFFFFFFu) {
            settings_next_slot = i;
            break;
        }
    }
}

static void settings_changed(uint32_t now) {
    settings_dirty = true;
    settings_save_ms = now + SAVE_DELAY_MS;
}

static void settings_task(uint32_t now) {
    if (!settings_dirty || !ms_reached(now, settings_save_ms)) return;
    settings_dirty = false;

    const settings_t *last = NULL;
    for (int i = settings_next_slot - 1; i >= 0 && !last; i--) {
        if (settings_valid(settings_slot(i))) last = settings_slot(i);
    }
    if (last && last->enabled == enabled && last->mode == mode) return;

    static uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof page);
    settings_t rec = {SETTINGS_MAGIC, enabled, (uint8_t)mode, (uint8_t)(enabled ^ mode ^ 0xA5), 0xFF};
    memcpy(page, &rec, sizeof rec);

    // Code runs from flash, so it must not be touched while erasing or
    // programming. This only runs on core 0 and the flash functions are in RAM.
    uint32_t flags = save_and_disable_interrupts();
    if (settings_next_slot >= SETTINGS_SLOTS) {
        flash_range_erase(SETTINGS_OFFSET, FLASH_SECTOR_SIZE);
        settings_next_slot = 0;
    }
    flash_range_program(SETTINGS_OFFSET + settings_next_slot * FLASH_PAGE_SIZE, page, FLASH_PAGE_SIZE);
    restore_interrupts(flags);
    settings_next_slot++;
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
    wiggle_start(&wiggler, mode, now);
}

static void on_long_press(uint32_t now) {
    enabled = !enabled;
    led_flash_until = now;
    reset_mode(now);
    settings_changed(now);
}

static void on_short_press(uint32_t now) {
    mode = (mode + 1) % WIGGLE_MODE_COUNT;
    led_flash_until = now + MODE_FLASH_MS;
    reset_mode(now);
    settings_changed(now);
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
    settings_load();
    wiggle_init(&wiggler, get_rand_32);
    led_init();
    reset_mode(0);
    tusb_init();

    while (true) {
        tud_task();
        uint32_t now = to_ms_since_boot(get_absolute_time());

        button_task(now);
        settings_task(now);

        // Don't move (and so never wake the host) while it is suspended.
        if (enabled && tud_mounted() && !tud_suspended()) {
            int8_t dx, dy;
            if (tud_hid_ready() && wiggle_poll(&wiggler, now, &dx, &dy)) mouse_move(dx, dy);
        }

        led_update(now);
    }
}
