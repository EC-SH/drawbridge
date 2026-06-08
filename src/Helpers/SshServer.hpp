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

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
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

    // Runtime toggle — writes "ssh_enabled" (u8) to NVS namespace "storage" and
    // calls start() or stop() as appropriate.
    void setEnabled(bool enabled);

    // Returns true if "ssh_enabled" NVS key is 1 (or if NVS is absent on host).
    bool isEnabled() const;

    // Register a command with the esp_console subsystem. Must be called before
    // start() so commands are available when the first SSH session connects.
    // Wraps esp_console_cmd_register() on device; no-op on host.
    void registerCommand(const esp_console_cmd_t* cmd);

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

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO)
    TaskHandle_t _taskHandle{nullptr};
#else
    void* _taskHandle{nullptr};       // placeholder on host — no FreeRTOS
#endif
};
