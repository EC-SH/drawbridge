#pragma once
// SshServer.hpp — SSH-accessible esp_console terminal for pocket-dial.
//
// Architecture:
//   * Singleton access via SshServer::instance().
//   * start() reads NVS key "ssh_enabled" (u8) from namespace "storage". If 0,
//     the listener is a no-op. If wolfSSH is not linked, the server stubs itself
//     out cleanly so the rest of the firmware can still call all methods.
//   * All registered esp_console commands work regardless of wolfSSH presence;
//     if wolfSSH is absent they print "SSH engine not available" when the transport
//     is invoked, but the commands are registered so other console paths (UART,
//     future USB serial) can invoke them identically.
//   * The listen task is pinned to Core 0, priority 3 — below the SIP task
//     (priority 5) so the signaling path is never starved.
//
// wolfSSH availability:
//   The component guard POCKETDIAL_HAS_WOLFSSH is set in CMakeLists.txt (or
//   platformio.ini) when the wolfSSH component is actually linked. Without it
//   the implementation compiles as a stub. DO NOT add wolfSSH as a CMake
//   dependency here — the build system decides that; this file adapts at
//   compile time.

#include <string>
#include <cstdint>
#include <atomic>

// SSH listen port, shared by both backends (wolfSSH and littlessh) so the two
// can never drift. Devices serve real port 22; the host build overrides this to
// 2222 via CMake (-D PD_HOST_SSH_PORT) since 22 is usually privileged on desktops.
#ifndef POCKETDIAL_SSH_PORT
#define POCKETDIAL_SSH_PORT 22
#endif

// Forward-declared so the header never drags the full SIP engine into non-display
// builds. The TUI session reads live registrar stats through this pointer when one
// is attached (attachHandler), else it falls back to zeros with a correct spine.
class RequestsHandler;

#if defined(ESP_PLATFORM) || defined(ESP32)
#include "esp_console.h"
#else
// Host stub: minimal esp_console_cmd_t definition so the header compiles on Linux/Windows.
// The real struct (from esp_console.h) uses a raw function-pointer field; we replicate
// just enough for the SshServer interface to compile without dragging in ESP headers.
typedef int (*esp_console_cmd_func_t)(int argc, char** argv);
struct esp_console_cmd_t
{
    const char*              command;
    const char*              help;
    const void*              argtable;
    esp_console_cmd_func_t   func;
};
#endif

class SshServer
{
public:
    // Obtain the singleton instance (Meyer's singleton; thread-safe init in C++11).
    static SshServer& instance();

    // Start listening on TCP port 22 (or the port configured in NVS "ssh_port" u16).
    // No-op if NVS key "ssh_enabled" == 0. If wolfSSH is not linked, logs a stub
    // warning and returns immediately.
    void start();

    // Gracefully stop the listener task and close all active sessions.
    void stop();

    // Called by the littlessh backend task just before it self-deletes, so the
    // owning singleton's task handle is cleared and a later start() can respawn
    // (and stop() never vTaskDelete's an already-dead task). No-op contract for
    // the wolfSSH backend, whose listen task never returns on its own.
    void clearBackendTask() { _taskHandle = nullptr; }

    // Runtime toggle — writes "ssh_enabled" (u8) to NVS namespace "storage" and
    // calls start() or stop() as appropriate.
    void setEnabled(bool enabled);

    // Returns true if "ssh_enabled" NVS key is 1 (or if NVS is absent on host).
    bool isEnabled() const;

    // Register a command with the esp_console subsystem. Must be called before
    // start() so commands are available when the first SSH session connects.
    // Wraps esp_console_cmd_register() on device; no-op on host.
    void registerCommand(const esp_console_cmd_t* cmd);

    // Terminal geometry of the active SSH session, captured from the client's
    // pty-req (RFC 4254 §6.2). The upcoming ANSI TUI reads these to size itself.
    // Defaults to a sane 80x24 until a pty-req sets them; 0 is never reported.
    // Valid for the duration of a session; reset to the defaults between sessions.
    uint16_t terminalCols() const { return _termCols; }
    uint16_t terminalRows() const { return _termRows; }
    // True once a pty-req has been seen on the current session (the client is a
    // real terminal, so the TUI may use cursor control / raw-mode sequences).
    bool     hasPty()       const { return _hasPty; }

    // Wire the live SIP registrar so the TUI's hub status line / title-bar clock
    // reflect reality. Called once from app_main after the SipServer is created
    // (SshServer::instance().attachHandler(&server.getHandler())). Storing a raw
    // pointer is safe: the SipServer outlives the SSH session task for the life of
    // the process. Thread-safe to read — RequestsHandler's dashboard getters
    // snapshot-copy under their own mutex. Pass nullptr to detach.
    // _handler is written by the SIP task (attachHandler, once at boot) and read by
    // the SSH task (handler(), every TUI snapshot). Make the cross-task hand-off
    // explicit with an atomic: release on store, acquire on load, so the SSH task
    // sees a fully-constructed RequestsHandler. Zero cost on this target (a plain
    // aligned pointer load/store); the `if (h)` guards already cover the pre-attach
    // nullptr window.
    void attachHandler(RequestsHandler* h)
    {
        _handler.store(h, std::memory_order_release);
    }
    RequestsHandler* handler() const
    {
        return _handler.load(std::memory_order_acquire);
    }

    // Wire the live network identity so the TUI banner ADDR + hub network-mode tag
    // show reality instead of the 0.0.0.0 / "AP mode" defaults. Called once from
    // app_main after the netif has an IP (the display main already logs
    // "Local IP is: ..."). `wifiMode` is the NVS 'storage'/'wifi_mode' value:
    //   1 = STATION (joined an existing AP, DHCP client)
    //   2 = standalone SoftAP (own hotspot)
    //   0 = captive-portal SETUP (onboarding AP)
    // `ssid` is the joined/served SSID (may be empty/nullptr → unset). Plain copies
    // under a tiny mutex-free contract: written once at boot, read on the SSH task;
    // the strings are stable for the process lifetime. Passing nullptr leaves a
    // field unchanged-from-default. Falls back to the LiveStats defaults if never
    // called (a non-display build that never plumbs this stays correct, just 0.0.0.0).
    void setNetInfo(const char* ip, uint8_t wifiMode, const char* ssid);
    const std::string& netIp()   const { return _netIp; }
    uint8_t            netMode() const { return _netMode; }
    const std::string& netSsid() const { return _netSsid; }
    bool               netInfoSet() const { return _netInfoSet; }

private:
    SshServer() = default;
    ~SshServer() = default;

    // Prevent copy and move — singleton.
    SshServer(const SshServer&)            = delete;
    SshServer& operator=(const SshServer&) = delete;
    SshServer(SshServer&&)                 = delete;
    SshServer& operator=(SshServer&&)      = delete;

    // FreeRTOS task entry point (static trampoline → instance method).
    // Core 0, priority 3, stack 8192 bytes.
    static void sshListenTask(void* arg);

    // Internal helpers.
    void initConsole();               // register all built-in commands once
    void registerBuiltinCommands();

    bool _consoleInitialized{false};
    bool _enabled{true};

    // Active-session terminal geometry (see terminalCols()/terminalRows()).
    uint16_t _termCols{80};
    uint16_t _termRows{24};
    bool     _hasPty{false};

    // Live SIP registrar for the TUI (see attachHandler()). nullptr until wired.
    // Atomic: written by the SIP task, read by the SSH task (acquire/release).
    std::atomic<RequestsHandler*> _handler{nullptr};

    // Live network identity for the TUI (see setNetInfo()). Defaults mirror the
    // LiveStats defaults so an unset state still renders a correct spine.
    std::string _netIp{"0.0.0.0"};
    std::string _netSsid;
    uint8_t     _netMode{0};        // 0=SETUP/captive, 1=STATION, 2=AP (standalone)
    bool        _netInfoSet{false}; // true once setNetInfo() has run

#if defined(ESP_PLATFORM) || defined(ESP32)
    TaskHandle_t _taskHandle{nullptr};
#else
    void* _taskHandle{nullptr};       // placeholder on host — no FreeRTOS
#endif
};
