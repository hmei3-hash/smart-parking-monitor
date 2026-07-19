#include "espnow_heartbeat.h"
#include "esp_now.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "heartbeat";
static void (*timeout_cb)(uint8_t *peer_mac) = NULL;

esp_err_t heartbeat_init(void)
{
    // TODO: Initialize ESP-NOW, register send/recv callbacks
    ESP_LOGI(TAG, "Heartbeat subsystem initialized");
    return ESP_OK;
}

esp_err_t heartbeat_send(void)
{
    // TODO: Broadcast heartbeat packet to all peers
    return ESP_OK;
}

void heartbeat_register_timeout_cb(void (*cb)(uint8_t *peer_mac))
{
    timeout_cb = cb;
}

void heartbeat_task(void *arg)
{
    while (1) {
        // TODO: Send heartbeat, check peer timeouts
        heartbeat_send();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
