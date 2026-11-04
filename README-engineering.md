# Engineering notes — ESP32-S3-Touch-LCD-7 / -7B display driver

How the driver works, why it is shaped the way it is, and what each piece
measured. Other references:

- The board itself (the I/O expander, power, RS-485) is in the board library's
  [README-engineering.md](https://github.com/chipguyhere/ESP32-S3-Touch-LCD-7-7B_board/blob/main/README-engineering.md).
- The schematic is in its
  [schematic-description-7B.md](https://github.com/chipguyhere/ESP32-S3-Touch-LCD-7-7B_board/blob/main/schematic-description-7B.md).

All measurements are from the bench, 2026-10-26/27, on three -7B units,
unless they say -7. "Arduino on
core 0" means the board option *Tools ▸ Arduino Runs On: Core 0*
(`LoopCore=0`); the default is core 1.

The driver began as a port of `chipguy_ESP32-S3-Touch-LCD-7_display` (800×480).
The RGB data and sync pins are identical. It then grew the GDMA fill, the row
cache, triple buffering, the two-core split, range write-backs and the
half-resolution mode described below. On 2026-11-03 the two libraries were
merged into this one, which drives either board.

## Two panels, one driver

`begin()` brings up the board library, which identifies the board (see its
[README-engineering.md](https://github.com/chipguyhere/ESP32-S3-Touch-LCD-7-7B_board/blob/main/README-engineering.md#telling-the-two-boards-apart)),
then `_applyBoard()` sets:

| | -7 | -7B |
| --- | --- | --- |
| Panel | 800×480 ST7262 | 1024×600 |
| HSYNC pulse/back/front | 4/8/8 (htotal 820) | 162/152/48 (htotal 1386) |
| VSYNC pulse/back/front | 4/8/8 (vtotal 500) | 45/13/3 (vtotal 661) |
| Default pclk | 14 MHz, 34.1 Hz | 30 MHz, 32.7 Hz |
| Clamp | 13 MHz (`LCD7_7B_MIN_PCLK_HZ_7`) | 12 MHz (`_7B`) |
| Bounce period (20 rows) | 1171 µs | 924 µs |
| Bounce buffers | 2 × 32 KB | 2 × 40 KB |

`_width` / `_height` are members, not constants. What used to be sized from
them at compile time — the row cache's per-row state and dirty bitmaps, a
fill-colour line buffer — is sized for 1024×600 (`LCD7_7B_MAX_WIDTH/HEIGHT`),
which costs the -7 under 200 bytes of internal RAM. The row cache's raw-row
threshold is now the row width, and the pacing threshold already followed the
measured bounce period. Both panels' rows (1600 and 2048 bytes) are 64-byte
multiples and their half-resolution rows (400 and 512 px) are 16-byte
multiples, as the GDMA fill and the SIMD upscale need.

The -7's experimental cooperative fill (`BOUNCE_FILL_COOP`) was not carried
over: it was never tested on hardware, the GDMA fill superseded it, and its
core-0 PSRAM reader is the pattern rejected below.

## Raster band (`setRasterRows`)

Panel rows the fill task gets from an app callback, drawn straight into the
SRAM bounce buffer just before the scan instead of copied from PSRAM. The band
is latched at frame start and each request carries `frameCount()`, so the app
advances its state once per panel frame and never tears. `_rasterFill()` gives
the buffer a new row-cache content id, so a harvest still queued can't encode
the app's pixels, and waits (bounded) for any m2m copy still in flight.

`examples/RasterBouncingBalls` uses it with `BOUNCE_FILL_SAME_CORE`: m2m copies
competed with the panel's DMA and with the raster for internal RAM. On a -7 at
14 MHz with Arduino on core 0 (2026-11-02): 34.1 fps at 100 balls, and 0 late
chunks from 20 to 200 balls. The worst chunk, 200 see-through balls, finished
715 µs after its interrupt against a 1171 µs deadline.

The merged build, one binary on both boards (2026-11-03, Arduino on core 0,
100 balls): -7 34.1 fps, -7B 32.7 fps, each its panel's full rate, 0 late
chunks. The -7B also counts 15–16 DMA-late and 36 dropped fills during
start-up and none afterwards. The separate 7B library's build shows the same
counts, so this predates the merge; the -7 shows none. A timing test put all
of them inside `lv_setup.begin()`, before the ball band exists, so the
balls' crowded first frames aren't the cause. With the default GDMA fill the
same start-up gives 2 late and 0 dropped. The cause: in the CPU fill modes
`paceWrites()` returns at once, so the clears of framebuffers 2 and 3 (1.2 MB
each on the -7B) ran unpaced -- usually from core 0, against the fill's
cache and PSRAM reads on core 1. Fixed 2026-11-03: in the CPU modes
`_clearPaced()` writes 4 KB pieces only while the fill task is idle (queue
empty, task blocked) and skips the write-back the CPU fill doesn't need.
Start-up is now 0 late / 0 dropped on both boards with Arduino on either core
(three -7B boots), for about 100 ms more in `lv_setup.begin()` on the -7B.

---

## The shape of it

The panel is a RAM-less RGB (DPI) panel: the ESP32-S3 must stream every pixel of
every frame. The work splits like this:

| Where | What |
| --- | --- |
| PSRAM | 2–3 framebuffers, 1024×600 RGB565, 1.2 MB each (300 KB at half resolution) |
| SRAM, core 1 | two 20-row **bounce buffers** (40 KB each), refilled by the **fill task** as the panel's GDMA streams them out |
| SRAM | LVGL's draw band (80 KB, two halves) |
| core 1 | the bounce interrupt, the fill task (highest priority), the **copy task** (band → framebuffers) |
| core 0 or 1 | LVGL and `loop()`, wherever *Arduino Runs On* puts them |

- **The bounce interrupt** does only the time-critical work: it latches the
  active framebuffer and sprites at frame start, draws the flash-write overlay
  (which must run with the cache disabled), and queues the fill. 600 rows / 20 =
  30 fills per frame, about 980 a second; each must land before the panel scans
  that buffer again.
- **The fill task** refills a bounce buffer from the displayed framebuffer.
- **LVGL renders PARTIAL** into the SRAM band. The **copy task** copies each
  area into the framebuffer being built (rotating or mirroring as needed),
  reconciles the other buffers, and queues the flip.

### Why core 1 for all the PSRAM work

The fill and the copy both lean on the PSRAM bus. On one core the fill task
preempts the copy every bounce period, so they take turns. Run on two cores,
they collide, and fills miss their deadline. A late fill leaves the previous
chunk's rows in the bounce buffer: a band of the picture shows up a few rows out
of place.

That is why the display's interrupt, fill and copy are pinned to core 1
(`LV_SETUP_DISPLAY_CORE`), even when `setup()` runs on core 0. `lv_setup` then
starts the display from a short-lived task on core 1. The I²C bus is started
first, on the app's core, where touch and the sketch's own I²C traffic run.

---

## The fill

### GDMA, without esp_async_memcpy

`BOUNCE_FILL_GDMA` (the default) copies framebuffer rows to the bounce buffer
with a memory-to-memory GDMA channel pair driven directly (`m2mInit`,
`m2mSubmit`, `m2mEof`):

- each chunk is one descriptor chain (4032-byte pieces, 64-byte bursts);
- chains are queued in a 4-slot ring;
- each chain's completion interrupt starts the next.

ESP-IDF's `esp_async_memcpy` writes back the cache over its whole PSRAM source on
every call. That measured 125 µs of CPU per 33 KB chunk, with interrupts off.
The direct engine takes 14 µs per submit, and the fill task dropped from 25–40 %
to 13–19 % of a core. Cache coherency is handled where the framebuffers are
written instead (see [Cache coherency](#cache-coherency)).

The DMA writes faster than the panel reads (about 77 vs 44 MB/s), so it stays
ahead of the scan as long as it finishes within about **two** bounce periods.
`g_bbDmaMinSlackUs` and `g_bbDmaLate` measure against that. "Late" is the
glitch band described above.

### Row cache

Each screen row can carry a short op list in PSRAM: RUN (a 32-bit pixel pair
repeated) and COPY (fetch words from the framebuffer). The fill then expands the
list on the CPU instead of DMAing the whole 2 KB row.

- **How rows get encoded:** from the bounce buffer just after their DMA lands
  (no extra PSRAM reads). A row is encoded only once it has stayed unchanged for
  one harvest, so rows that animate every frame stay on the DMA path. A row whose
  list plus copied pixels would exceed 1 KB stays raw.
- **Who reports changes:** writers call `markRowsChanged(y1, y2, fbIndex)`. With
  three framebuffers the marks are kept per buffer and applied when that buffer
  is shown.
- **What it saves:** with a mostly still screen, about 90 % of rows are cached,
  and the fill's PSRAM reads fall from about 40 to about 6 MB/s.

An *authoritative* row cache was tried and abandoned. The renderer spliced
LVGL's rows straight into the op lists, leaving the framebuffer stale. Every row
was then served from op lists, but each splice cost about 40 µs of PSRAM latency
and lists fragmented: 5 fps where the simple cache gave 11. It would need
50–80 KB of SRAM for the op lists to pay off.

### Fill-ahead

A 300 µs periodic timer asks the fill task to pre-fill the rows of the bounce
buffer the panel has already scanned, with the next chunk. The scan position is
(now − last bounce interrupt) / line time (1386 pixel clocks), less 2 rows of
margin. That adds up to one bounce period to each fill's deadline. It never
crosses a frame boundary (the flip is latched there) and never runs while
vertical blanking precedes the buffer's scan. About 55–65 % of rows get
pre-filled.

It is **automatic: on with the CPU fill, off with GDMA**
(`setFillAhead()` overrides). GDMA already has two periods of slack, and
fill-ahead chains queued ahead of a real fill made things worse (46 late fills in
20 s versus 0).

### CPU fill

`BOUNCE_FILL_SAME_CORE` copies uncached rows with the CPU instead of GDMA. The
CPU reads through the cache, so this mode needs **no cache write-back at all**,
and never blocks interrupts for one. The price is CPU on core 1. With Arduino on
core 0 (fps / late fills per 2 s):

| Fill | 5 balls | 10 balls | 20 balls |
| --- | --- | --- | --- |
| GDMA | 33 / 0 | 33 / 0 | 25 / 0 |
| CPU + fill-ahead | 33 / 0 | 33 / 7 | 15 / 29 |

About 40 % of rows have no op list even at 5 balls (changed rows, plus detailed
text), so the CPU copies about 25 MB/s and core 1 saturates under motion. The CPU
fill suits mostly-still UIs where interrupt latency matters more than frame rate.

### Pacing

The copy task calls `paceWrites()` before each 8-row piece it writes. It waits
while the last DMA chunk had less than `LCD7_7B_PACE_SLACK_US` (700 µs) of slack,
so a busy UI slows itself rather than glitching the panel. The threshold is tuned
for 20-row bounce buffers. Halving them to 10 rows cost about 35 % of the frame
rate (twice the per-chunk overhead, and the pacing then throttles constantly), so
`LCD7_7B_BOUNCE_ROWS` stays at 20.

---

## Cache coherency

The GDMA reads PSRAM behind the data cache, so CPU writes to the framebuffers
must be written back before the DMA reads them.

**Now: range write-backs.**

- **How it works:** the renderer writes back each 8-row piece it writes (blit and
  reconcile) right after writing it, with `writeBackRange()`. It calls
  `setRangeWriteBack(true)`, so the flip does no whole-cache write-back.
- **Interrupts off:** `writeBackRange()` uses `Cache_WriteBack_Addr` in 4 KB
  pieces, each in its own critical section. That holds interrupts off on core 1
  for at most 45–75 µs, and never stalls core 0.
- **Cost:** the cache steps through every line of a range, dirty or not, at
  about 70–80 ns per 32-byte line. So the cost tracks bytes written: 12–30 % of
  core 1 at 5–20 balls.
- **Testing:** 36 board-minutes of soak with Arduino on core 0 and LVGL's heap
  in PSRAM, plus 15 board-minutes at 20 balls. No crashes.

**Before that, a crash.** The flip used to write back the whole cache with
`Cache_WriteBack_All()`. With Arduino on core 0, that crashed core 0 every 1–2
minutes, in unrelated code: a pointer holding pixel values, corrupted spinlocks
in the I²C driver, an interrupt-watchdog timeout. Both cores share the data
cache, and a whole-cache operation walks the lines core 0 is using.

- **Isolating it:** removing the write-back stopped the crashes.
- **Ruled out:** stacks, the copy task, the row cache and the GDMA engine.
- **The first fix:** stall the other core around it with
  `esp_ipc_isr_stall_other_cpu()`, as ESP-IDF's flash driver does around
  whole-cache operations. It worked (18 board-minutes, no crash), but blocked
  interrupts on *both* cores for up to ~700 µs per frame.
- **Why range write-backs win:** they removed that block, with the same
  stability. They are what ESP-IDF's own `esp_cache_msync` does unstalled. The
  hardware mechanism behind the original crash is not proven.

`setRangeWriteBack(false)` restores the whole-cache-plus-stall path for a writer
that doesn't write back its own ranges. The drawing calls (`fillRect`, …) write
back their own rows.

All cache-sync operations hold one spinlock (`s_cache_mux`). The S3's sync
registers take one operation at a time from either core. Two at once panicked
with "Dcache sync parameter configuration error" when a flip on one core met a
DMA submit on the other.

**Data-cache autoload** (`LCD7_7B_DCACHE_AUTOLOAD`) is enabled only for the CPU
fill. With GDMA it can't help, and autoload combined with a range write-back
hung the cache.

---

## LVGL integration (`lv_setup`)

### The renderer path, every rotation

LVGL renders PARTIAL in its own orientation. `LvglRotatedRenderer` copies (0°),
mirrors (180°) or rotates (90/270°) each area into the landscape framebuffers,
so the driver always runs at 0° and every PSRAM write goes through code that
paces and writes back.

### Band size

LVGL redraws every object an area touches, and an area's height is the band
size divided by its width. So a wide redraw cut into many areas repeats work: a
list row cut across three areas is laid out and drawn three times. Scrolling a
half-width list with Arduino on core 0:

| Band | 40 KB | **80 KB (default)** | 128 KB |
| --- | --- | --- | --- |
| Half-width list | 8 fps | 11 | 12 |
| Full-width list | 5.5 | 7.5 | 8.5 |

BouncingBalls' small areas run the same at 40 KB.

### Triple buffering and the frame tick

- **Triple buffering** (`tripleBuffer`, on; 1.2 MB of PSRAM): `frameDone()`
  queues the finished frame and LVGL starts the next at once.
  - It waits only if the frame queued before is still unshown (a one-deep FIFO;
    skipping a frame would also skip its row-cache marks).
  - Each buffer keeps a "missing compared with the newest frame" list. The
    buffer being built subtracts what LVGL draws, then copies the rest from the
    newest complete frame.
  - 20 balls went from 10.9 to 15.6 fps; the flip wait went from about 20 ms
    to 0.3 ms.
- **Frame tick** (`frameTick`, on): `lv_tick` is `display.frameTickMs()`, which
  advances once per refresh by the exact frame period. So `lv_anim` steps
  (scroll snap-back, transitions) land one frame apart. Side effect: LVGL's own
  CPU and timing figures, which use `lv_tick`, read 0, so the examples show
  per-core load from FreeRTOS instead (`cglcd7_7b_idle_percent_core0/1()`).

### The copy on core 1 (`asyncCopy`)

- **What it does:** the band is split in two. The flush queues an area to the
  copy task and returns, LVGL renders the next area into the other half, and
  `flush_wait_cb` blocks only when it needs a half the task hasn't released.
- **Where it runs:** the copy task runs on core 1, priority 5, below the fill.
- **With Arduino on core 0:** LVGL draws on core 0 while core 1 copies, and the
  wait for a free half is only about 6 % of core 0's time.
- **Why it's forced on there:** with Arduino on core 0 and a synchronous flush,
  the copy would run on core 0 and collide with the fill. So `asyncCopy` is
  forced on whenever LVGL runs off the display core.

| 20 balls, full resolution | fps |
| --- | --- |
| Arduino on core 1 | ~16.5 |
| Arduino on core 0 | ~23–25 |

### LVGL's heap in PSRAM

The examples set `LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM`, and `lv_setup` supplies
the allocator: PSRAM first, internal RAM as fallback. Draw layers and glyph
buffers are pointed back at internal RAM first through LVGL's draw-buffer
handlers, because every blend reads them.

- **What it saves:** the 64 KB internal pool is gone, which leaves room for
  Wi-Fi and TLS and lets the UI grow.
- **The internal pool's limits:** two 60-row lists overran it (task watchdog);
  a 96 KB pool froze when the UI was rebuilt.
- **Cost:** none measurable. Scrolling a list ran 9 fps on the PSRAM heap
  versus 8.5 with the internal pool.

### Build settings in `lv_conf.h`

- **`#pragma GCC optimize ("O2")`** inside the `LV_CONF_H` guard. Every LVGL
  source includes `lv_conf.h` first, so it reaches all of LVGL without touching
  the build. About 10 % less LVGL render time, for about 80 KB of flash.
- **`LV_DEF_REFR_PERIOD 5`.** It is also LVGL's animation timer, and at 33 ms it
  beat against the 30.6 ms frame.
- **`LV_DRAW_SW_DRAW_UNIT_CNT 1`, `LV_OS_NONE`.** A second draw unit measured no
  gain: LVGL runs only non-overlapping draw tasks in parallel, and a widget
  always overlaps the background beneath it.

### Where LVGL's time goes

Profiled while scrolling a list: about 300 µs of per-object work for every area
an object touches, about 1 ms per label, about 27 µs per glyph. The software
renderer, not the display, sets the frame rate for text-heavy motion.

| Arduino on core 0, scrolling | Full resolution | Half resolution |
| --- | --- | --- |
| Half-width list | 11 fps | 28.5 fps |
| Full-width list | 7 fps | 19 fps |
| Half-width picture | 16.5 fps | 33 fps |
| Full-width picture | 9 fps | 33 fps |

A picture held in PSRAM, scrolled with Arduino on core 0, gives up to about 15
late fills a second at full resolution: LVGL on core 0 streams the picture from
PSRAM while core 1's fill needs the same bus. With Arduino on core 1 there were
none.

---

## Half resolution

`lv_setup.halfResolution = true` (or `display.setHalfResolution(true)` before
`begin()`):

- **Sizes:** the framebuffers are 512×300 (300 KB) and LVGL sees 512×300, or
  300×512 in portrait. Touch coordinates are halved.
- **The fill** (`_fillHalf`) scales up 2× as it fills each bounce buffer:
  - even output rows are source rows, each pixel followed by its average with
    the next;
  - odd rows average the even rows above and below;
  - the right and bottom edges repeat the last pixel;
  - a chunk's last odd row takes its next row straight from the source
    framebuffer, so chunk boundaries need nothing from the other bounce buffer.
- **No GDMA, no write-backs, no row cache.** The CPU reads through the cache, and
  the fill reads about 10 MB/s of PSRAM instead of 40.
- **Not supported:** the drawing calls, sprites and scrolling.

**The averaging.** RGB565 pixels are averaged two per 32-bit word:
`(a & b) + (((a ^ b) & 0xF7DEF7DE) >> 1)`. The mask clears the lowest bit of
each colour field and of each pixel, so the halving never borrows across them.
It was checked against a per-channel reference.

**SIMD (PIE).** The doubling and averaging run on the S3's 128-bit vector unit,
bit-identical to the C versions:

| Cycles per output pixel | Doubling a row | Averaging two rows |
| --- | --- | --- |
| C | 5.0 | 7–8.5 |
| SIMD | 1.0–1.4 | 1.4–1.5 |

The whole fill went from about 50 % to about 26 % of core 1. Instruction facts,
probed on the chip:

- **The halving:** `ee.vsr.32` shifts arithmetically, so it uses `ee.vmul.u16`
  by a vector of 1s with SAR = 1 (an unsigned per-lane shift).
- **The add:** `ee.vadds.s16` saturates as signed, and a correct average can pass
  0x7FFF. XORing each lane with 0x8000 before and after the add makes it exact.
- **Neighbours and interleaving:** `ee.src.q` with SAR_BYTE = 2 gives each
  pixel's right-hand neighbour. `ee.vzip.16` interleaves pixels and their
  averages into output order.
- **Alignment:** the bounce buffers are only 4-byte aligned (measured 8 and 12
  bytes off). The routines do the first words in C up to 16-byte alignment, then
  extract both the pixels and their neighbours from aligned source vectors,
  re-setting SAR_BYTE between the two.
- **Inlining:** they use `loopgtz` hardware loops, and are `noinline` so they
  never land inside a loop the compiler has made a hardware loop itself (that
  hung).

| Half resolution, scrolling | Arduino on core 0 | Arduino on core 1 |
| --- | --- | --- |
| Half-width list | 28.5 fps | 11 fps |
| Full-width list | 19 | 13.5 |
| Pictures | 33 | 14.5–24 |

There were no late fills in any of these runs.

---

## Refresh rate

Refresh is pclk / (htotal × vtotal), with the porches in the table above:
pclk / 916 146 on the -7B (**32.7 Hz at the default 30 MHz**) and pclk / 410 000
on the -7 (**34.1 Hz at 14 MHz**).

- **-7B: the panel kept lock down to 12 MHz** (`LCD7_7B_MIN_PCLK_HZ_7B`), but
  dark areas visibly flicker below about 30 Hz. So the practical floor is
  flicker, not lock.
- **-7: the ST7262 drops lock below ~12 MHz** and runs its self-test colour
  cycle (white/red/green/blue/black, ~4 s each). 11.5 MHz never locked, 12 MHz
  was marginal from a cold start, 12.5 MHz was the lowest reliable value; the
  clamp is 13 MHz for margin. Waveshare's stock value is 16 MHz (~39 Hz).
- **CPU cost:** with the old all-CPU fill (no GDMA, no row cache), the
  full-frame copy left about 28 % of the core at 30 MHz and 71 % at 12 MHz.
- **PSRAM clock:** `psramClockMHz()` reads the PSRAM clock from the memory
  controller. The Arduino core's prebuilt libraries fix it at 80 MHz. Waveshare's
  factory firmware ran 120 MHz, which needs a custom build.

## Start-up

- Framebuffer 1 is cleared before `esp_lcd_panel_init()`. Clearing it after,
  while the first GDMA fills ran, made dozens of late fills at boot.
- Later framebuffers are cleared in 8 KB pieces gated by `paceWrites()` and
  written back.
- The slack books skip chunks completed before a bounce period has been
  measured.
- The backlight comes on at the end of `begin()`.

## Rotation

All rotations go through the renderer path (see above). Without it — RGB332
mode, an explicit CPU fill with `partialRender` off — 180° is done in the fill
instead: the PSRAM side is still read forward and the SRAM side written
backward.

## Backlight

`setBacklight(percent)`: 0 turns the backlight off (EXIO2 low). On the -7B,
1–100 sets the expander's PWM and turns the boost converter on. The -7's
CH422G has no PWM, so 1–100 is simply on. The PWM is inverted by the
hardware (it injects into the converter's feedback); the board library hides
that. See the board's
[README-engineering.md](https://github.com/chipguyhere/ESP32-S3-Touch-LCD-7-7B_board/blob/main/README-engineering.md#backlight-18).

## Touch

The GT911 driver comes from TAMC_GT911. Its `read()` took the point count from
the low nibble of a status byte (up to 15) and wrote that many points into
`points[5]`. A failed I²C read returns 0xFF ("ready, 15 touches"), and the
overflow overwrote the `_board` reference after the array. Counts above 5 are
now rejected and counted in `badReads`.

## Kept from the -7, unchanged in principle

- **Flash-write overlay** (`beginFlashMessage()`): while flash writes stall
  PSRAM, the interrupt fills the bounce buffers from an SRAM bitmap.
- **Desync workaround**, disabled, behind `LCD7_7B_BOUNCE_DESYNC_WORKAROUND`.
- **Sprites and hardware scroll**, in the fill path. They are not used by the
  renderer path, and not supported at half resolution.

## Tried and rejected

| Idea | Result |
| --- | --- |
| Authoritative row cache (splice LVGL rows into op lists) | 5 fps vs 11; needs ~50–80 KB SRAM |
| Two LVGL draw units | no gain |
| `translate_x/y` or `lv_obj_move_to()` instead of `lv_obj_set_pos()` | same cost (a parent layout update either way) |
| Copy task on core 0 | cross-core PSRAM collisions: late fills |
| Stricter pacing, or waiting for DMA idle, against late fills with Arduino on core 0 | slower, late fills remained |
| 10-row bounce buffers | ~35 % slower |
| Fill-ahead with GDMA | more late fills |
| Fusing the half-resolution doubling and averaging into one pass | slower (branches) |
| The -7's cooperative dual-core fill | dropped in the port |
