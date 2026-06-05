/*
 * esp_main_eth_lan8720.cpp — ESP-IDF entry point for SipServer over LAN8720 Ethernet
 *
 * Targets: LilyGO T-Internet-COM
 *          (classic ESP32 + LAN8720 PHY via the internal EMAC / RMII, 10/100 Mbps)
 *
 * Unlike the Waveshare ESP32-S3-ETH (W5500 over SPI — see esp_main_eth.cpp), this
 * board uses the ORIGINAL ESP32's built-in Ethernet MAC driving an external
 * LAN8720 PHY over RMII. The ESP32 sources the 50 MHz RMII reference clock from
 * its internal APLL and outputs it on GPIO0 (the board's ETH_CLOCK_GPIO0_OUT).
 *
 * Build:   idf.py set-target esp32
 *          idf.py -D SIP_TRANSPORT=lan8720 build
 *
 * Pin mapping is taken from the T-Internet-COM schematic + example/Arduino/ETHDemo/config.h.
 * The LilyGO ESP-IDF examples are legacy IDF v4.x (smi pins on eth_mac_config_t,
 * esp_eth_phy_new_lan8720, make build system) — this file uses the CURRENT v5.3.x
 * internal-EMAC API instead: eth_esp32_emac_config_t + esp_eth_phy_new_lan87xx.
 *
 * VERIFY against YOUR board revision before flashing — in particular the PHY
 * power-enable pin and whether a separate PHY reset is wired.
 */

#include <cstring>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"      // pulls in esp_eth_mac_esp.h (internal EMAC) + esp_eth_phy.h (LAN87xx)
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_mac.h"      // esp_read_mac, ESP_MAC_ETH
#include "esp_timer.h"    // esp_timer_get_time

#include "driver/gpio.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "SipServer.hpp"
#include "HttpServer.hpp"
#include "OtaUpdater.hpp"

// ── Tag for ESP_LOG ────────────────────────────────────────────────────────
static const char* TAG = "SipServerLAN8720";

// ── LAN8720 RMII Pin Mapping (LilyGO T-Internet-COM) ──────────────────────
//    From example/Arduino/ETHDemo/config.h. Verify against your board revision.
#define LAN8720_MDC_GPIO       23   // ETH_MDC_PIN
#define LAN8720_MDIO_GPIO      18   // ETH_MDIO_PIN
#define LAN8720_PHY_ADDR        0   // ETH_ADDR
#define LAN8720_POWER_GPIO      4   // ETH_POWER_PIN — PHY enable, driven HIGH before init
// The Arduino BSP only toggles ETH_POWER_PIN (GPIO4); it does not drive a separate
// PHY reset through the IDF PHY driver. config.h also defines NRST=5, but on this
// board that is believed to be the 4G modem reset, NOT the LAN8720 — so leave the
// PHY reset unmanaged here (-1) and let the power-enable bring the PHY up. If your
// board wires a dedicated PHY reset, set this to that GPIO instead.
#define LAN8720_PHY_RST_GPIO   -1

// ── Static IP fallback (set USE_STATIC_IP to 1 to skip DHCP) ──────────────
#define USE_STATIC_IP     0
#define STATIC_IP         "192.168.1.200"
#define STATIC_GATEWAY    "192.168.1.1"
#define STATIC_NETMASK    "255.255.255.0"

// ── SIP configuration ─────────────────────────────────────────────────────
#define SIP_PORT          5060

// ── Event group bits ──────────────────────────────────────────────────────
#define ETH_CONNECTED_BIT BIT0
#define ETH_GOT_IP_BIT    BIT1

static EventGroupHandle_t s_eth_event_group = nullptr;
static esp_netif_t*       s_eth_netif       = nullptr;
static std::string        s_ip_addr;

// Global SIP server pointer so the HTTP dashboard task can reach the handler
static SipServer*         g_sipServer = nullptr;

#define HTTP_DASHBOARD_PORT 80

// ── Event handlers ────────────────────────────────────────────────────────

static void eth_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    switch (event_id)
    {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet link UP");
            xEventGroupSetBits(s_eth_event_group, ETH_CONNECTED_BIT);
            break;

        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet link DOWN");
            xEventGroupClearBits(s_eth_event_group, ETH_CONNECTED_BIT | ETH_GOT_IP_BIT);
            break;

        default:
            break;
    }
}

static void ip_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    if (event_id == IP_EVENT_ETH_GOT_IP)
    {
        ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
        char buf[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, buf, sizeof(buf));
        s_ip_addr = buf;

        ESP_LOGI(TAG, "IP:      %s", buf);
        esp_ip4addr_ntoa(&event->ip_info.gw, buf, sizeof(buf));
        ESP_LOGI(TAG, "Gateway: %s", buf);
        esp_ip4addr_ntoa(&event->ip_info.netmask, buf, sizeof(buf));
        ESP_LOGI(TAG, "Netmask: %s", buf);

        xEventGroupSetBits(s_eth_event_group, ETH_GOT_IP_BIT);
    }
}

// ── Ethernet initialisation (internal EMAC + LAN8720 over RMII) ────────────

static esp_eth_handle_t eth_init_lan8720(void)
{
    // ── Power-enable the PHY ────────────────────────────────────────────
    // Drive the enable/power GPIO high and give the LAN8720 a moment to come
    // out of power-down before we drive the RMII clock and touch MDIO.
    gpio_config_t pwr = {};
    pwr.mode         = GPIO_MODE_OUTPUT;
    pwr.pin_bit_mask = 1ULL << LAN8720_POWER_GPIO;
    ESP_ERROR_CHECK(gpio_config(&pwr));
    gpio_set_level(static_cast<gpio_num_t>(LAN8720_POWER_GPIO), 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // ── Internal EMAC (RMII) config ─────────────────────────────────────
    // Start from the IDF default (sets sane dma_burst_len / intr_priority /
    // interface=RMII), then pin the board-specific SMI pins and clock source.
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num  = LAN8720_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = LAN8720_MDIO_GPIO;
    emac_cfg.interface         = EMAC_DATA_INTERFACE_RMII;
    // ESP32 sources the 50 MHz RMII reference clock from its internal APLL and
    // outputs it on GPIO0 — this is the board's ETH_CLOCK_GPIO0_OUT wiring.
    // NOTE: GPIO0 is also a boot-strapping pin; driving the clock out of it can
    // interfere with auto-download mode. If flashing becomes flaky, hold the
    // BOOT button (or break the clock) during the esptool sync.
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
    emac_cfg.clock_config.rmii.clock_gpio = EMAC_APPL_CLK_OUT_GPIO;  // GPIO0

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_config);

    // ── PHY config ──────────────────────────────────────────────────────
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr       = LAN8720_PHY_ADDR;
    phy_config.reset_gpio_num = LAN8720_PHY_RST_GPIO;
    esp_eth_phy_t* phy = esp_eth_phy_new_lan87xx(&phy_config);

    // ── Install driver ──────────────────────────────────────────────────
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = nullptr;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    // ── Set MAC address from ESP32 efuse ────────────────────────────────
    uint8_t mac_addr[6];
    esp_read_mac(mac_addr, ESP_MAC_ETH);
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));

    return eth_handle;
}

// ── SIP server task ───────────────────────────────────────────────────────

static void sip_server_task(void* pvParameters)
{
    ESP_LOGI(TAG, "Starting SipServer on %s:%d", s_ip_addr.c_str(), SIP_PORT);

    g_sipServer = new SipServer(s_ip_addr, SIP_PORT);
    ESP_LOGI(TAG, "SIP server is RUNNING.  Point softphones at %s:%d",
             s_ip_addr.c_str(), SIP_PORT);

    unsigned long lastHeartbeat = 0;
    while (true)
    {
        if (g_sipServer) {
            g_sipServer->getHandler().tick();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        unsigned long nowSec = (unsigned long)(esp_timer_get_time() / 1000000);
        if (nowSec - lastHeartbeat >= 30)
        {
            lastHeartbeat = nowSec;
            ESP_LOGI(TAG, "Heartbeat — IP: %s  Uptime: %lus",
                     s_ip_addr.c_str(), nowSec);
        }
    }

    vTaskDelete(nullptr);
}

static void http_server_task(void* pvParameters)
{
    // Wait for SIP server to initialize
    while (g_sipServer == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Starting CGA CRT Dashboard on %s:%d", s_ip_addr.c_str(), HTTP_DASHBOARD_PORT);

    HttpServer http(s_ip_addr, HTTP_DASHBOARD_PORT, &g_sipServer->getHandler());
    http.start();
    ESP_LOGI(TAG, "CGA CRT Dashboard RUNNING at http://%s:%d/",
             s_ip_addr.c_str(), HTTP_DASHBOARD_PORT);

    // OTA rollback confirmation (see docs/OTA.md): after a few seconds of healthy
    // operation, confirm this image so the bootloader won't roll it back on the
    // next reset. No-op unless the running image is pending verify.
    int otaSettleSec = 0;
    bool otaConfirmed = false;
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!otaConfirmed && ++otaSettleSec >= 5)
        {
            otaConfirmed = true;
            if (OtaUpdater::isPendingVerify())
            {
                OtaUpdater::markValid();
                ESP_LOGI(TAG, "OTA: new image confirmed valid after healthy boot");
            }
        }
    }

    vTaskDelete(nullptr);
}

// ── app_main ──────────────────────────────────────────────────────────────

extern "C" void app_main(void)
{
    // ── NVS (required by some drivers) ──────────────────────────────────
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ── Networking stack ────────────────────────────────────────────────
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_eth_event_group = xEventGroupCreate();

    // ── Register event handlers ─────────────────────────────────────────
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               &eth_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               &ip_event_handler, nullptr));

    // ── Create default netif for Ethernet ───────────────────────────────
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);

    // ── Initialise LAN8720 (internal EMAC over RMII) ────────────────────
    esp_eth_handle_t eth_handle = eth_init_lan8720();

    // ── Glue driver to netif ────────────────────────────────────────────
    esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(eth_handle));

    // ── Static IP (optional) ────────────────────────────────────────────
#if USE_STATIC_IP
    esp_netif_dhcpc_stop(s_eth_netif);
    esp_netif_ip_info_t ip_info = {};
    esp_netif_str_to_ip4(STATIC_IP,      &ip_info.ip);
    esp_netif_str_to_ip4(STATIC_GATEWAY,  &ip_info.gw);
    esp_netif_str_to_ip4(STATIC_NETMASK,  &ip_info.netmask);
    esp_netif_set_ip_info(s_eth_netif, &ip_info);
    ESP_LOGI(TAG, "Static IP configured: %s", STATIC_IP);
#endif

    // ── Start Ethernet ──────────────────────────────────────────────────
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    ESP_LOGI(TAG, "Waiting for Ethernet link + IP ...");

    // ── Wait for IP ─────────────────────────────────────────────────────
    EventBits_t bits = xEventGroupWaitBits(s_eth_event_group,
                                           ETH_GOT_IP_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(30000));
    if (!(bits & ETH_GOT_IP_BIT))
    {
        ESP_LOGE(TAG, "FATAL: No IP after 30 s.  Check cable / link / DHCP.");
        return;
    }

    // ── Launch SIP server on Core 1 ─────────────────────────────────────
    xTaskCreatePinnedToCore(&sip_server_task, "sip_server", 8192, nullptr, 5, nullptr, 1);

    // ── Launch HTTP dashboard on Core 0 ─────────────────────────────────
    xTaskCreatePinnedToCore(&http_server_task, "http_dashboard", 8192, nullptr, 4, nullptr, 0);
}
