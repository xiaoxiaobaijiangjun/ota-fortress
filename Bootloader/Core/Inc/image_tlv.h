/*****************************************************************************************************************************
 * image_tlv.h - trailer TLV format appended to signed firmware packages
 *
 * A signed package uploaded to a slot is:  [image N bytes][TLV trailer]
 * The ACTIVATE command carries image_len = N (the image only), so the CRC and
 * the boot-time integrity check cover the image; the TLV trailer carries the
 * authentication data:
 *
 *   [magic 'F','T','L','V' 4B][tlv_len LE16][entries... tlv_len bytes]
 *   entry: [type LE16][len LE16][value ...]
 *
 * tlv_len is the byte length of the entry area: on flash the trailer is
 * followed by erased 0xFF up to the slot end, so the trailer must carry its
 * own length instead of relying on an end-of-file boundary.
 *
 * Mandatory entries (exact lengths, duplicates rejected):
 *   0x0001 SHA-256 digest, 32B  : SHA256(image || security_version_le32)
 *   0x0002 ECDSA-P256 sig, 64B : r||s (32B each, big-endian) over the digest
 *   0x0003 security_version, 4B: LE32 anti-rollback counter (feature B)
 *
 * The parser is byte-wise and bounds-checked: the trailer may start at any
 * alignment inside the slot.
 *****************************************************************************************************************************/

#ifndef IMAGE_TLV_H_
#define IMAGE_TLV_H_

#include <stdint.h>

#define IMAGE_TLV_MAGIC_0            'F'
#define IMAGE_TLV_MAGIC_1            'T'
#define IMAGE_TLV_MAGIC_2            'L'
#define IMAGE_TLV_MAGIC_3            'V'

#define IMAGE_TLV_TYPE_SHA256        0x0001U
#define IMAGE_TLV_TYPE_ECDSA_P256    0x0002U
#define IMAGE_TLV_TYPE_SEC_VERSION   0x0003U

#define IMAGE_TLV_DIGEST_LEN         32U
#define IMAGE_TLV_SIG_LEN            64U
#define IMAGE_TLV_SV_LEN             4U

/* Total trailer size when all three mandatory entries are present:
 * 4 (magic) + 2 (tlv_len) + 3 * (2 + 2) + 32 + 64 + 4 = 118 bytes. */
#define IMAGE_TLV_FIXED_LEN          118U

typedef struct
{
	uint32_t security_version;      /* 0 when the entry is absent */
	const uint8_t *digest;          /* 32 bytes inside the trailer, NULL if absent */
	const uint8_t *signature;       /* 64 bytes inside the trailer, NULL if absent */
} Image_TLV_Info_t;

/* Parse the TLV trailer stored at [start, start + max_len). Returns 1 when the
 * magic, all bounds checks and the mandatory-entry rules pass. */
uint8_t Image_TLV_Parse(uint32_t start, uint32_t max_len, Image_TLV_Info_t *info);

#endif /* IMAGE_TLV_H_ */
