#include <winsock2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These checks must also run in the Release build used by CI. */
#undef NDEBUG
#include <assert.h>

#include <libavcodec/avcodec.h>

#include "logger.h"
#include "raop_buffer.h"
#include "raop.h"
#include "raop_rtp_mirror.h"
#include "plist/include/plist.h"

static SOCKET
connect_local(unsigned short port)
{
	SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in address = {0};
	DWORD timeout = 3000;
	assert(client != INVALID_SOCKET);
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(port);
	assert(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) == 0);
	assert(connect(client, (const struct sockaddr *)&address, sizeof(address)) == 0);
	return client;
}

static char *
request(SOCKET client, const char *message, char response[4096], int *body_length)
{
	int received = 0;
	assert(send(client, message, (int)strlen(message), 0) == (int)strlen(message));
	for (;;) {
		char *body;
		int count = recv(client, response + received, 4095 - received, 0);
		assert(count > 0);
		received += count;
		response[received] = '\0';
		body = strstr(response, "\r\n\r\n");
		if (body) {
			const char *length = strstr(response, "Content-Length:");
			*body_length = length ? atoi(length + strlen("Content-Length:")) : 0;
			body += 4;
			if (received >= body - response + *body_length) return body;
		}
		assert(received < 4095);
	}
}

static void
ignore_audio(void *cls, pcm_data_struct *data, const char *name, const char *id)
{
	(void)cls; (void)data; (void)name; (void)id;
}

static int
deny_pin(void *cls, const char *address, const char *pin)
{
	assert(address != NULL && strcmp(pin, "1234") == 0);
	InterlockedIncrement((volatile LONG *)cls);
	return 0;
}

static void
check_receiver(void)
{
	volatile LONG pin_requests = 0;
	raop_callbacks_t callbacks = {0};
	unsigned short port = 0;
	char response[4096];
	int length;
	char *body;
	plist_t info = NULL, display;
	uint64_t value = 0;
	raop_t *server;
	SOCKET client;
	callbacks.cls = (void *)&pin_requests;
	callbacks.audio_process = ignore_audio;
	callbacks.pin_request = deny_pin;
	server = raop_init(2, &callbacks);
	assert(server != NULL);
	raop_set_display_size(server, 2560, 1440);
	raop_set_password(server, "1234");
	assert(raop_start(server, &port) == 1);
	client = connect_local(port);
	body = request(client, "GET /info RTSP/1.0\r\nCSeq: 1\r\n\r\n", response, &length);
	assert(strstr(response, "RTSP/1.0 200 OK") == response);
	plist_from_bin(body, (uint32_t)length, &info);
	assert(info != NULL);
	plist_get_uint_val(plist_dict_get_item(info, "features"), &value);
	assert(value == 0x5A7FFEE6ULL);
	display = plist_array_get_item(plist_dict_get_item(info, "displays"), 0);
	assert(display != NULL);
	plist_get_uint_val(plist_dict_get_item(display, "width"), &value);
	assert(value == 2560);
	plist_get_uint_val(plist_dict_get_item(display, "height"), &value);
	assert(value == 1440);
	plist_free(info);
	request(client, "SETUP /stream RTSP/1.0\r\nCSeq: 2\r\nContent-Length: 0\r\n\r\n", response, &length);
	assert(strstr(response, "RTSP/1.0 470 ") == response);
	assert(InterlockedCompareExchange(&pin_requests, 0, 0) == 1);
	closesocket(client);
	raop_destroy(server);
	puts("Validated receiver resolution, capability advertisement, and PIN denial.");
}

static void
sender_state(void *cls, int paused, const char *name, const char *id)
{
	(void)name; (void)id;
	SetEvent(((HANDLE *)cls)[paused ? 0 : 1]);
}

static void
check_mirror_reconnect(logger_t *logger)
{
	const unsigned char remote[] = {127, 0, 0, 1};
	const unsigned char key[32] = {0};
	HANDLE events[] = {CreateEvent(NULL, FALSE, FALSE, NULL), CreateEvent(NULL, FALSE, FALSE, NULL)};
	raop_callbacks_t callbacks = {0};
	raop_rtp_mirror_t *mirror;
	unsigned short data_port = 0, timing_port = 0;
	char packet[128] = {0};
	SOCKET client;
	WSADATA wsa;
	assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
	assert(events[0] && events[1]);
	callbacks.cls = events;
	callbacks.video_set_sender_paused = sender_state;
	mirror = raop_rtp_mirror_init(logger, &callbacks, remote, sizeof(remote), "smoke", "smoke", key, key, 9);
	assert(mirror != NULL);
	raop_rtp_start_mirror(mirror, 0, 9, &timing_port, &data_port);
	assert(data_port != 0);
	client = connect_local(data_port);
	packet[4] = 1;
	packet[6] = 0x56;
	assert(send(client, packet, sizeof(packet), 0) == sizeof(packet));
	assert(WaitForSingleObject(events[0], 3000) == WAIT_OBJECT_0);
	closesocket(client);
	/* A paused sender may replace its TCP connection without ending the session. */
	client = connect_local(data_port);
	packet[6] = 0x16;
	assert(send(client, packet, sizeof(packet), 0) == sizeof(packet));
	assert(WaitForSingleObject(events[1], 3000) == WAIT_OBJECT_0);
	/* An interrupted header must also leave the listener available for recovery. */
	assert(send(client, packet, 8, 0) == 8);
	closesocket(client);
	client = connect_local(data_port);
	packet[6] = 0x56;
	assert(send(client, packet, sizeof(packet), 0) == sizeof(packet));
	assert(WaitForSingleObject(events[0], 3000) == WAIT_OBJECT_0);
	closesocket(client);
	raop_rtp_mirror_stop(mirror);
	raop_rtp_mirror_stop(mirror);
	raop_rtp_mirror_destroy(mirror);
	CloseHandle(events[0]);
	CloseHandle(events[1]);
	WSACleanup();
	puts("Validated mirror pause/resume, interrupted-header recovery, and repeated stop.");
}

static void
write_log(void *context, int level, const char *message)
{
	(void)context;
	(void)level;
	fprintf(stderr, "%s\n", message);
}

int
main(void)
{
	const unsigned char aes_key[16] = {0};
	const unsigned char aes_iv[16] = {0};
	const unsigned char ecdh_secret[32] = {0};
	logger_t *logger = logger_init();
	raop_buffer_t *buffer;

	if (!logger) {
		fprintf(stderr, "Could not create the test logger.\n");
		return 1;
	}
	logger_set_level(logger, LOGGER_DEBUG);
	logger_set_callback(logger, write_log, NULL);
	if (!avcodec_find_decoder(AV_CODEC_ID_H264)) {
		fprintf(stderr, "The production FFmpeg build does not contain the H.264 decoder.\n");
		logger_destroy(logger);
		return 1;
	}

	buffer = raop_buffer_init(logger, aes_key, aes_iv, ecdh_secret);
	if (!buffer) {
		fprintf(stderr, "Could not initialize the production AAC-ELD decoder.\n");
		logger_destroy(logger);
		return 1;
	}

	raop_buffer_destroy(buffer);
	check_receiver();
	check_mirror_reconnect(logger);
	logger_destroy(logger);
	puts("Validated production FFmpeg H.264 availability and AAC-ELD decoder initialization.");
	return 0;
}
