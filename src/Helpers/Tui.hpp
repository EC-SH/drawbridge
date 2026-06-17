#pragma once
// Tui.hpp — pocket-dial ANSI "sysop terminal" TUI (foundation, B2b-1).
//
// This is the retro telco/BBS operator board rendered over an SSH PTY. It is the
// product's face: a 3-zone screen spine (title bar / body / key-hint footer), the
// BRASS(default)/PHOSPHOR theme palette mapped to xterm-16 SGR, the glyph+label
// status lexicon (never color alone), and a single-key typeahead hub.
//
// Design source of truth: docs/design/{README,brand,tui-style,tui-ia,accessibility}.md.
// The geometry, glyphs, palette, copy and keybindings below implement those docs.
//
// ── Transport-agnostic by construction ───────────────────────────────────────
// Tui does NOT include wolfSSH (or any transport). It is driven by:
//   * a write callback  std::function<void(const char*, size_t)>  (bytes → channel)
//   * input bytes fed one connection-read at a time via feed()/feedByte()
// SshServer.cpp wires the callback to wolfSSH_stream_send and pumps
// wolfSSH_stream_read into feed(). This keeps the renderer host-compilable and
// unit-testable (tests inject a std::string-collecting callback) with zero ESP or
// wolfSSH dependency — the only platform guard is for esp_timer (uptime/clock),
// behind the existing defined(ESP_PLATFORM)||defined(ESP32).
//
// ── Live data ────────────────────────────────────────────────────────────────
// The hub status line + title-bar clock reflect reality through a LiveStats
// snapshot supplied by a StatsProvider callback (SshServer fills it from the
// attached RequestsHandler + esp_timer). If no provider is set, everything reads
// zero but the spine stays correct (the renderer never depends on live data).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Tui
{
public:
    // ── Theme + rendering capability tiers (renderer contract, tui-style §6) ──
    enum class Theme { Brass, Phosphor };

    // The status-lexicon states (brand §4.5). Used by the live monitor so the
    // renderer maps a state → its glyph+label+role in ONE place (never color alone).
    enum class State { Online, Unreach, Ringing, Active, Dnd, Idle };

    // One row of the live-call matrix (tui-style §2.9). `ch` is the 1-based channel
    // slot; an idle slot has empty ext/dest and State::Idle.
    struct CallRow
    {
        int         ch    = 0;
        std::string ext;          // calling extension ("101")
        std::string dest;         // "→ 102" / "→ grp:sales" (caller fills the arrow)
        int         durSec = 0;   // talk/setup duration
        std::string codec = "PCMU";
        State       state = State::Idle;
    };

    // One registration-roster entry (tui-style §3.3 ROSTER block).
    struct RosterRow
    {
        std::string ext;
        std::string name;         // display name (may be empty → show ext only)
        State       state = State::Unreach;
    };

    // How a call ended (tui-style §3.11 / IA §6.2). Mirrors src/SIP/CdrResult so the
    // renderer carries no SIP dependency — SshServer maps the real enum onto this one
    // when it builds the CDR snapshot. The glyph+LABEL lexicon lives in cdrChip().
    enum class CdrResult { Answered, Busy, Cancelled, Unavailable, Failed };

    // One Call Detail Record row for the [5] REPORTS · Recent Calls view (§3.11.1)
    // and its detail modal (§3.11.2). A flat value copy of src/SIP/CallDetailRecord —
    // taken off the SIP thread under RequestsHandler's snapshot mutex — so reading it
    // never blocks signaling and Tui stays host-/SIP-agnostic. `startMs` is the same
    // monotonic-uptime basis as the title-bar clock (no RTC guarantee) → rendered as
    // HH:MM:SS of uptime, honest about the lack of a wall clock.
    struct CdrEntry
    {
        std::string caller;       // From extension ("101")
        std::string callee;       // To extension ("102" / "grp:sales")
        uint64_t    startMs = 0;  // call-start, monotonic-uptime ms (clock basis)
        uint32_t    durationSec = 0;  // talk time (0 if never connected)
        CdrResult   result = CdrResult::Failed;
        std::string codec = "PCMU";   // detail modal field (server media is PCMU)
        std::string callId;       // detail modal field (may be empty → "—")
    };

    // The [5] REPORTS snapshot — the CDR ring (newest first) the screen reads. Fetched
    // only while the reports screen is active (the hub stays cheap), like MonitorSnapshot.
    struct ReportsSnapshot
    {
        std::vector<CdrEntry> cdr;   // newest first (RequestsHandler order)
    };

    // The [4] SECURITY snapshot — admin/session facts the screen reads (§3.10). Plain
    // copies taken off the SIP/SSH state so the renderer never blocks. `provisioned`
    // and `sshEnabled` drive the SET/none + access chips; the session block names the
    // current operator + peer captured at accept time.
    struct SecurityInfo
    {
        bool        provisioned = false;  // AdminAuth::isProvisioned() → ● SET / ○ none
        bool        sshEnabled  = true;   // SshServer::isEnabled() → access chip
        std::string adminExt    = "101";  // RequestsHandler::getAdminExt()
        std::string sessionUser = "sysop"; // SSH login name for this session
        std::string sessionPeer = "?";     // "ip:port" of the connected operator
        std::string sinceClock  = "";      // uptime HH:MM:SS when the session opened
    };

    // ── [4]/[D] REGISTRAR · DEVICES surface (STAGE 3: digest auth + Learn mode) ──
    // The registrar admission policy (RequestsHandler::RegistrarMode) surfaced to the
    // operator as a plain word + helper copy. Mirrors the SIP enum value range so the
    // setter round-trips without the renderer pulling in the SIP header.
    enum class RegMode { Open = 0, Learn = 1, Secure = 2 };

    // One adopted-device row (RequestsHandler::AdoptedDevice) flattened for the screen.
    // `secured` folds the SipSecretStore state (the device's extension can digest-auth);
    // `online` is the live registration binding. The screen renders these as the chip
    // lexicon: ● ONLINE / ◐ LEARNED / ◆ SECURED — glyph+LABEL, never colour alone.
    struct DeviceRow
    {
        std::string mac;          // 12 lowercase hex chars ("aabbccddeeff")
        std::string ext;          // the AOR it last registered as ("101")
        bool        secured = false;  // promoted to MAC-lock + digest-enforced
        bool        online  = false;  // currently has a live registration binding
    };

    // The [4]/[D] REGISTRAR snapshot the screen reads: the current admission mode plus
    // the adopted-device roster. Copies taken off the SIP thread (SshServer fills it
    // from RequestsHandler::getRegistrarMode + getAdoptedDevices) so reading never
    // blocks signaling. Absent provider → Open mode + empty list (an honest panel).
    struct RegistrarInfo
    {
        RegMode                mode = RegMode::Open;
        std::vector<DeviceRow> devices;
        std::string            realm = "drawbridge";   // digest realm (informational)
    };

    // ── [3] PBX CONFIG snapshot (tui-style §3.5-§3.9) ─────────────────────────
    // The whole config surface the five tabs read, snapshotted off the SIP thread
    // (SshServer fills it from RequestsHandler's thread-safe getters). Plain value
    // copies so reading never blocks signaling and the renderer stays SIP-agnostic.
    //
    // HONESTY: there is no persistent "provisioned extension" store today — the
    // registrar is OPEN (POCKETDIAL_OPEN_REGISTRAR). `extensions` therefore lists the
    // currently-REGISTERED extensions (from getActiveClients) with their live DND /
    // forward / ring-group status folded in. The Extensions "add"/range flow renders
    // per §3.5.3 but is honestly stubbed (nothing to persist) — see renderPbxExt.

    // One row of the Extensions / Forwards table: a registered extension plus the
    // per-extension feature state the firmware DOES persist (DND + 3 forwards). `name`
    // is empty today (no display-name store) → the table shows the ext number alone.
    struct PbxExt
    {
        std::string ext;
        std::string name;          // display name — empty until a name store exists
        State       state = State::Unreach;  // ● ONLINE / ○ UNREACH (registered?)
        bool        dnd   = false;
        std::string cfu;           // CFU (always) target — "" = unset; "grp:x" = group
        std::string cfb;           // CFB (busy)
        std::string cfna;          // CFNA (no-answer)
        std::string ringGroup;     // group this ext belongs to (for §3.5 FWD column), "" = none
        // SIP digest credential state (STAGE 3): true iff the SipSecretStore holds an
        // HA1 for this extension (it can authenticate). Drives the ◆ SECURED / · none
        // chip in the Extensions tab. Plumbed from SipSecretStore::hasSecret(ext).
        bool        secured = false;
    };

    // One ring/hunt group row (tui-style §3.6). `members` are the stored member
    // extensions in order; `ringAll` picks the plain-language mode word. `badMembers`
    // counts members that no longer map to a registered extension (the G6 integrity
    // flag → "⚠ n NOT AN EXTENSION").
    struct PbxGroup
    {
        std::string              name;       // group extension/name (e.g. "600")
        bool                     ringAll = true;  // true=Ring everyone, false=One at a time
        std::vector<std::string> members;
        int                      badMembers = 0;
    };

    struct PbxConfigSnapshot
    {
        std::vector<PbxExt>   extensions;  // registered extensions + feature state
        std::vector<PbxGroup> groups;      // ring/hunt groups
        int  maxExt    = 32;               // POCKETDIAL_MAX_CLIENTS (headroom cap)
        int  maxMembers = 32;              // POCKETDIAL_MAX_CLIENTS (group member cap)
        std::string adminExt = "101";      // always a valid pick (wizard [3/5])
    };

    // The live monitor snapshot — richer than LiveStats, fetched only while the
    // monitor screen is active (so the hub stays cheap). Everything here is a copy
    // taken off the SIP thread via RequestsHandler's snapshot getters + platform
    // heap/uptime, so reading it never blocks signaling.
    struct MonitorSnapshot
    {
        std::vector<CallRow>   calls;     // live + idle channels (up to maxCalls)
        std::vector<RosterRow> roster;    // registered + known-down extensions
        int      online      = 0;
        int      unreachable = 0;
        int      activeCalls = 0;
        int      maxCalls    = 8;
        int      memUsedPct  = 0;         // heap used % (real; vitals bar)
        int      poolUsed    = 0;         // == activeCalls (n/8 calls vitals)
        uint64_t uptimeSec   = 0;         // UP vitals clock
    };

    // Live registrar/system snapshot the hub + title bar read. Plain data so it is
    // trivial to fill from RequestsHandler off the SIP thread (all its getters are
    // snapshot-copied under their own mutex) and trivial to fake in a unit test.
    struct LiveStats
    {
        int      online      = 0;   // registered clients (● ONLINE)
        int      unreachable = 0;   // known-but-down (○ UNREACH); 0 until rosters exist
        int      activeCalls = 0;   // live sessions (n/8 calls)
        int      maxCalls    = 8;   // POCKETDIAL_MAX_SESSIONS ceiling
        int      extCount    = 0;   // provisioned extensions (ext n/32)
        int      maxExt      = 32;  // POCKETDIAL_MAX_CLIENTS ceiling
        uint64_t uptimeSec   = 0;   // for the title-bar clock (HH:MM:SS of uptime)
        bool     apMode      = true;  // legacy two-way tag (kept for compatibility)
        // Real network role for the hub status tail + banner mode word. From the
        // NVS 'storage'/'wifi_mode' value plumbed via SshServer::setNetInfo():
        //   1 = STATION (joined an AP)   2 = AP (own hotspot)   0 = SETUP (captive)
        // Rendered as the literal word STATION / AP / SETUP (never color-alone).
        uint8_t  netMode     = 2;     // default AP — matches apMode=true legacy
        bool     provisioned = false; // AdminAuth::isProvisioned() — banner branch
        std::string host = "drawbridge.local";  // identity block
        std::string ip   = "0.0.0.0";
        std::string mac  = "00:00:00:00:00:00";
        std::string fw   = "v3.0.0";
        std::string ssid = "";         // joined/served SSID ([2] NETWORK); "" → unset
        int      heapUsedPct = 0;      // real heap used % ([6] ABOUT vitals)
    };

    // ── [3]/TRUNK — PSTN trunk config surface ─────────────────────────────────
    // A display-safe flattening of the SIP-side TrunkConfig: the secret itself is
    // NEVER carried here (only the fact one is stored), so the renderer cannot
    // echo it even by accident. `connected` is the live anchor state; `useLoopback`
    // selects the mock loopback anchor (no PSTN) vs the live trunk.
    struct TrunkInfo
    {
        std::string baseUrl;          // https://pbx.example.com:5001 ("" = unset)
        std::string clientId;         // trunk API client id ("" = unset)
        std::string sourceDn;         // the DN the device originates calls as
        bool secretSet   = false;     // a client secret is stored (◆ SET / · none)
        bool useLoopback = true;      // true = loopback mock anchor (no PSTN)
        bool connected   = false;     // live anchor connection state
    };

    using WriteFn    = std::function<void(const char*, size_t)>;
    using StatsFn    = std::function<LiveStats()>;
    using MonitorFn  = std::function<MonitorSnapshot()>;
    using ReportsFn  = std::function<ReportsSnapshot()>;
    using SecurityFn = std::function<SecurityInfo()>;
    // Apply a PIN change (current, new): returns "" on success or a short error to
    // show inline (e.g. "current PIN is wrong", "PIN too short"). Wired by SshServer
    // to AdminAuth::verifyPin + setPin; keeps Tui free of the crypto dependency so
    // the host renderer test links only Tui.cpp.
    using PinChangeFn = std::function<std::string(const std::string&, const std::string&)>;
    // Toggle SSH access ([4]/[K]): receives the desired enabled state. Wired by
    // SshServer to setEnabled(); host stub no-ops.
    using SshToggleFn = std::function<void(bool)>;

    // ── [3] PBX CONFIG providers + action callbacks ───────────────────────────
    // The snapshot the five tabs read (extensions + groups + caps). Fetched only
    // while the PBX screen is active. Absent → an empty-but-honest panel.
    using PbxConfigFn = std::function<PbxConfigSnapshot()>;
    // Toggle DND for an extension ([3]/Forwards [Space]). Wired to setDnd(ext,on).
    using DndSetFn    = std::function<void(const std::string& ext, bool on)>;
    // Set one forward trigger ("always"/"busy"/"noanswer") for an extension; an
    // empty target clears it. Wired to setForward(ext,trigger,target). The target is
    // a bare extension. NOTE: forward-to-ring-group ("grp:<name>") is NOT yet wired
    // in the SIP layer (redirectInvite→findClient does not unwrap the token), so the
    // picker offers extensions only; group forward targets are a tracked follow-up.
    using ForwardSetFn = std::function<void(const std::string& ext,
                                            const std::string& trigger,
                                            const std::string& target)>;
    // Replace a ring group's membership + mode (members CSV, "ringall"|"hunt"); an
    // empty member list deletes the group. Wired to setRingGroup(name,members,mode).
    // Returns "" on success or a short operator-terse error to show inline.
    using RingGroupSetFn = std::function<std::string(const std::string& name,
                                                     const std::string& membersCsv,
                                                     const std::string& mode)>;

    // ── [4]/[D] REGISTRAR · DEVICES providers + action callbacks (STAGE 3) ────
    // The registrar snapshot the Devices screen reads. Fetched only while that screen
    // (or the Extensions tab's secret state) is active. Absent → Open + empty list.
    using RegistrarFn = std::function<RegistrarInfo()>;
    // Change the registrar admission mode ([4]/[D]/[M] picker). Wired to
    // RequestsHandler::setRegistrarMode; host stub no-ops. Runtime + NVS-persisted.
    using RegModeSetFn = std::function<void(RegMode mode)>;
    // Assign/rotate a per-extension SIP secret (never echoed). Returns "" on success or
    // a short operator-terse error to show inline. Wired to SipSecretStore::setSecret.
    // An empty secret is rejected by the store (the modal also guards length locally).
    using SecretSetFn = std::function<std::string(const std::string& ext,
                                                  const std::string& secret)>;
    // Promote a learned device to Secured (MAC-lock + digest-enforced). Accepts a MAC
    // or an extension. Returns true on success. Wired to RequestsHandler::secureDevice.
    using DeviceSecureFn = std::function<bool(const std::string& macOrExt)>;
    // Forget a device entirely (guarded). Accepts a MAC or an extension. Returns true
    // on success. Wired to RequestsHandler::forgetDevice. Host stub returns false.
    using DeviceForgetFn = std::function<bool(const std::string& macOrExt)>;

    // ── [3]/TRUNK providers + apply (mirrors the PbxConfigFn/RingGroupSetFn pattern) ─
    // Snapshot the Trunk tab reads. Absent → an honest "(unset)" loopback panel.
    using TrunkFn = std::function<TrunkInfo()>;
    // Apply the trunk editor: (baseUrl, clientId, secret, sourceDn, useLoopback).
    // An EMPTY secret means "keep the existing secret" (the wiring overlays it onto
    // the stored config). Returns "" on success ("applies on next reboot") or a
    // short operator-terse error to show inline.
    using TrunkSetFn = std::function<std::string(const std::string& baseUrl,
                                                 const std::string& clientId,
                                                 const std::string& secret,
                                                 const std::string& sourceDn,
                                                 bool useLoopback)>;

    Tui() = default;

    // Wire the byte sink (transport send). Required before begin().
    void setWriter(WriteFn w) { _write = std::move(w); }
    // Wire the live-data source. Optional; absent → zeros with a correct spine.
    void setStatsProvider(StatsFn s) { _stats = std::move(s); }
    // Wire the live-monitor data source. Optional; absent → an all-idle monitor
    // with a correct spine (the screen still draws, just empty).
    void setMonitorProvider(MonitorFn m) { _monitor = std::move(m); }
    // Wire the [5] REPORTS CDR source. Optional; absent → an empty (but honest)
    // recent-calls view with a correct spine.
    void setReportsProvider(ReportsFn r) { _reports = std::move(r); }
    // Wire the [4] SECURITY facts source. Optional; absent → defaults (unprovisioned,
    // SSH on) with a correct spine.
    void setSecurityProvider(SecurityFn s) { _security = std::move(s); }
    // Wire the [4]/[P] change-PIN apply action. Optional; absent → the modal reports
    // "not available on this build" instead of mutating anything.
    void setPinChanger(PinChangeFn p) { _pinChanger = std::move(p); }
    // Wire the [4]/[K] SSH access toggle action. Optional; absent → the key is a no-op.
    void setSshToggle(SshToggleFn s) { _sshToggle = std::move(s); }
    // Wire the [3] PBX CONFIG snapshot source. Optional; absent → an empty panel
    // (the tabs still draw, just with no rows) with a correct spine.
    void setPbxConfigProvider(PbxConfigFn p) { _pbxConfig = std::move(p); }
    // Wire the [3]/Forwards [Space] DND toggle. Optional; absent → the key no-ops.
    void setDndSetter(DndSetFn d) { _dndSet = std::move(d); }
    // Wire the [3]/Forwards forward-editor apply. Optional; absent → the editor
    // reports "not available on this build" instead of mutating anything.
    void setForwardSetter(ForwardSetFn f) { _forwardSet = std::move(f); }
    // Wire the [3]/Ring Groups create/edit/delete apply. Optional; absent → the
    // editor reports "not available on this build".
    void setRingGroupSetter(RingGroupSetFn r) { _ringGroupSet = std::move(r); }

    // ── [4]/[D] REGISTRAR · DEVICES wiring (STAGE 3) ──────────────────────────
    // Wire the registrar snapshot source. Optional; absent → Open mode + empty list
    // (the screen still draws, just with no devices) with a correct spine.
    void setRegistrarProvider(RegistrarFn r) { _registrar = std::move(r); }
    // Wire the registrar mode-change action ([4]/[D]/[M]). Optional; absent → the
    // mode picker reports "not available on this build" instead of mutating anything.
    void setRegModeSetter(RegModeSetFn r) { _regModeSet = std::move(r); }
    // Wire the assign/rotate-secret action ([4]/[D]/[A] and Extensions [S]). Optional;
    // absent → the secret modal reports "not available on this build".
    void setSecretSetter(SecretSetFn s) { _secretSet = std::move(s); }
    // Wire the secure-device action ([4]/[D]/[S]). Optional; absent → the key no-ops.
    void setDeviceSecurer(DeviceSecureFn d) { _deviceSecure = std::move(d); }
    // Wire the forget-device action ([4]/[D]/[F], guarded). Optional; absent → no-op.
    void setDeviceForgetter(DeviceForgetFn d) { _deviceForget = std::move(d); }
    // Wire the [3]/TRUNK snapshot source. Optional; absent → an honest unset panel.
    void setTrunkProvider(TrunkFn t) { _trunk = std::move(t); }
    // Wire the [3]/TRUNK editor apply. Optional; absent → the editor reports
    // "not available on this build" instead of mutating anything.
    void setTrunkSetter(TrunkSetFn t) { _trunkSet = std::move(t); }

    // Terminal geometry from the pty-req (SshServer::terminalCols/Rows). The
    // renderer is authored at the 80×24 floor and SCALES UP with the pty: the
    // frame derives from fcols()/frows() (clamped 80–132 × 24–50), so a larger
    // terminal gets a wider frame and a taller body. cols/rows of 0 → 80×24.
    void setSize(uint16_t cols, uint16_t rows);

    // Re-render the CURRENT screen (clear + full draw) without changing any state.
    // Called by the transport after a live resize (window-change) so the frame
    // re-fits the new geometry. Safe on any screen, including modals.
    void redraw();

    // Rendering tiers (renderer contract, tui-style §6.1):
    //   unicode=true  → box-drawing + status glyphs;  false → ASCII fallback map.
    //   color=true    → 16-color SGR;                 false → no SGR (mono).
    // Defaults: full fidelity (unicode + color). A --no-color/TERM=dumb client
    // calls setColor(false) (and usually setUnicode(false)) before begin().
    void setUnicode(bool on) { _unicode = on; }
    void setColor(bool on)   { _color = on; }
    void setTheme(Theme t)   { _theme = t; }
    Theme theme() const { return _theme; }

    // ── Session lifecycle ────────────────────────────────────────────────────
    // Draw the post-connect banner then the hub. Call once after the pty is up.
    void begin();
    // Feed raw input bytes from the channel. Returns false once the session
    // should end (the operator pressed [L] logout); SshServer then tears down.
    bool feed(const char* data, size_t len);
    bool feedByte(unsigned char c);
    // True until logout/disconnect; mirrors feed()'s return.
    bool running() const { return _running; }

    // Repaint live cells (clock + hub headroom). Safe to call ~1 Hz from the
    // session loop; in B2b-1 this is optional (the loop is input-driven), but the
    // hook exists so the next increment's 1 Hz monitor drops in without rework.
    // No-op (beyond a possible clock tick) unless on a live screen.
    void tickLive();

    // ── Logical keys (decoded from raw bytes incl. escape sequences) ─────────
    enum class Key {
        None, Char, Enter, Backspace, Esc, CtrlL,
        Up, Down, Left, Right, Tab
    };

    // Screen-router states. B2b-1: BANNER → HUB → placeholders/help/confirm.
    // B2b-2 adds Monitor (the [1] live wallboard). B2b-3 turns the [2]/[4]/[5]/[6]
    // placeholders into real screens plus their modals:
    //   Network/Security/Reports/About — the four hub destinations.
    //   ModeConfirm  — [2]/[M] guarded Wi-Fi mode switch (reboots; §3.13 shell).
    //   FactoryConfirm — [4]/[X] factory reset, DOUBLE-confirmed (§3.10 note / IA §2).
    //   ChangePin    — [4]/[P] never-echoed PIN entry modal (§3.10.1).
    //   CdrDetail    — [5] Enter on a CDR row → call-detail modal (§3.11.2).
    // B2b-4 adds the [3] PBX CONFIG tabbed panel (PbxConfig) and its modals:
    //   PbxAddMenu    — Extensions [A] → single | range submenu (§3.5.1)
    //   PbxAddSingle  — Extensions add-single form (§3.5.2; honest stub)
    //   PbxAddRange   — Extensions range-batch form (§3.5.3; honest stub)
    //   PbxGroupEdit  — Ring Groups create/edit editor (§3.6.1/§3.6.2)
    //   PbxGroupDelete— Ring Groups [D] guarded delete confirm (§2.8)
    //   PbxForwardEdit— Forwards/DND [Enter] CFU/CFB/CFNA editor (§3.7.1)
    //   PbxForwardPick— Forward target picker: extensions only (§3.7.2; ring groups
    //                    excluded until SIP resolves a group forward target)
    //   PbxIvrEdit    — IVR digit editor (§3.8.1; honest stub)
    enum class Screen {
        Banner, Hub, Help, RebootConfirm, Placeholder, Monitor,
        Network, Security, Reports, About,
        ModeConfirm, FactoryConfirm, ChangePin, CdrDetail,
        PbxConfig, PbxAddMenu, PbxAddSingle, PbxAddRange,
        PbxGroupEdit, PbxGroupDelete, PbxForwardEdit, PbxForwardPick, PbxIvrEdit,
        // STAGE 3 — registrar/devices surface + its modals:
        //   Devices         — [4]/[D] registrar mode + adopted-device roster (§ new)
        //   RegModePick     — [4]/[D]/[M] registrar mode chooser (Standalone/Learn/New)
        //   SecretEntry     — [4]/[D]/[A] + Extensions [S] never-echoed secret modal
        //   DeviceForget    — [4]/[D]/[F] guarded forget-device confirm
        Devices, RegModePick, SecretEntry, DeviceForget,
        // TRUNK + first-run:
        //   PbxTrunkEdit — [3]/TRUNK [E] editor modal (never-echoed secret)
        //   FirstRun     — unprovisioned banner routes here: numbered setup steps
        PbxTrunkEdit, FirstRun,
        // Easter eggs (Tier-2, pull-only, summoned from the : command palette at the hub):
        //   DrawbridgeEgg — :drawbridge → portcullis ASCII + commissioning facts
        //   OperatorCard  — :operator   → night-shift switchboard card
        DrawbridgeEgg, OperatorCard
    };

    // The active PBX tab (tui-style §2.5 tab strip). Public so host tests can assert
    // tab switching without reaching into private state.
    enum class PbxTab { Extensions, RingGroups, Forwards, Ivr, Features, Trunk };
    PbxTab pbxTab() const { return _pbxTab; }
    Screen screen() const { return _screen; }
    // True while the monitor's 1 Hz repaint is frozen ([F]); test/observability hook.
    bool monitorFrozen() const { return _monFrozen; }

    // Display width in terminal COLUMNS of a (possibly SGR-colored, multibyte-glyph)
    // string — the single source of truth for every framed-row pad/right-border calc
    // (see the .cpp for the rationale). Public + static so host tests can assert it
    // directly; it touches no instance state.
    static int dispWidth(const std::string& s);

private:
    // ── Frame geometry (derived from the pty size; 80×24 is the floor) ────────
    // fcols()/frows() clamp the pty geometry to the supported envelope; every
    // frame/pad calculation routes through them so a resize re-fits the spine.
    int fcols() const
    {
        int c = _cols ? _cols : 80;
        if (c < 80) c = 80;
        if (c > 132) c = 132;
        return c;
    }
    int frows() const
    {
        int r = _rows ? _rows : 24;
        if (r < 24) r = 24;
        if (r > 50) r = 50;
        return r;
    }
    // The body-row budget between the 3-row title spine and the 3-row footer.
    // 18 at the 80×24 floor; grows row-for-row with a taller terminal.
    int bodyRows() const { return frows() - 6; }

    // ── ANSI primitives (emit through _write; honor _color) ──────────────────
    void put(const char* s);
    void put(const std::string& s);
    void putn(const char* s, size_t n);
    void clearScreen();
    void home();
    void moveTo(int row, int col);
    void hideCursor();
    void showCursor();
    void sgrReset();
    void sgr(const char* code);           // raw "ESC[<code>m" if color on

    // Semantic color roles (resolve per theme; no-op when _color is off). Each
    // wraps the NEXT put with the role's SGR then a reset — callers pass the text.
    enum class Role { Border, Text, Header, Lamp, Dim, Dnd, Alert };
    const char* sgrFor(Role r) const;     // the bare SGR digits for this role+theme
    void roled(Role r, const std::string& text);  // SGR(role) + text + reset

    // ── Glyph table (one indirection → free mono degradation, tui-style §6.2) ─
    enum class Glyph {
        Online, Unreach, Ringing, Active, Dnd, Fwd, Alert, Ready,
        BoxTL, BoxTR, BoxBL, BoxBR, BoxH, BoxV, BoxVR, BoxVL,
        Marker, Sigil, JackDot, WordmarkL, WordmarkR
    };
    const char* glyph(Glyph g) const;     // unicode or ASCII per _unicode

    // ── Components (assembled from primitives; geometry per tui-style §2) ─────
    void drawSpineTop(const char* mode, const LiveStats& st);   // row 1 title bar
    void drawFooter(const std::string& keys);                   // row 24 key-hint
    std::string themeLabel() const;       // "Theme: BRASS ▸" / "PHOSPHOR ▸"
    void statusChip(Glyph g, Role r, const char* label);        // glyph+LABEL+color

    // Resolve a status-lexicon State → its (glyph, role, label) so the monitor maps
    // state in ONE place. Appends a colored "glyph LABEL" chip to `out`, advancing
    // the visible width `vis`. label==nullptr uses the state's canonical label.
    void appendStateChip(State s, std::string& out, int& vis,
                         const char* labelOverride = nullptr);
    // Visible column width of a State chip ("● ONLINE" etc.) for layout math.
    int  stateChipVis(State s, const char* labelOverride = nullptr) const;

    // A State → plain (uncoloured) "glyph LABEL" string, for use inside a reverse-video
    // selected row where an inner colour reset would break the highlight span.
    std::string stateLabelPlain(State s) const;

    // A 10-cell vitals bar (tui-style §2.10): "[██████░░░░] 61%". The numeric label
    // is authoritative; the bar reinforces (fill=accent, empty=dim). Appends to out.
    void appendVitalsBar(int pct, std::string& out, int& vis);

    // Center-line helper: pad `text` (visible width `vis`) to 80 and emit.
    void centerLine(const std::string& text, int vis);


    // ── Screens ──────────────────────────────────────────────────────────────
    void renderBanner();
    void renderHub();
    void renderHelp();
    void renderRebootConfirm();
    void renderPlaceholder();
    void renderMonitor();                 // [1] live wallboard (clear + full draw)
    void drawMonitorFrame();              // emit the whole monitor from the cursor
    void repaintHubLive();                // clock + headroom only (no full clear)
    void repaintMonitorLive();            // home + redraw frame (no full clear)
    void renderNetwork();                 // [2] NETWORK status + guarded mode switch
    void renderSecurity();                // [4] SECURITY admin/session + actions
    void renderReports();                 // [5] REPORTS — Recent Calls / Event Log
    void renderAbout();                   // [6] ABOUT honesty card
    void renderModeConfirm();             // [2]/[M] Wi-Fi mode switch confirm (reboots)
    void renderFactoryConfirm();          // [4]/[X] factory reset DOUBLE-confirm
    void renderChangePin();               // [4]/[P] never-echoed PIN entry modal
    void renderCdrDetail();               // [5] Enter → CDR detail modal

    // ── STAGE 3: registrar/devices surface + modals ───────────────────────────
    void renderDevices();                 // [4]/[D] registrar mode + adopted devices
    void renderRegModePick();             // [4]/[D]/[M] registrar mode chooser
    void renderSecretEntry();             // never-echoed secret assign/rotate modal
    void renderDeviceForget();            // [4]/[D]/[F] guarded forget confirm

    // ── [3] PBX CONFIG tabbed panel (B2b-4) ───────────────────────────────────
    void renderPbxConfig();               // dispatch to the active tab body
    void drawPbxTabStrip(int& bodyUsed);  // the shared tab strip + underline (§2.5)
    void renderPbxExtensions(int& bodyUsed);  // Extensions tab body (§3.5)
    void renderPbxRingGroups(int& bodyUsed);  // Ring Groups tab body (§3.6)
    void renderPbxForwards(int& bodyUsed);    // Forwards/DND tab body (§3.7)
    void renderPbxIvr(int& bodyUsed);         // IVR tab body (§3.8)
    void renderPbxFeatures(int& bodyUsed);    // Features tab body (§3.9)
    void renderPbxTrunk(int& bodyUsed);       // TRUNK tab body (PSTN trunk config)
    void renderPbxTrunkEdit();                // [3]/TRUNK [E] editor modal
    void renderFirstRun();                    // first-boot setup checklist screen
    void renderPbxAddMenu();              // §3.5.1 single|range submenu
    void renderPbxAddSingle();            // §3.5.2 add-single form (honest stub)
    void renderPbxAddRange();             // §3.5.3 range-batch form (honest stub)
    void renderPbxGroupEdit();            // §3.6.1/§3.6.2 group create/edit editor
    void renderPbxGroupDelete();          // §2.8 guarded group delete confirm
    void renderPbxForwardEdit();          // §3.7.1 CFU/CFB/CFNA editor
    void renderPbxForwardPick();          // §3.7.2 forward target picker
    void renderPbxIvrEdit();              // §3.8.1 IVR digit editor (honest stub)

    // Shared framed-body helpers used by the new full-frame screens (a single 3-zone
    // spine: blank rows + label/value rows, padded to the 18-row body). `key` rows
    // colour the [X] hotkey in the header role; the value side stays brass text.
    void drawConfirmBox(const char* title, const char* glyphLabel, Role glyphRole,
                        const std::vector<std::string>& lines,
                        const char* destructiveBtn, const char* safeBtn,
                        bool destructiveFocused, const char* keyLine);

    // ── Input handling per screen ────────────────────────────────────────────
    void onKeyHub(Key k, unsigned char ch);
    void onKeyHelp(Key k, unsigned char ch);
    void onKeyConfirm(Key k, unsigned char ch);
    void onKeyPlaceholder(Key k, unsigned char ch);
    void onKeyMonitor(Key k, unsigned char ch);
    void onKeyNetwork(Key k, unsigned char ch);
    void onKeySecurity(Key k, unsigned char ch);
    void onKeyReports(Key k, unsigned char ch);
    void onKeyAbout(Key k, unsigned char ch);
    void onKeyModeConfirm(Key k, unsigned char ch);
    void onKeyFactoryConfirm(Key k, unsigned char ch);
    void onKeyChangePin(Key k, unsigned char ch);
    void onKeyCdrDetail(Key k, unsigned char ch);
    // ── STAGE 3: registrar/devices surface + modals ───────────────────────────
    void onKeyDevices(Key k, unsigned char ch);
    void onKeyRegModePick(Key k, unsigned char ch);
    void onKeySecretEntry(Key k, unsigned char ch);
    void onKeyDeviceForget(Key k, unsigned char ch);
    // [3] PBX CONFIG input handlers (one per screen, like the rest of the engine).
    void onKeyPbxConfig(Key k, unsigned char ch);
    void onKeyPbxAddMenu(Key k, unsigned char ch);
    void onKeyPbxAddSingle(Key k, unsigned char ch);
    void onKeyPbxAddRange(Key k, unsigned char ch);
    void onKeyPbxGroupEdit(Key k, unsigned char ch);
    void onKeyPbxGroupDelete(Key k, unsigned char ch);
    void onKeyPbxForwardEdit(Key k, unsigned char ch);
    void onKeyPbxForwardPick(Key k, unsigned char ch);
    void onKeyPbxIvrEdit(Key k, unsigned char ch);
    void onKeyPbxTrunkEdit(Key k, unsigned char ch);
    void onKeyFirstRun(Key k, unsigned char ch);
    // Easter egg screens.
    void renderDrawbridgeEgg();
    void onKeyDrawbridgeEgg(Key k, unsigned char ch);
    void renderOperatorCard();
    void onKeyOperatorCard(Key k, unsigned char ch);

    void gotoScreen(Screen s);
    void toggleTheme();
    LiveStats stats() const;              // call provider or return zeros
    MonitorSnapshot monitor() const;      // call provider or return an idle snapshot
    ReportsSnapshot reports() const;      // call provider or return an empty CDR view
    SecurityInfo    security() const;     // call provider or return safe defaults
    PbxConfigSnapshot pbxConfig() const;  // call provider or return an empty panel
    RegistrarInfo   registrar() const;    // call provider or return Open + empty list
    TrunkInfo       trunk() const;        // call provider or return an unset loopback

    // Build the forward-target picker list (every registered extension) plus the
    // trailing "(clear this forward)" option, from the current snapshot. Each entry
    // is {token, label}: token is "" for clear or a bare ext; label is the rendered
    // row text. Ring groups are intentionally excluded — the SIP layer cannot resolve
    // a "grp:<name>" forward target yet (see Tui.cpp), so offering it would be
    // dishonest. Built fresh each open so it tracks the live roster.
    struct PickEntry { std::string token, label; State state = State::Online; bool isGroup = false; };
    std::vector<PickEntry> buildForwardPickList(const PbxConfigSnapshot& cfg) const;

    // Perform the [2]/[M] guarded action: write NVS 'storage'/'wifi_mode' (toggling
    // STATION↔AP) and esp_restart() — mirrors cmd_set_topology. ESP-only; host returns
    // home. Confirmed from onKeyModeConfirm() on [y].
    void applyModeSwitch();
    // Perform the [4]/[X] guarded action: nvs_flash_erase() + esp_restart(). ESP-only;
    // host returns home. Confirmed from onKeyFactoryConfirm() on the SECOND [y].
    void applyFactoryReset();

    // Format uptime seconds → "HH:MM:SS" (clamped; days fold into hours per spec
    // brevity — the title bar carries time-of-day-style HH:MM:SS of uptime).
    static std::string fmtClock(uint64_t sec);

    // ── State ────────────────────────────────────────────────────────────────
    WriteFn   _write;
    StatsFn   _stats;
    ReportsFn _reports;
    SecurityFn _security;
    PinChangeFn _pinChanger;
    SshToggleFn _sshToggle;
    PbxConfigFn    _pbxConfig;
    DndSetFn       _dndSet;
    ForwardSetFn   _forwardSet;
    RingGroupSetFn _ringGroupSet;
    // STAGE 3 registrar/devices wiring.
    RegistrarFn    _registrar;
    RegModeSetFn   _regModeSet;
    SecretSetFn    _secretSet;
    DeviceSecureFn _deviceSecure;
    DeviceForgetFn _deviceForget;
    // [3]/TRUNK wiring.
    TrunkFn        _trunk;
    TrunkSetFn     _trunkSet;
    bool     _running   = false;
    bool     _unicode   = true;
    bool     _color     = true;
    Theme    _theme     = Theme::Brass;
    Screen   _screen    = Screen::Banner;
    uint16_t _cols      = 80;
    uint16_t _rows      = 24;

    // Which hub item the placeholder is standing in for (1-6), for its title.
    int      _placeholderItem = 0;

    // : command palette (hub only). When _cmdMode is true the hub prompt slot
    // shows ":[_cmdBuf]_"; Enter routes known verbs to easter egg screens; Esc cancels.
    bool        _cmdMode = false;
    std::string _cmdBuf;

    // The screen Help/placeholder was entered FROM, so Esc returns to the origin
    // (tui-ia §3: "context help, scoped to current screen; Esc never leaves the
    // screen"). Set when routing into Help; defaults to Hub.
    Screen   _helpReturn = Screen::Hub;

    // Escape-sequence decoder state. A lone ESC is a logical Esc; ESC '[' 'A'..'D'
    // is an arrow. We disambiguate by a tiny state machine (no timeout needed for
    // a single feed() buffer; a bare trailing ESC is held and resolved on the next
    // byte or flushed as Esc when feed() finishes — see flushPendingEsc()).
    enum class Decode { Normal, GotEsc, GotCsi };
    Decode   _decode    = Decode::Normal;

    // Cached last-rendered live values so repaintHubLive() only rewrites cells
    // that changed (cursor-positioned overwrite, never a full clear — brief §6).
    std::string _lastClock;

    // ── Live monitor state ([1]) ─────────────────────────────────────────────
    MonitorFn _monitor;
    bool      _monFrozen = false;         // [F] freeze: pauses the 1 Hz repaint
    // Screen row where the call-matrix DATA begins, and how many channel rows it
    // spans, so repaintMonitorLive() can cursor-address exactly those cells.
    // Layout: row4 LIVE CALLS · row5 col-header · row6 rule · row7..10 = 4 channels.
    static constexpr int kMonMatrixRow = 7;   // first CH data row (1-based screen row)
    static constexpr int kMonMaxChans  = 4;   // channels shown in the matrix block
    // Screen rows for the live cells the badge/vitals repaint touches.
    static constexpr int kMonBadgeRow  = 4;   // "1/8 active · ⟳ 1 Hz" line
    static constexpr int kMonVitalsRow = 13;  // first VITALS bar row (CPU/MEM/UP)

    // ── [5] REPORTS state ────────────────────────────────────────────────────
    // Two views toggled with [Tab] (§3.11): Recent Calls (the CDR ring) and the
    // Event Log tail. `_cdrSel` is the highlighted CDR row (0-based, newest=0);
    // `_cdrTop` is the first visible row for paging a list taller than the body.
    bool _reportsEventLog = false;   // false = Recent Calls, true = Event Log
    int  _cdrSel = 0;
    int  _cdrTop = 0;
    // CDR rows visible in the body at once: 8 at the 24-row floor, growing
    // row-for-row with a taller terminal (the body budget grows the same way).
    int  cdrRows() const { return 8 + (frows() - 24); }

    // ── [4] SECURITY · Change-PIN modal ([P]) state (§3.10.1) ────────────────
    // Three never-echoed fields (current / new / confirm). `_pinField` is the
    // focused field (0..2); each holds the typed digits (rendered as bullets). A
    // transient result message (e.g. "PINs do not match") is shown until the next
    // keystroke. The action calls AdminAuth::verifyPin + setPin on Apply.
    int         _pinField = 0;
    std::string _pinCur, _pinNew, _pinConf;
    std::string _pinMsg;             // inline result/validation line ("" = none)
    // Screen to return to after the ChangePin modal closes: Security normally,
    // FirstRun when opened from the first-run checklist ([1] set the admin PIN).
    Screen      _pinReturn = Screen::Security;

    // ── Factory-reset DOUBLE-confirm step ([4]/[X]) ──────────────────────────
    // 0 = first confirm pending, 1 = first passed → second (final) confirm pending.
    int _factoryStep = 0;

    // ── [3] PBX CONFIG state (B2b-4) ─────────────────────────────────────────
    // The active tab + the per-list selection cursor. `_pbxSel` is reused by every
    // table tab (Extensions / Ring Groups / Forwards / IVR); it is clamped against the
    // active list in each render so switching tabs never lands out of range. `_pbxTop`
    // pages a list taller than the body.
    PbxTab _pbxTab = PbxTab::Extensions;
    int    _pbxSel = 0;
    int    _pbxTop = 0;
    // Table rows visible in the body at once: 8 at the 24-row floor, growing
    // with the terminal height like cdrRows().
    int    pbxRows() const { return 8 + (frows() - 24); }
    // The screen to return to after a modal closes (always PbxConfig today; kept
    // explicit so a future cross-link entry point can set a different origin).
    Screen _pbxReturn = Screen::PbxConfig;

    // Add-extension submenu focus (§3.5.1): 0 = Single, 1 = Range.
    int _pbxAddChoice = 0;

    // ── Ring Group editor state (§3.6.1 edit / §3.6.2 create) ────────────────
    // `_grpCreate` distinguishes the CREATE flow (empty box, < Create >) from EDIT
    // (pre-filled, < Apply >). `_grpName` is the (editable) group name; `_grpRingAll`
    // the mode radio; `_grpMembers`/`_grpChecked` are the parallel roster + tick state;
    // `_grpOrder` records the pick sequence (1-based) for Hunt order. `_grpFocus`:
    //   0 = Name, 1 = Mode, 2 = checklist, 3 = buttons. `_grpMemberSel` is the focused
    // checklist row. `_grpBtn` is the focused button (0=action,1=cancel). `_grpMsg` is
    // the inline guard line (e.g. "pick at least one extension").
    bool        _grpCreate = false;
    std::string _grpName;
    std::string _grpOrigName;        // the name being edited (for the setter key on rename)
    bool        _grpRingAll = true;
    std::vector<std::string> _grpMembers;   // candidate roster (ext numbers)
    std::vector<bool>        _grpChecked;    // parallel tick state
    std::vector<int>         _grpOrder;      // parallel hunt-order (0 = unpicked)
    int         _grpNextOrder = 1;           // next hunt-order number to assign
    int         _grpFocus = 0;
    int         _grpMemberSel = 0;
    int         _grpBtn = 0;
    std::string _grpMsg;
    // The ring-group DELETE victim, captured BY NAME when [D] is pressed (not by
    // _pbxSel index): the confirm screen and the y-handler each take an independent
    // pbxConfig() snapshot, so an index could point at a different row if the live
    // config shifted between reads. The name pins the victim across both snapshots.
    std::string _grpDelName;

    // ── Forward editor state (§3.7.1 / §3.7.2) ───────────────────────────────
    // `_fwdExt` is the extension being edited. `_fwdDnd` its DND toggle. The three
    // targets are the picker values ("" / ext / "grp:x"). `_fwdFocus`:
    //   0 = DND, 1 = CFU, 2 = CFB, 3 = CFNA, 4 = buttons. `_fwdBtn` focused button.
    // The picker (§3.7.2) reuses `_fwdPickSel` over the buildForwardPickList() rows,
    // and `_fwdPickField` records WHICH forward (1/2/3) the picker is choosing for.
    std::string _fwdExt;
    bool        _fwdDnd = false;
    std::string _fwdCfu, _fwdCfb, _fwdCfna;
    int         _fwdFocus = 1;       // open on CFU (the canonical "point at a group")
    int         _fwdBtn = 0;
    std::string _fwdMsg;
    int         _fwdPickSel = 0;
    int         _fwdPickField = 1;   // 1=CFU 2=CFB 3=CFNA

    // ── IVR digit-editor state (§3.8.1; honest stub) ─────────────────────────
    // The IVR has no persistent menu store yet (DTMF collection exists, no menu). The
    // editor renders the §3.8.1 layout over a fixed digit row but cannot persist; the
    // action radio focus is tracked so the layout is interactive/legible.
    char _ivrDigit = '1';
    int  _ivrAction = 0;             // 0=group 1=ext 2=prompt 3=unset (radio focus)

    // ── STAGE 3: registrar/devices surface state ─────────────────────────────
    // `_devSel`/`_devTop` are the device-roster selection cursor + page top, reused by
    // the Devices screen exactly like _pbxSel/_pbxTop. `_regPickSel` is the registrar-
    // mode chooser focus (0=Standalone 1=Learn 2=New). `_devForgetTarget` pins the
    // forget victim BY MAC across the confirm + apply snapshots (same discipline as
    // _grpDelName). `_devMsg` is the inline result line on the Devices screen.
    int         _devSel = 0;
    int         _devTop = 0;
    // Device rows visible in the body at once: 7 at the 24-row floor (so the
    // always-reserved result row keeps renderDevices within the body budget),
    // growing with the terminal height like cdrRows().
    int         devRows() const { return 7 + (frows() - 24); }
    int         _regPickSel = 0;
    std::string _devForgetTarget;        // MAC pinned at [F] time
    std::string _devForgetExt;           // its extension (for the confirm copy)
    std::string _devMsg;                 // inline result/guard line ("" = none)

    // Never-echoed secret-entry modal state (shared by [4]/[D]/[A] and Extensions [S]).
    // `_secretExt` is the target extension; `_secretBuf` the typed (never-echoed) chars;
    // `_secretMsg` the inline result/validation line. `_secretReturn` is the screen to
    // return to after Apply/Cancel (Devices or PbxConfig), so the modal is reusable.
    std::string _secretExt;
    std::string _secretBuf;
    std::string _secretMsg;
    Screen      _secretReturn = Screen::Devices;
    // Secret-entry bounds (operator-set passwords; the store also validates).
    static constexpr size_t kSecretMin = 6;
    static constexpr size_t kSecretMax = 32;

    // ── [3]/TRUNK editor state (PbxTrunkEdit modal) ───────────────────────────
    // `_trkFocus`: 0=Base URL 1=Client ID 2=Secret 3=Source DN 4=mode radio
    // 5=buttons. The secret field is NEVER echoed (bullets only); leaving it
    // empty means "keep the existing secret" — the wiring overlays it. `_trkMsg`
    // is the inline guard/error line inside the modal; `_trkResult` is the
    // result line shown back on the Trunk tab after a successful apply.
    std::string _trkUrl, _trkId, _trkSecret, _trkDn;
    bool        _trkLoopback = true;
    int         _trkFocus = 0;
    int         _trkBtn   = 0;       // 0 = Apply, 1 = Cancel (focus 5)
    std::string _trkMsg;             // inline modal guard line ("" = none)
    std::string _trkResult;          // Trunk tab result line ("" = none)
};
