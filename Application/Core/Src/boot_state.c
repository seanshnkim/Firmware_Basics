/*
 * boot_state.c
 *
 *  Created on: Dec 29, 2025
 *      Author: sean-shk
 */
#include "boot_state.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

extern CRC_HandleTypeDef hcrc;

// Forward declaration
static int write_to_flash_unified(uint32_t address, const void *data, uint16_t size);

static uint32_t calculate_crc32(const void *data, size_t length) {
	// Reset CRC peripheral
	__HAL_CRC_DR_RESET(&hcrc);

	// Calculate CRC
	return HAL_CRC_Calculate(&hcrc, (uint32_t*)data, length / 4);
}

/**
 * @brief Read boot state from flash
 * @param state Pointer to structure to fill
 * @return 0 on success, -1 if invalid magic, -2 if CRC mismatch
 */
int boot_state_read(boot_state_t *state) {
    // Step 1: Read from flash
    memcpy(state, (void*)BOOT_STATE_ADDRESS, sizeof(boot_state_t));

    // Step 2: Check magic number
    if (state->magic_number != BOOT_STATE_MAGIC) {
        return -1;  // Invalid or erased
    }

    // Step 3: Verify CRC32
    uint32_t saved_crc = state->crc32;
    state->crc32 = 0;
    uint32_t calculated_crc = calculate_crc32(state, sizeof(boot_state_t));

    printf("  calculated CRC32: 0x%08lX\r\n", calculated_crc);

    state->crc32 = saved_crc;  // Restore it

    if (calculated_crc != saved_crc) {
        return -2;  // Corrupted
    }

    return 0;  // Success!
}

int boot_state_write(const boot_state_t *state) {
    // Create a local copy so we can calculate CRC without modifying input
    boot_state_t state_copy;
    memcpy(&state_copy, state, sizeof(boot_state_t));

    // Calculate CRC32 (excluding the crc32 field itself)
    state_copy.crc32 = 0;
    state_copy.crc32 = calculate_crc32(&state_copy, sizeof(boot_state_t));

    printf("DEBUG: Writing to flash:\r\n");
    printf("  magic_number: 0x%08lX\r\n", state_copy.magic_number);
    printf("  bank_a_status: 0x%08lX\r\n", state_copy.bank_a_status);
    printf("  bank_b_status: 0x%08lX\r\n", state_copy.bank_b_status);
    printf("  active_bank: 0x%08lX\r\n", state_copy.active_bank);
    printf("  CRC32: 0x%08lX\r\n", state_copy.crc32);

    if (write_to_flash_unified(BOOT_STATE_ADDRESS, &state_copy, sizeof(boot_state_t)) != 0) {
        return -1;
    }

    return 0;
}

int boot_state_erase(void) {
	// 1. Unlock flash
	HAL_FLASH_Unlock();

	// 2. Erase sector
	FLASH_EraseInitTypeDef erase_config;
	erase_config.TypeErase = FLASH_TYPEERASE_SECTORS;
	erase_config.Sector = FLASH_SECTOR_8;  // Our boot state sector
	erase_config.NbSectors = 1;
	erase_config.VoltageRange = FLASH_VOLTAGE_RANGE_3;  // 2.7V to 3.6V

	uint32_t sector_error = 0;
	HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase_config, &sector_error);

	if (status != HAL_OK) {
		HAL_FLASH_Lock();
		return -1;  // Erase failed
	}
	// 3. Lock flash
	HAL_FLASH_Lock();

	return 0;
}

uint32_t boot_state_get_bank_address(uint32_t bank) {
	uint32_t bank_address = 0;

    switch (bank) {
    case BANK_A:
    	bank_address = BANK_A_ADDRESS;
    	break;
    case BANK_B:
    	bank_address = BANK_B_ADDRESS;
    	break;
    case BANK_INVALID:
    	break;
    }
    return bank_address;
}

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

