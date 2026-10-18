/*****************************************************************************************************************************
 * can_bare.c - register-level bxCAN (F103, CAN1 on PA11/PA12, default mapping).
 *
 * Design notes:
 *  - auto retransmission ON (NART=0): a frame without any ACK keeps retrying,
 *    so every send waits RQCP with a timeout and ABORTS the mailbox on expiry,
 *    otherwise a silent bus (node jumped to its app, cable pulled) wedges TX
 *    forever.
 *  - ABOM=1: automatic bus-off recovery, bench friendly.
 *  - filter: 16-bit id list, only 0x6E1 (device replies) reaches FIFO0.
 *****************************************************************************************************************************/

#include "stm32f1xx_hal.h"
#include "can_bare.h"

static CanStats stats;

static void can_gpio_init(void)
{
	GPIO_InitTypeDef gpio = {0};

	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_AFIO_CLK_ENABLE();
	__HAL_RCC_CAN1_CLK_ENABLE();

	/* PA11 = CAN_RX: floating input; PA12 = CAN_TX: AF push-pull */
	gpio.Pin = GPIO_PIN_11;
	gpio.Mode = GPIO_MODE_INPUT;
	gpio.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(GPIOA, &gpio);

	gpio.Pin = GPIO_PIN_12;
	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOA, &gpio);
}

void can_bare_init(void)
{
	uint32_t t0;

	can_gpio_init();

	/* enter init mode */
	CAN->MCR &= ~(CAN_MCR_SLEEP | CAN_MCR_DBF);
	CAN->MCR |= CAN_MCR_INRQ;
	t0 = HAL_GetTick();
	while((CAN->MSR & CAN_MSR_INAK) == 0U)
	{
		if((HAL_GetTick() - t0) > 10U)
		{
			return;   /* peripheral wedged: leave defaults, sends will fail */
		}
	}

	CAN->MCR = CAN_MCR_ABOM            /* auto bus-off recovery */
	         | CAN_MCR_INRQ;           /* NART=0 -> auto retransmission */
	CAN->BTR = ((0U) << 24)            /* SJW = 1 tq */
	         | ((1U) << 20)            /* TS2 = 2 tq */
	         | ((14U) << 16)           /* TS1 = 15 tq */
	         | (3U);                   /* BRP = 4  -> 36/4 = 9 MHz, 18 tq = 500 kbps */

	/* filter bank 0: 16-bit id list, accept only 0x6E1 into FIFO0 */
	CAN->FMR |= CAN_FMR_FINIT;
	CAN->FM1R |= CAN_FM1R_FMBM0;                       /* list mode */
	CAN->FS1R &= ~CAN_FS1R_FSCB0;                      /* 16-bit scale */
	CAN->FFA1R &= ~CAN_FFA1R_FFA0;                     /* FIFO0 */
	CAN->sFilterRegister[0].FR1 = ((uint32_t)(CANM_RSP_ID << 5) << 16) | (uint32_t)(CANM_RSP_ID << 5);
	CAN->sFilterRegister[0].FR2 = ((uint32_t)(CANM_RSP_ID << 5) << 16) | (uint32_t)(CANM_RSP_ID << 5);
	CAN->FA1R |= CAN_FA1R_FACT0;
	CAN->FMR &= ~CAN_FMR_FINIT;

	/* leave init mode -> peripheral goes on the bus */
	CAN->MCR &= ~CAN_MCR_INRQ;
	t0 = HAL_GetTick();
	while((CAN->MSR & CAN_MSR_INAK) != 0U)
	{
		if((HAL_GetTick() - t0) > 10U)
		{
			return;
		}
	}
}

static const uint32_t abrq_bit[3] = {CAN_TSR_ABRQ0, CAN_TSR_ABRQ1, CAN_TSR_ABRQ2};
static const uint32_t rqcp_bit[3] = {CAN_TSR_RQCP0, CAN_TSR_RQCP1, CAN_TSR_RQCP2};
static const uint32_t txok_bit[3] = {CAN_TSR_TXOK0, CAN_TSR_TXOK1, CAN_TSR_TXOK2};
static const uint32_t tme_bit[3]  = {CAN_TSR_TME0, CAN_TSR_TME1, CAN_TSR_TME2};

uint8_t can_bare_send(uint32_t std_id, const uint8_t d[8])
{
	uint8_t mb;
	uint32_t t0;

	for(mb = 0U; mb < 3U; mb++)
	{
		if((CAN->TSR & tme_bit[mb]) != 0U)
		{
			break;
		}
	}
	if(mb >= 3U)
	{
		stats.tx_fail++;
		return 0U;
	}

	CAN->sTxMailBox[mb].TDLR = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
	CAN->sTxMailBox[mb].TDHR = (uint32_t)d[4] | ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
	CAN->sTxMailBox[mb].TDTR = 8U;
	CAN->sTxMailBox[mb].TIR  = (std_id << CAN_TI0R_STID_Pos) | CAN_TI0R_TXRQ;

	/* 20 ms: a 500 kbps frame needs ~0.25 ms; anything longer means no ACK */
	t0 = HAL_GetTick();
	while((CAN->TSR & rqcp_bit[mb]) == 0U)
	{
		if((HAL_GetTick() - t0) > 20U)
		{
			CAN->TSR = abrq_bit[mb];   /* abort: free the mailbox */
			stats.tx_fail++;
			return 0U;
		}
	}
	if((CAN->TSR & txok_bit[mb]) != 0U)
	{
		stats.tx_ok++;
		return 1U;
	}
	stats.tx_fail++;   /* aborted by error (ALST/TERR) */
	return 0U;
}

uint8_t can_bare_recv(uint32_t *std_id, uint8_t d[8])
{
	uint32_t rir, dl, dh;

	if((CAN->RF0R & CAN_RF0R_FMP0) == 0U)
	{
		return 0U;
	}

	rir = CAN->sFIFOMailBox[0].RIR;
	dl = CAN->sFIFOMailBox[0].RDLR;
	dh = CAN->sFIFOMailBox[0].RDHR;
	d[0] = (uint8_t)(dl & 0xFFU);
	d[1] = (uint8_t)((dl >> 8) & 0xFFU);
	d[2] = (uint8_t)((dl >> 16) & 0xFFU);
	d[3] = (uint8_t)((dl >> 24) & 0xFFU);
	d[4] = (uint8_t)(dh & 0xFFU);
	d[5] = (uint8_t)((dh >> 8) & 0xFFU);
	d[6] = (uint8_t)((dh >> 16) & 0xFFU);
	d[7] = (uint8_t)((dh >> 24) & 0xFFU);

	*std_id = (rir >> CAN_RI0R_STID_Pos) & 0x7FFU;
	CAN->RF0R |= CAN_RF0R_RFOM0;   /* release FIFO0 slot */
	stats.rx_ok++;
	return 1U;
}

void can_bare_stats(CanStats *out)
{
	stats.tec  = (uint8_t)((CAN->ESR >> 8) & 0xFFU);
	stats.rec  = (uint8_t)((CAN->ESR >> 16) & 0xFFU);
	stats.boff = (uint8_t)((CAN->ESR & CAN_ESR_BOFF) ? 1U : 0U);
	*out = stats;
}
