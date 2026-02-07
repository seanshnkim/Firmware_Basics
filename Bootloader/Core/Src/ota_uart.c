/*
 * ota_uart.c
 * Simple polling-based UART receiver for OTA packets
 *  Created on: Jan 5, 2026
 *      Author: sean-shk
 */

#include "ota_uart.h"
#include "ota_manager.h"
#include "ota_protocol.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

extern UART_HandleTypeDef huart1;

/**
 * @brief Receive exact number of bytes from UART with timeout
 * @param buffer Buffer to store received data
 * @param size Number of bytes to receive
 * @param timeout_ms Timeout in milliseconds
 * @return 0 on success, -1 on timeout/error
 */
static int uart_receive_bytes(uint8_t *buffer, uint16_t size, uint32_t timeout_ms) {
    HAL_StatusTypeDef status = HAL_UART_Receive(&huart1, buffer, size, timeout_ms);

    if (status == HAL_OK) {
        return 0;
    } else if (status == HAL_TIMEOUT) {
        return -1;  // Timeout
    } else {
        return -1;  // Error
    }
}

/**
 * @brief Peek at first byte to determine packet type
 * @param packet_type Output: packet type
 * @return 0 on success, -1 on error
 */
static int uart_peek_packet_type(uint8_t *packet_type) {
    uint8_t header[5];  // magic (4 bytes) + type (1 byte)

    if (uart_receive_bytes(header, 5, 5000) != 0) {
        return -1;  // Timeout or error
    }

    uint32_t magic = *((uint32_t*)header);
    *packet_type = header[4];

    // Validate magic number
    if (magic != OTA_MAGIC_START && magic != OTA_MAGIC_DATA) {
        printf("ERROR: Invalid magic number: 0x%08lX\r\n", magic);
        return -1;
    }

    return 0;
}

/**
 * @brief Receive and process START packet
 * @param ctx OTA context
 * @return 0 on success, -1 on error
 */
static int receive_start_packet(ota_context_t *ctx) {
    ota_start_packet_t pkt;

    // We already read magic (4) + type (1), now read remaining fields
    // Remaining: firmware_size (4) + firmware_version (4) + firmware_crc32 (4)
    //            + total_chunks (4) + target_bank (1) = 17 bytes

    // First, rewind and read entire packet
    uint8_t buffer[sizeof(ota_start_packet_t)];

    if (uart_receive_bytes(buffer, sizeof(ota_start_packet_t), 5000) != 0) {
        printf("ERROR: Timeout receiving START packet\r\n");
        return -1;
    }

    // Copy to packet structure
    memcpy(&pkt, buffer, sizeof(ota_start_packet_t));

    // Process packet
    ota_process_start_packet(ctx, &pkt);

    return (ctx->state == OTA_STATE_RECEIVING_DATA) ? 0 : -1;
}

/**
 * @brief Receive and process DATA packet
 * @param ctx OTA context
 * @return 0 on success, -1 on error
 */
static int receive_data_packet(ota_context_t *ctx) {
    ota_data_packet_t pkt;

    // Read entire DATA packet
    // Total size: magic (4) + type (1) + chunk_number (4) + chunk_size (2)
    //            + chunk_crc32 (4) + padding (3) + data (1024) = 1042 bytes

    uint8_t buffer[sizeof(ota_data_packet_t)];

    if (uart_receive_bytes(buffer, sizeof(ota_data_packet_t), 10000) != 0) {
        printf("ERROR: Timeout receiving DATA packet\r\n");
        return -1;
    }

    // Copy to packet structure
    memcpy(&pkt, buffer, sizeof(ota_data_packet_t));

    // Process packet
    ota_process_data_packet(ctx, &pkt);

    return (ctx->state != OTA_STATE_ERROR) ? 0 : -1;
}

/**
 * @brief Receive and process END packet
 * @param ctx OTA context
 * @return 0 on success, -1 on error
 */
static int receive_end_packet(ota_context_t *ctx) {
    ota_end_packet_t pkt;

    // Read entire END packet: magic (4) + type (1) = 5 bytes
    uint8_t buffer[sizeof(ota_end_packet_t)];

    if (uart_receive_bytes(buffer, sizeof(ota_end_packet_t), 5000) != 0) {
        printf("ERROR: Timeout receiving END packet\r\n");
        return -1;
    }

    // Copy to packet structure
    memcpy(&pkt, buffer, sizeof(ota_end_packet_t));

    // Process packet
    ota_process_end_packet(ctx, &pkt);

    return (ctx->state == OTA_STATE_COMPLETE) ? 0 : -1;
}

/**
 * @brief Main OTA UART receiver loop
 * @param ctx OTA context (must be initialized)
 */
void ota_uart_receive_loop(ota_context_t *ctx) {
    printf("\r\n");
    printf("========================================\r\n");
    printf("  OTA UART RECEIVER READY\r\n");
    printf("========================================\r\n");
    printf("Waiting for OTA packets...\r\n");
    printf("(Send firmware using: python ota_sender.py app.bin %s)\r\n", "/dev/ttyACM0");

    while (1) {
        uint8_t packet_type;

        // Wait for next packet (this blocks until data arrives)
        if (uart_peek_packet_type(&packet_type) != 0) {
            // Timeout or error - just continue waiting
            continue;
        }

        printf("\r\nReceived packet type: 0x%02X\r\n", packet_type);

        switch (packet_type) {
            case OTA_PKT_START:
                if (receive_start_packet(ctx) != 0) {
                    printf("✗ START packet processing failed\r\n");
                }
                break;

            case OTA_PKT_DATA:
                if (receive_data_packet(ctx) != 0) {
                    printf("✗ DATA packet processing failed\r\n");
                }
                break;

            case OTA_PKT_END:
                if (receive_end_packet(ctx) != 0) {
                    printf("✗ END packet processing failed\r\n");
                } else {
                    printf("\r\n✓ OTA UPDATE COMPLETE!\r\n");
                    printf("Please reset the device to boot new firmware.\r\n");
                    // Exit the loop after successful OTA
                    return;
                }
                break;

            case OTA_PKT_ABORT:
                printf("Received ABORT packet - stopping OTA\r\n");
                ota_init(ctx);  // Reset to IDLE
                break;

            default:
                printf("ERROR: Unknown packet type: 0x%02X\r\n", packet_type);
                break;
        }

        // Small delay to prevent tight loop
        HAL_Delay(10);
    }
}
