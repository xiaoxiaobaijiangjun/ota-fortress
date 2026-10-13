/*
 * fota_meta.h - shared A/B update metadata (identical copy in Bootloader/Core/Inc)
 *
 * STM32F407VE memory map:
 *   0x08000000  Bootloader (32 KB, sectors 0-1)
 *   0x08008000  Metadata    (16 KB, sector 2)  <-- this file's domain
 *   0x0800C000  Slot A      (208 KB, sectors 3-5)
 *   0x08040000  Slot B      (256 KB, sectors 6-7)
 *
 * The on-flash struct layout MUST stay byte-identical with:
 *   - Host/fota_host.py   (struct.pack("<IBBBBIII"))
 *   - Bootloader/Core/Inc/fota_meta.h (same copy)
 */

#ifndef FOTA_META_H_
#define FOTA_META_H_

#include <stdint.h>

#define META_BASE_ADDRESS        0x08008000U
#define META_SECTOR_NUMBER       2U

#define META_MAGIC               0x464F5441U   /* "FOTA" */
#define META_VERSION             1U

#define META_STATE_EMPTY         0xFFU         /* sector blank / no metadata */
#define META_STATE_IDLE          0x01U         /* active slot is confirmed good */
#define META_STATE_TRIAL         0x02U         /* active slot awaits app confirmation */

#define META_SLOT_A              0U
#define META_SLOT_B              1U

typedef struct
{
    uint32_t magic;        /* META_MAGIC, written last -> torn writes invalidate it */
    uint8_t  version;
    uint8_t  active_slot;  /* slot to boot: META_SLOT_A / META_SLOT_B */
    uint8_t  state;        /* META_STATE_* */
    uint8_t  reserved;
    uint32_t image_len;    /* length of the image in the active slot (bytes) */
    uint32_t image_crc;    /* FOTA CRC32 of that image */
    uint32_t meta_crc;     /* FOTA CRC32 of the 16 bytes above */
} FOTA_Meta_t;

/* Layout check: 4+1+1+1+1+4+4+4 = 20 bytes, no padding on ARM */
typedef char FOTA_Meta_Size_Check[(sizeof(FOTA_Meta_t) == 20U) ? 1 : -1];

#endif /* FOTA_META_H_ */
