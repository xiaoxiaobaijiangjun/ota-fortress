/*****************************************************************************************************************************
 * image_tlv.c - bounds-checked byte-wise parser for the signed-package trailer
 *****************************************************************************************************************************/

#include "image_tlv.h"

/* Read a little-endian 16-bit value at p[0..1] (any alignment). */
static uint16_t tlv_rd16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint8_t Image_TLV_Parse(uint32_t start, uint32_t max_len, Image_TLV_Info_t *info)
{
	const uint8_t *p = (const uint8_t *)start;
	uint32_t region_end;
	uint32_t pos;
	uint16_t tlv_len;
	uint16_t type;
	uint16_t len;
	uint8_t have_digest = 0;
	uint8_t have_sig = 0;
	uint8_t have_sv = 0;

	info->security_version = 0;
	info->digest = 0;
	info->signature = 0;

	/* Header: 4B magic + 2B entry-area length. The trailer ends at
	 * 6 + tlv_len; the rest of the region up to the slot end is erased
	 * flash (0xFF) and must not be parsed as entries. */
	if(max_len < 6)
	{
		return 0;
	}
	if((p[0] != IMAGE_TLV_MAGIC_0) || (p[1] != IMAGE_TLV_MAGIC_1) ||
	   (p[2] != IMAGE_TLV_MAGIC_2) || (p[3] != IMAGE_TLV_MAGIC_3))
	{
		return 0;
	}
	tlv_len = tlv_rd16(&p[4]);
	if((6U + (uint32_t)tlv_len) > max_len)
	{
		return 0;
	}
	region_end = 6U + (uint32_t)tlv_len;
	pos = 6;

	while(pos < region_end)
	{
		const uint8_t *value;

		/* Entry header: 2B type + 2B len */
		if((pos + 4) > region_end)
		{
			return 0;
		}
		type = tlv_rd16(&p[pos]);
		len = tlv_rd16(&p[pos + 2]);
		pos += 4;

		if((pos + len) > region_end)
		{
			return 0;
		}
		value = &p[pos];
		pos += len;

		switch(type)
		{
			case IMAGE_TLV_TYPE_SHA256:
				if((len != IMAGE_TLV_DIGEST_LEN) || have_digest)
				{
					return 0;
				}
				info->digest = value;
				have_digest = 1;
				break;

			case IMAGE_TLV_TYPE_ECDSA_P256:
				if((len != IMAGE_TLV_SIG_LEN) || have_sig)
				{
					return 0;
				}
				info->signature = value;
				have_sig = 1;
				break;

			case IMAGE_TLV_TYPE_SEC_VERSION:
				if((len != IMAGE_TLV_SV_LEN) || have_sv)
				{
					return 0;
				}
				info->security_version = (uint32_t)value[0] |
				                         ((uint32_t)value[1] << 8) |
				                         ((uint32_t)value[2] << 16) |
				                         ((uint32_t)value[3] << 24);
				have_sv = 1;
				break;

			default:
				/* Unknown entry type: the package is not what this bootloader
				 * expects. Reject instead of skip - forward compatibility is
				 * not worth an ambiguous verification surface. */
				return 0;
		}
	}

	/* The entries must exactly fill the declared entry area. */
	if(pos != region_end)
	{
		return 0;
	}

	/* All three entries are mandatory for a signed package. */
	if(!have_digest || !have_sig || !have_sv)
	{
		return 0;
	}

	return 1;
}
