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
#include <stdio.h>

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

#if CANM_SNIFFER
/*****************************************************************************************************************************
 * Silent bus monitor build (C2 bus verdict experiment). This node never drives
 * the bus - not even an ACK - so the F407's power-on selftest frame (0x6E1)
 * keeps retransmitting, which is the excitation under test.
 *
 * The bench CH340 currently sits on the F407 side, so the primary verdict
 * channel is the PC13 LED; USART1 prints the same data if a USB-TTL is ever
 * attached to PA9 (115200).
 *
 * LED grammar (PC13, active low):
 *   1 Hz square          - alive, bus silent to us, LEC quiet
 *   10 Hz flicker (LATCHED once any frame lands: the F407 excitation window
 *                        is only ~1 s after its RESET, the evidence must not
 *                        blink away at the next mode update) -> frames were
 *                        RECEIVED -> F407 TX + master RX fine, suspicion
 *                        narrows to the master TX path
 *   double-blink + pause - no frames but LEC climbing (stuff/form/ack/bit/crc)
 *                          -> bit-level decode failure: physical/timing, scope
 *
 * Procedure: watch the LED, press RESET on the F407, watch the LED during the
 * second after reset (its selftest frame retransmits for the 1 s boot window;
 * if a valid app is present it jumps away after that).
 * [RX] prints are throttled to 1 per 50 ms; the retransmit storm can run
 * thousands of frames per second, unthrottled printing would starve the
 * LEC poll and overflow FIFO0. Full rate is visible in the [ST] rx counter.
 *****************************************************************************************************************************/
#define LED_SLOW    0U
#define LED_FAST    1U
#define LED_DOUBLE  2U

int main(void)
{
	uint32_t id;
	uint32_t lec[7] = {0};
	uint32_t lec_err_prev = 0U, rx_total = 0U;
	uint32_t last_st = 0U, last_rx_print = 0U, rx_window = 0U;
	uint8_t led_mode = LED_SLOW;
	uint8_t d[8];
	char line[96];

	HAL_Init();
	SystemClock_Config();
	led_init();
	can_bare_init();
	usart_bare_init();

	/* TXD force test: reconfigure PA12 from CAN alternate function to a plain
	 * GPIO push-pull LOW - the strongest possible dominant request a TJA1050
	 * can ever see on its TXD input. If transceiver #1 (and the bus, and #2's
	 * receiver, and the borrowed wire) are alive, PA11 goes ~permanently LOW
	 * and the [ST] pa11 counter saturates. Still ~zero means #1's driver does
	 * not answer even this. */
	{
		GPIO_InitTypeDef gpio = {0};
		gpio.Pin = GPIO_PIN_12;
		gpio.Mode = GPIO_MODE_OUTPUT_PP;
		gpio.Speed = GPIO_SPEED_FREQ_LOW;
		HAL_GPIO_Init(GPIOA, &gpio);
		HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_RESET);
	}

	{
		int n = snprintf(line, sizeof(line),
			"[TEST] PA12 forced LOW = dominant request on TJA#1 TXD\r\n");
		if(n > 0)
		{
			usart_bare_write((const uint8_t *)line, (uint16_t)n);
		}
	}

	/* raw config dump: settles where bxCAN's RX actually lives.
	 * MAPR[15:14] = CAN_REMAP: 00=PA11/PA12 (expected), 01=PB8/PB9, 11=PD0/PD1
	 * CRH[15:12] = PA11 cfg: 0001 = floating input (expected)
	 * CRH[19:16] = PA12 cfg: 1011 = AF push-pull 50MHz (expected) */
	{
		int n = snprintf(line, sizeof(line),
			"[CFG] mapr=%08lX crh=%08lX crl=%08lX mcr=%08lX btr=%08lX esr=%08lX\r\n",
			(unsigned long)AFIO->MAPR, (unsigned long)GPIOA->CRH, (unsigned long)GPIOA->CRL,
			(unsigned long)CAN1->MCR, (unsigned long)CAN1->BTR, (unsigned long)CAN1->ESR);
		if(n > 0)
		{
			usart_bare_write((const uint8_t *)line, (uint16_t)n);
		}
	}

	for(;;)
	{
		can_bare_lec_poll();
		can_bare_pa11_poll();

		while(can_bare_recv(&id, d))
		{
			rx_window++;
			rx_total++;
			if((HAL_GetTick() - last_rx_print) >= 50U)
			{
				int n = snprintf(line, sizeof(line),
					"[RX] %03lX %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
					(unsigned long)id,
					d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
				if(n > 0)
				{
					usart_bare_write((const uint8_t *)line, (uint16_t)n);
					last_rx_print = HAL_GetTick();
				}
			}
		}

		/* verdict LED: mode picked once per second from the last second's data */
		{
			uint32_t now = HAL_GetTick();
			uint32_t on;
			if(led_mode == LED_FAST)
			{
				on = ((now / 50U) & 1U);
			}
			else if(led_mode == LED_DOUBLE)
			{
				on = ((now % 1000U) < 200U) && (((now / 100U) & 1U) == 0U);
			}
			else
			{
				on = ((now / 500U) & 1U);
			}
			HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
			(void)now;
		}

		if((HAL_GetTick() - last_st) >= 1000U)
		{
			CanStats st;
			uint32_t lec_err = 0U;
			uint32_t pa11_low, pa11_tot;
			uint8_t i;
			int n;
			last_st = HAL_GetTick();
			can_bare_stats(&st);
			can_bare_lec_get(lec);
			can_bare_pa11_get(&pa11_low, &pa11_tot);
			for(i = 1U; i < 7U; i++)
			{
				lec_err += lec[i];
			}
			n = snprintf(line, sizeof(line),
				"[ST] rx/s=%lu txok=%lu txfail=%lu pa11=%lu/%lu esr=%08lX tec=%u rec=%u boff=%u lec=%lu,%lu,%lu,%lu,%lu,%lu,%lu\r\n",
				(unsigned long)rx_window,
				(unsigned long)st.tx_ok, (unsigned long)st.tx_fail,
				(unsigned long)pa11_low, (unsigned long)pa11_tot,
				(unsigned long)CAN1->ESR,
				st.tec, st.rec, st.boff,
				(unsigned long)lec[0], (unsigned long)lec[1], (unsigned long)lec[2],
				(unsigned long)lec[3], (unsigned long)lec[4], (unsigned long)lec[5],
				(unsigned long)lec[6]);
			if(n > 0)
			{
				usart_bare_write((const uint8_t *)line, (uint16_t)n);
			}
			led_mode = (rx_total > 0U) ? (uint8_t)LED_FAST
			          : (lec_err > lec_err_prev) ? (uint8_t)LED_DOUBLE
			          : (uint8_t)LED_SLOW;
			lec_err_prev = lec_err;
			rx_window = 0U;
		}
	}
}

#else
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
#endif /* CANM_SNIFFER */
