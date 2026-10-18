/*****************************************************************************************************************************
 * bridge.h - the PC-to-CAN pump.
 *
 * Pure pipe on purpose: the PC sees the exact byte stream it would see on a
 * direct USB-TTL link, so fota_host.py runs UNCHANGED (only --port differs).
 *  - requests arrive LEN-framed [LEN][cmd][args][crc32] and go out as ISO-TP
 *    PDUs on 0x6E0; reassembled replies are written back verbatim.
 *  - the 0xAA reboot magic (a lone byte the host sends to a RUNNING app) is
 *    swallowed: over CAN only the bootloader listens, entry is the NRST button.
 *  - idle keep-alive: SF with length 0 is silently dropped by the F407's
 *    reassembler but still counts as "CAN activity", which extends the
 *    bootloader's 1 s autoboot window - kills the reset-timing race.
 *  - local stats command 0x5A (never sent by fota_host) returns counters as
 *    one ASCII line, used for the C2 acceptance evidence (frames/errors).
 *****************************************************************************************************************************/

#ifndef BRIDGE_H_
#define BRIDGE_H_

#include <stdint.h>

void bridge_init(void);
void bridge_service(void);       /* call from the main loop */
uint8_t bridge_busy(void);       /* 1 = a request/response exchange is in flight */
void bridge_keepalive(void);     /* one length-0 SF; call when idle */
uint8_t bridge_link_ok(void);    /* 1 = recent CAN TX got an ACK (partner alive) */

#endif /* BRIDGE_H_ */
