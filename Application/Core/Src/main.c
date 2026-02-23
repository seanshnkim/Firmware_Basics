/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Application with OTA Update Support
  * 
  * This application:
  * 1. Waits 5 seconds for OTA START packet on USART2 (HM-10)
  * 2. If OTA packet received -> Enter OTA mode and update firmware
  * 3. If timeout -> Run normal application (LED blink)
  * 4. After OTA completes -> Reset to boot new firmware
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usb_host.h"
#include <stdio.h>
#include <string.h>

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ota_protocol.h"
#include "ota_manager.h"
#include "ota_uart.h"
#include "boot_state.h"
/* USER CODE END Includes */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define OTA_WAIT_TIMEOUT_MS  5000
/* USER CODE END PD */

/* Private variables ---------------------------------------------------------*/
CRC_HandleTypeDef hcrc;
DMA2D_HandleTypeDef hdma2d;
I2C_HandleTypeDef hi2c3;
LTDC_HandleTypeDef hltdc;
SPI_HandleTypeDef hspi5;
TIM_HandleTypeDef htim1;
UART_HandleTypeDef huart1; // ST-Link VCP Debug
UART_HandleTypeDef huart2; // HM-10 OTA
SDRAM_HandleTypeDef hsdram1;

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_CRC_Init(void);
static void MX_DMA2D_Init(void);
static void MX_FMC_Init(void);
static void MX_I2C3_Init(void);
static void MX_LTDC_Init(void);
static void MX_SPI5_Init(void);
static void MX_TIM1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
void MX_USB_HOST_Process(void);

/* USER CODE BEGIN PFP */
static int check_for_ota_start_packet(uint32_t timeout_ms, ota_context_t *ctx);
static void run_normal_application(void);
static void enter_ota_mode(ota_context_t *ctx);
/* USER CODE END PFP */

/* USER CODE BEGIN 0 */

int _write(int file, char *ptr, int len)
{
    HAL_UART_Transmit(&huart1, (uint8_t*)ptr, len, 1000);
    return len;
}

/**
 * @brief Send ACK response over USART2 (HM-10)
 * @param last_chunk Last successfully processed chunk (0xFFFFFFFF = none yet)
 */
static void send_ack(uint32_t last_chunk) {
    uint8_t response[10];
    uint32_t magic = OTA_MAGIC_START;
    uint8_t pkt_type = OTA_PKT_ACK;
    uint8_t error_code = OTA_ERR_NONE;
    memcpy(response, &magic, 4);
    response[4] = pkt_type;
    response[5] = error_code;
    memcpy(response + 6, &last_chunk, 4);
    HAL_UART_Transmit(&huart2, response, 10, 1000);
}

/**
 * @brief Send NACK response over USART2 (HM-10)
 * @param error_code OTA_ERR_* error code
 * @param last_chunk Last successfully processed chunk (0xFFFFFFFF = none yet)
 */
static void send_nack(uint8_t error_code, uint32_t last_chunk) {
    uint8_t response[10];
    uint32_t magic = OTA_MAGIC_START;
    uint8_t pkt_type = OTA_PKT_NACK;
    memcpy(response, &magic, 4);
    response[4] = pkt_type;
    response[5] = error_code;
    memcpy(response + 6, &last_chunk, 4);
    HAL_UART_Transmit(&huart2, response, 10, 1000);
}

/**
 * @brief Wait for and validate an OTA START packet
 *
 * Receives the full START packet, validates all fields, sends ACK/NACK,
 * and populates the OTA context so enter_ota_mode() can proceed directly
 * to DATA packets without re-processing START.
 *
 * @param timeout_ms How long to wait for each receive attempt
 * @param ctx        OTA context to populate on success
 * @return 1 if valid START packet received and ACK sent, 0 otherwise
 */
static int check_for_ota_start_packet(uint32_t timeout_ms, ota_context_t *ctx) {
    uint8_t buf[sizeof(ota_start_packet_t)];
    ota_start_packet_t *pkt = (ota_start_packet_t*)buf;

    printf("Waiting %lu ms for OTA START packet...\r\n", timeout_ms);

    int max_retries = 3;

    for (int attempt = 0; attempt < max_retries; attempt++) {

        HAL_StatusTypeDef status = HAL_UART_Receive(&huart2, buf, sizeof(buf), timeout_ms);

        if (status == HAL_TIMEOUT) {
            /* No data at all within the window - give up immediately,
               don't waste more timeout cycles */
            printf("No OTA packet detected (timeout). Starting normal app...\r\n");
            return 0;
        }

        if (status != HAL_OK) {
            printf("UART error during OTA check.\r\n");
            send_nack(OTA_ERR_SEQUENCE, 0xFFFFFFFF);
            continue;
        }

        /* --- Validate magic and packet type --- */
        if (pkt->magic != OTA_MAGIC_START || pkt->packet_type != OTA_PKT_START) {
            printf("Invalid magic/type (magic: 0x%08lX, type: 0x%02X)\r\n",
                   pkt->magic, pkt->packet_type);
            send_nack(OTA_ERR_SEQUENCE, 0xFFFFFFFF);
            continue;
        }

        /* --- Validate firmware_size --- */
        if (pkt->firmware_size == 0 || pkt->firmware_size > BANK_SIZE) {
            printf("Invalid firmware size: %lu\r\n", pkt->firmware_size);
            send_nack(OTA_ERR_SIZE, 0xFFFFFFFF);
            continue;
        }

        /* --- Validate total_chunks --- */
        uint32_t expected_chunks =
            (pkt->firmware_size + OTA_CHUNK_SIZE - 1) / OTA_CHUNK_SIZE;
        if (pkt->total_chunks == 0 || pkt->total_chunks != expected_chunks) {
            printf("Invalid total_chunks: %lu (expected %lu)\r\n",
                   pkt->total_chunks, expected_chunks);
            send_nack(OTA_ERR_SIZE, 0xFFFFFFFF);
            continue;
        }

        /* --- Validate target bank --- */
        if (pkt->target_bank != BANK_A && pkt->target_bank != BANK_B) {
            printf("Invalid target bank: 0x%02X\r\n", pkt->target_bank);
            send_nack(OTA_ERR_SEQUENCE, 0xFFFFFFFF);
            continue;
        }

        /* --- All checks passed: populate context and process START --- */
        printf("OTA START packet valid! Processing...\r\n");
        ota_process_start_packet(ctx, pkt);

        if (ctx->state != OTA_STATE_RECEIVING_DATA) {
            /* ota_process_start_packet already sent a NACK internally */
            printf("START processing failed (state: %d)\r\n", ctx->state);
            continue;
        }

        /* ota_process_start_packet sends its own ACK on success,
           so we don't send a duplicate here */
        printf("OTA START accepted. Ready for DATA packets.\r\n");

        /* Visual confirmation: fast blink after successful handshake */
        for (int i = 0; i < 6; i++) {
            HAL_GPIO_TogglePin(GPIOG, GPIO_PIN_14);
            HAL_Delay(100);
        }

        return 1;
    }

    printf("OTA START failed after %d attempts.\r\n", max_retries);
    return 0;
}

/**
 * @brief Run normal application (LED blink)
 */
static void run_normal_application(void) {
    printf("\r\n");
    printf("========================================\r\n");
    printf("  NORMAL APPLICATION MODE\r\n");
    printf("========================================\r\n");
    printf("Application v1.0 running from Bank A\r\n");
    printf("LED blinking on PG13...\r\n");

    while (1) {
        HAL_GPIO_WritePin(GPIOG, GPIO_PIN_13, GPIO_PIN_SET);
        HAL_Delay(300);
        HAL_GPIO_WritePin(GPIOG, GPIO_PIN_13, GPIO_PIN_RESET);
        HAL_Delay(300);
    }
}

/**
 * @brief Enter OTA update mode
 *
 * Called after check_for_ota_start_packet() has already:
 *   - Received and validated the START packet
 *   - Populated ctx via ota_process_start_packet()
 *   - Erased the target bank
 *   - Sent the ACK
 *
 * This function only needs to handle DATA and END packets.
 *
 * @param ctx Pre-populated OTA context (state == OTA_STATE_RECEIVING_DATA)
 */
static void enter_ota_mode(ota_context_t *ctx) {
    printf("\r\n");
    printf("========================================\r\n");
    printf("  ENTERING OTA UPDATE MODE\r\n");
    printf("========================================\r\n");

    /* Receive DATA and END packets until complete */
    ota_uart_receive_loop(ctx);

    /* ota_uart_receive_loop returns only on OTA_STATE_COMPLETE or abort.
       Boot state has already been updated inside ota_process_end_packet(). */
    if (ctx->state == OTA_STATE_COMPLETE) {
        printf("\r\n");
        printf("========================================\r\n");
        printf("  OTA UPDATE COMPLETED!\r\n");
        printf("========================================\r\n");

        for (int i = 0; i < 10; i++) {
            HAL_GPIO_TogglePin(GPIOG, GPIO_PIN_14);
            HAL_Delay(150);
        }

        printf("Rebooting in 3 seconds...\r\n");
        HAL_Delay(3000);
    } else {
        printf("OTA failed or aborted. Rebooting...\r\n");
        HAL_Delay(1000);
    }

    NVIC_SystemReset();
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  */
int main(void)
{
    /* MCU Configuration */
    HAL_Init();
    SystemClock_Config();

    /* Initialize peripherals */
    MX_GPIO_Init();
    MX_CRC_Init();
    MX_DMA2D_Init();
    MX_FMC_Init();
    MX_I2C3_Init();
    MX_LTDC_Init();
    MX_SPI5_Init();
    MX_TIM1_Init();
    MX_USART1_UART_Init();
    MX_USART2_UART_Init();
    MX_USB_HOST_Init();

    printf("\r\n");
    printf("========================================\r\n");
    printf("  STM32F429 APPLICATION STARTUP\r\n");
    printf("========================================\r\n");
    printf("Firmware Version: 1.2\r\n");
    printf("Running from: Bank A (0x%08X)\r\n", BANK_A_ADDRESS);
    printf("USART1 Baud Rate: 115200 (VCP)\r\n");
    printf("USART2 Baud Rate: 9600 (HM-10)\r\n\r\n");

    /* Initialize OTA context before the check so we can pass it through */
    ota_context_t ota_ctx;
    ota_init(&ota_ctx);

    /* Check for OTA request */
    if (check_for_ota_start_packet(OTA_WAIT_TIMEOUT_MS, &ota_ctx)) {
        enter_ota_mode(&ota_ctx);
        /* Never returns - resets inside enter_ota_mode */
    } else {
        run_normal_application();
        /* Never returns - infinite loop */
    }

    while (1) { }
}
