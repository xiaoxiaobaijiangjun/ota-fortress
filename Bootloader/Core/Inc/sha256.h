/*****************************************************************************************************************************
 * sha256.h - software SHA-256 (FIPS 180-4) for image digest verification
 *
 * The F407 hash peripheral is not used on purpose: the bootloader runs on the
 * default HSI clock with minimal peripheral init, and a portable software
 * implementation keeps the boot path independent of RCC state.
 *****************************************************************************************************************************/

#ifndef SHA256_H_
#define SHA256_H_

#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST_SIZE  32U

typedef struct
{
	uint32_t state[8];
	uint64_t bitlen;
	uint8_t  data[64];
	uint32_t datalen;
} SHA256_CTX;

void sha256_init(SHA256_CTX *ctx);
void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len);
void sha256_final(SHA256_CTX *ctx, uint8_t hash[SHA256_DIGEST_SIZE]);

#endif /* SHA256_H_ */
