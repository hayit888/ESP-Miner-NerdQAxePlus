#include <string.h>

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_task_wdt.h"
#include "mbedtls/platform.h"
#include "nvs_flash.h"

#include "apis_task.h"
#include "asic_jobs.h"
#include "asic_result_task.h"
#include "boards/board.h"
#include "boards/nerdaxe.h"
#include "boards/nerdaxegamma.h"
#include "boards/nerdaxegaia.h"
#include "boards/nerdeko.h"
#include "boards/nerdhaxegamma.h"
#include "boards/nerdoctaxegamma.h"
#include "boards/nerdoctaxeplus.h"
#include "boards/nerdqaxeplus.h"
#include "boards/nerdqaxeplus2.h"
#include "boards/nerdqx.h"
#include "boards/q1370.h"
#include "boards/q1373.h"
#include "create_jobs_task.h"
#include "discord.h"
#include "global_state.h"
#include "hashrate_monitor_task.h"
#include "history.h"
#include "http_server.h"
#include "influx_task.h"
#include "macros.h"
#include "main.h"
#include "nvs_config.h"
#include "otp/otp.h"
#include "ping_task.h"
#include "serial.h"
#include "stratum/stratum_manager_fallback.h"
#include "stratum/stratum_manager_dual_pool.h"
#include "system.h"
#include "wifi_health.h"
#include "can_task.h"
#include "can_slave_task.h"
#include "can_master_task.h"
#include "guards.h"
#include "utils.h"
#include "network/mdns_service.h"
#include "network/network_manager.h"

#define STRATUM_WATCHDOG_TIMEOUT_SECONDS 86400

System SYSTEM_MODULE;

PowerManagementTask POWER_MANAGEMENT_MODULE;
HashrateMonitor HASHRATE_MONITOR;

StratumManager *STRATUM_MANAGER = nullptr;
APIsFetcher APIs_FETCHER;
FactoryOTAUpdate FACTORY_OTA_UPDATER;

DiscordAlerter discordAlerter;

AsicJobs asicJobs;
EXT_RAM_BSS_ATTR AsicJobs slaveAsicJobs[CAN_SLAVE_MAX];

OTP otp;
SNTP sntp;

NetworkManager NETWORK;

static const char *TAG = "nerd*axe";

#ifndef CONFIG_SPIRAM
#error "firmware will not work without psram"
#endif

uint64_t now_ms()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t) tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
}

uint32_t now()
{
    return (uint32_t) (now_ms() / 1000ull);
}

bool is_time_synced(void)
{
    return (sntp.isTimeSynced() && now() >= 1609459200);
}

static void request_stratum_reconnect()
{
    if (!STRATUM_MANAGER) {
        return;
    }
    STRATUM_MANAGER->reconnectAll();
}

static void on_preferred_changed()
{
    ESP_LOGW(TAG, "Network preferred route changed -> request stratum reconnect");
    request_stratum_reconnect();
}

static void setup_network(bool hasEth)
{
    char *wifi_ssid = Config::getWifiSSID();
    char *wifi_pass = Config::getWifiPass();
    char *hostname  = Config::getHostname();

    MemoryGuard gSsid(wifi_ssid);
    MemoryGuard gWifiPass(wifi_pass);
    MemoryGuard gHostname(hostname);

    SYSTEM_MODULE.setSsid(wifi_ssid);

    NETWORK.setHookPreferredChanged(on_preferred_changed);

    ESP_ERROR_CHECK(NETWORK.start(wifi_ssid, wifi_pass, hostname, hasEth));

    start_rest_server(NULL);

    for (;;) {
        EventBits_t bits = NETWORK.waitAnyIpMs(pdMS_TO_TICKS(30000));

        if (bits != 0) {
            if (NETWORK.hasEthIp()) {
                ESP_LOGI(TAG, "Network up via ETH (preferred)");
                SYSTEM_MODULE.setWifiStatus("LAN Connected!");
            } else {
                ESP_LOGI(TAG, "Network up via WiFi");
                SYSTEM_MODULE.setWifiStatus("WiFi Connected!");
            }
            mdns_service_start(hostname, SYSTEM_MODULE.getBoard());
            return;
        }

        ESP_LOGI(TAG, "Waiting for network IP... (AP is available for config)");
    }
}

void initWatchdog()
{
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = STRATUM_WATCHDOG_TIMEOUT_SECONDS * 1000,
        .idle_core_mask = 0,
        .trigger_panic = false
    };

    esp_err_t result = esp_task_wdt_init(&wdt_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize watchdog: %d\n", result);
    }
}

StratumManager* newStratumManager() {
    int mode = (int) Config::getPoolMode();
    switch (mode) {
    case 0:
        return new StratumManagerFallback();
    case 1:
        return new StratumManagerDualPool();
    default:
        ESP_LOGE(TAG, "invalid pool mode %d", (int) mode);
        return nullptr;
    }
}

void *psram_calloc(size_t num, size_t size)
{
    void *ptr = CALLOC(num, size);
    if (!ptr) {
        ESP_LOGE(TAG, "PSRAM allocation failed! Falling back to internal RAM.");
        ptr = heap_caps_calloc(num, size, MALLOC_CAP_DEFAULT);
    }
    return ptr;
}

void free_psram(void *ptr)
{
    heap_caps_free(ptr);
}

extern "C" void app_main(void)
{
    initWatchdog();

    mbedtls_platform_set_calloc_free(psram_calloc, free_psram);

    ESP_LOGI(TAG, "Welcome to the Nerd*Axe - hack the planet!");
    ESP_ERROR_CHECK(nvs_flash_init());

    esp_reset_reason_t reason = SYSTEM_MODULE.showLastResetReason();

    Config::migrate_config();

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(esp_netif_init());

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

#ifdef NERDQAXEPLUS
    Board *board = new NerdQaxePlus();
#endif
#ifdef NERDQAXEPLUS2
    Board *board = new NerdQaxePlus2();
#endif
#ifdef NERDOCTAXEPLUS
    Board *board = new NerdOctaxePlus();
#endif
#ifdef NERDAXE
    Board *board = new NerdAxe();
#endif
#ifdef NERDOCTAXEGAMMA
    Board *board = new NerdOctaxeGamma();
#endif
#ifdef NERDAXEGAMMA
    Board *board = new NerdaxeGamma();
#endif
#ifdef NERDAXEGAIA
    Board *board = new NerdaxeGaia();
#endif
#ifdef NERDHAXEGAMMA
    Board *board = new NerdHaxeGamma();
#endif
#ifdef NERDEKO
    Board *board = new NerdEko();
#endif
#ifdef NERDQX
    Board *board = new NerdQX();
#endif
#ifdef Q1370
    Board *board = new Q1370B();
#endif
#ifdef Q1373
    Board *board = new Q1373B();
#endif

    if (board->hasEthernet()) {
        const EthPins *ep = board->getEthPins();
        if (ep) {
            NETWORK.setEthPins(ep->sclk, ep->mosi, ep->miso, ep->cs, ep->rst, ep->irq);
        }
        NETWORK.earlyEthSpiInit();
    }

    board->loadSettings();
    board->initBoard();

    SYSTEM_MODULE.setBoard(board);

    size_t total_psram = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM found with %dMB", total_psram / (1024 * 1024));
    ESP_LOGI(TAG, "Found Device Model: %s", board->getDeviceModel());
    ESP_LOGI(TAG, "Found Board Version: %d", board->getVersion());

    uint64_t best_diff = Config::getBestDiff();
    bool should_self_test = Config::isSelfTestEnabled();
    if (should_self_test && !best_diff) {
        board->selfTest();
        vTaskDelay(pdMS_TO_TICKS(60 * 60 * 1000));
    }

    const bool canSlave = board->isCanSlave();

    if (canSlave) {
