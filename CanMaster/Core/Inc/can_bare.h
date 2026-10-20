/*****************************************************************************************************************************
 * can_bare.h - register-level bxCAN driver (no HAL CAN dependency: the meter
 * project skeleton carries no HAL CAN sources, and the master is polled anyway).
 *
 * Bus: 500 kbps, APB1 = 36 MHz (72 MHz HCLK / 2), prescaler 4 -> 9 MHz tq,
 * 1 + 15 + 2 = 18 tq -> sample point 88.9 % (the F407 node runs 87.5 %;
 * equal nominal rate, interop safe).
 *****************************************************************************************************************************/

#ifndef CAN_BARE_H_
#define CAN_BARE_H_

#include <stdint.h>

/* 1 = silent listen build (C2 bus verdict experiment): BTR gets SILM (TX stays
 * recessive, never ACKs), filter accepts every id, main loop is a monitor.
 * 0 = normal master build. */
#define CANM_SNIFFER   0

#define CANM_REQ_ID   0x6E0U   /* host -> device */
#define CANM_RSP_ID   0x6E1U   /* device -> host */

typedef struct
{
	uint32_t tx_ok;       /* frames transmitted (mailbox completed) */
	uint32_t tx_fail;     /* aborted: no ACK / bus problem */
	uint32_t rx_ok;       /* accepted frames pulled from FIFO0 */
	uint32_t rx_dropped;  /* frames seen with wrong id (should stay 0) */
	uint8_t  tec;         /* TX error counter snapshot (ESR) */
	uint8_t  rec;         /* RX error counter snapshot (ESR) */
	uint8_t  boff;        /* bus-off flag snapshot (ESR) */
} CanStats;

void     can_bare_init(void);
uint8_t  can_bare_send(uint32_t std_id, const uint8_t d[8]);      /* 1 = acked */
uint8_t  can_bare_recv(uint32_t *std_id, uint8_t d[8]);           /* 1 = got one */
void     can_bare_stats(CanStats *out);

#if CANM_SNIFFER
/* LEC (ESR[6:4]) is the hardware verdict on the last bus event; it latches
 * until software re-arms it by writing 111. Poll often from the main loop.
 * Counters by code: 0=none 1=stuff 2=form 3=ack 4=bit-rec 5=bit-dom 6=crc. */
void     can_bare_lec_poll(void);
void     can_bare_lec_get(uint32_t cnt[7]);
/* PA11 (CAN_RX pin) raw level sampler: does the signal reach the pin at all */
void     can_bare_pa11_poll(void);
void     can_bare_pa11_get(uint32_t *low, uint32_t *tot);
#endif

#endif /* CAN_BARE_H_ */
