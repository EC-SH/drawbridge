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

// ─────────────────────────────────────────────────────────────────────────────
// Multi-screen navigation (Task 3A)
// ─────────────────────────────────────────────────────────────────────────────

typedef enum {
    SCREEN_SWITCHBOARD = 0,
    SCREEN_PROVISIONING,
    SCREEN_TOPOLOGY,
    SCREEN_TELEMETRY,
    SCREEN_PERIMETER,
    SCREEN_COUNT
} ui_screen_t;

void ui_navigate_to(ui_screen_t screen);
ui_screen_t ui_current_screen(void);

// ─────────────────────────────────────────────────────────────────────────────
// Telemetry snapshot (Task 3D)
// ─────────────────────────────────────────────────────────────────────────────

typedef struct {
    char ip[20];
    char gateway[20];
    char netmask[20];
    char dns[20];
    char ssid[33];
    uint8_t wifi_mode;
    int session_count;
    int session_max;
    int client_count;
    int client_max;
    char sessions_text[512]; // pre-formatted session table
} UiTelemetrySnapshot;

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

// Initialize the LVGL switchboard interface (header, status strip, jack board,
// patch strip, operator-log ticker, and the QR/reboot/onboarding modals).
void ui_init(void);

// Transition between Onboarding/Provisioning mode and the main switchboard.
// In provisioning mode ui_navigate_to(SCREEN_PROVISIONING) is called.
// Signature unchanged so esp_main_display.cpp compiles without modification.
void ui_set_onboarding_mode(bool onboarding, const char* ssid = "My-Ap", const char* pass = "12345678");

// Update the compact status strip (IP:port, EXT n/32, active CALLS) and the header
// clock/uptime + online dot. Mirrors the cadence of the old dashboard refresh.
void ui_update_status(const std::string& ip, int uptimeSec, int stationNum, int clientCount, int sessionCount);

// Repaint the jack board + patch strip from a fresh registrar snapshot. Only tiles
// whose state/label actually changed are touched, so the partial-refresh path keeps
// incremental updates cheap. Safe to call every poll.
void ui_update_board(const UiBoardSnapshot& snap);

// Append one line to the operator-log ticker (append-only, bounded; does NOT trigger
// a full-screen repaint under the partial-refresh display driver). Also feeds the
// 32-line telemetry log ring buffer.
void ui_add_log(const char* line);

// Update topology screen widgets (called from system_status_task when SCREEN_TOPOLOGY
// is active; already called under s_lvgl_mux).
void ui_update_topology(const char *ip, uint8_t mode, int clients);

// Update telemetry screen widgets (called from system_status_task when SCREEN_TELEMETRY
// is active; already called under s_lvgl_mux).
void ui_update_telemetry(const UiTelemetrySnapshot *snap);

// Header battery/aux indicator (kept for source compatibility; folded into header).
void ui_set_battery(float volts, int percent);

// Low-level capacitive-touch coordinate router (AXS15231B). Fallback hit-testing for
// the modal buttons when LVGL's own input routing is offset.
void ui_handle_touch_press(int16_t x, int16_t y);
