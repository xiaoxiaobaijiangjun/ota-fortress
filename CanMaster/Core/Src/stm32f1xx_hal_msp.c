/*****************************************************************************************************************************
 * stm32f1xx_hal_msp.c - all MSP hooks unused: clocks and pins are brought up
 * locally in each bare driver / main.c (this is not a CubeMX project).
 *****************************************************************************************************************************/

#include "stm32f1xx_hal.h"

void HAL_MspInit(void)
{
	__HAL_RCC_AFIO_CLK_ENABLE();
	__HAL_RCC_PWR_CLK_ENABLE();
}
