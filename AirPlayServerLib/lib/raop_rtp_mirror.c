//
// Created by Administrator on 2019/1/29/029.
//

#include "raop_rtp_mirror.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include "raop.h"
#include "netutils.h"
#include "compat.h"
#include "logger.h"
#include "byteutils.h"
#include "mirror_buffer.h"
#include "stream.h"

#ifdef WIN32
#include <WinSock2.h>
#include <mstcpip.h>
#include <time.h>
#include <windows.h>
typedef SOCKET mirror_stream_socket_t;
#else
typedef int mirror_stream_socket_t;
#endif // WIN32


#define MAX_MIRROR_PAYLOAD_SIZE (8 * 1024 * 1024)
#define MIRROR_STREAM_RECEIVE_TIMEOUT_MS 5000
#define MIRROR_STREAM_RECONNECT_GRACE_MS 120000
#define MIRROR_TIMING_RESPONSE_TIMEOUT_MS 300
#define MIRROR_TIMING_PROBE_INTERVAL_SECONDS 3
#define MIRROR_TIMING_TIMEOUT_REPORT_INTERVAL 10

static uint64_t
mirror_now_ms(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return ((uint64_t)now.tv_sec * 1000ULL) + ((uint64_t)now.tv_usec / 1000ULL);
}


struct h264codec_s {
    unsigned char compatibility;
    size_t lengthofPPS;
    size_t lengthofSPS;
    unsigned char level;
    unsigned char numberOfPPS;
    unsigned char* picture_parameter_set;
    unsigned char profile_high;
    unsigned char reserved3andSPS;
    unsigned char reserved6andNAL;
    unsigned char* sequence;
    unsigned char version;
};

struct raop_rtp_mirror_s {
    logger_t *logger;
    raop_callbacks_t callbacks;

    /* Buffer to handle all resends */
    mirror_buffer_t *buffer;

    raop_rtp_mirror_t *mirror;
    /* Remote address as sockaddr */
    struct sockaddr_storage remote_saddr;
    socklen_t remote_saddr_len;
	char remoteName[128];
	char remoteDeviceId[128];

    /* MUTEX LOCKED VARIABLES START */
    /* These variables only edited mutex locked */
    int running;
    int joined;
    int stop_in_progress;

    int flush;
    thread_handle_t thread_mirror;
    thread_handle_t thread_time;
    // For thread_mirror exit unexpeced.
    thread_handle_t thread_exit_exception;
    mutex_handle_t run_mutex;

    mutex_handle_t time_mutex;
    cond_handle_t time_cond;
    /* MUTEX LOCKED VARIABLES END */
    int mirror_data_sock, mirror_time_sock;

    unsigned short mirror_data_lport;
    unsigned short mirror_timing_rport;
    unsigned short mirror_timing_lport;
};

/* Read one complete protocol field without allowing a half-delivered TCP frame
 * to block the mirror thread forever. Network-path changes (notably enabling a
 * VPN on the sender) can leave an established socket with no more bytes. */
static int
mirror_recv_exact(raop_rtp_mirror_t *mirror, mirror_stream_socket_t socket_fd,
    unsigned char *buffer, int length)
{
    int offset = 0;

    if (mirror == NULL || socket_fd == -1 || buffer == NULL || length < 0) {
        return -1;
    }
    while (offset < length) {
        fd_set rfds;
        struct timeval timeout;
        int ready;
        int received;
        int running;

        MUTEX_LOCK(mirror->run_mutex);
        running = mirror->running;
        MUTEX_UNLOCK(mirror->run_mutex);
        if (!running) {
            return -1;
        }

        FD_ZERO(&rfds);
        FD_SET(socket_fd, &rfds);
        timeout.tv_sec = MIRROR_STREAM_RECEIVE_TIMEOUT_MS / 1000;
        timeout.tv_usec = (MIRROR_STREAM_RECEIVE_TIMEOUT_MS % 1000) * 1000;
        ready = select((int)(socket_fd + 1), &rfds, NULL, NULL, &timeout);
        if (ready == 0) {
            logger_log(mirror->logger, LOGGER_WARNING,
                "Mirror TCP read timed out after %d ms", MIRROR_STREAM_RECEIVE_TIMEOUT_MS);
            return -1;
        }
        if (ready < 0) {
            logger_log(mirror->logger, LOGGER_WARNING,
                "Mirror TCP select failed");
            return -1;
        }

        received = recv(socket_fd, (char *)buffer + offset, length - offset, 0);
        if (received <= 0) {
            logger_log(mirror->logger, LOGGER_INFO,
                received == 0 ? "Mirror TCP socket closed" : "Mirror TCP receive failed");
            return -1;
        }
        offset += received;
    }
    return 0;
}

static void
mirror_enable_keepalive(mirror_stream_socket_t socket_fd)
{
    int enabled = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE,
        (const char *)&enabled, sizeof(enabled));
#ifdef WIN32
    {
        struct tcp_keepalive settings;
        DWORD bytes_returned = 0;
        settings.onoff = 1;
        settings.keepalivetime = 5000;
        settings.keepaliveinterval = 1000;
        WSAIoctl((SOCKET)socket_fd, SIO_KEEPALIVE_VALS,
            &settings, sizeof(settings), NULL, 0, &bytes_returned, NULL, NULL);
    }
#endif
}

static int
raop_rtp_parse_remote(raop_rtp_mirror_t *raop_rtp_mirror, const unsigned char *remote, int remotelen)
{
    char current[25];
    int family;
    int ret;
    assert(raop_rtp_mirror);
    if (remotelen == 4) {
        family = AF_INET;
    } else if (remotelen == 16) {
        family = AF_INET6;
    } else {
        return -1;
    }
    memset(current, 0, sizeof(current));
    sprintf(current, "%d.%d.%d.%d", remote[0], remote[1], remote[2], remote[3]);
    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_parse_remote ip = %s", current);
    ret = netutils_parse_address(family, current,
                                 &raop_rtp_mirror->remote_saddr,
                                 sizeof(raop_rtp_mirror->remote_saddr));
    if (ret < 0) {
        return -1;
    }
    raop_rtp_mirror->remote_saddr_len = ret;
    return 0;
}

#define NO_FLUSH (-42)
raop_rtp_mirror_t *raop_rtp_mirror_init(logger_t *logger, raop_callbacks_t *callbacks, const unsigned char *remote, int remotelen,
	                                    const char* remoteName, const char* remoteDeviceId,
                                        const unsigned char *aeskey, const unsigned char *ecdh_secret, unsigned short timing_rport)
{
    raop_rtp_mirror_t *raop_rtp_mirror;

    assert(logger);
    assert(callbacks);

    raop_rtp_mirror = calloc(1, sizeof(raop_rtp_mirror_t));
    if (!raop_rtp_mirror) {
        return NULL;
    }
    raop_rtp_mirror->logger = logger;
    raop_rtp_mirror->mirror_timing_rport = timing_rport;

    memcpy(&raop_rtp_mirror->callbacks, callbacks, sizeof(raop_callbacks_t));
    raop_rtp_mirror->buffer = mirror_buffer_init(logger, aeskey, ecdh_secret);
    if (!raop_rtp_mirror->buffer) {
        free(raop_rtp_mirror);
        return NULL;
    }
    if (raop_rtp_parse_remote(raop_rtp_mirror, remote, remotelen) < 0) {
        free(raop_rtp_mirror);
        return NULL;
    }
	memset(raop_rtp_mirror->remoteName, 0, 128);
	memset(raop_rtp_mirror->remoteDeviceId, 0, 128);
	if (remoteName != NULL) {
		strncpy(raop_rtp_mirror->remoteName, remoteName, min(128, strlen(remoteName)));
	}
	if (remoteDeviceId != NULL) {
		strncpy(raop_rtp_mirror->remoteDeviceId, remoteDeviceId, min(128, strlen(remoteDeviceId)));
	}

    raop_rtp_mirror->running = 0;
    raop_rtp_mirror->joined = 1;
    raop_rtp_mirror->stop_in_progress = 0;
    raop_rtp_mirror->flush = NO_FLUSH;

    MUTEX_CREATE(raop_rtp_mirror->run_mutex);
    MUTEX_CREATE(raop_rtp_mirror->time_mutex);
    COND_CREATE(raop_rtp_mirror->time_cond);
    return raop_rtp_mirror;
}

void
raop_rtp_init_mirror_aes(raop_rtp_mirror_t *raop_rtp_mirror, uint64_t streamConnectionID)
{
    mirror_buffer_init_aes(raop_rtp_mirror->buffer, streamConnectionID);
}

/**
 * ntp
 */
static void
mirror_timing_wait_for_next_probe(raop_rtp_mirror_t *raop_rtp_mirror)
{
    struct timeval now;
    struct timespec outtime;
#ifndef WIN32
    MUTEX_LOCK(raop_rtp_mirror->time_mutex);
#endif // !WIN32
    gettimeofday(&now, NULL);
    outtime.tv_sec = now.tv_sec + MIRROR_TIMING_PROBE_INTERVAL_SECONDS;
    outtime.tv_nsec = now.tv_usec * 1000;
    pthread_cond_timedwait(&raop_rtp_mirror->time_cond, &raop_rtp_mirror->time_mutex, &outtime);
#ifndef WIN32
    MUTEX_UNLOCK(raop_rtp_mirror->time_mutex);
#endif // !WIN32
}

static THREAD_RETVAL
raop_rtp_mirror_thread_time(void *arg)
{
    raop_rtp_mirror_t *raop_rtp_mirror = arg;
    assert(raop_rtp_mirror);
    struct sockaddr_storage saddr;
    socklen_t saddrlen;
    unsigned char packet[128];
    int packetlen;
    unsigned char time[48]={35,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    uint64_t base = now_us();
    uint64_t rec_pts = 0;
    unsigned int consecutive_timeouts = 0;
    while (1) {
        MUTEX_LOCK(raop_rtp_mirror->run_mutex);
        if (!raop_rtp_mirror->running) {
            MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
            break;
        }
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        uint64_t send_time = now_us() - base + rec_pts;

        byteutils_put_timeStamp(time, 40, send_time);
        logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror_thread_time send time 48 bytes, port = %d", raop_rtp_mirror->mirror_timing_rport);
        struct sockaddr_in *addr = (struct sockaddr_in *)&raop_rtp_mirror->remote_saddr;
        addr->sin_port = htons(raop_rtp_mirror->mirror_timing_rport);
        int sendlen = sendto(raop_rtp_mirror->mirror_time_sock, (char *)time, sizeof(time), 0, (struct sockaddr *) &raop_rtp_mirror->remote_saddr, raop_rtp_mirror->remote_saddr_len);
        logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror_thread_time sendlen = %d", sendlen);

        fd_set rfds;
        struct timeval tv;
        int nfds, ret;
        /* Timing synchronization is control traffic, not part of the video hot
         * path. Give the phone enough time to answer and keep probes on the
         * protocol's multi-second cadence. A 1 ms timeout with a 10 ms retry
         * floods a slow or busy sender with roughly 100 timing requests/sec. */
        tv.tv_sec = MIRROR_TIMING_RESPONSE_TIMEOUT_MS / 1000;
        tv.tv_usec = (MIRROR_TIMING_RESPONSE_TIMEOUT_MS % 1000) * 1000;

        /* Get the correct nfds value and set rfds */
        FD_ZERO(&rfds);
        FD_SET(raop_rtp_mirror->mirror_time_sock, &rfds);
        nfds = raop_rtp_mirror->mirror_time_sock + 1;
        ret = select(nfds, &rfds, NULL, NULL, &tv);
        if (ret == 0) {
            consecutive_timeouts++;
            if (consecutive_timeouts == 1
                || consecutive_timeouts % MIRROR_TIMING_TIMEOUT_REPORT_INTERVAL == 0) {
                logger_log(raop_rtp_mirror->logger, LOGGER_INFO,
                           "Mirror timing response timeout (consecutive=%u)", consecutive_timeouts);
            }
        } else if (ret == -1) {
            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Mirror timing socket select failed");
            break;
        } else {
            saddrlen = sizeof(saddr);
            packetlen = recvfrom(raop_rtp_mirror->mirror_time_sock, (char *)packet, sizeof(packet), 0,
                                 (struct sockaddr *)&saddr, &saddrlen);
            logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror_thread_time receive time packetlen = %d", packetlen);
            if (packetlen >= 48) {
                if (consecutive_timeouts > 0) {
                    logger_log(raop_rtp_mirror->logger, LOGGER_INFO,
                               "Mirror timing responses resumed after %u timeout(s)", consecutive_timeouts);
                }
                consecutive_timeouts = 0;
                // 32-40 Local time of receiver when NTP request arrives at receiver. T2
                rec_pts = byteutils_read_timeStamp(packet, 32);
            } else {
                consecutive_timeouts++;
                if (consecutive_timeouts == 1
                    || consecutive_timeouts % MIRROR_TIMING_TIMEOUT_REPORT_INTERVAL == 0) {
                    logger_log(raop_rtp_mirror->logger, LOGGER_INFO,
                               "Mirror timing response was invalid (bytes=%d, consecutive=%u)",
                               packetlen, consecutive_timeouts);
                }
            }
        }
        mirror_timing_wait_for_next_probe(raop_rtp_mirror);
    }
    logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Exiting UDP raop_rtp_mirror_thread_time thread");
    return 0;
}
//#define DUMP_H264

static THREAD_RETVAL
raop_exception_thread(void* arg)
{
    raop_rtp_mirror_t* raop_rtp_mirror = arg;
    raop_rtp_mirror_stop(raop_rtp_mirror);
    return 0;
}

#define RAOP_PACKET_LEN 32768
/**
 * mirror
 */
static THREAD_RETVAL
raop_rtp_mirror_thread(void *arg)
{
    raop_rtp_mirror_t *raop_rtp_mirror = arg;
    mirror_stream_socket_t stream_fd = -1;
    unsigned char packet[128];
    memset(packet, 0 , 128);
    unsigned int readstart = 0;
    uint64_t pts_base = 0;
    uint64_t pts = 0;
    uint64_t reconnect_deadline_ms = 0;
    int sender_paused = 0;
    unsigned int data_connection_generation = 0;
    assert(raop_rtp_mirror);

    int exceptionExit = 0;
#ifdef DUMP_H264
    // C decrypted
    FILE* file = fopen("demo.h264", "wb");
    // encrypted source file
    FILE* file_source = fopen("demo.source", "wb");

    FILE* file_len = fopen("demo.len", "wb");
#endif
    while (1) {
        fd_set rfds;
        struct timeval tv;
        int nfds, ret;
        MUTEX_LOCK(raop_rtp_mirror->run_mutex);
        if (!raop_rtp_mirror->running) {
            MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
            break;
        }
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        if (stream_fd == -1 && !sender_paused && reconnect_deadline_ms != 0
            && mirror_now_ms() >= reconnect_deadline_ms) {
            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Mirror data reconnect grace expired");
            exceptionExit = 1;
            break;
        }
        /* Set timeout value to 1ms (reduced from 5ms for lower latency) */
        tv.tv_sec = 0;
        tv.tv_usec = 1000;

        /* Get the correct nfds value and set rfds */
        FD_ZERO(&rfds);
        if (stream_fd == -1) {
            FD_SET(raop_rtp_mirror->mirror_data_sock, &rfds);
            nfds = raop_rtp_mirror->mirror_data_sock+1;
        } else {
            FD_SET(stream_fd, &rfds);
#ifndef WIN32
            nfds = stream_fd+1;
#endif
        }
        ret = select(nfds, &rfds, NULL, NULL, &tv);
        if (ret == 0) {
            /* Timeout happened */
            continue;
        } else if (ret == -1) {
            /* FIXME: Error happened */
            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Error in select");
            exceptionExit = 1;
            break;
        }
        if (stream_fd == -1 && FD_ISSET(raop_rtp_mirror->mirror_data_sock, &rfds)) {
            struct sockaddr_storage saddr;
            socklen_t saddrlen;

            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Accepting client");
            saddrlen = sizeof(saddr);
            stream_fd = accept(raop_rtp_mirror->mirror_data_sock, (struct sockaddr *)&saddr, &saddrlen);
            if (stream_fd == -1) {
                /* FIXME: Error happened */
                logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Error in accept %d %s", errno, strerror(errno));
                exceptionExit = 1;
                break;
            }
#ifdef WIN32
            {
                DWORD receive_timeout = MIRROR_STREAM_RECEIVE_TIMEOUT_MS;
                setsockopt(stream_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&receive_timeout, sizeof(receive_timeout));
            }
#endif
            data_connection_generation++;
            logger_log(raop_rtp_mirror->logger, LOGGER_INFO,
                       "Mirror data client connected (generation=%u)", data_connection_generation);
            reconnect_deadline_ms = 0;
            mirror_enable_keepalive(stream_fd);
        }
        if (stream_fd != -1 && FD_ISSET(stream_fd, &rfds)) {
            // packetlen initially 0
            ret = recv(stream_fd, packet + readstart, 4 - readstart, 0);
            if (ret == 0) {
                /* The sender can cycle this data connection while its AirPlay
                 * session remains valid. Preserve the listener for reconnect. */
                logger_log(raop_rtp_mirror->logger, LOGGER_INFO,
                           "Mirror data client closed the TCP socket; awaiting reconnect (generation=%u, sender_paused=%d)",
                           data_connection_generation, sender_paused);
                closesocket(stream_fd);
                stream_fd = -1;
                reconnect_deadline_ms = sender_paused ? 0 : mirror_now_ms() + MIRROR_STREAM_RECONNECT_GRACE_MS;
                memset(packet, 0, 128);
                readstart = 0;
                pts_base = 0;
                pts = 0;
                continue;
            } else if (ret == -1) {
                logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Mirror data header receive failed; awaiting reconnect");
                closesocket(stream_fd);
                stream_fd = -1;
                reconnect_deadline_ms = sender_paused ? 0 : mirror_now_ms() + MIRROR_STREAM_RECONNECT_GRACE_MS;
                memset(packet, 0, 128);
                readstart = 0;
                pts_base = 0;
                pts = 0;
                continue;
            }
            readstart += ret;
            if (readstart < 4) {
                continue;
            }
            if ((packet[0] == 80 && packet[1] == 79 && packet[2] == 83 && packet[3] == 84) || (packet[0] == 71 && packet[1] == 69 && packet[2] == 84)) {
                // POST or GET
                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "handle http data");
            } else {
                // normal data block
                // read remaining 124 bytes
                if (mirror_recv_exact(raop_rtp_mirror, stream_fd, packet + readstart, 128 - readstart) < 0) {
                    logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Mirror packet header was interrupted; awaiting reconnect");
                    closesocket(stream_fd);
                    stream_fd = -1;
                    reconnect_deadline_ms = sender_paused ? 0 : mirror_now_ms() + MIRROR_STREAM_RECONNECT_GRACE_MS;
                    memset(packet, 0, 128);
                    readstart = 0;
                    pts_base = 0;
                    pts = 0;
                    continue;
                }
                int payloadsize = byteutils_get_int(packet, 0);
                if (payloadsize < 0 || payloadsize > MAX_MIRROR_PAYLOAD_SIZE) {
                    logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Invalid mirror payload size %d; resetting data connection", payloadsize);
                    closesocket(stream_fd);
                    stream_fd = -1;
                    reconnect_deadline_ms = sender_paused ? 0 : mirror_now_ms() + MIRROR_STREAM_RECONNECT_GRACE_MS;
                    memset(packet, 0, 128);
                    readstart = 0;
                    pts_base = 0;
                    pts = 0;
                    continue;
                }
                // FIXME: The calculation method here needs to be confirmed
                short payloadtype = (short) (byteutils_get_short(packet, 4) & 0xff);
                short payloadoption = byteutils_get_short(packet, 6);

                // process content data
                if (payloadtype == 0) {
                    if (payloadsize <= 0) {
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Dropped empty encrypted mirror payload");
                        memset(packet, 0, 128);
                        readstart = 0;
                        continue;
                    }
                    uint64_t payloadntp = byteutils_get_long(packet, 8);
                    // read time
                    if (pts_base == 0) {
                        pts_base = ntptopts(payloadntp);
                    } else {
                        pts =  ntptopts(payloadntp) - pts_base;
                    }
                    // this is encrypted data
                    unsigned char* payload_in = malloc(payloadsize);
                    unsigned char* payload = malloc(payloadsize);
                    if (!payload_in || !payload) {
                        free(payload_in);
                        free(payload);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Failed to allocate mirror payload buffers");
                        exceptionExit = 1;
                        break;
                    }
                    if (mirror_recv_exact(raop_rtp_mirror, stream_fd, payload_in, payloadsize) < 0) {
                        free(payload_in);
                        free(payload);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Encrypted mirror payload was interrupted; awaiting reconnect");
                        closesocket(stream_fd);
                        stream_fd = -1;
                        reconnect_deadline_ms = sender_paused ? 0 : mirror_now_ms() + MIRROR_STREAM_RECONNECT_GRACE_MS;
                        memset(packet, 0, 128);
                        readstart = 0;
                        pts_base = 0;
                        pts = 0;
                        continue;
                    }
                    if (sender_paused && raop_rtp_mirror->callbacks.video_set_sender_paused) {
                        raop_rtp_mirror->callbacks.video_set_sender_paused(
                            raop_rtp_mirror->callbacks.cls, 0,
                            raop_rtp_mirror->remoteName, raop_rtp_mirror->remoteDeviceId);
                    }
                    sender_paused = 0;
                    //logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "readstart = %d", readstart);
#ifdef DUMP_H264
                    fwrite(payload_in, payloadsize, 1, file_source);
                    fwrite(&readstart, sizeof(readstart), 1, file_len);
#endif
                    // decrypt data
                    mirror_buffer_decrypt(raop_rtp_mirror->buffer, payload_in, payload, payloadsize);
                    int nalu_size = 0;
                    int nalu_num = 0;
                    int invalid_access_unit = 0;
                    while (nalu_size + 4 <= payloadsize) {
                        int nc_len = (payload[nalu_size + 0] << 24) | (payload[nalu_size + 1] << 16) | (payload[nalu_size + 2] << 8) | (payload[nalu_size + 3]);
                        if (nc_len <= 0 || nc_len > payloadsize - nalu_size - 4) {
                            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Dropped malformed H264 access unit (NAL length %d)", nc_len);
                            invalid_access_unit = 1;
                            break;
                        }
                        if (nc_len > 0) {
                            payload[nalu_size + 0] = 0;
                            payload[nalu_size + 1] = 0;
                            payload[nalu_size + 2] = 0;
                            payload[nalu_size + 3] = 1;
                            //int nalutype = payload[4] & 0x1f;
                            //logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "nalutype = %d", nalutype);
                            nalu_size += nc_len + 4;
                            nalu_num++;
                        }
                    }
                    if (invalid_access_unit || nalu_size != payloadsize) {
                        free(payload_in);
                        free(payload);
                        memset(packet, 0, 128);
                        readstart = 0;
                        continue;
                    }
                    //logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "nalu_size = %d, payloadsize = %d nalu_num = %d", nalu_size, payloadsize, nalu_num);

                    // write to file
#ifdef DUMP_H264
                    fwrite(payload, payloadsize, 1, file);
#endif
                    h264_decode_struct h264_data;
                    h264_data.data_len = payloadsize;
                    h264_data.data = payload;
                    h264_data.frame_type = packet[5] == 0x10 ? 0 : 1;
                    h264_data.pts = pts;
                    raop_rtp_mirror->callbacks.video_process(raop_rtp_mirror->callbacks.cls, &h264_data, raop_rtp_mirror->remoteName, raop_rtp_mirror->remoteDeviceId);
                    free(payload_in);
                    free(payload);
                } else if ((payloadtype & 255) == 1) {
                    if (packet[6] == 0x56) {
                        if (!sender_paused && raop_rtp_mirror->callbacks.video_set_sender_paused) {
                            raop_rtp_mirror->callbacks.video_set_sender_paused(
                                raop_rtp_mirror->callbacks.cls, 1,
                                raop_rtp_mirror->remoteName, raop_rtp_mirror->remoteDeviceId);
                        }
                        sender_paused = 1;
                        reconnect_deadline_ms = 0;
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO,
                                   "Mirror video stream paused by sender (option=0x%04x, payload=%d)",
                                   (unsigned short)payloadoption, payloadsize);
                    } else if (packet[6] == 0x16) {
                        if (sender_paused && raop_rtp_mirror->callbacks.video_set_sender_paused) {
                            raop_rtp_mirror->callbacks.video_set_sender_paused(
                                raop_rtp_mirror->callbacks.cls, 0,
                                raop_rtp_mirror->remoteName, raop_rtp_mirror->remoteDeviceId);
                        }
                        sender_paused = 0;
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO,
                                   "Mirror video stream resumed by sender (option=0x%04x, payload=%d)",
                                   (unsigned short)payloadoption, payloadsize);
                    }
                    if (payloadsize <= 0) {
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Dropped empty mirror codec payload");
                        memset(packet, 0, 128);
                        readstart = 0;
                        continue;
                    }
                    float mWidthSource = byteutils_get_float(packet, 40);
                    float mHeightSource = byteutils_get_float(packet, 44);
                    float mWidth = byteutils_get_float(packet, 56);
                    float mHeight =byteutils_get_float(packet, 60);
                    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG,
                               "mWidthSource = %f mHeightSource = %f mWidth = %f mHeight = %f",
                               mWidthSource, mHeightSource, mWidth, mHeight);
                    if (raop_rtp_mirror->callbacks.video_report_geometry) {
                        raop_rtp_mirror->callbacks.video_report_geometry(
                            raop_rtp_mirror->callbacks.cls,
                            mWidthSource, mHeightSource, mWidth, mHeight,
                            raop_rtp_mirror->remoteName, raop_rtp_mirror->remoteDeviceId);
                    }

                    // sps_pps this data is not encrypted
                    unsigned char* payload = malloc(payloadsize);
                    if (!payload) {
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Failed to allocate mirror codec payload buffer");
                        exceptionExit = 1;
                        break;
                    }
                    if (mirror_recv_exact(raop_rtp_mirror, stream_fd, payload, payloadsize) < 0) {
                        free(payload);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Mirror codec payload was interrupted; awaiting reconnect");
                        closesocket(stream_fd);
                        stream_fd = -1;
                        reconnect_deadline_ms = sender_paused ? 0 : mirror_now_ms() + MIRROR_STREAM_RECONNECT_GRACE_MS;
                        memset(packet, 0, 128);
                        readstart = 0;
                        pts_base = 0;
                        pts = 0;
                        continue;
                    }
                    if (payloadsize < 11) {
                        free(payload);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Mirror codec payload is too short: %d", payloadsize);
                        memset(packet, 0, 128);
                        readstart = 0;
                        continue;
                    }
                    h264codec_t h264;
                    h264.version = payload[0];
                    h264.profile_high = payload[1];
                    h264.compatibility = payload[2];
                    h264.level = payload[3];
                    h264.reserved6andNAL = payload[4];
                    h264.reserved3andSPS = payload[5];
                    const size_t codec_payload_size = (size_t)payloadsize;
                    h264.lengthofSPS = ((size_t)payload[6] << 8) | (size_t)payload[7];
                    const size_t pps_count_offset = 8 + h264.lengthofSPS;
                    if (h264.lengthofSPS == 0 || pps_count_offset > codec_payload_size
                        || codec_payload_size - pps_count_offset < 3) {
                        free(payload);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Invalid SPS length: %zu", h264.lengthofSPS);
                        memset(packet, 0, 128);
                        readstart = 0;
                        continue;
                    }
                    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "lengthofSPS = %zu", h264.lengthofSPS);
                    h264.sequence = malloc(h264.lengthofSPS);
                    if (!h264.sequence) {
                        free(payload);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Failed to allocate SPS buffer");
                        exceptionExit = 1;
                        break;
                    }
                    memcpy(h264.sequence, payload + 8, h264.lengthofSPS);
                    h264.numberOfPPS = payload[pps_count_offset];
                    const size_t pps_length_offset = pps_count_offset + 1;
                    const size_t pps_data_offset = pps_length_offset + 2;
                    h264.lengthofPPS = ((size_t)payload[pps_length_offset] << 8)
                        | (size_t)payload[pps_length_offset + 1];
                    if (h264.numberOfPPS == 0 || h264.lengthofPPS == 0
                        || h264.lengthofPPS > codec_payload_size - pps_data_offset) {
                        free(payload);
                        free(h264.sequence);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Invalid PPS length: %zu", h264.lengthofPPS);
                        memset(packet, 0, 128);
                        readstart = 0;
                        continue;
                    }
                    h264.picture_parameter_set = malloc(h264.lengthofPPS);
                    if (!h264.picture_parameter_set) {
                        free(payload);
                        free(h264.sequence);
                        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Failed to allocate PPS buffer");
                        exceptionExit = 1;
                        break;
                    }
                    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "lengthofPPS = %zu", h264.lengthofPPS);
                    memcpy(h264.picture_parameter_set, payload + pps_data_offset, h264.lengthofPPS);
                    if (h264.lengthofSPS + h264.lengthofPPS < 102400) {
                        // copy sps_pps
                        int sps_pps_len = (int)(h264.lengthofSPS + h264.lengthofPPS + 8);
                        unsigned char* sps_pps = malloc(sps_pps_len);
                        if (!sps_pps) {
                            free(payload);
                            free(h264.picture_parameter_set);
                            free(h264.sequence);
                            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Failed to allocate SPS/PPS buffer");
                            exceptionExit = 1;
                            break;
                        }
                        sps_pps[0] = 0;
                        sps_pps[1] = 0;
                        sps_pps[2] = 0;
                        sps_pps[3] = 1;
                        memcpy(sps_pps + 4, h264.sequence, h264.lengthofSPS);
                        sps_pps[h264.lengthofSPS + 4] = 0;
                        sps_pps[h264.lengthofSPS + 5] = 0;
                        sps_pps[h264.lengthofSPS + 6] = 0;
                        sps_pps[h264.lengthofSPS + 7] = 1;
                        memcpy(sps_pps + h264.lengthofSPS + 8, h264.picture_parameter_set, h264.lengthofPPS);
#ifdef DUMP_H264
                        fwrite(sps_pps, sps_pps_len, 1, file);
#endif
                        h264_decode_struct h264_data;
                        h264_data.data_len = sps_pps_len;
                        h264_data.data = sps_pps;
                        h264_data.frame_type = 0;
                        h264_data.pts = 0;
                        raop_rtp_mirror->callbacks.video_process(raop_rtp_mirror->callbacks.cls, &h264_data, raop_rtp_mirror->remoteName, raop_rtp_mirror->remoteDeviceId);
                        free(sps_pps);
                    }
                    free(payload);
                    free(h264.picture_parameter_set);
                    free(h264.sequence);
                } else if (payloadtype == (short) 2) {
                    if (payloadsize > 0) {
                        unsigned char* payload_in = malloc(payloadsize);
                        if (!payload_in || mirror_recv_exact(raop_rtp_mirror, stream_fd, payload_in, payloadsize) < 0) {
                            free(payload_in);
                            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Mirror control payload was interrupted; awaiting reconnect");
                            closesocket(stream_fd);
                            stream_fd = -1;
                            reconnect_deadline_ms = sender_paused ? 0 : mirror_now_ms() + MIRROR_STREAM_RECONNECT_GRACE_MS;
                            memset(packet, 0, 128);
                            readstart = 0;
                            pts_base = 0;
                            pts = 0;
                            continue;
                        }
						free(payload_in);
                    }
                } else if (payloadtype == (short) 4) {
                    if (payloadsize > 0) {
                        unsigned char* payload_in = malloc(payloadsize);
                        if (!payload_in || mirror_recv_exact(raop_rtp_mirror, stream_fd, payload_in, payloadsize) < 0) {
                            free(payload_in);
                            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Mirror control payload was interrupted; awaiting reconnect");
                            closesocket(stream_fd);
                            stream_fd = -1;
                            reconnect_deadline_ms = sender_paused ? 0 : mirror_now_ms() + MIRROR_STREAM_RECONNECT_GRACE_MS;
                            memset(packet, 0, 128);
                            readstart = 0;
                            pts_base = 0;
                            pts = 0;
                            continue;
                        }
						free(payload_in);
                    }
                } else {
                    if (payloadsize > 0) {
                        unsigned char* payload_in = malloc(payloadsize);
                        if (!payload_in || mirror_recv_exact(raop_rtp_mirror, stream_fd, payload_in, payloadsize) < 0) {
                            free(payload_in);
                            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Unknown mirror payload was interrupted; awaiting reconnect");
                            closesocket(stream_fd);
                            stream_fd = -1;
                            reconnect_deadline_ms = sender_paused ? 0 : mirror_now_ms() + MIRROR_STREAM_RECONNECT_GRACE_MS;
                            memset(packet, 0, 128);
                            readstart = 0;
                            pts_base = 0;
                            pts = 0;
                            continue;
                        }
                        free(payload_in);
                    }
                }
            }
            memset(packet, 0 , 128);
            readstart = 0;
        }
    }

    /* Close the stream file descriptor */
    if (stream_fd != -1) {
        closesocket(stream_fd);
    }
    if (exceptionExit) {
        if (raop_rtp_mirror->thread_exit_exception != NULL) {
            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Exiting exception thread[1]");
            THREAD_JOIN(raop_rtp_mirror->thread_exit_exception);
            raop_rtp_mirror->thread_exit_exception = NULL;
            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Exception thread exit[1]");
        }
        THREAD_CREATE(raop_rtp_mirror->thread_exit_exception, raop_exception_thread, raop_rtp_mirror);
    }
    logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Exiting TCP raop_rtp_mirror_thread thread");
#ifdef DUMP_H264
    fclose(file);
    fclose(file_source);
    fclose(file_len);
#endif
    return 0;
}

void
raop_rtp_start_mirror(raop_rtp_mirror_t *raop_rtp_mirror, int use_udp, unsigned short mirror_timing_rport, unsigned short * mirror_timing_lport,
                      unsigned short *mirror_data_lport)
{
    int use_ipv6 = 0;

    assert(raop_rtp_mirror);

    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    if (raop_rtp_mirror->running || !raop_rtp_mirror->joined || raop_rtp_mirror->stop_in_progress) {
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        return;
    }

    //raop_rtp_mirror->mirror_timing_rport = mirror_timing_rport;
    if (raop_rtp_mirror->remote_saddr.ss_family == AF_INET6) {
        use_ipv6 = 1;
    }
    use_ipv6 = 0;
    if (raop_rtp_init_mirror_sockets(raop_rtp_mirror, use_ipv6) < 0) {
        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Initializing sockets failed");
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        return;
    }
    if (mirror_timing_lport) *mirror_timing_lport = raop_rtp_mirror->mirror_timing_lport;
    if (mirror_data_lport) *mirror_data_lport = raop_rtp_mirror->mirror_data_lport;

    /* Create the thread and initialize running values */
    raop_rtp_mirror->running = 1;
    raop_rtp_mirror->joined = 0;

    THREAD_CREATE(raop_rtp_mirror->thread_mirror, raop_rtp_mirror_thread, raop_rtp_mirror);
    THREAD_CREATE(raop_rtp_mirror->thread_time, raop_rtp_mirror_thread_time, raop_rtp_mirror);
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);

    if (raop_rtp_mirror->callbacks.connected != NULL) {
        raop_rtp_mirror->callbacks.connected(raop_rtp_mirror->callbacks.cls, raop_rtp_mirror->remoteName, raop_rtp_mirror->remoteDeviceId);
    }
}

void raop_rtp_mirror_stop(raop_rtp_mirror_t *raop_rtp_mirror) {
    int mirror_data_sock;
    int mirror_time_sock;

    /* A stream-specific TEARDOWN may reference a stream that this control
     * connection never initialized. Treat that as an idempotent stop. */
    if (!raop_rtp_mirror) {
        return;
    }
    assert(raop_rtp_mirror);
    logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Stopping raop rtp mirror");

    /* A TCP close and RTSP TEARDOWN can race. Only one caller may close and
     * join the worker threads; duplicate and re-entrant stops return. */
    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    if (raop_rtp_mirror->joined || raop_rtp_mirror->stop_in_progress) {
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        return;
    }
    raop_rtp_mirror->stop_in_progress = 1;
    raop_rtp_mirror->running = 0;
    mirror_data_sock = raop_rtp_mirror->mirror_data_sock;
    mirror_time_sock = raop_rtp_mirror->mirror_time_sock;
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);

    /* Keep the shared descriptors valid until the workers have left FD_SET,
     * but close local snapshots to wake any pending socket operation. */
    if (mirror_data_sock != -1) {
        closesocket(mirror_data_sock);
    }
    if (mirror_time_sock != -1) {
        closesocket(mirror_time_sock);
    }

#ifndef WIN32
    MUTEX_LOCK(raop_rtp_mirror->time_mutex);
#endif // !WIN32
    COND_SIGNAL(raop_rtp_mirror->time_cond);
#ifndef WIN32
    MUTEX_UNLOCK(raop_rtp_mirror->time_mutex);
#endif // !WIN32

    logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Join mirror thread");
    THREAD_JOIN(raop_rtp_mirror->thread_mirror);

    logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Join mirror time thread");
    THREAD_JOIN(raop_rtp_mirror->thread_time);

    if (raop_rtp_mirror->callbacks.disconnected != NULL) {
        raop_rtp_mirror->callbacks.disconnected(raop_rtp_mirror->callbacks.cls, raop_rtp_mirror->remoteName, raop_rtp_mirror->remoteDeviceId);
    }
    logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Raop rtp mirror stopped");

    /* Publish completion only after callbacks and logging finish. Destroy waits
     * for this state before releasing the object. */
    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    raop_rtp_mirror->mirror_data_sock = -1;
    raop_rtp_mirror->mirror_time_sock = -1;
    raop_rtp_mirror->joined = 1;
    raop_rtp_mirror->stop_in_progress = 0;
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
}

void raop_rtp_mirror_destroy(raop_rtp_mirror_t *raop_rtp_mirror) {
    if (raop_rtp_mirror) {
        int stop_complete = 0;

        raop_rtp_mirror_stop(raop_rtp_mirror);

        /* stop() deliberately returns for a duplicate caller. Destruction must
         * wait for the owner to finish using the object. */
        while (!stop_complete) {
            MUTEX_LOCK(raop_rtp_mirror->run_mutex);
            stop_complete = raop_rtp_mirror->joined;
            MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
            if (!stop_complete) {
                sleepms(1);
            }
        }

        if (raop_rtp_mirror->thread_exit_exception) {
            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Exiting exception thread");
            THREAD_JOIN(raop_rtp_mirror->thread_exit_exception);
            raop_rtp_mirror->thread_exit_exception = NULL;
            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Exception thread exit");
        }
        MUTEX_DESTROY(raop_rtp_mirror->run_mutex);
        MUTEX_DESTROY(raop_rtp_mirror->time_mutex);
        COND_DESTROY(raop_rtp_mirror->time_cond);
        mirror_buffer_destroy(raop_rtp_mirror->buffer);
        free(raop_rtp_mirror);
    }
}

static int
raop_rtp_init_mirror_sockets(raop_rtp_mirror_t *raop_rtp_mirror, int use_ipv6)
{
    int dsock = -1, tsock = -1;
    unsigned short tport = 0, dport = 0;

    assert(raop_rtp_mirror);

    dsock = netutils_init_socket(&dport, use_ipv6, 0);
    tsock = netutils_init_socket(&tport, use_ipv6, 1);
    if (dsock == -1 || tsock == -1) {
        goto sockets_cleanup;
    }

    /* Listen to the data socket if using TCP */
    if (listen(dsock, 1) < 0)
        goto sockets_cleanup;


    /* Set socket descriptors */
    raop_rtp_mirror->mirror_data_sock = dsock;
    raop_rtp_mirror->mirror_time_sock = tsock;

    /* Set port values */
    raop_rtp_mirror->mirror_data_lport = dport;
    raop_rtp_mirror->mirror_timing_lport = tport;

    logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Mirror data port: %d, timing port: ", dport, tport);

    return 0;

    sockets_cleanup:
    if (tsock != -1) closesocket(tsock);
    if (dsock != -1) closesocket(dsock);
    return -1;
}
