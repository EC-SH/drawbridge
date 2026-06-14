cmake_minimum_required(VERSION 3.11)
project(pocket_dial_tests CXX)

# The codebase is C++17 (std::string_view / std::optional in SipMessage, the digest
# store, etc.). GCC/Clang default to C++17, so CI was implicitly fine, but MSVC
# defaults to C++14 and fails to parse those headers. Pin the standard explicitly so
# the host suite builds identically on every toolchain.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include_directories(${gtest_SOURCE_DIR}/include ../src/SIP ../src/Helpers)

add_executable(sip_parser_tests
    SipMessage_test.cpp
    SipMessage_header_test.cpp
    SipStatus_test.cpp
    SipDigest_test.cpp
    ArpLookup_test.cpp
    Pbx_test.cpp
    PageZone_test.cpp
    UrlEncode_test.cpp
    Rtp_test.cpp
    Hold_test.cpp
    RtpReceiver_test.cpp
    Park_test.cpp
    # Audit concurrency & memory-safety cluster (#41 message-pool race, #54 pool
    # sizing, #70 virtual-peer pool). Drives RequestsHandler + the static message
    # pool directly; host-compilable like the other RequestsHandler-backed suites.
    PoolSafety_test.cpp
    # Audit coverage cluster (#48 #49 #61 #62): direct host tests for the admin
    # credential/session manager, the 3CX anchor's pure parse/URL logic (extracted
    # to a host header so the ESP-only impl's contract is testable), the telephony
    # credential table + provider registry, and the OTA session state machine.
    AdminAuth_test.cpp
    ThreeCxAnchorLogic_test.cpp
    TelephonyApi_test.cpp
    OtaUpdater_test.cpp
    # OTA session state machine (host stub mirrors the device esp_ota_* lifecycle).
    # Linked for OtaUpdater_test.cpp (#62); the real flashing path stays on-device.
    ../src/Helpers/OtaUpdater.cpp
    Tui_test.cpp
    # The ANSI sysop-terminal renderer is transport-agnostic and host-compilable
    # (no wolfSSH, no ESP — only an esp_timer guard, behind ESP_PLATFORM). It links
    # cleanly into the host test binary so the banner/hub/mono-fallback render is
    # asserted on the desktop.
    ../src/Helpers/Tui.cpp
    # SIP digest-auth primitives + per-extension HA1 secret store. Both are
    # host-compilable: SipDigest vendors a portable MD5 (no mbedTLS) and
    # SipSecretStore uses an in-memory map under !ESP_PLATFORM (no NVS).
    ../src/Helpers/SipDigest.cpp
    ../src/Helpers/SipSecretStore.cpp
    # ArpLookup compiles to a host stub (returns nullopt — no ARP table off-device)
    # under !ESP_PLATFORM, so it links cleanly into the host test binary and lets
    # RequestsHandler's Learn-mode path build on the desktop.
    ../src/Helpers/ArpLookup.cpp
    # AdminAuth is self-contained (vendored SHA-256, host-stubbed NVS/RNG under
    # !ESP_PLATFORM). RequestsHandler::onDtmfInfo references AdminAuth::verifyPin for
    # the DTMF *PIN# admin menu, so it must be linked into the host binary too.
    ../src/Helpers/AdminAuth.cpp
    ../src/SIP/SipMessage.cpp
    ../src/SIP/SipStatus.cpp
    # Media beachhead (RTP sender + 440 media extension). RtpSender.cpp compiles to
    # host stubs (no sockets/FreeRTOS) under !ESP_PLATFORM; RequestsHandler.cpp +
    # SipSdpMessage/SipClient/Session are pulled in so the 440 SDP-answer and the
    # single-stream cap are unit-testable on the desktop build.
    ../src/SIP/RtpSender.cpp
    # Inbound media path (RtpReceiver): mirror of RtpSender. Host-stubbed under
    # !ESP_PLATFORM so the µ-law decode + RTP-parse helpers and the single-stream
    # cap are unit-testable on the desktop build (see RtpReceiver_test.cpp).
    ../src/SIP/RtpReceiver.cpp
    ../src/SIP/RequestsHandler.cpp
    # Telephony provider/API credential layer: RequestsHandler references these
    # (getTelephonyApis/setTelephonyApi + the boot provider select), so the test
    # binary must link them too. The host binary picks them up via GLOB.
    ../src/SIP/TelephonyApiConfig.cpp
    ../src/SIP/TelephonyProvider.cpp
    ../src/SIP/SipSdpMessage.cpp
    ../src/SIP/SipClient.cpp
    ../src/SIP/Session.cpp
    # DRAWBRIDGE M0: Playout buffer, Media bridge, and Anchor Clients
    PlayoutBuffer_test.cpp
    ../src/SIP/PlayoutBuffer.cpp
    ../src/SIP/MediaBridge.cpp
    ../src/SIP/LoopbackAnchorClient.cpp
    ../src/SIP/ThreeCxAnchorClient.cpp
)

target_link_libraries(sip_parser_tests PRIVATE gtest gtest_main)

if (WIN32)
    target_link_libraries(sip_parser_tests PRIVATE wsock32 ws2_32)
else()
    target_link_libraries(sip_parser_tests PRIVATE pthread)
endif()

add_test(NAME sip_parser_tests COMMAND sip_parser_tests)
