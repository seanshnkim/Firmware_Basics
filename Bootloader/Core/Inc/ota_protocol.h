/*
 * ota_protocol.h
 *
 *  Created on: Jan 2, 2026
 *      Author: sean-shk
 */

#ifndef INC_OTA_PROTOCOL_H_
#define INC_OTA_PROTOCOL_H_

#include <stdint.h>

// Protocol magic numbers
#define OTA_MAGIC_START     0xAA55AA55
#define OTA_MAGIC_DATA      0x55AA55AA

// Packet types
#define OTA_PKT_START       0x01  // Initial handshake with firmware info
#define OTA_PKT_DATA        0x02  // Data chunk
#define OTA_PKT_END         0x03  // Transfer complete
#define OTA_PKT_ACK         0x04  // Acknowledgment
#define OTA_PKT_NACK        0x05  // Negative acknowledgment (error)
#define OTA_PKT_ABORT       0x06  // Abort transfer

// Error codes
#define OTA_ERR_NONE        0x00
#define OTA_ERR_CRC         0x01
#define OTA_ERR_SIZE        0x02
#define OTA_ERR_FLASH       0x03
#define OTA_ERR_SEQUENCE    0x04
#define OTA_ERR_TIMEOUT     0x05

// Configuration
#define OTA_CHUNK_SIZE      1024  // 1KB chunks
#define OTA_MAX_RETRIES     3
#define OTA_TIMEOUT_MS      5000

// START packet: Sent by host to begin transfer
typedef struct {
    uint32_t magic;              // OTA_MAGIC_START
    uint8_t packet_type;         // OTA_PKT_START
    uint32_t firmware_size;      // Total size in bytes
    uint32_t firmware_version;   // Version number
    uint32_t firmware_crc32;     // CRC32 of entire firmware
    uint32_t total_chunks;       // Number of data chunks to expect
    uint8_t target_bank;         // BANK_A or BANK_B
} __attribute__((packed)) ota_start_packet_t;

// DATA packet: Contains one chunk of firmware
typedef struct {
    uint32_t magic;              // OTA_MAGIC_DATA
    uint8_t packet_type;         // OTA_PKT_DATA
    uint32_t chunk_number;       // Sequential chunk number (0-based)
    uint16_t chunk_size;         // Size of data in this chunk (≤ OTA_CHUNK_SIZE)
    uint32_t chunk_crc32;        // CRC32 of this chunk's data
    uint8_t data[OTA_CHUNK_SIZE]; // Actual firmware data
} __attribute__((packed)) ota_data_packet_t;

// END packet: Signals transfer complete
typedef struct {
    uint32_t magic;              // OTA_MAGIC_START
    uint8_t packet_type;         // OTA_PKT_END
} __attribute__((packed)) ota_end_packet_t;

// ACK/NACK packet: Device response
typedef struct {
    uint32_t magic;              // OTA_MAGIC_START
    uint8_t packet_type;         // OTA_PKT_ACK or OTA_PKT_NACK
    uint8_t error_code;          // OTA_ERR_* if NACK
    uint32_t last_chunk_received; // Last successfully received chunk
} __attribute__((packed)) ota_response_packet_t;

#endif /* INC_OTA_PROTOCOL_H_ */
