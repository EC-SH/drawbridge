// Tui_test.cpp — host unit coverage for the ANSI sysop-terminal renderer
// (src/Helpers/Tui.cpp). The TUI is transport-agnostic by design: it writes
// through a std::function callback and is fed input bytes, so the desktop test
// simply collects everything written into a std::string and asserts on it. No
// wolfSSH, no ESP — this is exactly the seam SshServer wires on device.
//
// Coverage (per the B2b-1 acceptance notes):
//   * the banner contains the POCKET-DIAL nameplate + descriptor,
//   * the hub lists all six numbered destinations + R/L + the theme label,
//   * the mono (--no-color) tier emits NO SGR color escapes,
//   * the theme toggle flips the footer label BRASS ↔ PHOSPHOR,
//   * logout ([L]) ends the session.

#include <gtest/gtest.h>
#include "Tui.hpp"

#include <string>

namespace {

// A test harness: a Tui wired to a string sink, with a fixed live-stats snapshot
// so assertions on the headroom line are deterministic.
struct Harness {
    Tui tui;
    std::string out;

    Harness(bool unicode = true, bool color = true) {
        tui.setWriter([this](const char* d, size_t n) { out.append(d, n); });
        tui.setStatsProvider([] {
            Tui::LiveStats s;
            s.online = 4; s.unreachable = 1;
            s.activeCalls = 1; s.maxCalls = 8;
            s.extCount = 12; s.maxExt = 32;
            s.uptimeSec = 4 * 3600 + 2 * 60 + 17;  // 04:02:17
            s.apMode = true;
            return s;
        });
        // A fixed monitor snapshot: one active call (101 → 102), one ringing
        // (104 → grp:sales), a four-entry roster (incl. a DND), and real-ish vitals.
        tui.setMonitorProvider([] {
            Tui::MonitorSnapshot m;
            m.online = 4; m.unreachable = 1; m.maxCalls = 8;
            m.memUsedPct = 34; m.uptimeSec = 4 * 3600 + 2 * 60 + 17;
            m.calls.push_back({1, "101", "\xe2\x86\x92 102", 134, "PCMU", Tui::State::Active});
            m.calls.push_back({2, "104", "\xe2\x86\x92 grp:sales", 7, "PCMU", Tui::State::Ringing});
            m.activeCalls = 2; m.poolUsed = 2;
            m.roster.push_back({"101", "Maria", Tui::State::Online});
            m.roster.push_back({"102", "Sam",   Tui::State::Online});
            m.roster.push_back({"103", "Lee",   Tui::State::Unreach});
            m.roster.push_back({"106", "Warehouse", Tui::State::Dnd});
            return m;
        });
        // A fixed CDR snapshot for the [5] REPORTS views (newest first), exercising
        // every CdrResult chip so the lexicon assertions are deterministic.
        tui.setReportsProvider([] {
            Tui::ReportsSnapshot r;
            auto mk = [](const char* a, const char* b, uint64_t startMs,
                         uint32_t dur, Tui::CdrResult res) {
                Tui::CdrEntry e; e.caller = a; e.callee = b; e.startMs = startMs;
                e.durationSec = dur; e.result = res; e.codec = "PCMU";
                e.callId = "7f3a-b201"; return e;
            };
            r.cdr.push_back(mk("101", "102",        66249000, 134, Tui::CdrResult::Answered));
            r.cdr.push_back(mk("104", "grp:sales",  66110000,  48, Tui::CdrResult::Answered));
            r.cdr.push_back(mk("103", "105",        65942000,   0, Tui::CdrResult::Busy));
            r.cdr.push_back(mk("101", "200",        65731000,   0, Tui::CdrResult::Unavailable));
            r.cdr.push_back(mk("106", "104",        65478000,   0, Tui::CdrResult::Cancelled));
            r.cdr.push_back(mk("105", "101",        65324000,   0, Tui::CdrResult::Failed));
            return r;
        });
        // A fixed SECURITY snapshot: provisioned, SSH on, with a session peer.
        tui.setSecurityProvider([] {
            Tui::SecurityInfo s;
            s.provisioned = true; s.sshEnabled = true; s.adminExt = "100";
            s.sessionUser = "sysop"; s.sessionPeer = "192.168.4.23:51514";
            s.sinceClock = "04:02:41";
            return s;
        });
        // A fixed [3] PBX CONFIG snapshot: three registered extensions (one in DND,
        // one forwarded to a group), and two ring groups (one with a phantom member
        // for the G6 integrity flag). The setters record their last call so the
        // mutation tests can assert the wiring without a real RequestsHandler.
        tui.setPbxConfigProvider([this] {
            Tui::PbxConfigSnapshot c;
            c.maxExt = 32; c.maxMembers = 32; c.adminExt = "100";
            { Tui::PbxExt e; e.ext = "101"; e.state = Tui::State::Online; c.extensions.push_back(e); }
            { Tui::PbxExt e; e.ext = "102"; e.state = Tui::State::Online; e.dnd = true;
              e.cfu = "205"; c.extensions.push_back(e); }
            { Tui::PbxExt e; e.ext = "104"; e.state = Tui::State::Online;
              e.cfu = "grp:sales"; e.ringGroup = "sales"; c.extensions.push_back(e); }
            { Tui::PbxGroup g; g.name = "sales"; g.ringAll = true;
              g.members = {"101","104"}; g.badMembers = 0; c.groups.push_back(g); }
            { Tui::PbxGroup g; g.name = "support"; g.ringAll = false;
              g.members = {"101","104","200"}; g.badMembers = 1; c.groups.push_back(g); }
            (void)this;
            return c;
        });
        tui.setDndSetter([this](const std::string& ext, bool on) {
            lastDndExt = ext; lastDndOn = on; ++dndCalls;
        });
        tui.setForwardSetter([this](const std::string& ext, const std::string& trig,
                                    const std::string& tgt) {
            lastFwdExt = ext; lastFwdTrig = trig; lastFwdTarget = tgt; ++fwdCalls;
        });
        tui.setRingGroupSetter([this](const std::string& name, const std::string& mem,
                                      const std::string& mode) -> std::string {
            lastGrpName = name; lastGrpMembers = mem; lastGrpMode = mode; ++grpCalls;
            return "";
        });
        tui.setUnicode(unicode);
        tui.setColor(color);
    }

    // Mutation-callback capture (PBX wiring assertions).
    std::string lastDndExt, lastFwdExt, lastFwdTrig, lastFwdTarget;
    std::string lastGrpName, lastGrpMembers, lastGrpMode;
    bool lastDndOn = false;
    int  dndCalls = 0, fwdCalls = 0, grpCalls = 0;

    void clear() { out.clear(); }
    // Drive the banner past its "press any key" greeting into the hub.
    void toHub() {
        tui.begin();          // banner
        tui.feedByte(' ');    // any key → hub
    }
};

// True if `s` contains any SGR colour escape (ESC [ <digits> m). We accept the
// cursor/clear escapes (ESC[2J, ESC[H, ESC[<r>;<c>H, ESC[?25l) in mono mode but
// NOT colour SGR. The simplest robust check: no "\x1b[" followed by a digit and
// terminated by 'm' that isn't a pure reset is emitted when colour is off — in
// practice, mono mode must emit zero "\x1b[" + digit + "m" sequences at all.
bool hasColorSgr(const std::string& s) {
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        if (s[i] == 0x1b && s[i + 1] == '[') {
            size_t j = i + 2;
            bool sawDigit = false;
            while (j < s.size() && (isdigit((unsigned char)s[j]) || s[j] == ';')) {
                if (isdigit((unsigned char)s[j])) sawDigit = true;
                ++j;
            }
            if (j < s.size() && s[j] == 'm' && sawDigit) return true;  // an SGR
        }
    }
    return false;
}

} // namespace

// ── Banner ────────────────────────────────────────────────────────────────────

TEST(Tui, BannerContainsNameplateAndDescriptor) {
    Harness h;
    h.tui.begin();   // draws the banner
    // The FIGlet nameplate's last art row is unmistakable, and the descriptor line
    // carries SYSOP TERMINAL + the single-board SIP PBX sub-line.
    EXPECT_NE(h.out.find("|_|    \\___/ \\____|"), std::string::npos);
    EXPECT_NE(h.out.find("S Y S O P   T E R M I N A L"), std::string::npos);
    EXPECT_NE(h.out.find("single-board SIP PBX"), std::string::npos);
    EXPECT_NE(h.out.find("Authorized sysops only"), std::string::npos);
}

TEST(Tui, BannerMonoTierEmitsNoColor) {
    Harness h(/*unicode=*/false, /*color=*/false);
    h.tui.begin();
    EXPECT_FALSE(hasColorSgr(h.out));
    // The ASCII fallback frame uses '+'/'='/'|' for the box (brand §3.3).
    EXPECT_NE(h.out.find("+="), std::string::npos);
    // Meaning is preserved without colour: the descriptor word still reads.
    EXPECT_NE(h.out.find("S Y S O P   T E R M I N A L"), std::string::npos);
}

// ── Hub ───────────────────────────────────────────────────────────────────────

TEST(Tui, HubListsAllSixDestinations) {
    Harness h;
    h.toHub();
    EXPECT_NE(h.out.find("[1] SYSTEM MONITOR"), std::string::npos);
    EXPECT_NE(h.out.find("[2] NETWORK"), std::string::npos);
    EXPECT_NE(h.out.find("[3] PBX CONFIG"), std::string::npos);
    EXPECT_NE(h.out.find("[4] SECURITY"), std::string::npos);
    EXPECT_NE(h.out.find("[5] REPORTS/LOGS"), std::string::npos);
    EXPECT_NE(h.out.find("[6] ABOUT"), std::string::npos);
    EXPECT_NE(h.out.find("[R] REBOOT"), std::string::npos);
    EXPECT_NE(h.out.find("[L] LOGOUT"), std::string::npos);
}

TEST(Tui, HubHeadroomShowsRealCounts) {
    Harness h;
    h.toHub();
    // The fixed snapshot is 4 online / 1 unreach / 1-of-8 calls / 12-of-32 exts.
    EXPECT_NE(h.out.find("4 ONLINE"), std::string::npos);
    EXPECT_NE(h.out.find("1 UNREACH"), std::string::npos);
    EXPECT_NE(h.out.find("1/8 calls"), std::string::npos);
    EXPECT_NE(h.out.find("ext 12/32"), std::string::npos);
}

TEST(Tui, HubFooterNamesTheTheme) {
    Harness h;
    h.toHub();
    EXPECT_NE(h.out.find("Theme: BRASS"), std::string::npos);
}

TEST(Tui, HubMonoTierEmitsNoColor) {
    Harness h(/*unicode=*/false, /*color=*/false);
    h.toHub();
    EXPECT_FALSE(hasColorSgr(h.out));
    // Still fully legible: all six destinations present in the mono tier.
    EXPECT_NE(h.out.find("[1] SYSTEM MONITOR"), std::string::npos);
    EXPECT_NE(h.out.find("[6] ABOUT"), std::string::npos);
}

// ── Theme toggle ──────────────────────────────────────────────────────────────

TEST(Tui, ThemeTogglesBrassToPhosphor) {
    Harness h;
    h.toHub();
    EXPECT_EQ(h.tui.theme(), Tui::Theme::Brass);
    h.clear();
    h.tui.feedByte('T');                 // hub typeahead: toggle theme
    EXPECT_EQ(h.tui.theme(), Tui::Theme::Phosphor);
    EXPECT_NE(h.out.find("Theme: PHOSPHOR"), std::string::npos);
}

// ── Navigation + lifecycle ──────────────────────────────────────────────────

TEST(Tui, NumberKeyOpensScreenThenEscReturns) {
    Harness h;
    h.toHub();
    h.clear();
    h.tui.feedByte('3');                 // → PBX CONFIG (the real tabbed panel, B2b-4)
    EXPECT_EQ(h.tui.screen(), Tui::Screen::PbxConfig);
    EXPECT_NE(h.out.find("PBX CONFIG"), std::string::npos);
    // A lone Esc (no trailing '[') backs out to the hub.
    h.tui.feedByte(0x1b);
    h.tui.feed("", 0);                   // flush the pending bare-ESC
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Hub);
}

TEST(Tui, HelpOverlayReachableAndDismissable) {
    Harness h;
    h.toHub();
    h.clear();
    h.tui.feedByte('?');
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Help);
    EXPECT_NE(h.out.find("State key:"), std::string::npos);
    h.tui.feedByte(0x1b);
    h.tui.feed("", 0);
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Hub);
}

TEST(Tui, RebootOpensGuardedConfirm) {
    Harness h;
    h.toHub();
    h.clear();
    h.tui.feedByte('R');
    EXPECT_EQ(h.tui.screen(), Tui::Screen::RebootConfirm);
    EXPECT_NE(h.out.find("REBOOT now?"), std::string::npos);
    EXPECT_NE(h.out.find("Stay up"), std::string::npos);   // safe default present
    // 'n' (safe) backs out without rebooting (host build: just returns home).
    h.tui.feedByte('n');
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Hub);
}

TEST(Tui, LogoutEndsSession) {
    Harness h;
    h.toHub();
    EXPECT_TRUE(h.tui.running());
    bool alive = h.tui.feedByte('L');
    EXPECT_FALSE(alive);
    EXPECT_FALSE(h.tui.running());
}

// ── [1] SYSTEM MONITOR (B2b-2) ───────────────────────────────────────────────

TEST(Tui, MonitorOpensFromHubAndRendersWallboard) {
    Harness h;
    h.toHub();
    h.clear();
    h.tui.feedByte('1');                 // hub typeahead: [1] → live wallboard
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Monitor);
    // The three §3.3 blocks are present, by their authoritative labels (not color).
    EXPECT_NE(h.out.find("LIVE CALLS"), std::string::npos);
    EXPECT_NE(h.out.find("ROSTER"), std::string::npos);
    EXPECT_NE(h.out.find("VITALS"), std::string::npos);
    // The MONITOR mode tag is in the title bar and the live badge reads 1 Hz.
    EXPECT_NE(h.out.find("[ MONITOR ]"), std::string::npos);
    EXPECT_NE(h.out.find("active"), std::string::npos);
    // Footer keys per the locked scheme — and NOT [P] (no on-device PCAP, honesty).
    EXPECT_NE(h.out.find("[F] Freeze"), std::string::npos);
    EXPECT_NE(h.out.find("[C] Clear"), std::string::npos);
    EXPECT_EQ(h.out.find("[P]"), std::string::npos);
}

TEST(Tui, MonitorFreezeTogglesBadge) {
    Harness h;
    h.toHub();
    h.tui.feedByte('1');
    EXPECT_FALSE(h.tui.monitorFrozen());
    h.clear();
    h.tui.feedByte('F');                 // [F] freezes the 1 Hz repaint
    EXPECT_TRUE(h.tui.monitorFrozen());
    EXPECT_NE(h.out.find("FROZEN"), std::string::npos);
    h.tui.feedByte('F');                 // toggles back
    EXPECT_FALSE(h.tui.monitorFrozen());
}

TEST(Tui, MonitorEscReturnsToHub) {
    Harness h;
    h.toHub();
    h.tui.feedByte('1');
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Monitor);
    h.tui.feedByte(0x1b);                // bare Esc
    h.tui.feed("", 0);                   // flush the pending bare-ESC
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Hub);
}

TEST(Tui, MonitorMonoTierEmitsNoColor) {
    Harness h(/*unicode=*/false, /*color=*/false);
    h.toHub();
    h.tui.feedByte('1');
    EXPECT_FALSE(hasColorSgr(h.out));
    EXPECT_NE(h.out.find("LIVE CALLS"), std::string::npos);
    EXPECT_NE(h.out.find("VITALS"), std::string::npos);
}

// dispWidth counts terminal COLUMNS, not bytes: multibyte glyphs are 1 col and ANSI
// SGR runs are skipped. This is the helper that keeps every framed right rail flush.
TEST(Tui, DispWidthCountsColumnsNotBytes) {
    EXPECT_EQ(Tui::dispWidth("ABC"), 3);
    EXPECT_EQ(Tui::dispWidth("\xe2\x97\x8f ONLINE"), 8);   // ● (3 bytes) + " ONLINE" = 8 cols
    EXPECT_EQ(Tui::dispWidth("\xc2\xb7"), 1);               // · (2 bytes) = 1 col
    // An SGR-wrapped glyph measures by its visible text alone (escape run skipped).
    EXPECT_EQ(Tui::dispWidth("\x1b[93m\xe2\x97\x86\x1b[0m ACTIVE"), 8);  // ◆ + " ACTIVE"
}

// Arrow escape sequences decode without being treated as a bare Esc (they are
// no-ops on the hub for B2b-1, but must not log the operator out or crash).
TEST(Tui, ArrowSequenceDoesNotLogOut) {
    Harness h;
    h.toHub();
    EXPECT_TRUE(h.tui.feed("\x1b[A", 3));   // Up arrow
    EXPECT_TRUE(h.tui.running());
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Hub);
}

// ── [6] ABOUT (B2b-3) ────────────────────────────────────────────────────────

TEST(Tui, AboutOpensAndShowsHonestyCard) {
    Harness h;
    h.toHub();
    h.clear();
    h.tui.feedByte('6');
    EXPECT_EQ(h.tui.screen(), Tui::Screen::About);
    EXPECT_NE(h.out.find("[ ABOUT ]"), std::string::npos);
    EXPECT_NE(h.out.find("ESP32-S3"), std::string::npos);
    EXPECT_NE(h.out.find("32 extensions"), std::string::npos);
    EXPECT_NE(h.out.find("no SD card"), std::string::npos);
    EXPECT_NE(h.out.find("arrives in v2"), std::string::npos);
    // Esc returns to the hub.
    h.tui.feedByte(0x1b); h.tui.feed("", 0);
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Hub);
}

// ── [2] NETWORK (B2b-3) ──────────────────────────────────────────────────────

TEST(Tui, NetworkOpensAndModeSwitchIsGuarded) {
    Harness h;
    h.toHub();
    h.clear();
    h.tui.feedByte('2');
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Network);
    EXPECT_NE(h.out.find("[ NETWORK ]"), std::string::npos);
    EXPECT_NE(h.out.find("NETWORK STATUS"), std::string::npos);
    EXPECT_NE(h.out.find("Switch network mode"), std::string::npos);
    // [M] opens the guarded confirm (does NOT immediately reboot).
    h.tui.feedByte('M');
    EXPECT_EQ(h.tui.screen(), Tui::Screen::ModeConfirm);
    EXPECT_NE(h.out.find("MODE SWITCH"), std::string::npos);
    // 'n' (safe) cancels back to the network screen (host build does not restart).
    h.tui.feedByte('n');
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Network);
    // Esc returns to the hub.
    h.tui.feedByte(0x1b); h.tui.feed("", 0);
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Hub);
}

// ── [5] REPORTS (B2b-3) ──────────────────────────────────────────────────────

TEST(Tui, ReportsShowsCdrWithResultWords) {
    Harness h;
    h.toHub();
    h.clear();
    h.tui.feedByte('5');
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Reports);
    EXPECT_NE(h.out.find("[ REPORTS ]"), std::string::npos);
    EXPECT_NE(h.out.find("Recent Calls"), std::string::npos);
    // Results render in WORDS (the lexicon), not color alone.
    EXPECT_NE(h.out.find("answered"), std::string::npos);
    EXPECT_NE(h.out.find("busy"), std::string::npos);
    EXPECT_NE(h.out.find("unavailable"), std::string::npos);
    EXPECT_NE(h.out.find("cancelled"), std::string::npos);
    EXPECT_NE(h.out.find("failed"), std::string::npos);
    // [Enter] opens the CDR detail modal.
    h.clear();
    h.tui.feedByte('\r');
    EXPECT_EQ(h.tui.screen(), Tui::Screen::CdrDetail);
    EXPECT_NE(h.out.find("Call detail"), std::string::npos);
    EXPECT_NE(h.out.find("Codec"), std::string::npos);
    // Esc returns to the list, Esc again to the hub.
    h.tui.feedByte(0x1b); h.tui.feed("", 0);
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Reports);
    h.tui.feedByte(0x1b); h.tui.feed("", 0);
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Hub);
}

TEST(Tui, ReportsTabFlipsToEventLog) {
    Harness h;
    h.toHub();
    h.tui.feedByte('5');
    h.clear();
    h.tui.feedByte('\t');                 // [Tab] flips view
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Reports);
    EXPECT_NE(h.out.find("Event Log"), std::string::npos);
    // The Event Log footer names how to flip back.
    EXPECT_NE(h.out.find("Recent Calls"), std::string::npos);
}

// ── [4] SECURITY (B2b-3) ─────────────────────────────────────────────────────

TEST(Tui, SecurityOpensAndShowsAdminAccess) {
    Harness h;
    h.toHub();
    h.clear();
    h.tui.feedByte('4');
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Security);
    EXPECT_NE(h.out.find("[ SECURITY ]"), std::string::npos);
    EXPECT_NE(h.out.find("ADMIN ACCESS"), std::string::npos);
    EXPECT_NE(h.out.find("SET"), std::string::npos);          // PIN ● SET chip
    EXPECT_NE(h.out.find("Change admin PIN"), std::string::npos);
    EXPECT_NE(h.out.find("Factory reset"), std::string::npos);
    // Esc returns to the hub.
    h.tui.feedByte(0x1b); h.tui.feed("", 0);
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Hub);
}

TEST(Tui, SecurityChangePinNeverEchoesDigits) {
    Harness h;
    h.tui.setPinChanger([](const std::string&, const std::string&) {
        return std::string("");   // accept everything for the test
    });
    h.toHub();
    h.tui.feedByte('4');
    h.tui.feedByte('P');                  // open the change-PIN modal
    EXPECT_EQ(h.tui.screen(), Tui::Screen::ChangePin);
    h.clear();
    h.tui.feedByte('1'); h.tui.feedByte('2'); h.tui.feedByte('3'); h.tui.feedByte('4');
    // The raw digits must NEVER appear in the output (bullets only).
    EXPECT_EQ(h.out.find("1234"), std::string::npos);
    // Esc cancels back to the security screen.
    h.tui.feedByte(0x1b); h.tui.feed("", 0);
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Security);
}

TEST(Tui, SecurityFactoryResetIsDoubleConfirmed) {
    Harness h;
    h.toHub();
    h.tui.feedByte('4');
    h.tui.feedByte('X');                  // factory reset → first confirm
    EXPECT_EQ(h.tui.screen(), Tui::Screen::FactoryConfirm);
    EXPECT_NE(h.out.find("1 of 2"), std::string::npos);
    h.clear();
    h.tui.feedByte('y');                  // first yes → advances to second confirm
    EXPECT_EQ(h.tui.screen(), Tui::Screen::FactoryConfirm);
    EXPECT_NE(h.out.find("2 of 2"), std::string::npos);
    // Esc still cancels out of the second step (host build never erases).
    h.tui.feedByte(0x1b); h.tui.feed("", 0);
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Security);
}

TEST(Tui, NewScreensMonoTierEmitNoColor) {
    Harness h(/*unicode=*/false, /*color=*/false);
    h.toHub();
    for (char k : {'2', '4', '5', '6'}) {
        h.clear();
        h.tui.feedByte(k);
        EXPECT_FALSE(hasColorSgr(h.out)) << "screen key " << k << " emitted color";
        h.tui.feedByte(0x1b); h.tui.feed("", 0);   // back to hub for the next key
    }
}

// ── [3] PBX CONFIG tabbed panel ───────────────────────────────────────────────

TEST(Tui, PbxConfigOpensOnExtensionsTab) {
    Harness h;
    h.toHub();
    h.clear();
    h.tui.feedByte('3');
    EXPECT_EQ(h.tui.screen(), Tui::Screen::PbxConfig);
    EXPECT_EQ(h.tui.pbxTab(), Tui::PbxTab::Extensions);
    EXPECT_NE(h.out.find("[ PBX CONFIG ]"), std::string::npos);
    // The tab strip names all five tabs.
    EXPECT_NE(h.out.find("Extensions"), std::string::npos);
    EXPECT_NE(h.out.find("Ring Groups"), std::string::npos);
    EXPECT_NE(h.out.find("Forwards/DND"), std::string::npos);
    EXPECT_NE(h.out.find("IVR"), std::string::npos);
    EXPECT_NE(h.out.find("Features"), std::string::npos);
    // The registered roster renders.
    EXPECT_NE(h.out.find("101"), std::string::npos);
    // Esc returns to the hub.
    h.tui.feedByte(0x1b); h.tui.feed("", 0);
    EXPECT_EQ(h.tui.screen(), Tui::Screen::Hub);
}

TEST(Tui, PbxTabSwitchingCyclesTabs) {
    Harness h;
    h.toHub();
    h.tui.feedByte('3');
    EXPECT_EQ(h.tui.pbxTab(), Tui::PbxTab::Extensions);
    h.tui.feedByte('\t');   // → Ring Groups
    EXPECT_EQ(h.tui.pbxTab(), Tui::PbxTab::RingGroups);
    h.tui.feedByte('\t');   // → Forwards/DND
    EXPECT_EQ(h.tui.pbxTab(), Tui::PbxTab::Forwards);
    h.tui.feedByte('\t'); h.tui.feedByte('\t');   // → IVR → Features
    EXPECT_EQ(h.tui.pbxTab(), Tui::PbxTab::Features);
    h.tui.feedByte('\t');   // wraps → Extensions
    EXPECT_EQ(h.tui.pbxTab(), Tui::PbxTab::Extensions);
    // Left arrow wraps backward to Features.
    h.tui.feed("\x1b[D", 3);
    EXPECT_EQ(h.tui.pbxTab(), Tui::PbxTab::Features);
}

TEST(Tui, PbxFeaturesTabListsOnlyRealStarCodes) {
    Harness h;
    h.toHub();
    h.tui.feedByte('3');
    // Jump to the Features tab (4 Tabs forward) and clear, then redraw.
    for (int i = 0; i < 4; ++i) h.tui.feedByte('\t');
    h.clear();
    h.tui.feed("\x0c", 1);   // Ctrl-L redraw
    EXPECT_EQ(h.tui.pbxTab(), Tui::PbxTab::Features);
    for (const char* code : {"*60", "*80", "*72", "*73", "*69", "*11"})
        EXPECT_NE(h.out.find(code), std::string::npos) << "missing " << code;
    // No invented codes (e.g. *97 voicemail) appear.
    EXPECT_EQ(h.out.find("*97"), std::string::npos);
}

TEST(Tui, PbxForwardsSpaceTogglesDndWired) {
    Harness h;
    h.toHub();
    h.tui.feedByte('3');
    h.tui.feedByte('\t'); h.tui.feedByte('\t');   // → Forwards/DND
    EXPECT_EQ(h.tui.pbxTab(), Tui::PbxTab::Forwards);
    // Row 0 = ext 101 (dnd=false). Space flips it on via the wired setter.
    h.tui.feedByte(' ');
    EXPECT_EQ(h.dndCalls, 1);
    EXPECT_EQ(h.lastDndExt, "101");
    EXPECT_TRUE(h.lastDndOn);
}

TEST(Tui, PbxForwardEditorAppliesForwards) {
    Harness h;
    h.toHub();
    h.tui.feedByte('3');
    h.tui.feedByte('\t'); h.tui.feedByte('\t');   // → Forwards/DND
    h.tui.feed("\x1b[B", 3);                       // ↓ to ext 102 (row 1)
    h.tui.feedByte('\r');                           // Enter → forward editor
    EXPECT_EQ(h.tui.screen(), Tui::Screen::PbxForwardEdit);
    EXPECT_NE(h.out.find("ext 102"), std::string::npos);
    // Enter applies: DND + all three triggers are pushed (102 has cfu=205).
    h.tui.feedByte('\r');
    EXPECT_GE(h.fwdCalls, 3);
    EXPECT_EQ(h.lastFwdExt, "102");
    EXPECT_EQ(h.tui.screen(), Tui::Screen::PbxConfig);
}

TEST(Tui, PbxForwardPickerLandsGroupTarget) {
    Harness h;
    h.toHub();
    h.tui.feedByte('3');
    h.tui.feedByte('\t'); h.tui.feedByte('\t');   // → Forwards/DND
    h.tui.feedByte('\r');                           // Enter on ext 101 → editor (CFU focus)
    EXPECT_EQ(h.tui.screen(), Tui::Screen::PbxForwardEdit);
    h.tui.feedByte(' ');                            // Space opens the CFU picker
    EXPECT_EQ(h.tui.screen(), Tui::Screen::PbxForwardPick);
    // The picker lists the RING GROUPS section.
    EXPECT_NE(h.out.find("RING GROUPS"), std::string::npos);
    EXPECT_NE(h.out.find("grp:sales"), std::string::npos);
}

TEST(Tui, PbxRingGroupCreateRequiresAMember) {
    Harness h;
    h.toHub();
    h.tui.feedByte('3');
    h.tui.feedByte('\t');                  // → Ring Groups
    EXPECT_EQ(h.tui.pbxTab(), Tui::PbxTab::RingGroups);
    h.tui.feedByte('A');                   // create flow
    EXPECT_EQ(h.tui.screen(), Tui::Screen::PbxGroupEdit);
    // Type a name (focus starts on Name field).
    for (char c : {'o','p','s'}) h.tui.feedByte(c);
    // Enter with zero members → guarded, stays on the editor, no setter call.
    h.tui.feedByte('\r');
    EXPECT_EQ(h.tui.screen(), Tui::Screen::PbxGroupEdit);
    EXPECT_EQ(h.grpCalls, 0);
    // Move to the checklist (Tab x2: Name→Mode→checklist), tick one member, apply.
    h.tui.feedByte('\t'); h.tui.feedByte('\t');
    h.tui.feedByte(' ');                   // tick the first roster member
    h.tui.feedByte('\r');                  // Enter → applies
    EXPECT_EQ(h.grpCalls, 1);
    EXPECT_EQ(h.lastGrpName, "ops");
    EXPECT_FALSE(h.lastGrpMembers.empty());
    EXPECT_EQ(h.lastGrpMode, "ringall");
    EXPECT_EQ(h.tui.screen(), Tui::Screen::PbxConfig);
}

TEST(Tui, PbxRingGroupDeleteIsGuardedAndWired) {
    Harness h;
    h.toHub();
    h.tui.feedByte('3');
    h.tui.feedByte('\t');                  // → Ring Groups
    h.tui.feedByte('D');                   // delete the selected group (sales)
    EXPECT_EQ(h.tui.screen(), Tui::Screen::PbxGroupDelete);
    // Safe default focused: Enter cancels (no setter call).
    h.tui.feedByte('\r');
    EXPECT_EQ(h.tui.screen(), Tui::Screen::PbxConfig);
    EXPECT_EQ(h.grpCalls, 0);
    // Re-open and confirm with 'y' → setRingGroup(name, "", ...) deletes it.
    h.tui.feedByte('D');
    h.tui.feedByte('y');
    EXPECT_EQ(h.grpCalls, 1);
    EXPECT_EQ(h.lastGrpName, "sales");
    EXPECT_TRUE(h.lastGrpMembers.empty());
}

TEST(Tui, PbxAddExtensionFlowIsAnHonestStub) {
    Harness h;
    h.toHub();
    h.tui.feedByte('3');                   // Extensions tab (default)
    h.tui.feedByte('A');                   // add submenu
    EXPECT_EQ(h.tui.screen(), Tui::Screen::PbxAddMenu);
    h.tui.feedByte('\r');                  // Single (default focus)
    EXPECT_EQ(h.tui.screen(), Tui::Screen::PbxAddSingle);
    // The stub note is explicit (no fake provisioning store).
    EXPECT_NE(h.out.find("no provisioning store"), std::string::npos);
    h.tui.feedByte(0x1b); h.tui.feed("", 0);   // back to the submenu
    EXPECT_EQ(h.tui.screen(), Tui::Screen::PbxAddMenu);
}

TEST(Tui, PbxConfigMonoTierEmitsNoColor) {
    Harness h(/*unicode=*/false, /*color=*/false);
    h.toHub();
    h.tui.feedByte('3');
    // Walk every tab in the mono tier — none may emit colour SGR.
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(hasColorSgr(h.out)) << "PBX tab " << i << " emitted color";
        h.clear();
        h.tui.feedByte('\t');
    }
}
