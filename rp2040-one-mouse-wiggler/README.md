# RP2040-One mouse wiggler

Firmware for the Waveshare RP2040-One that enumerates as a Logitech
"USB Optical Mouse" (046d:c077) and moves the pointer on its own. It never
clicks.

## Controls (onboard BOOT button)

| Action | Result |
| --- | --- |
| Long press (≥ 0.7 s) | Enable / disable. LED on while enabled. |
| Short press | Cycle mode. When disabled, the LED briefly flashes the new mode's colour. |

Modes:

- **Quiet (green, saved mode 0):** bounded straight-line drift, with one
  variable-size step every 3.2–14 seconds. Active-axis steps alternate between
  4–7 and 16–28 counts. Direction stays unchanged for at least six reports.
  Each active axis stays within ±192 counts of the activation point.
- **Sweep (blue, saved mode 1):** short straight sweeps of 6–8 variable-size
  reports, spaced 10–30 ms apart, followed by a 3.2–25 second rest. Active-axis
  steps alternate between 4–12 and 24–72 counts. Each active axis stays within
  ±400 counts of the activation point.

**Behavior change:** quiet replaces the old 2-count square and moves farther.
Sweep replaces the old curved “realistic” mode; it is not a model of human hand
motion. A horizontal, vertical or diagonal line is selected randomly at boot
and retained through mode changes and suspend/resume. There is no per-report
noise, circle, overshoot/correction pair, or exact return-to-origin requirement.

These bounds are raw HID counts, not screen pixels. Acceleration, screen edges
and other mice are invisible to the firmware. The bounded origin restarts when
enabling, switching modes or resuming/reconnecting, so repeated restarts can
produce cumulative screen drift.

The on/off state and the mode are saved to flash about 2 s after the last
button press, and restored when the board is plugged in again. That means it
starts wiggling straight away if it was on when you unplugged it. On first
use it starts disabled in quiet mode. Existing square/realistic settings map to
quiet/sweep respectively; the persisted format and mode numbers are unchanged.
While the host is suspended it does
nothing, so it won't wake a sleeping PC. Holding BOOT while plugging in still
enters the UF2 bootloader for reflashing.

The movement engine is the hardware-independent C library in `lib/wiggle/`.
Bounds and mode IDs are in `wiggle.h`; report-size ranges and scheduling are in
`wiggle.c`. LED/button settings remain in `src/main.c`. Late polls do not replay
missed reports, rejected USB submissions are retained for retry, and stale runs
are discarded on disable/disconnect/suspend. Buttons and wheel remain zero.

## Detector validation

The [host framework](../jiggler-detection-test/README.md) compiles the **same
library** as the firmware and the unmodified vendored detector. Both modes pass
its strict detection test in all three calibration scenarios. No mode is
exempted, no thresholds are increased, and no fake clicks or touchpad identities
are used. The existing USB identity is unchanged.

Extended validation: **zero detections across 600 two-hour mode/scenario/poll
combinations**: two modes × three scenarios × twenty holdout seeds (1000–1019)
× five polling intervals (1, 8, 10, 16, 32 ms). Highest behavior/combined scores
were 60/36; primary qualifiers remained at zero. The circle positive control
still triggered in every scenario. Another 644 generator cases check bounds,
variance, reversal counts, timing, stalls, reset behavior and timer rollover.

This means **not detected by the tested framework**, not universally undetectable.
Geometric and alternating-direction heuristics can still contribute 30 points
each. The fixed-line pattern could be recognized by other/future detectors.
Results depend on the existing allowlisted VID: the stock Pico VID `2E8A`
correctly triggers detection in a negative control. Real USB delivery, merged
or lost Raw Input reports, old movement histories and repeated manual mode
cycling are not equivalent to the fresh, steady-mode simulated runs. Actual
hardware testing is still required; the UF2 has been built but not flashed.

See the framework README for reproducible commands and the score rationale.

## USB identity

`src/usb_descriptors.c` reproduces the `lsusb -v` output of the real mouse:
VID/PID, bcdDevice 72.00, strings "Logitech" / "USB Optical Mouse", no
serial number, bMaxPacketSize0 8, one boot-mouse interface, 100 mA, remote
wakeup, 4-byte EP 0x81 at bInterval 10, and a 46-byte report descriptor.
It has no extra CDC/serial interface.

The 46-byte report descriptor was taken from the real mouse with
`usbhid-dump`. To compare the wiggler against a mouse on Linux:

```sh
sudo usbhid-dump -d 046d:c077 -e descriptor
```

The one difference that can't be removed: the RP2040's USB controller only
works as a **full-speed** device, so lsusb shows `Full Speed (12Mbps)`
instead of `Low Speed (1Mbps)`.

## Building

Requires the [Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.x (with
the `lib/tinyusb` submodule) and an `arm-none-eabi` toolchain.

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja
ninja -C build
```

You can also open this folder with the Raspberry Pi Pico VS Code extension
("Import Project").

## Flashing

Hold BOOT while plugging the board in. An `RPI-RP2` drive appears; copy
`build/mouse_wiggler.uf2` onto it.
