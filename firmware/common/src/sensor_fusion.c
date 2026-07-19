#include "sensor_fusion.h"
#include "esp_log.h"

static const char *TAG = "sensor_fusion";

esp_err_t fusion_init(void)
{
    ESP_LOGI(TAG, "Sensor fusion module initialized");
    return ESP_OK;
}

fusion_result_t fusion_update(bool pressure_occupied, bool vision_occupied)
{
    if (pressure_occupied && vision_occupied) {
        return FUSION_OCCUPIED;
    } else if (!pressure_occupied && !vision_occupied) {
        return FUSION_NORMAL;
    } else if (pressure_occupied && !vision_occupied) {
        return FUSION_OCCLUSION;
    } else {
        return FUSION_SENSOR_FAULT;
    }
}
