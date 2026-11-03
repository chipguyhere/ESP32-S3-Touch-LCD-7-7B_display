/*
 * lv_setup.hpp - The bridge between LVGL and your display + touch hardware.
 *
 * THIS is the file that wires LVGL up to a specific board.  Right now it just
 * pulls in the ready-made setup that ships inside the
 * chipguy_Waveshare-ESP32-S3-Touch-LCD-7-7B_display library, so you don't have to maintain a
 * copy:
 *
 *   - It creates a global `board`       (shared I2C bus + I/O expander)
 *   - It creates a global `display`     (the ESP32-S3-Touch-LCD-7B RGB driver)
 *   - It creates a global `lv_setup`    (an lv_setup_class instance)
 *   - Calling lv_setup.begin(rotation)  initializes display + touch + LVGL
 *
 * --------------------------------------------------------------------------
 *  TARGETING A DIFFERENT BOARD
 *  Point the #include below at a different library's setup header and the same
 *  sketch (ui.cpp / ui.h, etc.) drives a different board — as long as that
 *  header exposes the same `display` / `lv_setup` shape.
 *
 *  CUSTOMIZING HOW LVGL IS WIRED UP
 *  Because this file lives in your sketch folder, it's the easy place to take
 *  control.  Open the real implementation at:
 *
 *      <Arduino libraries>/chipguy_Waveshare-ESP32-S3-Touch-LCD-7-7B_display/src/lv_setup_ESP32S3_Touch_LCD_7_7B.hpp
 *
 *  copy its entire contents over the #include line below, and edit freely —
 *  draw-band size, rotation handling, the DIRECT/rotated flush callbacks, the
 *  touch coordinate mapping, etc.  Nothing else in the sketch needs to change.
 * --------------------------------------------------------------------------
 *
 * Include this file after lv_conf.h and lvgl.h in your sketch.
 *
 *
 * For reference, the setup class this pulls in looks like:
 *
 *   // A global instance named `lv_setup` is created for you.
 *   class lv_setup_class {
 *   public:
 *       // Initialize display, touch, and LVGL.  Call once from setup().
 *       // rotation: 0 / 180 are handled in the driver (DIRECT render); 90 / 270
 *       //           render PARTIAL in portrait and rotate into the landscape
 *       //           framebuffers.  Index aliases 1/2/3 work for 90/180/270.
 *       void begin(uint16_t rotation = 0);
 *   };
 *
 *   // Also provided as globals: the board (shared I2C bus + I/O expander)
 *   // and the display driver, handy for resolution:
 *   //   display.width(), display.height()
 *   extern ESP32S3_Touch_LCD_7_7B_Board board;
 *   extern ESP32S3_Touch_LCD_7_7B      display;
 *
 * Tip: this panel uses a GT911 capacitive touch controller (no calibration
 * needed).  If touch comes out mirrored, see the rotation mapping in the real
 * implementation's _touchpad_read.
 */

#pragma once

#include <lv_setup_ESP32S3_Touch_LCD_7_7B.hpp>
