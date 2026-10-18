/*****************************************************************************************************************************
 * bridge.c - see bridge.h.
 *****************************************************************************************************************************/

#include <stdio.h>
#include <string.h>
#include "stm32f1xx_hal.h"
#include "can_bare.h"
#include "usart_bare.h"
#include "isotp_master.h"
#include "bridge.h"

#define BRIDGE_MAGIC_BYTE   0xAAU
#define BRIDGE_STATS_CMD    0x5AU
#define BRIDGE_REPLY_TO_MS  4500U   /* fota_host reads with a 5 s timeout */

typedef enum { P_WAIT_LEN = 0, P_COLLECT } ParseState;

static uint8_t  pdu[ISOTP_MAX + 1];
static uint8_t  reply[ISOTP_MAX + 1];
static uint16_t pdu_pos;
static ParseState state = P_WAIT_LEN;
static uint8_t  exchange_open;      /* between request accepted and reply flushed */
static uint8_t  link_ok = 1U;       /* last CAN TX was ACKed (or nothing sent yet) */

typedef struct
{
	uint32_t requests;
	uint32_t replies;
	uint32_t rx_timeouts;
	uint32_t send_fails;
	uint32_t magic_swallowed;
	uint32_t junk_dropped;
	uint32_t stats_served;
	uint32_t keepalives;
} BridgeStats;

static BridgeStats bstats;

void bridge_init(void)
{
	state = P_WAIT_LEN;
	pdu_pos = 0U;
	exchange_open = 0U;
}

/* Feed one PC byte; returns 1 when a complete request PDU sits in pdu[]. */
static uint8_t parse_feed(uint8_t b)
{
	if(state == P_WAIT_LEN)
	{
		if(b == BRIDGE_MAGIC_BYTE)
		{
			bstats.magic_swallowed++;
			return 0U;   /* reboot magic: UART-app-only, never valid as a LEN byte */
		}
		if((b < 2U) || (b > ISOTP_MAX))
		{
			bstats.junk_dropped++;
			return 0U;
		}
		pdu[0] = b;
		pdu_pos = 1U;
		state = P_COLLECT;
		return 0U;
	}
	pdu[pdu_pos++] = b;
	if(pdu_pos >= (uint16_t)(pdu[0] + 1U))
	{
		state = P_WAIT_LEN;
		pdu_pos = 0U;
		return 1U;
	}
	return 0U;
}

static void stats_line(char *out, uint32_t n)
{
	CanStats c;

	can_bare_stats(&c);
	(void)snprintf(out, n,
		"CANM req=%lu rep=%lu rxto=%lu sfail=%lu ka=%lu magic=%lu junk=%lu "
		"| CAN tx=%lu txfail=%lu rx=%lu tec=%lu rec=%lu boff=%u\r\n",
		(unsigned long)bstats.requests, (unsigned long)bstats.replies,
		(unsigned long)bstats.rx_timeouts, (unsigned long)bstats.send_fails,
		(unsigned long)bstats.keepalives, (unsigned long)bstats.magic_swallowed,
		(unsigned long)bstats.junk_dropped,
		(unsigned long)c.tx_ok, (unsigned long)c.tx_fail, (unsigned long)c.rx_ok,
		(unsigned long)c.tec, (unsigned long)c.rec, (unsigned)c.boff);
}

static void handle_pdu(void)
{
	int16_t rlen;
	char line[160];

	bstats.requests++;

	if(pdu[1] == BRIDGE_STATS_CMD)
	{
		/* master-local query: answer ourselves, do not touch the bus */
		bstats.stats_served++;
		stats_line(line, sizeof(line));
		usart_bare_write((const uint8_t *)line, (uint16_t)strlen(line));
		return;
	}

	exchange_open = 1U;
	if(isotp_send_pdu(pdu, (uint16_t)(pdu[0] + 1U)) == 0U)
	{
		bstats.send_fails++;
		link_ok = 0U;
		exchange_open = 0U;
		return;   /* stay silent: the host times out and the user retries */
	}
	link_ok = 1U;
	rlen = isotp_recv_pdu(reply, BRIDGE_REPLY_TO_MS);
	if(rlen > 0)
	{
		usart_bare_write(reply, (uint16_t)rlen);
		bstats.replies++;
	}
	else
	{
		bstats.rx_timeouts++;
	}
	exchange_open = 0U;
}

void bridge_service(void)
{
	uint8_t b;

	while(usart_bare_pop(&b) != 0U)
	{
		if(parse_feed(b) != 0U)
		{
			handle_pdu();
		}
	}
}

uint8_t bridge_busy(void)
{
	return (uint8_t)((state == P_COLLECT) || (exchange_open != 0U));
}

void bridge_keepalive(void)
{
	uint8_t f[8] = {0};   /* SF, length 0: dropped by the slave, extends its window */

	if(can_bare_send(CANM_REQ_ID, f) != 0U)
	{
		bstats.keepalives++;
		link_ok = 1U;
	}
	else
	{
		link_ok = 0U;
	}
}

uint8_t bridge_link_ok(void)
{
	return link_ok;
}
