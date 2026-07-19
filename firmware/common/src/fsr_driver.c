#include "fsr_driver.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "fsr_driver";
static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_channel_t adc_channel;

esp_err_t fsr_driver_init(gpio_num_t adc_pin)
{
    // TODO: Configure ADC oneshot unit for the given pin
    ESP_LOGI(TAG, "FSR driver initialized on GPIO %d", adc_pin);
    return ESP_OK;
}

int fsr_read_raw(void)
{
    int raw = 0;
    // TODO: Read ADC oneshot value
    return raw;
}

bool fsr_is_occupied(int threshold)
{
    return fsr_read_raw() > threshold;
}
