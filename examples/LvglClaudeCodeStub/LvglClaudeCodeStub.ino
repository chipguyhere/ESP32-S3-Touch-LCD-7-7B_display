/*******************************************************************************
 * Waveshare ESP32-S3-Touch-LCD-7 / -7B LVGL 9.3 Claude Code Stub
 *
 * A minimal starting point for building an LVGL application from scratch on
 * the Waveshare ESP32-S3-Touch-LCD-7 / -7B development board (1024x600 RGB LCD +
 * GT911 capacitive touch).
 *
 * This is the same scaffold as the Lvgl93SquarelineLauncher example, but with
 * the SquareLine-generated UI removed.  Instead of designing the interface
 * visually and exporting it, you write the UI yourself: by hand, or by handing
 * this sketch to an AI coding assistant such as Claude Code.
 *
 * The UI lives in a tiny ui.h / ui.cpp pair in this sketch folder.  All of the
 * display, touch, and LVGL plumbing is already wired up (see lv_setup.hpp)
 * before ui_init() is called, so ui_init() is a blank canvas: it receives a
 * live LVGL screen and you create widgets on it.  To start your app, open
 * ui.cpp and replace the contents of ui_init() with your own widgets.
 *
 * Because everything except the UI is already wired up, this scaffold is ideal
 * to hand to Claude Code: point it at this sketch folder and describe the app
 * you want.  It can focus entirely on ui.cpp and the screens you add, without
 * touching the display/touch/LVGL plumbing.
 *
 * Requires the following libraries:
 *   - chipguy_Waveshare-ESP32-S3-Touch-LCD-7-7B_display  (display + touch, this library)
 *   - chipguy_Waveshare-ESP32-S3-Touch-LCD-7-7B_board   (shared I2C bus + expander; a dependency)
 *   - lvgl 9
 *
 * Arduino IDE Board Settings:
 *   - Board: ESP32S3 Dev Module
 *   - PSRAM: OPI PSRAM
 *   - Upload Speed: 230400  (faster rates corrupted on the -7B's CH343 link;
 *     the -7 takes 460800)
 *   - Flash Size: 8MB (64Mb) and Partition Scheme: 8M with spiffs (3MB APP/1.5MB
 *     SPIFFS) work on both boards.  The -7B has 16 MB: 16MB (128Mb) and
 *     16M Flash (3MB APP/9.9MB FATFS) use all of it.
 *   - Serial output: the USB-C port marked UART, with the UART1/UART2
 *     switch on UART1.  Serial0 always prints there.
 *
 * Copyright (c) 2025 chipguyhere
 * MIT License
 ******************************************************************************/

// This is built for LVGL 9.3, the same version SquareLine generates for.
// It's important for the config and library versions to be compatible.
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"


// Fonts
// The Montserrat font is built into LVGL in multiple sizes, each size takes up memory.
// We enable the 14 point font as a default font.
#define LV_FONT_MONTSERRAT_14 1
// need other sizes?  Edit lv_conf.h where they are turned off (0) and turn them on (1)

// The UI for this example lives in ui.h / ui.cpp in this sketch folder.
// It is a hand-written "Hello, world!" screen meant as a starting point.
// Build your own application by editing ui_init() in ui.cpp.
#include "ui.h"


void setup() {
    // Serial0 is UART0, which reaches the USB-C port marked UART when the
    // UART1/UART2 switch is on UART1.
    Serial0.begin(115200);

    // Initialize display, touch, and LVGL.
    // Rotation: lv_setup.begin(180) for an upside-down panel, or begin(90) /
    // begin(270) for portrait (600x1024 or 480x800).  See the README.
    // Half resolution: LVGL draws 512x300 (-7B) or 400x240 (-7) and the display scales it up 2x,
    // smoothly.  Much faster motion and scrolling; small text looks softer.
    // Good for walk-up UIs with big buttons.  Uncomment to try it:
    // lv_setup.halfResolution = true;
    lv_setup.begin();
    Serial0.printf("LVGL initialized with %dx%d touchscreen\n", display.width(), display.height());

    // Start the application's own setup
    ui_init();
}


void loop() {
    // Give loop control to LVGL objects created by the application
    lv_timer_handler(); /* let the GUI do its work */
    delay(5);
}
