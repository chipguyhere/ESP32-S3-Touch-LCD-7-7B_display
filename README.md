# chipguy_Waveshare-ESP32-S3-Touch-LCD-7-7B_display

A **display + touch** library for two 7-inch Waveshare boards:

| Board | Screen |
| --- | --- |
| **ESP32-S3-Touch-LCD-7** | 800×480 |
| **ESP32-S3-Touch-LCD-7B** | 1024×600, with a dimmable backlight and a battery charger |

It runs the screen and the touch for you. You write an **LVGL 9** app and
nothing else. The same sketch runs on either board: the library works out which
one it is when it starts, and the screen size follows.

The examples are the heart of the library. Two of them are **starting points**:
the screen, touch and LVGL are already running before your code gets control.
Pick the one that matches how you want to build your screens, copy it, and
replace its placeholder UI with your own.

---

## Installation

There is no Arduino Library Manager entry. Install from GitHub ZIP files:

1. Download **this library** and **`chipguy_Waveshare-ESP32-S3-Touch-LCD-7-7B_board`**
   (its partner — this library needs it). On each GitHub page, click
   **Code ▸ Download ZIP**.
2. Unzip both. Move the folders into your Arduino **libraries** folder:
   - macOS / Linux: `~/Documents/Arduino/libraries/`
   - Windows: `Documents\Arduino\libraries\`
3. Install **LVGL** from the Arduino Library Manager (search "lvgl"). Version 9.3
   or later.
4. Restart the Arduino IDE so the examples appear in the **File** menu.

### Arduino IDE board settings

In the **Tools** menu, set:

- **Board:** ESP32S3 Dev Module
- **PSRAM:** OPI PSRAM *(required — the picture is stored there)*
- **Flash Size:** 8MB (64Mb) — works on both boards. The -7B has 16 MB; pick
  16MB (128Mb) there to use all of it.
- **Partition Scheme:** 8M with spiffs (3MB APP/1.5MB SPIFFS), or on the -7B
  with 16 MB chosen, 16M Flash (3MB APP/9.9MB FATFS)
- **Upload Speed:** 230400 *(faster speeds failed on a -7B; the -7 takes 460800)*
- **Arduino Runs On:** Core 0 for smoother screens. Core 1 works too. See
  [Which core to run on](#which-core-to-run-on) below.

### Which USB port

Each board has two USB-C ports. Plug into the one marked **UART** to upload and
to see `Serial0` messages. A small slide switch near it, marked **UART1 / UART2**,
must be on **UART1**, or nothing gets through.

---

## The examples — pick your starting point

Open them from **File ▸ Examples ▸ chipguy_Waveshare-ESP32-S3-Touch-LCD-7-7B_display**.
To make one your own, use **File ▸ Save As** first, so your work lives outside
the library. Each example folder has its own `lv_setup.hpp` and `lv_conf.h`, so a
copied sketch stands on its own.

### LvglClaudeCodeStub — write the UI yourself (by hand or with an AI)

The UI lives in `ui.h` / `ui.cpp` in the sketch folder. `ui_init()` gets a live
LVGL screen. Delete the demo inside it (a label that follows your finger and a
color-changing box) and build your own screens with normal LVGL calls. This is
the one to hand to an AI coding assistant such as Claude Code.

### Lvgl93SquarelineLauncher — design the UI in SquareLine Studio

[SquareLine Studio](https://squareline.io/) is a drag-and-drop screen designer.
Make a project the size of your board's screen — **1024×600** for the -7B,
**800×480** for the -7 — with **16-bit** color and no rotation. Export the UI
files and replace the sketch's whole `src/` folder with the export. Your own code
goes in the `.ino`. A SquareLine design is made for one screen size, so this is
the one example that doesn't move between the two boards by itself.

### The finished demos

- **LvglCalculator** is a working calculator.
- **LvglBouncingBalls** is an animation. Sliders set how many balls there are and
  how fast they move.
- **LvglScrollingLists** shows lists of text, either two side by side or one
  wide one. Its **Image** button swaps in a big picture instead. Turn on
  **Auto-scroll** to watch them move by themselves.
- **RasterBouncingBalls** draws up to 200 balls at the screen's full refresh
  rate. It does this by skipping LVGL for the top of the screen and drawing
  straight into the pictures being sent to the panel. Only the controls at the
  bottom are LVGL. It shows what the hardware can do; most apps don't need it.

All of them size themselves to the screen, so they are good reading for how to
build a layout in plain LVGL. The animated ones show two things in the bottom
corners:

- **Bottom right:** how many frames per second the screen is drawing, and how
  busy each of the ESP32's two cores is, e.g. `24 FPS | CPU 82%,77%`.
- **Bottom left:** which core your program runs on.

---

## The one API you call: `lv_setup.begin()`

```cpp
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

void setup() {
    Serial0.begin(115200);
    lv_setup.begin();           // screen + touch + LVGL, ready to use
    // ...build your UI on lv_screen_active() here (or call ui_init())...
}

void loop() {
    lv_timer_handler();         // let LVGL run
    delay(5);
}
```

- **`lv_setup.begin()`** — call once in `setup()`. After it returns, LVGL is
  running and you build your UI with ordinary LVGL calls.
- **`lv_setup.begin(180)`** — the same, for a screen mounted upside down.
- **`lv_setup.begin(90)` / `begin(270)`** — portrait (the screen turned on its
  side). If the picture or touch comes out the wrong way round, swap 90 and 270.
  `1`, `2` and `3` also work, meaning 90, 180 and 270.
- **`display.width()` / `display.height()`** — the screen's size: 1024 × 600
  on the -7B, 800 × 480 on the -7 (swapped in portrait). Lay out your UI from
  these, not from fixed numbers, and it works on both boards.
- **`board.modelName()`** — which board this is, as text.
- **`display.setBacklight(percent)`** — screen brightness, 0 to 100. 0 turns
  the backlight off. The -7's backlight is only on or off, so there any number
  above 0 means fully on.
- **`lv_setup.halfResolution = true;`** before `lv_setup.begin()` — see
  [Half resolution](#half-resolution) below. Every example has this line, ready
  to uncomment.

Your app code only talks to LVGL. To move it to another chipguy display board,
swap in that board's `lv_setup.hpp` and `lv_conf.h`; the rest stays the same.

### Which core to run on

The ESP32 has two processor cores, numbered 0 and 1. These screens have no memory
of their own, so the ESP32 must re-send the whole picture about 33 times a
second. That work always runs on core 1.

Your program (`setup()`, `loop()` and LVGL) runs on the core chosen in
**Tools ▸ Arduino Runs On**:

- **Core 0 is smoother.** LVGL draws on core 0 while core 1 sends pixels. With a
  lot moving on screen, that is about half again as many frames per second.
- **Core 1 is the safer choice for some sketches.** Keep core 1 if your sketch
  moves a lot of data through PSRAM (big images, large buffers). On core 0 that
  traffic can compete with the screen and show brief flickering bands. On core
  1 the two take turns.

Either setting works. The **Why?** button in the examples explains the same
thing on screen.

### Half resolution

Add `lv_setup.halfResolution = true;` before `lv_setup.begin()` (each example
has the line, commented out). LVGL then draws a picture half as wide and half as
tall as the screen, and the board scales it up to fill the screen, smoothing the
edges. Motion and scrolling get two to three times faster. Small text looks
softer.

Use it for walk-up screens with big buttons: a volume knob, a few source buttons,
a menu. Keep full resolution for text people read at length.

---

## Also on the board

This library only runs the screen and touch. The partner board library covers
the rest:

- **Battery (-7B)** — a socket for a single lithium cell, which charges from the
  UART USB-C port. The board library reads the battery voltage. See
  [README-power.md](https://github.com/chipguyhere/ESP32-S3-Touch-LCD-7-7B_board/blob/main/README-power.md).
- **microSD card slot** — you bring your own card driver. It does not share
  anything with the screen. See
  [README-sdcard.md](https://github.com/chipguyhere/ESP32-S3-Touch-LCD-7-7B_board/blob/main/README-sdcard.md).
- **RS-485, CAN, an I²C header and more** — see
  [README-board.md](https://github.com/chipguyhere/ESP32-S3-Touch-LCD-7-7B_board/blob/main/README-board.md).

Each of those files covers both boards, with a section on what is different on
the -7.

---

## Where to go next

- [README-engineering.md](README-engineering.md) — engineers: how the display
  driver works inside, the measurements behind the advice above, and what was
  tried and rejected.
- [The board library's README](https://github.com/chipguyhere/ESP32-S3-Touch-LCD-7-7B_board/blob/main/README.md)
  — the shared I²C bus, the expander chip, how the two boards are told apart,
  battery, and everything else on the board.
- [README-limitations-errata.md](https://github.com/chipguyhere/ESP32-S3-Touch-LCD-7-7B_board/blob/main/README-limitations-errata.md)
  — anyone choosing a board: what it can't do.
- [schematic-description-7.md](https://github.com/chipguyhere/ESP32-S3-Touch-LCD-7-7B_board/blob/main/schematic-description-7.md)
  and [schematic-description-7B.md](https://github.com/chipguyhere/ESP32-S3-Touch-LCD-7-7B_board/blob/main/schematic-description-7B.md)
  — engineers and LLMs without the drawing: each board's schematic in text.

---

## License

This library is **mixed-license**:

- The original code (display driver, examples) is **MIT** — see [LICENSE](LICENSE).
- The **GT911 touch driver** (`chipguy_ESP32S3_Touch_LCD_7_7B_touch.h` / `.cpp`)
  is a derivative of [TAMC_GT911](https://github.com/tamctec/gt911-arduino)
  (Copyright © TAMC) under the **Apache License, Version 2.0** — see
  [LICENSE-Apache-2.0.txt](LICENSE-Apache-2.0.txt).

The panel timings come from Waveshare's ESP32-S3-Touch-LCD-7 and -7B demos.
`lv_conf.h` is based on LVGL's template (LVGL is MIT-licensed).

This library replaces `chipguy_ESP32-S3-Touch-LCD-7_display` and
`chipguy_Waveshare-ESP32-S3-Touch-LCD-7B_display`.
