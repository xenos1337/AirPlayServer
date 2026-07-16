#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pinpair.h"
#include "crypto/crypto.h"
#include "ed25519/sha512.h"

#pragma comment(lib, "bcrypt.lib")

#define SRP_GROUP_BYTES 256
#define SRP_SHA1_BYTES 20
#define SRP_SESSION_KEY_BYTES 40
#define SRP_PRIVATE_BYTES 32
#define PINPAIR_USERNAME_BYTES 64

/* RFC 5054's 2048-bit SRP group, used by Apple's pair-setup-pin protocol. */
static const char kSrpPrimeHex[] =
	"AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
	"A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
	"95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
	"747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
	"8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
	"60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
	"FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73";

struct pin_pairing_s {
	char username[PINPAIR_USERNAME_BYTES];
	uint8_t salt[PINPAIR_SALT_SIZE];
	uint8_t verifier[SRP_GROUP_BYTES];
	uint8_t private_key[SRP_PRIVATE_BYTES];
	uint8_t public_key[SRP_GROUP_BYTES];
	uint8_t session_key[SRP_SESSION_KEY_BYTES];
	int verified;
};

static int
hex_nibble(char value)
{
	if (value >= '0' && value <= '9') return value - '0';
	if (value >= 'a' && value <= 'f') return value - 'a' + 10;
	if (value >= 'A' && value <= 'F') return value - 'A' + 10;
	return -1;
}

static BI_CTX *
srp_context_create(void)
{
	uint8_t prime[SRP_GROUP_BYTES];
	BI_CTX *ctx = bi_initialize();
	bigint *n;
	int i;

	if (ctx == NULL) return NULL;
	for (i = 0; i < SRP_GROUP_BYTES; ++i) {
		int high = hex_nibble(kSrpPrimeHex[i * 2]);
		int low = hex_nibble(kSrpPrimeHex[i * 2 + 1]);
		if (high < 0 || low < 0) {
			bi_terminate(ctx);
			return NULL;
		}
		prime[i] = (uint8_t)((high << 4) | low);
	}
	n = bi_import(ctx, prime, sizeof(prime));
	SecureZeroMemory(prime, sizeof(prime));
	if (n == NULL) {
		bi_terminate(ctx);
		return NULL;
	}
	bi_set_mod(ctx, n, BIGINT_M_OFFSET);
	ctx->mod_offset = BIGINT_M_OFFSET;
	return ctx;
}

static void
srp_context_destroy(BI_CTX *ctx)
{
	if (ctx != NULL) {
		bi_free_mod(ctx, BIGINT_M_OFFSET);
		bi_terminate(ctx);
	}
}

static int
srp_modexp(const uint8_t *base, int base_len, const uint8_t *exponent,
	int exponent_len, uint8_t output[SRP_GROUP_BYTES])
{
	BI_CTX *ctx;
	bigint *base_number;
	bigint *exponent_number;
	bigint *result;

	if (base == NULL || exponent == NULL || output == NULL ||
		base_len <= 0 || exponent_len <= 0) {
		return -1;
	}
	ctx = srp_context_create();
	if (ctx == NULL) return -1;
	base_number = bi_import(ctx, base, base_len);
	exponent_number = bi_import(ctx, exponent, exponent_len);
	if (base_number == NULL || exponent_number == NULL) {
		if (base_number != NULL) bi_free(ctx, base_number);
		if (exponent_number != NULL) bi_free(ctx, exponent_number);
		srp_context_destroy(ctx);
		return -1;
	}
	result = bi_mod_power(ctx, base_number, exponent_number);
	if (result == NULL) {
		srp_context_destroy(ctx);
		return -1;
	}
	bi_export(ctx, result, output, SRP_GROUP_BYTES);
	srp_context_destroy(ctx);
	return 0;
}

static int
srp_multiply(const uint8_t *left, int left_len, const uint8_t *right,
	int right_len, uint8_t *output, int output_len)
{
	BI_CTX *ctx;
	bigint *left_number;
	bigint *right_number;
	bigint *result;

	if (left == NULL || right == NULL || output == NULL || left_len <= 0 ||
		right_len <= 0 || output_len <= 0) return -1;
	ctx = bi_initialize();
	if (ctx == NULL) return -1;
	left_number = bi_import(ctx, left, left_len);
	right_number = bi_import(ctx, right, right_len);
	if (left_number == NULL || right_number == NULL) {
		if (left_number != NULL) bi_free(ctx, left_number);
		if (right_number != NULL) bi_free(ctx, right_number);
		bi_terminate(ctx);
		return -1;
	}
	result = bi_multiply(ctx, left_number, right_number);
	if (result == NULL) {
		bi_terminate(ctx);
		return -1;
	}
	bi_export(ctx, result, output, output_len);
	bi_terminate(ctx);
	return 0;
}

static int
srp_add(const uint8_t *left, int left_len, const uint8_t *right,
	int right_len, uint8_t *output, int output_len)
{
	BI_CTX *ctx;
	bigint *left_number;
	bigint *right_number;
	bigint *result;

	if (left == NULL || right == NULL || output == NULL || left_len <= 0 ||
		right_len <= 0 || output_len <= 0) return -1;
	ctx = bi_initialize();
	if (ctx == NULL) return -1;
	left_number = bi_import(ctx, left, left_len);
	right_number = bi_import(ctx, right, right_len);
	if (left_number == NULL || right_number == NULL) {
		if (left_number != NULL) bi_free(ctx, left_number);
		if (right_number != NULL) bi_free(ctx, right_number);
		bi_terminate(ctx);
		return -1;
	}
	result = bi_add(ctx, left_number, right_number);
	if (result == NULL) {
		bi_terminate(ctx);
		return -1;
	}
	bi_export(ctx, result, output, output_len);
	bi_terminate(ctx);
	return 0;
}

static int
srp_reduce(const uint8_t *input, int input_len, uint8_t output[SRP_GROUP_BYTES])
{
	static const uint8_t one[] = { 1 };
	return srp_modexp(input, input_len, one, sizeof(one), output);
}

static const uint8_t *
trim_number(const uint8_t *value, int *length)
{
	while (*length > 1 && *value == 0) {
		++value;
		--*length;
	}
	return value;
}

static void
sha1_hash(const uint8_t *data, int data_len, uint8_t result[SRP_SHA1_BYTES])
{
	SHA1_CTX ctx;
	SHA1_Init(&ctx);
	if (data_len > 0) SHA1_Update(&ctx, data, data_len);
	SHA1_Final(result, &ctx);
}

static void
sha1_hash_username(const char *username, uint8_t result[SRP_SHA1_BYTES])
{
	sha1_hash((const uint8_t *)username, (int)strlen(username), result);
}

static void
sha1_update_number(SHA1_CTX *ctx, const uint8_t *value, int value_len)
{
	const uint8_t *trimmed = trim_number(value, &value_len);
	SHA1_Update(ctx, trimmed, value_len);
}

static void
srp_compute_x(const char *username, const char *pin,
	const uint8_t salt[PINPAIR_SALT_SIZE], uint8_t x[SRP_SHA1_BYTES])
{
	uint8_t inner[SRP_SHA1_BYTES];
	SHA1_CTX ctx;

	SHA1_Init(&ctx);
	SHA1_Update(&ctx, (const uint8_t *)username, (int)strlen(username));
	SHA1_Update(&ctx, (const uint8_t *)":", 1);
	SHA1_Update(&ctx, (const uint8_t *)pin, (int)strlen(pin));
	SHA1_Final(inner, &ctx);

	SHA1_Init(&ctx);
	SHA1_Update(&ctx, salt, PINPAIR_SALT_SIZE);
	SHA1_Update(&ctx, inner, sizeof(inner));
	SHA1_Final(x, &ctx);
	SecureZeroMemory(inner, sizeof(inner));
}

static void
srp_compute_k(uint8_t k[SRP_SHA1_BYTES])
{
	uint8_t buffer[SRP_GROUP_BYTES * 2];
	int i;

	for (i = 0; i < SRP_GROUP_BYTES; ++i) {
		int high = hex_nibble(kSrpPrimeHex[i * 2]);
		int low = hex_nibble(kSrpPrimeHex[i * 2 + 1]);
		buffer[i] = (uint8_t)((high << 4) | low);
	}
	memset(buffer + SRP_GROUP_BYTES, 0, SRP_GROUP_BYTES);
	buffer[SRP_GROUP_BYTES * 2 - 1] = 2;
	sha1_hash(buffer, sizeof(buffer), k);
	SecureZeroMemory(buffer, sizeof(buffer));
}

static void
srp_compute_u(const uint8_t client_public_key[SRP_GROUP_BYTES],
	const uint8_t server_public_key[SRP_GROUP_BYTES], uint8_t u[SRP_SHA1_BYTES])
{
	uint8_t buffer[SRP_GROUP_BYTES * 2];
	memcpy(buffer, client_public_key, SRP_GROUP_BYTES);
	memcpy(buffer + SRP_GROUP_BYTES, server_public_key, SRP_GROUP_BYTES);
	sha1_hash(buffer, sizeof(buffer), u);
	SecureZeroMemory(buffer, sizeof(buffer));
}

static void
srp_compute_session_key(const uint8_t secret[SRP_GROUP_BYTES],
	uint8_t session_key[SRP_SESSION_KEY_BYTES])
{
	uint8_t counter[4] = { 0, 0, 0, 0 };
	const uint8_t *trimmed;
	int secret_len = SRP_GROUP_BYTES;
	SHA1_CTX ctx;

	trimmed = trim_number(secret, &secret_len);
	SHA1_Init(&ctx);
	SHA1_Update(&ctx, trimmed, secret_len);
	SHA1_Update(&ctx, counter, sizeof(counter));
	SHA1_Final(session_key, &ctx);
	counter[3] = 1;
	SHA1_Init(&ctx);
	SHA1_Update(&ctx, trimmed, secret_len);
	SHA1_Update(&ctx, counter, sizeof(counter));
	SHA1_Final(session_key + SRP_SHA1_BYTES, &ctx);
}

static void
srp_compute_client_proof(const char *username,
	const uint8_t salt[PINPAIR_SALT_SIZE],
	const uint8_t client_public_key[SRP_GROUP_BYTES],
	const uint8_t server_public_key[SRP_GROUP_BYTES],
	const uint8_t session_key[SRP_SESSION_KEY_BYTES],
	uint8_t proof[SRP_SHA1_BYTES])
{
	uint8_t n[SRP_GROUP_BYTES];
	uint8_t hash_n[SRP_SHA1_BYTES];
	uint8_t hash_g[SRP_SHA1_BYTES];
	uint8_t hash_user[SRP_SHA1_BYTES];
	uint8_t xor_hash[SRP_SHA1_BYTES];
	uint8_t generator = 2;
	SHA1_CTX ctx;
	int i;

	for (i = 0; i < SRP_GROUP_BYTES; ++i) {
		int high = hex_nibble(kSrpPrimeHex[i * 2]);
		int low = hex_nibble(kSrpPrimeHex[i * 2 + 1]);
		n[i] = (uint8_t)((high << 4) | low);
	}
	sha1_hash(n, sizeof(n), hash_n);
	sha1_hash(&generator, sizeof(generator), hash_g);
	sha1_hash_username(username, hash_user);
	for (i = 0; i < SRP_SHA1_BYTES; ++i) {
		xor_hash[i] = hash_n[i] ^ hash_g[i];
	}

	SHA1_Init(&ctx);
	SHA1_Update(&ctx, xor_hash, sizeof(xor_hash));
	SHA1_Update(&ctx, hash_user, sizeof(hash_user));
	sha1_update_number(&ctx, salt, PINPAIR_SALT_SIZE);
	sha1_update_number(&ctx, client_public_key, SRP_GROUP_BYTES);
	sha1_update_number(&ctx, server_public_key, SRP_GROUP_BYTES);
	SHA1_Update(&ctx, session_key, SRP_SESSION_KEY_BYTES);
	SHA1_Final(proof, &ctx);
	SecureZeroMemory(n, sizeof(n));
	SecureZeroMemory(hash_n, sizeof(hash_n));
	SecureZeroMemory(hash_g, sizeof(hash_g));
	SecureZeroMemory(hash_user, sizeof(hash_user));
	SecureZeroMemory(xor_hash, sizeof(xor_hash));
}

static void
srp_compute_server_proof(const uint8_t client_public_key[SRP_GROUP_BYTES],
	const uint8_t client_proof[SRP_SHA1_BYTES],
	const uint8_t session_key[SRP_SESSION_KEY_BYTES],
	uint8_t proof[SRP_SHA1_BYTES])
{
	SHA1_CTX ctx;
	SHA1_Init(&ctx);
	sha1_update_number(&ctx, client_public_key, SRP_GROUP_BYTES);
	SHA1_Update(&ctx, client_proof, SRP_SHA1_BYTES);
	SHA1_Update(&ctx, session_key, SRP_SESSION_KEY_BYTES);
	SHA1_Final(proof, &ctx);
}

static int
secure_equal(const uint8_t *left, const uint8_t *right, int length)
{
	uint8_t difference = 0;
	int i;
	for (i = 0; i < length; ++i) difference |= left[i] ^ right[i];
	return difference == 0;
}

static int
aes_gcm_crypt(int encrypt, const uint8_t key_bytes[16], const uint8_t iv[16],
	const uint8_t input[PINPAIR_ED25519_KEY_SIZE],
	uint8_t output[PINPAIR_ED25519_KEY_SIZE], uint8_t tag[PINPAIR_AUTH_TAG_SIZE])
{
	BCRYPT_ALG_HANDLE algorithm = NULL;
	BCRYPT_KEY_HANDLE key = NULL;
	BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
	PUCHAR key_object = NULL;
	DWORD key_object_size = 0;
	DWORD result_size = 0;
	NTSTATUS status;
	int success = 0;

	status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, NULL, 0);
	if (status != 0) goto cleanup;
	status = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
		(PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
	if (status != 0) goto cleanup;
	status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&key_object_size,
		sizeof(key_object_size), &result_size, 0);
	if (status != 0 || key_object_size == 0) goto cleanup;
	key_object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, key_object_size);
	if (key_object == NULL) goto cleanup;
	status = BCryptGenerateSymmetricKey(algorithm, &key, key_object, key_object_size,
		(PUCHAR)key_bytes, 16, 0);
	if (status != 0) goto cleanup;
	BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
	auth_info.pbNonce = (PUCHAR)iv;
	auth_info.cbNonce = 16;
	auth_info.pbTag = tag;
	auth_info.cbTag = PINPAIR_AUTH_TAG_SIZE;
	if (encrypt) {
		status = BCryptEncrypt(key, (PUCHAR)input, PINPAIR_ED25519_KEY_SIZE,
			&auth_info, NULL, 0, output, PINPAIR_ED25519_KEY_SIZE, &result_size, 0);
	} else {
		status = BCryptDecrypt(key, (PUCHAR)input, PINPAIR_ED25519_KEY_SIZE,
			&auth_info, NULL, 0, output, PINPAIR_ED25519_KEY_SIZE, &result_size, 0);
	}
	success = status == 0 && result_size == PINPAIR_ED25519_KEY_SIZE;

cleanup:
	if (key != NULL) BCryptDestroyKey(key);
	if (key_object != NULL) {
		SecureZeroMemory(key_object, key_object_size);
		HeapFree(GetProcessHeap(), 0, key_object);
	}
	if (algorithm != NULL) BCryptCloseAlgorithmProvider(algorithm, 0);
	return success ? 0 : -1;
}

pin_pairing_t *
pin_pairing_create(const char *username, const char *pin,
	uint8_t salt[PINPAIR_SALT_SIZE], uint8_t public_key[PINPAIR_PUBLIC_KEY_SIZE])
{
	pin_pairing_t *pairing;
	uint8_t x[SRP_SHA1_BYTES];
	uint8_t multiplier[SRP_SHA1_BYTES];
	uint8_t generator[] = { 2 };
	uint8_t multiplied[SRP_GROUP_BYTES * 2];
	uint8_t multiplied_mod[SRP_GROUP_BYTES];
	uint8_t generator_power[SRP_GROUP_BYTES];
	uint8_t sum[SRP_GROUP_BYTES * 2];

	if (username == NULL || pin == NULL || salt == NULL || public_key == NULL ||
		username[0] == '\0' || pin[0] == '\0' ||
		strlen(username) >= PINPAIR_USERNAME_BYTES) return NULL;
	pairing = (pin_pairing_t *)calloc(1, sizeof(pin_pairing_t));
	if (pairing == NULL) return NULL;
	strncpy(pairing->username, username, sizeof(pairing->username) - 1);
	if (BCryptGenRandom(NULL, pairing->salt, PINPAIR_SALT_SIZE,
		BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0 ||
		BCryptGenRandom(NULL, pairing->private_key, SRP_PRIVATE_BYTES,
			BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
		pin_pairing_destroy(pairing);
		return NULL;
	}
	/* Keep their exported bignum representation at the protocol's fixed size. */
	pairing->salt[0] |= 0x80;
	pairing->private_key[0] |= 0x80;
	srp_compute_x(pairing->username, pin, pairing->salt, x);
	if (srp_modexp(generator, sizeof(generator), x, sizeof(x), pairing->verifier) != 0) {
		SecureZeroMemory(x, sizeof(x));
		pin_pairing_destroy(pairing);
		return NULL;
	}
	srp_compute_k(multiplier);
	if (srp_multiply(multiplier, sizeof(multiplier), pairing->verifier,
		SRP_GROUP_BYTES, multiplied, sizeof(multiplied)) != 0 ||
		srp_reduce(multiplied, sizeof(multiplied), multiplied_mod) != 0 ||
		srp_modexp(generator, sizeof(generator), pairing->private_key,
			SRP_PRIVATE_BYTES, generator_power) != 0 ||
		srp_add(multiplied_mod, sizeof(multiplied_mod), generator_power,
			sizeof(generator_power), sum, sizeof(sum)) != 0 ||
		srp_reduce(sum, sizeof(sum), pairing->public_key) != 0) {
		SecureZeroMemory(x, sizeof(x));
		SecureZeroMemory(multiplier, sizeof(multiplier));
		SecureZeroMemory(multiplied, sizeof(multiplied));
		SecureZeroMemory(multiplied_mod, sizeof(multiplied_mod));
		SecureZeroMemory(generator_power, sizeof(generator_power));
		SecureZeroMemory(sum, sizeof(sum));
		pin_pairing_destroy(pairing);
		return NULL;
	}
	memcpy(salt, pairing->salt, PINPAIR_SALT_SIZE);
	memcpy(public_key, pairing->public_key, PINPAIR_PUBLIC_KEY_SIZE);
	SecureZeroMemory(x, sizeof(x));
	SecureZeroMemory(multiplier, sizeof(multiplier));
	SecureZeroMemory(multiplied, sizeof(multiplied));
	SecureZeroMemory(multiplied_mod, sizeof(multiplied_mod));
	SecureZeroMemory(generator_power, sizeof(generator_power));
	SecureZeroMemory(sum, sizeof(sum));
	return pairing;
}

int
pin_pairing_verify(pin_pairing_t *pairing, const uint8_t *client_public_key,
	int client_public_key_len, const uint8_t *client_proof, int client_proof_len,
	uint8_t server_proof[PINPAIR_PROOF_SIZE])
{
	uint8_t client_public_key_fixed[SRP_GROUP_BYTES];
	uint8_t client_public_key_mod[SRP_GROUP_BYTES];
	uint8_t scrambling[SRP_SHA1_BYTES];
	uint8_t verifier_power[SRP_GROUP_BYTES];
	uint8_t product[SRP_GROUP_BYTES * 2];
	uint8_t base[SRP_GROUP_BYTES];
	uint8_t secret[SRP_GROUP_BYTES];
	uint8_t expected_proof[SRP_SHA1_BYTES];
	int result = -1;

	if (pairing == NULL || client_public_key == NULL || client_proof == NULL ||
		server_proof == NULL || client_public_key_len <= 0 ||
		client_public_key_len > SRP_GROUP_BYTES || client_proof_len != SRP_SHA1_BYTES) {
		return -1;
	}
	memset(client_public_key_fixed, 0, sizeof(client_public_key_fixed));
	memcpy(client_public_key_fixed + SRP_GROUP_BYTES - client_public_key_len,
		client_public_key, client_public_key_len);
	if (srp_reduce(client_public_key_fixed, sizeof(client_public_key_fixed),
		client_public_key_mod) != 0) goto cleanup;
	{
		int index;
		int nonzero = 0;
		for (index = 0; index < SRP_GROUP_BYTES; ++index) nonzero |= client_public_key_mod[index];
		if (!nonzero) goto cleanup;
	}
	srp_compute_u(client_public_key_fixed, pairing->public_key, scrambling);
	if (srp_modexp(pairing->verifier, sizeof(pairing->verifier), scrambling,
		sizeof(scrambling), verifier_power) != 0 ||
		srp_multiply(client_public_key_fixed, sizeof(client_public_key_fixed), verifier_power,
			sizeof(verifier_power), product, sizeof(product)) != 0 ||
		srp_reduce(product, sizeof(product), base) != 0 ||
		srp_modexp(base, sizeof(base), pairing->private_key,
			sizeof(pairing->private_key), secret) != 0) goto cleanup;
	srp_compute_session_key(secret, pairing->session_key);
	srp_compute_client_proof(pairing->username, pairing->salt, client_public_key_fixed,
		pairing->public_key, pairing->session_key, expected_proof);
	if (!secure_equal(expected_proof, client_proof, SRP_SHA1_BYTES)) goto cleanup;
	srp_compute_server_proof(client_public_key_fixed, expected_proof,
		pairing->session_key, server_proof);
	pairing->verified = 1;
	result = 0;

cleanup:
	SecureZeroMemory(client_public_key_fixed, sizeof(client_public_key_fixed));
	SecureZeroMemory(client_public_key_mod, sizeof(client_public_key_mod));
	SecureZeroMemory(scrambling, sizeof(scrambling));
	SecureZeroMemory(verifier_power, sizeof(verifier_power));
	SecureZeroMemory(product, sizeof(product));
	SecureZeroMemory(base, sizeof(base));
	SecureZeroMemory(secret, sizeof(secret));
	SecureZeroMemory(expected_proof, sizeof(expected_proof));
	return result;
}

int
pin_pairing_confirm(pin_pairing_t *pairing,
	const uint8_t client_epk[PINPAIR_ED25519_KEY_SIZE],
	const uint8_t client_auth_tag[PINPAIR_AUTH_TAG_SIZE],
	const uint8_t server_public_key[PINPAIR_ED25519_KEY_SIZE],
	uint8_t server_epk[PINPAIR_ED25519_KEY_SIZE],
	uint8_t server_auth_tag[PINPAIR_AUTH_TAG_SIZE],
	uint8_t client_public_key[PINPAIR_ED25519_KEY_SIZE])
{
	uint8_t hash[64];
	uint8_t key[16];
	uint8_t iv[16];
	sha512_context ctx;
	int result;

	if (pairing == NULL || !pairing->verified || client_epk == NULL ||
		client_auth_tag == NULL || server_public_key == NULL || server_epk == NULL ||
		server_auth_tag == NULL || client_public_key == NULL) return -1;
	sha512_init(&ctx);
	sha512_update(&ctx, (const uint8_t *)"Pair-Setup-AES-Key", 18);
	sha512_update(&ctx, pairing->session_key, sizeof(pairing->session_key));
	sha512_final(&ctx, hash);
	memcpy(key, hash, sizeof(key));
	sha512_init(&ctx);
	sha512_update(&ctx, (const uint8_t *)"Pair-Setup-AES-IV", 17);
	sha512_update(&ctx, pairing->session_key, sizeof(pairing->session_key));
	sha512_final(&ctx, hash);
	memcpy(iv, hash, sizeof(iv));
	iv[15]++;
	memcpy(server_auth_tag, client_auth_tag, PINPAIR_AUTH_TAG_SIZE);
	result = aes_gcm_crypt(0, key, iv, client_epk, client_public_key, server_auth_tag);
	if (result == 0) {
		iv[15]++;
		result = aes_gcm_crypt(1, key, iv, server_public_key, server_epk,
			server_auth_tag);
	}
	SecureZeroMemory(hash, sizeof(hash));
	SecureZeroMemory(key, sizeof(key));
	SecureZeroMemory(iv, sizeof(iv));
	return result;
}

void
pin_pairing_destroy(pin_pairing_t *pairing)
{
	if (pairing != NULL) {
		SecureZeroMemory(pairing, sizeof(*pairing));
		free(pairing);
	}
}
