#pragma once

#include <string>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Pocket-Dial on-device touchscreen UI — "telephone SWITCHBOARD" theme.
//
// LVGL v8.4 (NOT v9 — the installed managed component is lvgl/lvgl ^8.3.11). All
// object/style/anim calls below target the v8 API. The display is 320x480 portrait
// (AXS15231B QSPI). The UI is built once on the main task, then driven from the
// Core-1 LVGL task and the Core-0 status task; every ui_* call MUST be made under
// the s_lvgl_mux recursive mutex in esp_main_display.cpp.
// ─────────────────────────────────────────────────────────────────────────────

// Bounds mirror the registrar pools (PoolConfig.hpp: 32 clients / 8 sessions). The
// jack board shows up to UI_MAX_JACKS tiles; extras scroll. The patch strip and log
// ticker are bounded the same way so nothing allocates on the LVGL tick.
#define UI_MAX_JACKS    32
#define UI_MAX_PATCHES  8
#define UI_LOG_LINES    6      // visible operator-log ticker rows

// Lamp / jack states, derived on the SIP side from the registrar snapshot.
enum UiJackState {
    UI_JACK_EMPTY = 0,   // unregistered slot  -> dark recessed tile
    UI_JACK_IDLE,        // registered & idle   -> lamp lit steady (accent)
    UI_JACK_INCALL,      // in an active call   -> lamp pulses (LVGL opacity anim)
    UI_JACK_DND          // do-not-disturb      -> amber ring
};

// One extension tile. `ext[0]=='\0'` marks an empty slot. Fixed-size, POD: copied
// into the UI snapshot with no heap churn on the polling tick.
struct UiJack {
    char ext[12];
    uint8_t state;       // UiJackState
};

// One active patch-cord (A <-> B). Fixed-size POD.
struct UiPatch {
    char a[12];
    char b[12];
    char state[12];      // short session-state tag ("Connected", "Invited", ...)
};

// Whole-board snapshot handed from the SIP poller (Core 0) to the UI. Caller fills
// only the first jackCount/patchCount entries; the UI bounds-checks both. Plain POD
// so it can be stack-built in system_status_task and copied in under the LVGL mutex.
struct UiBoardSnapshot {
    UiJack  jacks[UI_MAX_JACKS];
    int     jackCount;
    UiPatch patches[UI_MAX_PATCHES];
    int     patchCount;
};

// Initialize the LVGL switchboard interface (header, status strip, jack board,
// patch strip, operator-log ticker, and the QR/reboot/onboarding modals).
void ui_init(void);

// Transition between Onboarding Setup Mode and the main switchboard. In onboarding
// mode the scannable Wi-Fi QR join code + setup instructions cover the board.
void ui_set_onboarding_mode(bool onboarding, const char* ssid = "My-Ap", const char* pass = "12345678");

// Update the compact status strip (IP:port, EXT n/32, active CALLS) and the header
// clock/uptime + online dot. Mirrors the cadence of the old dashboard refresh.
void ui_update_status(const std::string& ip, int uptimeSec, int stationNum, int clientCount, int sessionCount);

// Repaint the jack board + patch strip from a fresh registrar snapshot. Only tiles
// whose state/label actually changed are touched, so the partial-refresh path keeps
// incremental updates cheap. Safe to call every poll.
void ui_update_board(const UiBoardSnapshot& snap);

// Append one line to the operator-log ticker (append-only, bounded; does NOT trigger
// a full-screen repaint under the partial-refresh display driver).
void ui_add_log(const char* line);

// Header battery/aux indicator (kept for source compatibility; folded into header).
void ui_set_battery(float volts, int percent);

// Low-level capacitive-touch coordinate router (AXS15231B). Fallback hit-testing for
// the modal buttons when LVGL's own input routing is offset.
void ui_handle_touch_press(int16_t x, int16_t y);
