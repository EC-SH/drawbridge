#include "ui.h"
#include "lvgl.h"
#include "qrcode.h"
#include "esp_log.h"
#include "esp_system.h"
#include <cstdio>
#include <cstring>

static const char* TAG = "ui_cpp";

// ─────────────────────────────────────────────────────────────────────────────
// THEME — vintage telephone operator's switchboard, now a passive WALLBOARD.
//   deep charcoal board, warm brass borders/labels, ONE hot accent (amber/phosphor)
//   for live call "glow". Centralized in the `t` struct (PALETTES[current_theme]).
//   Tap anywhere on the wallboard cycles BRASS <-> PHOSPHOR (the kept delight).
//   LVGL v8.4 colour API: LV_COLOR_MAKE(r,g,b).
// ─────────────────────────────────────────────────────────────────────────────
enum Theme {
    THEME_BRASS = 0,    // default: brass on charcoal, amber lamps
    THEME_PHOSPHOR,     // green phosphor lamps (alt operator board)
    THEME_COUNT
};

struct LVColorTheme {
    lv_color_t bg;          // board background (deep charcoal)
    lv_color_t panel;       // recessed panel / row face
    lv_color_t border;      // warm brass borders / rails
    lv_color_t text;        // brass label text
    lv_color_t highlight;   // bright brass / section headers / ringing
    lv_color_t accent;      // hot accent: live call glow / online lamp
    lv_color_t jack_empty;  // dim recessed (empty roster slot)
    lv_color_t dnd;         // DND amber
    lv_color_t alert;       // offline / fault red
};

static const LVColorTheme PALETTES[THEME_COUNT] = {
    // 0: BRASS — deep charcoal board, brass rails, amber lamps
    {
        LV_COLOR_MAKE(22, 20, 18),     // bg     near-black warm charcoal
        LV_COLOR_MAKE(40, 36, 30),     // panel  recessed brown-charcoal
        LV_COLOR_MAKE(176, 132, 56),   // border brass
        LV_COLOR_MAKE(214, 178, 110),  // text   warm brass
        LV_COLOR_MAKE(245, 214, 150),  // highlight bright brass / ringing
        LV_COLOR_MAKE(255, 176, 32),   // accent amber glow (active call / online)
        LV_COLOR_MAKE(30, 27, 23),     // jack_empty dark recessed
        LV_COLOR_MAKE(255, 176, 32),   // dnd    amber
        LV_COLOR_MAKE(200, 64, 40)     // alert  ember red
    },
    // 1: PHOSPHOR — charcoal board, brass rails, green phosphor lamps
    {
        LV_COLOR_MAKE(16, 20, 16),     // bg
        LV_COLOR_MAKE(28, 36, 28),     // panel
        LV_COLOR_MAKE(150, 130, 60),   // border (dim brass)
        LV_COLOR_MAKE(170, 210, 150),  // text
        LV_COLOR_MAKE(210, 240, 190),  // highlight / ringing
        LV_COLOR_MAKE(64, 255, 96),    // accent green phosphor glow
        LV_COLOR_MAKE(22, 28, 22),     // jack_empty
        LV_COLOR_MAKE(255, 176, 32),   // dnd amber (still amber for DND)
        LV_COLOR_MAKE(220, 70, 50)     // alert
    }
};

// ─── Globals & UI State ───
static Theme current_theme = THEME_BRASS;
static bool  is_onboarding = false;
static bool  is_online     = false;   // any station associated -> online lamp lit

// Core layout objects
static lv_obj_t* main_container = nullptr;

// Header (~46px): online lamp + POCKET-DIAL wordmark + uptime clock
static lv_obj_t* header_bar   = nullptr;
static lv_obj_t* title_label  = nullptr;
static lv_obj_t* online_dot   = nullptr;
static lv_obj_t* clock_label  = nullptr;

// Reach strip (~52px): "pocketdial.local  192.168.x.x" + "SSH HERE TO CONFIGURE"
static lv_obj_t* reach_strip  = nullptr;
static lv_obj_t* reach_label  = nullptr;   // host + ip
static lv_obj_t* ssh_label    = nullptr;   // the SSH cue

// LIVE CALLS hero — the glow. A bordered panel titled "LIVE CALLS" holding a
// scrollable column of fixed call rows (lamp glyph + "A -> B" + state/dur).
static lv_obj_t* calls_panel  = nullptr;
static lv_obj_t* calls_title  = nullptr;
static lv_obj_t* calls_list   = nullptr;   // flex-column scroll container
static lv_obj_t* calls_idle   = nullptr;   // "(no active calls)" placeholder
struct CallRow {
    lv_obj_t* row   = nullptr;
    lv_obj_t* lamp  = nullptr;   // ◆ active / ◐ ringing glyph
    lv_obj_t* label = nullptr;   // "106 -> 110   01:23"
    uint8_t   last_state = 0xFF;
    char      last_text[64] = {0};
};
static CallRow call_rows[UI_MAX_CALLS];

// Roster / counts strip (~30px): "EXT n/32   CALLS n/8   DND: 101 104 +2"
static lv_obj_t* counts_strip = nullptr;
static lv_obj_t* counts_label = nullptr;
static lv_obj_t* dnd_label    = nullptr;

// Vitals strip (~24px): "UP 01:23:45   HEAP 71%"
static lv_obj_t* vitals_strip = nullptr;
static lv_obj_t* vitals_label = nullptr;

// Operator-log ticker (~70px). Append-only ring of UI_LOG_LINES lines in one label.
static lv_obj_t* log_ticker  = nullptr;
static char      log_lines[UI_LOG_LINES][64];
static int       log_head = 0;
static int       log_count = 0;

// Onboarding / first-boot splash overlay (brand + join QR + SSH instructions).
static lv_obj_t* onboarding_modal = nullptr;
static lv_obj_t* onboarding_title = nullptr;
static lv_obj_t* onboarding_ssid_label = nullptr;
static lv_obj_t* onboarding_qr_canvas = nullptr;
static uint8_t*  onboarding_qr_buffer = nullptr;
static lv_obj_t* onboarding_hint = nullptr;

// Only Montserrat 14 is enabled in this sdkconfig (the shared sdkconfig is pinned
// and not ours to edit), so it is the one font we may reference.
#define UI_FONT (&lv_font_montserrat_14)

// State glyphs for the call lamp. We use LVGL's built-in symbol glyphs (merged into
// the Montserrat font by LVGL's default config) so they always render — arbitrary
// Unicode (◆/◐) is NOT in Montserrat 14 and would show a missing-glyph box. PLAY (a
// solid triangle) reads as a lit "active" lamp; BELL reads as "ringing".
#define GLYPH_ACTIVE  LV_SYMBOL_PLAY   // active call (the glow)
#define GLYPH_RINGING LV_SYMBOL_BELL   // ringing

// ─────────────────────────────────────────────────────────────────────────────
// NOTE: this panel runs in full_refresh mode (it garbles on partial windowed
// writes — see esp_main_display.cpp), so every repaint redraws the whole 320x480
// frame. There are therefore NO infinite animations anywhere in this UI: a live
// call is shown as a STATIC hot-accent lamp, not a pulse. State changes are
// discrete, and ui_update_* only touches objects whose content actually changed.
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Theme application. Recolours the live object tree from PALETTES[].
// ─────────────────────────────────────────────────────────────────────────────
static void style_call_row(int i, uint8_t state, bool used) {
    const LVColorTheme& t = PALETTES[current_theme];
    CallRow& cr = call_rows[i];
    if (!cr.row) return;

    lv_obj_set_style_bg_color(cr.row, t.panel, LV_PART_MAIN);
    lv_obj_set_style_border_color(cr.row, t.border, LV_PART_MAIN);
    lv_obj_set_style_text_color(cr.label, t.text, LV_PART_MAIN);

    if (!used) {
        // Idle/empty row — dim it (it is hidden anyway, but keep colours coherent
        // for when the theme flips while a row is parked).
        lv_obj_set_style_text_color(cr.lamp, t.jack_empty, LV_PART_MAIN);
        lv_label_set_text(cr.lamp, GLYPH_ACTIVE);
        return;
    }

    if (state == UI_CALL_ACTIVE) {
        // The glow: hot accent ◆, brass-bright border halo.
        lv_obj_set_style_text_color(cr.lamp, t.accent, LV_PART_MAIN);
        lv_label_set_text(cr.lamp, GLYPH_ACTIVE);
        lv_obj_set_style_border_color(cr.row, t.highlight, LV_PART_MAIN);
        lv_obj_set_style_border_width(cr.row, 2, LV_PART_MAIN);
    } else { // UI_CALL_RINGING
        lv_obj_set_style_text_color(cr.lamp, t.highlight, LV_PART_MAIN);
        lv_label_set_text(cr.lamp, GLYPH_RINGING);
        lv_obj_set_style_border_color(cr.row, t.border, LV_PART_MAIN);
        lv_obj_set_style_border_width(cr.row, 1, LV_PART_MAIN);
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
    lv_obj_set_style_bg_color(online_dot, is_online ? t.accent : t.alert, LV_PART_MAIN);

    // Reach strip
    lv_obj_set_style_bg_color(reach_strip, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_color(reach_strip, t.border, LV_PART_MAIN);
    lv_obj_set_style_text_color(reach_label, t.highlight, LV_PART_MAIN);
    lv_obj_set_style_text_color(ssh_label, t.accent, LV_PART_MAIN);

    // LIVE CALLS panel
    lv_obj_set_style_bg_color(calls_panel, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_color(calls_panel, t.border, LV_PART_MAIN);
    lv_obj_set_style_bg_color(calls_title, t.border, LV_PART_MAIN);
    lv_obj_set_style_text_color(calls_title, t.bg, LV_PART_MAIN);
    lv_obj_set_style_bg_color(calls_list, t.bg, LV_PART_MAIN);
    lv_obj_set_style_text_color(calls_idle, t.text, LV_PART_MAIN);
    for (int i = 0; i < UI_MAX_CALLS; i++) {
        bool used = (call_rows[i].last_state != 0xFF);
        uint8_t st = used ? call_rows[i].last_state : (uint8_t)UI_CALL_ACTIVE;
        style_call_row(i, st, used);
    }

    // Roster / counts strip
    lv_obj_set_style_bg_color(counts_strip, t.panel, LV_PART_MAIN);
    lv_obj_set_style_border_color(counts_strip, t.border, LV_PART_MAIN);
    lv_obj_set_style_text_color(counts_label, t.text, LV_PART_MAIN);
    lv_obj_set_style_text_color(dnd_label, t.dnd, LV_PART_MAIN);

    // Vitals strip
    lv_obj_set_style_bg_color(vitals_strip, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_color(vitals_strip, t.border, LV_PART_MAIN);
    lv_obj_set_style_text_color(vitals_label, t.text, LV_PART_MAIN);

    // Log ticker
    lv_obj_set_style_bg_color(log_ticker, t.bg, LV_PART_MAIN);
    lv_obj_set_style_border_color(log_ticker, t.border, LV_PART_MAIN);
    lv_obj_set_style_text_color(log_ticker, t.text, LV_PART_MAIN);

    // Onboarding splash
    if (onboarding_modal) {
        lv_obj_set_style_bg_color(onboarding_modal, t.bg, LV_PART_MAIN);
        lv_obj_set_style_border_color(onboarding_modal, t.border, LV_PART_MAIN);
        lv_obj_set_style_bg_color(onboarding_title, t.border, LV_PART_MAIN);
        lv_obj_set_style_text_color(onboarding_title, t.bg, LV_PART_MAIN);
        lv_obj_set_style_text_color(onboarding_ssid_label, t.highlight, LV_PART_MAIN);
        lv_obj_set_style_text_color(onboarding_hint, t.accent, LV_PART_MAIN);
    }
}

// ─── Tap-anywhere -> cycle theme (the kept delight; passive otherwise) ───
static void board_tap_cb(lv_event_t* e) {
    (void)e;
    if (is_onboarding) return;
    current_theme = (Theme)((current_theme + 1) % THEME_COUNT);
    ESP_LOGI(TAG, "Theme cycled -> %s", current_theme == THEME_BRASS ? "BRASS" : "PHOSPHOR");
    apply_theme();
}

// ─── QR Code drawing (PRESERVED from original; used only by onboarding) ───
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

// ─── Helper: build the LIVE CALLS hero (panel + scrollable fixed rows) ───
static void build_calls_panel() {
    const LVColorTheme& t = PALETTES[current_theme];

    calls_panel = lv_obj_create(main_container);
    lv_obj_set_size(calls_panel, 314, 188);
    lv_obj_align(calls_panel, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_border_width(calls_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(calls_panel, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(calls_panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(calls_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(calls_panel, LV_OBJ_FLAG_EVENT_BUBBLE);  // taps reach the board (theme cycle)

    // Section header band — "LIVE CALLS" in inverse brass.
    calls_title = lv_label_create(calls_panel);
    lv_obj_set_style_text_font(calls_title, UI_FONT, LV_PART_MAIN);
    lv_obj_set_size(calls_title, 314, 22);
    lv_label_set_text(calls_title, " LIVE CALLS");
    lv_obj_set_style_bg_opa(calls_title, LV_OPA_COVER, LV_PART_MAIN);  // inverse-brass band
    lv_obj_set_style_pad_top(calls_title, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_left(calls_title, 6, LV_PART_MAIN);
    lv_obj_align(calls_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(calls_title, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Scrollable list region beneath the title band.
    calls_list = lv_obj_create(calls_panel);
    lv_obj_set_size(calls_list, 314, 164);
    lv_obj_align(calls_list, LV_ALIGN_TOP_LEFT, 0, 24);
    lv_obj_set_style_border_width(calls_list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(calls_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(calls_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(calls_list, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(calls_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(calls_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(calls_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(calls_list, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Idle placeholder, shown when there are zero live calls.
    calls_idle = lv_label_create(calls_list);
    lv_obj_set_style_text_font(calls_idle, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(calls_idle, "(no active calls)");
    lv_obj_add_flag(calls_idle, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Pre-create the fixed call rows (hidden until used). No heap churn per poll.
    for (int i = 0; i < UI_MAX_CALLS; i++) {
        CallRow& cr = call_rows[i];
        cr.row = lv_obj_create(calls_list);
        lv_obj_set_size(cr.row, 296, 30);
        lv_obj_set_style_radius(cr.row, 3, LV_PART_MAIN);
        lv_obj_set_style_pad_all(cr.row, 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(cr.row, 1, LV_PART_MAIN);
        lv_obj_clear_flag(cr.row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cr.row, LV_OBJ_FLAG_EVENT_BUBBLE);

        cr.lamp = lv_label_create(cr.row);
        lv_obj_set_style_text_font(cr.lamp, UI_FONT, LV_PART_MAIN);
        lv_label_set_text(cr.lamp, GLYPH_ACTIVE);
        lv_obj_align(cr.lamp, LV_ALIGN_LEFT_MID, 2, 0);
        lv_obj_add_flag(cr.lamp, LV_OBJ_FLAG_EVENT_BUBBLE);

        cr.label = lv_label_create(cr.row);
        lv_obj_set_style_text_font(cr.label, UI_FONT, LV_PART_MAIN);
        lv_label_set_text(cr.label, "");
        lv_obj_align(cr.label, LV_ALIGN_LEFT_MID, 24, 0);
        lv_obj_add_flag(cr.label, LV_OBJ_FLAG_EVENT_BUBBLE);

        cr.last_state = 0xFF;
        cr.last_text[0] = '\0';
        lv_obj_add_flag(cr.row, LV_OBJ_FLAG_HIDDEN);
    }
    (void)t;
}

// ─────────────────────────────────────────────────────────────────────────────
// UI init — build the wallboard.
// ─────────────────────────────────────────────────────────────────────────────
void ui_init(void) {
    ESP_LOGI(TAG, "Initializing LVGL wallboard interface (LVGL v8)...");

    main_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(main_container, 320, 480);
    lv_obj_set_style_pad_all(main_container, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(main_container, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(main_container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(main_container, LV_OBJ_FLAG_SCROLLABLE);
    // Tap anywhere on the board cycles the theme (the kept delight). The board is
    // otherwise passive — no config taps.
    lv_obj_add_flag(main_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(main_container, board_tap_cb, LV_EVENT_CLICKED, NULL);

    // ── Header (~46px): online lamp + POCKET-DIAL wordmark + uptime clock ──
    header_bar = lv_obj_create(main_container);
    lv_obj_set_size(header_bar, 320, 46);
    lv_obj_align(header_bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_pad_all(header_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(header_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(header_bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_radius(header_bar, 0, LV_PART_MAIN);
    lv_obj_clear_flag(header_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(header_bar, LV_OBJ_FLAG_EVENT_BUBBLE);  // let taps reach the board

    online_dot = lv_obj_create(header_bar);
    lv_obj_set_size(online_dot, 12, 12);
    lv_obj_set_style_radius(online_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(online_dot, 0, LV_PART_MAIN);
    lv_obj_align(online_dot, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_clear_flag(online_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(online_dot, LV_OBJ_FLAG_EVENT_BUBBLE);

    title_label = lv_label_create(header_bar);
    lv_obj_set_style_text_font(title_label, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(title_label, "POCKET-DIAL");
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 30, 0);
    lv_obj_add_flag(title_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    clock_label = lv_label_create(header_bar);
    lv_obj_set_style_text_font(clock_label, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(clock_label, "00:00:00");
    lv_obj_align(clock_label, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_flag(clock_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // ── Reach strip (~52px): host + IP, and the SSH cue ──
    reach_strip = lv_obj_create(main_container);
    lv_obj_set_size(reach_strip, 320, 52);
    lv_obj_align(reach_strip, LV_ALIGN_TOP_LEFT, 0, 46);
    lv_obj_set_style_pad_all(reach_strip, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(reach_strip, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(reach_strip, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_radius(reach_strip, 0, LV_PART_MAIN);
    lv_obj_clear_flag(reach_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(reach_strip, LV_OBJ_FLAG_EVENT_BUBBLE);

    reach_label = lv_label_create(reach_strip);
    lv_obj_set_style_text_font(reach_label, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(reach_label, "pocketdial.local  0.0.0.0");
    lv_obj_align(reach_label, LV_ALIGN_TOP_LEFT, 10, 6);
    lv_obj_add_flag(reach_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    ssh_label = lv_label_create(reach_strip);
    lv_obj_set_style_text_font(ssh_label, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(ssh_label, LV_SYMBOL_SETTINGS "  SSH HERE TO CONFIGURE");
    lv_obj_align(ssh_label, LV_ALIGN_BOTTOM_LEFT, 10, -6);
    lv_obj_add_flag(ssh_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // ── LIVE CALLS hero ──
    build_calls_panel();

    // ── Roster / counts strip (~30px) ──
    counts_strip = lv_obj_create(main_container);
    lv_obj_set_size(counts_strip, 314, 30);
    lv_obj_align(counts_strip, LV_ALIGN_TOP_MID, 0, 294);
    lv_obj_set_style_border_width(counts_strip, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(counts_strip, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(counts_strip, 2, LV_PART_MAIN);
    lv_obj_clear_flag(counts_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(counts_strip, LV_OBJ_FLAG_EVENT_BUBBLE);

    counts_label = lv_label_create(counts_strip);
    lv_obj_set_style_text_font(counts_label, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(counts_label, "EXT 0/32   CALLS 0/8");
    lv_obj_align(counts_label, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_flag(counts_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    dnd_label = lv_label_create(counts_strip);
    lv_obj_set_style_text_font(dnd_label, UI_FONT, LV_PART_MAIN);
    lv_label_set_long_mode(dnd_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(dnd_label, 150);
    lv_label_set_text(dnd_label, "");
    lv_obj_align(dnd_label, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_add_flag(dnd_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // ── Vitals strip (~24px) ──
    vitals_strip = lv_obj_create(main_container);
    lv_obj_set_size(vitals_strip, 314, 26);
    lv_obj_align(vitals_strip, LV_ALIGN_TOP_MID, 0, 328);
    lv_obj_set_style_border_width(vitals_strip, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(vitals_strip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(vitals_strip, 0, LV_PART_MAIN);
    lv_obj_clear_flag(vitals_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(vitals_strip, LV_OBJ_FLAG_EVENT_BUBBLE);

    vitals_label = lv_label_create(vitals_strip);
    lv_obj_set_style_text_font(vitals_label, UI_FONT, LV_PART_MAIN);
    lv_label_set_text(vitals_label, "UP 00:00:00   HEAP --%");
    lv_obj_align(vitals_label, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_flag(vitals_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // ── Operator-log ticker (~70px) ──
    log_ticker = lv_obj_create(main_container);
    lv_obj_set_size(log_ticker, 314, 92);
    lv_obj_align(log_ticker, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_border_width(log_ticker, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(log_ticker, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(log_ticker, 4, LV_PART_MAIN);
    lv_obj_clear_flag(log_ticker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(log_ticker, UI_FONT, LV_PART_MAIN);
    lv_obj_add_flag(log_ticker, LV_OBJ_FLAG_EVENT_BUBBLE);
    // The actual text lives in a child label so the ticker box repaints in isolation.
    // The child is stored on the container's user_data so ui_add_log() can find it.
    lv_obj_t* tlbl = lv_label_create(log_ticker);
    lv_obj_set_style_text_font(tlbl, UI_FONT, LV_PART_MAIN);
    lv_label_set_long_mode(tlbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(tlbl, 304);
    lv_label_set_text(tlbl, "OPERATOR LOG");
    lv_obj_align(tlbl, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(tlbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_user_data(log_ticker, tlbl);

    apply_theme();
}

// ─── Onboarding / first-boot splash (brand + join QR + "then SSH") ───
// Config is SSH-only AFTER the box is on the network, but a fresh/un-provisioned
// box still needs a way to get onto Wi-Fi the first time. This overlay covers the
// board: brand, a scannable Wi-Fi-join QR, and instructions to then SSH to
// pocketdial.local. No ongoing touch-config surfaces.
void ui_set_onboarding_mode(bool onboarding, const char* ssid, const char* pass) {
    is_onboarding = onboarding;
    if (!onboarding) {
        if (onboarding_modal) lv_obj_add_flag(onboarding_modal, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    ESP_LOGI(TAG, "Spinning up Onboarding/Splash view: SSID=%s", ssid);

    if (!onboarding_modal) {
        onboarding_modal = lv_obj_create(main_container);
        lv_obj_set_size(onboarding_modal, 320, 480);
        lv_obj_align(onboarding_modal, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_border_width(onboarding_modal, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(onboarding_modal, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(onboarding_modal, 10, LV_PART_MAIN);
        lv_obj_clear_flag(onboarding_modal, LV_OBJ_FLAG_SCROLLABLE);

        // Brand band.
        onboarding_title = lv_label_create(onboarding_modal);
        lv_obj_set_style_text_font(onboarding_title, UI_FONT, LV_PART_MAIN);
        lv_label_set_text(onboarding_title, "  POCKET-DIAL  -  SETUP");
        lv_obj_set_style_bg_opa(onboarding_title, LV_OPA_COVER, LV_PART_MAIN);  // inverse-brass band
        lv_obj_set_size(onboarding_title, 300, 24);
        lv_obj_align(onboarding_title, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_set_style_pad_top(onboarding_title, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_left(onboarding_title, 14, LV_PART_MAIN);

        onboarding_ssid_label = lv_label_create(onboarding_modal);
        lv_obj_set_style_text_font(onboarding_ssid_label, UI_FONT, LV_PART_MAIN);
        lv_obj_align(onboarding_ssid_label, LV_ALIGN_TOP_MID, 0, 48);

        #if defined(ESP_PLATFORM)
        onboarding_qr_buffer = (uint8_t*)malloc(19602);
        #else
        static uint8_t static_onboard_qr_buf[19602];
        onboarding_qr_buffer = static_onboard_qr_buf;
        #endif
        onboarding_qr_canvas = lv_canvas_create(onboarding_modal);
        lv_canvas_set_buffer(onboarding_qr_canvas, onboarding_qr_buffer, 99, 99, LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(onboarding_qr_canvas, LV_ALIGN_CENTER, 0, -10);

        lv_obj_t* prompt_lbl = lv_label_create(onboarding_modal);
        lv_obj_set_style_text_font(prompt_lbl, UI_FONT, LV_PART_MAIN);
        lv_label_set_text(prompt_lbl,
            "1. Scan the QR to join Wi-Fi\n"
            "2. Open http://192.168.4.1/\n"
            "   to point me at your network");
        lv_obj_set_style_text_align(prompt_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(prompt_lbl, LV_ALIGN_BOTTOM_MID, 0, -54);

        // The SSH cue — once online, all config is over SSH.
        onboarding_hint = lv_label_create(onboarding_modal);
        lv_obj_set_style_text_font(onboarding_hint, UI_FONT, LV_PART_MAIN);
        lv_label_set_text(onboarding_hint, "then SSH to pocketdial.local");
        lv_obj_set_style_text_align(onboarding_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(onboarding_hint, LV_ALIGN_BOTTOM_MID, 0, -14);
    }

    lv_obj_clear_flag(onboarding_modal, LV_OBJ_FLAG_HIDDEN);

    char ssid_msg[128];
    snprintf(ssid_msg, sizeof(ssid_msg), "Join: %s   Pass: %s", ssid, pass);
    lv_label_set_text(onboarding_ssid_label, ssid_msg);
    lv_obj_set_style_text_align(onboarding_ssid_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    char qr_str[128];
    if (strlen(pass) > 0) {
        snprintf(qr_str, sizeof(qr_str), "WIFI:S:%s;T:WPA;P:%s;;", ssid, pass);
    } else {
        snprintf(qr_str, sizeof(qr_str), "WIFI:S:%s;T:nopass;;", ssid);
    }
    draw_qr(onboarding_qr_canvas, onboarding_qr_buffer, qr_str);

    apply_theme();
}

// ─── Header clock + reach line + counts + vitals ───
void ui_update_status(const std::string& ip, int uptimeSec, int stationNum,
                      int clientCount, int sessionCount, int freeHeapPct) {
    if (is_onboarding) return;

    char buf[96];

    // Header clock = uptime HH:MM:SS
    int hrs = uptimeSec / 3600;
    int mins = (uptimeSec % 3600) / 60;
    int secs = uptimeSec % 60;
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hrs, mins, secs);
    if (clock_label) lv_label_set_text(clock_label, buf);

    // Reach line: host + active IP.
    snprintf(buf, sizeof(buf), "pocketdial.local  %s", ip.c_str());
    if (reach_label) lv_label_set_text(reach_label, buf);

    // Online lamp: lit (accent) when at least one station is associated; else red.
    is_online = (stationNum > 0);
    if (online_dot) {
        const LVColorTheme& t = PALETTES[current_theme];
        lv_obj_set_style_bg_color(online_dot, is_online ? t.accent : t.alert, LV_PART_MAIN);
    }

    // Counts: EXT n/32  CALLS n/8 (the registrar pool caps).
    snprintf(buf, sizeof(buf), "EXT %d/%d   CALLS %d/%d",
             clientCount, UI_MAX_EXTENSIONS, sessionCount, UI_MAX_CALLS);
    if (counts_label) lv_label_set_text(counts_label, buf);

    // Vitals: uptime + free-heap %. (CPU omitted — not cheaply available here.)
    if (freeHeapPct >= 0) {
        snprintf(buf, sizeof(buf), "UP %02d:%02d:%02d   HEAP %d%%", hrs, mins, secs, freeHeapPct);
    } else {
        snprintf(buf, sizeof(buf), "UP %02d:%02d:%02d   HEAP --%%", hrs, mins, secs);
    }
    if (vitals_label) lv_label_set_text(vitals_label, buf);
}

// ─── Board update: live calls + roster counts + DND chips ───
void ui_update_board(const UiBoardSnapshot& snap) {
    if (is_onboarding) return;

    int nc = snap.callCount;
    if (nc > UI_MAX_CALLS) nc = UI_MAX_CALLS;

    // Idle placeholder visible only when there are zero live calls.
    if (calls_idle) {
        if (nc == 0) lv_obj_clear_flag(calls_idle, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(calls_idle, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < UI_MAX_CALLS; i++) {
        CallRow& cr = call_rows[i];
        if (i < nc) {
            const UiCall& c = snap.calls[i];
            uint8_t state = (c.state == UI_CALL_ACTIVE) ? UI_CALL_ACTIVE : UI_CALL_RINGING;

            // Compose the row text: "A -> B   MM:SS" (or "ringing"). Buffer matches
            // CallRow::last_text; minutes are clamped to 2 digits so the duration field
            // stays bounded (a call >99 min just pins at 99:59).
            char text[64];
            if (state == UI_CALL_ACTIVE && c.durationSec > 0) {
                int mm = c.durationSec / 60, ss = c.durationSec % 60;
                if (mm > 99) { mm = 99; ss = 59; }
                snprintf(text, sizeof(text), "%s " LV_SYMBOL_RIGHT " %s   %02d:%02d", c.a, c.b, mm, ss);
            } else if (state == UI_CALL_ACTIVE) {
                snprintf(text, sizeof(text), "%s " LV_SYMBOL_RIGHT " %s   active", c.a, c.b);
            } else {
                snprintf(text, sizeof(text), "%s " LV_SYMBOL_RIGHT " %s   ringing", c.a, c.b);
            }

            bool text_changed  = (strncmp(cr.last_text, text, sizeof(cr.last_text)) != 0);
            bool state_changed = (cr.last_state != state);
            if (text_changed) {
                lv_label_set_text(cr.label, text);
                strncpy(cr.last_text, text, sizeof(cr.last_text) - 1);
                cr.last_text[sizeof(cr.last_text) - 1] = '\0';
            }
            if (state_changed || text_changed) {
                style_call_row(i, state, true);
                cr.last_state = state;
                lv_obj_clear_flag(cr.row, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (cr.last_state != 0xFF) {
            // Row no longer in use — park it.
            cr.last_state = 0xFF;
            cr.last_text[0] = '\0';
            lv_obj_add_flag(cr.row, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // DND chips: "DND: 101 104 +2"
    if (dnd_label) {
        char dndline[160];
        int nd = snap.dndCount;
        if (nd > UI_MAX_DND_CHIPS) nd = UI_MAX_DND_CHIPS;
        if (nd == 0 && snap.dndOverflow <= 0) {
            dndline[0] = '\0';
        } else {
            size_t off = 0;
            off += snprintf(dndline + off, sizeof(dndline) - off, "DND:");
            for (int i = 0; i < nd && off < sizeof(dndline) - 1; i++) {
                off += snprintf(dndline + off, sizeof(dndline) - off, " %s", snap.dnd[i]);
            }
            if (snap.dndOverflow > 0 && off < sizeof(dndline) - 1) {
                snprintf(dndline + off, sizeof(dndline) - off, " +%d", snap.dndOverflow);
            }
        }
        lv_label_set_text(dnd_label, dndline);
    }
}

// ─── Operator-log ticker (append-only ring, bounded) ───
void ui_add_log(const char* line) {
    if (!log_ticker || !line) return;

    strncpy(log_lines[log_head], line, sizeof(log_lines[0]) - 1);
    log_lines[log_head][sizeof(log_lines[0]) - 1] = '\0';
    log_head = (log_head + 1) % UI_LOG_LINES;
    if (log_count < UI_LOG_LINES) log_count++;

    // Rebuild the visible block oldest->newest into one label (single repaint).
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
    // The wallboard header shows uptime/clock rather than battery; this is a no-op
    // retained so existing callers in esp_main_display.cpp keep linking.
    (void)volts; (void)percent;
}

// ─── Direct touch coordinate router ───
// The wallboard is passive: the only gesture is tap-anywhere to cycle the theme,
// routed by LVGL's own indev hit-testing to board_tap_cb. No coordinate fallbacks
// are needed (there are no config buttons/modals left). During onboarding the
// captive web portal does the configuring, so there is nothing to route here either.
void ui_handle_touch_press(int16_t x, int16_t y) {
    (void)x; (void)y;
}
