/*
 * ota_uart.h
 *
 *  Created on: Jan 5, 2026
 *      Author: sean-shk
 */

#ifndef INC_OTA_UART_H_
#define INC_OTA_UART_H_

#include "ota_manager.h"

/**
 * @brief Main OTA UART receiver loop
 * @param ctx OTA context (must be initialized)
 *
 * This function blocks and waits for OTA packets over UART.
 * It will return only when OTA is complete or aborted.
 */
void ota_uart_receive_loop(ota_context_t *ctx);

#endif /* INC_OTA_UART_H_ */
