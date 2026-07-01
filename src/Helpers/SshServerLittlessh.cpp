// SshServerLittlessh.cpp — littlessh-backed SSH transport for SshServer.
// SPDX-License-Identifier: MIT
//
// Compiled only when POCKETDIAL_HAS_LITTLESSH is defined — set by main/CMakeLists.txt
// for every non-display transport (eth/wifi/lan8720), which don't link wolfSSH. Gives
// those transports a real SSH console on the PSA Crypto API (mbedTLS) — no wolfSSL/
// wolfSSH — driving the SAME ANSI Tui as the wolfSSH path. The transport-agnostic Tui
// providers + setters are lifted verbatim from SshServer.cpp; de-duplicating them behind
// a shared header is a tracked follow-up (kept isolated here so this never affects the
// wolfSSH build).
#ifdef POCKETDIAL_HAS_LITTLESSH

#include <string>
#include <cstdint>
#include <cstring>
#include <functional>

#include "SshServer.hpp"
#include "Tui.hpp"
#include "AdminAuth.hpp"
#include "RequestsHandler.hpp"
#include "SipSecretStore.hpp"
#include "littlessh.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG_L = "ssh-little";

// ===== BEGIN lifted providers (SshServer.cpp 554-839) =========================
static Tui::LiveStats buildLiveStats()
{
    Tui::LiveStats st;
    st.uptimeSec   = static_cast<uint64_t>(esp_timer_get_time() / 1000000LL);
    st.provisioned = AdminAuth::isProvisioned();
    st.maxCalls    = POCKETDIAL_MAX_SESSIONS;
    st.maxExt      = POCKETDIAL_MAX_CLIENTS;

    RequestsHandler* h = SshServer::instance().handler();
    if (h)
    {
        // getClientCount/getSessionCount snapshot-copy under _snapshotMutex.
        st.online      = static_cast<int>(h->getClientCount());
        st.extCount    = st.online;                       // provisioned == registered (no separate roster yet)
        st.unreachable = 0;                               // UNREACH roster lands with the [1] monitor increment
        st.activeCalls = static_cast<int>(h->getSessionCount());
    }

    // Identity block fields. The IP/MAC are best-effort; the spine is correct even
    // if they read defaults.
    // This backend serves the wired transports (eth/lan8720), so report the
    // Ethernet interface MAC — the one the network actually sees — not the
    // Wi-Fi STA MAC the display/wolfSSH build reads. Falls back to the base MAC
    // if no eth MAC is provisioned.
    char mac[18] = {0};
    uint8_t m[6] = {0};
    if (esp_read_mac(m, ESP_MAC_ETH) == ESP_OK ||
        esp_read_mac(m, ESP_MAC_BASE) == ESP_OK)
    {
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
        st.mac = mac;
    }
    st.host = "drawbridge.local";
    st.fw   = "v3.0.0";

    // Real heap used % for the [6] ABOUT vitals line — same basis as the monitor's
    // MEM bar: (allocated)/(allocated+free) of the default (internal+8bit) heap the
    // SIP/SSH tasks run on. Honest number; 0 only if the heap query fails.
    {
        multi_heap_info_t info{};
        heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);
        size_t total = info.total_free_bytes + info.total_allocated_bytes;
        if (total > 0)
            st.heapUsedPct = static_cast<int>((info.total_allocated_bytes * 100ULL) / total);
    }

    // Live IP + network role, plumbed from app_main via setNetInfo() once the
    // netif has an address. Until then the LiveStats defaults (0.0.0.0 / AP) stand
    // so the spine is always correct.
    SshServer& self = SshServer::instance();
    if (self.netInfoSet())
    {
        st.ip      = self.netIp();
        st.netMode = self.netMode();          // 0=SETUP 1=STATION 2=AP
        st.apMode  = (self.netMode() != 1);   // legacy bool: STATION→Client, else AP
        st.ssid    = self.netSsid();          // [2] NETWORK SSID line (may be empty)
    }
    return st;
}

// Build the [5] REPORTS CDR snapshot the Recent-Calls / Event-Log views read. Maps
// the SIP-side CallDetailRecord ring (newest first, copied under _snapshotMutex) onto
// the renderer's SIP-agnostic Tui::CdrEntry. Fetched only while the reports screen is
// active; never blocks signaling.
static Tui::ReportsSnapshot buildReportsSnapshot()
{
    Tui::ReportsSnapshot rs;
    RequestsHandler* h = SshServer::instance().handler();
    if (!h) return rs;
    auto recs = h->getCallDetailRecords();   // newest first
    rs.cdr.reserve(recs.size());
    for (const auto& r : recs)
    {
        Tui::CdrEntry e;
        e.caller      = r.caller;
        e.callee      = r.callee;
        e.startMs     = r.startMs;
        e.durationSec = r.durationSec;
        switch (r.result)
        {
            case CdrResult::Answered:    e.result = Tui::CdrResult::Answered;    break;
            case CdrResult::Busy:        e.result = Tui::CdrResult::Busy;        break;
            case CdrResult::Cancelled:   e.result = Tui::CdrResult::Cancelled;   break;
            case CdrResult::Unavailable: e.result = Tui::CdrResult::Unavailable; break;
            case CdrResult::Failed:      e.result = Tui::CdrResult::Failed;      break;
        }
        e.codec  = "PCMU";   // server media is PCMU; the CDR carries no per-call codec
        rs.cdr.push_back(std::move(e));
    }
    return rs;
}

// Build the [3] PBX CONFIG snapshot the five tabs read. Folds the registrar's live
// roster (getActiveClients) together with the persisted DND / forward / ring-group
// config (getDndExtensions / getForwards / getRingGroups), all snapshot-copied under
// _snapshotMutex. Fetched only while the PBX screen is active; never blocks signaling.
//
// HONESTY: getActiveClients lists currently-REGISTERED extensions (the open registrar
// has no separate provisioned store). So `extensions` here is exactly that roster with
// its real, persisted feature state attached — no invented rows. There is also no
// display-name store, so PbxExt::name stays empty (the tables show the ext number).
static Tui::PbxConfigSnapshot buildPbxConfigSnapshot()
{
    Tui::PbxConfigSnapshot cfg;
    cfg.maxExt     = POCKETDIAL_MAX_CLIENTS;
    cfg.maxMembers = POCKETDIAL_MAX_CLIENTS;

    RequestsHandler* h = SshServer::instance().handler();
    if (!h) return cfg;

    cfg.adminExt = h->getAdminExt();
    auto clients   = h->getActiveClients();   // {ext, ip:port}
    auto dndList   = h->getDndExtensions();   // {ext...}
    auto forwards  = h->getForwards();        // {ext, always, busy, noAnswer}
    auto groups    = h->getRingGroups();      // {groupExt, "ringall"|"hunt", "m1,m2,.."}

    // Index the set of currently-registered extensions for the G6 integrity flag.
    auto isRegistered = [&](const std::string& ext) {
        for (const auto& c : clients) if (c.first == ext) return true;
        return false;
    };

    // Build the ring-group rows first so the per-extension "ringGroup" membership and
    // the §3.5 FWD column can reference them.
    for (const auto& g : groups)
    {
        Tui::PbxGroup pg;
        pg.name    = std::get<0>(g);
        pg.ringAll = (std::get<1>(g) != "hunt");
        pg.members = pbx::splitMembers(std::get<2>(g));
        for (const auto& m : pg.members) if (!isRegistered(m)) ++pg.badMembers;
        cfg.groups.push_back(std::move(pg));
    }

    // One PbxExt per registered extension, with its DND + forward + group state.
    for (const auto& c : clients)
    {
        Tui::PbxExt e;
        e.ext   = c.first;
        e.state = Tui::State::Online;          // registered == online
        for (const auto& d : dndList) if (d == e.ext) { e.dnd = true; break; }
        for (const auto& f : forwards)
        {
            if (std::get<0>(f) == e.ext)
            {
                e.cfu  = std::get<1>(f);
                e.cfb  = std::get<2>(f);
                e.cfna = std::get<3>(f);
                break;
            }
        }
        for (const auto& g : cfg.groups)
            for (const auto& m : g.members)
                if (m == e.ext) { e.ringGroup = g.name; break; }
        // STAGE 3: fold the SIP digest credential state (does the SipSecretStore hold
        // an HA1 for this extension?). Drives the Extensions tab ◆ SECURED / · none chip.
        e.secured = SipSecretStore::hasSecret(e.ext);
        cfg.extensions.push_back(std::move(e));
    }
    return cfg;
}

// Map the SIP-layer RegistrarMode enum onto the Tui's RegMode (same value range; the
// renderer carries no SIP dependency). Kept as a tiny free function so both the
// provider and the mode-setter round-trip through one place.
static Tui::RegMode toTuiRegMode(RequestsHandler::RegistrarMode m)
{
    switch (m)
    {
        case RequestsHandler::RegistrarMode::Open:   return Tui::RegMode::Open;
        case RequestsHandler::RegistrarMode::Learn:  return Tui::RegMode::Learn;
        case RequestsHandler::RegistrarMode::Secure: return Tui::RegMode::Secure;
    }
    return Tui::RegMode::Open;
}
static RequestsHandler::RegistrarMode fromTuiRegMode(Tui::RegMode m)
{
    switch (m)
    {
        case Tui::RegMode::Open:   return RequestsHandler::RegistrarMode::Open;
        case Tui::RegMode::Learn:  return RequestsHandler::RegistrarMode::Learn;
        case Tui::RegMode::Secure: return RequestsHandler::RegistrarMode::Secure;
    }
    return RequestsHandler::RegistrarMode::Open;
}

// Build the [4]/[D] REGISTRAR snapshot: the live registrar admission mode + the
// adopted-device roster (getAdoptedDevices). Each device's `secured` folds in BOTH the
// SIP-side DeviceState::Secured AND the SipSecretStore having an HA1 for its extension
// (a learned device whose ext has been given a secret reads as secured-capable). The
// `online` flag is the live registration binding. Fetched only while the screen is up.
static Tui::RegistrarInfo buildRegistrarInfo()
{
    Tui::RegistrarInfo ri;
    ri.realm = SipSecretStore::kRealm;

    RequestsHandler* h = SshServer::instance().handler();
    if (!h) return ri;

    ri.mode = toTuiRegMode(h->getRegistrarMode());
    for (const auto& d : h->getAdoptedDevices())
    {
        Tui::DeviceRow row;
        row.mac     = d.mac;
        row.ext     = d.extension;
        row.online  = d.online;
        row.secured = (d.state == RequestsHandler::DeviceState::Secured) ||
                      (!d.extension.empty() && SipSecretStore::hasSecret(d.extension));
        ri.devices.push_back(std::move(row));
    }
    return ri;
}

// Build the richer live-monitor snapshot the [1] SYSTEM MONITOR wallboard reads.
// Pulls the live-call matrix, the registration roster, and real heap/uptime vitals
// off the registrar's thread-safe snapshot getters (each copies under its own
// mutex). Fetched only while the monitor screen is active (the hub stays cheap).
// Honest numbers only: MEM is the real free-heap ratio; CPU% is intentionally NOT
// reported (FreeRTOS runtime stats are off in sdkconfig) — the renderer shows MEM/
// POOL/UP and omits CPU rather than faking it.
static Tui::MonitorSnapshot buildMonitorSnapshot()
{
    Tui::MonitorSnapshot ms;
    ms.uptimeSec = static_cast<uint64_t>(esp_timer_get_time() / 1000000LL);
    ms.maxCalls  = POCKETDIAL_MAX_SESSIONS;

    // Real heap used %: (total - free) / total. heap_caps_get_info gives both; we use
    // the default (internal+8bit) heap which is what the SIP/SSH tasks actually run on.
    {
        multi_heap_info_t info{};
        heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);
        size_t total = info.total_free_bytes + info.total_allocated_bytes;
        if (total > 0)
            ms.memUsedPct = static_cast<int>((info.total_allocated_bytes * 100ULL) / total);
    }

    RequestsHandler* h = SshServer::instance().handler();
    if (h)
    {
        auto clients  = h->getActiveClients();   // {ext, ip:port}
        auto sessions = h->getActiveSessions();  // {a, b, stateStr-as-int, ...}
        auto dndList  = h->getDndExtensions();    // {ext...}

        ms.online      = static_cast<int>(clients.size());
        ms.unreachable = 0;                       // no known-down roster yet (honest 0)
        // ms.poolUsed / ms.activeCalls are set from the matrix loop below (live calls).

        // Live-call matrix: one CallRow per active session, numbered into channels.
        // getActiveSessions() returns {caller, callee, stateString, durationSec}
        // (the 3rd element is sessionStateToString(), the 4th is the talk duration).
        // Map the state string to the status-lexicon State (Connected→ACTIVE,
        // Invited→RINGING, teardown states fold to idle and are skipped here).
        int ch = 1;
        ms.activeCalls = 0;
        for (const auto& sx : sessions)
        {
            if (ch > ms.maxCalls) break;
            const std::string& stStr = std::get<2>(sx);
            Tui::State state;
            if (stStr == "Connected")     state = Tui::State::Active;
            else if (stStr == "Invited")  state = Tui::State::Ringing;
            else                          continue;   // teardown — not a live channel

            Tui::CallRow cr;
            cr.ch     = ch++;
            cr.ext    = std::get<0>(sx);                                  // caller
            cr.dest   = std::string("→ ") + std::get<1>(sx); // -> callee
            cr.durSec = std::get<3>(sx);                                  // talk seconds
            cr.codec  = "PCMU";
            cr.state  = state;
            ms.activeCalls++;
            ms.calls.push_back(std::move(cr));
        }

        ms.poolUsed = ms.activeCalls;             // VITALS "POOL n/8 calls"

        // Roster: one entry per registered client, ● ONLINE; DND members read ⊘ DND.
        std::vector<std::string> dnd(dndList.begin(), dndList.end());
        for (const auto& c : clients)
        {
            Tui::RosterRow rr;
            rr.ext   = c.first;
            rr.state = Tui::State::Online;
            for (const auto& d : dnd) { if (d == c.first) { rr.state = Tui::State::Dnd; break; } }
            ms.roster.push_back(std::move(rr));
        }
    }
    return ms;
}

// ===== END lifted providers ===================================================

// Wire all transport-agnostic Tui providers/setters (lifted from SshServer.cpp
// runTuiSession 852-912, 933-962). peer/sessUptime are supplied by the caller.
static void configureTui(Tui& tui, const std::string& peer, uint64_t sessUptime)
{
    tui.setStatsProvider(&buildLiveStats);
    tui.setMonitorProvider(&buildMonitorSnapshot);
    tui.setReportsProvider(&buildReportsSnapshot);
    tui.setPbxConfigProvider(&buildPbxConfigSnapshot);

    // ── [3] PBX CONFIG mutation actions (all thread-safe, NVS-persisted) ──────
    // DND toggle ([3]/Forwards [Space]) → setDnd(ext,on).
    tui.setDndSetter([](const std::string& ext, bool on) {
        RequestsHandler* hh = SshServer::instance().handler();
        if (hh) hh->setDnd(ext, on);
    });
    // Forward set ([3]/Forwards editor) → setForward(ext,trigger,target). An empty
    // target clears the trigger. Targets are bare extensions only. NOTE: forward-to-
    // ring-group is NOT wired in the SIP layer — redirectInvite→findClient does not
    // unwrap a "grp:<name>" token, so such a forward would silently no-op at call
    // time. The [3] Forwards picker therefore offers extensions only (it no longer
    // lists ring groups). Resolving group forward targets is a tracked follow-up.
    tui.setForwardSetter([](const std::string& ext, const std::string& trigger,
                            const std::string& target) {
        RequestsHandler* hh = SshServer::instance().handler();
        if (hh) hh->setForward(ext, trigger, target);
    });
    // Ring-group create/edit/delete ([3]/Ring Groups) → setRingGroup(name,members,
    // mode). An empty member list deletes the group. Returns "" on success or a short
    // operator-terse error to surface inline.
    tui.setRingGroupSetter([](const std::string& name, const std::string& members,
                              const std::string& mode) -> std::string {
        RequestsHandler* hh = SshServer::instance().handler();
        if (!hh) return "PBX not attached.";
        hh->setRingGroup(name, members, mode);
        return "";
    });

    // ── [4]/[D] REGISTRAR · DEVICES providers + actions (STAGE 3) ─────────────
    // Snapshot: live registrar mode + adopted-device roster (+ per-ext secret state).
    tui.setRegistrarProvider(&buildRegistrarInfo);
    // Mode change ([4]/[D]/[M]) → RequestsHandler::setRegistrarMode (runtime + NVS).
    tui.setRegModeSetter([](Tui::RegMode m) {
        RequestsHandler* hh = SshServer::instance().handler();
        if (hh) hh->setRegistrarMode(fromTuiRegMode(m));
    });
    // Assign/rotate a per-extension SIP secret ([4]/[D]/[A] + Extensions [S]). Stores
    // HA1=MD5(ext:realm:secret) in the SipSecretStore. Never echoed by the TUI; the
    // plaintext is consumed here and never persisted (only its HA1 is).
    tui.setSecretSetter([](const std::string& ext, const std::string& secret) -> std::string {
        if (!SipSecretStore::setSecret(ext, secret))
            return "Secret rejected (bad extension or empty).";
        return "";
    });
    // Secure/lock a learned device ([4]/[D]/[S]) → RequestsHandler::secureDevice.
    tui.setDeviceSecurer([](const std::string& macOrExt) -> bool {
        RequestsHandler* hh = SshServer::instance().handler();
        return hh ? hh->secureDevice(macOrExt) : false;
    });
    // Forget a device ([4]/[D]/[F], guarded) → RequestsHandler::forgetDevice.
    tui.setDeviceForgetter([](const std::string& macOrExt) -> bool {
        RequestsHandler* hh = SshServer::instance().handler();
        return hh ? hh->forgetDevice(macOrExt) : false;
    });

    // ── Per-session SECURITY context ([4]) ───────────────────────────────────
    tui.setSecurityProvider([peer, sessUptime]() {
        Tui::SecurityInfo si;
        si.provisioned = AdminAuth::isProvisioned();
        si.sshEnabled  = SshServer::instance().isEnabled();
        si.sessionUser = "sysop";
        si.sessionPeer = peer;
        // "since" rendered as uptime HH:MM:SS at session open (no RTC wall clock).
        unsigned h = (unsigned)((sessUptime / 3600ULL) % 100ULL);
        unsigned m = (unsigned)((sessUptime / 60ULL) % 60ULL);
        unsigned s = (unsigned)(sessUptime % 60ULL);
        char b[16]; snprintf(b, sizeof(b), "%02u:%02u:%02u", h, m, s);
        si.sinceClock = b;
        RequestsHandler* hh = SshServer::instance().handler();
        if (hh) si.adminExt = hh->getAdminExt();
        return si;
    });

    // ── [4]/[P] change-PIN apply: verify current (if provisioned) + set new. ──
    tui.setPinChanger([](const std::string& cur, const std::string& neu) -> std::string {
        // Issue #57: re-auth on the SSH channel (this runs in the SSH TUI session).
        if (AdminAuth::isProvisioned() && !AdminAuth::verifyPin(cur, AdminAuth::Channel::Ssh))
            return "Current PIN is wrong.";
        if (!AdminAuth::setPin(neu))
            return "New PIN rejected (too short?).";
        return "";   // success
    });

    // ── [4]/[K] SSH access toggle: persist + start/stop the engine. ───────────
    tui.setSshToggle([](bool enabled) {
        SshServer::instance().setEnabled(enabled);
    });

    // ── [3]/TRUNK provider + apply (WAN trunk / dial-9 PSTN access) ───────────
    // Kept in lockstep with the wolfSSH backend's configureTui (SshServer.cpp).
    // Snapshot flattens RequestsHandler::TrunkConfig onto Tui::TrunkInfo — the
    // secret itself NEVER crosses (only `secretSet`).
    tui.setTrunkProvider([]() -> Tui::TrunkInfo {
        Tui::TrunkInfo t;
        RequestsHandler* hh = SshServer::instance().handler();
        if (!hh) return t;
        RequestsHandler::TrunkConfig cfg = hh->getTrunkConfig();
        t.baseUrl     = cfg.baseUrl;
        t.clientId    = cfg.clientId;
        t.sourceDn    = cfg.sourceDn;
        t.secretSet   = !cfg.clientSecret.empty();
        t.useLoopback = cfg.useLoopback;
        t.connected   = hh->isTrunkConnected();
        return t;
    });
    // Apply: overlay edited fields on the stored config. An EMPTY secret means
    // "keep the existing secret" (the modal never echoes it). setTrunkConfig
    // validates + persists; returns "" on success (applies on reboot).
    tui.setTrunkSetter([](const std::string& baseUrl, const std::string& clientId,
                          const std::string& secret, const std::string& sourceDn,
                          bool useLoopback) -> std::string {
        RequestsHandler* hh = SshServer::instance().handler();
        if (!hh) return "PBX not attached.";
        RequestsHandler::TrunkConfig cfg = hh->getTrunkConfig();
        cfg.baseUrl     = baseUrl;
        cfg.clientId    = clientId;
        if (!secret.empty()) cfg.clientSecret = secret;   // empty = keep existing
        cfg.sourceDn    = sourceDn;
        cfg.useLoopback = useLoopback;
        return hh->setTrunkConfig(cfg);
    });
}

// ── littlessh session glue (one client at a time; static state is fine) ───────
static Tui*     g_tui  = nullptr;
static uint16_t g_cols = 80;
static uint16_t g_rows = 24;
static uint8_t  g_hostkey[32];
static lssh_config_t g_cfg;
// Cooperative shutdown flag (wired to lssh_config_t::stop): set by the SSH
// disable toggle so the accept loop unwinds gracefully instead of the task
// being vTaskDelete'd out from under the bound socket + live crypto state.
static volatile bool g_stop = false;
// The Tui state machine is NOT re-entrant. lssh_write() may pump the connection
// while stalled on channel window and deliver an inbound keystroke (on_data)
// mid-render; this guard makes such a re-entrant feed/tick a no-op rather than
// interleaving two renders into one byte stream. A keystroke arriving during a
// repaint is dropped — acceptable, and only reachable when the 64 KB window is
// momentarily exhausted (effectively never for a tiny-write TUI console).
static bool g_tui_busy = false;
// Frame buffer: Tui::put() calls accumulate here; one lssh_write() drains it per
// feed()/tickLive() — 50-150 tiny TLS packets collapse to one. reserve(8192) at
// session open keeps the steady-state allocation-free. ~20 KB worst case (132×50)
// lives on the heap (PSRAM-friendly), outside the SIP zero-heap hot path.
static std::string g_frame;

static void flush_frame(lssh_session_t* s)
{
    if (!g_frame.empty())
    {
        lssh_write(s, reinterpret_cast<const uint8_t*>(g_frame.data()), g_frame.size());
        g_frame.clear();
    }
}

// Onboarding model (mirrors wolfSSH wsUserAuth): SSH is OPEN until an admin PIN
// is provisioned, then the admin PIN is the SSH password. Username is ignored
// at auth time — lssh_username(s) is used in ll_on_open() for Guided Mode.
static bool ll_password_auth(void* u, const char* user, const char* pass)
{
    (void)u; (void)user;
    if (!AdminAuth::isProvisioned()) return true;
    // Issue #57: account SSH brute-force on the SSH channel so spraying here cannot
    // trip the admin's HTTP-login lockout (and vice-versa).
    return AdminAuth::verifyPin(pass, AdminAuth::Channel::Ssh);
}

static void ll_on_pty(void* u, lssh_session_t* s, uint16_t cols, uint16_t rows)
{
    (void)u; (void)s;
    g_cols = cols ? cols : 80;
    g_rows = rows ? rows : 24;
    if (g_tui) g_tui->setSize(g_cols, g_rows);
}

static void ll_on_open(void* u, lssh_session_t* s, const char* exec_cmd)
{
    (void)u;
    if (exec_cmd) { lssh_printf(s, "interactive console only\r\n"); lssh_exit(s, 1); return; }
    static Tui tui;                 // single client → one persistent instance
    g_tui = &tui;
    g_frame.clear();
    g_frame.reserve(8192);
    tui.setWriter([](const char* d, size_t n) {
        if (d && n) g_frame.append(d, n);
    });
    const uint64_t up = static_cast<uint64_t>(esp_timer_get_time() / 1000000LL);
    configureTui(tui, "?", up);     // littlessh doesn't expose the peer fd → "?"
    tui.setSize(g_cols, g_rows);
    const char* uname = lssh_username(s);
    tui.setUsername(uname ? uname : "");
    // #157: flush the INITIAL paint (banner + first screen, several KB) now rather
    // than leaving it buffered until the first keystroke or the ≤1 s idle beat —
    // otherwise a fresh connection shows a blank terminal until the user types.
    // Guard the flush like feed()/tickLive(): lssh_write() can pump the connection
    // on a window stall and re-enter ll_on_data, whose feed would append to (and
    // possibly reallocate) g_frame under the in-flight write.
    g_tui_busy = true;
    tui.begin();
    flush_frame(s);
    g_tui_busy = false;
}

static void ll_on_data(void* u, lssh_session_t* s, const uint8_t* data, size_t len)
{
    (void)u;
    if (!g_tui || g_tui_busy) return;   // drop input that arrives mid-render
    g_tui_busy = true;
    bool keep = g_tui->feed(reinterpret_cast<const char*>(data), len);
    flush_frame(s);
    g_tui_busy = false;
    if (!keep) lssh_exit(s, 0);         // operator logged out ([L])
}

// Idle beat (recv timeout, channel up): refresh the live wallboard/clock so the
// [1] System Monitor and title-bar time advance without waiting for a keystroke,
// matching the wolfSSH backend's 1 Hz tickLive() cadence.
static void ll_on_idle(void* u, lssh_session_t* s)
{
    (void)u; (void)s;
    if (!g_tui || g_tui_busy) return;
    g_tui_busy = true;
    g_tui->tickLive();
    flush_frame(s);
    g_tui_busy = false;
}

static void ll_on_close(void* u, lssh_session_t* s)
{
    (void)u; (void)s;
    g_tui = nullptr;
    g_cols = 80; g_rows = 24;   // reset so the next client without a pty-req
                               // inherits the 80x24 default, not stale geometry
}

// Host key: 32-byte P-256 scalar in NVS ("storage"/"ssh_p256_key"); make one if absent.
static bool load_or_make_hostkey()
{
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) != ESP_OK)
        return lssh_hostkey_generate(g_hostkey) == 0;   // no NVS → ephemeral
    size_t len = 32;
    bool ok = (nvs_get_blob(h, "ssh_p256_key", g_hostkey, &len) == ESP_OK && len == 32);
    if (!ok && lssh_hostkey_generate(g_hostkey) == 0) {
        // Persist so the host key is stable across boots (TOFU/known_hosts). If
        // the write fails (e.g. full partition) the key is still usable for this
        // boot, but warn loudly — silently regenerating every boot trains
        // operators to click through host-key-changed MITM warnings.
        esp_err_t e = nvs_set_blob(h, "ssh_p256_key", g_hostkey, 32);
        if (e == ESP_OK) e = nvs_commit(h);
        if (e != ESP_OK)
            ESP_LOGW(TAG_L, "host key not persisted (%s) — will regenerate next boot",
                     esp_err_to_name(e));
        ok = true;
    }
    nvs_close(h);
    return ok;
}

// FreeRTOS task entry — spawned by SshServer::start() under POCKETDIAL_HAS_LITTLESSH.
extern "C" void pd_littlessh_task(void* arg)
{
    (void)arg;
    if (!load_or_make_hostkey()) {
        ESP_LOGE(TAG_L, "no SSH host key — littlessh not started");
        SshServer::instance().clearBackendTask();   // don't leave a stale handle
        vTaskDelete(nullptr);
        return;
    }
    char fp[64];
    if (lssh_hostkey_fingerprint(g_hostkey, fp, sizeof fp) == 0)
        ESP_LOGI(TAG_L, "littlessh host key %s", fp);

    g_stop = false;
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.port            = POCKETDIAL_SSH_PORT;
    g_cfg.host_key        = g_hostkey;
    g_cfg.auth_max_tries  = 3;
    // Doubles as the idle-tick cadence (1 Hz live wallboard) and the backstop
    // that stops a silent/half-open peer from wedging the single-client server.
    g_cfg.recv_timeout_ms = 1000;
    g_cfg.banner          = "pocket-dial \xE2\x80\x94 authorized use only\r\n";
    g_cfg.password_auth   = ll_password_auth;
    g_cfg.on_open         = ll_on_open;
    g_cfg.on_data         = ll_on_data;
    g_cfg.on_pty          = ll_on_pty;
    g_cfg.on_close        = ll_on_close;
    g_cfg.on_idle         = ll_on_idle;
    g_cfg.stop            = &g_stop;       // cooperative shutdown (no vTaskDelete)

    ESP_LOGI(TAG_L, "littlessh SSH console starting on port %u", (unsigned)POCKETDIAL_SSH_PORT);
    lssh_server_run(&g_cfg);       // blocks until g_stop or an unrecoverable error
    ESP_LOGI(TAG_L, "littlessh SSH console stopped");
    // Clear the owning SshServer's task handle BEFORE self-deleting, so a later
    // start() can spawn a fresh task and stop() never vTaskDelete's a dead TCB.
    SshServer::instance().clearBackendTask();
    vTaskDelete(nullptr);
}

// Cooperative stop hook, called by SshServer::stop() under the littlessh backend
// (runs on whatever task toggled SSH off — typically the SSH task itself, from
// the [4]/[K] TUI action). Just raises the flag; the accept/recv loop unwinds at
// the next idle beat and the task tears itself down cleanly via clearBackendTask().
extern "C" void pd_littlessh_request_stop(void)
{
    g_stop = true;
}

// Re-arm the listener: called by SshServer::start() so that re-enabling SSH after
// a cooperative stop (or while the previous task is still winding down) does not
// leave the stale stop flag set.
extern "C" void pd_littlessh_clear_stop(void)
{
    g_stop = false;
}

#endif // POCKETDIAL_HAS_LITTLESSH
