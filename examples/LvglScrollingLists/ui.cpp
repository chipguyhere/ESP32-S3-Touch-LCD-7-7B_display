/*
 * ui.cpp - A resolution-independent "scrolling lists" demo in plain LVGL 9.
 *
 * Lists are what most UIs scroll, so this measures how smoothly a typical one
 * moves: either two half-width lists side by side, or one full-width list.
 * Either way they fill the middle two thirds of the screen, as they would in a
 * real UI with a header and some buttons around them.
 *
 *   - "One list" / "Two lists" swaps the layout.
 *   - "Image" swaps a list for a large picture (one side, then both sides).
 *     The picture lives in a big memory buffer, so scrolling it makes LVGL
 *     read a lot of memory -- a heavier, different kind of load than text.
 *   - The Auto-scroll switch moves the left (or only) pane by itself, so the
 *     frame rate can be watched without touching the screen.
 *
 * Nothing here is pinned to a resolution: ui_init() reads the live display size
 * from LVGL, so the same file works on any board whose sketch brings up LVGL
 * through an lv_setup.hpp of the same shape (portrait or landscape, any size).
 */

#include "ui.h"
#include <math.h>
/* lv_obj_scroll_by_raw(): what LVGL's own touch scrolling uses (see below). */
#include "core/lv_obj_scroll_private.h"

/* ----------------------------------------------------------------- tunables */

#ifndef LIST_ITEMS
#define LIST_ITEMS        60     /* rows per list                             */
#endif
#define AUTO_SCROLL_PX_S  240    /* auto-scroll speed, pixels per second      */
#define IMAGE_HEIGHTS     3      /* picture height, in list heights           */

/* ----------------------------------------------------------------- state    */

static lv_obj_t      *g_area;          /* the middle two thirds               */
static lv_obj_t      *g_panes[2];      /* one or two lists or pictures        */
static int            g_pane_count = 0;
static bool           g_two = true;
static int            g_image = 0;     /* 0 none, 1 one side, 2 both sides    */
static bool           g_auto = false;
static bool           g_dragging = false;   /* panes told SCROLL_BEGIN      */
static int            g_dir[2] = {1, -1};   /* auto-scroll direction per pane */
static uint32_t       g_last_tick = 0;
static lv_obj_t      *g_mode_label, *g_image_label;
static lv_draw_buf_t *g_picture;       /* made on first use, then kept        */

/* ----------------------------------------------------------------- filler   */

static const char *const kWords[] = {
    "lorem", "ipsum", "dolor", "sit", "amet", "consectetur", "adipiscing",
    "elit", "sed", "do", "eiusmod", "tempor", "incididunt", "ut", "labore",
    "et", "dolore", "magna", "aliqua", "enim", "ad", "minim", "veniam",
    "quis", "nostrud", "exercitation", "ullamco", "laboris", "nisi",
    "aliquip", "ex", "ea", "commodo", "consequat", "duis", "aute", "irure",
    "in", "reprehenderit", "voluptate", "velit", "esse", "cillum", "fugiat",
    "nulla", "pariatur", "excepteur", "sint", "occaecat", "cupidatat",
};
#define N_WORDS (sizeof(kWords) / sizeof(kWords[0]))

/* A repeatable line of 4..9 filler words for row n of list l. */
static void filler(char *out, size_t cap, int l, int n) {
    uint32_t s = (uint32_t)(l * 7919 + n * 104729 + 1);
    int words = 4 + (int)(s % 6);
    size_t len = 0;
    for (int i = 0; i < words && len + 16 < cap; i++) {
        s = s * 1103515245u + 12345u;
        const char *w = kWords[(s >> 16) % N_WORDS];
        len += lv_snprintf(out + len, cap - len, i ? " %s" : "%s", w);
    }
    if (len && out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;   /* capitalize */
}

/* ----------------------------------------------------------------- picture  */

/* A colourful "plasma" picture, drawn once into a big buffer.  It's made from
 * a few overlapping waves and rings mapped through a rainbow palette, so every
 * row is different and nothing compresses. */
static lv_draw_buf_t *make_picture(int32_t w, int32_t h) {
    /* The image handlers' memory: with this board's lv_setup that's PSRAM. */
    lv_draw_buf_t *buf = lv_draw_buf_create_ex(lv_draw_buf_get_image_handlers(),
                                               w, h, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (!buf) return NULL;

    static uint16_t palette[256];
    static int8_t   wave[256];
    for (int i = 0; i < 256; i++) {
        float a = i * (float)M_PI * 2 / 256;
        wave[i] = (int8_t)(sinf(a) * 63);
        lv_color_t c = lv_color_hsv_to_rgb((uint16_t)(i * 360 / 256), 80, 95);
        palette[i] = lv_color_to_u16(c);
    }
    int32_t cx = w / 2, cy = h / 3;
    for (int32_t y = 0; y < h; y++) {
        uint16_t *row = (uint16_t *)(buf->data + (size_t)y * buf->header.stride);
        for (int32_t x = 0; x < w; x++) {
            int32_t dx = x - cx, dy = y - cy;
            int ring = (int)(sqrtf((float)(dx * dx + dy * dy)) * 0.08f * 256 / 6.2832f);
            int v = wave[(x * 2) & 255] + wave[(y * 3) & 255] +
                    wave[((x + y) * 2) & 255] + wave[ring & 255];
            row[x] = palette[(v + 128 + y / 4) & 255];
        }
    }
    return buf;
}

/* ----------------------------------------------------------------- panes    */

static const char *const kIcons[] = {
    LV_SYMBOL_FILE, LV_SYMBOL_IMAGE, LV_SYMBOL_AUDIO, LV_SYMBOL_VIDEO,
    LV_SYMBOL_DIRECTORY, LV_SYMBOL_SETTINGS,
};

static lv_obj_t *make_list(lv_obj_t *parent, int which) {
    lv_obj_t *list = lv_list_create(parent);
    char line[96];
    for (int n = 0; n < LIST_ITEMS; n++) {
        if (n % 15 == 0) {
            lv_snprintf(line, sizeof(line), "Section %d", n / 15 + 1);
            lv_list_add_text(list, line);
        }
        filler(line, sizeof(line), which, n);
        lv_obj_t *btn = lv_list_add_button(list, kIcons[(n + which) % 6], line);
        /* A list button's text scrolls in a loop when it doesn't fit, which
         * redraws it on every frame.  End it with "..." instead, as most UIs do. */
        for (uint32_t i = 0; i < lv_obj_get_child_count(btn); i++) {
            lv_obj_t *child = lv_obj_get_child(btn, (int32_t)i);
            if (lv_obj_check_type(child, &lv_label_class))
                lv_label_set_long_mode(child, LV_LABEL_LONG_DOT);
        }
    }
    return list;
}

/* A scrolling pane showing the picture (or a note if there wasn't room). */
static lv_obj_t *make_picture_pane(lv_obj_t *parent) {
    lv_obj_t *pane = lv_obj_create(parent);
    lv_obj_set_style_pad_all(pane, 0, 0);
    lv_obj_set_scroll_dir(pane, LV_DIR_VER);
    if (g_picture) {
        lv_obj_t *img = lv_image_create(pane);
        lv_image_set_src(img, g_picture);
    } else {
        lv_obj_t *msg = lv_label_create(pane);
        lv_label_set_text(msg, "Not enough memory for the picture.");
        lv_obj_center(msg);
    }
    return pane;
}

/* Auto-scroll behaves like one long finger drag: SCROLL_BEGIN once, raw
 * scroll steps, SCROLL_END when it stops -- exactly what LVGL's touch input
 * does.  (lv_obj_scroll_by() would instead send BEGIN and END around every
 * step, and each pair restyles and re-lays out every row of the list:
 * measured at more than twice the cost of drawing it.) */
/* Only the left (or only) pane moves: a real UI rarely scrolls two lists at
 * once.  The other pane stays put, the way a second list would. */
static void drag_begin(void) {
    if (g_dragging || !g_pane_count) return;
    lv_obj_send_event(g_panes[0], LV_EVENT_SCROLL_BEGIN, NULL);
    g_dragging = true;
}
static void drag_end(void) {
    if (!g_dragging) return;
    lv_obj_send_event(g_panes[0], LV_EVENT_SCROLL_END, NULL);
    g_dragging = false;
}

/* (Re)build the panes in the middle area for the current settings. */
static void build_panes(void) {
    drag_end();
    for (int i = 0; i < g_pane_count; i++) lv_obj_delete(g_panes[i]);
    g_pane_count = g_two ? 2 : 1;

    int32_t aw = lv_obj_get_content_width(g_area);
    int32_t ah = lv_obj_get_content_height(g_area);
    int32_t gap = 12;
    if (g_image && !g_picture) g_picture = make_picture(aw, ah * IMAGE_HEIGHTS);
    for (int i = 0; i < g_pane_count; i++) {
        bool picture = (g_image == 2) || (g_image == 1 && i == 0);
        lv_obj_t *pane = picture ? make_picture_pane(g_area) : make_list(g_area, i);
        int32_t w = g_two ? (aw - gap) / 2 : aw;
        lv_obj_set_size(pane, w, ah);
        lv_obj_set_pos(pane, i * (w + gap), 0);
        g_panes[i] = pane;
    }
    lv_label_set_text(g_mode_label, g_two ? "One list" : "Two lists");
    static const char *const kImageText[] = {"Image: none", "Image: one side", "Image: both"};
    lv_label_set_text(g_image_label, kImageText[g_image]);
}

/* ----------------------------------------------------------------- scroll   */

/* Move the pane by however far AUTO_SCROLL_PX_S says it should have gone since
 * the last call, so the speed stays constant whatever the frame rate is -- and
 * a slow frame shows up as a jump. */
static void auto_scroll_cb(lv_timer_t *t) {
    LV_UNUSED(t);
    uint32_t now = lv_tick_get();
    uint32_t dt = now - g_last_tick;
    g_last_tick = now;
    if (!g_auto || dt == 0) return;
    int32_t step = (int32_t)(AUTO_SCROLL_PX_S * dt / 1000);
    if (step <= 0) return;
    drag_begin();
    for (int i = 0; i < 1 && i < g_pane_count; i++) {
        lv_obj_t *pane = g_panes[i];
        if (g_dir[i] > 0 && lv_obj_get_scroll_bottom(pane) <= 0) g_dir[i] = -1;
        if (g_dir[i] < 0 && lv_obj_get_scroll_top(pane) <= 0)    g_dir[i] = 1;
        int32_t room = g_dir[i] > 0 ? lv_obj_get_scroll_bottom(pane) : lv_obj_get_scroll_top(pane);
        int32_t s = step < room ? step : room;
        if (s > 0) lv_obj_scroll_by_raw(pane, 0, -g_dir[i] * s);
    }
}

/* ----------------------------------------------------------------- controls */

static void mode_clicked(lv_event_t *e) {
    LV_UNUSED(e);
    ui_set_two_lists(!g_two);
}

static void image_clicked(lv_event_t *e) {
    LV_UNUSED(e);
    ui_set_image((g_image + 1) % 3);
}

static void auto_changed(lv_event_t *e) {
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    ui_set_auto_scroll(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/* A button whose width fits the longest text it will show. */
static lv_obj_t *make_button(lv_obj_t *parent, const char *longest, lv_obj_t **label,
                             lv_event_cb_t cb) {
    lv_obj_t *btn = lv_button_create(parent);
    *label = lv_label_create(btn);
    lv_label_set_text(*label, longest);
    lv_obj_center(*label);
    lv_obj_update_layout(btn);
    lv_obj_set_width(btn, lv_obj_get_width(btn));
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/* ----------------------------------------------------------------- build    */

void ui_init(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101018), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_display_t *disp = lv_display_get_default();
    int32_t W = lv_display_get_horizontal_resolution(disp);
    int32_t H = lv_display_get_vertical_resolution(disp);
    int32_t m = 12;              /* margin                                    */
    int32_t band = H / 6;        /* header and footer: a sixth each           */

    /* --- header: controls on the right, title and hint on the left --- */
    /* The controls come first and always stay reachable: a right-aligned row
     * that wraps onto a second line only if the screen is too narrow.  The
     * title and hint use whatever room is left beside them, shortened or
     * hidden when there isn't enough. */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, W - 2 * m, LV_SIZE_CONTENT);
    lv_obj_set_pos(bar, m, m);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 12, 0);
    lv_obj_set_style_pad_row(bar, 8, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    make_button(bar, "Image: one side", &g_image_label, image_clicked);
    make_button(bar, "Two lists", &g_mode_label, mode_clicked);
    lv_obj_t *sw_label = lv_label_create(bar);
    lv_obj_set_style_text_color(sw_label, lv_color_white(), 0);
    lv_label_set_text(sw_label, "Auto-scroll");
    lv_obj_t *sw = lv_switch_create(bar);
    lv_obj_set_size(sw, 50, 26);
    lv_obj_add_event_cb(sw, auto_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_update_layout(bar);

    /* Room for the title: left of the controls' first row.  If the controls
     * wrapped onto a second line, there is none. */
    int32_t first_y = lv_obj_get_y(lv_obj_get_child(bar, 0));
    bool wrapped = lv_obj_get_y(lv_obj_get_child(bar, -1)) > first_y + 4;
    int32_t left = W;
    for (uint32_t i = 0; i < lv_obj_get_child_count(bar); i++) {
        lv_obj_t *ch = lv_obj_get_child(bar, (int32_t)i);
        if (lv_obj_get_y(ch) <= first_y + 4 && lv_obj_get_x(ch) < left) left = lv_obj_get_x(ch);
    }
    int32_t room = wrapped ? 0 : left - 16;   /* x in the bar = screen x - m */

    int32_t header_bottom = m + lv_obj_get_height(bar);
    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Scrolling lists");
    lv_obj_set_pos(title, m, m);
    lv_obj_update_layout(title);
    if (lv_obj_get_width(title) > room) {
        lv_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);   /* the controls need the width */
    } else {
        lv_obj_t *hint = lv_label_create(scr);
        lv_obj_set_style_text_color(hint, lv_color_hex(0xA0A0B0), 0);
        lv_label_set_text(hint, "Drag a list, or turn on Auto-scroll.");
        lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
        lv_obj_set_width(hint, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(hint, room, 0);
        lv_obj_align_to(hint, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
        lv_obj_update_layout(hint);
        int32_t hb = lv_obj_get_y(hint) + lv_obj_get_height(hint);
        if (hb > header_bottom) header_bottom = hb;
    }

    /* --- the lists: below the header, above the bottom sixth (kept for the
     * sketch's notes) --- */
    int32_t top = header_bottom + m;
    if (top < band) top = band;
    g_area = lv_obj_create(scr);
    lv_obj_remove_style_all(g_area);
    lv_obj_set_size(g_area, W - 2 * m, H - top - band);
    lv_obj_set_pos(g_area, m, top);
    lv_obj_remove_flag(g_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(g_area);
    build_panes();

    g_last_tick = lv_tick_get();
    lv_timer_create(auto_scroll_cb, 10, NULL);
}

void ui_set_two_lists(bool two) {
    if (two == g_two && g_pane_count) return;
    g_two = two;
    build_panes();
}

void ui_set_image(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
    g_image = mode;
    build_panes();
}

void ui_set_auto_scroll(bool on) {
    g_auto = on;
    if (!on) drag_end();
    g_last_tick = lv_tick_get();
}
