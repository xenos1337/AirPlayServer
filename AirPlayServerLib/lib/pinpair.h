#ifndef PINPAIR_H
#define PINPAIR_H

#include <stdint.h>

typedef struct pin_pairing_s pin_pairing_t;

#define PINPAIR_SALT_SIZE 16
#define PINPAIR_PUBLIC_KEY_SIZE 256
#define PINPAIR_PROOF_SIZE 20
#define PINPAIR_ED25519_KEY_SIZE 32
#define PINPAIR_AUTH_TAG_SIZE 16

pin_pairing_t *pin_pairing_create(const char *username, const char *pin,
	uint8_t salt[PINPAIR_SALT_SIZE],
	uint8_t public_key[PINPAIR_PUBLIC_KEY_SIZE]);

int pin_pairing_verify(pin_pairing_t *pairing, const uint8_t *client_public_key,
	int client_public_key_len, const uint8_t *client_proof, int client_proof_len,
	uint8_t server_proof[PINPAIR_PROOF_SIZE]);

int pin_pairing_confirm(pin_pairing_t *pairing,
	const uint8_t client_epk[PINPAIR_ED25519_KEY_SIZE],
	const uint8_t client_auth_tag[PINPAIR_AUTH_TAG_SIZE],
	const uint8_t server_public_key[PINPAIR_ED25519_KEY_SIZE],
	uint8_t server_epk[PINPAIR_ED25519_KEY_SIZE],
	uint8_t server_auth_tag[PINPAIR_AUTH_TAG_SIZE],
	uint8_t client_public_key[PINPAIR_ED25519_KEY_SIZE]);

void pin_pairing_destroy(pin_pairing_t *pairing);

#endif
