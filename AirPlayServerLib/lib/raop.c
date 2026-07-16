/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <limits.h>

#include "raop.h"
#include "raop_rtp.h"
#include "raop_rtp.h"
#include "pairing.h"
#include "httpd.h"
#include "digest.h"
#include "pinpair.h"

#include "global.h"
#include "fairplay.h"
#include "netutils.h"
#include "logger.h"
#include "compat.h"
#include "raop_rtp_mirror.h"
// #include <android/log.h>

#define MAX_PASSWORD_LEN 64
#define MAX_NONCE_LEN 32

struct raop_s {
	/* Callbacks for audio */
	raop_callbacks_t callbacks;

	/* Logger instance */
	logger_t *logger;

	/* Pairing, HTTP daemon and RSA key */
	pairing_t *pairing;
	httpd_t *httpd;
	char password[MAX_PASSWORD_LEN + 1];
	/* PIN pairing is an HTTP exchange; Apple devices can reconnect between
	 * its individual requests, so this must outlive a TCP connection. */
	pin_pairing_t *pin_pairing;
	int pin_pairing_approved;
	unsigned char pin_pairing_remote[16];
	int pin_pairing_remotelen;
	unsigned char paired_client_keys[10][PINPAIR_ED25519_KEY_SIZE];
	int paired_client_key_count;

    unsigned short port;
};

struct raop_conn_s {
	raop_t *raop;
	raop_rtp_t *raop_rtp;
	raop_rtp_mirror_t *raop_rtp_mirror;
	fairplay_t *fairplay;
	pairing_session_t *pairing;

	unsigned char *local;
	int locallen;

	unsigned char *remote;
	int remotelen;

	char nonce[MAX_NONCE_LEN + 1];
	int authenticated;
	int pin_access_granted;

};
typedef struct raop_conn_s raop_conn_t;

#include "raop_handlers.h"

static void *
conn_init(void *opaque, unsigned char *local, int locallen, unsigned char *remote, int remotelen)
{
	raop_t *raop = opaque;
	raop_conn_t *conn;

	assert(raop);

	conn = calloc(1, sizeof(raop_conn_t));
	if (!conn) {
		return NULL;
	}
	conn->raop = raop;
	conn->raop_rtp = NULL;
	conn->fairplay = fairplay_init(raop->logger);
	//fairplay_init2();
	if (!conn->fairplay) {
		free(conn);
		return NULL;
	}
	conn->pairing = pairing_session_init(raop->pairing);
	if (!conn->pairing) {
		fairplay_destroy(conn->fairplay);
		free(conn);
		return NULL;
	}

	if (locallen == 4) {
		logger_log(conn->raop->logger, LOGGER_INFO,
		           "Local: %d.%d.%d.%d",
		           local[0], local[1], local[2], local[3]);
	} else if (locallen == 16) {
		logger_log(conn->raop->logger, LOGGER_INFO,
		           "Local: %02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
		           local[0], local[1], local[2], local[3], local[4], local[5], local[6], local[7],
		           local[8], local[9], local[10], local[11], local[12], local[13], local[14], local[15]);
	}
	if (remotelen == 4) {
		logger_log(conn->raop->logger, LOGGER_INFO,
		           "Remote: %d.%d.%d.%d",
		           remote[0], remote[1], remote[2], remote[3]);
	} else if (remotelen == 16) {
		logger_log(conn->raop->logger, LOGGER_INFO,
		           "Remote: %02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
		           remote[0], remote[1], remote[2], remote[3], remote[4], remote[5], remote[6], remote[7],
		           remote[8], remote[9], remote[10], remote[11], remote[12], remote[13], remote[14], remote[15]);
	}

	conn->local = malloc(locallen);
	assert(conn->local);
	memcpy(conn->local, local, locallen);

	conn->remote = malloc(remotelen);
	assert(conn->remote);
	memcpy(conn->remote, remote, remotelen);

	conn->locallen = locallen;
	conn->remotelen = remotelen;
	digest_generate_nonce(conn->nonce, sizeof(conn->nonce));

	return conn;
}

static int raop_request_pin_approval(raop_conn_t *conn);
static void raop_pairpin_fail(raop_conn_t *conn, http_request_t *request,
	http_response_t **response);

static int
conn_require_password(raop_conn_t *conn, http_request_t *request,
	http_response_t **response, const char *cseq)
{
	const char *method = http_request_get_method(request);
	const char *url = http_request_get_url(request);
	const char *authorization = http_request_get_header(request, "Authorization");
	if (authorization == NULL) {
		authorization = http_request_get_header(request, "authorization");
	}
	char challenge[96];

	if (conn->raop->password[0] == '\0' || conn->authenticated) {
		return 1;
	}
	if (!conn->pin_access_granted) {
		if (!raop_request_pin_approval(conn)) {
			raop_pairpin_fail(conn, request, response);
			return 0;
		}
		conn->pin_access_granted = 1;
	}

	if (method != NULL && url != NULL &&
		digest_is_valid("raop", conn->raop->password, conn->nonce,
			method, url, authorization)) {
		conn->authenticated = 1;
		logger_log(conn->raop->logger, LOGGER_INFO,
			"AirPlay PIN authentication accepted");
		return 1;
	}

	snprintf(challenge, sizeof(challenge),
		"Digest realm=\"raop\", nonce=\"%s\"", conn->nonce);
	http_response_destroy(*response);
	*response = http_response_init("RTSP/1.0", 401, "Unauthorized");
	if (cseq != NULL) {
		http_response_add_header(*response, "CSeq", cseq);
	}
	http_response_add_header(*response, "Server", "AirTunes/845.5.1");
	http_response_add_header(*response, "WWW-Authenticate", challenge);
	http_response_finish(*response, NULL, 0);
	logger_log(conn->raop->logger, LOGGER_INFO,
		"AirPlay PIN authentication required");
	return 0;
}

static int
raop_is_paired_client(raop_t *raop,
	const unsigned char public_key[PINPAIR_ED25519_KEY_SIZE])
{
	int i;
	for (i = 0; i < raop->paired_client_key_count; ++i) {
		if (memcmp(raop->paired_client_keys[i], public_key,
			PINPAIR_ED25519_KEY_SIZE) == 0) {
			return 1;
		}
	}
	return 0;
}

static void
raop_remember_paired_client(raop_t *raop,
	const unsigned char public_key[PINPAIR_ED25519_KEY_SIZE])
{
	if (raop_is_paired_client(raop, public_key)) return;
	if (raop->paired_client_key_count < 10) {
		memcpy(raop->paired_client_keys[raop->paired_client_key_count++],
			public_key, PINPAIR_ED25519_KEY_SIZE);
		return;
	}
	memmove(raop->paired_client_keys, raop->paired_client_keys + 1,
		(sizeof(raop->paired_client_keys) - PINPAIR_ED25519_KEY_SIZE));
	memcpy(raop->paired_client_keys[9], public_key, PINPAIR_ED25519_KEY_SIZE);
}

static void
raop_reset_pin_pairing(raop_t *raop)
{
	if (raop->pin_pairing != NULL) {
		pin_pairing_destroy(raop->pin_pairing);
		raop->pin_pairing = NULL;
	}
}

static void
raop_clear_pin_pairing_approval(raop_t *raop)
{
	raop->pin_pairing_approved = 0;
	raop->pin_pairing_remotelen = 0;
	SecureZeroMemory(raop->pin_pairing_remote, sizeof(raop->pin_pairing_remote));
}

static void
raop_grant_pin_pairing_approval(raop_conn_t *conn)
{
	int length = conn->remotelen;
	if (length < 0) length = 0;
	if (length > (int)sizeof(conn->raop->pin_pairing_remote)) {
		length = sizeof(conn->raop->pin_pairing_remote);
	}
	raop_clear_pin_pairing_approval(conn->raop);
	if (length > 0) {
		memcpy(conn->raop->pin_pairing_remote, conn->remote, length);
	}
	conn->raop->pin_pairing_remotelen = length;
	conn->raop->pin_pairing_approved = 1;
}

static int
raop_has_pin_pairing_approval(raop_conn_t *conn)
{
	return conn->raop->pin_pairing_approved &&
		conn->raop->pin_pairing_remotelen == conn->remotelen &&
		conn->remotelen > 0 &&
		memcmp(conn->raop->pin_pairing_remote, conn->remote, conn->remotelen) == 0;
}

static void
raop_format_remote_address(const raop_conn_t *conn, char address[64])
{
	if (conn->remotelen == 4) {
		snprintf(address, 64, "%u.%u.%u.%u", conn->remote[0], conn->remote[1],
			conn->remote[2], conn->remote[3]);
		return;
	}
	strncpy(address, "Nearby device", 64);
}

static int
raop_request_pin_approval(raop_conn_t *conn)
{
	char address[64] = { 0 };
	if (conn->raop->password[0] == '\0' || conn->raop->callbacks.pin_request == NULL) {
		return 1;
	}
	raop_format_remote_address(conn, address);
	return conn->raop->callbacks.pin_request(conn->raop->callbacks.cls, address,
		conn->raop->password) != 0;
}

static void
raop_prepare_paired_session(raop_conn_t *conn, http_request_t *request)
{
	unsigned char *data;
	int data_len = 0;

	/* A PIN-paired sender may open pair-verify on a new HTTP connection.
	 * Its Ed25519 key is in the first pair-verify message. */
	data = (unsigned char *)http_request_get_data(request, &data_len);
	if (data_len == 4 + 32 + PINPAIR_ED25519_KEY_SIZE && data[0] == 1 &&
		raop_is_paired_client(conn->raop, data + 4 + 32)) {
		pairing_session_set_setup_status(conn->pairing);
	}
}

static void
raop_pairpin_fail(raop_conn_t *conn, http_request_t *request,
	http_response_t **response)
{
	const char *cseq = http_request_get_header(request, "CSeq");
	http_response_destroy(*response);
	*response = http_response_init("RTSP/1.0", 470, "Client Authentication Failure");
	if (cseq != NULL) {
		http_response_add_header(*response, "CSeq", cseq);
	}
	http_response_add_header(*response, "Server", "AirTunes/845.5.1");
	logger_log(conn->raop->logger, LOGGER_WARNING,
		"AirPlay PIN authentication failed");
}

static void
raop_handler_pairpinstart(raop_conn_t *conn, http_request_t *request,
	http_response_t **response, char **response_data, int *response_datalen)
{
	(void)response_data;
	(void)response_datalen;
	if (conn->raop->password[0] == '\0') {
		logger_log(conn->raop->logger, LOGGER_WARNING,
			"Ignoring PIN pairing request while PIN protection is disabled");
		return;
	}
	raop_reset_pin_pairing(conn->raop);
	raop_clear_pin_pairing_approval(conn->raop);
	if (!raop_request_pin_approval(conn)) {
		raop_pairpin_fail(conn, request, response);
		return;
	}
	conn->pin_access_granted = 1;
	raop_grant_pin_pairing_approval(conn);
	logger_log(conn->raop->logger, LOGGER_INFO,
		"Apple device PIN pairing approved");
}

static void
raop_handler_pairsetup_pin(raop_conn_t *conn, http_request_t *request,
	http_response_t **response, char **response_data, int *response_datalen)
{
	const char *data;
	int data_len = 0;
	plist_t root = NULL;
	plist_t method_node;
	plist_t user_node;
	plist_t public_key_node;
	plist_t proof_node;
	plist_t epk_node;
	plist_t auth_tag_node;

	if (conn->raop->password[0] == '\0' || !raop_has_pin_pairing_approval(conn)) {
		raop_pairpin_fail(conn, request, response);
		return;
	}
	data = http_request_get_data(request, &data_len);
	if (data == NULL || data_len <= 0) {
		raop_pairpin_fail(conn, request, response);
		return;
	}
	plist_from_bin(data, (uint32_t)data_len, &root);
	if (!PLIST_IS_DICT(root)) {
		raop_pairpin_fail(conn, request, response);
		if (root != NULL) plist_free(root);
		return;
	}

	method_node = plist_dict_get_item(root, "method");
	user_node = plist_dict_get_item(root, "user");
	public_key_node = plist_dict_get_item(root, "pk");
	proof_node = plist_dict_get_item(root, "proof");
	epk_node = plist_dict_get_item(root, "epk");
	auth_tag_node = plist_dict_get_item(root, "authTag");

	if (PLIST_IS_STRING(method_node) && PLIST_IS_STRING(user_node)) {
		char *method = NULL;
		char *user = NULL;
		uint8_t salt[PINPAIR_SALT_SIZE];
		uint8_t public_key[PINPAIR_PUBLIC_KEY_SIZE];
		plist_t result;

		plist_get_string_val(method_node, &method);
		plist_get_string_val(user_node, &user);
		if (method == NULL || user == NULL || strcmp(method, "pin") != 0) {
			if (method != NULL) free(method);
			if (user != NULL) free(user);
			plist_free(root);
			raop_clear_pin_pairing_approval(conn->raop);
			raop_pairpin_fail(conn, request, response);
			return;
		}
		raop_reset_pin_pairing(conn->raop);
		conn->raop->pin_pairing = pin_pairing_create(user, conn->raop->password,
			salt, public_key);
		SecureZeroMemory(user, strlen(user));
		free(user);
		free(method);
		plist_free(root);
		if (conn->raop->pin_pairing == NULL) {
			raop_clear_pin_pairing_approval(conn->raop);
			raop_pairpin_fail(conn, request, response);
			return;
		}
		result = plist_new_dict();
		plist_dict_set_item(result, "pk", plist_new_data((const char *)public_key,
			PINPAIR_PUBLIC_KEY_SIZE));
		plist_dict_set_item(result, "salt", plist_new_data((const char *)salt,
			PINPAIR_SALT_SIZE));
		plist_to_bin(result, response_data, (uint32_t *)response_datalen);
		plist_free(result);
		http_response_add_header(*response, "Content-Type",
			"application/x-apple-binary-plist");
		SecureZeroMemory(salt, sizeof(salt));
		SecureZeroMemory(public_key, sizeof(public_key));
		return;
	}

	if (PLIST_IS_DATA(public_key_node) && PLIST_IS_DATA(proof_node)) {
		char *client_public_key = NULL;
		char *client_proof = NULL;
		uint64_t client_public_key_len = 0;
		uint64_t client_proof_len = 0;
		uint8_t server_proof[PINPAIR_PROOF_SIZE];
		plist_t result;
		int verified;

		plist_get_data_val(public_key_node, &client_public_key, &client_public_key_len);
		plist_get_data_val(proof_node, &client_proof, &client_proof_len);
		verified = conn->raop->pin_pairing != NULL && client_public_key != NULL &&
			client_proof != NULL && client_public_key_len <= INT_MAX &&
			client_proof_len <= INT_MAX &&
			pin_pairing_verify(conn->raop->pin_pairing, (const uint8_t *)client_public_key,
				(int)client_public_key_len, (const uint8_t *)client_proof,
				(int)client_proof_len, server_proof) == 0;
		if (client_public_key != NULL) {
			SecureZeroMemory(client_public_key, (SIZE_T)client_public_key_len);
			free(client_public_key);
		}
		if (client_proof != NULL) {
			SecureZeroMemory(client_proof, (SIZE_T)client_proof_len);
			free(client_proof);
		}
		plist_free(root);
		if (!verified) {
			SecureZeroMemory(server_proof, sizeof(server_proof));
			// A mistyped PIN is recoverable. Keep this receiver-side SRP context
			// and this device's approval so iOS can send a new proof (or begin a
			// fresh /pair-setup-pin exchange) after the user corrects the code.
			raop_pairpin_fail(conn, request, response);
			return;
		}
		result = plist_new_dict();
		plist_dict_set_item(result, "proof", plist_new_data((const char *)server_proof,
			PINPAIR_PROOF_SIZE));
		plist_to_bin(result, response_data, (uint32_t *)response_datalen);
		plist_free(result);
		http_response_add_header(*response, "Content-Type",
			"application/x-apple-binary-plist");
		SecureZeroMemory(server_proof, sizeof(server_proof));
		return;
	}

	if (PLIST_IS_DATA(epk_node) && PLIST_IS_DATA(auth_tag_node)) {
		char *client_epk = NULL;
		char *client_auth_tag = NULL;
		uint64_t client_epk_len = 0;
		uint64_t client_auth_tag_len = 0;
		uint8_t server_public_key[PINPAIR_ED25519_KEY_SIZE];
		uint8_t server_epk[PINPAIR_ED25519_KEY_SIZE];
		uint8_t server_auth_tag[PINPAIR_AUTH_TAG_SIZE];
		uint8_t paired_client_key[PINPAIR_ED25519_KEY_SIZE];
		plist_t result;
		int confirmed;

		plist_get_data_val(epk_node, &client_epk, &client_epk_len);
		plist_get_data_val(auth_tag_node, &client_auth_tag, &client_auth_tag_len);
		pairing_get_public_key(conn->raop->pairing, server_public_key);
		confirmed = conn->raop->pin_pairing != NULL && client_epk != NULL &&
			client_auth_tag != NULL && client_epk_len == PINPAIR_ED25519_KEY_SIZE &&
			client_auth_tag_len == PINPAIR_AUTH_TAG_SIZE &&
			pin_pairing_confirm(conn->raop->pin_pairing, (const uint8_t *)client_epk,
				(const uint8_t *)client_auth_tag, server_public_key, server_epk,
				server_auth_tag, paired_client_key) == 0;
		if (client_epk != NULL) {
			SecureZeroMemory(client_epk, (SIZE_T)client_epk_len);
			free(client_epk);
		}
		if (client_auth_tag != NULL) {
			SecureZeroMemory(client_auth_tag, (SIZE_T)client_auth_tag_len);
			free(client_auth_tag);
		}
		plist_free(root);
		SecureZeroMemory(server_public_key, sizeof(server_public_key));
		if (!confirmed) {
			SecureZeroMemory(server_epk, sizeof(server_epk));
			SecureZeroMemory(server_auth_tag, sizeof(server_auth_tag));
			SecureZeroMemory(paired_client_key, sizeof(paired_client_key));
			raop_reset_pin_pairing(conn->raop);
			raop_clear_pin_pairing_approval(conn->raop);
			raop_pairpin_fail(conn, request, response);
			return;
		}
		raop_remember_paired_client(conn->raop, paired_client_key);
		conn->authenticated = 1;
		pairing_session_set_setup_status(conn->pairing);
		raop_reset_pin_pairing(conn->raop);
		raop_clear_pin_pairing_approval(conn->raop);
		result = plist_new_dict();
		plist_dict_set_item(result, "epk", plist_new_data((const char *)server_epk,
			PINPAIR_ED25519_KEY_SIZE));
		plist_dict_set_item(result, "authTag", plist_new_data((const char *)server_auth_tag,
			PINPAIR_AUTH_TAG_SIZE));
		plist_to_bin(result, response_data, (uint32_t *)response_datalen);
		plist_free(result);
		http_response_add_header(*response, "Content-Type",
			"application/x-apple-binary-plist");
		SecureZeroMemory(server_epk, sizeof(server_epk));
		SecureZeroMemory(server_auth_tag, sizeof(server_auth_tag));
		SecureZeroMemory(paired_client_key, sizeof(paired_client_key));
		return;
	}

	plist_free(root);
	raop_clear_pin_pairing_approval(conn->raop);
	raop_pairpin_fail(conn, request, response);
}

static void
conn_request(void *ptr, http_request_t *request, http_response_t **response)
{
	raop_conn_t *conn = ptr;
    logger_log(conn->raop->logger, LOGGER_DEBUG, "conn_request");
	const char *method;
	const char *url;
	const char *cseq;

	char *response_data = NULL;
	int response_datalen = 0;

	method = http_request_get_method(request);
	url = http_request_get_url(request);
	cseq = http_request_get_header(request, "CSeq");
	if (!method || !cseq) {
		return;
	}

	*response = http_response_init("RTSP/1.0", 200, "OK");

	http_response_add_header(*response, "CSeq", cseq);
	//http_response_add_header(*response, "Apple-Jack-Status", "connected; type=analog");
	http_response_add_header(*response, "Server", "AirTunes/845.5.1");

	logger_log(conn->raop->logger, LOGGER_DEBUG, "Handling request %s with URL %s", method, url);
	// AirPlay's password prompt is triggered by the first SETUP. Pairing and
	// capability requests must remain available so the sender can reach it.
	if (!strcmp(method, "SETUP") &&
		!conn_require_password(conn, request, response, cseq)) {
		return;
	}
	raop_handler_t handler = NULL;
	if (!strcmp(method, "POST") && !strcmp(url, "/pair-pin-start")) {
		raop_handler_pairpinstart(conn, request, response,
			&response_data, &response_datalen);
	} else if (!strcmp(method, "POST") && !strcmp(url, "/pair-setup-pin")) {
		raop_handler_pairsetup_pin(conn, request, response,
			&response_data, &response_datalen);
	} else if (!strcmp(method, "GET") && !strcmp(url, "/info")) {
		handler = &raop_handler_info;
	} else if (!strcmp(method, "POST") && !strcmp(url, "/pair-setup")) {
		handler = &raop_handler_pairsetup;
	} else if (!strcmp(method, "POST") && !strcmp(url, "/pair-verify")) {
		raop_prepare_paired_session(conn, request);
		handler = &raop_handler_pairverify;
	} else if (!strcmp(method, "POST") && !strcmp(url, "/fp-setup")) {
		handler = &raop_handler_fpsetup;
	} else if (!strcmp(method, "OPTIONS")) {
		handler = &raop_handler_options;
	} else if (!strcmp(method, "SETUP")) {
		handler = &raop_handler_setup;
	} else if (!strcmp(method, "GET_PARAMETER")) {
		handler = &raop_handler_get_parameter;
	} else if (!strcmp(method, "SET_PARAMETER")) {
		handler = &raop_handler_set_parameter;
	} else if (!strcmp(method, "POST") && !strcmp(url, "/feedback")) {
		handler = &raop_handler_feedback;
	} else if (!strcmp(method, "RECORD")) {
        handler = &raop_handler_record;
	} else if (!strcmp(method, "FLUSH")) {
		const char *rtpinfo;
		int next_seq = -1;

		rtpinfo = http_request_get_header(request, "RTP-Info");
		if (rtpinfo) {
			logger_log(conn->raop->logger, LOGGER_INFO, "Flush with RTP-Info: %s", rtpinfo);
			if (!strncmp(rtpinfo, "seq=", 4)) {
				next_seq = strtol(rtpinfo+4, NULL, 10);
			}
		}
		if (conn->raop_rtp) {
			raop_rtp_flush(conn->raop_rtp, next_seq);
		} else {
			logger_log(conn->raop->logger, LOGGER_WARNING, "RAOP not initialized at FLUSH");
		}
	} else if (!strcmp(method, "TEARDOWN")) {
		handler = &raop_handler_teardown;
		//http_response_add_header(*response, "Connection", "close");
		//if (conn->raop_rtp) {
		//	/* Destroy our RTP session */
		//	raop_rtp_destroy(conn->raop_rtp);
		//	conn->raop_rtp = NULL;
		//}
  //      if (conn->raop_rtp_mirror) {
  //          /* Destroy our mirror session */
  //          raop_rtp_mirror_destroy(conn->raop_rtp_mirror);
  //          conn->raop_rtp_mirror = NULL;
  //      }
	}
	if (handler != NULL) {
		handler(conn, request, *response, &response_data, &response_datalen);
		if (!strcmp(method, "POST") && !strcmp(url, "/pair-verify") &&
			conn->raop->password[0] != '\0' &&
			pairing_session_is_finished(conn->pairing)) {
			unsigned char remote_public_key[PINPAIR_ED25519_KEY_SIZE];
			if (pairing_session_get_remote_public_key(conn->pairing,
				remote_public_key) == 0 &&
				raop_is_paired_client(conn->raop, remote_public_key)) {
				conn->authenticated = 1;
				logger_log(conn->raop->logger, LOGGER_INFO,
					"AirPlay PIN paired client verified");
			}
			SecureZeroMemory(remote_public_key, sizeof(remote_public_key));
		}
	}
	http_response_finish(*response, response_data, response_datalen);
	if (response_data) {
		free(response_data);
		response_data = NULL;
		response_datalen = 0;
	}
}

static void
conn_destroy(void *ptr)
{
	raop_conn_t *conn = ptr;
	if (conn->raop_rtp) {
		/* This is done in case TEARDOWN was not called */
		raop_rtp_destroy(conn->raop_rtp);
	}
    if (conn->raop_rtp_mirror) {
        /* This is done in case TEARDOWN was not called */
        raop_rtp_mirror_destroy(conn->raop_rtp_mirror);
    }
	free(conn->local);
	free(conn->remote);
	pairing_session_destroy(conn->pairing);
	fairplay_destroy(conn->fairplay);
	free(conn);
}

raop_t *
raop_init(int max_clients, raop_callbacks_t *callbacks)
{
	raop_t *raop;
	pairing_t *pairing;
	httpd_t *httpd;
	httpd_callbacks_t httpd_cbs;

	assert(callbacks);
	assert(max_clients > 0);
	assert(max_clients < 100);

	/* Initialize the network */
	if (netutils_init() < 0) {
		return NULL;
	}

	/* Validate the callbacks structure */
	if (!callbacks->audio_process) {
		return NULL;
	}

	/* Allocate the raop_t structure */
	raop = calloc(1, sizeof(raop_t));
	if (!raop) {
		return NULL;
	}

	/* Initialize the logger */
	raop->logger = logger_init();
	pairing = pairing_init_generate();
	if (!pairing) {
		free(raop);
		return NULL;
	}

	/* Set HTTP callbacks to our handlers */
	memset(&httpd_cbs, 0, sizeof(httpd_cbs));
	httpd_cbs.opaque = raop;
	httpd_cbs.conn_init = &conn_init;
	httpd_cbs.conn_request = &conn_request;
	httpd_cbs.conn_destroy = &conn_destroy;

	/* Initialize the http daemon */
	httpd = httpd_init(raop->logger, &httpd_cbs, max_clients);
	if (!httpd) {
		pairing_destroy(pairing);
		free(raop);
		return NULL;
	}
	/* Copy callbacks structure */
	memcpy(&raop->callbacks, callbacks, sizeof(raop_callbacks_t));
	raop->pairing = pairing;
	raop->httpd = httpd;
	return raop;
}

void
raop_destroy(raop_t *raop)
{
	if (raop) {
		raop_stop(raop);

		raop_reset_pin_pairing(raop);
		raop_clear_pin_pairing_approval(raop);
		pairing_destroy(raop->pairing);
		httpd_destroy(raop->httpd);
		logger_destroy(raop->logger);
		free(raop);

		/* Cleanup the network */
		netutils_cleanup();
	}
}

int
raop_is_running(raop_t *raop)
{
	assert(raop);

	return httpd_is_running(raop->httpd);
}

void
raop_set_log_level(raop_t *raop, int level)
{
	assert(raop);

	logger_set_level(raop->logger, level);
}

void
raop_set_port(raop_t *raop, unsigned short port)
{
    assert(raop);
    raop->port = port;
}

unsigned short
raop_get_port(raop_t *raop)
{
    assert(raop);
    return raop->port;
}

void *
raop_get_callback_cls(raop_t *raop)
{
    assert(raop);
    return raop->callbacks.cls;
}

void
raop_set_log_callback(raop_t *raop, raop_log_callback_t callback, void *cls)
{
	assert(raop);

	logger_set_callback(raop->logger, callback, cls);
}

void
raop_set_password(raop_t *raop, const char *password)
{
	assert(raop);

	raop_reset_pin_pairing(raop);
	raop_clear_pin_pairing_approval(raop);
	memset(raop->password, 0, sizeof(raop->password));
	if (password != NULL) {
		strncpy(raop->password, password, MAX_PASSWORD_LEN);
	}
}

void raop_log(raop_t* raop, int level, const char* fmt, ...)
{
	static char buffer[4096];
	va_list ap;

	buffer[sizeof(buffer) - 1] = '\0';
	va_start(ap, fmt);
	vsnprintf(buffer, sizeof(buffer) - 1, fmt, ap);
	va_end(ap);

	logger_log(raop->logger, level, buffer);
}

int
raop_start(raop_t *raop, unsigned short *port)
{
	assert(raop);
	assert(port);
	return httpd_start(raop->httpd, port);
}

void
raop_stop(raop_t *raop)
{
	assert(raop);
	httpd_stop(raop->httpd);
}
