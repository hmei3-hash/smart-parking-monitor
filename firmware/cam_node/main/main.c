#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "fsr_driver.h"
#include "espnow_heartbeat.h"
#include "sensor_fusion.h"

static const char *TAG = "cam_node";

static SemaphoreHandle_t pressure_trigger_sem;
static QueueHandle_t inference_result_queue;
static SemaphoreHandle_t feature_buffer_mutex;

#define FSR_ADC_PIN         GPIO_NUM_1
#define FSR_THRESHOLD       2000
#define PRESSURE_TASK_STACK 4096
#define INFERENCE_TASK_STACK 8192
#define HEARTBEAT_TASK_STACK 4096
#define UPLOAD_TASK_STACK   4096

typedef struct {
    uint8_t class_id;
    float confidence;
    uint16_t bbox[4];
} inference_result_t;

static void pressure_polling_task(void *arg)
{
    while (1) {
        // TODO: Poll FSR sensor at configured interval
        if (fsr_is_occupied(FSR_THRESHOLD)) {
            xSemaphoreGive(pressure_trigger_sem);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void image_inference_task(void *arg)
{
    while (1) {
        // TODO: Wait for pressure trigger, capture image, run TFLite Micro inference
        if (xSemaphoreTake(pressure_trigger_sem, portMAX_DELAY) == pdTRUE) {
            // TODO: Capture camera frame
            // TODO: Run TFLite Micro inference
            // TODO: Post result to queue
            inference_result_t result = {0};
            xQueueSend(inference_result_queue, &result, portMAX_DELAY);
        }
    }
}

static void heartbeat_monitor_task(void *arg)
{
    heartbeat_task(arg);
}

static void wifi_upload_task(void *arg)
{
    while (1) {
        // TODO: Wait for inference result, upload session data via MQTT
        inference_result_t result;
        if (xQueueReceive(inference_result_queue, &result, portMAX_DELAY) == pdTRUE) {
            xSemaphoreTake(feature_buffer_mutex, portMAX_DELAY);
            // TODO: Update shared feature buffer
            // TODO: Upload to gateway via WiFi/MQTT
            xSemaphoreGive(feature_buffer_mutex);
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "CAM node starting...");

    pressure_trigger_sem = xSemaphoreCreateBinary();
    inference_result_queue = xQueueCreate(4, sizeof(inference_result_t));
    feature_buffer_mutex = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(fsr_driver_init(FSR_ADC_PIN));
    ESP_ERROR_CHECK(heartbeat_init());
    ESP_ERROR_CHECK(fusion_init());

    // Core 0: Sensing & Inference
    xTaskCreatePinnedToCore(pressure_polling_task, "pressure_poll", PRESSURE_TASK_STACK,
                            NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(image_inference_task, "img_inference", INFERENCE_TASK_STACK,
                            NULL, 4, NULL, 0);

    // Core 1: Communication
    xTaskCreatePinnedToCore(heartbeat_monitor_task, "heartbeat", HEARTBEAT_TASK_STACK,
                            NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(wifi_upload_task, "wifi_upload", UPLOAD_TASK_STACK,
                            NULL, 4, NULL, 1);
}
