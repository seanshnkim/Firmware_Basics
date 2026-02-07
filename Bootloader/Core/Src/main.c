/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "boot_state.h"
#include "ota_manager.h"
#include "ota_uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
CRC_HandleTypeDef hcrc;
UART_HandleTypeDef huart1;
TIM_HandleTypeDef htim1;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_CRC_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_TIM1_Init(void);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
int _write(int file, char *ptr, int len)
{
    HAL_UART_Transmit(&huart1, (uint8_t*)ptr, len, 1000);
    return len;
}

/**
 * @brief  Jump to application at specified address
 * @param  app_address: Start address of application (e.g., 0x08010000)
 * @retval None (doesn't return if successful)
 */
void jump_to_application(uint32_t app_address)
{
    printf("Preparing to jump to application at 0x%08lX...\r\n", app_address);

    // 1. Read the application's vector table
    //    First entry: Initial Stack Pointer
    //    Second entry: Reset Handler (entry point)
    uint32_t app_stack_pointer = *((__IO uint32_t*)app_address);
    uint32_t app_entry_point = *((__IO uint32_t*)(app_address + 4));

    printf("  App Stack Pointer: 0x%08lX\r\n", app_stack_pointer);
    printf("  App Entry Point:   0x%08lX\r\n", app_entry_point);

    // 2. Sanity check: Is the stack pointer valid?
    //    It should point to RAM (0x20000000 - 0x20030000 for STM32F429)
    if ((app_stack_pointer < 0x20000000) || (app_stack_pointer > 0x20030000))
    {
        printf("ERROR: Invalid stack pointer! Application may not be valid.\r\n");
        return;  // Don't jump to invalid application
    }

    printf("Jumping to application NOW!\r\n\r\n");
    HAL_Delay(100);  // Give UART time to send the message

    // 3. Disable interrupts
    __disable_irq();

    // 4. Disable all peripheral clocks (important!)
	__HAL_RCC_GPIOA_CLK_DISABLE();
	__HAL_RCC_GPIOB_CLK_DISABLE();
	__HAL_RCC_GPIOC_CLK_DISABLE();
	__HAL_RCC_GPIOD_CLK_DISABLE();
	__HAL_RCC_GPIOE_CLK_DISABLE();
	__HAL_RCC_GPIOF_CLK_DISABLE();
    __HAL_RCC_GPIOG_CLK_DISABLE();
	__HAL_RCC_GPIOH_CLK_DISABLE();
	__HAL_RCC_USART1_CLK_DISABLE();
	__HAL_RCC_USB_OTG_FS_CLK_DISABLE();
	__HAL_RCC_USB_OTG_HS_CLK_DISABLE();  // Add this!


	__HAL_RCC_DMA2D_CLK_DISABLE();
	__HAL_RCC_LTDC_CLK_DISABLE();
	__HAL_RCC_FMC_CLK_DISABLE();

	// 5. Deinitialize HAL
	HAL_DeInit();

    // 6. Disable SysTick
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;

    // 7. Clear all interrupt pending flags
	for (int i = 0; i < 8; i++)
	{
		NVIC->ICPR[i] = 0xFFFFFFFF;
	}

	// 8. Set the vector table address to the application's vector table
	SCB->VTOR = app_address;

    // 9. Set the stack pointer to the application's initial stack pointer
    __set_MSP(app_stack_pointer);

    // 10. Set control register
    __set_CONTROL(0);

    // 11. Jump to the application's reset handler
    void (*app_reset_handler)(void) = (void (*)(void))app_entry_point;
    app_reset_handler();

    // Should never reach here
    while (1);
}

/**
 * @brief Simulate OTA update with fake firmware
 */
void test_ota_simulation(void) {
    printf("\r\n");
    printf("========================================\r\n");
    printf("    OTA SIMULATION TEST\r\n");
    printf("========================================\r\n");

    // Step 1: Create fake firmware data
    printf("\n--- Step 1: Creating fake firmware ---\r\n");

    #define TEST_FIRMWARE_SIZE  (5 * 1024)  // 5KB test firmware
    uint8_t *test_firmware = malloc(TEST_FIRMWARE_SIZE);

    if (test_firmware == NULL) {
        printf("ERROR: Failed to allocate memory for test firmware\r\n");
        return;
    }

    // Fill with recognizable pattern
    for (int i = 0; i < TEST_FIRMWARE_SIZE; i++) {
        test_firmware[i] = (uint8_t)(i & 0xFF);  // 0x00, 0x01, 0x02... 0xFF, 0x00...
    }

    printf("Test firmware created: %d bytes\r\n", TEST_FIRMWARE_SIZE);

    // Calculate CRC32 of test firmware
    uint32_t firmware_crc = calculate_crc32(test_firmware, TEST_FIRMWARE_SIZE);
    printf("Test firmware CRC32: 0x%08lX\r\n", firmware_crc);

    // Step 2: Initialize OTA context
    printf("\n--- Step 2: Initializing OTA ---\r\n");
    ota_context_t ota_ctx;
    ota_init(&ota_ctx);
    printf("OTA context initialized\r\n");

    // Step 3: Send START packet
    printf("\n--- Step 3: Sending START packet ---\r\n");

    uint32_t total_chunks = (TEST_FIRMWARE_SIZE + OTA_CHUNK_SIZE - 1) / OTA_CHUNK_SIZE;

    ota_start_packet_t start_pkt = {
        .magic = OTA_MAGIC_START,
        .packet_type = OTA_PKT_START,
        .firmware_size = TEST_FIRMWARE_SIZE,
        .firmware_version = 0x02000100,  // Version 2.0.1 (Major.Minor.Patch)
        .firmware_crc32 = firmware_crc,
        .total_chunks = total_chunks,
        .target_bank = BANK_B  // We're running from Bank A, update Bank B
    };

    ota_process_start_packet(&ota_ctx, &start_pkt);

    if (ota_ctx.state != OTA_STATE_RECEIVING_DATA) {
        printf("ERROR: START packet failed! State: %d\r\n", ota_ctx.state);
        free(test_firmware);
        return;
    }

    printf("START packet accepted. Ready to receive %lu chunks\r\n", total_chunks);

    // Step 4: Send DATA packets
    printf("\n--- Step 4: Sending DATA packets ---\r\n");

    for (uint32_t chunk_num = 0; chunk_num < total_chunks; chunk_num++) {
        ota_data_packet_t data_pkt;

        data_pkt.magic = OTA_MAGIC_DATA;
        data_pkt.packet_type = OTA_PKT_DATA;
        data_pkt.chunk_number = chunk_num;

        // Calculate chunk size (last chunk might be smaller)
        uint32_t offset = chunk_num * OTA_CHUNK_SIZE;
        uint32_t remaining = TEST_FIRMWARE_SIZE - offset;
        data_pkt.chunk_size = (remaining > OTA_CHUNK_SIZE) ? OTA_CHUNK_SIZE : remaining;

        // Copy chunk data
        memcpy(data_pkt.data, test_firmware + offset, data_pkt.chunk_size);

        // Calculate chunk CRC
        data_pkt.chunk_crc32 = calculate_crc32(data_pkt.data, data_pkt.chunk_size);

        // Process the packet
        ota_process_data_packet(&ota_ctx, &data_pkt);

        if (ota_ctx.state == OTA_STATE_ERROR) {
            printf("ERROR: DATA packet %lu failed!\r\n", chunk_num);
            free(test_firmware);
            return;
        }

        // Print progress every 10 chunks
        if ((chunk_num + 1) % 10 == 0 || chunk_num == total_chunks - 1) {
            printf("Progress: %lu/%lu chunks (%lu%%)\r\n",
                   chunk_num + 1,
                   total_chunks,
                   ((chunk_num + 1) * 100) / total_chunks);
        }
    }

    if (ota_ctx.state != OTA_STATE_VERIFYING) {
        printf("ERROR: Not in VERIFYING state after all chunks! State: %d\r\n", ota_ctx.state);
        free(test_firmware);
        return;
    }

    printf("All DATA packets sent successfully!\r\n");

    // Step 5: Send END packet
    printf("\n--- Step 5: Sending END packet ---\r\n");

    ota_end_packet_t end_pkt = {
        .magic = OTA_MAGIC_START,
        .packet_type = OTA_PKT_END
    };

    ota_process_end_packet(&ota_ctx, &end_pkt);

    if (ota_ctx.state != OTA_STATE_COMPLETE) {
        printf("ERROR: END packet failed! State: %d\r\n", ota_ctx.state);
        free(test_firmware);
        return;
    }

    printf("END packet processed successfully!\r\n");

    // Step 6: Verify boot state was updated
    printf("\n--- Step 6: Verifying boot state ---\r\n");

    boot_state_t state;
    if (boot_state_read(&state) == 0) {
        printf("Boot state updated:\r\n");
        printf("  Active bank: %s\r\n", state.active_bank == BANK_A ? "Bank A" : "Bank B");
        printf("  Bank A status: %s\r\n", state.bank_a_status == BANK_STATUS_VALID ? "VALID" : "INVALID");
        printf("  Bank B status: %s\r\n", state.bank_b_status == BANK_STATUS_VALID ? "VALID" : "INVALID");
    } else {
        printf("ERROR: Failed to read boot state\r\n");
    }

    // Cleanup
    free(test_firmware);

    printf("\r\n========================================\r\n");
    printf("✓ OTA SIMULATION TEST COMPLETE!\r\n");
    printf("========================================\r\n");
    printf("\nNext steps:\r\n");
    printf("1. Reset the device\r\n");
    printf("2. Bootloader should boot from Bank B\r\n");
    printf("3. Verify new firmware is running\r\n");
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_CRC_Init();
  MX_TIM1_Init();
  MX_USART1_UART_Init();

  /* USER CODE BEGIN 2 */
  printf("========================================\r\n");
  printf("    BOOTLOADER v1.0                    \r\n");
  printf("========================================\r\n");
  printf("Running at address: 0x%08lX\r\n", (uint32_t)&main);
  printf("\r\n");

  // Blink LED a few times to show bootloader is running
  printf("Bootloader running... (LED blinks 3 times)\r\n");
  for (int i = 0; i < 3; i++)
  {
	HAL_GPIO_WritePin(GPIOG, GPIO_PIN_13, GPIO_PIN_SET);
	HAL_Delay(200);
	HAL_GPIO_WritePin(GPIOG, GPIO_PIN_13, GPIO_PIN_RESET);
	HAL_Delay(200);
  }

  // Note: We're running from bootloader (0x08000000), not from Bank A or B
  // The OTA simulation will assume we're running from Bank A for testing purposes
  printf("Note: Running OTA simulation (pretending to run from Bank A)\r\n");

  // Run OTA simulation test
  test_ota_simulation();

  printf("Running from: 0x%08lX\r\n", SCB->VTOR);
  printf("Current bank: %s\r\n",
		 (SCB->VTOR == 0x08010000) ? "Bank A" : "Bank B");
  // Initialize OTA context
  ota_context_t ota_ctx;
  ota_init(&ota_ctx);

  // Enter OTA receive mode
  printf("\r\nEntering OTA mode...\r\n");
  ota_uart_receive_loop(&ota_ctx);

  // If we return from OTA (successful or aborted), just blink LED
  printf("\r\nOTA complete. Blinking LED...\r\n");

  while (1) {
	  HAL_GPIO_TogglePin(GPIOG, GPIO_PIN_13);
	  HAL_Delay(1000);
  }
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 72;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 3;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief CRC Initialization Function
  * @param None
  * @retval None
  */
static void MX_CRC_Init(void)
{

  /* USER CODE BEGIN CRC_Init 0 */

  /* USER CODE END CRC_Init 0 */

  /* USER CODE BEGIN CRC_Init 1 */

  /* USER CODE END CRC_Init 1 */
  hcrc.Instance = CRC;
  if (HAL_CRC_Init(&hcrc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CRC_Init 2 */

  /* USER CODE END CRC_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief TIM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM1_Init(void)
{

  /* USER CODE BEGIN TIM1_Init 0 */

  /* USER CODE END TIM1_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM1_Init 1 */

  /* USER CODE END TIM1_Init 1 */
  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 0;
  htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim1.Init.Period = 65535;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim1, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM1_Init 2 */

  /* USER CODE END TIM1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
/* USER CODE BEGIN MX_GPIO_Init_1 */
/* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, NCS_MEMS_SPI_Pin|CSX_Pin|OTG_FS_PSO_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(ACP_RST_GPIO_Port, ACP_RST_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOD, RDX_Pin|WRX_DCX_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOG, LD3_Pin|LD4_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : NCS_MEMS_SPI_Pin CSX_Pin OTG_FS_PSO_Pin */
  GPIO_InitStruct.Pin = NCS_MEMS_SPI_Pin|CSX_Pin|OTG_FS_PSO_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : B1_Pin MEMS_INT1_Pin MEMS_INT2_Pin TP_INT1_Pin */
  GPIO_InitStruct.Pin = B1_Pin|MEMS_INT1_Pin|MEMS_INT2_Pin|TP_INT1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_EVT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : ACP_RST_Pin */
  GPIO_InitStruct.Pin = ACP_RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(ACP_RST_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : OTG_FS_OC_Pin */
  GPIO_InitStruct.Pin = OTG_FS_OC_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_EVT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(OTG_FS_OC_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : BOOT1_Pin */
  GPIO_InitStruct.Pin = BOOT1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(BOOT1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : TE_Pin */
  GPIO_InitStruct.Pin = TE_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(TE_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : RDX_Pin WRX_DCX_Pin */
  GPIO_InitStruct.Pin = RDX_Pin|WRX_DCX_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /*Configure GPIO pins : LD3_Pin LD4_Pin */
  GPIO_InitStruct.Pin = LD3_Pin|LD4_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

/* USER CODE BEGIN MX_GPIO_Init_2 */
/* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6) {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
