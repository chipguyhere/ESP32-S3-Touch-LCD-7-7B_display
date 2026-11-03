/*
 * lv_setup_ESP32S3_Touch_LCD_7_7B.hpp - Connects LVGL to the Waveshare
 * ESP32-S3-Touch-LCD-7 (800x480) and -7B (1024x600): RGB panel + GT911
 * capacitive touch.  The board is detected at begin().
 *
 * This is the *actual* LVGL-to-driver glue for this board.  It ships inside the
 * library (src/) so every example can share one copy.  Each example's sketch
 * folder contains a small lv_setup.hpp that simply #includes this file; that
 * thin shim is the file you edit if you want to take this code over and
 * customize it (see the comments in any example's lv_setup.hpp).
 *
 * Include this file (or, normally, the example's lv_setup.hpp) after lvgl.h.
 *
 * Rotations 0 and 180 use LVGL DIRECT render straight into the panel's two RGB
 * framebuffers (the driver reads PSRAM forward).  Rotations 90 and 270 use
 * LVGL PARTIAL render in portrait, then rotate each region into the landscape
 * framebuffers, reconciling the two buffers with a deferred dirty-rectangle
 * scheme (see chipguy_ESP32S3_Touch_LCD_7_7B_rotate.hpp and *_dirtyrects.h).
 * Touch comes from the GT911 controller over the board's shared I2C bus.
 */

#pragma once

#include <ESP32S3_Touch_LCD_7_7B_Board.h>            // shared I2C bus + expander
#include <chipguy_ESP32S3_Touch_LCD_7_7B_display.h>
#include <chipguy_ESP32S3_Touch_LCD_7_7B_touch.h>
#include <chipguy_ESP32S3_Touch_LCD_7_7B_rotate.hpp> // 90/270 rotate + reconcile
#include <Wire.h>
#include <lvgl.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// ---- LVGL's heap in PSRAM (lv_conf.h: LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM) ----
// LVGL's objects, styles and strings go to PSRAM first, falling back to
// internal RAM only if PSRAM is full, so the UI's size doesn't eat into the
// internal RAM that Wi-Fi, TLS and the rest of the app need.  The price is
// that LVGL walks its object tree in PSRAM, competing with the display for
// the bus.  LVGL's draw buffers (the transient layers for opacity and
// transforms) and glyph bitmaps are the exception: lv_setup.begin() points
// them at internal RAM first, because every blend reads them.  Decoded and
// app-created images stay in PSRAM.  This file is included by exactly one
// file in the sketch, so these definitions appear once.
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
#include <draw/lv_draw_buf_private.h>   // lv_draw_buf_handlers_t's callbacks
static size_t s_lvInUse = 0, s_lvPeak = 0;   // bytes LVGL holds (heap block sizes)
static inline void _lvTrack(void *p, int sign) {
    if (!p) return;
    size_t n = heap_caps_get_allocated_size(p);
    if (sign > 0) { s_lvInUse += n; if (s_lvInUse > s_lvPeak) s_lvPeak = s_lvInUse; }
    else s_lvInUse -= n;
}
#define LV_SETUP_PSRAM_FIRST   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
#define LV_SETUP_INTERNAL_FIRST MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
extern "C" {
void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { LV_UNUSED(mem); LV_UNUSED(bytes); return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }
void *lv_malloc_core(size_t size) {
    void *p = heap_caps_malloc_prefer(size, 2, LV_SETUP_PSRAM_FIRST);
    _lvTrack(p, 1);
    return p;
}
void *lv_realloc_core(void *p, size_t new_size) {
    _lvTrack(p, -1);
    void *q = heap_caps_realloc_prefer(p, new_size, 2, LV_SETUP_PSRAM_FIRST);
    _lvTrack(q ? q : p, 1);   // on failure p is still held
    return q;
}
void lv_free_core(void *p) {
    _lvTrack(p, -1);
    heap_caps_free(p);
}
void lv_mem_monitor_core(lv_mem_monitor_t *mon) {
    lv_memzero(mon, sizeof(*mon));
    mon->total_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    mon->free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    mon->free_biggest_size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    mon->max_used = s_lvPeak;
    mon->used_pct = mon->total_size ? (uint8_t)(100 - mon->free_size * 100 / mon->total_size) : 0;
}
lv_result_t lv_mem_test_core(void) {
    return heap_caps_check_integrity_all(true) ? LV_RESULT_OK : LV_RESULT_INVALID;
}
}   // extern "C"
// Bytes LVGL itself holds right now, and its peak (the monitor above reports
// the whole PSRAM heap, which the framebuffers share).
static inline size_t lv_setup_lvgl_bytes() { return s_lvInUse; }
static inline size_t lv_setup_lvgl_peak_bytes() { return s_lvPeak; }

static void *_lvDrawBufMalloc(size_t size, lv_color_format_t cf) {
    LV_UNUSED(cf);
    void *p = heap_caps_malloc_prefer(size + LV_DRAW_BUF_ALIGN - 1, 2, LV_SETUP_INTERNAL_FIRST);
    _lvTrack(p, 1);
    return p;
}
static void _lvDrawBufFree(void *p) {
    _lvTrack(p, -1);
    heap_caps_free(p);
}
#endif

// The board owns the shared I2C bus and the IO-extension expander (display
// reset/supply/backlight and touch reset).  The display and touch drivers reach their
// hardware through it.
static ESP32S3_Touch_LCD_7_7B_Board board;
static ESP32S3_Touch_LCD_7_7B display(board, 0, 0);

// Size of the LVGL PARTIAL draw band (internal SRAM), for the renderer path
// and RGB332.  With asyncCopy it is split into two halves, one rendered while
// the other is copied.  LVGL derives an area's height from this and its
// width, and redraws every object an area touches, so wide redraws (a
// scrolling list) cost much more with a small band: a list row cut across
// three areas is laid out and drawn three times.  Measured, scrolling a
// half-width list with Arduino on core 0: 8 fps at 40 KB, 11 at 80 KB, 12 at
// 128 KB.  (Small areas, like BouncingBalls', don't care.)  The PSRAM LVGL
// heap frees more internal RAM than this takes.  Reduce if you run out of
// internal RAM at boot.
#ifndef LV_SETUP_ROT_BAND_BYTES
#define LV_SETUP_ROT_BAND_BYTES (80 * 1024)
#endif
// The copy task (asyncCopy): priority above loop() (1) and LVGL's draw thread
// (3), below the bounce fill; and its core, -1 = the core lv_setup.begin() runs
// on (LVGL's).
#ifndef LV_SETUP_COPY_TASK_PRIO
#define LV_SETUP_COPY_TASK_PRIO 5
#endif
#ifndef LV_SETUP_COPY_TASK_CORE
#define LV_SETUP_COPY_TASK_CORE -1
#endif
// The core that runs the display's interrupt, bounce fill and (asyncCopy) copy
// task -- everything that touches PSRAM on the display's behalf.
#ifndef LV_SETUP_DISPLAY_CORE
#define LV_SETUP_DISPLAY_CORE 1
#endif

class lv_setup_class {
public:
    // Set before begin() to change; the defaults are what was measured best.
    //
    // partialRender: have LVGL render PARTIAL into an SRAM band and copy into
    //   the framebuffers ourselves -- the path 90/270 already use -- instead of
    //   rendering DIRECT into PSRAM, so every PSRAM write goes through code we
    //   control.  Applies with the GDMA fill (the default) and the same-core
    //   CPU fill.
    // paceWrites: gate those copies on the GDMA fill's slack (see
    //   display.paceWrites()), so a busy UI slows itself down rather than
    //   glitching the panel.
    // rowCache: with that PARTIAL path, keep run-length-encoded copies of
    //   unchanged scanlines so the bounce fill reads far less from PSRAM (see
    //   display.setRowCache()).
    // tripleBuffer: with that PARTIAL path, allocate a third framebuffer
    //   (1.2 MB of PSRAM on the -7B, 750 KB on the -7) so LVGL renders the next frame while a finished one
    //   waits for its flip, instead of blocking up to a whole refresh.
    // frameTick: LVGL's clock is the panel's frame clock (display.frameTickMs())
    //   instead of millis(): it advances once per refresh, by the frame period,
    //   so time-based animations (lv_anim: scroll snap-back, transitions) move
    //   by whole frames and land evenly on screen.
    bool partialRender = true;
    bool paceWrites = true;
    bool rowCache = true;
    // asyncCopy: with that PARTIAL path, copy each rendered area into the
    //   framebuffers (and reconcile, and queue the flip) in a task of its own,
    //   while LVGL renders the next area into a second band.  The band memory
    //   is split in two, so this costs no extra SRAM.  The task runs on the
    //   display core (LV_SETUP_DISPLAY_CORE), next to the bounce fill: the fill
    //   preempts it, so the two PSRAM users never compete across cores.  It
    //   pays off when LVGL runs on the other core (Tools > Arduino Runs On:
    //   Core 0), where LVGL's work -- SRAM only -- doesn't contend for PSRAM.
    // halfResolution: LVGL works at half the panel each way (512x300 on the -7B, 400x240 on
    //   the -7; swapped in portrait) and the
    //   display scales that up 2x, interpolating (see
    //   display.setHalfResolution()).  A quarter of the pixels to draw, copy
    //   and send from PSRAM: much smoother motion, softer small text.  For
    //   walk-up UIs with big buttons.  Off by default.
    bool tripleBuffer = true;
    bool frameTick = true;
    bool halfResolution = false;
    bool asyncCopy = true;

    // rotation: 0 / 180 are handled in the driver (DIRECT render, forward PSRAM
    //           read -- see display.begin()).  90 / 270 are handled here: LVGL
    //           renders PARTIAL in portrait and our flush rotates each region
    //           into the landscape framebuffers, reconciling the two buffers
    //           with a deferred dirty-rectangle scheme (see the rotate header).
    //           The index aliases 1/2/3 are accepted for 90/180/270.
    //
    // pclk_hz: the panel's pixel clock, which sets the refresh rate and so the
    //          share of the app core the per-frame copy takes.  0 = the
    //          board's default (14 MHz on the -7, 30 MHz on the -7B).  See
    //          display.begin()'s header comment.
    //
    // color:   ESP32S3_Touch_LCD_7_7B::COLOR_RGB565 (default) or COLOR_RGB332, the
    //          opt-in 8-bit framebuffer that halves the per-frame PSRAM read
    //          (see setColorMode() in the display header).  In RGB332 mode LVGL
    //          still renders RGB565, PARTIAL, into an SRAM band; the flush
    //          quantizes just the changed areas into the one 8-bit framebuffer.
    //          Rotations 0 and 180 only.
    void begin(uint16_t rotation = 0, uint32_t pclk_hz = LCD7_7B_DEFAULT_PCLK_HZ,
               ESP32S3_Touch_LCD_7_7B::ColorMode color = ESP32S3_Touch_LCD_7_7B::COLOR_RGB565) {
        rotation = cglcd7_7b_normalize_rotation(rotation);  // 1/2/3 -> 90/180/270
        bool rgb332 = (color == ESP32S3_Touch_LCD_7_7B::COLOR_RGB332);
        if (rgb332 && (rotation == 90 || rotation == 270)) {
            Serial0.println("RGB332 mode supports rotation 0 and 180 only; using 0");
            rotation = 0;
        }
        _rotation = rotation;
        bool quarter = (rotation == 90 || rotation == 270);
        if (halfResolution) {
            if (rgb332) {
                Serial0.println("halfResolution is RGB565 only; ignoring COLOR_RGB332");
                rgb332 = false;
                color = ESP32S3_Touch_LCD_7_7B::COLOR_RGB565;
            }
            display.setHalfResolution(true);
        }
        display.setColorMode(color);

        // Through the renderer (the default), every rotation is done above the
        // driver -- 90/270 rotate, 180 mirrors -- so the framebuffer is always
        // in scan-out order and the driver runs at 0, where GDMA, pacing and the
        // row cache all work.  Otherwise (RGB332, an explicit CPU fill mode, or
        // partialRender off) the driver does the 180 flip in its fill.
        ESP32S3_Touch_LCD_7_7B::BounceFillMode req = display.getBounceFillMode();
        bool viaRenderer = quarter || halfResolution ||
            (partialRender && !rgb332 &&
             (req == ESP32S3_Touch_LCD_7_7B::BOUNCE_FILL_DEFAULT ||
              req == ESP32S3_Touch_LCD_7_7B::BOUNCE_FILL_GDMA ||
              req == ESP32S3_Touch_LCD_7_7B::BOUNCE_FILL_SAME_CORE));
        uint16_t driverRotation = viaRenderer ? 0 : rotation;
        // The pixel clock (and so the refresh rate, and so the CPU cost of the
        // per-frame copy) is the driver's default; see display.begin()'s
        // header comment.
        //
        // The display's interrupt and bounce-fill task live on the core that
        // calls display.begin().  Keep them on LV_SETUP_DISPLAY_CORE: if
        // setup() runs elsewhere (Tools > Arduino Runs On: Core 0), start the
        // display from a short-lived task there, so LVGL and loop() get the
        // other core to themselves.
        // The board -- the shared I2C bus and its interrupt -- starts here, on the
        // app's core, where touch and the sketch's own I2C traffic run.  (Left to
        // display.begin() on the display core, the bus interrupt sat on the other
        // core from its callers, and the I2C driver's state got corrupted under
        // load.)  display.begin()'s own board.begin() is then a no-op.
        board.begin();
        // With LVGL on the other core, a synchronous flush would write PSRAM
        // from that core while the bounce fill's DMA runs on this one: fills
        // miss their deadline and the screen shows displaced bands.  The copy
        // must stay on the display core, so asyncCopy is required there.
        if (!asyncCopy && LV_SETUP_DISPLAY_CORE >= 0 && xPortGetCoreID() != LV_SETUP_DISPLAY_CORE) {
            Serial0.println("lv_setup: asyncCopy is required when LVGL runs off the display core; enabling it");
            asyncCopy = true;
        }
        if (!_beginDisplayOn(LV_SETUP_DISPLAY_CORE, pclk_hz, driverRotation)) {
            Serial0.println("Display init failed!");
            while (1) delay(100);
        }
        if (!rgb332) display.allocateSecondFramebuffer();
        if (viaRenderer && tripleBuffer) display.allocateThirdFramebuffer();
        if (viaRenderer && rowCache && !halfResolution)
            display.setRowCache(true);   // GDMA or the CPU fill on the display core
        Serial0.println("Display initialized");
        display.setBacklight(true);

        // Touch is left in pass-through (raw physical/landscape coordinates);
        // _touchpad_read maps them to the active rotation below.
        _touch.begin();

        lv_init();
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
        // Draw layers and glyphs: internal RAM first (see the allocator above).
        lv_draw_buf_get_handlers()->buf_malloc_cb = _lvDrawBufMalloc;
        lv_draw_buf_get_handlers()->buf_free_cb = _lvDrawBufFree;
        lv_draw_buf_get_font_handlers()->buf_malloc_cb = _lvDrawBufMalloc;
        lv_draw_buf_get_font_handlers()->buf_free_cb = _lvDrawBufFree;
#endif

        if (rgb332) {
            // ---- RGB332: PARTIAL render into an SRAM band, quantized on flush ----
            lv_display_t *disp = lv_display_create(display.width(), display.height());
            void *band = heap_caps_malloc(LV_SETUP_ROT_BAND_BYTES,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!band) {
                Serial0.println("Failed to allocate RGB332 draw band (internal SRAM)");
                while (1) delay(100);
            }
            lv_display_set_buffers(disp, band, nullptr, LV_SETUP_ROT_BAND_BYTES,
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
            lv_display_set_flush_cb(disp, _disp_flush_rgb332);
            lv_display_set_user_data(disp, this);
        } else if (!viaRenderer) {
            // ---- 0 / 180: DIRECT render straight into the two framebuffers ----
            lv_display_t *disp = lv_display_create(display.width(), display.height());
            lv_display_set_buffers(disp, display.getFramebuffer(), display.getFramebuffer2(),
                display.height() * display.width() * sizeof(lv_color_t),
                LV_DISPLAY_RENDER_MODE_DIRECT);
            lv_display_set_flush_cb(disp, _disp_flush_direct);
            lv_display_set_user_data(disp, this);
        } else {
            // ---- PARTIAL render + our own copy/reconcile into the framebuffers:
            //      90 / 270 rotate; 0 / 180 (partialRender) copy straight ----
            _renderer = new LvglRotatedRenderer(display, rotation, paceWrites);

            // Create the display at the LOGICAL size (portrait for 90/270); our
            // flush maps each partial area into the physical framebuffers.
            lv_display_t *disp = quarter
                ? lv_display_create(display.fbHeight(), display.fbWidth())
                : lv_display_create(display.fbWidth(), display.fbHeight());

            // One PARTIAL draw band in internal SRAM (fast strided reads during
            // the transpose).  Single buffer is fine: the flush is synchronous
            // CPU work (no DMA to overlap).
            void *band = heap_caps_malloc(LV_SETUP_ROT_BAND_BYTES,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!band) {
                Serial0.println("Failed to allocate rotation draw band (internal SRAM)");
                while (1) delay(100);
            }
            if (asyncCopy && _startCopyTask()) {
                // Two half bands: LVGL renders into one while the copy task
                // empties the other.
                const size_t half = (LV_SETUP_ROT_BAND_BYTES / 2) & ~(size_t)63;
                lv_display_set_buffers(disp, band, (uint8_t *)band + half, half,
                                       LV_DISPLAY_RENDER_MODE_PARTIAL);
                lv_display_set_flush_cb(disp, _disp_flush_queued);
                lv_display_set_flush_wait_cb(disp, _disp_flush_wait);
            } else {
                lv_display_set_buffers(disp, band, nullptr, LV_SETUP_ROT_BAND_BYTES,
                                       LV_DISPLAY_RENDER_MODE_PARTIAL);
                lv_display_set_flush_cb(disp, _disp_flush_rotated);
            }
            lv_display_set_user_data(disp, this);
        }

        static lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, _touchpad_read);
        lv_indev_set_user_data(indev, this);

        lv_tick_set_cb(frameTick ? _frame_tick_get : _tick_get);
    }

private:
    ESP32S3_Touch_LCD_7_7B_Touch _touch{board};
    int _rotation = 0;
    LvglRotatedRenderer* _renderer = nullptr;

    static uint32_t _tick_get(void) { return millis(); }
    static uint32_t _frame_tick_get(void) { return display.frameTickMs(); }

    // RGB332 PARTIAL flush: pushImage() quantizes RGB565 -> 332 as it writes
    // the area into the (single, live) 8-bit framebuffer.
    static void _disp_flush_rgb332(lv_display_t *disp, const lv_area_t *area, uint8_t *pixelmap) {
        display.pushImage(area->x1, area->y1, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1,
                          (const uint16_t *)pixelmap);
        lv_disp_flush_ready(disp);
    }

    // 0/180 DIRECT flush: LVGL renders into a full framebuffer; on the last area
    // of the frame, reveal that buffer at vsync.
    static void _disp_flush_direct(lv_display_t *disp, const lv_area_t *area, uint8_t *pixelmap) {
        if (lv_display_flush_is_last(disp)) {
            display.setActiveFramebuffer(
                ((void *)pixelmap == (void *)(display.getFramebuffer())) ? 1 : 2, true);
        }
        lv_disp_flush_ready(disp);
    }

    // 90/270 PARTIAL flush: rotate the area into the buffer we're building;
    // reconcile + swap on the last area of the frame.
    static void _disp_flush_rotated(lv_display_t *disp, const lv_area_t *area, uint8_t *pixelmap) {
        auto *self = (lv_setup_class *)lv_display_get_user_data(disp);
        self->_renderer->flushRegion(area, pixelmap);
        if (lv_display_flush_is_last(disp)) self->_renderer->frameDone();
        lv_disp_flush_ready(disp);
    }

    // ---- asyncCopy: the flush hands each area to the copy task.  LVGL waits
    //      (flush_wait_cb) before it flushes the next area, so at most one is
    //      outstanding, and the band it lives in isn't rendered into again
    //      until the task has released it. ----
    struct CopyJob { lv_area_t area; uint8_t *px; bool last; };
    QueueHandle_t _copyQueue = nullptr;
    SemaphoreHandle_t _copyDone = nullptr;

    // Run display.begin() on another core and wait for it.  The hand-back is
    // a task notification, not a semaphore: deleting a semaphore the moment
    // the waiter wakes frees it while the other core can still be inside
    // xSemaphoreGive() -- a use-after-free that corrupted the heap.
    struct BeginArgs { uint32_t pclk; uint16_t rot; bool ok; TaskHandle_t waiter; };
    static void _beginTask(void *arg) {
        auto *a = (BeginArgs *)arg;
        TaskHandle_t waiter = a->waiter;   // a lives on the waiter's stack: done with it after this
        a->ok = display.begin(a->pclk, a->rot);
        xTaskNotifyGive(waiter);
        vTaskDelete(nullptr);
    }
    static bool _beginDisplayOn(int core, uint32_t pclk, uint16_t rot) {
        if (core < 0 || core == xPortGetCoreID()) return display.begin(pclk, rot);
        BeginArgs a = { pclk, rot, false, xTaskGetCurrentTaskHandle() };
        if (xTaskCreatePinnedToCore(_beginTask, "lcd77b_begin", 4096, &a,   // uses ~2.4 KB
                                    uxTaskPriorityGet(nullptr), nullptr, core) != pdPASS)
            return display.begin(pclk, rot);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        return a.ok;
    }

    bool _startCopyTask() {
        _copyQueue = xQueueCreate(2, sizeof(CopyJob));
        _copyDone = xSemaphoreCreateBinary();
        if (!_copyQueue || !_copyDone) return false;
        BaseType_t core = LV_SETUP_COPY_TASK_CORE < 0 ? LV_SETUP_DISPLAY_CORE : LV_SETUP_COPY_TASK_CORE;
        return xTaskCreatePinnedToCore(_copyTask, "lv_copy", 4096, this,
                                       LV_SETUP_COPY_TASK_PRIO, nullptr, core) == pdPASS;
    }

    static void _copyTask(void *arg) {
        auto *self = (lv_setup_class *)arg;
        CopyJob job;
        for (;;) {
            if (xQueueReceive(self->_copyQueue, &job, portMAX_DELAY) != pdTRUE) continue;
            self->_renderer->flushRegion(&job.area, job.px);
            if (job.last) self->_renderer->frameDone();
            xSemaphoreGive(self->_copyDone);
        }
    }

    static void _disp_flush_queued(lv_display_t *disp, const lv_area_t *area, uint8_t *pixelmap) {
        auto *self = (lv_setup_class *)lv_display_get_user_data(disp);
        CopyJob job = { *area, pixelmap, lv_display_flush_is_last(disp) };
        xQueueSend(self->_copyQueue, &job, portMAX_DELAY);
        // No lv_display_flush_ready(): _disp_flush_wait() waits for the task.
    }

    static void _disp_flush_wait(lv_display_t *disp) {
        auto *self = (lv_setup_class *)lv_display_get_user_data(disp);
        xSemaphoreTake(self->_copyDone, portMAX_DELAY);
    }

    static void _touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
        auto *self = (lv_setup_class *)lv_indev_get_user_data(indev);
        static uint32_t lastReadTime;
        static bool lastWasTouched;
        static int32_t lastx, lasty;
        // Rate limiting to once every 20ms improves drag tracking
        if ((millis() - lastReadTime) > 20) {
            self->_touch.read();
            lastWasTouched = self->_touch.isTouched;
            if (lastWasTouched) {
                // The GT911 reports raw physical (landscape) coordinates; map them
                // to the active rotation so they line up with the LVGL UI.  If
                // touch comes out mirrored on your board, swap the 90/270 cases.
                int rx = self->_touch.points[0].x;
                int ry = self->_touch.points[0].y;
                int W = display.width();    // 1024 or 800
                int H = display.height();   // 600 or 480
                switch (self->_rotation) {
                    case 90:  lastx = (H - 1) - ry; lasty = rx;             break;
                    case 180: lastx = (W - 1) - rx; lasty = (H - 1) - ry;   break;
                    case 270: lastx = ry;           lasty = (W - 1) - rx;   break;
                    default:  lastx = rx;           lasty = ry;             break;  // 0
                }
                if (display.halfResolution()) { lastx /= 2; lasty /= 2; }
            }
            lastReadTime = millis();
        }
        if (lastWasTouched) {
            data->state = LV_INDEV_STATE_PR;
            data->point.x = lastx;
            data->point.y = lasty;
        } else {
            data->state = LV_INDEV_STATE_REL;
        }
    }
};

static lv_setup_class lv_setup;
