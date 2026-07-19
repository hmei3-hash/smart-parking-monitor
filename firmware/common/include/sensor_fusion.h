#ifndef SENSOR_FUSION_H
#define SENSOR_FUSION_H

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    FUSION_NORMAL,
    FUSION_OCCUPIED,
    FUSION_OCCLUSION,
    FUSION_SENSOR_FAULT
} fusion_result_t;

/**
 * Initialize the sensor fusion module.
 */
esp_err_t fusion_init(void);

/**
 * Update fusion state with latest pressure and vision readings.
 */
fusion_result_t fusion_update(bool pressure_occupied, bool vision_occupied);

#endif /* SENSOR_FUSION_H */
