/*******************************************************************************
 * Waveshare ESP32-S3-Touch-LCD-7 / -7B LVGL Bouncing Balls
 *
 * A resolution-independent demo: 1..20 circular balls bounce under gravity,
 * each a random color, re-launching to a random height on every floor bounce.
 * Two sliders at the bottom select the number of balls and the speed.
 *
 * The whole UI is laid out from the live display resolution at runtime (see
 * ui.cpp), so this sketch's ui.cpp / ui.h drop unchanged into any board whose
 * lv_setup.hpp follows the same shape, in any orientation.
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
 *   - Arduino Runs On: Core 0 for the best frame rate (Core 1 works too; the
 *     screen then shows a note, and its "Why?" button explains the choice)
 *   - Serial output: the USB-C port marked UART (on the -7B, with the
 *     UART1/UART2 switch on UART1).  Serial0 always prints there.
 *
 * Copyright (c) 2025 chipguyhere
 * MIT License
 ******************************************************************************/

// The bundled lv_conf.h is based on LVGL 9.3 (LVGL 9 compatible).
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

// The bouncing-balls UI lives in ui.h / ui.cpp in this sketch folder.
#include "ui.h"

void setup() {
    // Serial0 is UART0, which reaches the USB-C port marked UART (on the
    // -7B, when the UART1/UART2 switch is on UART1).
    Serial0.begin(115200);

    // Initialize display, touch, and LVGL.
    // Rotation: lv_setup.begin(180) for an upside-down panel, or begin(90) /
    // begin(270) for portrait (600x1024 or 480x800).  The demo adapts to whatever resolution
    // the chosen orientation reports.
    // Half resolution: LVGL draws 512x300 (-7B) or 400x240 (-7) and the display scales it up 2x,
    // smoothly.  Much faster motion and scrolling; small text looks softer.
    // Good for walk-up UIs with big buttons.  Uncomment to try it:
    // lv_setup.halfResolution = true;
    lv_setup.begin();
    Serial0.printf("LVGL initialized with %dx%d touchscreen\n",
                   display.width(), display.height());

    // Build the bouncing-balls screen.
    ui_init();

    // setup() and loop() -- and so LVGL -- run on the core chosen in
    // Tools > Arduino Runs On; the display driver works on core 1.  Show both,
    // with a nudge when they share core 1.
    show_core_notice();
    show_perf_label();
}

// ---- Frame rate and per-core CPU load, bottom right ----
// (LVGL's own performance monitor times its loop with lv_tick, which this
// board's lv_setup advances once per panel refresh, so it can't see CPU load.
// The display library reads each core's idle time from FreeRTOS instead.)

static volatile uint32_t g_renders = 0;
static lv_obj_t *g_perf;

static void count_render(lv_event_t *e) {
    LV_UNUSED(e);
    g_renders++;
}

static void perf_update(lv_timer_t *t) {
    LV_UNUSED(t);
    static uint32_t lastMs = 0, lastRenders = 0;
    uint32_t now = millis(), r = g_renders;
    uint32_t fps = (now != lastMs) ? (r - lastRenders) * 1000 / (now - lastMs) : 0;
    lastMs = now;
    lastRenders = r;
    lv_label_set_text_fmt(g_perf, "%u FPS | CPU %u%%,%u%%", (unsigned)fps,
                          (unsigned)(100 - cglcd7_7b_idle_percent_core0()),
                          (unsigned)(100 - cglcd7_7b_idle_percent_core1()));
}

static void show_perf_label() {
    lv_display_add_event_cb(lv_display_get_default(), count_render, LV_EVENT_RENDER_READY, NULL);
    g_perf = lv_label_create(lv_layer_top());
    lv_obj_set_style_bg_color(g_perf, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_perf, LV_OPA_50, 0);
    lv_obj_set_style_text_color(g_perf, lv_color_white(), 0);
    lv_obj_set_style_pad_all(g_perf, 4, 0);
    lv_obj_align(g_perf, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_label_set_text(g_perf, "");
    lv_timer_create(perf_update, 1000, NULL);
}

// ---- Which core runs what, bottom left, with a [Why] explanation ----

static const char *kWhyText =
    "This board's screen has no memory of its own, so this display driver "
    "re-sends every pixel from PSRAM about 33 times a second. That work, and "
    "copying what LVGL draws into the picture, always runs on core 1: both "
    "use the PSRAM bus, and on one core they take turns instead of colliding.\n\n"
    "LVGL itself mostly works in internal RAM. When Arduino (setup, loop, and "
    "so LVGL) runs on core 0, LVGL draws the next part of the screen while "
    "core 1 moves pixels. With many balls that is about half again as many "
    "frames per second. Under the heaviest loads you may rarely see a brief "
    "flicker band.\n\n"
    "This is advice about THIS display driver, not a rule for every "
    "sketch.\n\n"
    "A sketch that uses PSRAM heavily (image assets, big buffers, or any "
    "other reason) should usually stay on core 1. On core 1, the display's "
    "work can interrupt it, so the two take turns with PSRAM. On core 0, "
    "nothing makes it give way, and its PSRAM traffic can stall the "
    "display. That kind of app often cares little about animation frame "
    "rate anyway. Keeping everything on core 1 keeps all the heavy PSRAM "
    "use on one core. Measure your own app before you move it.\n\n"
    "To change it: Tools > Arduino Runs On > Core 0.";

static void why_clicked(lv_event_t *e) {
    LV_UNUSED(e);
    lv_obj_t *mb = lv_msgbox_create(NULL);   // modal
    lv_msgbox_add_title(mb, "Why core 0?");
    lv_msgbox_add_text(mb, kWhyText);
    lv_msgbox_add_close_button(mb);
    int32_t w = lv_display_get_horizontal_resolution(NULL) - 40;
    lv_obj_set_width(mb, w < 560 ? w : 560);
    // Fit the screen height; the text scrolls inside if it's taller.
    lv_obj_set_style_max_height(mb, lv_display_get_vertical_resolution(NULL) - 40, 0);
    lv_obj_set_flex_grow(lv_msgbox_get_content(mb), 1);
}

static void show_core_notice() {
    lv_obj_t *scr = lv_screen_active();
    int32_t W = lv_display_get_horizontal_resolution(NULL);

    // A row along the bottom left, leaving the bottom right to the
    // frame-rate / CPU label.
    int32_t rowW = W - 24 - 190;
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, rowW, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 12, -6);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    int app = xPortGetCoreID();
    lv_obj_t *msg = lv_label_create(row);
    if (app == LV_SETUP_DISPLAY_CORE)
        lv_label_set_text_fmt(msg, "Arduino on core %d, display on core %d. Run Arduino on core 0 "
                              "for improved performance.", app, LV_SETUP_DISPLAY_CORE);
    else
        lv_label_set_text_fmt(msg, "Arduino on core %d, display on core %d.", app, LV_SETUP_DISPLAY_CORE);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(msg, app == LV_SETUP_DISPLAY_CORE ? lv_color_hex(0xFFD060)
                                                                  : lv_color_hex(0xA0A0B0), 0);
    // As wide as its text, so the button sits right after it; wraps only if
    // the row is too narrow for both (portrait).
    lv_obj_set_width(msg, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(msg, rowW - 70, 0);

    lv_obj_t *btn = lv_button_create(row);
    lv_obj_set_style_pad_hor(btn, 12, 0);
    lv_obj_set_style_pad_ver(btn, 4, 0);
    lv_obj_add_event_cb(btn, why_clicked, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(btn), "Why?");
}

void loop() {
    // Give loop control to LVGL.
    lv_timer_handler();
    delay(5);
}
