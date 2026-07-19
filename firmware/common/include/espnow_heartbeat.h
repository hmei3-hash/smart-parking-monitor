#ifndef ESPNOW_HEARTBEAT_H
#define ESPNOW_HEARTBEAT_H

#include <stdint.h>
#include "esp_err.h"

/**
 * Initialize ESP-NOW heartbeat subsystem.
 */
esp_err_t heartbeat_init(void);

/**
 * Send a heartbeat packet to all registered peers.
 */
esp_err_t heartbeat_send(void);

/**
 * Register a callback invoked when a peer's heartbeat times out.
 */
void heartbeat_register_timeout_cb(void (*cb)(uint8_t *peer_mac));

/**
 * FreeRTOS task function for periodic heartbeat transmission and monitoring.
 */
void heartbeat_task(void *arg);

#endif /* ESPNOW_HEARTBEAT_H */
