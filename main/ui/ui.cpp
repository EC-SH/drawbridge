#include "ui.h"
#include "lvgl.h"
#include "qrcode.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <cstdio>
#include <cstring>

static const char* TAG = "ui_cpp";

// ─────────────────────────────────────────────────────────────────────────────
// THEME — vintage telephone operator's switchboard.
//   deep charcoal board, warm brass borders/labels, ONE hot accent (amber/phosphor)
//   for live lamps. Centralized here in the `t` struct (PALETTES[current_theme]).
//   LVGL v8.4 colour API: LV_COLOR_MAKE(r,g,b).
// ─────────────────────────────────────────────────────────────────────────────
enum Theme {
    THEME_BRASS = 0,    // default: brass on charcoal, amber lamps
    THEME_PHOSPHOR,     // green phosphor lamps (alt operator board)
    THEME_COUNT
};

struct LVColorTheme {
    lv_color_t bg;          // board background (deep charcoal)
    lv_color_t panel;       // recessed panel / tile face
    lv_color_t border;      // warm brass borders / rails
    lv_color_t text;        // brass label text
    lv_color_t highlight;   // bright brass / section headers
    lv_color_t accent;      // hot accent: lit lamp / live indicators
    lv_color_t jack_empty;  // unregistered (dark recessed) tile face
    lv_color_t dnd;         // DND amber ring
    lv_color_t alert;       // destructive (reboot) red
};

static const LVColorTheme PALETTES[THEME_COUNT] = {
    // 0: BRASS — deep charcoal board, brass rails, amber lamps
    {
        LV_COLOR_MAKE(22, 20, 18),     // bg     near-black warm charcoal
        LV_COLOR_MAKE(40, 36, 30),     // panel  recessed brown-charcoal
        LV_COLOR_MAKE(176, 132, 56),   // border brass
        LV_COLOR_MAKE(214, 178, 110),  // text   warm brass
        LV_COLOR_MAKE(245, 214, 150),  // highlight bright brass
        LV_COLOR_MAKE(255, 176, 32),   // accent amber lamp
        LV_COLOR_MAKE(30, 27, 23),     // jack_empty dark recessed
        LV_COLOR_MAKE(255, 176, 32),   // dnd    amber ring
        LV_COLOR_MAKE(200, 64, 40)     // alert  ember red
    },
    // 1: PHOSPHOR — charcoal board, brass rails, green phosphor lamps
    {
        LV_COLOR_MAKE(16, 20, 16),     // bg
        LV_COLOR_MAKE(28, 36, 28),     // panel
        LV_COLOR_MAKE(150, 130, 60),   // border (dim brass)
        LV_COLOR_MAKE(170, 210, 150),  // text
        LV_COLOR_MAKE(210, 240, 190),  // highlight
        LV_COLOR_MAKE(64, 255, 96),    // accent green phosphor
        LV_COLOR_MAKE(22, 28, 22),     // jack_empty
        LV_COLOR_MAKE(255, 176, 32),   // dnd amber ring (still amber for DND)
        LV_COLOR_MAKE(220, 70, 50)     // alert
    }
};

// ─── Globals & UI State ───
static Theme current_theme = THEME_BRASS;
static bool  is_onboarding = false;

// Core layout objects
static lv_obj_t* main_container = nullptr;

// Header (~40px)
static lv_obj_t* header_bar   = nullptr;
static lv_obj_t* title_label  = nullptr;   // "POCKET-DIAL" wordmark
static lv_obj_t* online_dot   = nullptr;   // live online indicator
static lv_obj_t* clock_label  = nullptr;   // uptime / clock
static lv_obj_t* hdr_qr_btn   = nullptr;   // folded-in QR onboarding button
static lv_obj_t* hdr_rb_btn   = nullptr;   // folded-in reboot button

// Status strip (~36px)
static lv_obj_t* status_strip = nullptr;
static lv_obj_t* status_label = nullptr;   // "IP:port  EXT n/32  CALLS k" (monospace)

// Jack board (the hero, scrollable grid)
static lv_obj_t* jack_board   = nullptr;
struct JackTile {
    lv_obj_t* tile  = nullptr;
    lv_obj_t* lamp  = nullptr;
    lv_obj_t* label = nullptr;
    lv_anim_t pulse;
    bool      pulsing = false;
    uint8_t   last_state = 0xFF;   // force first paint
    char      last_ext[12] = {0};
};
static JackTile jacks[UI_MAX_JACKS];

// Active-call / patch strip (~40px)
static lv_obj_t* patch_strip = nullptr;
static lv_obj_t* patch_label = nullptr;    // "106 <-> 777   108 <-> 110"

// Operator-log ticker (~60px). Append-only ring of UI_LOG_LINES lines kept in a
// single label — no growing textarea, so an append repaints only the ticker box.
static lv_obj_t* log_ticker  = nullptr;
static char      log_lines[UI_LOG_LINES][64];
static int       log_head = 0;     // next slot to write
static int       log_count = 0;

// Overlays / Modals (preserved from the original console UI)
static lv_obj_t* qr_modal = nullptr;
static lv_obj_t* qr_canvas = nullptr;
static uint8_t*  qr_canvas_buffer = nullptr;
static lv_obj_t* qr_close_btn = nullptr;

static lv_obj_t* reboot_modal = nullptr;
static lv_obj_t* reboot_title = nullptr;
static lv_obj_t* reboot_msg = nullptr;
static lv_obj_t* reboot_yes_btn = nullptr;
static lv_obj_t* reboot_no_btn = nullptr;

// Jack detail popup (tap a jack -> extension / state / last-seen)
static lv_obj_t* jack_modal = nullptr;
static lv_obj_t* jack_modal_title = nullptr;
static lv_obj_t* jack_modal_msg = nullptr;
static lv_obj_t* jack_modal_close = nullptr;

// Onboarding objects
static lv_obj_t* onboarding_modal = nullptr;
static lv_obj_t* onboarding_title = nullptr;
static lv_obj_t* onboarding_ssid_label = nullptr;
static lv_obj_t* onboarding_qr_canvas = nullptr;
static uint8_t*  onboarding_qr_buffer = nullptr;
static lv_obj_t* onboarding_ap_btn = nullptr;
static lv_obj_t* onboarding_ap_btn_label = nullptr;

// Only Montserrat 14 is enabled in this sdkconfig (the shared sdkconfig is pinned
// and not ours to edit), so it is the one font we may reference. Chunky touch
// targets come from tile geometry, not a bigger glyph set.
#define UI_FONT (&lv_font_montserrat_14)

// ─────────────────────────────────────────────────────────────────────────────
// NOTE: in-call state is shown as a STATIC distinct lamp (white-hot + brass halo),
// not an animation. This panel must run in full_refresh mode (it garbles on partial
// windowed writes — see esp_main_display.cpp), so an infinite lamp animation would
// repaint the entire 320x480 frame every tick. Discrete state changes only.
// ─────────────────────────────────────────────────────────────────────────────
// Theme application. Walks the live object tree and recolours from PALETTES[].
// ─────────────────────────────────────────────────────────────────────────────
static void style_jack_tile(int i, uint8_t state) {
    const LVColorTheme& t = PALETTES[current_theme];
    JackTile& jt = jacks[i];

    // Tile face + brass rail
    lv_color_t face = (state == UI_JACK_EMPTY) ? t.jack_empty : t.panel;
    lv_obj_set_style_bg_color(jt.tile, face, LV_PART_MAIN);
    lv_obj_set_style_border_color(jt.tile, t.border, LV_PART_MAIN);
    lv_obj_set_style_text_color(jt.label, (state == UI_JACK_EMPTY) ? t.border : t.text, LV_PART_MAIN);

    // Tile ring conveys state statically: DND = thick amber (dnd) ring; IN-CALL = a
    // 2px brass (border) halo; everything else = the plain 1px brass rail. Distinct by
    // colour+width with no animation, so a call doesn't repaint the screen continuously.
    if (state == UI_JACK_DND) {
        lv_obj_set_style_border_color(jt.tile, t.dnd, LV_PART_MAIN);
        lv_obj_set_style_border_width(jt.tile, 3, LV_PART_MAIN);
    } else if (state == UI_JACK_INCALL) {
        lv_obj_set_style_border_color(jt.tile, t.highlight, LV_PART_MAIN);
        lv_obj_set_style_border_width(jt.tile, 2, LV_PART_MAIN);
    } else {
        lv_obj_set_style_border_color(jt.tile, t.border, LV_PART_MAIN);
        lv_obj_set_style_border_width(jt.tile, 1, LV_PART_MAIN);
    }

    // Lamp (all static — no animation under full_refresh):
    //   EMPTY  = dim recessed dot      IDLE   = amber lamp lit
    //   DND    = amber lamp            INCALL = white-hot lamp (clearly "this line is live")
    switch (state) {
        case UI_JACK_EMPTY:
            lv_obj_set_style_bg_color(jt.lamp, t.jack_empty, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(jt.lamp, LV_OPA_30, LV_PART_MAIN);
            break;
        case UI_JACK_IDLE:
            lv_obj_set_style_bg_color(jt.lamp, t.accent, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(jt.lamp, LV_OPA_COVER, LV_PART_MAIN);
            break;
        case UI_JACK_DND:
            lv_obj_set_style_bg_color(jt.lamp, t.dnd, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(jt.lamp, LV_OPA_COVER, LV_PART_MAIN);
            break;
        case UI_JACK_INCALL:
            lv_obj_set_style_bg_color(jt.lamp, t.highlight, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(jt.lamp, LV_OPA_COVER, LV_PART_MAIN);
            break;
    }
}

static void apply_theme() {
    const LVColorTheme& t = PALETTES[current_theme];

    lv_obj_set_style_bg_color(main_container, t.bg, LV_PART_MAIN);

    // Header
    lv_obj_set_style_bg_color(header_bar, t.panel, LV_PART_MAIN);
    lv_obj_set_style_border_color(header_bar, t.border, LV_PART_MAIN);
    lv_obj_set_style_text_color(title_label, t.highlight, LV_PART_MAIN);
    lv_obj_set_style_text_color(clock_label, t.text, LV_PART_MAIN);
    lv_obj_set_style_bg_color(online_dot, t.accent, LV_PART_MAIN);
    lv_obj_set_style_bg_color(hdr_qr_btn, t.border, LV_PART_MAIN);
    lv_obj_set_style_bg_color(hdr_rb_btn, t.alert, LV_PART_MAIN);

    // Status strip
    lv_obj_set_style_bg_color(status_strip, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_color(status_strip, t.border, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, t.text, LV_PART_MAIN);

    // Jack board container
    lv_obj_set_style_bg_color(jack_board, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_color(jack_board, t.border, LV_PART_MAIN);
    for (int i = 0; i < UI_MAX_JACKS; i++) {
        uint8_t st = (jacks[i].last_state == 0xFF) ? (uint8_t)UI_JACK_EMPTY : jacks[i].last_state;
        style_jack_tile(i, st);
    }

    // Patch strip
    lv_obj_set_style_bg_color(patch_strip, t.panel, LV_PART_MAIN);
    lv_obj_set_style_border_color(patch_strip, t.border, LV_PART_MAIN);
    lv_obj_set_style_text_color(patch_label, t.accent, LV_PART_MAIN);

    // Log ticker
    lv_obj_set_style_bg_color(log_ticker, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_color(log_ticker, t.border, LV_PART_MAIN);
    lv_obj_set_style_text_color(log_ticker, t.text, LV_PART_MAIN);

    // Modals
    if (qr_modal) {
        lv_obj_set_style_bg_color(qr_modal, t.panel, LV_PART_MAIN);
        lv_obj_set_style_border_color(qr_modal, t.border, LV_PART_MAIN);
        lv_obj_set_style_bg_color(qr_close_btn, t.border, LV_PART_MAIN);
    }
    if (reboot_modal) {
        lv_obj_set_style_bg_color(reboot_modal, t.panel, LV_PART_MAIN);
        lv_obj_set_style_border_color(reboot_modal, t.alert, LV_PART_MAIN);
        lv_obj_set_style_bg_color(reboot_title, t.alert, LV_PART_MAIN);
        lv_obj_set_style_text_color(reboot_msg, t.text, LV_PART_MAIN);
        lv_obj_set_style_bg_color(reboot_yes_btn, t.alert, LV_PART_MAIN);
        lv_obj_set_style_bg_color(reboot_no_btn, t.border, LV_PART_MAIN);
    }
    if (jack_modal) {
        lv_obj_set_style_bg_color(jack_modal, t.panel, LV_PART_MAIN);
        lv_obj_set_style_border_color(jack_modal, t.border, LV_PART_MAIN);
        lv_obj_set_style_bg_color(jack_modal_title, t.border, LV_PART_MAIN);
        lv_obj_set_style_text_color(jack_modal_title, t.bg, LV_PART_MAIN);
        lv_obj_set_style_text_color(jack_modal_msg, t.text, LV_PART_MAIN);
        lv_obj_set_style_bg_color(jack_modal_close, t.border, LV_PART_MAIN);
    }
    if (onboarding_modal) {
        lv_obj_set_style_bg_color(onboarding_modal, t.bg, LV_PART_MAIN);
        lv_obj_set_style_border_color(onboarding_modal, t.border, LV_PART_MAIN);
        lv_obj_set_style_bg_color(onboarding_title, t.border, LV_PART_MAIN);
        lv_obj_set_style_text_color(onboarding_title, t.bg, LV_PART_MAIN);
        lv_obj_set_style_text_color(onboarding_ssid_label, t.highlight, LV_PART_MAIN);
        lv_obj_set_style_bg_color(onboarding_ap_btn, t.border, LV_PART_MAIN);
        lv_obj_set_style_text_color(onboarding_ap_btn_label, t.bg, LV_PART_MAIN);
    }
}

// ─── Button / tile events ───
static void qr_btn_cb(lv_event_t* e) {
    if (qr_modal) { lv_obj_clear_flag(qr_modal, LV_OBJ_FLAG_HIDDEN); apply_theme(); }
}
static void qr_close_cb(lv_event_t* e) {
    if (qr_modal) lv_obj_add_flag(qr_modal, LV_OBJ_FLAG_HIDDEN);
}
static void reboot_btn_cb(lv_event_t* e) {
    if (reboot_modal) { lv_obj_clear_flag(reboot_modal, LV_OBJ_FLAG_HIDDEN); apply_theme(); }
}
static void reboot_no_cb(lv_event_t* e) {
    if (reboot_modal) lv_obj_add_flag(reboot_modal, LV_OBJ_FLAG_HIDDEN);
}
static void reboot_yes_cb(lv_event_t* e) {
    ESP_LOGI(TAG, "Hardware Reboot Triggered from Touch screen!");
    esp_restart();
}
static void onboarding_ap_cb(lv_event_t* e) {
    ESP_LOGI(TAG, "Standalone AP Mode triggered from Touch screen!");
    #if defined(ESP_PLATFORM)
    nvs_handle_t nvs_handle;
    if (nvs_open("storage", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, "wifi_mode", 2); // 2 = Standalone AP
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    esp_restart();
    #endif
}

static void jack_modal_close_cb(lv_event_t* e) {
    if (jack_modal) lv_obj_add_flag(jack_modal, LV_OBJ_FLAG_HIDDEN);
}

// Tapping a jack opens the detail popup. The tile index is carried in user_data.
static void jack_tile_cb(lv_event_t* e) {
    if (is_onboarding || !jack_modal) return;
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= UI_MAX_JACKS) return;
    JackTile& jt = jacks[idx];

    const char* state_str = "EMPTY";
    switch (jt.last_state) {
        case UI_JACK_IDLE:   state_str = "REGISTERED / IDLE"; break;
        case UI_JACK_INCALL: state_str = "IN CALL"; break;
        case UI_JACK_DND:    state_str = "DO NOT DISTURB"; break;
        default:             state_str = "EMPTY SLOT"; break;
    }
    char buf[96];
    if (jt.last_ext[0] != '\0') {
        snprintf(buf, sizeof(buf), "Extension: %s\nState: %s", jt.last_ext, state_str);
    } else {
        snprintf(buf, sizeof(buf), "Jack %02d\nState: %s", (int)idx + 1, state_str);
    }
    lv_label_set_text(jack_modal_msg, buf);
    lv_obj_clear_flag(jack_modal, LV_OBJ_FLAG_HIDDEN);
    apply_theme();
}

// ─── QR Code Generator Drawing (PRESERVED from original) ───
static void draw_qr(lv_obj_t* canvas, uint8_t* canvas_buffer, const char* text) {
    QRCode qrcode;
    uint8_t qrcodeData[qrcode_getBufferSize(4)];
    if (qrcode_initText(&qrcode, qrcodeData, 4, ECC_LOW, text) != 0) {
        ESP_LOGE(TAG, "Failed to generate QR Code");
        return;
    }
    int scale = 3; // 33 * 3 = 99 px
    lv_canvas_fill_bg(canvas, lv_color_make(255, 255, 255), LV_OPA_COVER);
    for (int y = 0; y < qrcode.size; y++) {
        for (int x = 0; x < qrcode.size; x++) {
            lv_color_t color = qrcode_getModule(&qrcode, x, y) ? lv_color_make(0, 0, 0) : lv_color_make(255, 255, 255);
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    lv_canvas_set_px(canvas, x * scale + sx, y * scale + sy, color);
                }
            }
        }
    }
}

// ─── Helper: build the jack board grid ───
static void build_jack_board() {
    // 3-column grid (flex row-wrap fits 3 x 92px tiles across 314px). Chunky finger targets.
    const int TILE_W = 92;
    const int TILE_H = 64;
    const int GAP = 6;

    jack_board = lv_obj_create(main_container);
    lv_obj_set_size(jack_board, 314, 262);
    lv_obj_align(jack_board, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_style_border_width(jack_board, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(jack_board, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(jack_board, GAP, LV_PART_MAIN);
    lv_obj_set_style_pad_row(jack_board, GAP, LV_PART_MAIN);
    lv_obj_set_style_pad_column(jack_board, GAP, LV_PART_MAIN);
    lv_obj_set_flex_flow(jack_board, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scroll_dir(jack_board, LV_DIR_VER);   // scroll vertically if >rows fit
    lv_obj_set_scrollbar_mode(jack_board, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < UI_MAX_JACKS; i++) {
        JackTile& jt = jacks[i];
        jt.tile = lv_obj_create(jack_board);
        lv_obj_set_size(jt.tile, TILE_W, TILE_H);
        lv_obj_set_style_radius(jt.tile, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_all(jt.tile, 4, LV_PART_MAIN);
        lv_obj_clear_flag(jt.tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(jt.tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(jt.tile, jack_tile_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        // Lamp: a small round indicator in the tile's top-right.
        jt.lamp = lv_obj_create(jt.tile);
        lv_obj_set_size(jt.lamp, 14, 14);
        lv_obj_set_style_radius(jt.lamp, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(jt.lamp, 0, LV_PART_MAIN);
        lv_obj_align(jt.lamp, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_clear_flag(jt.lamp, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(jt.lamp, LV_OBJ_FLAG_CLICKABLE);

        // Extension number label, bottom-left, big-ish for legibility.
        jt.label = lv_label_create(jt.tile);
        lv_obj_set_style_text_font(jt.label, UI_FONT, LV_PART_MAIN);
        lv_label_set_text(jt.label, "----");
        lv_obj_align(jt.label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        strcpy(jt.last_ext, "");
        jt.last_state = 0xFF;
        style_jack_tile(i, UI_JACK_EMPTY);
        jt.last_state = UI_JACK_EMPTY;
        // Start hidden: only jacks that are actually present (registered/in-call/DND)
        // are revealed by ui_update_board. No prepopulated empty boxes — the flex board
        // packs just the live jacks from the top-left.
        lv_obj_add_flag(jt.tile, LV_OBJ_FLAG_HIDDEN);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// UI init — build the switchboard.
// ─────────────────────────────────────────────────────────────────────────────
void ui_init(void) {
    ESP_LOGI(TAG, "Initializing LVGL switchboard interface (LVGL v8)...");

    main_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(main_container, 320, 480);
    lv_obj_set_style_pad_all(main_container, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(main_container, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(main_container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(main_container, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header (~40px): wordmark + online dot + clock + folded QR/REBOOT buttons ──
    header_bar = lv_obj_create(main_container);
    lv_obj_set_size(header_bar, 320, 40);
    lv_obj_align(header_bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_pad_all(header_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(header_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(header_bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_radius(header_bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header_bar, LV_OBJ_FLAG_SCROLLABLE);

    online_dot = lv_obj_create(header_bar);
    lv_obj_set_size(online_dot, 12, 12);
    lv_obj_set_style_radius(online_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(online_dot, 0, LV_PART_MAIN);
    lv_obj_align(online_dot, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_clear_flag(online_dot, LV_OBJ_FLAG_SCROLLABLE);

    title_label = lv_label_create(header_bar);
    lv_obj_set_style_text_font(title_label, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(title_label, "POCKET-DIAL");
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 26, 0);

    clock_label = lv_label_create(header_bar);
    lv_obj_set_style_text_font(clock_label, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(clock_label, "00:00:00");
    lv_obj_align(clock_label, LV_ALIGN_LEFT_MID, 132, 0);

    // Folded-in action buttons (kept reachable, original handlers/modals intact).
    hdr_rb_btn = lv_btn_create(header_bar);
    lv_obj_set_size(hdr_rb_btn, 40, 30);
    lv_obj_align(hdr_rb_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_radius(hdr_rb_btn, 3, LV_PART_MAIN);
    lv_obj_add_event_cb(hdr_rb_btn, reboot_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* rb_lbl = lv_label_create(hdr_rb_btn);
    lv_obj_set_style_text_font(rb_lbl, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(rb_lbl, LV_SYMBOL_POWER);
    lv_obj_center(rb_lbl);

    hdr_qr_btn = lv_btn_create(header_bar);
    lv_obj_set_size(hdr_qr_btn, 40, 30);
    lv_obj_align(hdr_qr_btn, LV_ALIGN_RIGHT_MID, -48, 0);
    lv_obj_set_style_radius(hdr_qr_btn, 3, LV_PART_MAIN);
    lv_obj_add_event_cb(hdr_qr_btn, qr_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* qr_lbl = lv_label_create(hdr_qr_btn);
    lv_obj_set_style_text_font(qr_lbl, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(qr_lbl, LV_SYMBOL_WIFI);
    lv_obj_center(qr_lbl);

    // ── Status strip (~36px): IP:port  EXT n/32  CALLS k ──
    status_strip = lv_obj_create(main_container);
    lv_obj_set_size(status_strip, 320, 36);
    lv_obj_align(status_strip, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_style_pad_all(status_strip, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(status_strip, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(status_strip, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_radius(status_strip, 0, LV_PART_MAIN);
    lv_obj_clear_flag(status_strip, LV_OBJ_FLAG_SCROLLABLE);

    status_label = lv_label_create(status_strip);
    lv_obj_set_style_text_font(status_label, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(status_label, "0.0.0.0:5060   EXT 0/32   CALLS 0");
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 8, 0);

    // ── Jack board (hero) ──
    build_jack_board();

    // ── Patch strip (~40px) ──
    patch_strip = lv_obj_create(main_container);
    lv_obj_set_size(patch_strip, 314, 38);
    lv_obj_align(patch_strip, LV_ALIGN_TOP_MID, 0, 344);
    lv_obj_set_style_border_width(patch_strip, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(patch_strip, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(patch_strip, 4, LV_PART_MAIN);
    lv_obj_clear_flag(patch_strip, LV_OBJ_FLAG_SCROLLABLE);

    patch_label = lv_label_create(patch_strip);
    lv_obj_set_style_text_font(patch_label, UI_FONT, LV_PART_MAIN);
    lv_label_set_long_mode(patch_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(patch_label, 304);
    lv_label_set_text(patch_label, "PATCH: (idle)");
    lv_obj_align(patch_label, LV_ALIGN_LEFT_MID, 2, 0);

    // ── Operator-log ticker (~60px) ──
    log_ticker = lv_obj_create(main_container);
    lv_obj_set_size(log_ticker, 314, 84);
    lv_obj_align(log_ticker, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_border_width(log_ticker, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(log_ticker, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(log_ticker, 4, LV_PART_MAIN);
    lv_obj_clear_flag(log_ticker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(log_ticker, UI_FONT, LV_PART_MAIN);
    // log_ticker is a styled lv_obj container; the actual text lives in a child label so the
    // ticker box repaints in isolation. Store the child on the container's user_data so
    // ui_add_log() can find it without another global.
    lv_obj_t* tlbl = lv_label_create(log_ticker);
    lv_obj_set_style_text_font(tlbl, UI_FONT, LV_PART_MAIN);
    lv_label_set_long_mode(tlbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(tlbl, 304);
    lv_label_set_text(tlbl, "OPERATOR LOG");
    lv_obj_align(tlbl, LV_ALIGN_TOP_LEFT, 0, 0);
    // Repurpose log_ticker pointer's text via its child: store child in a static.
    // (We keep log_ticker as the styled container; text writes target its child label.)
    lv_obj_set_user_data(log_ticker, tlbl);

    // ── QR Wi-Fi modal (PRESERVED) ──
    qr_modal = lv_obj_create(main_container);
    lv_obj_set_size(qr_modal, 260, 260);
    lv_obj_center(qr_modal);
    lv_obj_set_style_border_width(qr_modal, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(qr_modal, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(qr_modal, 10, LV_PART_MAIN);
    lv_obj_clear_flag(qr_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(qr_modal, LV_OBJ_FLAG_HIDDEN);

    #if defined(ESP_PLATFORM)
    qr_canvas_buffer = (uint8_t*)malloc(19602);
    #else
    static uint8_t static_qr_buf[19602];
    qr_canvas_buffer = static_qr_buf;
    #endif
    qr_canvas = lv_canvas_create(qr_modal);
    lv_canvas_set_buffer(qr_canvas, qr_canvas_buffer, 99, 99, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(qr_canvas, LV_ALIGN_TOP_MID, 0, 10);
    draw_qr(qr_canvas, qr_canvas_buffer, "WIFI:S:esp32-sipserver;T:nopass;;");

    qr_close_btn = lv_btn_create(qr_modal);
    lv_obj_set_size(qr_close_btn, 160, 36);
    lv_obj_align(qr_close_btn, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_radius(qr_close_btn, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(qr_close_btn, qr_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* close_label = lv_label_create(qr_close_btn);
    lv_obj_set_style_text_font(close_label, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(close_label, "CLOSE");
    lv_obj_center(close_label);

    // ── Reboot confirm modal (PRESERVED) ──
    reboot_modal = lv_obj_create(main_container);
    lv_obj_set_size(reboot_modal, 260, 180);
    lv_obj_center(reboot_modal);
    lv_obj_set_style_border_width(reboot_modal, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(reboot_modal, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(reboot_modal, 0, LV_PART_MAIN);
    lv_obj_clear_flag(reboot_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(reboot_modal, LV_OBJ_FLAG_HIDDEN);

    reboot_title = lv_label_create(reboot_modal);
    lv_obj_set_style_text_font(reboot_title, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(reboot_title, " WARNING: SYSTEM REBOOT");
    lv_obj_set_size(reboot_title, 256, 22);
    lv_obj_align(reboot_title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_top(reboot_title, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_left(reboot_title, 6, LV_PART_MAIN);

    reboot_msg = lv_label_create(reboot_modal);
    lv_obj_set_style_text_font(reboot_msg, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(reboot_msg, "Reboot the SIP PBX board?");
    lv_obj_align(reboot_msg, LV_ALIGN_CENTER, 0, -10);

    reboot_yes_btn = lv_btn_create(reboot_modal);
    lv_obj_set_size(reboot_yes_btn, 80, 36);
    lv_obj_align(reboot_yes_btn, LV_ALIGN_BOTTOM_LEFT, 20, -15);
    lv_obj_set_style_radius(reboot_yes_btn, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(reboot_yes_btn, reboot_yes_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* yes_lbl = lv_label_create(reboot_yes_btn);
    lv_obj_set_style_text_font(yes_lbl, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(yes_lbl, "YES");
    lv_obj_center(yes_lbl);

    reboot_no_btn = lv_btn_create(reboot_modal);
    lv_obj_set_size(reboot_no_btn, 80, 36);
    lv_obj_align(reboot_no_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -15);
    lv_obj_set_style_radius(reboot_no_btn, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(reboot_no_btn, reboot_no_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* no_lbl = lv_label_create(reboot_no_btn);
    lv_obj_set_style_text_font(no_lbl, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(no_lbl, "NO");
    lv_obj_center(no_lbl);

    // ── Jack detail popup ──
    jack_modal = lv_obj_create(main_container);
    lv_obj_set_size(jack_modal, 240, 150);
    lv_obj_center(jack_modal);
    lv_obj_set_style_border_width(jack_modal, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(jack_modal, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(jack_modal, 0, LV_PART_MAIN);
    lv_obj_clear_flag(jack_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(jack_modal, LV_OBJ_FLAG_HIDDEN);

    jack_modal_title = lv_label_create(jack_modal);
    lv_obj_set_style_text_font(jack_modal_title, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(jack_modal_title, " JACK DETAIL");
    lv_obj_set_size(jack_modal_title, 236, 22);
    lv_obj_align(jack_modal_title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_top(jack_modal_title, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_left(jack_modal_title, 6, LV_PART_MAIN);

    jack_modal_msg = lv_label_create(jack_modal);
    lv_obj_set_style_text_font(jack_modal_msg, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(jack_modal_msg, "Extension: ----\nState: EMPTY");
    lv_obj_align(jack_modal_msg, LV_ALIGN_CENTER, 0, -6);

    jack_modal_close = lv_btn_create(jack_modal);
    lv_obj_set_size(jack_modal_close, 100, 36);
    lv_obj_align(jack_modal_close, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_radius(jack_modal_close, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(jack_modal_close, jack_modal_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* jm_close_lbl = lv_label_create(jack_modal_close);
    lv_obj_set_style_text_font(jm_close_lbl, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(jm_close_lbl, "CLOSE");
    lv_obj_center(jm_close_lbl);

    apply_theme();
}

// ─── Onboarding mode (PRESERVED flow, restyled) ───
void ui_set_onboarding_mode(bool onboarding, const char* ssid, const char* pass) {
    is_onboarding = onboarding;
    if (!onboarding) {
        if (onboarding_modal) lv_obj_add_flag(onboarding_modal, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    ESP_LOGI(TAG, "Spinning up Onboarding UI View: SSID=%s", ssid);

    if (!onboarding_modal) {
        onboarding_modal = lv_obj_create(main_container);
        lv_obj_set_size(onboarding_modal, 320, 480);
        lv_obj_align(onboarding_modal, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_border_width(onboarding_modal, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(onboarding_modal, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(onboarding_modal, 10, LV_PART_MAIN);
        lv_obj_clear_flag(onboarding_modal, LV_OBJ_FLAG_SCROLLABLE);

        onboarding_title = lv_label_create(onboarding_modal);
        lv_obj_set_style_text_font(onboarding_title, UI_FONT, LV_PART_MAIN);
        lv_label_set_text(onboarding_title, "  AWAITING WI-FI CONFIG");
        lv_obj_set_size(onboarding_title, 300, 24);
        lv_obj_align(onboarding_title, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_set_style_pad_top(onboarding_title, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_left(onboarding_title, 14, LV_PART_MAIN);

        onboarding_ssid_label = lv_label_create(onboarding_modal);
        lv_obj_set_style_text_font(onboarding_ssid_label, UI_FONT, LV_PART_MAIN);
        lv_obj_align(onboarding_ssid_label, LV_ALIGN_TOP_MID, 0, 50);

        #if defined(ESP_PLATFORM)
        onboarding_qr_buffer = (uint8_t*)malloc(19602);
        #else
        static uint8_t static_onboard_qr_buf[19602];
        onboarding_qr_buffer = static_onboard_qr_buf;
        #endif
        onboarding_qr_canvas = lv_canvas_create(onboarding_modal);
        lv_canvas_set_buffer(onboarding_qr_canvas, onboarding_qr_buffer, 99, 99, LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(onboarding_qr_canvas, LV_ALIGN_CENTER, 0, -20);

        lv_obj_t* prompt_lbl = lv_label_create(onboarding_modal);
        lv_obj_set_style_text_font(prompt_lbl, UI_FONT, LV_PART_MAIN);
        lv_label_set_text(prompt_lbl, "Scan the QR code to join Wi-Fi,\nthen access: http://192.168.4.1/\nor choose AP mode below to run\nthe server standalone.");
        lv_obj_set_style_text_align(prompt_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(prompt_lbl, LV_ALIGN_BOTTOM_MID, 0, -110);

        onboarding_ap_btn = lv_btn_create(onboarding_modal);
        lv_obj_set_size(onboarding_ap_btn, 240, 40);
        lv_obj_align(onboarding_ap_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
        lv_obj_set_style_radius(onboarding_ap_btn, 4, LV_PART_MAIN);
        lv_obj_add_event_cb(onboarding_ap_btn, onboarding_ap_cb, LV_EVENT_CLICKED, NULL);
        onboarding_ap_btn_label = lv_label_create(onboarding_ap_btn);
        lv_obj_set_style_text_font(onboarding_ap_btn_label, UI_FONT, LV_PART_MAIN);
        lv_label_set_text(onboarding_ap_btn_label, "START STANDALONE AP MODE");
        lv_obj_center(onboarding_ap_btn_label);
    }

    lv_obj_clear_flag(onboarding_modal, LV_OBJ_FLAG_HIDDEN);

    char ssid_msg[128];
    snprintf(ssid_msg, sizeof(ssid_msg), "SSID: %s\nPass: %s", ssid, pass);
    lv_label_set_text(onboarding_ssid_label, ssid_msg);
    lv_obj_set_style_text_align(onboarding_ssid_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    char qr_str[128];
    if (strlen(pass) > 0) {
        snprintf(qr_str, sizeof(qr_str), "WIFI:S:%s;T:WPA;P:%s;;", ssid, pass);
    } else {
        snprintf(qr_str, sizeof(qr_str), "WIFI:S:%s;T:nopass;;", ssid);
    }
    draw_qr(onboarding_qr_canvas, onboarding_qr_buffer, qr_str);
    if (qr_canvas) draw_qr(qr_canvas, qr_canvas_buffer, qr_str);

    apply_theme();
}

// ─── Status strip + header clock ───
void ui_update_status(const std::string& ip, int uptimeSec, int stationNum, int clientCount, int sessionCount) {
    if (is_onboarding) return;

    char buf[96];
    // Status strip: IP:port  EXT n/32  CALLS k  (monospace-ish numerals via the label)
    snprintf(buf, sizeof(buf), "%s:5060   EXT %d/%d   CALLS %d",
             ip.c_str(), clientCount, UI_MAX_JACKS, sessionCount);
    if (status_label) lv_label_set_text(status_label, buf);

    // Header clock = uptime HH:MM:SS
    int hrs = uptimeSec / 3600;
    int mins = (uptimeSec % 3600) / 60;
    int secs = uptimeSec % 60;
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hrs, mins, secs);
    if (clock_label) lv_label_set_text(clock_label, buf);

    // Online dot reflects whether any station is associated to the AP.
    if (online_dot) {
        const LVColorTheme& t = PALETTES[current_theme];
        lv_obj_set_style_bg_color(online_dot, stationNum > 0 ? t.accent : t.panel, LV_PART_MAIN);
    }
}

// ─── Board update: jacks + patches ───
void ui_update_board(const UiBoardSnapshot& snap) {
    if (is_onboarding) return;

    int n = snap.jackCount;
    if (n > UI_MAX_JACKS) n = UI_MAX_JACKS;

    for (int i = 0; i < UI_MAX_JACKS; i++) {
        JackTile& jt = jacks[i];
        uint8_t   state = UI_JACK_EMPTY;
        const char* ext = "";
        if (i < n) {
            state = snap.jacks[i].state;
            ext   = snap.jacks[i].ext;
        }
        // Only touch the tile if something actually changed (keeps partial repaint minimal).
        bool ext_changed = strncmp(jt.last_ext, ext, sizeof(jt.last_ext)) != 0;
        if (ext_changed) {
            if (ext[0] != '\0') lv_label_set_text(jt.label, ext);
            else                lv_label_set_text(jt.label, "----");
            strncpy(jt.last_ext, ext, sizeof(jt.last_ext) - 1);
            jt.last_ext[sizeof(jt.last_ext) - 1] = '\0';
        }
        if (jt.last_state != state) {
            style_jack_tile(i, state);
            jt.last_state = state;
            // Reveal live jacks, hide empties — keeps the board to who's actually present.
            if (state == UI_JACK_EMPTY) lv_obj_add_flag(jt.tile, LV_OBJ_FLAG_HIDDEN);
            else                        lv_obj_clear_flag(jt.tile, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Patch strip: "A <-> B   A <-> B ..." bounded to UI_MAX_PATCHES.
    char line[256];
    line[0] = '\0';
    int np = snap.patchCount;
    if (np > UI_MAX_PATCHES) np = UI_MAX_PATCHES;
    if (np == 0) {
        snprintf(line, sizeof(line), "PATCH: (idle)");
    } else {
        size_t off = 0;
        off += snprintf(line + off, sizeof(line) - off, "PATCH: ");
        for (int i = 0; i < np && off < sizeof(line) - 1; i++) {
            off += snprintf(line + off, sizeof(line) - off, "%s%s <-> %s",
                            (i == 0 ? "" : "   "), snap.patches[i].a, snap.patches[i].b);
        }
    }
    if (patch_label) lv_label_set_text(patch_label, line);
}

// ─── Operator-log ticker (append-only ring, bounded) ───
void ui_add_log(const char* line) {
    if (!log_ticker || !line) return;

    strncpy(log_lines[log_head], line, sizeof(log_lines[0]) - 1);
    log_lines[log_head][sizeof(log_lines[0]) - 1] = '\0';
    log_head = (log_head + 1) % UI_LOG_LINES;
    if (log_count < UI_LOG_LINES) log_count++;

    // Rebuild the visible block oldest->newest into one label (single repaint of the box).
    char block[UI_LOG_LINES * 64];
    block[0] = '\0';
    size_t off = 0;
    int start = (log_head - log_count + UI_LOG_LINES) % UI_LOG_LINES;
    for (int i = 0; i < log_count && off < sizeof(block) - 1; i++) {
        int idx = (start + i) % UI_LOG_LINES;
        off += snprintf(block + off, sizeof(block) - off, "%s%s",
                        (i == 0 ? "" : "\n"), log_lines[idx]);
    }
    lv_obj_t* tlbl = (lv_obj_t*)lv_obj_get_user_data(log_ticker);
    if (tlbl) lv_label_set_text(tlbl, block);
}

// ─── Header aux indicator (kept for compatibility; folded into header) ───
void ui_set_battery(float volts, int percent) {
    // The switchboard header shows uptime/clock rather than battery; this is a no-op
    // retained so existing callers in esp_main_display.cpp keep linking. If a battery
    // sense is wired later, route it to a small header glyph here.
    (void)volts; (void)percent;
}

// ─── Direct touch coordinate router (modal fallbacks; PRESERVED behaviour) ───
void ui_handle_touch_press(int16_t x, int16_t y) {
    // Onboarding standalone-AP button fallback.
    if (is_onboarding) {
        if (x >= 40 && x <= 280 && y >= 405 && y <= 455) {
            lv_event_send(onboarding_ap_btn, LV_EVENT_CLICKED, NULL);
        }
        return;
    }

    // Reboot modal fallback (centered 260x180 -> buttons near vertical center).
    if (reboot_modal && !lv_obj_has_flag(reboot_modal, LV_OBJ_FLAG_HIDDEN)) {
        if (x >= 35 && x <= 135 && y >= 285 && y <= 330) {
            lv_event_send(reboot_yes_btn, LV_EVENT_CLICKED, NULL);
        } else if (x >= 155 && x <= 255 && y >= 285 && y <= 330) {
            lv_event_send(reboot_no_btn, LV_EVENT_CLICKED, NULL);
        }
        return;
    }

    // QR modal close fallback.
    if (qr_modal && !lv_obj_has_flag(qr_modal, LV_OBJ_FLAG_HIDDEN)) {
        if (x >= 80 && x <= 240 && y >= 320 && y <= 365) {
            lv_event_send(qr_close_btn, LV_EVENT_CLICKED, NULL);
        }
        return;
    }

    // Jack detail modal close fallback.
    if (jack_modal && !lv_obj_has_flag(jack_modal, LV_OBJ_FLAG_HIDDEN)) {
        if (x >= 110 && x <= 210 && y >= 290 && y <= 335) {
            lv_event_send(jack_modal_close, LV_EVENT_CLICKED, NULL);
        }
        return;
    }
    // Main board taps (header buttons + jack tiles) are routed by LVGL's own indev
    // hit-testing; no coordinate fallback needed here.
}
