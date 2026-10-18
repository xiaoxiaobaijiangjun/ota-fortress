/*****************************************************************************************************************************
 * CanMaster main - F103C8T6 CAN upgrade master station (C2).
 *
 * Board: generic F103C8T6 minimal system board, 8 MHz HSE -> 72 MHz PLL.
 * Wiring: USART1 PA9/PA10 to the PC (CH340, crossed), CAN PA11/PA12 to the
 * TJA1050 transceiver (5 V), LED on PC13 (active low, if the board has one).
 *
 * Main loop = pump + keep-alive:
 *  - bridge_service() moves LEN-framed commands between UART and CAN
 *  - when idle, one length-0 SF every 300 ms keeps the F407 bootloader in its
 *    command window; after 10 consecutive unanswered sends (partner absent)
 *    the rate drops to one probe every 3 s so a later RESET still re-catches.
 *****************************************************************************************************************************/

#include "stm32f1xx_hal.h"
#include "can_bare.h"
#include "usart_bare.h"
#include "isotp_master.h"
#include "bridge.h"

#define KA_FAST_MS   300U
#define KA_SLOW_MS   800U

static void Error_Handler(void) { for(;;) {} }

static void SystemClock_Config(void)
{
	RCC_OscInitTypeDef osc = {0};
	RCC_ClkInitTypeDef clk = {0};

	osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
	osc.HSEState = RCC_HSE_ON;
	osc.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
	osc.PLL.PLLState = RCC_PLL_ON;
	osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
	osc.PLL.PLLMUL = RCC_PLL_MUL9;              /* 8 MHz * 9 = 72 MHz */
	if(HAL_RCC_OscConfig(&osc) != HAL_OK)
	{
		Error_Handler();
	}

	clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
	                RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	clk.AHBCLKDivider = RCC_SYSCLK_DIV1;        /* HCLK  72 MHz */
	clk.APB1CLKDivider = RCC_HCLK_DIV2;         /* PCLK1 36 MHz (CAN)  */
	clk.APB2CLKDivider = RCC_HCLK_DIV1;         /* PCLK2 72 MHz (USART1) */
	if(HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK)
	{
		Error_Handler();
	}
}

static void led_init(void)
{
	GPIO_InitTypeDef gpio = {0};

	__HAL_RCC_GPIOC_CLK_ENABLE();
	gpio.Pin = GPIO_PIN_13;
	gpio.Mode = GPIO_MODE_OUTPUT_PP;
	gpio.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOC, &gpio);
	HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);   /* off (active low) */
}

int main(void)
{
	uint32_t last_ka = 0U, last_blink = 0U, blink_fast_until = 0U;

	HAL_Init();
	SystemClock_Config();
	led_init();
	isotp_init();
	can_bare_init();
	usart_bare_init();
	bridge_init();

	for(;;)
	{
		bridge_service();

		/* keep-alive only when nothing is in flight; unanswered sends (no ACK
		 * on the bus) drop the rate so a silent bus is not hammered */
		if(bridge_busy() == 0U)
		{
			uint32_t now = HAL_GetTick();
			uint32_t period = bridge_link_ok() ? KA_FAST_MS : KA_SLOW_MS;

			if((now - last_ka) >= period)
			{
				bridge_keepalive();
				last_ka = now;
				if(bridge_link_ok() != 0U)
				{
					blink_fast_until = now + 200U;   /* flash on each ACKed KA */
				}
			}
		}

		/* LED: slow idle heartbeat, short flash on keep-alive ACK */
		{
			uint32_t now = HAL_GetTick();

			if(now < blink_fast_until)
			{
				if(((now / 50U) & 1U) != 0U)
				{
					HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
				}
				else
				{
					HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
				}
			}
			else if((now - last_blink) >= 1000U)
			{
				HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
				last_blink = now;
			}
		}
	}
}
