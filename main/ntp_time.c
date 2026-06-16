// ntp_time.c — minimal NIST wall-clock sync, smallest practical exposure surface.
//
// Why this shape:
//   * SNTP CLIENT, poll mode  -> we only ever SEND a request and read the reply; there is
//     NO listening socket and NO inbound NTP port (we are not an NTP server).
//   * single hardcoded server -> DHCP-advertised time servers are explicitly ignored
//     (server_from_dhcp=false), so a rogue DHCP can't redirect our clock source.
//   * transient socket        -> open for the brief sync, then esp_netif_sntp_deinit()
//     releases the UDP socket; nothing is held between syncs (this box is socket-tight).
//   * plausibility guard      -> snapshot the clock before sync; if the reply yields an
//     absurd time, revert it. (Full anti-spoofing needs NTS — deliberately out of scope.)
//
// Sets the system wall clock in UTC. time()/gettimeofday() consumers (CDRs, logs) then get
// real time. Does NOT touch the monotonic esp_timer the anchor token-lifetime logic uses.

#include "ntp_time.h"

#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <time.h>
#include <sys/time.h>

static const char *TAG = "ntp";

#define NTP_SERVER       "time.nist.gov"            // NIST round-robin
#define NTP_SYNC_WAIT_MS 10000                       // bounded wait for a reply
#define NTP_RESYNC_MS    (6 * 60 * 60 * 1000)        // re-sync every 6 h (correct RTC drift)
#define NTP_SANE_MIN     ((time_t)1735689600)        // 2025-01-01 UTC: reject anything earlier

// One transient sync: open -> query NIST -> validate -> close + release the socket.
static void ntp_sync_once(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    cfg.start                      = true;
    cfg.server_from_dhcp           = false;   // never trust a DHCP-advertised time source
    cfg.renew_servers_after_new_IP = false;

    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "sntp init failed");
        return;
    }

    struct timeval before;
    gettimeofday(&before, NULL);              // snapshot so we can undo an implausible jump

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(NTP_SYNC_WAIT_MS)) == ESP_OK) {
        time_t now = 0;
        time(&now);
        if (now >= NTP_SANE_MIN) {
            struct tm utc;
            gmtime_r(&now, &utc);
            char ts[24];
            strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%SZ", &utc);
            ESP_LOGI(TAG, "clock set from %s: %s", NTP_SERVER, ts);
        } else {
            settimeofday(&before, NULL);       // bogus reply -> undo, keep the prior clock
            ESP_LOGW(TAG, "implausible time (%lld) rejected; reverted", (long long)now);
        }
    } else {
        ESP_LOGW(TAG, "no reply from %s within %d ms", NTP_SERVER, NTP_SYNC_WAIT_MS);
    }

    esp_netif_sntp_deinit();                    // release the UDP socket — nothing held between syncs
}

static void ntp_task(void *arg)
{
    (void)arg;
    for (;;) {
        ntp_sync_once();
        vTaskDelay(pdMS_TO_TICKS(NTP_RESYNC_MS));
    }
}

void ntp_time_start(void)
{
    // 4 KB stack covers SNTP + strftime; low priority — time is not latency-critical.
    xTaskCreate(ntp_task, "ntp", 4096, NULL, 3, NULL);
}
