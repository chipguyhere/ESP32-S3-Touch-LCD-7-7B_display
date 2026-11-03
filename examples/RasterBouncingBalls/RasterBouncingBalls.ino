/*******************************************************************************
 * Waveshare ESP32-S3-Touch-LCD-7 / -7B -- Bouncing balls at the panel's refresh rate
 *
 * A proof of concept: the top of the screen bypasses LVGL entirely.  The
 * display driver's bounce fill calls balls_raster() (balls.cpp) for each
 * 20-row chunk of the ball area just before the panel scans it, and the balls
 * are drawn straight into the SRAM bounce buffer -- no framebuffer, no PSRAM.
 * Only the control strip at the bottom is LVGL: two sliders (ball count up to
 * 200, and speed), a see-through (75 % opaque) switch and the frame-rate / CPU readout.
 *
 * Because the balls are drawn while the panel scans, every panel refresh is a
 * frame of the animation, and the physics advances once per refresh from one
 * snapshot, so nothing tears.
 *
 * Requires the following libraries:
 *   - chipguy_Waveshare-ESP32-S3-Touch-LCD-7-7B_display  (this library)
 *   - chipguy_Waveshare-ESP32-S3-Touch-LCD-7-7B_board
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
 *   - Arduino Runs On: Core 0 (Core 1 works too; the balls are drawn on core 1
 *     either way, by the display's fill task)
 *   - Serial output: the USB-C port marked UART (on the -7B, with the
 *     UART1/UART2 switch on UART1).  Serial0 prints a status line every second.
 *
 * Copyright (c) 2026 chipguyhere
 * MIT License
 ******************************************************************************/

#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"
#include "balls.h"

// Pixel clock.  0 = the board's default: 30 MHz on the -7B (32.7 Hz;
// refresh = pclk / 916146), 14 MHz on the -7 (34.1 Hz; pclk / 410000).
// The driver prints the figure at boot.  -7B: faster clocks broke the
// picture (40 MHz and 51.2 MHz tried, 2026-10-30), even with nothing read
// from PSRAM.  -7: Waveshare's stock 16 MHz is ~39 Hz; faster clocks give
// each chunk less time to draw -- watch "chunk drawn in" and "late".
#ifndef RASTER_PCLK_HZ
#define RASTER_PCLK_HZ 0
#endif

// The LVGL control strip's height at the bottom; the balls get the rows
// above it (440 on the -7B, 320 on the -7).  A multiple of the bounce
// buffer height (20 rows).
#define STRIP_ROWS 160
static_assert(STRIP_ROWS % ESP32S3_Touch_LCD_7_7B::bounceRows() == 0,
              "STRIP_ROWS must be a multiple of the bounce buffer height");
static int g_ballRows = 0;   // display.height() - STRIP_ROWS, set in setup()

#define START_BALLS 100
#define START_SEE_THROUGH true
// Speed slider: 1..100, where 100 is the LVGL BouncingBalls demo's speed.
// It is labeled so that 20 (a fifth of that, slow enough to follow 100+ balls)
// reads 1.0x and the top reads 5.0x.
#define START_SPEED 20
static void speed_text(lv_obj_t *label, int v) {
    lv_label_set_text_fmt(label, "Speed: %d.%02dx", v * 5 / 100, v * 5 % 100);
}

static lv_obj_t *g_countLabel, *g_speedLabel, *g_perf;
static uint32_t g_hz10 = 0;   // the panel's refresh rate x10, measured at boot

static void count_cb(lv_event_t *e) {
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    balls_set_count(v);
    lv_label_set_text_fmt(g_countLabel, "Balls: %d", v);
}

static void speed_cb(lv_event_t *e) {
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));   // 1..100: 0.05x .. 5.00x
    balls_set_speed(v);
    speed_text(g_speedLabel, v);
}

static void see_through_cb(lv_event_t *e) {
    balls_set_see_through(lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED));
}

// Once a second: frames the ball area drew (one per panel refresh unless one
// was late), per-core CPU, and the longest time balls_raster() took for one
// 20-row chunk against the time it has (one bounce period).
static void perf_update(lv_timer_t *t) {
    LV_UNUSED(t);
    static uint32_t lastMs = 0, lastFrames = 0, lastLate = 0;
    uint32_t now = millis(), f = balls_frames(), late = g_rasterLate;
    uint32_t fps10 = (now != lastMs) ? (f - lastFrames) * 10000 / (now - lastMs) : 0;
    uint32_t fnUs = g_rasterMaxFnUs;
    g_rasterMaxFnUs = 0;
    uint32_t c0 = 100 - cglcd7_7b_idle_percent_core0(), c1 = 100 - cglcd7_7b_idle_percent_core1();
    const uint32_t hz10 = g_hz10;
    // Counting whole frames over about a second rounds to +/-0.1: within
    // 0.5 of the panel's rate, every frame was drawn, so show that rate.
    uint32_t shown10 = (fps10 + 5 >= hz10 && fps10 <= hz10 + 5) ? hz10 : fps10;
    // The bounce period: the time each chunk has (measured by the driver).
    uint32_t chunkUs = g_bbPeriodUs;
    lv_label_set_text_fmt(g_perf,
        "%u.%u FPS (panel %u.%u Hz) | CPU %u%%,%u%%\n"
        "chunk drawn in %u of %u us | late %u",
        (unsigned)(shown10 / 10), (unsigned)(shown10 % 10), (unsigned)(hz10 / 10), (unsigned)(hz10 % 10),
        (unsigned)c0, (unsigned)c1, (unsigned)fnUs, (unsigned)chunkUs, (unsigned)(late - lastLate));
    Serial0.printf("fps %u.%u  cpu %u,%u  chunk %u/%u us  late %u (total %u)  dma late %u  dropped %u\n",
                   (unsigned)(fps10 / 10), (unsigned)(fps10 % 10), (unsigned)c0, (unsigned)c1,
                   (unsigned)fnUs, (unsigned)chunkUs, (unsigned)(late - lastLate), (unsigned)late,
                   (unsigned)g_bbDmaLate, (unsigned)g_bbDroppedCount);
    lastMs = now;
    lastFrames = f;
    lastLate = late;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *make_slider(lv_obj_t *parent, int x, int y, int w, int lo, int hi, int v,
                             lv_event_cb_t cb) {
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_size(s, w, 16);
    lv_obj_set_pos(s, x, y);
    lv_slider_set_range(s, lo, hi);
    lv_slider_set_value(s, v, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_ext_click_area(s, 12);
    return s;
}

static void build_strip() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x181820), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    const int W = display.width();
    const int m = 16, labelW = 120, swColW = 120;
    const int sliderX = m + labelW, sliderW = W - sliderX - swColW - m;
    int y = g_ballRows + 14;

    g_countLabel = make_label(scr, "");
    lv_label_set_text_fmt(g_countLabel, "Balls: %d", START_BALLS);
    lv_obj_set_pos(g_countLabel, m, y);
    make_slider(scr, sliderX, y + 1, sliderW, 1, BALLS_MAX, START_BALLS, count_cb);
    balls_set_count(START_BALLS);

    // See-through switch, in a column right of the sliders.
    lv_obj_t *sw = lv_switch_create(scr);
    lv_obj_set_size(sw, 50, 26);
    lv_obj_set_pos(sw, W - m - swColW / 2 - 25, y - 4);
    if (START_SEE_THROUGH) lv_obj_add_state(sw, LV_STATE_CHECKED);
    balls_set_see_through(START_SEE_THROUGH);
    lv_obj_add_event_cb(sw, see_through_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *swLabel = make_label(scr, "See-through");
    lv_obj_align_to(swLabel, sw, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    y += 40;

    g_speedLabel = make_label(scr, "");
    speed_text(g_speedLabel, START_SPEED);
    lv_obj_set_pos(g_speedLabel, m, y);
    make_slider(scr, sliderX, y + 1, sliderW, 1, 100, START_SPEED, speed_cb);
    balls_set_speed(START_SPEED);
    y += 40;

    // Bottom left: how this demo gets its frame rate.
    lv_obj_t *note = lv_label_create(scr);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, W / 2 + 40);
    lv_obj_set_style_text_color(note, lv_color_hex(0xA0A0B0), 0);
    lv_label_set_text_fmt(note,
        "This frame rate comes from bypassing LVGL for the ball area: the balls are "
        "drawn straight into the display's scan-out buffers. Only this strip is LVGL. "
        "(Arduino on core %d.)", xPortGetCoreID());
    lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, m, -8);

    // Bottom right: frame rate and CPU.
    g_perf = make_label(scr, "");
    lv_obj_set_style_text_align(g_perf, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(g_perf, LV_ALIGN_BOTTOM_RIGHT, -m, -8);
    lv_timer_create(perf_update, 1000, NULL);
}

void setup() {
    Serial0.begin(115200);

    // The LVGL strip is filled by the CPU, from the row cache's run-length
    // lists where it can, not by memory-to-memory DMA: the DMA copies
    // competed with the panel's own DMA and with the raster for internal RAM.
    display.setBounceFillMode(ESP32S3_Touch_LCD_7_7B::BOUNCE_FILL_SAME_CORE);
    lv_setup.begin(0, RASTER_PCLK_HZ);

    // The real frame period, after the S3's clock divider has rounded the
    // pixel clock: wait for a few frames.
    uint32_t f0 = display.frameCount();
    while (display.frameCount() - f0 < 3) delay(5);
    uint32_t periodUs = display.framePeriodUs();
    Serial0.printf("Panel frame period %u us (%.2f Hz)\n", (unsigned)periodUs, 1e6f / periodUs);
    g_hz10 = periodUs ? (10000000 + periodUs / 2) / periodUs : 0;

    g_ballRows = display.height() - STRIP_ROWS;
    balls_init(display.width(), g_ballRows, periodUs);
    balls_start_helper(LV_SETUP_DISPLAY_CORE ^ 1);   // half of each chunk on the other core
    if (!display.setRasterRows(0, g_ballRows, balls_raster))
        Serial0.println("setRasterRows refused (half resolution, RGB332 or 180-degree driver flip?)");

    build_strip();
}

// Serial 'b': step through ball counts, opaque then see-through, two seconds
// each, and print the longest chunk and the late chunks for each.  (The
// sliders and the switch keep their own settings; touch one to restore it.)
static void bench() {
    static const int counts[] = { 20, 50, 100, 150, 200 };
    for (int st = 0; st < 2; st++) {
        balls_set_see_through(st);
        for (int c : counts) {
            balls_set_count(c);
            delay(300);
            g_rasterMaxFnUs = 0;
            g_rasterMaxUs = 0;
            extern volatile uint32_t g_physWaitUs;
            g_physWaitUs = 0;
            uint32_t cyc0 = g_rasterCycles, n0 = g_rasterChunks;
            uint32_t late0 = g_rasterLate;
            uint32_t t0 = millis();
            while (millis() - t0 < 2000) { lv_timer_handler(); delay(5); }
            Serial0.printf("bench %-11s %3d balls: chunk drawn in %u us max, done %u us after its interrupt, late %u\n",
                           st ? "see-through" : "opaque", c, (unsigned)g_rasterMaxFnUs,
                           (unsigned)g_rasterMaxUs, (unsigned)(g_rasterLate - late0));
            uint32_t n = g_rasterChunks - n0;
            Serial0.printf("   average %u us; first chunk waited for physics %u us at most\n",
                           (unsigned)(n ? (g_rasterCycles - cyc0) / n / 240 : 0), (unsigned)g_physWaitUs);
        }
    }
    balls_set_see_through(START_SEE_THROUGH);
    balls_set_count(START_BALLS);
}

void loop() {
    if (Serial0.available() && Serial0.read() == 'b') bench();
    lv_timer_handler();
    delay(5);
}
