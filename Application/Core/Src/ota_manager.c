/*
 * ota_manager.c — one-line fix: ota_send_response transmits on huart2, not huart1
 *
 * ONLY CHANGE from original: line 146
 *   HAL_UART_Transmit(&huart1, ...)  →  HAL_UART_Transmit(&huart2, ...)
 *
 * Everything else is identical to your original ota_manager.c.
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

extern CRC_HandleTypeDef hcrc;

uint32_t calculate_crc32(const void *data, size_t length) {
    __HAL_CRC_DR_RESET(&hcrc);

    const uint32_t *words = (const uint32_t*)data;
    size_t num_words = length / 4;

    uint32_t crc = 0;
    if (num_words > 0) {
        crc = HAL_CRC_Calculate(&hcrc, (uint32_t*)words, num_words);
    }

    size_t remaining = length % 4;
    if (remaining > 0) {
        uint32_t last_word = 0;
        memcpy(&last_word, (uint8_t*)data + num_words * 4, remaining);
        crc = HAL_CRC_Accumulate(&hcrc, &last_word, 1);
    }

    return crc;
}

static uint32_t ota_get_current_bank(void) {
    uint32_t vtor = SCB->VTOR;

    if (vtor == BANK_A_ADDRESS) return BANK_A_ADDRESS;
    if (vtor == BANK_B_ADDRESS) return BANK_B_ADDRESS;

    if (vtor == 0x08000000 || vtor == 0x00000000) {
        printf("DEBUG: Running from bootloader (VTOR=0x%08lX), simulating Bank A\r\n", vtor);
        return BANK_A_ADDRESS;
    }

    printf("WARNING: Unknown VTOR value: 0x%08lX\r\n", vtor);
    return 0;
}

static uint32_t ota_get_inactive_bank(void) {
    uint32_t current = ota_get_current_bank();
    printf("current: 0x%08lX\n", current);

    if (current == BANK_A_ADDRESS) return BANK_B_ADDRESS;
    if (current == BANK_B_ADDRESS) return BANK_A_ADDRESS;

    return 0;
}

int ota_erase_bank(uint32_t bank_address) {
    printf("Erasing bank at 0x%08lX...\r\n", bank_address);

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase_config;
    erase_config.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase_config.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    uint32_t sector_error = 0;

    if (bank_address == BANK_A_ADDRESS) {
        erase_config.Sector = FLASH_SECTOR_4;
        erase_config.NbSectors = 2;
    } else if (bank_address == BANK_B_ADDRESS) {
        erase_config.Sector = FLASH_SECTOR_6;
        erase_config.NbSectors = 2;
    } else {
        HAL_FLASH_Lock();
        return -1;
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

/* ---- FIXED: transmit on huart2 (HM-10), not huart1 (debug VCP) ---- */
extern UART_HandleTypeDef huart2;

void ota_send_response(const ota_context_t *ctx, uint8_t packet_type) {
    ota_response_packet_t response;

    response.magic = OTA_MAGIC_START;
    response.packet_type = packet_type;
    response.error_code = ctx->error_code;
    response.last_chunk_received = ctx->chunks_received;

    HAL_UART_Transmit(&huart2, (uint8_t*)&response, sizeof(response), 1000);  /* <-- FIXED */

    if (packet_type == OTA_PKT_ACK) {
        printf("Sent ACK (chunks received: %lu)\r\n", ctx->chunks_received);
    } else {
        printf("Sent NACK (error code: %d)\r\n", ctx->error_code);
    }
}

void ota_process_start_packet(ota_context_t *ctx, const ota_start_packet_t *pkt) {
    printf("\r\n=== OTA START Packet ===\r\n");

    if (ctx->state != OTA_STATE_IDLE) {
        printf("ERROR: Not in IDLE state (current: %d)\r\n", ctx->state);
        ctx->error_code = OTA_ERR_SEQUENCE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    if (pkt->magic != OTA_MAGIC_START) {
        printf("ERROR: Invalid magic number\r\n");
        ctx->error_code = OTA_ERR_SEQUENCE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    if (pkt->firmware_size == 0 || pkt->firmware_size > BANK_SIZE) {
        printf("ERROR: Invalid firmware size: %lu\r\n", pkt->firmware_size);
        ctx->error_code = OTA_ERR_SIZE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    uint32_t inactive_bank = ota_get_inactive_bank();
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
        printf("  Requested: 0x%08lX  Required: 0x%08lX\r\n", requested_bank, inactive_bank);
        ctx->error_code = OTA_ERR_SEQUENCE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    ctx->target_bank_address = inactive_bank;
    printf("Target bank: 0x%08lX\r\n", ctx->target_bank_address);

    if (ota_erase_bank(ctx->target_bank_address) != 0) {
        printf("ERROR: Failed to erase target bank\r\n");
        ctx->error_code = OTA_ERR_FLASH;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    ctx->firmware_size = pkt->firmware_size;
    ctx->firmware_version = pkt->firmware_version;
    ctx->firmware_crc32 = pkt->firmware_crc32;
    ctx->total_chunks = pkt->total_chunks;
    ctx->chunks_received = 0;
    ctx->expected_chunk_number = 0;
    ctx->bytes_written = 0;
    ctx->state = OTA_STATE_RECEIVING_DATA;

    printf("Ready to receive %lu chunks (%lu bytes)!\r\n",
           ctx->total_chunks, ctx->firmware_size);

    ota_send_response(ctx, OTA_PKT_ACK);
}

static int write_to_flash_unified(uint32_t address, const void *data, uint16_t size) {
    HAL_FLASH_Unlock();

    const uint32_t *words = (const uint32_t*)data;
    uint16_t num_full_words = size / 4;

    for (int i = 0; i < num_full_words; i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, words[i]) != HAL_OK) {
            HAL_FLASH_Lock();
            return -1;
        }
        address += 4;
    }

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
    if (ctx->state != OTA_STATE_RECEIVING_DATA) {
        printf("ERROR: Not in RECEIVING_DATA state\r\n");
        ctx->error_code = OTA_ERR_SEQUENCE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    if (pkt->magic != OTA_MAGIC_DATA) {
        printf("ERROR: Invalid data packet magic\r\n");
        ctx->error_code = OTA_ERR_SEQUENCE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    if (pkt->chunk_number != ctx->expected_chunk_number) {
        printf("ERROR: Wrong chunk (expected %lu, got %lu)\r\n",
               ctx->expected_chunk_number, pkt->chunk_number);
        ctx->error_code = OTA_ERR_SEQUENCE;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    if (pkt->chunk_size == 0 || pkt->chunk_size > OTA_CHUNK_SIZE) {
        printf("ERROR: Invalid chunk size: %u\r\n", pkt->chunk_size);
        ctx->error_code = OTA_ERR_SIZE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    uint32_t calculated_crc = calculate_crc32(pkt->data, pkt->chunk_size);
    if (calculated_crc != pkt->chunk_crc32) {
        printf("ERROR: Chunk CRC mismatch (got 0x%08lX, expected 0x%08lX)\r\n",
               calculated_crc, pkt->chunk_crc32);
        ctx->error_code = OTA_ERR_CRC;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    uint32_t write_address = ctx->target_bank_address + (pkt->chunk_number * OTA_CHUNK_SIZE);
    printf("Chunk %lu/%lu: writing %u bytes to 0x%08lX\r\n",
           pkt->chunk_number + 1, ctx->total_chunks, pkt->chunk_size, write_address);

    if (write_to_flash_unified(write_address, pkt->data, pkt->chunk_size) != 0) {
        printf("ERROR: Flash write failed\r\n");
        ctx->error_code = OTA_ERR_FLASH;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    ctx->chunks_received++;
    ctx->expected_chunk_number++;
    ctx->bytes_written += pkt->chunk_size;

    ota_send_response(ctx, OTA_PKT_ACK);

    if (ctx->chunks_received == ctx->total_chunks) {
        printf("All chunks received! Transitioning to VERIFYING...\r\n");
        ctx->state = OTA_STATE_VERIFYING;
    }
}

uint32_t ota_calculate_firmware_crc32(uint32_t address, uint32_t size) {
    __HAL_CRC_DR_RESET(&hcrc);

    const uint32_t BUFFER_SIZE = 1024;
    uint32_t remaining = size;
    uint32_t offset = 0;

    while (remaining > 0) {
        uint32_t chunk_size = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
        const uint8_t *flash_data = (const uint8_t*)(address + offset);

        uint32_t num_words = chunk_size / 4;
        if (num_words > 0) {
            if (offset == 0) {
                HAL_CRC_Calculate(&hcrc, (uint32_t*)flash_data, num_words);
            } else {
                HAL_CRC_Accumulate(&hcrc, (uint32_t*)flash_data, num_words);
            }
        }

        uint32_t chunk_remaining = chunk_size % 4;
        if (chunk_remaining > 0) {
            uint32_t last_word = 0;
            memcpy(&last_word, flash_data + (num_words * 4), chunk_remaining);
            HAL_CRC_Accumulate(&hcrc, &last_word, 1);
        }

        offset += chunk_size;
        remaining -= chunk_size;
    }

    return hcrc.Instance->DR;
}

int ota_update_boot_state(const ota_context_t *ctx) {
    boot_state_t new_state;

    uint32_t updated_bank = (ctx->target_bank_address == BANK_A_ADDRESS) ? BANK_A : BANK_B;

    new_state.magic_number = BOOT_STATE_MAGIC;
    new_state.active_bank = updated_bank;
    new_state.crc32 = 0;

    if (updated_bank == BANK_A) {
        new_state.bank_a_status = BANK_STATUS_VALID;
        new_state.bank_b_status = BANK_STATUS_INVALID;
    } else {
        new_state.bank_a_status = BANK_STATUS_INVALID;
        new_state.bank_b_status = BANK_STATUS_VALID;
    }

    if (boot_state_erase() != 0) return -1;
    if (boot_state_write(&new_state) != 0) return -1;

    return 0;
}

void ota_process_end_packet(ota_context_t *ctx, const ota_end_packet_t *pkt) {
    printf("\r\n=== OTA END Packet ===\r\n");

    if (ctx->state != OTA_STATE_VERIFYING) {
        printf("ERROR: Not in VERIFYING state (current: %d)\r\n", ctx->state);
        ctx->error_code = OTA_ERR_SEQUENCE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    if (pkt->magic != OTA_MAGIC_START) {
        printf("ERROR: Invalid END packet magic\r\n");
        ctx->error_code = OTA_ERR_SEQUENCE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    printf("Verifying firmware...\r\n");
    printf("  Expected: %lu bytes, CRC32: 0x%08lX\r\n",
           ctx->firmware_size, ctx->firmware_crc32);
    printf("  Written:  %lu bytes\r\n", ctx->bytes_written);

    if (ctx->bytes_written != ctx->firmware_size) {
        printf("ERROR: Size mismatch!\r\n");
        ctx->error_code = OTA_ERR_SIZE;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    uint32_t calculated_crc = ota_calculate_firmware_crc32(
        ctx->target_bank_address, ctx->firmware_size);
    printf("  Calculated CRC32: 0x%08lX\r\n", calculated_crc);

    if (calculated_crc != ctx->firmware_crc32) {
        printf("ERROR: CRC32 mismatch! Firmware corrupted.\r\n");
        ctx->error_code = OTA_ERR_CRC;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    printf("Firmware verification PASSED!\r\n");
    ctx->state = OTA_STATE_FINALIZING;

    if (ota_update_boot_state(ctx) != 0) {
        printf("ERROR: Failed to update boot state\r\n");
        ctx->error_code = OTA_ERR_FLASH;
        ctx->state = OTA_STATE_ERROR;
        ota_send_response(ctx, OTA_PKT_NACK);
        return;
    }

    printf("Boot state updated!\r\n");
    printf("OTA complete! New firmware at 0x%08lX\r\n", ctx->target_bank_address);

    ctx->state = OTA_STATE_COMPLETE;
    ota_send_response(ctx, OTA_PKT_ACK);
}
