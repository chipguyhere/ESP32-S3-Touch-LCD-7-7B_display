/*******************************************************************************
 * Waveshare ESP32-S3-Touch-LCD-7 / -7B LVGL Calculator
 *
 * For the Waveshare ESP32-S3-Touch-LCD-7 / -7B (1024x600 RGB LCD
 * + GT911 capacitive touch).
 *
 * A working four-function calculator with a touch keypad, styled after the
 * familiar dark iOS calculator: a black background, dark-gray number keys, a
 * light-gray top function row (backspace / C / %) and an orange operator
 * column (/ x - + =).
 *
 * Resolution independence is the point of this example.  The whole UI is laid
 * out from the live display resolution in ui.cpp — button diameter, gaps,
 * margins and font sizes are all derived from width()/height() at runtime.
 * Nothing is hard-coded to either panel's size, so the identical ui.cpp lays out a
 * proportionate keypad on a different panel without edits.  See the layout
 * math in build_keypad() in ui.cpp.  (The calculator is adapted from the
 * rectangular ESP32-S3-Touch-LCD-2 version, since its grid suits a rectangle.)
 *
 * Requires the following libraries:
 *   - chipguy_Waveshare-ESP32-S3-Touch-LCD-7-7B_display  (display + touch, this library)
 *   - chipguy_Waveshare-ESP32-S3-Touch-LCD-7-7B_board   (shared I2C bus + expander; a dependency)
 *   - lvgl 9
 *
 * Arduino IDE Board Settings (Tools menu):
 *   - Board: "ESP32S3 Dev Module" (in Tools > Board > esp32)
 *   - Flash Size: "8MB (64Mb)" works on both boards ("16MB (128Mb)" uses all of
 *     the -7B's flash)
 *   - PSRAM: "OPI PSRAM"   (the board has 8MB PSRAM)
 *   - Serial output: the USB-C port marked UART (on the -7B, with the
 *     UART1/UART2 switch on UART1).  Serial0 always prints there.
 *
 ******************************************************************************/

// The bundled lv_conf.h is based on LVGL 9.3 (LVGL 9 compatible).
// It's important for the config and library versions to be compatible.
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

// The calculator UI lives in ui.h / ui.cpp in this sketch folder.
#include "ui.h"

void setup() {
    // Serial0 is UART0, which reaches the USB-C port marked UART (on the
    // -7B, when the UART1/UART2 switch is on UART1).
    Serial0.begin(115200);

    // Initialize display, touch, and LVGL.  The UI adapts to whatever
    // resolution the display reports, so try lv_setup.begin(90) or begin(270)
    // for a portrait (600x1024 or 480x800) calculator; begin(180) flips an upside-down
    // panel.  See the README.
    // Half resolution: LVGL draws 512x300 (-7B) or 400x240 (-7) and the display scales it up 2x,
    // smoothly.  Much faster motion and scrolling; small text looks softer.
    // Good for walk-up UIs with big buttons.  Uncomment to try it:
    // lv_setup.halfResolution = true;
    lv_setup.begin();
    Serial0.printf("LVGL initialized with %dx%d touchscreen\n",
                   display.width(), display.height());

    // Build the calculator screen.
    ui_init();
}

void loop() {
    // Give loop control to LVGL.
    lv_timer_handler();
    delay(5);
}
