# Select transport: "wifi" for SoftAP mode, "eth" for W5500 Ethernet (PoE), "display" for Guition AXS15231B Display
# Override at build time:  idf.py -D SIP_TRANSPORT=display build
if(NOT DEFINED SIP_TRANSPORT)
    set(SIP_TRANSPORT "eth")      # default to Ethernet for Waveshare board
endif()

set(ADDITIONAL_SRCS "")

if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(IDF_V6_DRIVERS esp_driver_gpio esp_driver_spi)
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
elseif(SIP_TRANSPORT STREQUAL "display")
    set(MAIN_SRC "esp_main_display.cpp")
    set(EXTRA_REQUIRES esp_lcd lvgl__lvgl ${IDF_V6_DRIVERS})
    set(ADDITIONAL_SRCS "drivers/esp_lcd_axs15231b.c" "ui/ui.cpp" "wifi/DnsServer.cpp")
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
                            "../src/SIP/RequestsHandler.cpp"
                            "../src/SIP/Session.cpp"
                            "../src/SIP/SipClient.cpp"
                            "../src/SIP/SipMessage.cpp"
                            "../src/SIP/SipMessageFactory.cpp"
                            "../src/SIP/SipSdpMessage.cpp"
                            "../src/SIP/SipServer.cpp"
                            "../src/SIP/SipStatus.cpp"
                    INCLUDE_DIRS "../src/Helpers" "../src/SIP" "drivers" "ui" "wifi"
                    REQUIRES nvs_flash lwip esp_netif esp_event freertos mdns
                    PRIV_REQUIRES esp_wifi ${EXTRA_REQUIRES})

# Issue #49: the display transport reserves Core 1 for the LVGL graphics task and
# runs the SIP engine on Core 0, so the UDP receiver (which carries SIP signaling)
# must stay on Core 0. The SoftAP/Ethernet transports run SIP on Core 1 and use the
# UdpServer default (Core 1).
if(NOT SIP_TRANSPORT STREQUAL "eth")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE POCKETDIAL_HAS_WIFI)
endif()

if(SIP_TRANSPORT STREQUAL "display")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE POCKETDIAL_UDP_RX_CORE=0)
endif()
