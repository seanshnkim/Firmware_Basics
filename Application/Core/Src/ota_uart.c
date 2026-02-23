/*
 * ota_uart.c
 * Polling-based UART receiver for OTA DATA and END packets.
 *
 * NOTE: The START packet is handled entirely by check_for_ota_start_packet()
 * in main.c before this loop is entered. By the time ota_uart_receive_loop()
 * is called, ctx->state is already OTA_STATE_RECEIVING_DATA.
 */

#include "ota_uart.h"
#include "ota_manager.h"
#include "ota_protocol.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

extern UART_HandleTypeDef huart2;

/**
 * @brief Receive exact number of bytes from USART2 with timeout
 * @return 0 on success, -1 on timeout/error
 */
static int uart_receive_bytes(uint8_t *buffer, uint16_t size, uint32_t timeout_ms) {
    HAL_StatusTypeDef status = HAL_UART_Receive(&huart2, buffer, size, timeout_ms);
    return (status == HAL_OK) ? 0 : -1;
}

/**
 * @brief Read packet header (magic + type) and return packet type
 *
 * Reads exactly 5 bytes: magic (4) + packet_type (1).
 * The caller is responsible for reading the remainder of the packet.
 *
 * @param header_buf  Caller-supplied 5-byte buffer; filled on success
 * @param packet_type Output: packet type byte
 * @return 0 on success, -1 on timeout or invalid magic
 */
static int read_packet_header(uint8_t *header_buf, uint8_t *packet_type) {
    if (uart_receive_bytes(header_buf, 5, 10000) != 0) {
        printf("Header receive timeout\r\n");
        return -1;
    }

    uint32_t magic;
    memcpy(&magic, header_buf, 4);
    *packet_type = header_buf[4];

    if (magic != OTA_MAGIC_START && magic != OTA_MAGIC_DATA) {
        printf("ERROR: Invalid magic: 0x%08lX\r\n", magic);
        return -1;
    }

    return 0;
}

/**
 * @brief Receive and process a DATA packet
 *
 * The 5-byte header (magic + type) has already been read by the caller
 * and is passed in via header_buf. This function reads the remaining
 * (sizeof(ota_data_packet_t) - 5) bytes and assembles the full packet.
 *
 * @param ctx        OTA context
 * @param header_buf The 5 header bytes already received
 * @return 0 on success, -1 on error
 */
static int receive_data_packet(ota_context_t *ctx, const uint8_t *header_buf) {
    uint8_t buffer[sizeof(ota_data_packet_t)];

    /* Copy header we already have */
    memcpy(buffer, header_buf, 5);

    /* Read the rest: chunk_number(4) + chunk_size(2) + chunk_crc32(4) + data(1024) */
    uint16_t remaining = sizeof(ota_data_packet_t) - 5;
    if (uart_receive_bytes(buffer + 5, remaining, 15000) != 0) {
        printf("ERROR: Timeout receiving DATA packet body\r\n");
        return -1;
    }

    ota_data_packet_t pkt;
    memcpy(&pkt, buffer, sizeof(ota_data_packet_t));

    ota_process_data_packet(ctx, &pkt);

    return (ctx->state != OTA_STATE_ERROR) ? 0 : -1;
}

/**
 * @brief Receive and process an END packet
 *
 * The 5-byte header has already been read. END packet is only 5 bytes
 * total (magic + type), so there is nothing left to read.
 *
 * @param ctx        OTA context
 * @param header_buf The 5 header bytes already received
 * @return 0 on success, -1 on error
 */
static int receive_end_packet(ota_context_t *ctx, const uint8_t *header_buf) {
    ota_end_packet_t pkt;
    memcpy(&pkt, header_buf, sizeof(ota_end_packet_t));  // END packet is exactly 5 bytes

    ota_process_end_packet(ctx, &pkt);

    return (ctx->state == OTA_STATE_COMPLETE) ? 0 : -1;
}

/**
 * @brief Main OTA UART receiver loop — handles DATA and END packets only
 *
 * Precondition: ctx->state == OTA_STATE_RECEIVING_DATA
 * (START packet was already handled by check_for_ota_start_packet)
 *
 * Returns when OTA completes (OTA_STATE_COMPLETE) or is aborted.
 *
 * @param ctx OTA context pre-populated by ota_process_start_packet()
 */
void ota_uart_receive_loop(ota_context_t *ctx) {
    printf("\r\n");
    printf("========================================\r\n");
    printf("  OTA UART RECEIVER — WAITING FOR DATA\r\n");
    printf("========================================\r\n");
    printf("Expecting %lu chunks...\r\n", ctx->total_chunks);

    while (1) {
        uint8_t header[5];
        uint8_t packet_type;

        if (read_packet_header(header, &packet_type) != 0) {
            /* Timeout between packets - keep waiting */
            continue;
        }

        printf("Packet type: 0x%02X\r\n", packet_type);

        switch (packet_type) {

            case OTA_PKT_DATA:
                if (receive_data_packet(ctx, header) != 0) {
                    printf("DATA packet processing failed\r\n");
                    /* ota_process_data_packet already sent NACK;
                       keep looping so Python can retry the chunk */
                }
                break;

            case OTA_PKT_END:
                if (receive_end_packet(ctx, header) != 0) {
                    printf("END packet processing failed\r\n");
                } else {
                    printf("OTA transfer complete!\r\n");
                    return;  /* Success */
                }
                break;

            case OTA_PKT_START:
                /* START should not arrive here — already handled.
                   Drain it and send NACK so Python knows. */
                printf("WARNING: Unexpected START packet in data phase\r\n");
                {
                    uint8_t drain[sizeof(ota_start_packet_t) - 5];
                    uart_receive_bytes(drain, sizeof(drain), 2000);
                }
                break;

            case OTA_PKT_ABORT:
                printf("ABORT received — stopping OTA\r\n");
                ota_init(ctx);
                return;

            default:
                printf("ERROR: Unknown packet type 0x%02X\r\n", packet_type);
                break;
        }
    }
}
