/*****************************************************************************************************************************
 * usart_bare.c - see usart_bare.h. BRR = 72 MHz / 115200 = 625 exactly.
 *****************************************************************************************************************************/

#include "stm32f1xx_hal.h"
#include "usart_bare.h"

#define RING_SIZE   512U   /* power of two; one 75-byte command plus slack */

static volatile uint8_t  ring[RING_SIZE];
static volatile uint16_t ring_head = 0U;   /* IRQ writes here */
static volatile uint16_t ring_tail = 0U;   /* main loop reads here */
static volatile uint16_t dropped   = 0U;

void usart_bare_init(void)
{
	GPIO_InitTypeDef gpio = {0};

	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_USART1_CLK_ENABLE();

	/* F1: TX = AF push-pull, RX = floating input */
	gpio.Pin = GPIO_PIN_9;
	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOA, &gpio);

	gpio.Pin = GPIO_PIN_10;
	gpio.Mode = GPIO_MODE_INPUT;
	gpio.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(GPIOA, &gpio);

	USART1->BRR = 625U;                       /* 72 MHz / 115200 */
	USART1->CR1 = 0U;
	USART1->CR2 = 0U;
	USART1->CR3 = 0U;
	USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;

	HAL_NVIC_SetPriority(USART1_IRQn, 5U, 0U);
	HAL_NVIC_EnableIRQ(USART1_IRQn);
}

void usart_bare_irq(void)
{
	uint16_t next;
	uint8_t b;

	if((USART1->SR & USART_SR_ORE) != 0U)
	{
		(void)USART1->SR;                      /* ORE clears on the SR+DR read sequence */
		(void)USART1->DR;
		dropped++;
		return;
	}
	if((USART1->SR & USART_SR_RXNE) == 0U)
	{
		return;
	}
	b = (uint8_t)USART1->DR;
	next = (uint16_t)((ring_head + 1U) & (RING_SIZE - 1U));
	if(next != ring_tail)
	{
		ring[ring_head] = b;
		ring_head = next;
	}
	else
	{
		dropped++;                             /* ring full: drop newest */
	}
}

uint8_t usart_bare_pop(uint8_t *b)
{
	uint16_t head = ring_head;                 /* single IRQ writer, snapshot once */

	if(ring_tail == head)
	{
		return 0U;
	}
	*b = ring[ring_tail];
	ring_tail = (uint16_t)((ring_tail + 1U) & (RING_SIZE - 1U));
	return 1U;
}

uint16_t usart_bare_pending(void)
{
	return (uint16_t)((ring_head - ring_tail) & (RING_SIZE - 1U));
}

void usart_bare_write(const uint8_t *s, uint16_t n)
{
	uint32_t t0;

	while(n > 0U)
	{
		t0 = HAL_GetTick();
		while((USART1->SR & USART_SR_TXE) == 0U)
		{
			if((HAL_GetTick() - t0) > 50U)
			{
				return;                        /* TX wedged: drop the rest */
			}
		}
		USART1->DR = *s++;
		n--;
	}
	t0 = HAL_GetTick();
	while((USART1->SR & USART_SR_TC) == 0U)
	{
		if((HAL_GetTick() - t0) > 50U)
		{
			return;
		}
	}
}
