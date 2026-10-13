/**
 ******************************************************************************
 * @file    main.c
 * @brief   FOTA demo user application (STM32F407VET6)
 *
 * This application is the "payload" delivered by the FOTA bootloader:
 *  - linked at slot A (0x0800C000) or slot B (0x08040000), selectable with
 *    the APP_SLOT preprocessor define
 *  - relocates its vector table to APP_BASE_ADDRESS
 *  - blinks the board LED (PE9) and prints a heartbeat on USART1
 *  - confirms a pending update (TRIAL -> IDLE) after running healthy,
 *    so the bootloader keeps this slot active on the next boot
 *
 * Build slot A / slot B with the two generated Keil projects, upload them
 * via fota_host.py, then activate - the bootloader runs the active slot.
 ******************************************************************************
 */

#include <stdio.h>
#include <string.h>
#include "main.h"
#include "gpio.h"
#include "usart.h"
#include "fota_meta.h"

/* Private variables ---------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void APP_UART_Send_String(const char *str);
static uint32_t APP_CRC32_FOTA(const uint8_t *data, uint32_t len);
static uint8_t APP_Meta_Read(FOTA_Meta_t *meta);
static void APP_Meta_Confirm_Slot(void);
static void APP_Check_Reboot_Request(void);
static void APP_IWDG_Refresh(void);

/**
  * @brief  The application entry point.
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* CRITICAL: relocate the vector table to the application base address
   * BEFORE any interrupt (SysTick) is enabled. */
  SCB->VTOR = APP_BASE_ADDRESS;

  /* CRITICAL: the bootloader jumped here with interrupts disabled
   * (__disable_irq), so re-enable them AFTER the vector table is relocated.
   * Without this, SysTick never fires and HAL_GetTick stays at 0. */
  __enable_irq();

  /* MCU Configuration--------------------------------------------------------*/
  HAL_Init();
  SystemClock_Config();

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();

  /* USER CODE BEGIN 2 */
  {
    char banner[64];
    snprintf(banner, sizeof(banner), "\r\n[FOTA APP] v2.0 started from SLOT %c (0x%08X)\r\n",
             APP_SLOT_ID ? 'B' : 'A', (unsigned int)APP_BASE_ADDRESS);
    APP_UART_Send_String(banner);
  }
  APP_UART_Send_String("[FOTA APP] LED PE9 blinking, heartbeat on USART1\r\n");

  /* Infinite loop */
  uint32_t boot_tick = HAL_GetTick();
  uint8_t update_confirmed = 0;
  uint32_t last_led_tick = HAL_GetTick();
  uint32_t last_beat_tick = HAL_GetTick();

    while (1)
  {
    /* The bootloader started the IWDG at the jump: feed it on every loop
     * iteration (once started the watchdog can only be stopped by POR). */
    APP_IWDG_Refresh();

    if ((HAL_GetTick() - last_led_tick) >= APP_LED_TOGGLE_PERIOD_MS)
    {
      last_led_tick = HAL_GetTick();
      HAL_GPIO_TogglePin(APP_LED_GPIO_PORT, APP_LED_GPIO_PIN);
    }

    if ((HAL_GetTick() - last_beat_tick) >= APP_HEARTBEAT_PERIOD_MS)
    {
      last_beat_tick = HAL_GetTick();
      APP_UART_Send_String("[FOTA APP] alive (v2.0)\r\n");
    }

    /* Healthy after the confirm delay -> mark the current slot as good */
    if (!update_confirmed && (HAL_GetTick() - boot_tick) >= APP_CONFIRM_DELAY_MS)
    {
      APP_Meta_Confirm_Slot();
      update_confirmed = 1;
    }

    /* Magic byte over UART -> reset into the bootloader for the next update */
    APP_Check_Reboot_Request();
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration (HSI 16 MHz - no external crystal needed)
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                              | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief Send a NUL-terminated string over USART1 (blocking).
  */
static void APP_UART_Send_String(const char *str)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)str, (uint16_t)strlen(str), HAL_MAX_DELAY);
}

/**
 * @brief FOTA CRC32 - identical algorithm to the HOST and the STM32 hardware
 *        CRC peripheral (byte fed as a 32-bit word, MSB-first, no reflection).
 *        Used here to validate the metadata sector written by the bootloader.
 */
static uint32_t APP_CRC32_FOTA(const uint8_t *data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFUL;
  const uint32_t poly = 0x04C11DB7UL;
  uint32_t i;
  int b;

  for (i = 0; i < len; i++)
  {
    crc ^= data[i];
    for (b = 0; b < 32; b++)
    {
      if (crc & 0x80000000UL)
      {
        crc = (crc << 1) ^ poly;
      }
      else
      {
        crc <<= 1;
      }
    }
  }
  return crc;
}

/**
 * @brief Read and validate the A/B metadata from the metadata sector.
 */
static uint8_t APP_Meta_Read(FOTA_Meta_t *meta)
{
  const uint8_t *p = (const uint8_t *)META_BASE_ADDRESS;

  if (p[0] == 0xFF)
  {
    return 0;   /* blank sector: no metadata yet */
  }
  memcpy(meta, p, sizeof(FOTA_Meta_t));
  if (meta->magic != META_MAGIC || meta->version != META_VERSION)
  {
    return 0;
  }
  return (APP_CRC32_FOTA(p, META_CRC_COVERED_LEN) == meta->meta_crc);
}

/**
 * @brief Confirm this slot's update: TRIAL -> IDLE in the metadata sector.
 *        Only runs when the bootloader activated this slot and the app has
 *        been running healthy for APP_CONFIRM_DELAY_MS.
 */
static void APP_Meta_Confirm_Slot(void)
{
  FOTA_Meta_t meta;
  FLASH_EraseInitTypeDef EraseInit = {0};
  uint32_t SectorError = 0;
  uint32_t i;
  uint8_t ok = 0;

  if (!APP_Meta_Read(&meta))
  {
    return;
  }
  if (meta.state != META_STATE_TRIAL || meta.active_slot != APP_SLOT_ID)
  {
    return;
  }

  /* min_sv (anti-rollback floor) is carried over untouched: the app only
   * confirms the TRIAL state, it must never raise or lower the floor. */
  meta.state = META_STATE_IDLE;
  meta.meta_crc = APP_CRC32_FOTA((const uint8_t *)&meta, META_CRC_COVERED_LEN);

  HAL_FLASH_Unlock();
  EraseInit.TypeErase = FLASH_TYPEERASE_SECTORS;
  EraseInit.Banks = FLASH_BANK_1;
  EraseInit.Sector = META_SECTOR_NUMBER;
  EraseInit.NbSectors = 1U;
  EraseInit.VoltageRange = FLASH_VOLTAGE_RANGE_3;
  HAL_FLASHEx_Erase(&EraseInit, &SectorError);

  if (SectorError == 0xFFFFFFFFU)
  {
    ok = 1;
    for (i = 0; i < sizeof(FOTA_Meta_t); i++)
    {
      if (HAL_OK != HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE,
                                      META_BASE_ADDRESS + i,
                                      ((const uint8_t *)&meta)[i]))
      {
        ok = 0;
        break;
      }
    }
  }
  HAL_FLASH_Lock();

  if (ok)
  {
    APP_UART_Send_String("[FOTA APP] update confirmed (TRIAL -> IDLE)\r\n");
  }
}

/**
 * @brief Poll UART RX: on the magic byte reset into the bootloader so the
 *        HOST can run the next update. Drains pending bytes first.
 *
 *        NOTE: HAL_UART_Receive is a blocking call. With this HAL, Timeout=0
 *        returns immediately (HAL_TIMEOUT, see UART_WaitOnFlagUntilTimeout's
 *        "|| (Timeout == 0U)"), and HAL_MAX_DELAY would wait forever. Also,
 *        if SysTick/interrupts are down the tick is frozen and NO timeout
 *        ever expires. Poll the RXNE flag directly instead.
 */
static void APP_Check_Reboot_Request(void)
{
  uint8_t byte = 0;

  if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_ORE) == SET)
  {
    __HAL_UART_CLEAR_OREFLAG(&huart1);
  }

  while (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE) == SET)
  {
    byte = (uint8_t)(huart1.Instance->DR & 0xFFU);
    if (byte == APP_REBOOT_MAGIC)
    {
      NVIC_SystemReset();
    }
  }
}

/**
 * @brief Reload the independent watchdog counter (IWDG->KR = 0xAAAA).
 *        Called every main-loop iteration; harmless when the watchdog is off.
 */
static void APP_IWDG_Refresh(void)
{
  IWDG->KR = 0xAAAAU;
}

/**
  * @brief  This function is executed in case of error occurrence.
  */
void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}
