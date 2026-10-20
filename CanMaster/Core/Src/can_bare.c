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
	CAN1->MCR &= ~(CAN_MCR_SLEEP | CAN_MCR_DBF);
	CAN1->MCR |= CAN_MCR_INRQ;
	t0 = HAL_GetTick();
	while((CAN1->MSR & CAN_MSR_INAK) == 0U)
	{
		if((HAL_GetTick() - t0) > 10U)
		{
			return;   /* peripheral wedged: leave defaults, sends will fail */
		}
	}

	CAN1->MCR = CAN_MCR_ABOM            /* auto bus-off recovery */
	         | CAN_MCR_INRQ;           /* NART=0 -> auto retransmission */
#if CANM_SNIFFER
	/* identical bit timing as the master build (SP 88.9%). NORMAL mode but
	 * the sniffer main loop never transmits: the node becomes a pure ACKer +
	 * listener. With the borrowed 2nd transceiver RXD feeding PA11, this is
	 * enough to close the loop: F407's selftest frame gets ACKed (TXOK on
	 * its side) and lands intact in our FIFO. */
	CAN1->BTR = ((0U) << 24)            /* SJW = 1 tq */
	         | ((1U) << 20)            /* TS2 = 2 tq */
	         | ((14U) << 16)           /* TS1 = 15 tq */
	         | (3U);                   /* BRP = 4  -> 36/4 = 9 MHz, 18 tq = 500 kbps */
#else
	CAN1->BTR = ((0U) << 24)            /* SJW = 1 tq */
	         | ((1U) << 20)            /* TS2 = 2 tq */
	         | ((14U) << 16)           /* TS1 = 15 tq */
	         | (3U);                   /* BRP = 4  -> 36/4 = 9 MHz, 18 tq = 500 kbps */
#endif

	/* filter bank 0 */
	CAN1->FMR |= CAN_FMR_FINIT;
#if CANM_SNIFFER
	/* 32-bit mask mode, mask = 0: accept EVERYTHING into FIFO0 - the point is
	 * to see whatever actually shows up on the wire, at any id */
	CAN1->FM1R &= ~CAN_FM1R_FBM0;
	CAN1->FS1R |= CAN_FS1R_FSC0;
	CAN1->FFA1R &= ~CAN_FFA1R_FFA0;
	CAN1->sFilterRegister[0].FR1 = 0U;
	CAN1->sFilterRegister[0].FR2 = 0U;
#else
	CAN1->FM1R |= CAN_FM1R_FBM0;                       /* list mode */
	CAN1->FS1R &= ~CAN_FS1R_FSC0;                      /* 16-bit scale */
	CAN1->FFA1R &= ~CAN_FFA1R_FFA0;                     /* FIFO0 */
	CAN1->sFilterRegister[0].FR1 = ((uint32_t)(CANM_RSP_ID << 5) << 16) | (uint32_t)(CANM_RSP_ID << 5);
	CAN1->sFilterRegister[0].FR2 = ((uint32_t)(CANM_RSP_ID << 5) << 16) | (uint32_t)(CANM_RSP_ID << 5);
#endif
	CAN1->FA1R |= CAN_FA1R_FACT0;
	CAN1->FMR &= ~CAN_FMR_FINIT;

	/* leave init mode -> peripheral goes on the bus */
	CAN1->MCR &= ~CAN_MCR_INRQ;
	t0 = HAL_GetTick();
	while((CAN1->MSR & CAN_MSR_INAK) != 0U)
	{
		if((HAL_GetTick() - t0) > 10U)
		{
			return;
		}
	}
#if CANM_SNIFFER
	CAN1->ESR = CAN_ESR_LEC;   /* discard the reset-time LEC=0 fake event */
#endif
}

#if CANM_SNIFFER
/* PA11 raw-level sampler: counts LOW samples of the CAN_RX pin straight from
 * the GPIO data register, bypassing bxCAN. During the F407 retransmission
 * storm a live RX path must show LOWs (dominant ~40% of each frame); all-HIGH
 * means the signal dies before the pin: TJA1 receive side or its wire. */
static uint32_t pa11_low;
static uint32_t pa11_tot;

void can_bare_pa11_poll(void)
{
	pa11_tot++;
	if((GPIOA->IDR & (1UL << 11)) == 0U)
	{
		pa11_low++;
	}
}

void can_bare_pa11_get(uint32_t *low, uint32_t *tot)
{
	*low = pa11_low;
	*tot = pa11_tot;
}
#endif

void can_bare_set_timing(uint32_t brp, uint32_t ts1, uint32_t ts2)
{
	uint32_t t0;

	CAN1->MCR |= CAN_MCR_INRQ;
	t0 = HAL_GetTick();
	while((CAN1->MSR & CAN_MSR_INAK) == 0U)
	{
		if((HAL_GetTick() - t0) > 10U)
		{
			return;
		}
	}
	CAN1->BTR = ((0U) << 24) | (ts2 << 20) | (ts1 << 16) | brp;
	CAN1->MCR &= ~CAN_MCR_INRQ;
	t0 = HAL_GetTick();
	while((CAN1->MSR & CAN_MSR_INAK) != 0U)
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
		if((CAN1->TSR & tme_bit[mb]) != 0U)
		{
			break;
		}
	}
	if(mb >= 3U)
	{
		stats.tx_fail++;
		return 0U;
	}

	/* RQCPx is write-1-clear and PERSISTS after a completed request: without
	 * clearing, every wait after the first sees the STALE flag and returns the
	 * STALE verdict instantly (found the hard way on the bench: frames were
	 * being ACKed on the bus while the code reported failure). TXOK/ALST/TERR
	 * clear together with RQCP. ABRQ is deliberately not written here - it is
	 * an abort REQUEST and would kill the transmission we are about to start. */
	CAN1->TSR = rqcp_bit[mb];

	CAN1->sTxMailBox[mb].TDLR = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
	CAN1->sTxMailBox[mb].TDHR = (uint32_t)d[4] | ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
	CAN1->sTxMailBox[mb].TDTR = 8U;
	CAN1->sTxMailBox[mb].TIR  = (std_id << CAN_TI0R_STID_Pos) | CAN_TI0R_TXRQ;

	/* 20 ms: a 500 kbps frame needs ~0.25 ms; anything longer means no ACK */
	t0 = HAL_GetTick();
	while((CAN1->TSR & rqcp_bit[mb]) == 0U)
	{
		if((HAL_GetTick() - t0) > 20U)
		{
			CAN1->TSR = abrq_bit[mb];   /* abort: free the mailbox */
			stats.tx_fail++;
			return 0U;
		}
	}
	if((CAN1->TSR & txok_bit[mb]) != 0U)
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

	if((CAN1->RF0R & CAN_RF0R_FMP0) == 0U)
	{
		return 0U;
	}

	rir = CAN1->sFIFOMailBox[0].RIR;
	dl = CAN1->sFIFOMailBox[0].RDLR;
	dh = CAN1->sFIFOMailBox[0].RDHR;
	d[0] = (uint8_t)(dl & 0xFFU);
	d[1] = (uint8_t)((dl >> 8) & 0xFFU);
	d[2] = (uint8_t)((dl >> 16) & 0xFFU);
	d[3] = (uint8_t)((dl >> 24) & 0xFFU);
	d[4] = (uint8_t)(dh & 0xFFU);
	d[5] = (uint8_t)((dh >> 8) & 0xFFU);
	d[6] = (uint8_t)((dh >> 16) & 0xFFU);
	d[7] = (uint8_t)((dh >> 24) & 0xFFU);

	*std_id = (rir >> CAN_RI0R_STID_Pos) & 0x7FFU;
	CAN1->RF0R |= CAN_RF0R_RFOM0;   /* release FIFO0 slot */
	stats.rx_ok++;
	return 1U;
}

void can_bare_stats(CanStats *out)
{
	stats.tec  = (uint8_t)((CAN1->ESR >> 8) & 0xFFU);
	stats.rec  = (uint8_t)((CAN1->ESR >> 16) & 0xFFU);
	stats.boff = (uint8_t)((CAN1->ESR & CAN_ESR_BOFF) ? 1U : 0U);
	*out = stats;
}

#if CANM_SNIFFER
static uint32_t lec_cnt[7];

void can_bare_lec_poll(void)
{
	uint32_t lec = (CAN1->ESR & CAN_ESR_LEC) >> CAN_ESR_LEC_Pos;
	if(lec != 7U)
	{
		lec_cnt[lec]++;
		CAN1->ESR = CAN_ESR_LEC;   /* 111 = re-arm for the next event */
	}
}

void can_bare_lec_get(uint32_t cnt[7])
{
	uint8_t i;
	for(i = 0U; i < 7U; i++)
	{
		cnt[i] = lec_cnt[i];
	}
}
#endif
