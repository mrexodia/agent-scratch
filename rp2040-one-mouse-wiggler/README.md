# RP2040-One mouse wiggler

Plug the Waveshare RP2040-One into a USB port and it shows up as a USB mouse.
Press the **BOOT** button to start wiggling: the onboard RGB LED turns green
and the pointer moves 4 px right and back every 5 s. Press it again to stop.

## Flashing (Arduino IDE)

1. Add the board manager URL
   `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`
   and install **Raspberry Pi Pico/RP2040/RP2350** (arduino-pico).
2. Library Manager: install **Adafruit NeoPixel**.
3. Tools > Board > **Waveshare RP2040 One**.
4. Hold BOOT while plugging in (an `RPI-RP2` drive appears), then Upload.

Or with arduino-cli:

```sh
arduino-cli compile --upload -b rp2040:rp2040:waveshare_rp2040_one rp2040-one-mouse-wiggler
```

## Tweaks

Constants at the top of the sketch: `WIGGLE_INTERVAL_MS`, `WIGGLE_PIXELS`,
LED colour in `setLed()`, and `EXT_BUTTON_PIN` (optional extra push button
between that GPIO and GND; set to `-1` to disable).
