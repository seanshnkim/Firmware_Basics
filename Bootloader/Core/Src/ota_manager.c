/*
 * ota_manager.c
 *
 *  Created on: Jan 2, 2026
 *      Author: sean-shk
 */
#include "ota_manager.h"
#include "boot_state.h"
#include "main.h"
#include <stdio.h>
#include <string.h>


void ota_init(ota_context_t *ctx) {
    ctx->state = OTA_STATE_IDLE;
    ctx->target_bank_address = 0;
    ctx->firmware_size = 0;
    ctx->firmware_version = 0;
    ctx->firmware_crc32 = 0;
    ctx->total_chunks = 0;
    ctx->chunks_received = 0;
    ctx->expected_chunk_number = 0;
    ctx->bytes_written = 0;
    ctx->error_code = OTA_ERR_NONE;
}

static uint32_t calculate_crc32(const void *data, size_t length) {
    __HAL_CRC_DR_RESET(&hcrc);

    const uint32_t *words = (const uint32_t*)data;
    size_t num_words = length / 4;

    // Process full words
    uint32_t crc = 0;
    if (num_words > 0) {
        crc = HAL_CRC_Calculate(&hcrc, (uint32_t*)words, num_words);
    }

    // Process remaining bytes (if any)
    size_t remaining = length % 4;
    if (remaining > 0) {
        uint32_t last_word = 0;
        memcpy(&last_word, (uint8_t*)data + num_words * 4, remaining);
        crc = HAL_CRC_Accumulate(&hcrc, &last_word, 1);
    }

    return crc;
}

/**
 * @brief Get the currently active bank address
 * @return Bank A or Bank B address, or 0 if unknown
 */
static uint32_t ota_get_current_bank(void) {
    uint32_t vtor = SCB->VTOR;

    if (vtor == BANK_A_ADDRESS) {
        return BANK_A_ADDRESS;
    } else if (vtor == BANK_B_ADDRESS) {
        return BANK_B_ADDRESS;
    }

    return 0;  // Unknown/invalid
}

/**
 * @brief Get the inactive bank address (for OTA target)
 * @return Inactive bank address, or 0 if can't determine
 */
static uint32_t ota_get_inactive_bank(void) {
    uint32_t current = ota_get_current_bank();

    if (current == BANK_A_ADDRESS) {
        return BANK_B_ADDRESS;
    } else if (current == BANK_B_ADDRESS) {
        return BANK_A_ADDRESS;
    }

    return 0;  // Error
}

/**
 * @brief Erase a bank's flash sectors
 * @param bank_address Starting address of bank (BANK_A_ADDRESS or BANK_B_ADDRESS)
 * @return 0 on success, -1 on failure
 */
int ota_erase_bank(uint32_t bank_address) {
    printf("Erasing bank at 0x%08lX...\r\n", bank_address);

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase_config;
    erase_config.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase_config.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    uint32_t sector_error = 0;

    if (bank_address == BANK_A_ADDRESS) {
        // Erase sectors 4-5
        erase_config.Sector = FLASH_SECTOR_4;
        erase_config.NbSectors = 2;  // Sectors 4, 5
    } else if (bank_address == BANK_B_ADDRESS) {
        // Erase sectors 6-7
        erase_config.Sector = FLASH_SECTOR_6;
        erase_config.NbSectors = 2;  // Sectors 6, 7
    } else {
        HAL_FLASH_Lock();
        return -1;  // Invalid bank
    }

    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase_config, &sector_error);

    HAL_FLASH_Lock();

    if (status != HAL_OK) {
        printf("ERROR: Erase failed! Sector error: %lu\r\n", sector_error);
        return -1;
    }

    printf("Bank erased successfully!\r\n");
    return 0;
}

extern UART_HandleTypeDef huart1;  // Assuming you use UART1, adjust if different

void ota_send_response(const ota_context_t *ctx, uint8_t packet_type) {
    ota_response_packet_t response;

    response.magic = OTA_MAGIC_START;
    response.packet_type = packet_type;
    response.error_code = ctx->error_code;
    response.last_chunk_received = ctx->chunks_received;

    // Send response packet over UART
    HAL_UART_Transmit(&huart1, (uint8_t*)&response, sizeof(response), 1000);

    if (packet_type == OTA_PKT_ACK) {
        printf("Sent ACK (chunks received: %lu)\r\n", ctx->chunks_received);
    } else {
        printf("Sent NACK (error code: %d)\r\n", ctx->error_code);
    }
}

void ota_process_start_packet(ota_context_t *ctx, const ota_start_packet_t *pkt) {
    printf("\r\n=== OTA START Packet Received ===\r\n");

    // Check 1: Are we in IDLE state?
    if (ctx->state != OTA_STATE_IDLE) {
        printf("ERROR: Not in IDLE state (current: %d)\r\n", ctx->state);
        ctx->error_code = OTA_ERR_SEQUENCE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    // Check 2: Magic number
    if (pkt->magic != OTA_MAGIC_START) {
        printf("ERROR: Invalid magic number\r\n");
        ctx->error_code = OTA_ERR_SEQUENCE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    // Check 3: Firmware size valid?
    // TODO: What's the maximum size? (Hint: Bank size)
    // -> Should be 256KB
    if (pkt->firmware_size == 0 || pkt->firmware_size > BANK_SIZE) {
        printf("ERROR: Invalid firmware size: %lu\r\n", pkt->firmware_size);
        ctx->error_code = OTA_ERR_SIZE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    // Check 4: Target bank must be the INACTIVE bank
    uint32_t inactive_bank = ota_get_inactive_bank(); // It returns 0 if error

    if (inactive_bank == 0) {
        printf("ERROR: Cannot determine current bank\r\n");
        ctx->error_code = OTA_ERR_SEQUENCE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    uint32_t requested_bank = (pkt->target_bank == BANK_A) ? BANK_A_ADDRESS : BANK_B_ADDRESS;

    if (requested_bank != inactive_bank) {
        printf("ERROR: Target bank must be inactive bank\r\n");
        printf("  Current (active): 0x%08lX\r\n", ota_get_current_bank());
        printf("  Requested target: 0x%08lX\r\n", requested_bank);
        printf("  Required (inactive): 0x%08lX\r\n", inactive_bank);
        ctx->error_code = OTA_ERR_SEQUENCE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    ctx->target_bank_address = inactive_bank;
    printf("Target bank set to: 0x%08lX\r\n", ctx->target_bank_address);

    // Erase the target bank
    if (ota_erase_bank(ctx->target_bank_address) != 0) {
        printf("ERROR: Failed to erase target bank\r\n");
        ctx->error_code = OTA_ERR_FLASH;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    // Update context with transfer info
    ctx->firmware_size = pkt->firmware_size;
    ctx->firmware_version = pkt->firmware_version;
    ctx->firmware_crc32 = pkt->firmware_crc32;
    ctx->total_chunks = pkt->total_chunks;
    ctx->chunks_received = 0;
    ctx->expected_chunk_number = 0;
    ctx->bytes_written = 0;

    // Transition to RECEIVING_DATA state
    ctx->state = OTA_STATE_RECEIVING_DATA;

    printf("Ready to receive firmware!\r\n");
    ota_send_response(ctx, OTA_PKT_ACK);
}

/**
 * @brief Unified flash write function (handles any size)
 * @param address Flash address to write to
 * @param data Pointer to data to write
 * @param size Number of bytes to write
 * @return 0 on success, -1 on failure
 */
static int write_to_flash_unified(uint32_t address, const void *data, uint16_t size) {
    HAL_FLASH_Unlock();

    const uint32_t *words = (const uint32_t*)data;
    uint16_t num_full_words = size / 4;

    // Write full words
    for (int i = 0; i < num_full_words; i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, words[i]) != HAL_OK) {
            HAL_FLASH_Lock();
            return -1;
        }
        address += 4;
    }

    // Handle remaining bytes (if any)
    uint16_t remaining = size % 4;
    if (remaining > 0) {
        uint32_t last_word = 0xFFFFFFFF;
        memcpy(&last_word, (uint8_t*)data + num_full_words * 4, remaining);

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, last_word) != HAL_OK) {
            HAL_FLASH_Lock();
            return -1;
        }
    }

    HAL_FLASH_Lock();
    return 0;
}

void ota_process_data_packet(ota_context_t *ctx, const ota_data_packet_t *pkt) {
    // Check 1: Are we in RECEIVING_DATA state?
    if (ctx->state != OTA_STATE_RECEIVING_DATA) {
        printf("ERROR: Not in RECEIVING_DATA state\r\n");
        ctx->error_code = OTA_ERR_SEQUENCE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    // Check 2: Magic number
    if (pkt->magic != OTA_MAGIC_DATA) {
        printf("ERROR: Invalid data packet magic\r\n");
        ctx->error_code = OTA_ERR_SEQUENCE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    // Check 3: Is this the expected chunk number?
    if (pkt->chunk_number != ctx->expected_chunk_number) {
        printf("ERROR: Wrong chunk number (expected %lu, got %lu)\r\n",
               ctx->expected_chunk_number, pkt->chunk_number);
        ctx->error_code = OTA_ERR_SEQUENCE;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    // Check 4: Verify chunk CRC
    uint32_t calculated_crc = calculate_crc32(pkt->data, pkt->chunk_size);
    if (calculated_crc != pkt->chunk_crc32) {
        printf("ERROR: Chunk CRC mismatch\r\n");
        ctx->error_code = OTA_ERR_CRC;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    // Check 5: Validate chunk size
    if (pkt->chunk_size == 0 || pkt->chunk_size > OTA_CHUNK_SIZE) {
        printf("ERROR: Invalid chunk size: %u\r\n", pkt->chunk_size);
        ctx->error_code = OTA_ERR_SIZE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    // Additional check: Last chunk can be smaller, but not other chunks
    if (pkt->chunk_number < ctx->total_chunks - 1) {
        // Not the last chunk - must be full size
        if (pkt->chunk_size != OTA_CHUNK_SIZE) {
            printf("ERROR: Non-last chunk must be %d bytes\r\n", OTA_CHUNK_SIZE);
            ctx->error_code = OTA_ERR_SIZE;
            ctx->state = OTA_STATE_ERROR;
            ota_send_response(ctx, OTA_PKT_NACK);
            return;
        }
    }

    printf("Chunk %lu: %u bytes, CRC OK\r\n",
           pkt->chunk_number, pkt->chunk_size);

    // Write chunk to flash
    uint32_t write_address = ctx->target_bank_address + (pkt->chunk_number * OTA_CHUNK_SIZE);

    printf("Writing to 0x%08lX...\r\n", write_address);

    if ( write_to_flash_unified(write_address, pkt->data, pkt->chunk_size) != 0 ) {
    	printf("ERROR: Flash write failed\r\n");
		ctx->error_code = OTA_ERR_FLASH;
		ctx->state = OTA_STATE_ERROR;
		ota_send_response(ctx, OTA_PKT_NACK);
		return;
    }

    // Update context
    ctx->chunks_received++;
    ctx->expected_chunk_number++;
    ctx->bytes_written += pkt->chunk_size;

    // Send ACK
    ota_send_response(ctx, OTA_PKT_ACK);

    // Check if all chunks received
    if (ctx->chunks_received == ctx->total_chunks) {
        printf("All chunks received! Transitioning to VERIFYING...\r\n");
        ctx->state = OTA_STATE_VERIFYING;
    }
}

void ota_process_end_packet(ota_context_t *ctx, const ota_end_packet_t *pkt) {
    printf("\r\n=== OTA END Packet Received ===\r\n");

    // Check 1: Are we in VERIFYING state?
    if (ctx->state != OTA_STATE_VERIFYING) {
        printf("ERROR: Not in VERIFYING state (current: %d)\r\n", ctx->state);
        ctx->error_code = OTA_ERR_SEQUENCE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    // Check 2: Magic number
    if (pkt->magic != OTA_MAGIC_START) {
        printf("ERROR: Invalid END packet magic\r\n");
        ctx->error_code = OTA_ERR_SEQUENCE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    printf("Verifying firmware integrity...\r\n");
    printf("  Expected size: %lu bytes\r\n", ctx->firmware_size);
    printf("  Bytes written: %lu bytes\r\n", ctx->bytes_written);
    printf("  Expected CRC32: 0x%08lX\r\n", ctx->firmware_crc32);

    // Check 3: Verify total bytes written
    if (ctx->bytes_written != ctx->firmware_size) {
        printf("ERROR: Size mismatch!\r\n");
        ctx->error_code = OTA_ERR_SIZE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    // Check 4: Calculate CRC32 of entire firmware in flash
    printf("Calculating firmware CRC32 (this may take a moment)...\r\n");

    uint32_t calculated_crc = ota_calculate_firmware_crc32(
        ctx->target_bank_address,
        ctx->firmware_size
    );

    printf("  Calculated CRC32: 0x%08lX\r\n", calculated_crc);

    // Check 5: Compare CRC32
    if (calculated_crc != ctx->firmware_crc32) {
        printf("ERROR: CRC32 mismatch! Firmware is corrupted.\r\n");
        ctx->error_code = OTA_ERR_CRC;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    printf("✓ Firmware verification PASSED!\r\n");

    // Transition to FINALIZING
    ctx->state = OTA_STATE_FINALIZING;

    // Update boot state to mark new firmware as valid
    if (ota_update_boot_state(ctx) != 0) {
        printf("ERROR: Failed to update boot state\r\n");
        ctx->error_code = OTA_ERR_FLASH;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    printf("✓ Boot state updated!\r\n");
    printf("✓ OTA update complete!\r\n");
    printf("  New firmware version: %lu\r\n", ctx->firmware_version);
    printf("  Installed at: 0x%08lX\r\n", ctx->target_bank_address);

    ctx->state = OTA_STATE_COMPLETE;
    ota_send_response(ctx, OTA_PKT_ACK);

    printf("\r\nReboot the device to run the new firmware.\r\n");
}

/**
 * @brief Calculate CRC32 of firmware stored in flash
 * @param address Starting address of firmware
 * @param size Size of firmware in bytes
 * @return CRC32 value
 */
static uint32_t ota_calculate_firmware_crc32(uint32_t address, uint32_t size) {
    __HAL_CRC_DR_RESET(&hcrc);

    const uint32_t BUFFER_SIZE = 1024;  // 1KB buffer
    uint32_t remaining = size;
    uint32_t offset = 0;

    while (remaining > 0) {
        uint32_t chunk_size = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;

        // Read directly from flash (it's memory-mapped, so we can just read it)
        const uint8_t *flash_data = (const uint8_t*)(address + offset);

        // Calculate CRC for this chunk
        uint32_t num_words = chunk_size / 4;
        if (num_words > 0) {
            if (offset == 0) {
                // First chunk - use Calculate
                HAL_CRC_Calculate(&hcrc, (uint32_t*)flash_data, num_words);
            } else {
                // Subsequent chunks - use Accumulate
                HAL_CRC_Accumulate(&hcrc, (uint32_t*)flash_data, num_words);
            }
        }

        // Handle remaining bytes in this chunk
        uint32_t chunk_remaining = chunk_size % 4;
        if (chunk_remaining > 0) {
            uint32_t last_word = 0;
            memcpy(&last_word, flash_data + (num_words * 4), chunk_remaining);
            HAL_CRC_Accumulate(&hcrc, &last_word, 1);
        }

        offset += chunk_size;
        remaining -= chunk_size;
    }

    return hcrc.Instance->DR;  // Read final CRC value
}

/**
 * @brief Update boot state after successful OTA
 * @param ctx OTA context
 * @return 0 on success, -1 on failure
 */
static int ota_update_boot_state(const ota_context_t *ctx) {
    boot_state_t new_state;

    // Determine which bank we just updated
    uint32_t updated_bank;
    if (ctx->target_bank_address == BANK_A_ADDRESS) {
        updated_bank = BANK_A;
    } else {
        updated_bank = BANK_B;
    }

    // Create new boot state
    new_state.magic_number = BOOT_STATE_MAGIC;
    new_state.active_bank = updated_bank;  // Switch to new bank
    new_state.crc32 = 0;  // Will be calculated by boot_state_write

    // Mark updated bank as VALID
    if (updated_bank == BANK_A) {
        new_state.bank_a_status = BANK_STATUS_VALID;
        new_state.bank_b_status = BANK_STATUS_INVALID;  // Old firmware
    } else {
        new_state.bank_a_status = BANK_STATUS_INVALID;  // Old firmware
        new_state.bank_b_status = BANK_STATUS_VALID;
    }

    // Erase and write boot state
    if (boot_state_erase() != 0) {
        return -1;
    }

    if (boot_state_write(&new_state) != 0) {
        return -1;
    }

    return 0;
}
