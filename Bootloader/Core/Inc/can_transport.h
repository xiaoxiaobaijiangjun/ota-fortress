/*****************************************************************************************************************************
 * can_transport.h - CAN flashing transport (simplified ISO-TP over bxCAN)
 *
 * Architecture: "swap the physical layer, keep the protocol layer" - the 8
 * update commands, the 64-byte chunking and the frame CRC32 are IDENTICAL to
 * the UART transport; only the frame carrier changes (CAN classic frames).
 *
 *   request  (host   -> device) : CAN id 0x6E0
 *   response (device -> host  ) : CAN id 0x6E1
 *
 * Segmentation (simplified ISO-TP, mirrors tools/isotp.py):
 *   SF : [0x0L][<=7 bytes]
 *   FF : [0x1L][length LO][first 6 bytes]   (12-bit length)
 *   CF : [0x2N][7 bytes]                    (N = sequence, mod 16)
 *   FC : [0x30][BS][STmin]                  (receiver -> sender, BS=8, STmin=1ms)
 *
 * C1 (loopback, zero hardware): BL_CAN_LOOPBACK_TEST=1
 *   - bxCAN in internal loopback mode, no transceiver needed
 *   - test harness tunnel over the EXISTING UART:
 *       host -> MCU : [13][0x1C][8-byte CAN frame][crc32]  (inject, no reply)
 *       MCU  -> host: [13][0x1D][8-byte CAN frame][crc32]  (forward own TX)
 *     so the full CAN driver + ISO-TP stack runs on the real MCU with only
 *     the USB-TTL wired. C2 (real bus) sets BL_CAN_LOOPBACK_TEST=0.
 *
 * Bit timing (HSI 16 MHz, APB1 = 16 MHz, no PLL in the bootloader):
 *   prescaler=2 -> 8 MHz, 1+13+2 = 16 tq -> 500 kbps, sample point 87.5 %.
 *****************************************************************************************************************************/

#ifndef CAN_TRANSPORT_H_
#define CAN_TRANSPORT_H_

#include <stdint.h>

#define BL_CAN_REQ_ID               0x6E0U
#define BL_CAN_RSP_ID               0x6E1U

/* C1 loopback test harness; C2 real-bus default. Flip to 1U only to rerun the
 * on-device loopback harness (C1 evidence is archived under 02-实验记录). */
#define BL_CAN_LOOPBACK_TEST        0U

/* UART tunnel commands (only exist in loopback-test builds) */
#define CBL_CAN_INJECT_CMD          0x1C  /* host -> MCU: inject one CAN frame   */
#define CBL_CAN_FWD_CMD             0x1D  /* MCU -> host: forwarded CAN frame    */

/* BL_CAN_Service quiet window in DWT cycles (~4 ms at the 16 MHz HSI boot
 * clock): how long the CAN FIFO must stay empty after the last drained
 * frame before servicing returns. Cycle-counted because HAL_GetTick loses
 * seconds across flash-erase stalls. */
#define BL_CAN_QUIET_CYCLES         64000U

void    BL_CAN_Init(void);
void    BL_CAN_Service(void);
uint8_t BL_CAN_Activity(void);
uint8_t BL_CAN_Send_Packet(const uint8_t *data, uint16_t len);
uint8_t BL_CAN_Inject_And_Service(const uint8_t *frame8);

#endif /* CAN_TRANSPORT_H_ */
