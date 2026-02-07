/*
 * ota_manager.h
 *
 *  Created on: Jan 2, 2026
 *      Author: sean-shk
 */

#ifndef INC_OTA_MANAGER_H_
#define INC_OTA_MANAGER_H_

#include "ota_protocol.h"
#include <stdint.h>
#include <stddef.h>

// OTA state machine states
typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_RECEIVING_HEADER,
    OTA_STATE_RECEIVING_DATA,
    OTA_STATE_VERIFYING,
    OTA_STATE_FINALIZING,
    OTA_STATE_COMPLETE,
    OTA_STATE_ERROR
} ota_state_t;

// OTA context - keeps track of transfer progress
typedef struct {
    ota_state_t state;
    uint32_t target_bank_address;
    uint32_t firmware_size;
    uint32_t firmware_version;
    uint32_t firmware_crc32;
    uint32_t total_chunks;
    uint32_t chunks_received;
    uint32_t expected_chunk_number;
    uint32_t bytes_written;
    uint8_t error_code;
} ota_context_t;

// Public API
void ota_init(ota_context_t *ctx);
uint32_t calculate_crc32(const void *data, size_t length);
void ota_process_start_packet(ota_context_t *ctx, const ota_start_packet_t *pkt);
void ota_process_data_packet(ota_context_t *ctx, const ota_data_packet_t *pkt);
void ota_process_end_packet(ota_context_t *ctx, const ota_end_packet_t *pkt);
void ota_send_response(const ota_context_t *ctx, uint8_t packet_type);
int ota_erase_bank(uint32_t bank_address);
int ota_update_boot_state(const ota_context_t *ctx);

#endif /* INC_OTA_MANAGER_H_ */
