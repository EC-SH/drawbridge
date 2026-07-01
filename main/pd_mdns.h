// pd_mdns.h — shared mDNS bring-up for every transport entry point (issue #154).
//
// mDNS used to be initialised inside the SipServer constructor, which on ESP
// runs in the SIP task — AFTER the provisioning gate. A factory-fresh unit
// sitting at the gate therefore never advertised `drawbridge.local`, breaking
// the `ssh owner@drawbridge.local` first-boot onboarding path. It is now
// hoisted into each `app_main`, called BEFORE the provisioning gate via this
// single shared helper so the four entry points stay in lockstep.
//
// The hostname is POCKETDIAL_HOSTNAME (a compile def from main/CMakeLists.txt,
// default "drawbridge"; overridable so two units on one LAN needn't collide,
// issue #47). Failures degrade with a warning rather than aborting — `.local`
// is a convenience, and CONTRIBUTING_FIRMWARE requires driver returns be
// checked and handled, not fatal.
#pragma once

// Hostname fallback lives OUTSIDE the ESP guard so static analysis (cppcheck runs
// in host context, without the CMake compile def) can still expand the macro
// where entry points concatenate it into log strings. Real firmware builds get
// the value from main/CMakeLists.txt; this #ifndef only fills a gap.
#ifndef POCKETDIAL_HOSTNAME
#define POCKETDIAL_HOSTNAME "drawbridge"
#endif

#if defined(ESP_PLATFORM)

#include "mdns.h"
#include "esp_log.h"

// Bring up the mDNS responder advertising POCKETDIAL_HOSTNAME.local plus the SIP
// and HTTP-dashboard service records. Idempotent: mdns_init() returns ESP_OK if
// already initialised (a duplicate service_add is harmless). Pass a port <= 0 to
// skip that service record (e.g. a captive-portal-only role with no SIP socket).
// `tag` is the caller's ESP_LOG tag.
static inline void pd_mdns_start(const char* tag, int sipPort, int httpPort)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK)
    {
        ESP_LOGW(tag, "mDNS init failed (%d) - .local resolution unavailable", err);
        return;
    }
    if (mdns_hostname_set(POCKETDIAL_HOSTNAME) != ESP_OK)
    {
        ESP_LOGW(tag, "mDNS hostname_set failed");
    }
    mdns_instance_name_set(POCKETDIAL_HOSTNAME " SIP PBX");
    if (sipPort > 0)
    {
        mdns_service_add(NULL, "_sip", "_udp", sipPort, NULL, 0);
    }
    if (httpPort > 0)
    {
        mdns_service_add(NULL, "_http", "_tcp", httpPort, NULL, 0);
    }
    ESP_LOGI(tag, "mDNS: %s.local advertised", POCKETDIAL_HOSTNAME);
}

#endif // ESP_PLATFORM
