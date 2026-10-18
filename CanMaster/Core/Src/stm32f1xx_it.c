/*****************************************************************************************************************************
 * stm32f1xx_it.c - interrupt vectors this firmware actually uses.
 *****************************************************************************************************************************/

#include "stm32f1xx_hal.h"
#include "stm32f1xx_it.h"
#include "usart_bare.h"

void NMI_Handler(void)       { for(;;) {} }
void HardFault_Handler(void) { for(;;) {} }
void SVC_Handler(void)       { }
void PendSV_Handler(void)    { }

void SysTick_Handler(void)
{
	HAL_IncTick();
}

void USART1_IRQHandler(void)
{
	usart_bare_irq();
}
