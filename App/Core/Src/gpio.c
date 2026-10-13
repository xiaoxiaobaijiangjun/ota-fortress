/**
 ******************************************************************************
 * @file    gpio.c
 * @brief   GPIO configuration for the FOTA demo application
 ******************************************************************************
 */
#include "gpio.h"

/**
  * @brief GPIO Initialization: PE9 as push-pull output (demo LED)
  */
void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  APP_LED_GPIO_CLK_ENABLE();

  GPIO_InitStruct.Pin = APP_LED_GPIO_PIN;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(APP_LED_GPIO_PORT, &GPIO_InitStruct);

  HAL_GPIO_WritePin(APP_LED_GPIO_PORT, APP_LED_GPIO_PIN, GPIO_PIN_RESET);
}
