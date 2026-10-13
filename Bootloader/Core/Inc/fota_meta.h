/*
 * fota_meta.h - shared A/B update metadata (identical copy in App/Core/Inc)
 *
 * STM32F407VE memory map:
 *   0x08000000  Bootloader (32 KB, sectors 0-1)
 *   0x08008000  Metadata    (16 KB, sector 2)  <-- this file's domain
 *   0x0800C000  Slot A      (208 KB, sectors 3-5)
 *   0x08040000  Slot B      (256 KB, sectors 6-7)
 *
 * The on-flash struct layout MUST stay byte-identical with:
 *   - Host/fota_host.py   (struct.pack("<IBBBBIIII") + crc over first 20 bytes)
 *   - App/Core/Inc/fota_meta.h (same copy)
 *
 * v2 (2026-10-13): added min_sv, the anti-rollback floor. It is raised at
 * every successful ACTIVATE (min_sv = image security_version) and never
 * lowered - rolling back an unconfirmed TRIAL slot does NOT touch it. A v1
 * record on flash reads as invalid and falls back to defaults (min_sv = 0);
 * the slots themselves stay bootable through the vector-table trust path.
 */

#ifndef FOTA_META_H_
#define FOTA_META_H_

#include <stdint.h>

#define META_BASE_ADDRESS        0x08008000U
#define META_SECTOR_NUMBER       2U

#define META_MAGIC               0x464F5441U   /* "FOTA" */
#define META_VERSION             2U

#define META_STATE_EMPTY         0xFFU         /* sector blank / no metadata */
#define META_STATE_IDLE          0x01U         /* active slot is confirmed good */
#define META_STATE_TRIAL         0x02U         /* active slot awaits app confirmation */

#define META_SLOT_A              0U
#define META_SLOT_B              1U

/* meta_crc covers the first META_CRC_COVERED_LEN bytes of the record. */
#define META_CRC_COVERED_LEN     20U

typedef struct
{
    uint32_t magic;        /* META_MAGIC, written last -> torn writes invalidate it */
    uint8_t  version;
    uint8_t  active_slot;  /* slot to boot: META_SLOT_A / META_SLOT_B */
    uint8_t  state;        /* META_STATE_* */
    uint8_t  reserved;
    uint32_t image_len;    /* length of the image in the active slot (bytes) */
    uint32_t image_crc;    /* FOTA CRC32 of that image */
    uint32_t min_sv;       /* anti-rollback floor: highest activated security_version */
    uint32_t meta_crc;     /* FOTA CRC32 of the META_CRC_COVERED_LEN bytes above */
} FOTA_Meta_t;

/* Layout check: 4+1+1+1+1+4+4+4+4+4 = 24 bytes, no padding on ARM */
typedef char FOTA_Meta_Size_Check[(sizeof(FOTA_Meta_t) == 24U) ? 1 : -1];

#endif /* FOTA_META_H_ */
