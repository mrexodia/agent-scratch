// Mouse wiggler for the Waveshare RP2040-One.
//
// Press the button to toggle wiggling on/off. While wiggling, the onboard
// RGB LED (WS2812 on GPIO16) lights up and the mouse pointer is nudged a few
// pixels back and forth every WIGGLE_INTERVAL_MS.
//
// Button: by default the onboard BOOT button is used (read via the arduino-pico
// BOOTSEL keyword), so no extra hardware is needed. Holding BOOT while plugging
// the board in still enters the UF2 bootloader as usual. Optionally wire a push
// button between EXT_BUTTON_PIN and GND as well.
//
// Build with the arduino-pico core (Earle Philhower), board
// "Waveshare RP2040 One" (or "Raspberry Pi Pico"), and install the
// "Adafruit NeoPixel" library.

#include <Mouse.h>
#include <Adafruit_NeoPixel.h>

const uint8_t LED_PIN = 16;            // onboard WS2812
const int EXT_BUTTON_PIN = 29;         // optional external button to GND; -1 to disable
const uint32_t WIGGLE_INTERVAL_MS = 5000;
const int8_t WIGGLE_PIXELS = 4;
const uint32_t DEBOUNCE_MS = 50;

Adafruit_NeoPixel led(1, LED_PIN, NEO_GRB + NEO_KHZ800);

bool wiggling = false;
bool lastPressed = false;
uint32_t lastChangeMs = 0;
uint32_t lastWiggleMs = 0;

bool buttonPressed() {
  bool pressed = BOOTSEL;
  if (EXT_BUTTON_PIN >= 0 && digitalRead(EXT_BUTTON_PIN) == LOW) {
    pressed = true;
  }
  return pressed;
}

void setLed(bool on) {
  led.setPixelColor(0, on ? led.Color(0, 40, 0) : 0);  // dim green when active
  led.show();
}

void wiggle() {
  Mouse.move(WIGGLE_PIXELS, 0, 0);
  delay(50);
  Mouse.move(-WIGGLE_PIXELS, 0, 0);  // return to the original position
}

void setup() {
  if (EXT_BUTTON_PIN >= 0) {
    pinMode(EXT_BUTTON_PIN, INPUT_PULLUP);
  }
  led.begin();
  setLed(false);
  Mouse.begin();
}

void loop() {
  uint32_t now = millis();

  bool pressed = buttonPressed();
  if (pressed != lastPressed && now - lastChangeMs > DEBOUNCE_MS) {
    lastChangeMs = now;
    lastPressed = pressed;
    if (pressed) {  // toggle on press, not on release
      wiggling = !wiggling;
      setLed(wiggling);
      if (wiggling) {
        wiggle();
        lastWiggleMs = now;
      }
    }
  }

  if (wiggling && now - lastWiggleMs >= WIGGLE_INTERVAL_MS) {
    lastWiggleMs = now;
    wiggle();
  }

  delay(10);
}
