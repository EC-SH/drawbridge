# Select transport: "wifi" for SoftAP mode, "eth" for W5500 Ethernet (PoE),
#   "lan8720" for internal-EMAC LAN8720 Ethernet (LilyGO T-Internet-COM, classic ESP32),
#   "display" for Guition AXS15231B Display
# Override at build time:  idf.py -D SIP_TRANSPORT=display build
# NOTE: lan8720 requires the classic ESP32 (has the internal EMAC):  idf.py set-target esp32
if(NOT DEFINED SIP_TRANSPORT)
    set(SIP_TRANSPORT "eth")      # default to Ethernet for Waveshare board
endif()

set(ADDITIONAL_SRCS "")

if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(IDF_V6_DRIVERS esp_driver_gpio esp_driver_spi esp_driver_i2c)
else()
    set(IDF_V6_DRIVERS driver)
endif()

if(SIP_TRANSPORT STREQUAL "eth")
    set(MAIN_SRC "esp_main_eth.cpp")
    if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
        set(EXTRA_REQUIRES esp_eth espressif__w5500 ${IDF_V6_DRIVERS})
    else()
        set(EXTRA_REQUIRES esp_eth ${IDF_V6_DRIVERS})
    endif()
    message(STATUS "SipServer transport: W5500 Ethernet (PoE)")
elseif(SIP_TRANSPORT STREQUAL "lan8720")
    # LilyGO T-Internet-COM: classic ESP32 internal EMAC + external LAN8720 PHY (RMII).
    # Needs only esp_eth (internal EMAC is built in) + gpio for the PHY power pin.
    set(MAIN_SRC "esp_main_eth_lan8720.cpp")
    set(EXTRA_REQUIRES esp_eth ${IDF_V6_DRIVERS})
    message(STATUS "SipServer transport: LAN8720 Ethernet (internal EMAC / RMII, classic ESP32)")
elseif(SIP_TRANSPORT STREQUAL "display")
    set(MAIN_SRC "esp_main_display.cpp")
    set(EXTRA_REQUIRES esp_lcd espressif__esp_lcd_touch lvgl__lvgl esp_driver_ledc ${IDF_V6_DRIVERS})
    set(ADDITIONAL_SRCS "drivers/esp_lcd_axs15231b.c" "ui/ui.cpp" "ui/qrcode.c" "wifi/DnsServer.cpp")
    message(STATUS "SipServer transport: Guition AXS15231B Touch Display (LVGL)")
else()
    set(MAIN_SRC "esp_main.cpp")
    if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
        set(EXTRA_REQUIRES ${IDF_V6_DRIVERS})
    else()
        set(EXTRA_REQUIRES "")
    endif()
    message(STATUS "SipServer transport: WiFi SoftAP")
endif()

idf_component_register(SRCS ${MAIN_SRC}
                            ${ADDITIONAL_SRCS}
                            "../src/Helpers/UdpServer.cpp"
                            "../src/Helpers/HttpServer.cpp"
                            "../src/Helpers/AdminAuth.cpp"
                            "../src/Helpers/OtaUpdater.cpp"
                            "../src/SIP/RequestsHandler.cpp"
                            "../src/SIP/Session.cpp"
                            "../src/SIP/SipClient.cpp"
                            "../src/SIP/SipMessage.cpp"
                            "../src/SIP/SipMessageFactory.cpp"
                            "../src/SIP/SipSdpMessage.cpp"
                            "../src/SIP/SipServer.cpp"
                            "../src/SIP/SipStatus.cpp"
                            "../src/SIP/RtpSender.cpp"
                    INCLUDE_DIRS "../src/Helpers" "../src/SIP" "drivers" "ui" "wifi"
                    REQUIRES nvs_flash lwip esp_netif esp_event freertos mdns app_update esp_driver_ledc ${EXTRA_REQUIRES}
                    PRIV_REQUIRES esp_wifi)

# Issue #49: the display transport reserves Core 1 for the LVGL graphics task and
# runs the SIP engine on Core 0, so the UDP receiver (which carries SIP signaling)
# must stay on Core 0. The SoftAP/Ethernet transports run SIP on Core 1 and use the
# UdpServer default (Core 1).
# Pure-Ethernet transports (W5500 "eth" and LAN8720 "lan8720") have no WiFi at all,
# so they must NOT define POCKETDIAL_HAS_WIFI.
if(NOT SIP_TRANSPORT STREQUAL "eth" AND NOT SIP_TRANSPORT STREQUAL "lan8720")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE POCKETDIAL_HAS_WIFI)
endif()

if(SIP_TRANSPORT STREQUAL "display")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE POCKETDIAL_UDP_RX_CORE=0)
    # qrcode.c (ricmoo QRCode) uses #pragma mark (an Xcode-ism); GCC treats it as an
    # unknown pragma, which -Werror=all turns into an error. Demote to a warning here.
    set_source_files_properties("ui/qrcode.c" PROPERTIES COMPILE_FLAGS "-Wno-error=unknown-pragmas")
endif()

# Resource-constrained profile: shrink the pre-allocated SIP object pools to fit a
# low-RAM classic ESP32 (ESP32-WROOM, 520KB SRAM, no PSRAM). The message pool is the
# heaviest consumer (one std::string buffer per slot), so it gets cut the most.
# These override the #ifndef defaults in src/SIP/PoolConfig.hpp (32/8/32).
# Pair with sdkconfig.defaults.esp32_constrained + partitions_4mb.csv:
#   idf.py -D SIP_TRANSPORT=lan8720 -D SIP_CONSTRAINED=1 \
#          -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32_constrained" \
#          set-target esp32 build
if(SIP_CONSTRAINED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        POCKETDIAL_MAX_CLIENTS=8
        POCKETDIAL_MAX_SESSIONS=4
        POCKETDIAL_MSG_POOL=8)
    message(STATUS "SipServer profile: RESOURCE-CONSTRAINED (clients=8 sessions=4 msgpool=8)")
endif()
