/*****************************************************************************************************************************
 * can_transport.c - bxCAN driver + simplified ISO-TP RX/TX for the CAN
 * flashing transport. See can_transport.h for the frame formats.
 *
 * Concurrency model: everything is polled (no CAN interrupts), matching the
 * UART transport style. Long waits refresh the IWDG.
 *****************************************************************************************************************************/

#include <string.h>
#include "can_transport.h"
#include "bootloader.h"
#include "usart.h"

#define BL_CAN_MAX_PACKET      112U   /* covers the 75-byte upload command */

/* ISO-TP flow control parameters we advertise / honor */
#define BL_CAN_FC_BS           8U
#define BL_CAN_FC_STMIN_MS     1U
#define BL_CAN_CF_TIMEOUT_MS   500U
#define BL_CAN_FC_TIMEOUT_MS   200U

static CAN_HandleTypeDef hcan1;

/* ---------------- RX reassembly state ---------------- */
typedef struct
{
	uint8_t  buf[BL_CAN_MAX_PACKET];
	uint16_t expected;
	uint16_t received;
	uint8_t  active;
	uint8_t  last_sn;
	uint8_t  cf_since_fc;
	uint32_t last_frame_tick;
} BL_CanRx_t;

static BL_CanRx_t rx;

/* Defined in bootloader.c */
extern void BL_Set_Transport(uint8_t is_can);
extern BL_Status BL_Process_Packet(const uint8_t *Host_Buffer);
extern uint32_t Bootloader_CRC_Calculate(const uint8_t *pData, uint32_t Data_Len);
extern uint8_t Bootloader_CRC_Verify(uint8_t *pData, uint32_t Data_Len, uint32_t Host_CRC);

/************************************    Low-level frame IO    ************************************/
static uint8_t can_send_raw(uint32_t std_id, const uint8_t *d)
{
	CAN_TxHeaderTypeDef hdr;
	uint32_t mailbox = 0;
	uint32_t t0 = HAL_GetTick();

	memset(&hdr, 0, sizeof(hdr));
	hdr.StdId = std_id;
	hdr.IDE = CAN_ID_STD;
	hdr.RTR = CAN_RTR_DATA;
	hdr.DLC = 8;
	hdr.TransmitGlobalTime = DISABLE;

	while(HAL_CAN_GetTxMailboxesFreeLevel(&hcan1) == 0U)
	{
		if((HAL_GetTick() - t0) > 20U)
		{
			return 0;
		}
	}
	return (HAL_CAN_AddTxMessage(&hcan1, &hdr, (uint8_t *)d, &mailbox) == HAL_OK) ? 1U : 0U;
}

#if BL_CAN_LOOPBACK_TEST
/* Forward one of our own TX frames (looped back by bxCAN) to the host over
 * the UART tunnel: [13][0x1D][8-byte frame][crc32 over the first 10 bytes].
 * Raw UART on purpose: the transport router points at CAN right now, going
 * through it would recurse. */
static void can_forward_to_uart(const uint8_t *d)
{
	uint8_t pkt[14];
	uint32_t crc;

	pkt[0] = 13U;
	pkt[1] = CBL_CAN_FWD_CMD;
	memcpy(&pkt[2], d, 8U);
	crc = Bootloader_CRC_Calculate(pkt, 10U);
	pkt[10] = (uint8_t)(crc & 0xFFU);
	pkt[11] = (uint8_t)((crc >> 8) & 0xFFU);
	pkt[12] = (uint8_t)((crc >> 16) & 0xFFU);
	pkt[13] = (uint8_t)((crc >> 24) & 0xFFU);
	(void)HAL_UART_Transmit(BL_HOST_COMMUNICATION_UART, pkt, sizeof(pkt), HAL_MAX_DELAY);
}

/* Pull one pending UART packet (if any) and inject its CAN frame. Used while
 * the TX side blocks waiting for the host's FC frame, so the tunnel stays
 * full-duplex without interrupts. */
static void can_pump_uart(void)
{
	uint8_t len;
	uint8_t pkt[100];

	if(HAL_UART_Receive(BL_HOST_COMMUNICATION_UART, &len, 1U, 1U) != HAL_OK)
	{
		return;
	}
	if((len < 2U) || (len > 99U))
	{
		return;   /* stray byte: drop (the boot banner case, never expected here) */
	}
	pkt[0] = len;
	if(HAL_UART_Receive(BL_HOST_COMMUNICATION_UART, &pkt[1], len, 50U) != HAL_OK)
	{
		return;
	}
	if(Bootloader_CRC_Verify(pkt, (uint32_t)(len + 1U) - 4U,
	   *((uint32_t *)&pkt[len + 1U - 4U])) != CRC_VERIFICATION_PASSED)
	{
		return;
	}
	if(pkt[1] == CBL_CAN_INJECT_CMD)
	{
		(void)can_send_raw(BL_CAN_REQ_ID, &pkt[2]);
	}
}
#endif /* BL_CAN_LOOPBACK_TEST */

/************************************    RX reassembly    ************************************/
static void can_rx_reset(void)
{
	rx.active = 0;
	rx.received = 0;
	rx.expected = 0;
	rx.last_sn = 0;
	rx.cf_since_fc = 0;
}

static void can_rx_dispatch(void)
{
	/* The complete [len][cmd][...][crc32] packet is exactly what the UART
	 * transport would have delivered: same dispatcher, different carrier. */
	can_rx_reset();
	BL_Set_Transport(1U);
	(void)BL_Process_Packet(rx.buf);
	BL_Set_Transport(0U);
}

static void can_rx_feed(const uint8_t *d)
{
	uint8_t pci = d[0] >> 4;

	switch(pci)
	{
		case 0x0:   /* SF */
		{
			uint8_t n = d[0] & 0x0FU;
			if((n == 0U) || (n > 7U) || (n > BL_CAN_MAX_PACKET))
			{
				return;
			}
			memcpy(rx.buf, &d[1], n);
			rx.expected = n;
			can_rx_dispatch();
			break;
		}

		case 0x1:   /* FF */
		{
			uint16_t total = (uint16_t)(((uint16_t)(d[0] & 0x0FU) << 8) | d[1]);
			if((total < 8U) || (total > BL_CAN_MAX_PACKET))
			{
				can_rx_reset();
				return;
			}
			rx.expected = total;
			rx.received = 6U;
			memcpy(rx.buf, &d[2], 6U);
			rx.active = 1U;
			rx.last_sn = 0U;
			rx.cf_since_fc = 0U;
			rx.last_frame_tick = HAL_GetTick();
			/* flow control: we are the receiver, allow BS frames then more */
			{
				uint8_t fc[8] = {0};
				fc[0] = 0x30U;
				fc[1] = BL_CAN_FC_BS;
				fc[2] = BL_CAN_FC_STMIN_MS;
				(void)can_send_raw(BL_CAN_RSP_ID, fc);
			}
			break;
		}

		case 0x2:   /* CF */
		{
			uint8_t sn = d[0] & 0x0FU;
			uint16_t n;

			if(!rx.active)
			{
				return;
			}
			rx.last_frame_tick = HAL_GetTick();
			if(sn != ((rx.last_sn + 1U) & 0x0FU))
			{
				can_rx_reset();   /* lost frame: abort, host retries the packet */
				return;
			}
			rx.last_sn = sn;
			n = (uint16_t)(rx.expected - rx.received);
			if(n > 7U)
			{
				n = 7U;
			}
			memcpy(&rx.buf[rx.received], &d[1], n);
			rx.received += n;
			rx.cf_since_fc++;
			if(rx.cf_since_fc >= BL_CAN_FC_BS)
			{
				/* mid-stream flow control frame */
				uint8_t fc[8] = {0};
				fc[0] = 0x30U;
				fc[1] = BL_CAN_FC_BS;
				fc[2] = BL_CAN_FC_STMIN_MS;
				(void)can_send_raw(BL_CAN_RSP_ID, fc);
				rx.cf_since_fc = 0U;
			}
			if(rx.received >= rx.expected)
			{
				can_rx_dispatch();
			}
			break;
		}

		default:
			/* FC frames (0x3x) are consumed by the TX side, unknown PCI dropped */
			break;
	}
}

/************************************    TX segmentation    ************************************/
/* Wait for the receiver's FC frame. In loopback-test mode it also pumps the
 * UART tunnel so the host's FC (injected as a UART packet) reaches the CAN
 * FIFO while we block here. Returns 1 with bs/stmin filled, 0 on timeout. */
static uint8_t can_tx_wait_fc(uint8_t *bs, uint8_t *stmin)
{
	CAN_RxHeaderTypeDef hdr;
	uint8_t d[8];
	uint32_t t0 = HAL_GetTick();

	while((HAL_GetTick() - t0) < BL_CAN_FC_TIMEOUT_MS)
	{
		BL_IWDG_Refresh();
		while(HAL_CAN_GetRxFifoFillLevel(&hcan1, CAN_RX_FIFO0) > 0U)
		{
			if(HAL_CAN_GetRxMessage(&hcan1, CAN_RX_FIFO0, &hdr, d) != HAL_OK)
			{
				break;
			}
			if(hdr.IDE != CAN_ID_STD)
			{
				continue;
			}
			if(hdr.StdId == BL_CAN_RSP_ID)
			{
#if BL_CAN_LOOPBACK_TEST
				can_forward_to_uart(d);
#endif
				continue;
			}
			if((d[0] >> 4) == 0x3U)
			{
				if(d[0] == 0x30U)
				{
					*bs = d[1];
					*stmin = (d[2] > 0xF0U) ? 1U : d[2];  /* STmin >0xF0 = reserved */
					return 1U;
				}
				return 0U;   /* OVFLW / abort */
			}
		}
#if BL_CAN_LOOPBACK_TEST
		can_pump_uart();
#endif
	}
	return 0U;
}

uint8_t BL_CAN_Send_Packet(const uint8_t *data, uint16_t len)
{
	uint8_t f[8] = {0};
	uint16_t off;
	uint8_t sn = 0U;
	uint8_t since_fc = 0U;
	uint8_t bs = BL_CAN_FC_BS;
	uint8_t stmin = BL_CAN_FC_STMIN_MS;

	if(len <= 7U)
	{
		f[0] = (uint8_t)(0x00U | len);
		memcpy(&f[1], data, len);
		return can_send_raw(BL_CAN_RSP_ID, f);
	}

	/* FF */
	f[0] = (uint8_t)(0x10U | (len >> 8));
	f[1] = (uint8_t)(len & 0xFFU);
	memcpy(&f[2], data, 6U);
	if(can_send_raw(BL_CAN_RSP_ID, f) == 0U)
	{
		return 0U;
	}
	if(can_tx_wait_fc(&bs, &stmin) == 0U)
	{
		return 0U;
	}

	/* CFs */
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
			if(can_tx_wait_fc(&bs, &stmin) == 0U)
			{
				return 0U;
			}
			since_fc = 0U;
		}
		sn = (sn + 1U) & 0x0FU;
		f[0] = (uint8_t)(0x20U | sn);
		memcpy(&f[1], &data[off], n);
		if(can_send_raw(BL_CAN_RSP_ID, f) == 0U)
		{
			return 0U;
		}
		off += n;
		since_fc++;
		if(stmin != 0U)
		{
			HAL_Delay(stmin);
		}
	}
	return 1U;
}

/************************************    Service / init    ************************************/
void BL_CAN_Service(void)
{
	CAN_RxHeaderTypeDef hdr;
	uint8_t d[8];

	/* RX reassembly timeout: a stalled multi-frame must not wedge the FSM */
	if(rx.active && ((HAL_GetTick() - rx.last_frame_tick) > BL_CAN_CF_TIMEOUT_MS))
	{
		can_rx_reset();
	}

	while(HAL_CAN_GetRxFifoFillLevel(&hcan1, CAN_RX_FIFO0) > 0U)
	{
		BL_IWDG_Refresh();
		if(HAL_CAN_GetRxMessage(&hcan1, CAN_RX_FIFO0, &hdr, d) != HAL_OK)
		{
			break;
		}
		if(hdr.IDE != CAN_ID_STD)
		{
			continue;
		}
		if(hdr.StdId == BL_CAN_RSP_ID)
		{
			/* our own TX looped back (C1) - forward to the host tunnel */
#if BL_CAN_LOOPBACK_TEST
			can_forward_to_uart(d);
#endif
			continue;
		}
		if(hdr.StdId == BL_CAN_REQ_ID)
		{
			can_rx_feed(d);
		}
	}
}

uint8_t BL_CAN_Activity(void)
{
	return (HAL_CAN_GetRxFifoFillLevel(&hcan1, CAN_RX_FIFO0) > 0U) ? 1U : 0U;
}

uint8_t BL_CAN_Inject_And_Service(const uint8_t *frame8)
{
	if(can_send_raw(BL_CAN_REQ_ID, frame8) == 0U)
	{
		return 0U;
	}
	BL_CAN_Service();
	return 1U;
}

void BL_CAN_Init(void)
{
	GPIO_InitTypeDef gpio = {0};
	CAN_FilterTypeDef filt = {0};

	/* PB8 = CAN1_RX, PB9 = CAN1_TX (AF9). Chosen to keep PA11/PA12 (USB)
	 * free - see the project plan. */
	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_CAN1_CLK_ENABLE();
	gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Pull = GPIO_PULLUP;
	gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	gpio.Alternate = GPIO_AF9_CAN1;
	HAL_GPIO_Init(GPIOB, &gpio);

	hcan1.Instance = CAN1;
	hcan1.Init.Prescaler = 2U;                 /* 16 MHz APB1 -> 8 MHz CAN clk */
	hcan1.Init.Mode = BL_CAN_LOOPBACK_TEST ? CAN_MODE_LOOPBACK : CAN_MODE_NORMAL;
	hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
	hcan1.Init.TimeSeg1 = CAN_BS1_13TQ;        /* 1+13+2 = 16 tq -> 500 kbps, SP 87.5% */
	hcan1.Init.TimeSeg2 = CAN_BS2_2TQ;
	hcan1.Init.TimeTriggeredMode = DISABLE;
	hcan1.Init.AutoBusOff = ENABLE;
	hcan1.Init.AutoWakeUp = DISABLE;
	hcan1.Init.AutoRetransmission = BL_CAN_LOOPBACK_TEST ? DISABLE : ENABLE;
	hcan1.Init.ReceiveFifoLocked = DISABLE;
	hcan1.Init.TransmitFifoPriority = DISABLE;
	if(HAL_CAN_Init(&hcan1) != HAL_OK)
	{
		return;
	}

	/* 16-bit ID list: accept exactly 0x6E0 (requests) and 0x6E1 (own TX echo
	 * in loopback / bus monitoring in C2) into FIFO0. */
	filt.FilterBank = 0;
	filt.FilterMode = CAN_FILTERMODE_IDLIST;
	filt.FilterScale = CAN_FILTERSCALE_16BIT;
	filt.FilterIdHigh = (uint16_t)(BL_CAN_REQ_ID << 5);
	filt.FilterIdLow = (uint16_t)(BL_CAN_RSP_ID << 5);
	filt.FilterMaskIdHigh = 0x0000U;
	filt.FilterMaskIdLow = 0x0000U;
	filt.FilterFIFOAssignment = CAN_RX_FIFO0;
	filt.FilterActivation = ENABLE;
	(void)HAL_CAN_ConfigFilter(&hcan1, &filt);
	(void)HAL_CAN_Start(&hcan1);

	can_rx_reset();
}
