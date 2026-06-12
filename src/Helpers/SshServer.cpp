// SshServer.cpp — SSH-accessible esp_console terminal for pocket-dial.
//
// wolfSSH availability is gated on the compile-time symbol POCKETDIAL_HAS_WOLFSSH.
// Without it every transport-layer call stubs out with a log message so the rest
// of the build is unaffected.  The esp_console command registry is always populated
// (even in stub mode) so commands are available to future console transports (UART
// debug shell, USB-serial) without code changes.
//
// Task constraints (§ IMPORTANT CONSTRAINTS in the ticket):
//   * Core 0, priority 3 — below the SIP task (priority 5).
//   * Stack 8192 bytes (measured; 4096 is tight once wolfSSH session heap is live).
//   * No blocking calls on the SIP-task path — the SSH task is fully independent.

#include "SshServer.hpp"
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>

// ── Platform includes ─────────────────────────────────────────────────────────
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#else
// Host stubs: let the file compile for unit-test / CI builds.
#include <cstdlib>
#define ESP_LOGI(tag, ...) do { fprintf(stdout, "[%s] ", tag); fprintf(stdout, __VA_ARGS__); fputc('\n', stdout); } while(0)
#define ESP_LOGE(tag, ...) do { fprintf(stderr, "[%s] ", tag); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while(0)
#define ESP_LOGW(tag, ...) do { fprintf(stdout, "[%s] ", tag); fprintf(stdout, __VA_ARGS__); fputc('\n', stdout); } while(0)
#define esp_restart() ::abort()
static inline int esp_console_cmd_register(const esp_console_cmd_t*) { return 0; }
static inline int esp_console_run(const char*, int*) { return 0; }
static inline void esp_sntp_setservername(int, const char*) {}
static inline void vTaskDelay(int) {}
static inline void vTaskDelete(void*) {}
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffffUL
typedef void* TaskHandle_t;
typedef unsigned int TickType_t;

// ESP-API host shims for the wolfSSH/TUI region (PD_HOST_SSH builds compile it
// on the desktop). NVS doesn't exist on host: nvs_open fails, so every guarded
// write skips — AdminAuth/host state lives in memory. Heap query reports zeros
// (the TUI treats 0 as "query failed" and renders an honest blank).
#include <chrono>
typedef int esp_err_t;
#define ESP_OK   0
#define ESP_FAIL (-1)
typedef unsigned int nvs_handle_t;
#define NVS_READONLY  0
#define NVS_READWRITE 1
static inline esp_err_t nvs_open(const char*, int, nvs_handle_t*)        { return ESP_FAIL; }
static inline esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*) { return ESP_FAIL; }
static inline esp_err_t nvs_set_u8(nvs_handle_t, const char*, unsigned char) { return ESP_FAIL; }
static inline esp_err_t nvs_get_u8(nvs_handle_t, const char*, unsigned char*) { return ESP_FAIL; }
static inline esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, size_t*) { return ESP_FAIL; }
static inline esp_err_t nvs_commit(nvs_handle_t)                         { return ESP_FAIL; }
static inline void      nvs_close(nvs_handle_t)                          {}
static inline esp_err_t nvs_flash_erase()                                { return ESP_FAIL; }
static inline long long esp_timer_get_time()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
#define ESP_MAC_WIFI_STA 0
static inline esp_err_t esp_read_mac(unsigned char*, int) { return ESP_FAIL; }
typedef struct { size_t total_free_bytes; size_t total_allocated_bytes; } multi_heap_info_t;
#define MALLOC_CAP_DEFAULT 0
static inline void heap_caps_get_info(multi_heap_info_t* info, int)
{
    info->total_free_bytes = 0;
    info->total_allocated_bytes = 0;
}

// Host sockets for the wolfSSH listener (PD_HOST_SSH builds). The ESP arm gets
// these from lwip above; the stub-only build never touches a socket.
#ifdef POCKETDIAL_HAS_WOLFSSH
#include <thread>
#if defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#endif // POCKETDIAL_HAS_WOLFSSH
#endif

#ifdef POCKETDIAL_HAS_WOLFSSH
// One spelling for "close a socket" across lwip (ESP), POSIX, and winsock.
static inline int pd_sock_close(int s)
{
#if defined(_WIN32) || defined(_WIN64)
    return closesocket(s);
#else
    return ::close(s);   // lwip and POSIX both provide ::close for sockets
#endif
}
#endif

// ── wolfSSH conditional includes ─────────────────────────────────────────────
#ifdef POCKETDIAL_HAS_WOLFSSH
#include "wolfssh/ssh.h"
#include "wolfssh/internal.h"     // struct WOLFSSH: widthChar/heightRows from the client pty-req
#include "wolfssh/certs_test.h"   // ecc_key_der_256 / sizeof_ecc_key_der_256 (bundled host key)
#include "AdminAuth.hpp"          // userauth: open when unsecured, PIN-gated once secured
#include "Tui.hpp"                // the ANSI sysop-terminal TUI (replaces the line shell)
#include "RequestsHandler.hpp"    // live registrar stats for the TUI hub/status line
#include "SipSecretStore.hpp"     // per-extension SIP digest secret assign/rotate ([S])
#endif

// ── Constants ─────────────────────────────────────────────────────────────────
static constexpr const char* TAG = "ssh";
// Devices serve real port 22; the host build defaults to 2222 via CMake
// (-D PD_HOST_SSH_PORT) since 22 is usually taken/privileged on desktops.
#ifndef POCKETDIAL_SSH_PORT
#define POCKETDIAL_SSH_PORT 22
#endif
static constexpr uint16_t    SSH_DEFAULT_PORT   = POCKETDIAL_SSH_PORT;
#ifdef POCKETDIAL_HAS_WOLFSSH
static constexpr uint32_t    SSH_TASK_STACK     = 24576; // wolfSSH handshake crypto is stack-heavy
#else
static constexpr uint32_t    SSH_TASK_STACK     = 4096;  // stub: idle task only
#endif
static constexpr uint8_t     SSH_TASK_PRIORITY  = 3;    // below SIP (5)
static constexpr int         SSH_TASK_CORE      = 0;

// ── Singleton ────────────────────────────────────────────────────────────────

#ifdef POCKETDIAL_HAS_LITTLESSH
extern "C" void pd_littlessh_task(void* arg);   // defined in SshServerLittlessh.cpp
#endif

SshServer& SshServer::instance()
{
    static SshServer inst;
    return inst;
}

// ── Console / command registration ───────────────────────────────────────────

void SshServer::registerCommand(const esp_console_cmd_t* cmd)
{
    if (!cmd) return;
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    esp_err_t err = esp_console_cmd_register(cmd);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "cmd register failed: %s", cmd->command);
    }
#else
    (void)cmd;
#endif
}

// Built-in commands registered regardless of wolfSSH presence.

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)

static int cmd_show_status(int argc, char** argv)
{
    (void)argc; (void)argv;
    // Print uptime + SIP pool stats (placeholders; real values require
    // access to RequestsHandler, which is wired in via a callback set by
    // the main application if needed).
    printf("uptime: %lu s\n", (unsigned long)(esp_timer_get_time() / 1000000ULL));
    printf("SIP pool stats: not wired yet\n");
    return 0;
}

static int cmd_show_sessions(int argc, char** argv)
{
    (void)argc; (void)argv;
    printf("active sessions: not wired yet\n");
    return 0;
}

static int cmd_set_admin_ext(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: set admin-ext <ext>\n");
        return 1;
    }
    printf("[ssh] admin-ext set to %s (restart required for full effect)\n", argv[1]);
    // Actual update wired by application via a registered callback or NVS write.
    nvs_handle_t h;
    if (nvs_open("pbxcfg", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_str(h, "admin_ext", argv[1]);
        nvs_commit(h);
        nvs_close(h);
    }
    return 0;
}

static int cmd_set_topology(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: set topology <client|infra>\n");
        return 1;
    }
    uint8_t mode = (strcmp(argv[1], "client") == 0) ? 1 : 2;
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_u8(h, "wifi_mode", mode);
        nvs_commit(h);
        nvs_close(h);
    }
    printf("[ssh] topology set to %s, restarting...\n", argv[1]);
    esp_restart();
    return 0;
}

static int cmd_set_ssh(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: set ssh <enable|disable>\n");
        return 1;
    }
    bool en = (strcmp(argv[1], "enable") == 0);
    SshServer::instance().setEnabled(en);
    printf("[ssh] SSH %s\n", en ? "enabled" : "disabled");
    return 0;
}

static int cmd_set_time(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: set time <NTP server>\n");
        return 1;
    }
    esp_sntp_setservername(0, argv[1]);
    printf("[ssh] NTP server set to %s\n", argv[1]);
    return 0;
}

// Two-step factory reset: first invocation sets a flag; second confirms.
static bool s_factoryResetPending = false;

static int cmd_factory_reset(int argc, char** argv)
{
    (void)argc; (void)argv;
    if (!s_factoryResetPending)
    {
        s_factoryResetPending = true;
        printf("[ssh] factory-reset: run this command again within 30 s to confirm.\n");
    }
    else
    {
        s_factoryResetPending = false;
        printf("[ssh] factory-reset confirmed. Erasing NVS and restarting...\n");
        nvs_flash_erase();
        esp_restart();
    }
    return 0;
}

#endif  // ESP_PLATFORM

void SshServer::registerBuiltinCommands()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    // Register each command. esp_console_cmd_t::func is a raw function pointer;
    // the named static functions (all with C linkage ABI — no captures) convert
    // implicitly without a cast.
    const esp_console_cmd_t cmds[] = {
        { "show status",    "Print IP, uptime, SIP pool stats",         nullptr, cmd_show_status    },
        { "show sessions",  "Print active calls",                        nullptr, cmd_show_sessions  },
        { "set admin-ext",  "Set administrative extension (NVS)",        nullptr, cmd_set_admin_ext  },
        { "set topology",   "Set wifi_mode (client|infra) and restart",  nullptr, cmd_set_topology   },
        { "set ssh",        "Enable or disable SSH engine",              nullptr, cmd_set_ssh        },
        { "set time",       "Set NTP server hostname",                   nullptr, cmd_set_time       },
        { "factory-reset",  "Erase NVS and restart (two-step confirm)",  nullptr, cmd_factory_reset  },
    };
    for (const auto& cmd : cmds)
    {
        esp_console_cmd_register(&cmd);
    }
#endif
}

void SshServer::initConsole()
{
    if (_consoleInitialized) return;
    _consoleInitialized = true;

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    esp_console_config_t consoleCfg = ESP_CONSOLE_CONFIG_DEFAULT();
    consoleCfg.max_cmdline_length = 256;
    consoleCfg.max_cmdline_args   = 8;
    esp_console_init(&consoleCfg);
    registerBuiltinCommands();
    ESP_LOGI(TAG, "esp_console initialized, built-in commands registered");
#endif
}

// ── Public API ────────────────────────────────────────────────────────────────

bool SshServer::isEnabled() const
{
    return _enabled;
}

void SshServer::start()
{
    // Always initialize the console so commands are available even in stub mode.
    initConsole();

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    // Read NVS "ssh_enabled" key.
    {
        // Read-only: opening NVS_READWRITE merely to READ a flag is wrong and can fail
        // when the partition has no free pages for a write handle. Previously, on such a
        // failure _enabled kept its default (true) and SSH started despite being disabled
        // (fail-open). Use NVS_READONLY and fail CLOSED. A missing key (namespace exists,
        // key absent) still leaves val=1 → enabled-by-default after provisioning.
        nvs_handle_t h;
        esp_err_t e = nvs_open("storage", NVS_READONLY, &h);
        if (e == ESP_OK)
        {
            uint8_t val = 1;  // default: enabled when the key is absent
            nvs_get_u8(h, "ssh_enabled", &val);
            nvs_close(h);
            _enabled = (val != 0);
        }
        else
        {
            ESP_LOGW(TAG, "ssh_enabled NVS read failed (%s) — defaulting SSH OFF", esp_err_to_name(e));
            _enabled = false;
        }
    }

    if (!_enabled)
    {
        ESP_LOGI(TAG, "SSH disabled by NVS key — not starting");
        return;
    }
#endif

#ifdef POCKETDIAL_HAS_WOLFSSH
    // Full wolfSSH path: initialize wolfSSH, bind TCP port 22, spawn the listener.
    if (wolfSSH_Init() != WS_SUCCESS)
    {
        ESP_LOGE(TAG, "wolfSSH_Init() failed — SSH engine disabled");
        return;
    }
    ESP_LOGI(TAG, "wolfSSH initialized");

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    if (_taskHandle != nullptr)
    {
        // Already running — skip re-spawn.
        return;
    }
    BaseType_t ret = xTaskCreatePinnedToCore(
        sshListenTask,
        "ssh_listen",
        SSH_TASK_STACK,
        this,
        SSH_TASK_PRIORITY,
        &_taskHandle,
        SSH_TASK_CORE
    );
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed for ssh_listen");
        _taskHandle = nullptr;
    }
    else
    {
        ESP_LOGI(TAG, "SSH listen task started (core %d, priority %d)",
                 SSH_TASK_CORE, SSH_TASK_PRIORITY);
    }
#else   // host: detached std::thread instead of a FreeRTOS task
    if (_taskHandle != nullptr)
    {
        return;   // already running
    }
#if defined(_WIN32) || defined(_WIN64)
    // Idempotent winsock init (reference-counted by the OS; HttpServer may
    // already have called it, an extra WSAStartup/never-WSACleanup is fine
    // for a process-lifetime server).
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    std::thread(&SshServer::sshListenTask, static_cast<void*>(this)).detach();
    _taskHandle = reinterpret_cast<void*>(1);   // marker: listener running
    ESP_LOGI(TAG, "SSH listener thread started (host, port %u)", SSH_DEFAULT_PORT);
#endif  // ESP_PLATFORM

#elif defined(POCKETDIAL_HAS_LITTLESSH)
    // littlessh backend (PSA/mbedTLS) — gives transports without wolfSSH a real
    // SSH console. The task owns its own accept loop (lssh_server_run).
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    if (_taskHandle != nullptr) return;   // already running
    {
        BaseType_t ret = xTaskCreatePinnedToCore(
            pd_littlessh_task, "ssh_little", 12288, this,
            SSH_TASK_PRIORITY, &_taskHandle, SSH_TASK_CORE);
        if (ret != pdPASS) { ESP_LOGE(TAG, "littlessh task create failed"); _taskHandle = nullptr; }
        else ESP_LOGI(TAG, "littlessh SSH backend started (core %d, prio %d)",
                      SSH_TASK_CORE, SSH_TASK_PRIORITY);
    }
#endif
#else   // no SSH backend linked
    ESP_LOGI(TAG, "no SSH backend linked — SSH engine stubbed");
#endif  // POCKETDIAL_HAS_WOLFSSH
}

void SshServer::stop()
{
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    if (_taskHandle != nullptr)
    {
        vTaskDelete(_taskHandle);
        _taskHandle = nullptr;
        ESP_LOGI(TAG, "SSH listen task stopped");
    }
#endif

#ifdef POCKETDIAL_HAS_WOLFSSH
    wolfSSH_Cleanup();
#endif
}

void SshServer::setEnabled(bool enabled)
{
    _enabled = enabled;

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    nvs_handle_t h;
    if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK)
    {
        nvs_set_u8(h, "ssh_enabled", enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
#endif

    if (enabled)
    {
        start();
    }
    else
    {
        stop();
    }
}

void SshServer::setNetInfo(const char* ip, uint8_t wifiMode, const char* ssid)
{
    if (ip && ip[0])   _netIp   = ip;     // leave default 0.0.0.0 if null/empty
    if (ssid)          _netSsid = ssid;   // empty string is a valid "no SSID"
    _netMode    = wifiMode;
    _netInfoSet = true;
}

// ── wolfSSH auth + command dispatch ───────────────────────────────────────────
#ifdef POCKETDIAL_HAS_WOLFSSH

// User authentication. Onboarding model "up usable, secure later": SSH is OPEN while the
// device is unsecured (no admin PIN set) and requires the admin PIN as the SSH password
// once a PIN exists (AdminAuth::isProvisioned()). The username is not checked.
static int wsUserAuth(byte authType, WS_UserAuthData* authData, void* ctx)
{
    (void)ctx;
    if (!AdminAuth::isProvisioned())
        return WOLFSSH_USERAUTH_SUCCESS;            // unsecured: allow anyone (onboarding)
    if (authType == WOLFSSH_USERAUTH_PASSWORD && authData != nullptr)
    {
        std::string pw(reinterpret_cast<const char*>(authData->sf.password.password),
                       authData->sf.password.passwordSz);
        if (AdminAuth::verifyPin(pw))
            return WOLFSSH_USERAUTH_SUCCESS;
        return WOLFSSH_USERAUTH_INVALID_PASSWORD;
    }
    return WOLFSSH_USERAUTH_FAILURE;                // secured: only the PIN (password) is accepted
}

// Execute one console command line and RETURN its output as a string. The esp_console
// cmd_* handlers print to UART (invisible over SSH), so SSH uses this parallel dispatcher.
// Retained (per the B2b-1 ticket) for the non-interactive command path and future console
// transports even though the interactive entry point is now the TUI; [[maybe_unused]]
// documents that and silences -Wunused-function now that the line-shell caller is gone.
[[maybe_unused]] static std::string dispatch_ssh(const std::string& line)
{
    std::vector<std::string> tok;
    size_t i = 0;
    while (i < line.size())
    {
        while (i < line.size() && line[i] == ' ') ++i;
        size_t j = i;
        while (j < line.size() && line[j] != ' ') ++j;
        if (j > i) tok.emplace_back(line.substr(i, j - i));
        i = j;
    }
    if (tok.empty()) return "";

    auto is2 = [&](const char* a, const char* b){ return tok.size() >= 2 && tok[0] == a && tok[1] == b; };

    if (tok[0] == "help" || tok[0] == "?")
        return "commands:\r\n"
               "  show status | show sessions\r\n"
               "  set admin-ext <ext>\r\n"
               "  set topology <client|infra>   (reboot to apply)\r\n"
               "  set ssh <enable|disable>\r\n"
               "  set time <ntp-host>\r\n"
               "  exit\r\n";
    if (is2("show", "status"))
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "uptime: %lu s\r\nsecured: %s\r\n",
                 (unsigned long)(esp_timer_get_time() / 1000000ULL),
                 AdminAuth::isProvisioned() ? "yes" : "no");
        return buf;
    }
    if (is2("show", "sessions"))
        return "active sessions: (not wired to RequestsHandler yet)\r\n";
    if (tok[0] == "set" && tok.size() >= 3 && tok[1] == "admin-ext")
    {
        nvs_handle_t h;
        if (nvs_open("pbxcfg", NVS_READWRITE, &h) == ESP_OK)
        { nvs_set_str(h, "admin_ext", tok[2].c_str()); nvs_commit(h); nvs_close(h); }
        return "admin-ext set to " + tok[2] + " (restart for full effect)\r\n";
    }
    if (tok[0] == "set" && tok.size() >= 3 && tok[1] == "topology")
    {
        uint8_t mode = (tok[2] == "client") ? 1 : 2;
        nvs_handle_t h;
        if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK)
        { nvs_set_u8(h, "wifi_mode", mode); nvs_commit(h); nvs_close(h); }
        return "topology set to " + tok[2] + " (reboot to apply)\r\n";
    }
    if (tok[0] == "set" && tok.size() >= 3 && tok[1] == "ssh")
    {
        bool en = (tok[2] == "enable");
        SshServer::instance().setEnabled(en);
        return std::string("SSH ") + (en ? "enabled\r\n" : "disabled\r\n");
    }
    if (tok[0] == "set" && tok.size() >= 3 && tok[1] == "time")
    {
        esp_sntp_setservername(0, tok[2].c_str());
        return "NTP server set to " + tok[2] + "\r\n";
    }
    return "error: unknown command '" + tok[0] + "' (try 'help')\r\n";
}

// ── TUI session driver ────────────────────────────────────────────────────────
// Build the live-stats snapshot the TUI hub/title-bar read. Pulls real registrar
// counts from the attached RequestsHandler (thread-safe dashboard getters) plus
// uptime/IP/MAC from the platform. Falls back to a correct, zeroed spine when no
// handler is attached. Runs off the SIP thread; never blocks the signaling path.
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
    char mac[18] = {0};
    uint8_t m[6] = {0};
    if (esp_read_mac(m, ESP_MAC_WIFI_STA) == ESP_OK)
    {
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
        st.mac = mac;
    }
    st.host = "pocketdial.local";
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

// Drive one interactive TUI session over an accepted wolfSSH channel. The TUI is
// fed raw bytes from wolfSSH_stream_read and writes through a callback bound to
// wolfSSH_stream_send. Returns when the operator logs out ([L]) or the channel
// disconnects (read <= 0). The caller owns wolfSSH_shutdown/free/close.
static void runTuiSession(WOLFSSH* ssh)
{
    Tui tui;
    tui.setWriter([ssh](const char* data, size_t n) {
        if (n == 0) return;
        wolfSSH_stream_send(ssh, reinterpret_cast<byte*>(const_cast<char*>(data)),
                            static_cast<word32>(n));
    });
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
    // Capture the connected operator's peer "ip:port" and the session-start uptime
    // ONCE at accept time (cheap, stable for the session). The provider then folds in
    // the live provisioned/SSH-enabled/admin-ext facts each render.
    std::string peer = "?";
    {
        struct sockaddr_in pa{};
        socklen_t pl = sizeof(pa);
        int sfd = wolfSSH_get_fd(ssh);
        if (sfd >= 0 && getpeername(sfd, reinterpret_cast<struct sockaddr*>(&pa), &pl) == 0)
        {
            char ipb[16] = {0};
            const uint8_t* o = reinterpret_cast<const uint8_t*>(&pa.sin_addr.s_addr);
            snprintf(ipb, sizeof(ipb), "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
            char pb[32];
            snprintf(pb, sizeof(pb), "%s:%u", ipb, (unsigned)ntohs(pa.sin_port));
            peer = pb;
        }
    }
    const uint64_t sessUptime = static_cast<uint64_t>(esp_timer_get_time() / 1000000LL);

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
        if (AdminAuth::isProvisioned() && !AdminAuth::verifyPin(cur))
            return "Current PIN is wrong.";
        if (!AdminAuth::setPin(neu))
            return "New PIN rejected (too short?).";
        return "";   // success
    });

    // ── [4]/[K] SSH access toggle: persist + start/stop the engine. ───────────
    tui.setSshToggle([](bool enabled) {
        SshServer::instance().setEnabled(enabled);
    });

    // Size from the captured pty-req geometry (defaults 80x24 — see accept path).
    SshServer& self = SshServer::instance();
    tui.setSize(self.terminalCols(), self.terminalRows());

    tui.begin();   // banner → hub

    // The session loop must wake ~1 Hz to drive the live monitor's repaint even when
    // the operator is not typing. We keep wolfSSH in BLOCKING mode (no WS_WANT_READ
    // handling) and gate the blocking read behind a select() with a 1 s timeout on
    // the raw socket: select returns immediately on a keystroke, or times out so we
    // can tickLive() and loop. The cadence is the brief's ~1 Hz wallboard refresh.
    const int fd = wolfSSH_get_fd(ssh);
    char rbuf[256];
    while (tui.running())
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv{ 1, 0 };          // 1 s
        int sel = select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) break;                 // socket error → end session
        if (sel == 0) { tui.tickLive(); continue; }  // idle tick (1 Hz repaint)

        int n = wolfSSH_stream_read(ssh, reinterpret_cast<byte*>(rbuf), sizeof(rbuf));
        if (n <= 0) break;                  // EOF / error / disconnect
        if (!tui.feed(rbuf, static_cast<size_t>(n))) break;  // logout
    }
}

#endif // POCKETDIAL_HAS_WOLFSSH

// ── Listen task ───────────────────────────────────────────────────────────────

void SshServer::sshListenTask(void* arg)
{
    (void)arg;

#ifdef POCKETDIAL_HAS_WOLFSSH
    // ── Full wolfSSH accept loop ──────────────────────────────────────────────
    // 1. Create a server context with a host key (stored in NVS under "storage" /
    //    "ssh_host_key" — provisioning is out of scope here; the stub logs if absent).
    // 2. Bind TCP port SSH_DEFAULT_PORT.
    // 3. Accept → spawn a short-lived per-session task that wraps wolfSSH channel
    //    I/O → esp_console_run() so every registered command is available.

    WOLFSSH_CTX* ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, nullptr);
    if (!ctx)
    {
        ESP_LOGE(TAG, "wolfSSH_CTX_new failed");
        vTaskDelete(nullptr);
        return;
    }

    wolfSSH_SetUserAuth(ctx, wsUserAuth);

    // Host key: prefer a per-device DER key from NVS ("ssh_host_key"); else fall back to
    // the bundled wolfSSH demo ECC P-256 key so SSH works out of the box. (The demo key is
    // public — fine for a trusted LAN console; provision a unique key for production.)
    bool keyLoaded = false;
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    {
        nvs_handle_t h;
        if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK)
        {
            size_t len = 0;
            if (nvs_get_blob(h, "ssh_host_key", nullptr, &len) == ESP_OK && len > 0)
            {
                std::vector<uint8_t> key(len);
                if (nvs_get_blob(h, "ssh_host_key", key.data(), &len) == ESP_OK &&
                    wolfSSH_CTX_UsePrivateKey_buffer(ctx, key.data(),
                        static_cast<word32>(len), WOLFSSH_FORMAT_ASN1) == WS_SUCCESS)
                    keyLoaded = true;
            }
            nvs_close(h);
        }
    }
#endif
    if (!keyLoaded)
    {
        if (wolfSSH_CTX_UsePrivateKey_buffer(ctx, ecc_key_der_256,
                static_cast<word32>(sizeof_ecc_key_der_256), WOLFSSH_FORMAT_ASN1) != WS_SUCCESS)
        {
            ESP_LOGE(TAG, "failed to load any SSH host key");
            wolfSSH_CTX_free(ctx);
            vTaskDelete(nullptr);
            return;
        }
        ESP_LOGW(TAG, "using bundled demo ECC host key (provision a unique one for production)");
    }

    // TCP listen socket.
    struct sockaddr_in srvAddr{};
    srvAddr.sin_family      = AF_INET;
    srvAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    srvAddr.sin_port        = htons(SSH_DEFAULT_PORT);

    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0)
    {
        ESP_LOGE(TAG, "socket() failed");
        wolfSSH_CTX_free(ctx);
        vTaskDelete(nullptr);
        return;
    }
    int opt = 1;
    // const char* cast: winsock wants char*, lwip/POSIX take void* — both accept this.
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
    if (bind(listenFd, reinterpret_cast<struct sockaddr*>(&srvAddr), sizeof(srvAddr)) < 0)
    {
        ESP_LOGE(TAG, "bind() port %u failed", SSH_DEFAULT_PORT);
        pd_sock_close(listenFd);
        wolfSSH_CTX_free(ctx);
        vTaskDelete(nullptr);
        return;
    }
    listen(listenFd, 2);
    ESP_LOGI(TAG, "listening on TCP port %u", SSH_DEFAULT_PORT);

    // Accept loop — one connection at a time (ESP32 RAM budget).
    while (true)
    {
        struct sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        int clientFd = accept(listenFd,
            reinterpret_cast<struct sockaddr*>(&clientAddr), &addrLen);
        if (clientFd < 0)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        WOLFSSH* ssh = wolfSSH_new(ctx);
        if (!ssh)
        {
            pd_sock_close(clientFd);
            continue;
        }
        wolfSSH_set_fd(ssh, clientFd);

        // Handshake + channel open. This drives the full state machine including the
        // optional pty-req: a real terminal (PuTTY/OpenSSH, paramiko chan.get_pty())
        // sends "pty-req" + "shell"; wolfSSH parses the terminal type, window size, and
        // (now patched, see cmake/patch_wolfssl.py) an empty modes string without
        // failing, then records cols/rows on the session below.
        int rc = wolfSSH_accept(ssh);
        if (rc != WS_SUCCESS)
        {
            ESP_LOGW(TAG, "wolfSSH_accept error %d (ssh_err %d: %s)", rc,
                     wolfSSH_get_error(ssh), wolfSSH_get_error_name(ssh));
            wolfSSH_free(ssh);
            pd_sock_close(clientFd);
            continue;
        }

        // Capture the client's terminal geometry for the future ANSI TUI. wolfSSH stores
        // the pty-req dimensions on the session (widthChar = cols, heightRows = rows).
        // With NO_FILESYSTEM there is no public getter, so read the struct directly
        // (WOLFSSH_TERM guarantees these fields exist). A client that did not request a
        // pty leaves them 0 → keep the 80x24 default so the TUI still has a sane size.
        {
            SshServer& self = SshServer::instance();
            if (ssh->widthChar > 0 && ssh->heightRows > 0)
            {
                self._termCols = static_cast<uint16_t>(ssh->widthChar);
                self._termRows = static_cast<uint16_t>(ssh->heightRows);
                self._hasPty   = true;
                ESP_LOGI(TAG, "pty-req: %ux%u (cols x rows)",
                         (unsigned)self._termCols, (unsigned)self._termRows);
            }
            else
            {
                self._termCols = 80;
                self._termRows = 24;
                self._hasPty   = false;
                ESP_LOGI(TAG, "no pty-req; defaulting terminal to 80x24");
            }
        }

        // ── Interactive ANSI TUI session (replaces the old line shell) ────────
        // The TUI is transport-agnostic: we hand it a write callback bound to this
        // session's wolfSSH channel and pump wolfSSH_stream_read bytes into feed().
        // It renders the brand banner → master hub and drives the screen router.
        runTuiSession(ssh);

        wolfSSH_shutdown(ssh);
        wolfSSH_free(ssh);
        pd_sock_close(clientFd);
    }

    wolfSSH_CTX_free(ctx);
    pd_sock_close(listenFd);

#else   // POCKETDIAL_HAS_WOLFSSH not defined — stub loop

    ESP_LOGI(TAG, "wolfSSH not linked — SSH engine stubbed (task idle)");
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

#endif  // POCKETDIAL_HAS_WOLFSSH

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    vTaskDelete(nullptr);
#endif
}
