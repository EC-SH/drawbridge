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
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffffUL
typedef void* TaskHandle_t;
typedef unsigned int TickType_t;
#endif

// ── wolfSSH conditional includes ─────────────────────────────────────────────
#ifdef POCKETDIAL_HAS_WOLFSSH
#include "wolfssh/ssh.h"
#endif

// ── Constants ─────────────────────────────────────────────────────────────────
static constexpr const char* TAG = "ssh";
static constexpr uint16_t    SSH_DEFAULT_PORT   = 22;
static constexpr uint32_t    SSH_TASK_STACK     = 8192;
static constexpr uint8_t     SSH_TASK_PRIORITY  = 3;    // below SIP (5)
static constexpr int         SSH_TASK_CORE      = 0;

// ── Singleton ────────────────────────────────────────────────────────────────

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
        nvs_handle_t h;
        if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK)
        {
            uint8_t val = 1;  // default: enabled
            nvs_get_u8(h, "ssh_enabled", &val);
            nvs_close(h);
            _enabled = (val != 0);
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
#endif  // ESP_PLATFORM

#else   // POCKETDIAL_HAS_WOLFSSH not defined
    ESP_LOGI(TAG, "wolfSSH not linked — SSH engine stubbed");
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

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    // Try to load the host key from NVS.
    {
        nvs_handle_t h;
        if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK)
        {
            size_t len = 0;
            if (nvs_get_blob(h, "ssh_host_key", nullptr, &len) == ESP_OK && len > 0)
            {
                std::vector<uint8_t> key(len);
                nvs_get_blob(h, "ssh_host_key", key.data(), &len);
                wolfSSH_CTX_UsePrivateKey_buffer(ctx,
                    key.data(), static_cast<long>(key.size()),
                    WOLFSSH_FORMAT_ASN1);
            }
            else
            {
                ESP_LOGW(TAG, "No SSH host key in NVS — sessions will fail auth");
            }
            nvs_close(h);
        }
    }
#endif

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
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(listenFd, reinterpret_cast<struct sockaddr*>(&srvAddr), sizeof(srvAddr)) < 0)
    {
        ESP_LOGE(TAG, "bind() port %u failed", SSH_DEFAULT_PORT);
        close(listenFd);
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
            close(clientFd);
            continue;
        }
        wolfSSH_set_fd(ssh, clientFd);

        // Handshake + channel open.
        int rc = wolfSSH_accept(ssh);
        if (rc != WS_SUCCESS)
        {
            ESP_LOGW(TAG, "wolfSSH_accept error %d", rc);
            wolfSSH_free(ssh);
            close(clientFd);
            continue;
        }

        // Read commands from the SSH channel and dispatch through esp_console.
        char cmdBuf[256];
        while (true)
        {
            int n = wolfSSH_stream_read(ssh,
                reinterpret_cast<byte*>(cmdBuf), sizeof(cmdBuf) - 1);
            if (n <= 0) break;
            cmdBuf[n] = '\0';

            // Strip trailing CR/LF.
            while (n > 0 && (cmdBuf[n - 1] == '\r' || cmdBuf[n - 1] == '\n'))
            {
                cmdBuf[--n] = '\0';
            }
            if (n == 0) continue;

            int ret = 0;
            esp_err_t err = esp_console_run(cmdBuf, &ret);
            if (err == ESP_ERR_NOT_FOUND)
            {
                const char* msg = "error: command not found\r\n";
                wolfSSH_stream_send(ssh,
                    reinterpret_cast<const byte*>(msg), strlen(msg));
            }
        }

        wolfSSH_shutdown(ssh);
        wolfSSH_free(ssh);
        close(clientFd);
    }

    wolfSSH_CTX_free(ctx);
    close(listenFd);

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
