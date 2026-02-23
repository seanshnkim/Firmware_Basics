/*
 * boot_state.h
 *
 *  Created on: Dec 29, 2025
 *      Author: sean-shk
 */

#ifndef INC_BOOT_STATE_H_
#define INC_BOOT_STATE_H_

#include <stdint.h>

// Flash addresses
#define BANK_A_ADDRESS      0x08010000  // Sector 4-5 (192KB)
#define BANK_B_ADDRESS      0x08040000  // Sector 6-7 (256KB)
#define BOOT_STATE_ADDRESS  0x08080000  // Sector 8

// Bank selection (now uint32_t for word alignment)
#define BANK_A              0x00000000
#define BANK_B              0x00000001
#define BANK_INVALID        0xFFFFFFFF
#define BANK_SIZE  (256 * 1024)  // 256KB

// Bank status values (now uint32_t for word alignment)
#define BANK_STATUS_INVALID 0x00000000
#define BANK_STATUS_VALID   0x00000001
#define BANK_STATUS_TESTING 0x00000002

// Magic number to identify valid boot state
#define BOOT_STATE_MAGIC    0xDEADBEEF

typedef struct {
    uint32_t magic_number;      // 4 bytes
    uint32_t bank_a_status;     // 4 bytes
    uint32_t bank_b_status;     // 4 bytes
    uint32_t active_bank;       // 4 bytes
    uint32_t crc32;             // 4 bytes
} boot_state_t;  // Total: 20 bytes = 5 words ✓

// Function prototypes
int boot_state_read(boot_state_t *state);
int boot_state_write(const boot_state_t *state);
int boot_state_erase(void);
uint32_t boot_state_get_bank_address(uint32_t bank);


#endif /* INC_BOOT_STATE_H_ */
