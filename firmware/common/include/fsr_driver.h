#ifndef FSR_DRIVER_H
#define FSR_DRIVER_H

#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

/**
 * Initialize the FSR pressure sensor ADC channel.
 */
esp_err_t fsr_driver_init(gpio_num_t adc_pin);

/**
 * Read raw ADC value from the FSR sensor.
 */
int fsr_read_raw(void);

/**
 * Determine if the parking spot is occupied based on pressure threshold.
 */
bool fsr_is_occupied(int threshold);

#endif /* FSR_DRIVER_H */
