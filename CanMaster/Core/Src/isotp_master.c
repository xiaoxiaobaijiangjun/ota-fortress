/*****************************************************************************************************************************
 * isotp_master.c - see isotp_master.h. Sender mirrors BL_CAN_Send_Packet on the
 * F407 (FF -> wait the receiver's FC -> CFs with SN, BS blocks, STmin spacing);
 * receiver mirrors can_rx_feed (FC after FF and every BS CFs, SN check, 500 ms
 * CF stall timeout). Time base: DWT cycle counter (72 cycles/us), never stalls
 * here but immune to it anyway.
 *****************************************************************************************************************************/

#include "stm32f1xx_hal.h"
#include "can_bare.h"
#include "isotp_master.h"

#define FC_BS       8U
#define FC_STMIN_MS 1U

static void delay_us(uint32_t us)
{
	uint32_t t0 = DWT->CYCCNT;
	while((DWT->CYCCNT - t0) < (us * 72U))
	{
	}
}

void isotp_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	DWT->CYCCNT = 0U;
}

/* Pull one frame from 0x6E1 with a deadline; returns 1 with d filled. */
static uint8_t get_rsp_frame(uint8_t d[8], uint32_t deadline_ms)
{
	uint32_t id;

	while((int32_t)(deadline_ms - HAL_GetTick()) > 0)
	{
		if(can_bare_recv(&id, d) != 0U)
		{
			if(id == CANM_RSP_ID)
			{
				return 1U;
			}
			continue;   /* not ours (cannot happen with the filter, cheap safety) */
		}
	}
	return 0U;
}

/* Wait for a flow-control frame from the device. 1 = got CTS, bs/stmin filled. */
static uint8_t wait_fc(uint8_t *bs, uint8_t *stmin, uint32_t timeout_ms)
{
	uint8_t d[8];
	uint32_t deadline = HAL_GetTick() + timeout_ms;

	while(get_rsp_frame(d, deadline) != 0U)
	{
		if((d[0] >> 4) == 0x3U)
		{
			if(d[0] == 0x30U)
			{
				*bs = d[1];
				*stmin = (d[2] > 0xF0U) ? 1U : d[2];
				return 1U;
			}
			return 0U;   /* OVFLW / abort */
		}
		/* non-FC frame: keep waiting, multi-frame replies cannot arrive here */
	}
	return 0U;
}

uint8_t isotp_send_pdu(const uint8_t *data, uint16_t len)
{
	uint8_t f[8] = {0};
	uint8_t sn = 0U, since_fc = 0U;
	uint8_t bs = FC_BS, stmin = FC_STMIN_MS;
	uint16_t off;

	if((len == 0U) || (len > ISOTP_MAX))
	{
		return 0U;
	}
	if(len <= 7U)
	{
		f[0] = (uint8_t)(0x00U | len);
		for(uint16_t i = 0U; i < len; i++) { f[1U + i] = data[i]; }
		return can_bare_send(CANM_REQ_ID, f);
	}

	/* First Frame */
	f[0] = (uint8_t)(0x10U | (len >> 8));
	f[1] = (uint8_t)(len & 0xFFU);
	for(uint16_t i = 0U; i < 6U; i++) { f[2U + i] = data[i]; }
	if(can_bare_send(CANM_REQ_ID, f) == 0U)
	{
		return 0U;
	}
	if(wait_fc(&bs, &stmin, 250U) == 0U)
	{
		return 0U;
	}

	/* Consecutive Frames */
	off = 6U;
	while(off < len)
	{
		uint16_t n = (uint16_t)(len - off);
		if(n > 7U)
		{
			n = 7U;
		}
		if((bs != 0U) && (since_fc >= bs))
		{
			if(wait_fc(&bs, &stmin, 250U) == 0U)
			{
				return 0U;
			}
			since_fc = 0U;
		}
		sn = (uint8_t)((sn + 1U) & 0x0FU);
		f[0] = (uint8_t)(0x20U | sn);
		for(uint16_t i = 0U; i < n; i++) { f[1U + i] = data[off + i]; }
		for(uint16_t i = n; i < 7U; i++) { f[1U + i] = 0U; }
		if(can_bare_send(CANM_REQ_ID, f) == 0U)
		{
			return 0U;
		}
		off += n;
		since_fc++;
		if(stmin != 0U)
		{
			delay_us((uint32_t)stmin * 1000U);
		}
	}
	return 1U;
}

int16_t isotp_recv_pdu(uint8_t *buf, uint32_t timeout_ms)
{
	uint8_t d[8];
	uint8_t last_sn = 0U, cf_since_fc = 0U, active = 0U;
	uint16_t expected = 0U, received = 0U;
	uint32_t deadline = HAL_GetTick() + timeout_ms;

	while(get_rsp_frame(d, deadline) != 0U)
	{
		uint8_t pci = (uint8_t)(d[0] >> 4);

		if(pci == 0x0U)   /* SF */
		{
			uint8_t n = (uint8_t)(d[0] & 0x0FU);
			if((n == 0U) || (n > 7U) || (n > ISOTP_MAX))
			{
				continue;
			}
			for(uint8_t i = 0U; i < n; i++) { buf[i] = d[1U + i]; }
			return n;
		}
		if(pci == 0x1U)   /* FF: reply with our FC (BS=8, STmin=1) */
		{
			uint8_t fc[8] = {0};
			expected = (uint16_t)(((uint16_t)(d[0] & 0x0FU) << 8) | d[1]);
			if((expected < 8U) || (expected > ISOTP_MAX))
			{
				continue;
			}
			for(uint8_t i = 0U; i < 6U; i++) { buf[i] = d[2U + i]; }
			received = 6U;
			active = 1U;
			last_sn = 0U;
			cf_since_fc = 0U;
			fc[0] = 0x30U;
			fc[1] = FC_BS;
			fc[2] = FC_STMIN_MS;
			(void)can_bare_send(CANM_REQ_ID, fc);
			continue;
		}
		if((pci == 0x2U) && (active != 0U))   /* CF */
		{
			uint8_t sn = (uint8_t)(d[0] & 0x0FU);
			uint16_t n;

			if(sn != (uint8_t)((last_sn + 1U) & 0x0FU))
			{
				active = 0U;   /* lost frame: give up, the host retries the packet */
				continue;
			}
			last_sn = sn;
			n = (uint16_t)(expected - received);
			if(n > 7U)
			{
				n = 7U;
			}
			for(uint16_t i = 0U; i < n; i++) { buf[received + i] = d[1U + i]; }
			received += n;
			cf_since_fc++;
			if(cf_since_fc >= FC_BS)
			{
				uint8_t fc[8] = {0};
				fc[0] = 0x30U;
				fc[1] = FC_BS;
				fc[2] = FC_STMIN_MS;
				(void)can_bare_send(CANM_REQ_ID, fc);
				cf_since_fc = 0U;
			}
			if(received >= expected)
			{
				return (int16_t)expected;
			}
		}
	}
	return 0;
}
