/*
 * chipguy_ESP32S3_Touch_LCD_7_7B_rotate - 90/270-degree LVGL rendering for the
 * ESP32-S3-Touch-LCD-7B, done at the LVGL->framebuffer level (the driver itself
 * keeps reading PSRAM forward at 0 degrees; see the README).
 *
 * LVGL runs in PARTIAL render mode with rotation set, so it hands us partial
 * areas + pixels in LOGICAL (portrait) coordinates; LvglRotatedRenderer rotates
 * each into one of the driver's two PHYSICAL (landscape) framebuffers and keeps
 * the two buffers consistent with a deferred dirty-rectangle scheme:
 *
 *   - flushRegion(): rotate-blit the area into the buffer we're building
 *     (FB[draw]); record it in `thisFrame`; and SUBTRACT it from `pending`
 *     (regions FB[draw] still lacks vs the scanned buffer) -- so pixels about to
 *     be overwritten next frame are never copied (e.g. while scrolling).
 *   - frameDone() (on LVGL's last flush of the frame): copy whatever remains in
 *     `pending` from the scanned buffer into FB[draw], swap buffers at vsync,
 *     then carry `thisFrame` forward as the new `pending`.
 *
 * The same class also serves rotation 0 (identity) and 180 (mirror, each row
 * written forward into PSRAM from the SRAM band read backward), for the
 * PARTIAL-render path that keeps
 * every PSRAM write in our hands: with pace = true, each 8-row piece written to
 * a framebuffer first calls display.paceWrites(), so a GDMA bounce fill is never
 * starved by LVGL.  Each 8-row piece is also written back from the cache right
 * after it is written (display.writeBackRange), so the GDMA fill sees it and the
 * flip needs no whole-cache write-back -- nor the stall of the other core that
 * a whole-cache write-back requires.
 *
 * Everything favors forward, long, sequential PSRAM access: the rotate-blit
 * writes each physical row as one contiguous run (reading the SRAM partial
 * buffer strided), and the reconciliation copy is row-major full-width memcpys.
 *
 * Copyright (c) 2025 chipguyhere
 * MIT License
 */

#pragma once

#include <string.h>
#include <lvgl.h>
#include "chipguy_ESP32S3_Touch_LCD_7_7B_display.h"
#include "chipguy_ESP32S3_Touch_LCD_7_7B_dirtyrects.h"

// Renderer timing (microseconds, cumulative): blit = flushRegion copies (incl.
// pacing), reconcile = frameDone's pending copies, flipWait = waiting for the
// panel to take the new frame.
inline volatile uint32_t g_rendBlitUs = 0, g_rendReconcileUs = 0, g_rendFlipWaitUs = 0;

class LvglRotatedRenderer {
public:
    // rotation: 0, 90, 180 or 270 (index aliases 1/2/3 accepted), all done here
    // with the display begun at 0.  The second framebuffer must be allocated.
    //
    // With a third framebuffer allocated (display.getFramebuffer3()), frames
    // are triple-buffered: frameDone() queues the finished frame for the next
    // flip and returns, and LVGL renders the next frame into the third buffer
    // meanwhile.  It blocks only if the frame queued before is still waiting,
    // i.e. when rendering outpaces the panel.
    LvglRotatedRenderer(ESP32S3_Touch_LCD_7_7B& d, int rotation, bool pace = false)
        : display(d), _rotation(cglcd7_7b_normalize_rotation((uint16_t)rotation)),
          _pace(pace), _newest(1), _drawIndex(2) {
        _nbuf = d.getFramebuffer3() ? 3 : 2;
        // When the draw buffer's missing list runs out of slots, flush the
        // smallest pending copy immediately (newest frame -> buffer being
        // built) rather than coalescing -- so subtracted holes are never lost.
        // The other buffers' lists are add-only, so their bbox-coalesce
        // fallback is harmless; they get the sink when they become the draw
        // buffer.
        _missing[_drawIndex - 1].setOverflowSink(&LvglRotatedRenderer::pendingSink, this);
        // Every framebuffer write below is written back from the cache right
        // away, so the flip needn't write back (and stall) the whole cache.
        d.setRangeWriteBack(true);
    }

    // Rotate one LVGL partial area (logical/portrait) into FB[draw], record it,
    // and subtract it from the pending carry-over list.
    void flushRegion(const lv_area_t* area, uint8_t* px_map) {
        uint32_t tStart = micros();
        const uint16_t* src = (const uint16_t*)px_map;
        const int lx1 = area->x1, ly1 = area->y1, lx2 = area->x2, ly2 = area->y2;
        const int areaW = lx2 - lx1 + 1;
        // Framebuffer size: the panel's, or half of it (setHalfResolution).
        const int Wl = display.fbHeight(); // logical width  (600 or 480; rotated)
        const int Hl = display.fbWidth();  // logical height (1024 or 800; rotated)
        const int Wp = display.fbWidth();  // physical stride (1024 or 800)
        uint16_t* fbDraw = fbPtr(_drawIndex);

        CGRect phys;
        if (_rotation == 180) {
            // Mirror: logical (lx,ly) -> physical (Wp-1-lx, H-1-ly).  Each logical
            // row becomes one physical row, written forward over px while the
            // SRAM band is read backward.
            const int H = display.fbHeight();
            const int pxStart = Wp - 1 - lx2;
            for (int y0 = ly1; y0 <= ly2; y0 += kPieceRows) {
                int y1 = y0 + kPieceRows - 1; if (y1 > ly2) y1 = ly2;
                if (_pace) display.paceWrites();
                for (int ly = y0; ly <= y1; ly++) {
                    uint16_t* d = fbDraw + (size_t)(H - 1 - ly) * Wp + pxStart;
                    const uint16_t* s = src + (size_t)(ly - ly1) * areaW + (areaW - 1);
                    for (int k = 0; k < areaW; k++) *d++ = *s--;
                }
                _wbRows(fbDraw, H - 1 - y1, H - 1 - y0, pxStart, pxStart + areaW - 1);
            }
            phys = { (int16_t)pxStart, (int16_t)(H - 1 - ly2),
                     (int16_t)(Wp - 1 - lx1), (int16_t)(H - 1 - ly1) };
        } else if (_rotation == 0) {
            // Identity: one contiguous run per row, written in paced pieces.
            const size_t bytes = (size_t)areaW * sizeof(uint16_t);
            for (int y0 = ly1; y0 <= ly2; y0 += kPieceRows) {
                int y1 = y0 + kPieceRows - 1; if (y1 > ly2) y1 = ly2;
                if (_pace) display.paceWrites();
                for (int y = y0; y <= y1; y++)
                    memcpy(fbDraw + (size_t)y * Wp + lx1, src + (size_t)(y - ly1) * areaW, bytes);
                _wbRows(fbDraw, y0, y1, lx1, lx2);
            }
            phys = { (int16_t)lx1, (int16_t)ly1, (int16_t)lx2, (int16_t)ly2 };
        } else if (_rotation == 90) {
            // physical (px,py) = (ly, Wl-1-lx).  Each logical column -> one
            // physical row, written forward over px (=ly); src read strided.
            for (int lx = lx1; lx <= lx2; lx++) {
                int py = Wl - 1 - lx;
                uint16_t* d = fbDraw + (size_t)py * Wp + ly1;     // px starts at ly1
                const uint16_t* s = src + (lx - lx1);             // ly==ly1 row
                for (int ly = ly1; ly <= ly2; ly++) {
                    *d++ = *s;
                    s += areaW;
                }
                // Rows py = Wl-1-lx, written for lx ascending: write back every
                // kPieceRows rows, and at the end.
                if ((lx - lx1) % kPieceRows == kPieceRows - 1 || lx == lx2) {
                    int lxStart = lx - (lx - lx1) % kPieceRows;
                    _wbRows(fbDraw, Wl - 1 - lx, Wl - 1 - lxStart, ly1, ly2);
                }
            }
            phys = { (int16_t)ly1, (int16_t)(Wl - 1 - lx2),
                     (int16_t)ly2, (int16_t)(Wl - 1 - lx1) };
        } else {  // 270
            // physical (px,py) = (Hl-1-ly, lx).  Written forward over px, so ly
            // descends (from ly2) and src is read strided backward.
            const int pxStart = Hl - 1 - ly2;
            const int pxEnd   = Hl - 1 - ly1;
            for (int lx = lx1; lx <= lx2; lx++) {
                int py = lx;
                uint16_t* d = fbDraw + (size_t)py * Wp + pxStart;
                const uint16_t* s = src + (size_t)(ly2 - ly1) * areaW + (lx - lx1); // ly==ly2 row
                for (int px = pxStart; px <= pxEnd; px++) {
                    *d++ = *s;
                    s -= areaW;
                }
                // Rows py = lx, ascending.
                if ((lx - lx1) % kPieceRows == kPieceRows - 1 || lx == lx2) {
                    int lxStart = lx - (lx - lx1) % kPieceRows;
                    _wbRows(fbDraw, lxStart, lx, pxStart, pxEnd);
                }
            }
            phys = { (int16_t)(Hl - 1 - ly2), (int16_t)lx1,
                     (int16_t)(Hl - 1 - ly1), (int16_t)lx2 };
        }

        g_rendBlitUs += micros() - tStart;
        _thisFrame.add(phys);     // FB[draw] now has this region (freshly drawn)
        display.markRowsChanged(phys.y1, phys.y2, _drawIndex);   // row cache: they change when it's shown
        _missing[_drawIndex - 1].subtract(phys);  // ...so don't copy the old content there
    }

    // Called on LVGL's last flush of the frame: bring FB[draw] fully up to date,
    // queue it for the next flip, and pick the next draw buffer.
    void frameDone() {
        DirtyRectList& miss = _missing[_drawIndex - 1];
        // Bring FB[draw] up to date with the newest complete frame, wherever
        // this frame didn't draw (whatever the overflow sink didn't already
        // copy eagerly during the frame).
        uint32_t t0 = micros();
        for (int i = 0; i < miss.count(); i++) _copyRect(miss[i]);
        miss.clear();
        uint32_t t1 = micros();
        g_rendReconcileUs += t1 - t0;

        int next;
        if (_nbuf == 3) {
            // FIFO, one frame deep: a frame queued earlier must be shown first,
            // or it would be skipped and the row cache's marks with it.
            while (display.pendingFramebuffer() != display.activeFramebuffer()) vTaskDelay(1);
            int shown = display.activeFramebuffer();
            display.setActiveFramebuffer(_drawIndex, /*wait*/false);
            next = 6 - shown - _drawIndex;   // the buffer neither shown nor queued
        } else {
            display.setActiveFramebuffer(_drawIndex, /*wait*/true);   // tear-free swap
            next = 3 - _drawIndex;
        }
        g_rendFlipWaitUs += micros() - t1;

        // Every other buffer now lacks what this frame drew.
        for (int b = 1; b <= _nbuf; b++) {
            if (b == _drawIndex) continue;
            for (int i = 0; i < _thisFrame.count(); i++) _missing[b - 1].add(_thisFrame[i]);
        }
        _thisFrame.clear();
        _missing[_drawIndex - 1].setOverflowSink(nullptr, nullptr);
        _newest = _drawIndex;
        _drawIndex = next;
        _missing[_drawIndex - 1].setOverflowSink(&LvglRotatedRenderer::pendingSink, this);
    }

    // Diagnostic: times the pending list was coalesced to a bbox.  With the
    // overflow sink installed this must stay 0 (coalescing a hole-carrying list
    // would lose holes); it is the canary for the old trail bug.
    uint32_t overflowCount() const { return _missing[_drawIndex - 1].overflowCount; }

    // Diagnostic: times the list ran out of slots and eager-flushed the smallest
    // pending copy instead of deferring it.  Nonzero just means heavy churn; it
    // is correct (the work is done now rather than at frameDone), not an error.
    uint32_t evictionCount() const { return _missing[_drawIndex - 1].evictionCount; }

    // Diagnostics (bring-up): which framebuffer the panel scans vs we build.
    int debugActiveIndex() const { return display.activeFramebuffer(); }
    int debugDrawIndex() const { return _drawIndex; }

private:
    uint16_t* fbPtr(int idx) { return display.framebufferByIndex(idx); }

    // Copy one physical rectangle from the newest complete frame into the
    // buffer being built (not yet displayed) as forward, full-width row-major
    // runs.  Used both by frameDone()'s deferred reconcile and by the overflow
    // sink's eager flush -- both copy newest -> draw, which is always safe.
    void _copyRect(const CGRect& r) {
        uint16_t* srcFb = fbPtr(_newest);
        uint16_t* dstFb = fbPtr(_drawIndex);
        const int Wp = display.fbWidth();
        const size_t bytes = (size_t)(r.x2 - r.x1 + 1) * sizeof(uint16_t);
        for (int y0 = r.y1; y0 <= r.y2; y0 += kPieceRows) {
            int y1 = y0 + kPieceRows - 1; if (y1 > r.y2) y1 = r.y2;
            if (_pace) display.paceWrites();
            for (int y = y0; y <= y1; y++)
                memcpy(dstFb + (size_t)y * Wp + r.x1,
                       srcFb + (size_t)y * Wp + r.x1, bytes);
            _wbRows(dstFb, y0, y1, r.x1, r.x2);
        }
    }

    // Write back rows yA..yB, columns x1..x2, of a framebuffer from the cache:
    // one call for the contiguous span from the first pixel to the last.  The
    // untouched bytes between rows aren't dirty; the cache just steps past them.
    void _wbRows(uint16_t* fb, int yA, int yB, int x1, int x2) {
        const int Wp = display.fbWidth();
        uint16_t* a = fb + (size_t)yA * Wp + x1;
        uint16_t* b = fb + (size_t)yB * Wp + x2 + 1;
        display.writeBackRange(a, (size_t)(b - a) * sizeof(uint16_t));
    }

    // Overflow sink trampoline: the pending list calls this with the smallest
    // rectangle when it needs a free slot; we execute that copy immediately so
    // the obligation is met and the rect can be dropped without losing a hole.
    static void pendingSink(void* ctx, const CGRect& r) {
        ((LvglRotatedRenderer*)ctx)->_copyRect(r);
    }

    static const int kPieceRows = 8;   // rows per paced PSRAM write piece

    ESP32S3_Touch_LCD_7_7B& display;
    int _rotation;          // 0, 90, 180 or 270
    bool _pace;             // gate framebuffer writes on the bounce fill's slack
    int _nbuf;              // framebuffers in rotation (2 or 3)
    int _newest;            // framebuffer holding the newest complete frame
    int _drawIndex;         // framebuffer LVGL is building this frame
    DirtyRectList _missing[3]; // per framebuffer: regions it lacks vs FB[newest]
    DirtyRectList _thisFrame;// regions drawn this frame
};
