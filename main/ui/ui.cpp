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

// ─── Multi-screen router state (Task 3A) ───
static lv_obj_t*  s_screens[SCREEN_COUNT] = {};
static ui_screen_t s_current_screen = SCREEN_SWITCHBOARD;
static lv_obj_t*  s_nav_panel        = nullptr;

// Telemetry 32-line ring (extends the 6-line switchboard ticker for the full diagnostics view)
#define TELE_LOG_LINES 32
static char s_tele_ring[TELE_LOG_LINES][80];
static int  s_tele_ring_head  = 0;
static int  s_tele_ring_count = 0;

// Cached widget pointers for live update functions
static lv_obj_t* s_topo_ip_lbl   = nullptr;
static lv_obj_t* s_topo_mode_lbl = nullptr;
static lv_obj_t* s_topo_cli_lbl  = nullptr;
static lv_obj_t* s_topo_log_lbl  = nullptr;

static lv_obj_t* s_tele_net_lbl      = nullptr;
static lv_obj_t* s_tele_pool_lbl     = nullptr;
static lv_obj_t* s_tele_sessions_lbl = nullptr;
static lv_obj_t* s_tele_log_lbl      = nullptr;
static lv_obj_t* s_perim_ssh_sw      = nullptr;

// Provisioning screen state
static int  s_prov_stage         = 0;
static char s_prov_password[64]  = {};
static char s_prov_pin_first[16] = {};
static bool s_prov_pin_confirming = false;
static lv_obj_t* s_prov_panels[4]  = {};
static lv_obj_t* s_prov_pass_ta    = nullptr;
static lv_obj_t* s_prov_passc_ta   = nullptr;
static lv_obj_t* s_prov_pin_lbl    = nullptr;
static lv_obj_t* s_prov_ext_ta     = nullptr;
static lv_obj_t* s_prov_progress   = nullptr;
static lv_obj_t* s_prov_kbd        = nullptr;

// Forward declarations — builders are defined after the public API implementations
static void _build_provisioning_screen(void);
static void _build_topology_screen(void);
static void _build_telemetry_screen(void);
static void _build_perimeter_screen(void);

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

    s_screens[SCREEN_SWITCHBOARD] = lv_scr_act();
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

    // ── Nav menu button (≡) — opens workspace selector panel ──
    lv_obj_t* hdr_menu_btn = lv_btn_create(header_bar);
    lv_obj_set_size(hdr_menu_btn, 40, 30);
    lv_obj_align(hdr_menu_btn, LV_ALIGN_RIGHT_MID, -92, 0);
    lv_obj_set_style_radius(hdr_menu_btn, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(hdr_menu_btn, PALETTES[current_theme].border, LV_PART_MAIN);
    lv_obj_add_event_cb(hdr_menu_btn, [](lv_event_t*) {
        if (!s_nav_panel) return;
        if (lv_obj_has_flag(s_nav_panel, LV_OBJ_FLAG_HIDDEN))
            lv_obj_clear_flag(s_nav_panel, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_nav_panel, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* menu_lbl = lv_label_create(hdr_menu_btn);
    lv_obj_set_style_text_font(menu_lbl, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(menu_lbl, LV_SYMBOL_LIST);
    lv_obj_center(menu_lbl);

    // ── Nav panel (workspace selector, hidden by default) ──
    const LVColorTheme& tp = PALETTES[current_theme];
    s_nav_panel = lv_obj_create(main_container);
    lv_obj_set_size(s_nav_panel, 160, 160);
    lv_obj_align(s_nav_panel, LV_ALIGN_TOP_RIGHT, -4, 44);
    lv_obj_set_style_bg_color(s_nav_panel, tp.panel, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_nav_panel, tp.border, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_nav_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_nav_panel, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_nav_panel, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_nav_panel, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_nav_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_nav_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_nav_panel, LV_OBJ_FLAG_SCROLLABLE);
    // Nav entries
    static const struct { const char* label; ui_screen_t target; } nav_items[] = {
        { "TOPOLOGY",  SCREEN_TOPOLOGY  },
        { "TELEMETRY", SCREEN_TELEMETRY },
        { "PERIMETER", SCREEN_PERIMETER },
    };
    for (int ni = 0; ni < 3; ni++) {
        lv_obj_t* nb = lv_btn_create(s_nav_panel);
        lv_obj_set_size(nb, 144, 38);
        lv_obj_set_style_bg_color(nb, tp.border, LV_PART_MAIN);
        lv_obj_set_style_radius(nb, 4, LV_PART_MAIN);
        lv_obj_add_event_cb(nb, [](lv_event_t* e) {
            ui_screen_t t = (ui_screen_t)(intptr_t)lv_event_get_user_data(e);
            ui_navigate_to(t);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)nav_items[ni].target);
        lv_obj_t* nl = lv_label_create(nb);
        lv_obj_set_style_text_font(nl, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(nl, tp.bg, LV_PART_MAIN);
        lv_label_set_text(nl, nav_items[ni].label);
        lv_obj_center(nl);
    }

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

// ─── Onboarding / Provisioning mode ───
// When onboarding=true: navigate to the multi-stage provisioning screen.
// When onboarding=false: return to the main switchboard.
// The legacy SSID/pass parameters are kept for source compatibility but are unused when
// the provisioning screen is active (the screen reads NVS directly).
void ui_set_onboarding_mode(bool onboarding, const char* ssid, const char* pass) {
    is_onboarding = onboarding;
    if (!onboarding) {
        if (onboarding_modal) lv_obj_add_flag(onboarding_modal, LV_OBJ_FLAG_HIDDEN);
        ui_navigate_to(SCREEN_SWITCHBOARD);
        return;
    }

    // Route to the new provisioning screen; fall through to the legacy modal path
    // only if the provisioning screen failed to build (belt-and-suspenders).
    ui_navigate_to(SCREEN_PROVISIONING);
    if (s_screens[SCREEN_PROVISIONING] != nullptr) return;
    // ── Legacy captive-portal modal (fallback) ──

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
    if (!line) return;

    // Feed the telemetry 32-line ring (always, regardless of screen)
    strncpy(s_tele_ring[s_tele_ring_head], line, sizeof(s_tele_ring[0]) - 1);
    s_tele_ring[s_tele_ring_head][sizeof(s_tele_ring[0]) - 1] = '\0';
    s_tele_ring_head = (s_tele_ring_head + 1) % TELE_LOG_LINES;
    if (s_tele_ring_count < TELE_LOG_LINES) s_tele_ring_count++;

    if (!log_ticker) return;

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

// ─────────────────────────────────────────────────────────────────────────────
// Multi-screen router public API (Task 3A)
// ─────────────────────────────────────────────────────────────────────────────

ui_screen_t ui_current_screen(void) { return s_current_screen; }

void ui_navigate_to(ui_screen_t screen) {
    if (s_nav_panel) lv_obj_add_flag(s_nav_panel, LV_OBJ_FLAG_HIDDEN);
    if (screen == s_current_screen && s_screens[screen] != nullptr) return;

    if (s_screens[screen] == nullptr) {
        switch (screen) {
            case SCREEN_PROVISIONING: _build_provisioning_screen(); break;
            case SCREEN_TOPOLOGY:     _build_topology_screen();     break;
            case SCREEN_TELEMETRY:    _build_telemetry_screen();    break;
            case SCREEN_PERIMETER:    _build_perimeter_screen();    break;
            default: break;
        }
    }
    if (s_screens[screen] == nullptr) return;

    lv_scr_load_anim_t anim = (screen > s_current_screen)
        ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT;
    lv_scr_load_anim(s_screens[screen], anim, 250, 0, false);
    s_current_screen = screen;
}

// ─── Screen helper: standard title bar with back button ───
static lv_obj_t* _title_bar(lv_obj_t* parent, const char* title, ui_screen_t back_to) {
    const LVColorTheme& t = PALETTES[current_theme];
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 320, 44);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, t.panel, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, t.border, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_btn_create(bar);
    lv_obj_set_size(back, 56, 32);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(back, t.border, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(back, [](lv_event_t* e) {
        ui_screen_t tgt = (ui_screen_t)(intptr_t)lv_event_get_user_data(e);
        ui_navigate_to(tgt);
    }, LV_EVENT_CLICKED, (void*)(intptr_t)back_to);
    lv_obj_t* bl = lv_label_create(back);
    lv_obj_set_style_text_font(bl, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(bl, t.bg, LV_PART_MAIN);
    lv_label_set_text(bl, "< BACK");
    lv_obj_center(bl);

    lv_obj_t* lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(lbl, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, t.highlight, LV_PART_MAIN);
    lv_label_set_text(lbl, title);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 70, 0);
    return bar;
}

// ─── Screen helper: content container below the title bar ───
static lv_obj_t* _content_area(lv_obj_t* parent, int top_offset) {
    const LVColorTheme& t = PALETTES[current_theme];
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, 320, 480 - top_offset);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, 0, top_offset);
    lv_obj_set_style_bg_color(c, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(c, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_AUTO);
    return c;
}

// ─── Screen helper: wide info panel with label+value ───
static lv_obj_t* _info_panel(lv_obj_t* parent, const char* heading) {
    const LVColorTheme& t = PALETTES[current_theme];
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_set_width(p, 300);
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(p, t.panel, LV_PART_MAIN);
    lv_obj_set_style_border_color(p, t.border, LV_PART_MAIN);
    lv_obj_set_style_border_width(p, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(p, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p, 6, LV_PART_MAIN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* h = lv_label_create(p);
    lv_obj_set_style_text_font(h, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(h, t.highlight, LV_PART_MAIN);
    lv_label_set_text(h, heading);
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 0, 0);
    return p;
}

// ─── Screen helper: add a value label inside an info panel ───
static lv_obj_t* _panel_value(lv_obj_t* panel, int y_offset) {
    const LVColorTheme& t = PALETTES[current_theme];
    lv_obj_t* v = lv_label_create(panel);
    lv_obj_set_style_text_font(v, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(v, t.text, LV_PART_MAIN);
    lv_obj_set_width(v, 284);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_label_set_text(v, "—");
    lv_obj_align(v, LV_ALIGN_TOP_LEFT, 0, y_offset);
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// TASK 3B — Provisioning & Security Gate Flow
// ─────────────────────────────────────────────────────────────────────────────

static void _prov_show_stage(int stage) {
    s_prov_stage = stage;
    for (int i = 0; i < 4; i++) {
        if (!s_prov_panels[i]) continue;
        if (i == stage) lv_obj_clear_flag(s_prov_panels[i], LV_OBJ_FLAG_HIDDEN);
        else            lv_obj_add_flag(s_prov_panels[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_prov_progress) lv_bar_set_value(s_prov_progress, stage + 1, LV_ANIM_OFF);
    // Stage 1 uses a custom PIN pad — hide the shared LVGL keyboard there
    if (s_prov_kbd) {
        if (stage == 1) lv_obj_add_flag(s_prov_kbd, LV_OBJ_FLAG_HIDDEN);
        else            lv_obj_clear_flag(s_prov_kbd, LV_OBJ_FLAG_HIDDEN);
    }
}

static void _prov_commit_and_reboot(uint8_t wifi_mode) {
#if defined(ESP_PLATFORM)
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
        // Write credentials (simplified — credentialIsSet() checks non-empty admin_hash)
        nvs_set_str(h, "admin_hash", s_prov_password[0] ? s_prov_password : "pocketdial");
        nvs_set_str(h, "admin_pin",  s_prov_pin_first[0] ? s_prov_pin_first : "0000");
        nvs_set_u8(h,  "wifi_mode",  wifi_mode);
        nvs_set_u8(h,  "provisioned", 1);
        nvs_commit(h);
        nvs_close(h);
    }
    if (nvs_open("pbxcfg", NVS_READWRITE, &h) == ESP_OK) {
        // admin_ext written by Stage 3; we just commit here
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI("prov", "Provisioning complete — rebooting into mode %d", (int)wifi_mode);
    esp_restart();
#endif
}

static void _build_provisioning_screen(void) {
    const LVColorTheme& t = PALETTES[current_theme];
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    s_screens[SCREEN_PROVISIONING] = scr;

    // Title bar (no back button — cannot skip provisioning)
    lv_obj_t* bar = lv_obj_create(scr);
    lv_obj_set_size(bar, 320, 44);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, t.panel, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, t.border, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* bar_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(bar_lbl, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar_lbl, t.highlight, LV_PART_MAIN);
    lv_label_set_text(bar_lbl, "POCKET-DIAL  SETUP");
    lv_obj_align(bar_lbl, LV_ALIGN_LEFT_MID, 10, 0);

    // Progress bar
    s_prov_progress = lv_bar_create(scr);
    lv_obj_set_size(s_prov_progress, 280, 6);
    lv_obj_align(s_prov_progress, LV_ALIGN_TOP_MID, 0, 48);
    lv_bar_set_range(s_prov_progress, 1, 4);
    lv_bar_set_value(s_prov_progress, 1, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_prov_progress, t.panel, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_prov_progress, t.accent, LV_PART_INDICATOR);

    int panel_y = 60;
    int panel_h = 420 - panel_y;

    // ── Stage 0: Admin password ──
    lv_obj_t* p0 = lv_obj_create(scr);
    lv_obj_set_size(p0, 316, panel_h);
    lv_obj_align(p0, LV_ALIGN_TOP_MID, 0, panel_y);
    lv_obj_set_style_bg_color(p0, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_width(p0, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p0, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(p0, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(p0, LV_FLEX_FLOW_COLUMN);
    s_prov_panels[0] = p0;
    {
        lv_obj_t* h = lv_label_create(p0);
        lv_obj_set_style_text_font(h, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, t.highlight, LV_PART_MAIN);
        lv_label_set_text(h, "STEP 1/4 — SECURE YOUR DEVICE");

        lv_obj_t* l1 = lv_label_create(p0);
        lv_obj_set_style_text_font(l1, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(l1, t.text, LV_PART_MAIN);
        lv_label_set_text(l1, "Admin password:");

        s_prov_pass_ta = lv_textarea_create(p0);
        lv_obj_set_size(s_prov_pass_ta, 290, 38);
        lv_textarea_set_password_mode(s_prov_pass_ta, true);
        lv_textarea_set_one_line(s_prov_pass_ta, true);
        lv_textarea_set_placeholder_text(s_prov_pass_ta, "Enter password...");
        lv_obj_set_style_text_font(s_prov_pass_ta, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_prov_pass_ta, t.panel, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_prov_pass_ta, t.text, LV_PART_MAIN);

        lv_obj_t* l2 = lv_label_create(p0);
        lv_obj_set_style_text_font(l2, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(l2, t.text, LV_PART_MAIN);
        lv_label_set_text(l2, "Confirm password:");

        s_prov_passc_ta = lv_textarea_create(p0);
        lv_obj_set_size(s_prov_passc_ta, 290, 38);
        lv_textarea_set_password_mode(s_prov_passc_ta, true);
        lv_textarea_set_one_line(s_prov_passc_ta, true);
        lv_textarea_set_placeholder_text(s_prov_passc_ta, "Confirm password...");
        lv_obj_set_style_text_font(s_prov_passc_ta, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_prov_passc_ta, t.panel, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_prov_passc_ta, t.text, LV_PART_MAIN);

        lv_obj_t* err = lv_label_create(p0);
        lv_obj_set_style_text_font(err, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(err, t.alert, LV_PART_MAIN);
        lv_label_set_text(err, "");

        lv_obj_t* btn = lv_btn_create(p0);
        lv_obj_set_size(btn, 290, 42);
        lv_obj_set_style_bg_color(btn, t.border, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
        // Pass err label pointer via user_data for the callback
        lv_obj_set_user_data(btn, err);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            lv_obj_t* errl = (lv_obj_t*)lv_event_get_user_data(e);
            const char* pw  = lv_textarea_get_text(s_prov_pass_ta);
            const char* cpw = lv_textarea_get_text(s_prov_passc_ta);
            if (!pw || strlen(pw) < 4) {
                if (errl) lv_label_set_text(errl, "Password too short (min 4 chars)");
                return;
            }
            if (strcmp(pw, cpw) != 0) {
                if (errl) lv_label_set_text(errl, "Passwords do not match");
                return;
            }
            strncpy(s_prov_password, pw, sizeof(s_prov_password) - 1);
            _prov_show_stage(1);
        }, LV_EVENT_CLICKED, err);
        lv_obj_t* bl = lv_label_create(btn);
        lv_obj_set_style_text_font(bl, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(bl, t.bg, LV_PART_MAIN);
        lv_label_set_text(bl, "SET PASSWORD  >");
        lv_obj_center(bl);

        // On-screen keyboard (shared across stages)
        s_prov_kbd = lv_keyboard_create(scr);
        lv_keyboard_set_mode(s_prov_kbd, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_keyboard_set_textarea(s_prov_kbd, s_prov_pass_ta);
        lv_obj_set_size(s_prov_kbd, 320, 180);
        lv_obj_align(s_prov_kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(s_prov_kbd, t.panel, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_prov_kbd, t.border, LV_PART_MAIN);

        // Focus routing: tap on confirm field switches keyboard target
        lv_obj_add_event_cb(s_prov_passc_ta, [](lv_event_t*) {
            if (s_prov_kbd) lv_keyboard_set_textarea(s_prov_kbd, s_prov_passc_ta);
        }, LV_EVENT_FOCUSED, nullptr);
        lv_obj_add_event_cb(s_prov_pass_ta, [](lv_event_t*) {
            if (s_prov_kbd) lv_keyboard_set_textarea(s_prov_kbd, s_prov_pass_ta);
        }, LV_EVENT_FOCUSED, nullptr);
    }

    // ── Stage 1: Admin PIN ──
    lv_obj_t* p1 = lv_obj_create(scr);
    lv_obj_set_size(p1, 316, panel_h);
    lv_obj_align(p1, LV_ALIGN_TOP_MID, 0, panel_y);
    lv_obj_set_style_bg_color(p1, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_width(p1, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p1, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(p1, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(p1, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(p1, LV_OBJ_FLAG_HIDDEN);
    s_prov_panels[1] = p1;
    {
        lv_obj_t* h = lv_label_create(p1);
        lv_obj_set_style_text_font(h, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, t.highlight, LV_PART_MAIN);
        lv_label_set_text(h, "STEP 2/4 — SET ADMIN PIN");

        lv_obj_t* sub = lv_label_create(p1);
        lv_obj_set_style_text_font(sub, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(sub, t.text, LV_PART_MAIN);
        lv_label_set_text(sub, "Enter a 4–6 digit PIN for device tuning:");

        s_prov_pin_lbl = lv_label_create(p1);
        lv_obj_set_style_text_font(s_prov_pin_lbl, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_prov_pin_lbl, t.accent, LV_PART_MAIN);
        lv_label_set_text(s_prov_pin_lbl, "● ● ● ●");

        // Numeric PIN pad (3x4 grid)
        static char pin_entry[16] = {};
        static int  pin_len = 0;
        lv_obj_t* grid = lv_obj_create(p1);
        lv_obj_set_size(grid, 200, 200);
        lv_obj_set_style_bg_color(grid, t.bg, LV_PART_MAIN);
        lv_obj_set_style_border_width(grid, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(grid, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_row(grid, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_column(grid, 4, LV_PART_MAIN);
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
        static const char* keys[] = { "1","2","3","4","5","6","7","8","9","<","0","OK" };
        for (int ki = 0; ki < 12; ki++) {
            lv_obj_t* kb = lv_btn_create(grid);
            lv_obj_set_size(kb, 56, 42);
            lv_obj_set_style_bg_color(kb, t.border, LV_PART_MAIN);
            lv_obj_set_style_radius(kb, 4, LV_PART_MAIN);
            lv_obj_set_style_bg_color(kb, ki==11 ? t.accent : t.border, LV_PART_MAIN);
            lv_obj_add_event_cb(kb, [](lv_event_t* e) {
                const char* k = (const char*)lv_event_get_user_data(e);
                if (!k) return;
                if (strcmp(k, "<") == 0) {
                    if (pin_len > 0) pin_entry[--pin_len] = '\0';
                } else if (strcmp(k, "OK") == 0) {
                    if (pin_len < 4) return;
                    if (!s_prov_pin_confirming) {
                        strncpy(s_prov_pin_first, pin_entry, sizeof(s_prov_pin_first)-1);
                        s_prov_pin_confirming = true;
                        pin_len = 0; pin_entry[0] = '\0';
                        if (s_prov_pin_lbl) lv_label_set_text(s_prov_pin_lbl, "Confirm PIN:");
                        return;
                    }
                    if (strcmp(pin_entry, s_prov_pin_first) != 0) {
                        s_prov_pin_confirming = false;
                        pin_len = 0; pin_entry[0] = '\0';
                        if (s_prov_pin_lbl) lv_label_set_text(s_prov_pin_lbl, "Mismatch — re-enter PIN:");
                        return;
                    }
                    _prov_show_stage(2);
                    return;
                } else {
                    if (pin_len < 6) { pin_entry[pin_len++] = k[0]; pin_entry[pin_len] = '\0'; }
                }
                // Rebuild dot display
                if (s_prov_pin_lbl) {
                    char dots[32] = {};
                    for (int d = 0; d < pin_len; d++) {
                        strcat(dots, d > 0 ? " ●" : "●");
                    }
                    lv_label_set_text(s_prov_pin_lbl, dots[0] ? dots : "● ● ● ●");
                }
            }, LV_EVENT_CLICKED, (void*)keys[ki]);
            lv_obj_t* kl = lv_label_create(kb);
            lv_obj_set_style_text_font(kl, UI_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(kl, ki==11 ? t.bg : t.bg, LV_PART_MAIN);
            lv_label_set_text(kl, keys[ki]);
            lv_obj_center(kl);
        }
        lv_obj_t* back1 = lv_btn_create(p1);
        lv_obj_set_size(back1, 100, 36);
        lv_obj_set_style_bg_color(back1, t.panel, LV_PART_MAIN);
        lv_obj_set_style_radius(back1, 4, LV_PART_MAIN);
        lv_obj_add_event_cb(back1, [](lv_event_t*){ _prov_show_stage(0); }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* bl1 = lv_label_create(back1);
        lv_obj_set_style_text_font(bl1, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(bl1, t.text, LV_PART_MAIN);
        lv_label_set_text(bl1, "< BACK");
        lv_obj_center(bl1);
    }

    // ── Stage 2: Admin extension ──
    lv_obj_t* p2 = lv_obj_create(scr);
    lv_obj_set_size(p2, 316, panel_h);
    lv_obj_align(p2, LV_ALIGN_TOP_MID, 0, panel_y);
    lv_obj_set_style_bg_color(p2, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_width(p2, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p2, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(p2, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(p2, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(p2, LV_OBJ_FLAG_HIDDEN);
    s_prov_panels[2] = p2;
    {
        lv_obj_t* h = lv_label_create(p2);
        lv_obj_set_style_text_font(h, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, t.highlight, LV_PART_MAIN);
        lv_label_set_text(h, "STEP 3/4 — ADMIN EXTENSION");

        lv_obj_t* sub = lv_label_create(p2);
        lv_obj_set_style_text_font(sub, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(sub, t.text, LV_PART_MAIN);
        lv_label_set_text(sub, "Extension number for the admin handset\n(default: 101). Only this extension\ncan access system tuning codes.");

        s_prov_ext_ta = lv_textarea_create(p2);
        lv_obj_set_size(s_prov_ext_ta, 290, 38);
        lv_textarea_set_one_line(s_prov_ext_ta, true);
        lv_textarea_set_text(s_prov_ext_ta, "101");
        lv_obj_set_style_text_font(s_prov_ext_ta, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_prov_ext_ta, t.panel, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_prov_ext_ta, t.text, LV_PART_MAIN);
        if (s_prov_kbd) lv_keyboard_set_textarea(s_prov_kbd, s_prov_ext_ta);
        lv_obj_add_event_cb(s_prov_ext_ta, [](lv_event_t*){
            if (s_prov_kbd) {
                lv_keyboard_set_mode(s_prov_kbd, LV_KEYBOARD_MODE_NUMBER);
                lv_keyboard_set_textarea(s_prov_kbd, s_prov_ext_ta);
            }
        }, LV_EVENT_FOCUSED, nullptr);

        lv_obj_t* btn2 = lv_btn_create(p2);
        lv_obj_set_size(btn2, 290, 42);
        lv_obj_set_style_bg_color(btn2, t.border, LV_PART_MAIN);
        lv_obj_set_style_radius(btn2, 4, LV_PART_MAIN);
        lv_obj_add_event_cb(btn2, [](lv_event_t*) {
            const char* ext = lv_textarea_get_text(s_prov_ext_ta);
            if (!ext || strlen(ext) < 1) ext = "101";
#if defined(ESP_PLATFORM)
            nvs_handle_t h;
            if (nvs_open("pbxcfg", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_str(h, "admin_ext", ext);
                nvs_commit(h);
                nvs_close(h);
            }
#endif
            _prov_show_stage(3);
        }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* b2l = lv_label_create(btn2);
        lv_obj_set_style_text_font(b2l, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(b2l, t.bg, LV_PART_MAIN);
        lv_label_set_text(b2l, "CONFIRM EXTENSION  >");
        lv_obj_center(b2l);

        lv_obj_t* back2 = lv_btn_create(p2);
        lv_obj_set_size(back2, 100, 36);
        lv_obj_set_style_bg_color(back2, t.panel, LV_PART_MAIN);
        lv_obj_set_style_radius(back2, 4, LV_PART_MAIN);
        lv_obj_add_event_cb(back2, [](lv_event_t*){ _prov_show_stage(1); }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* b2bl = lv_label_create(back2);
        lv_obj_set_style_text_font(b2bl, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(b2bl, t.text, LV_PART_MAIN);
        lv_label_set_text(b2bl, "< BACK");
        lv_obj_center(b2bl);
    }

    // ── Stage 3: Topology selection ──
    lv_obj_t* p3 = lv_obj_create(scr);
    lv_obj_set_size(p3, 316, panel_h);
    lv_obj_align(p3, LV_ALIGN_TOP_MID, 0, panel_y);
    lv_obj_set_style_bg_color(p3, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_width(p3, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p3, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(p3, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(p3, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(p3, LV_OBJ_FLAG_HIDDEN);
    s_prov_panels[3] = p3;
    {
        lv_obj_t* h = lv_label_create(p3);
        lv_obj_set_style_text_font(h, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(h, t.highlight, LV_PART_MAIN);
        lv_label_set_text(h, "STEP 4/4 — NETWORK MODE");

        lv_obj_t* sub = lv_label_create(p3);
        lv_obj_set_style_text_font(sub, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(sub, t.text, LV_PART_MAIN);
        lv_label_set_text(sub, "Select how this device connects to\nyour network. Device will reboot.");

        // CLIENT button
        lv_obj_t* cb = lv_btn_create(p3);
        lv_obj_set_size(cb, 290, 70);
        lv_obj_set_style_bg_color(cb, t.panel, LV_PART_MAIN);
        lv_obj_set_style_border_color(cb, t.border, LV_PART_MAIN);
        lv_obj_set_style_border_width(cb, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(cb, 6, LV_PART_MAIN);
        lv_obj_add_event_cb(cb, [](lv_event_t*){ _prov_commit_and_reboot(1); }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* ct = lv_label_create(cb);
        lv_obj_set_style_text_font(ct, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(ct, t.highlight, LV_PART_MAIN);
        lv_label_set_text(ct, "CLIENT MODE\nJoin an existing Wi-Fi network");
        lv_obj_align(ct, LV_ALIGN_LEFT_MID, 8, 0);

        // INFRA button
        lv_obj_t* ib = lv_btn_create(p3);
        lv_obj_set_size(ib, 290, 70);
        lv_obj_set_style_bg_color(ib, t.panel, LV_PART_MAIN);
        lv_obj_set_style_border_color(ib, t.border, LV_PART_MAIN);
        lv_obj_set_style_border_width(ib, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(ib, 6, LV_PART_MAIN);
        lv_obj_add_event_cb(ib, [](lv_event_t*){ _prov_commit_and_reboot(2); }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* it = lv_label_create(ib);
        lv_obj_set_style_text_font(it, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(it, t.highlight, LV_PART_MAIN);
        lv_label_set_text(it, "INFRASTRUCTURE MODE\nAct as network hub for IP phones");
        lv_obj_align(it, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t* back3 = lv_btn_create(p3);
        lv_obj_set_size(back3, 100, 36);
        lv_obj_set_style_bg_color(back3, t.panel, LV_PART_MAIN);
        lv_obj_set_style_radius(back3, 4, LV_PART_MAIN);
        lv_obj_add_event_cb(back3, [](lv_event_t*){ _prov_show_stage(2); }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* b3l = lv_label_create(back3);
        lv_obj_set_style_text_font(b3l, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(b3l, t.text, LV_PART_MAIN);
        lv_label_set_text(b3l, "< BACK");
        lv_obj_center(b3l);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TASK 3C — Topology Configuration Workspace
// ─────────────────────────────────────────────────────────────────────────────

static void _build_topology_screen(void) {
    const LVColorTheme& t = PALETTES[current_theme];
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    s_screens[SCREEN_TOPOLOGY] = scr;

    _title_bar(scr, "TOPOLOGY", SCREEN_SWITCHBOARD);
    lv_obj_t* content = _content_area(scr, 46);

    // Mode panel
    lv_obj_t* mp = _info_panel(content, "NETWORK MODE");
    s_topo_mode_lbl = _panel_value(mp, 18);
    lv_obj_set_size(mp, 300, 54);
    lv_obj_clear_flag(mp, LV_OBJ_FLAG_SCROLLABLE);

    // IP panel
    lv_obj_t* ip = _info_panel(content, "IP ADDRESS");
    s_topo_ip_lbl = _panel_value(ip, 18);
    lv_obj_set_size(ip, 300, 54);
    lv_obj_clear_flag(ip, LV_OBJ_FLAG_SCROLLABLE);

    // Connected clients panel
    lv_obj_t* cp = _info_panel(content, "CONNECTED CLIENTS");
    s_topo_cli_lbl = _panel_value(cp, 18);
    lv_obj_set_size(cp, 300, 54);
    lv_obj_clear_flag(cp, LV_OBJ_FLAG_SCROLLABLE);

    // Switch topology button
    lv_obj_t* sw_btn = lv_btn_create(content);
    lv_obj_set_size(sw_btn, 290, 44);
    lv_obj_set_style_bg_color(sw_btn, t.border, LV_PART_MAIN);
    lv_obj_set_style_radius(sw_btn, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(sw_btn, [](lv_event_t* e) {
        // Show confirmation dialog
        lv_obj_t* dlg = (lv_obj_t*)lv_event_get_user_data(e);
        if (dlg) lv_obj_clear_flag(dlg, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, nullptr);  // user_data set below

    lv_obj_t* swl = lv_label_create(sw_btn);
    lv_obj_set_style_text_font(swl, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(swl, t.bg, LV_PART_MAIN);
    lv_label_set_text(swl, "SWITCH TOPOLOGY");
    lv_obj_center(swl);

    // Confirmation dialog
    lv_obj_t* dlg = lv_obj_create(scr);
    lv_obj_set_size(dlg, 280, 150);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, t.panel, LV_PART_MAIN);
    lv_obj_set_style_border_color(dlg, t.border, LV_PART_MAIN);
    lv_obj_set_style_border_width(dlg, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(dlg, 6, LV_PART_MAIN);
    lv_obj_add_flag(dlg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* dlg_lbl = lv_label_create(dlg);
    lv_obj_set_style_text_font(dlg_lbl, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(dlg_lbl, t.text, LV_PART_MAIN);
    lv_label_set_text(dlg_lbl, "Switch topology requires\na full reboot. Continue?");
    lv_obj_align(dlg_lbl, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t* yes = lv_btn_create(dlg);
    lv_obj_set_size(yes, 90, 36);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_LEFT, 12, -10);
    lv_obj_set_style_bg_color(yes, t.alert, LV_PART_MAIN);
    lv_obj_set_style_radius(yes, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(yes, [](lv_event_t*) {
#if defined(ESP_PLATFORM)
        nvs_handle_t h;
        uint8_t cur = 2;
        if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
            nvs_get_u8(h, "wifi_mode", &cur);
            nvs_set_u8(h, "wifi_mode", cur == 1 ? 2 : 1);
            nvs_commit(h);
            nvs_close(h);
        }
        esp_restart();
#endif
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* yl = lv_label_create(yes);
    lv_obj_set_style_text_font(yl, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(yl, t.bg, LV_PART_MAIN);
    lv_label_set_text(yl, "YES");
    lv_obj_center(yl);

    lv_obj_t* no = lv_btn_create(dlg);
    lv_obj_set_size(no, 90, 36);
    lv_obj_align(no, LV_ALIGN_BOTTOM_RIGHT, -12, -10);
    lv_obj_set_style_bg_color(no, t.border, LV_PART_MAIN);
    lv_obj_set_style_radius(no, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(no, [](lv_event_t* e) {
        lv_obj_t* d = (lv_obj_t*)lv_event_get_user_data(e);
        if (d) lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, dlg);
    lv_obj_t* nl = lv_label_create(no);
    lv_obj_set_style_text_font(nl, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(nl, t.bg, LV_PART_MAIN);
    lv_label_set_text(nl, "NO");
    lv_obj_center(nl);

    // Wire dialog to switch button
    lv_obj_set_user_data(sw_btn, dlg);

    // Diagnostic log strip (last 4 lines)
    lv_obj_t* lp = _info_panel(content, "BOOT / NET LOG");
    s_topo_log_lbl = _panel_value(lp, 18);
    lv_obj_set_height(lp, 80);
    lv_label_set_long_mode(s_topo_log_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(s_topo_log_lbl, 284, 56);
}

void ui_update_topology(const char* ip, uint8_t mode, int clients) {
    if (s_topo_ip_lbl)   lv_label_set_text(s_topo_ip_lbl, ip ? ip : "—");
    if (s_topo_mode_lbl) lv_label_set_text(s_topo_mode_lbl,
        mode == 1 ? "CLIENT (Wi-Fi STA)" : mode == 2 ? "INFRASTRUCTURE (AP+DHCP)" : "CAPTIVE PORTAL");
    if (s_topo_cli_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", clients);
        lv_label_set_text(s_topo_cli_lbl, buf);
    }
    // Refresh log strip from tele ring (last 4 lines)
    if (s_topo_log_lbl) {
        char block[4 * 80 + 4] = {};
        int n = s_tele_ring_count < 4 ? s_tele_ring_count : 4;
        int start = (s_tele_ring_head - n + TELE_LOG_LINES) % TELE_LOG_LINES;
        for (int i = 0; i < n; i++) {
            if (i > 0) strcat(block, "\n");
            strncat(block, s_tele_ring[(start + i) % TELE_LOG_LINES], 79);
        }
        lv_label_set_text(s_topo_log_lbl, block[0] ? block : "(no log entries)");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TASK 3D — Telemetry & Deep Diagnostics View
// ─────────────────────────────────────────────────────────────────────────────

static void _build_telemetry_screen(void) {
    const LVColorTheme& t = PALETTES[current_theme];
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    s_screens[SCREEN_TELEMETRY] = scr;

    _title_bar(scr, "TELEMETRY", SCREEN_SWITCHBOARD);
    lv_obj_t* content = _content_area(scr, 46);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    // Network config panel
    lv_obj_t* np = _info_panel(content, "NETWORK");
    s_tele_net_lbl = _panel_value(np, 18);
    lv_obj_set_height(np, 90);
    lv_label_set_long_mode(s_tele_net_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(s_tele_net_lbl, 284, 64);

    // SIP pool panel
    lv_obj_t* pp = _info_panel(content, "SIP POOL");
    s_tele_pool_lbl = _panel_value(pp, 18);
    lv_obj_set_height(pp, 54);

    // Active sessions panel
    lv_obj_t* sp = _info_panel(content, "ACTIVE SESSIONS");
    s_tele_sessions_lbl = _panel_value(sp, 18);
    lv_obj_set_height(sp, 80);
    lv_label_set_long_mode(s_tele_sessions_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(s_tele_sessions_lbl, 284, 56);

    // Full log stream
    lv_obj_t* lp = _info_panel(content, "LOG STREAM (last 32)");
    s_tele_log_lbl = _panel_value(lp, 18);
    lv_obj_set_height(lp, 160);
    lv_label_set_long_mode(s_tele_log_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(s_tele_log_lbl, 284, 136);
    lv_obj_set_scroll_dir(lp, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(lp, LV_SCROLLBAR_MODE_AUTO);
}

void ui_update_telemetry(const UiTelemetrySnapshot* snap) {
    if (!snap) return;
    if (s_tele_net_lbl) {
        char buf[120];
        snprintf(buf, sizeof(buf), "IP: %s  Mode: %s\nSSID: %s",
            snap->ip,
            snap->wifi_mode == 1 ? "CLIENT" : snap->wifi_mode == 2 ? "INFRA" : "CAPTIVE",
            snap->ssid[0] ? snap->ssid : "—");
        lv_label_set_text(s_tele_net_lbl, buf);
    }
    if (s_tele_pool_lbl) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Sessions: %d/%d   Clients: %d/%d",
            snap->session_count, snap->session_max,
            snap->client_count, snap->client_max);
        lv_label_set_text(s_tele_pool_lbl, buf);
    }
    if (s_tele_sessions_lbl) {
        lv_label_set_text(s_tele_sessions_lbl,
            snap->sessions_text[0] ? snap->sessions_text : "(no active sessions)");
    }
    if (s_tele_log_lbl) {
        char block[TELE_LOG_LINES * 82 + 4] = {};
        int start = (s_tele_ring_head - s_tele_ring_count + TELE_LOG_LINES) % TELE_LOG_LINES;
        for (int i = 0; i < s_tele_ring_count; i++) {
            if (i > 0) strcat(block, "\n");
            strncat(block, s_tele_ring[(start + i) % TELE_LOG_LINES], 79);
        }
        lv_label_set_text(s_tele_log_lbl, block[0] ? block : "(no log entries)");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TASK 3E — Interface & Perimeter Management
// ─────────────────────────────────────────────────────────────────────────────

static void _build_perimeter_screen(void) {
    const LVColorTheme& t = PALETTES[current_theme];
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
    s_screens[SCREEN_PERIMETER] = scr;

    _title_bar(scr, "PERIMETER", SCREEN_SWITCHBOARD);
    lv_obj_t* content = _content_area(scr, 46);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    // ── SSH Engine toggle ──
    lv_obj_t* ssh_panel = _info_panel(content, "SSH REMOTE ACCESS");
    lv_obj_set_height(ssh_panel, 70);
    lv_obj_clear_flag(ssh_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* ssh_row = lv_obj_create(ssh_panel);
    lv_obj_set_size(ssh_row, 284, 34);
    lv_obj_align(ssh_row, LV_ALIGN_TOP_LEFT, 0, 20);
    lv_obj_set_style_bg_color(ssh_row, t.panel, LV_PART_MAIN);
    lv_obj_set_style_border_width(ssh_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ssh_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* ssh_lbl = lv_label_create(ssh_row);
    lv_obj_set_style_text_font(ssh_lbl, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(ssh_lbl, t.text, LV_PART_MAIN);
    lv_label_set_text(ssh_lbl, "Enable SSH engine:");
    lv_obj_align(ssh_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    s_perim_ssh_sw = lv_switch_create(ssh_row);
    lv_obj_align(s_perim_ssh_sw, LV_ALIGN_RIGHT_MID, 0, 0);
#if defined(ESP_PLATFORM)
    {
        uint8_t en = 1;
        nvs_handle_t h;
        if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "ssh_enabled", &en);
            nvs_close(h);
        }
        if (en) lv_obj_add_state(s_perim_ssh_sw, LV_STATE_CHECKED);
        else    lv_obj_clear_state(s_perim_ssh_sw, LV_STATE_CHECKED);
    }
#else
    lv_obj_add_state(s_perim_ssh_sw, LV_STATE_CHECKED);
#endif
    lv_obj_add_event_cb(s_perim_ssh_sw, [](lv_event_t* e) {
        bool enabled = lv_obj_has_state((lv_obj_t*)lv_event_get_target(e), LV_STATE_CHECKED);
#if defined(ESP_PLATFORM)
        nvs_handle_t h;
        if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "ssh_enabled", enabled ? 1 : 0);
            nvs_commit(h);
            nvs_close(h);
        }
#endif
        ESP_LOGI("perim", "[ui] SSH %s", enabled ? "enabled" : "disabled");
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    // ── Admin extension edit ──
    lv_obj_t* ext_panel = _info_panel(content, "ADMIN EXTENSION");
    lv_obj_set_height(ext_panel, 80);
    lv_obj_clear_flag(ext_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* ext_ta = lv_textarea_create(ext_panel);
    lv_obj_set_size(ext_ta, 180, 36);
    lv_obj_align(ext_ta, LV_ALIGN_BOTTOM_LEFT, 0, -2);
    lv_textarea_set_one_line(ext_ta, true);
    lv_textarea_set_placeholder_text(ext_ta, "e.g. 101");
    lv_obj_set_style_text_font(ext_ta, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ext_ta, t.bg, LV_PART_MAIN);
    lv_obj_set_style_text_color(ext_ta, t.text, LV_PART_MAIN);
#if defined(ESP_PLATFORM)
    {
        char ext_str[16] = "101";
        size_t sz = sizeof(ext_str);
        nvs_handle_t h;
        if (nvs_open("pbxcfg", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_str(h, "admin_ext", ext_str, &sz);
            nvs_close(h);
        }
        lv_textarea_set_text(ext_ta, ext_str);
    }
#else
    lv_textarea_set_text(ext_ta, "101");
#endif
    lv_obj_t* ext_save = lv_btn_create(ext_panel);
    lv_obj_set_size(ext_save, 90, 36);
    lv_obj_align(ext_save, LV_ALIGN_BOTTOM_RIGHT, 0, -2);
    lv_obj_set_style_bg_color(ext_save, t.border, LV_PART_MAIN);
    lv_obj_set_style_radius(ext_save, 4, LV_PART_MAIN);
    lv_obj_set_user_data(ext_save, ext_ta);
    lv_obj_add_event_cb(ext_save, [](lv_event_t* e) {
        lv_obj_t* ta = (lv_obj_t*)lv_event_get_user_data(e);
        if (!ta) return;
        const char* val = lv_textarea_get_text(ta);
        if (!val || strlen(val) < 1) return;
#if defined(ESP_PLATFORM)
        nvs_handle_t h;
        if (nvs_open("pbxcfg", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "admin_ext", val);
            nvs_commit(h);
            nvs_close(h);
        }
#endif
        ESP_LOGI("perim", "[ui] admin_ext set to %s", val);
    }, LV_EVENT_CLICKED, ext_ta);
    lv_obj_t* esl = lv_label_create(ext_save);
    lv_obj_set_style_text_font(esl, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(esl, t.bg, LV_PART_MAIN);
    lv_label_set_text(esl, "SAVE");
    lv_obj_center(esl);

    // ── Factory reset section ──
    lv_obj_t* frp = lv_obj_create(content);
    lv_obj_set_size(frp, 300, 120);
    lv_obj_set_style_bg_color(frp, t.panel, LV_PART_MAIN);
    lv_obj_set_style_border_color(frp, t.alert, LV_PART_MAIN);
    lv_obj_set_style_border_width(frp, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(frp, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(frp, 8, LV_PART_MAIN);
    lv_obj_clear_flag(frp, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* frh = lv_label_create(frp);
    lv_obj_set_style_text_font(frh, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(frh, t.alert, LV_PART_MAIN);
    lv_label_set_text(frh, "DANGER ZONE — FACTORY RESET");
    lv_obj_align(frh, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* frs = lv_label_create(frp);
    lv_obj_set_style_text_font(frs, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(frs, t.text, LV_PART_MAIN);
    lv_label_set_text(frs, "Erases ALL NVS data. Requires\nconfirmation.");
    lv_obj_align(frs, LV_ALIGN_TOP_LEFT, 0, 18);

    // Confirmation dialog (hidden)
    lv_obj_t* fr_dlg = lv_obj_create(scr);
    lv_obj_set_size(fr_dlg, 280, 180);
    lv_obj_center(fr_dlg);
    lv_obj_set_style_bg_color(fr_dlg, t.panel, LV_PART_MAIN);
    lv_obj_set_style_border_color(fr_dlg, t.alert, LV_PART_MAIN);
    lv_obj_set_style_border_width(fr_dlg, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(fr_dlg, 6, LV_PART_MAIN);
    lv_obj_add_flag(fr_dlg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fr_dlg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* frdl = lv_label_create(fr_dlg);
    lv_obj_set_style_text_font(frdl, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(frdl, t.alert, LV_PART_MAIN);
    lv_label_set_text(frdl, "ERASE ALL DATA?\nThis cannot be undone.");
    lv_obj_align(frdl, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t* fry = lv_btn_create(fr_dlg);
    lv_obj_set_size(fry, 100, 38);
    lv_obj_align(fry, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_set_style_bg_color(fry, t.alert, LV_PART_MAIN);
    lv_obj_set_style_radius(fry, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(fry, [](lv_event_t*) {
#if defined(ESP_PLATFORM)
        nvs_flash_erase();
        esp_restart();
#endif
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* fryl = lv_label_create(fry);
    lv_obj_set_style_text_font(fryl, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(fryl, t.bg, LV_PART_MAIN);
    lv_label_set_text(fryl, "ERASE");
    lv_obj_center(fryl);

    lv_obj_t* frn = lv_btn_create(fr_dlg);
    lv_obj_set_size(frn, 100, 38);
    lv_obj_align(frn, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_style_bg_color(frn, t.border, LV_PART_MAIN);
    lv_obj_set_style_radius(frn, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(frn, [](lv_event_t* e) {
        lv_obj_t* d = (lv_obj_t*)lv_event_get_user_data(e);
        if (d) lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, fr_dlg);
    lv_obj_t* frnl = lv_label_create(frn);
    lv_obj_set_style_text_font(frnl, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(frnl, t.bg, LV_PART_MAIN);
    lv_label_set_text(frnl, "CANCEL");
    lv_obj_center(frnl);

    lv_obj_t* fr_btn = lv_btn_create(frp);
    lv_obj_set_size(fr_btn, 260, 36);
    lv_obj_align(fr_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(fr_btn, t.alert, LV_PART_MAIN);
    lv_obj_set_style_radius(fr_btn, 4, LV_PART_MAIN);
    lv_obj_set_user_data(fr_btn, fr_dlg);
    lv_obj_add_event_cb(fr_btn, [](lv_event_t* e) {
        lv_obj_t* d = (lv_obj_t*)lv_event_get_user_data(e);
        if (d) lv_obj_clear_flag(d, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, fr_dlg);
    lv_obj_t* frbl = lv_label_create(fr_btn);
    lv_obj_set_style_text_font(frbl, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(frbl, t.bg, LV_PART_MAIN);
    lv_label_set_text(frbl, "ERASE ALL DATA");
    lv_obj_center(frbl);
}
