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

- **Square (green):** moves 2 px per second around a 2 px square. The
  cursor stays put overall, so you can keep using the computer.
- **Realistic (blue):** every 3–25 s (sometimes in quick bursts) it makes a
  fast, slightly curved swipe to a random spot. Swipes speed up and slow
  down like a hand, with small tremor, sometimes overshoot and correct, and
  sometimes end with a small adjustment. Targets are kept within roughly
  ±700 × ±400 counts of where the cursor started.

The on/off state and the mode are saved to flash about 2 s after the last
button press, and restored when the board is plugged in again. That means it
starts wiggling straight away if it was on when you unplugged it. On first
use it starts disabled in square mode. While the host is suspended it does
nothing, so it won't wake a sleeping PC. Holding BOOT while plugging in still
enters the UF2 bootloader for reflashing.

Timings, sizes, colours and the long-press threshold are `#define`s at the
top of `src/main.c`.

## USB identity

`src/usb_descriptors.c` reproduces the `lsusb -v` output of the real mouse:
VID/PID, bcdDevice 72.00, strings "Logitech" / "USB Optical Mouse", no
serial number, bMaxPacketSize0 8, one boot-mouse interface, 100 mA, remote
wakeup, 4-byte EP 0x81 at bInterval 10, and a 46-byte report descriptor.
It has no extra CDC/serial interface.

The report descriptor bytes are the known c077 descriptor (8 buttons + X/Y/wheel,
46 bytes); lsusb printed `UNAVAILABLE`, so they weren't taken from your
dump. To compare against your own mouse on Linux:

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
