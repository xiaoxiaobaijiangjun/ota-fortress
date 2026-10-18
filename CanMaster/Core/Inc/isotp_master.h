/*****************************************************************************************************************************
 * isotp_master.h - simplified ISO-TP sender/receiver, wire-compatible with the
 * F407 side (Bootloader/Core/Src/can_transport.c, mirrors tools/isotp.py):
 *   SF [0x0L][<=7 bytes]   FF [0x1L][len LO][6 bytes]   CF [0x2N][7 bytes]
 *   FC [0x30][BS][STmin]   - our FC advertises BS=8, STmin=1 ms (as the slave does)
 * Request PDUs go out on 0x6E0, replies come back on 0x6E1.
 *****************************************************************************************************************************/

#ifndef ISOTP_MASTER_H_
#define ISOTP_MASTER_H_

#include <stdint.h>

#define ISOTP_MAX   118U   /* 1 len byte + 75-byte upload command + margin */

void    isotp_init(void);
uint8_t isotp_send_pdu(const uint8_t *data, uint16_t len);         /* 1 = delivered */
int16_t isotp_recv_pdu(uint8_t *buf, uint32_t timeout_ms);          /* >0 len, 0 timeout */

#endif /* ISOTP_MASTER_H_ */
