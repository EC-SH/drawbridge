#pragma once

#include <string>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Pocket-Dial on-device touchscreen UI — the "BBS-glow" live-status WALLBOARD.
//
// The 3.5" 320x480 portrait panel (Guition JC3248W535, AXS15231B QSPI) is no
// longer a configuration surface — ALL config moved to the SSH "sysop terminal."
// This screen is now a passive, always-on, glanceable monitor: the physical-screen
// counterpart of the SSH [1] System Monitor. It shows live calls, the extension
// roster + counts, vitals (uptime / free heap), a reach line ("SSH here to
// configure"), and the operator-log ticker. The only touch interaction left is
// tap-anywhere to cycle the brass/phosphor theme.
//
// LVGL v8.4 (NOT v9 — the installed managed component is lvgl/lvgl ^8.3.11). All
// object/style/anim calls below target the v8 API. The UI is built once on the main
// task, then driven from the Core-1 LVGL task and the Core-0 status task; every
// ui_* call MUST be made under the s_lvgl_mux recursive mutex in
// esp_main_display.cpp. The panel runs in FULL_REFRESH mode (see esp_main_display.cpp)
// so there are NO per-tick animations — state changes are discrete and cheap.
// ─────────────────────────────────────────────────────────────────────────────

// Bounds mirror the registrar pools (PoolConfig.hpp: 32 clients / 8 sessions).
#define UI_MAX_EXTENSIONS  32     // registered-extension roster cap (clientPool)
#define UI_MAX_CALLS       8      // live-call list cap (sessionPool)
#define UI_MAX_DND_CHIPS   8      // DND chips shown before "+N"
#define UI_LOG_LINES       4      // visible operator-log ticker rows

// Live-call state, derived on the SIP side from the registrar snapshot. Reuses the
// lamp colours: ACTIVE = hot accent (◆), RINGING = brass highlight (◐).
enum UiCallState {
    UI_CALL_RINGING = 0,   // Invited  -> ◐ ringing (brass highlight)
    UI_CALL_ACTIVE         // Connected -> ◆ active  (hot accent — the "glow")
};

// One live call row (caller ext -> destination). Fixed-size POD: copied into the
// snapshot with no heap churn on the polling tick.
struct UiCall {
    char a[12];          // caller extension
    char b[12];          // destination extension
    uint8_t state;       // UiCallState
    int     durationSec; // connected duration (0 while ringing)
};

// One registered extension (roster). `ext[0]=='\0'` marks an empty slot. POD.
struct UiExt {
    char ext[12];
    bool inCall;
    bool dnd;
};

// Whole-board snapshot handed from the SIP poller (Core 0) to the UI. The caller
// fills only the first callCount/extCount/dndCount entries; the UI bounds-checks
// all three. Plain POD so it can be stack-built in system_status_task and copied
// in under the LVGL mutex.
struct UiBoardSnapshot {
    UiCall calls[UI_MAX_CALLS];
    int    callCount;
    UiExt  exts[UI_MAX_EXTENSIONS];
    int    extCount;
    char   dnd[UI_MAX_DND_CHIPS][12];
    int    dndCount;
    int    dndOverflow;   // extra DND extensions beyond UI_MAX_DND_CHIPS (shown as "+N")
};

// Initialize the LVGL wallboard (header, reach line, LIVE CALLS hero, roster/counts,
// vitals, operator-log ticker, and the minimal first-boot onboarding/splash overlay).
void ui_init(void);

// Transition between first-boot Onboarding/Splash and the live wallboard. In
// onboarding mode a brand splash + scannable Wi-Fi join QR + "then SSH to
// pocketdial.local" instructions cover the board (config is SSH-only afterward).
void ui_set_onboarding_mode(bool onboarding, const char* ssid = "My-Ap", const char* pass = "12345678");

// Update the header (uptime clock + online lamp), the reach line (host + IP), the
// roster counts strip (EXT n/32 · CALLS n/8) and the vitals strip (uptime / free
// heap %). Mirrors the SSH monitor's vitals content. freeHeapPct < 0 => "—".
void ui_update_status(const std::string& ip, int uptimeSec, int stationNum,
                      int clientCount, int sessionCount, int freeHeapPct = -1);

// Repaint the LIVE CALLS list + roster + DND chips from a fresh registrar snapshot.
// Only rows/chips whose content actually changed are touched, so the full-refresh
// frame work stays minimal. Safe to call every poll. This is the "glow" — it lights
// up the instant a call is placed.
void ui_update_board(const UiBoardSnapshot& snap);

// Append one line to the operator-log ticker (append-only, bounded ring).
void ui_add_log(const char* line);

// Header aux indicator (kept for source compatibility; folded into the header).
void ui_set_battery(float volts, int percent);

// Low-level capacitive-touch coordinate router (AXS15231B). Wallboard is passive:
// the only gesture is tap-anywhere to cycle the brass/phosphor theme (and dismiss
// onboarding fallbacks). Kept so esp_main_display.cpp keeps linking.
void ui_handle_touch_press(int16_t x, int16_t y);
