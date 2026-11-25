/*
 * ESPHome native API server (plaintext framing), the subset a voice
 * satellite needs. Home Assistant connects to us on TCP port 6053.
 *
 * Written against esphome/components/api/api.proto as of 2026-11 (API 1.20).
 * We announce API 1.14: from 1.15 on clients expect DeviceCapabilities and
 * other newer messages that this server does not implement, and they keep a
 * compatibility path for older devices.
 *
 * Frame: 0x00, varint payload length, varint message type, payload.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sat.h"

#define API_MAJOR 1
#define API_MINOR 14

enum {
    MSG_HELLO_REQ = 1, MSG_HELLO_RESP = 2, MSG_AUTH_REQ = 3, MSG_AUTH_RESP = 4,
    MSG_DISCONNECT_REQ = 5, MSG_DISCONNECT_RESP = 6, MSG_PING_REQ = 7, MSG_PING_RESP = 8,
    MSG_DEVICE_INFO_REQ = 9, MSG_DEVICE_INFO_RESP = 10,
    MSG_LIST_ENTITIES_REQ = 11, MSG_LIST_BINARY_SENSOR = 12, MSG_LIST_SWITCH = 17,
    MSG_LIST_ENTITIES_DONE = 19, MSG_SUBSCRIBE_STATES = 20,
    MSG_BINARY_SENSOR_STATE = 21, MSG_SWITCH_STATE = 26, MSG_SWITCH_COMMAND = 33,
    MSG_LIST_BUTTON = 61, MSG_BUTTON_COMMAND = 62,
    MSG_LIST_MEDIA_PLAYER = 63, MSG_MEDIA_PLAYER_STATE = 64, MSG_MEDIA_PLAYER_COMMAND = 65,
    MSG_VA_SUBSCRIBE = 89, MSG_VA_REQUEST = 90, MSG_VA_RESPONSE = 91, MSG_VA_EVENT = 92,
    MSG_VA_AUDIO = 106, MSG_VA_TIMER = 115, MSG_VA_ANNOUNCE = 119,
    MSG_VA_ANNOUNCE_FINISHED = 120, MSG_VA_CONFIG_REQ = 121, MSG_VA_CONFIG_RESP = 122,
    MSG_VA_SET_CONFIG = 123,
};

/* VoiceAssistantFeature bits (aioesphomeapi model.py) */
#define VA_FEATURE_VOICE_ASSISTANT    (1 << 0)
#define VA_FEATURE_API_AUDIO          (1 << 2)
#define VA_FEATURE_TIMERS             (1 << 3)
#define VA_FEATURE_ANNOUNCE           (1 << 4)
#define VA_FEATURE_START_CONVERSATION (1 << 5)

/* MediaPlayerEntityFeature bits */
#define MP_VOLUME_SET     (1 << 2)
#define MP_VOLUME_MUTE    (1 << 3)
#define MP_PLAY_MEDIA     (1 << 9)
#define MP_VOLUME_STEP    (1 << 10)
#define MP_STOP           (1 << 12)
#define MP_MEDIA_ANNOUNCE (1 << 20)

#define VA_REQUEST_USE_VAD 1

/* Entity keys: Home Assistant tells entities apart by these. */
#define MEDIA_PLAYER_KEY  0x65636801u
#define MUTE_KEY          0x65636802u   /* switch: microphones off */
#define RINGING_KEY       0x65636803u   /* binary sensor: a finished timer rings */
#define STOP_RINGING_KEY  0x65636804u   /* button */

#define MAX_CONN 4
#define RX_MAX (64 * 1024)
#define TX_LIMIT (1024 * 1024)       /* a client this far behind is dropped */
#define TX_AUDIO_LIMIT (128 * 1024)  /* microphone audio waits in the ring instead */
#define PING_AFTER_MS 60000
#define DEAD_AFTER_MS 150000

struct conn {
    int fd;
    char peer[64];
    uint8_t *rx;
    size_t rx_len;
    struct pbuf tx;
    size_t tx_off;
    int hello, states, closing;
    uint64_t last_rx, last_ping;
};

static int listen_fd = -1;
static struct conn conns[MAX_CONN];
static struct conn *voice;           /* the connection subscribed to the voice assistant */
static struct pbuf scratch, scratch2;

static void conn_close(struct conn *c, const char *why)
{
    if (c->fd < 0)
        return;
    logi("api: %s disconnected (%s)", c->peer, why);
    close(c->fd);
    c->fd = -1;
    /* rx is kept for the next client in this slot: a caller further up the
     * stack may still be parsing a message out of it. */
    pb_free(&c->tx);
    c->tx_off = 0;
    if (voice == c) {
        voice = NULL;
        sat_voice_client_changed(0);
    }
}

static void conn_flush(struct conn *c)
{
    while (c->fd >= 0 && c->tx_off < c->tx.len) {
        ssize_t n = send(c->fd, c->tx.p + c->tx_off, c->tx.len - c->tx_off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            conn_close(c, strerror(errno));
            return;
        }
        c->tx_off += (size_t)n;
    }
    if (c->fd >= 0) {
        c->tx.len = 0;
        c->tx_off = 0;
        if (c->closing)
            conn_close(c, "closed by request");
    }
}

static void conn_send(struct conn *c, int type, const struct pbuf *payload)
{
    uint8_t zero = 0;

    if (!c || c->fd < 0)
        return;
    if (c->tx.len - c->tx_off > TX_LIMIT) {
        conn_close(c, "send queue overflow");
        return;
    }
    if (c->tx_off > 65536) {   /* partly sent: drop the part that is gone */
        memmove(c->tx.p, c->tx.p + c->tx_off, c->tx.len - c->tx_off);
        c->tx.len -= c->tx_off;
        c->tx_off = 0;
    }
    pb_raw(&c->tx, &zero, 1);
    pb_varint(&c->tx, payload ? payload->len : 0);
    pb_varint(&c->tx, (uint64_t)type);
    if (payload)
        pb_raw(&c->tx, payload->p, payload->len);
    conn_flush(c);
}

/* ---- outgoing messages -------------------------------------------------- */

static void send_hello(struct conn *c)
{
    pb_reset(&scratch);
    pb_uint(&scratch, 1, API_MAJOR);
    pb_uint(&scratch, 2, API_MINOR);
    pb_str(&scratch, 3, "echo-satellite " SAT_VERSION);
    pb_str(&scratch, 4, cfg.name);
    conn_send(c, MSG_HELLO_RESP, &scratch);
}

static void send_device_info(struct conn *c)
{
    pb_reset(&scratch);
    pb_str(&scratch, 2, cfg.name);
    pb_str(&scratch, 3, cfg.mac);
    pb_str(&scratch, 4, ESPHOME_VERSION);
    pb_str(&scratch, 5, __DATE__ ", " __TIME__);
    pb_str(&scratch, 6, "Amazon Echo (1st generation)");
    pb_str(&scratch, 8, "echoroot.echo-satellite");
    pb_str(&scratch, 9, SAT_VERSION);
    pb_str(&scratch, 12, "Amazon");
    pb_str(&scratch, 13, cfg.friendly_name);
    pb_uint(&scratch, 17, VA_FEATURE_VOICE_ASSISTANT | VA_FEATURE_API_AUDIO | VA_FEATURE_TIMERS |
                              VA_FEATURE_ANNOUNCE | VA_FEATURE_START_CONVERSATION);
    conn_send(c, MSG_DEVICE_INFO_RESP, &scratch);
}

static void add_format(struct pbuf *b, int purpose)
{
    /* What we ask Home Assistant's ffmpeg proxy and TTS to produce: WAV at
     * the speaker sink's own rate, so the Cortex-A8 neither decodes nor
     * resamples. */
    pb_reset(&scratch2);
    pb_str(&scratch2, 1, "wav");
    pb_uint(&scratch2, 2, (uint64_t)cfg.tts_rate);
    pb_uint(&scratch2, 3, 1);
    pb_uint(&scratch2, 4, (uint64_t)purpose);
    pb_uint(&scratch2, 5, 2);
    pb_sub(b, 9, &scratch2);
}

static void send_entities(struct conn *c)
{
    pb_reset(&scratch);
    pb_str(&scratch, 1, "media_player");
    pb_fixed32(&scratch, 2, MEDIA_PLAYER_KEY);
    pb_str(&scratch, 3, "Media player");
    add_format(&scratch, 0);   /* default */
    add_format(&scratch, 1);   /* announcement: also selects the TTS format */
    pb_uint(&scratch, 11, MP_VOLUME_SET | MP_VOLUME_MUTE | MP_PLAY_MEDIA | MP_VOLUME_STEP |
                              MP_STOP | MP_MEDIA_ANNOUNCE);
    conn_send(c, MSG_LIST_MEDIA_PLAYER, &scratch);

    /* Switch, binary sensor and button share the first three fields; the
     * icon is field 5 for switch and button, 8 for the binary sensor. */
    pb_reset(&scratch);
    pb_str(&scratch, 1, "mute");
    pb_fixed32(&scratch, 2, MUTE_KEY);
    pb_str(&scratch, 3, "Mute");
    pb_str(&scratch, 5, "mdi:microphone-off");
    conn_send(c, MSG_LIST_SWITCH, &scratch);

    pb_reset(&scratch);
    pb_str(&scratch, 1, "timer_ringing");
    pb_fixed32(&scratch, 2, RINGING_KEY);
    pb_str(&scratch, 3, "Timer ringing");
    pb_str(&scratch, 8, "mdi:timer-alert");
    conn_send(c, MSG_LIST_BINARY_SENSOR, &scratch);

    pb_reset(&scratch);
    pb_str(&scratch, 1, "stop_ringing");
    pb_fixed32(&scratch, 2, STOP_RINGING_KEY);
    pb_str(&scratch, 3, "Stop ringing");
    pb_str(&scratch, 5, "mdi:timer-off");
    conn_send(c, MSG_LIST_BUTTON, &scratch);

    conn_send(c, MSG_LIST_ENTITIES_DONE, NULL);
}

/* Switch and binary sensor states have the same form: key, then the state. */
static void send_bool_state(struct conn *c, int type, uint32_t key, int on)
{
    pb_reset(&scratch);
    pb_fixed32(&scratch, 1, key);
    pb_uint(&scratch, 2, on ? 1 : 0);
    conn_send(c, type, &scratch);
}

void api_mute_state(void)
{
    for (int i = 0; i < MAX_CONN; i++)
        if (conns[i].fd >= 0 && conns[i].states)
            send_bool_state(&conns[i], MSG_SWITCH_STATE, MUTE_KEY, sat_mic_muted());
}

void api_ring_state(void)
{
    for (int i = 0; i < MAX_CONN; i++)
        if (conns[i].fd >= 0 && conns[i].states)
            send_bool_state(&conns[i], MSG_BINARY_SENSOR_STATE, RINGING_KEY, sat_ringing());
}

static void send_media_state(struct conn *c)
{
    float volume = 0;
    int muted = 0;
    int state = sat_media_state(&volume, &muted);

    pb_reset(&scratch);
    pb_fixed32(&scratch, 1, MEDIA_PLAYER_KEY);
    pb_uint(&scratch, 2, (uint64_t)state);
    pb_float(&scratch, 3, volume);
    pb_uint(&scratch, 4, (uint64_t)muted);
    conn_send(c, MSG_MEDIA_PLAYER_STATE, &scratch);
}

void api_media_state(void)
{
    for (int i = 0; i < MAX_CONN; i++)
        if (conns[i].fd >= 0 && conns[i].states)
            send_media_state(&conns[i]);
}

static void send_va_config(struct conn *c)
{
    pb_reset(&scratch);
    for (int i = 0; i < n_wake_words; i++) {
        pb_reset(&scratch2);
        pb_str(&scratch2, 1, wake_words[i].id);
        pb_str(&scratch2, 2, wake_words[i].phrase);
        pb_str(&scratch2, 3, "en");
        pb_sub(&scratch, 1, &scratch2);
    }
    pb_str(&scratch, 2, sat_wake_word_id());
    pb_uint(&scratch, 3, 1);
    conn_send(c, MSG_VA_CONFIG_RESP, &scratch);
}

int api_voice_client(void) { return voice != NULL; }

void api_va_request(int start, const char *conversation_id, const char *wake_word_phrase)
{
    pb_reset(&scratch);
    pb_uint(&scratch, 1, (uint64_t)start);
    if (start) {
        pb_str(&scratch, 2, conversation_id);
        /* The wake word was already detected here; Home Assistant only has
         * to find the end of the command. */
        pb_uint(&scratch, 3, VA_REQUEST_USE_VAD);
        pb_reset(&scratch2);
        pb_uint(&scratch2, 1, (uint64_t)cfg.noise_suppression);
        pb_uint(&scratch2, 2, (uint64_t)cfg.auto_gain);
        pb_float(&scratch2, 3, (float)cfg.volume_multiplier);
        pb_sub(&scratch, 4, &scratch2);
        pb_str(&scratch, 5, wake_word_phrase);
    }
    conn_send(voice, MSG_VA_REQUEST, &scratch);
}

/* 0 while the connection is backed up: leave the audio in the ring for now. */
int api_va_ready(void)
{
    return voice && voice->tx.len - voice->tx_off <= TX_AUDIO_LIMIT;
}

void api_va_audio(const int16_t *samples, int n)
{
    pb_reset(&scratch);
    /* ARM and the wire format are both little-endian 16-bit. */
    pb_bytes(&scratch, 1, samples, (size_t)n * 2);
    conn_send(voice, MSG_VA_AUDIO, &scratch);
}

void api_va_announce_finished(int success)
{
    pb_reset(&scratch);
    pb_uint(&scratch, 1, (uint64_t)success);
    conn_send(voice, MSG_VA_ANNOUNCE_FINISHED, &scratch);
}

/* ---- incoming messages -------------------------------------------------- */

static void on_va_event(const uint8_t *p, size_t len)
{
    static struct va_kv kv[8];
    struct pbr r = { p, p + len }, sub;
    struct pbfield f, g;
    int type = 0, n = 0;

    while (pb_next(&r, &f) > 0) {
        if (f.num == 1 && f.wire == 0) {
            type = (int)f.v;
        } else if (f.num == 2 && f.wire == 2 && n < 8) {
            kv[n].name[0] = kv[n].value[0] = 0;
            sub.p = f.data;
            sub.end = f.data + f.len;
            while (pb_next(&sub, &g) > 0) {
                if (g.num == 1)
                    pb_field_str(&g, kv[n].name, sizeof(kv[n].name));
                else if (g.num == 2)
                    pb_field_str(&g, kv[n].value, sizeof(kv[n].value));
            }
            n++;
        }
    }
    sat_va_event(type, kv, n);
}

static void on_va_timer(const uint8_t *p, size_t len)
{
    struct pbr r = { p, p + len };
    struct pbfield f;
    char id[64] = "", name[64] = "";
    unsigned event = 0, total = 0, left = 0, active = 0;

    while (pb_next(&r, &f) > 0) {
        switch (f.num) {
        case 1: event = (unsigned)f.v; break;
        case 2: pb_field_str(&f, id, sizeof(id)); break;
        case 3: pb_field_str(&f, name, sizeof(name)); break;
        case 4: total = (unsigned)f.v; break;
        case 5: left = (unsigned)f.v; break;
        case 6: active = (unsigned)f.v; break;
        }
    }
    sat_va_timer((int)event, id, name, total, left, (int)active);
}

static void on_va_announce(const uint8_t *p, size_t len)
{
    static char media[1024], text[1024], pre[1024];
    struct pbr r = { p, p + len };
    struct pbfield f;
    int start_conversation = 0;

    media[0] = text[0] = pre[0] = 0;
    while (pb_next(&r, &f) > 0) {
        switch (f.num) {
        case 1: pb_field_str(&f, media, sizeof(media)); break;
        case 2: pb_field_str(&f, text, sizeof(text)); break;
        case 3: pb_field_str(&f, pre, sizeof(pre)); break;
        case 4: start_conversation = (int)f.v; break;
        }
    }
    sat_va_announce(media, text, pre, start_conversation);
}

static void on_media_command(const uint8_t *p, size_t len)
{
    static char url[1024];
    struct pbr r = { p, p + len };
    struct pbfield f;
    int has_command = 0, command = 0, has_volume = 0, has_url = 0, announcement = 0;
    float volume = 0;

    url[0] = 0;
    while (pb_next(&r, &f) > 0) {
        switch (f.num) {
        case 2: has_command = (int)f.v; break;
        case 3: command = (int)f.v; break;
        case 4: has_volume = (int)f.v; break;
        case 5: {
            uint32_t u = (uint32_t)f.v;
            memcpy(&volume, &u, 4);
            break;
        }
        case 6: has_url = (int)f.v; break;
        case 7: pb_field_str(&f, url, sizeof(url)); break;
        case 9: announcement = (int)f.v; break;
        }
    }
    sat_media_command(has_command, command, has_volume, volume, has_url, url, announcement);
}

/* Switch and button commands: the key, and for a switch the wanted state. */
static void on_entity_command(int type, const uint8_t *p, size_t len)
{
    struct pbr r = { p, p + len };
    struct pbfield f;
    uint32_t key = 0;
    int on = 0;

    while (pb_next(&r, &f) > 0) {
        if (f.num == 1 && f.wire == 5)
            key = (uint32_t)f.v;
        else if (f.num == 2 && f.wire == 0)
            on = f.v != 0;
    }
    if (type == MSG_SWITCH_COMMAND && key == MUTE_KEY)
        sat_mute_command(on);
    else if (type == MSG_BUTTON_COMMAND && key == STOP_RINGING_KEY)
        sat_stop_ringing();
    else
        logd("api: command for unknown entity %08x", (unsigned)key);
}

static void handle(struct conn *c, int type, const uint8_t *p, size_t len)
{
    struct pbr r = { p, p + len };
    struct pbfield f;

    if (!c->hello && type != MSG_HELLO_REQ) {
        conn_close(c, "message before hello");
        return;
    }
    switch (type) {
    case MSG_HELLO_REQ: {
        char info[96] = "";
        while (pb_next(&r, &f) > 0)
            if (f.num == 1)
                pb_field_str(&f, info, sizeof(info));
        logi("api: %s is \"%s\"", c->peer, info);
        c->hello = 1;
        send_hello(c);
        break;
    }
    case MSG_AUTH_REQ:   /* clients older than 2026.1 still send it */
        conn_send(c, MSG_AUTH_RESP, NULL);
        break;
    case MSG_DISCONNECT_REQ:
        c->closing = 1;
        conn_send(c, MSG_DISCONNECT_RESP, NULL);
        break;
    case MSG_DISCONNECT_RESP:
        conn_close(c, "disconnect acknowledged");
        break;
    case MSG_PING_REQ:
        conn_send(c, MSG_PING_RESP, NULL);
        break;
    case MSG_PING_RESP:
        break;
    case MSG_DEVICE_INFO_REQ:
        send_device_info(c);
        break;
    case MSG_LIST_ENTITIES_REQ:
        send_entities(c);
        break;
    case MSG_SUBSCRIBE_STATES:
        c->states = 1;
        send_media_state(c);
        send_bool_state(c, MSG_SWITCH_STATE, MUTE_KEY, sat_mic_muted());
        send_bool_state(c, MSG_BINARY_SENSOR_STATE, RINGING_KEY, sat_ringing());
        break;
    case MSG_MEDIA_PLAYER_COMMAND:
        on_media_command(p, len);
        break;
    case MSG_SWITCH_COMMAND:
    case MSG_BUTTON_COMMAND:
        on_entity_command(type, p, len);
        break;
    case MSG_VA_SUBSCRIBE: {
        int subscribe = 0;
        while (pb_next(&r, &f) > 0)
            if (f.num == 1)
                subscribe = (int)f.v;
        if (subscribe) {
            if (voice && voice != c)
                logw("api: voice assistant moves from %s to %s", voice->peer, c->peer);
            voice = c;
            sat_voice_client_changed(1);
        } else if (voice == c) {
            voice = NULL;
            sat_voice_client_changed(0);
        }
        break;
    }
    case MSG_VA_RESPONSE: {
        int error = 0;
        while (pb_next(&r, &f) > 0)
            if (f.num == 2)
                error = (int)f.v;
        if (c == voice)
            sat_va_response(error);
        break;
    }
    case MSG_VA_EVENT:
        if (c == voice)
            on_va_event(p, len);
        break;
    case MSG_VA_TIMER:
        on_va_timer(p, len);
        break;
    case MSG_VA_ANNOUNCE:
        if (c == voice)
            on_va_announce(p, len);
        break;
    case MSG_VA_CONFIG_REQ:
        send_va_config(c);
        break;
    case MSG_VA_SET_CONFIG: {
        char id[32] = "";
        while (pb_next(&r, &f) > 0)
            if (f.num == 1 && !id[0])
                pb_field_str(&f, id, sizeof(id));
        if (id[0])
            sat_set_wake_word(id);
        break;
    }
    default:
        /* Log, service and state subscriptions, time, audio for a speaker
         * we did not declare: nothing to do. */
        logd("api: ignoring message type %d (%u bytes)", type, (unsigned)len);
        break;
    }
}

static void conn_read(struct conn *c)
{
    for (;;) {
        ssize_t n = recv(c->fd, c->rx + c->rx_len, RX_MAX - c->rx_len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                conn_close(c, strerror(errno));
            return;
        }
        if (n == 0) {
            conn_close(c, "closed by peer");
            return;
        }
        c->rx_len += (size_t)n;
        c->last_rx = now_ms();

        size_t off = 0;
        while (c->fd >= 0 && off < c->rx_len) {
            struct pbr r = { c->rx + off, c->rx + c->rx_len };
            uint64_t size, type;

            if (*r.p != 0) {
                conn_close(c, *r.p == 1
                                  ? "client uses Noise encryption, which this server does not support: "
                                    "remove the encryption key for this device"
                                  : "bad frame");
                return;
            }
            r.p++;
            if (pb_read_varint(&r, &size) < 0 || pb_read_varint(&r, &type) < 0)
                break;   /* header incomplete */
            if (size > RX_MAX - 16) {
                conn_close(c, "frame too large");
                return;
            }
            if ((uint64_t)(r.end - r.p) < size)
                break;
            handle(c, (int)type, r.p, (size_t)size);
            off = (size_t)(r.p - c->rx) + (size_t)size;
        }
        if (c->fd < 0)
            return;
        memmove(c->rx, c->rx + off, c->rx_len - off);
        c->rx_len -= off;
    }
}

/* The API is unauthenticated, and whoever connects can open the microphone. */
static int allowed(const char *host)
{
    char list[sizeof(cfg.allow)], *save = NULL;

    if (!cfg.allow[0])
        return 1;
    strcopy(list, sizeof(list), cfg.allow);
    for (char *t = strtok_r(list, ", \t", &save); t; t = strtok_r(NULL, ", \t", &save))
        if (!strcmp(t, host))
            return 1;
    return 0;
}

static void accept_client(void)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    int one = 1, fd = accept(listen_fd, (struct sockaddr *)&ss, &sl);
    struct conn *c = NULL;
    char host[48] = "?";
    int port = 0;

    if (fd < 0)
        return;

    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
        port = ntohs(a->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
        inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof(host));
        port = ntohs(a->sin6_port);
        if (!strncmp(host, "::ffff:", 7))   /* IPv4 client on the dual-stack socket */
            memmove(host, host + 7, strlen(host + 7) + 1);
    }
    if (!allowed(host)) {
        logw("api: connection from %s refused, not in \"allow\"", host);
        close(fd);
        return;
    }
    for (int i = 0; i < MAX_CONN; i++)
        if (conns[i].fd < 0) {
            c = &conns[i];
            break;
        }
    if (!c || (!c->rx && !(c->rx = malloc(RX_MAX)))) {
        logw("api: connection from %s refused, too many clients", host);
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

    c->fd = fd;
    snprintf(c->peer, sizeof(c->peer), "%s:%d", host, port);
    c->rx_len = 0;
    c->tx.len = c->tx_off = 0;
    c->hello = c->states = c->closing = 0;
    c->last_rx = c->last_ping = now_ms();
    logi("api: %s connected", c->peer);
}

int api_init(void)
{
    int one = 1, zero = 0, fd;

    for (int i = 0; i < MAX_CONN; i++)
        conns[i].fd = -1;

    /* One dual-stack socket if the kernel has IPv6, otherwise IPv4. */
    fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in6 a6;
        memset(&a6, 0, sizeof(a6));
        a6.sin6_family = AF_INET6;
        a6.sin6_port = htons((uint16_t)cfg.port);
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        if (bind(fd, (struct sockaddr *)&a6, sizeof(a6)) < 0) {
            close(fd);
            fd = -1;
        }
    }
    if (fd < 0) {
        struct sockaddr_in a4;
        memset(&a4, 0, sizeof(a4));
        a4.sin_family = AF_INET;
        a4.sin_port = htons((uint16_t)cfg.port);
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, (struct sockaddr *)&a4, sizeof(a4)) < 0) {
            loge("api: cannot bind port %d: %s", cfg.port, strerror(errno));
            close(fd);
            return -1;
        }
    }
    if (listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    listen_fd = fd;
    logi("api: listening on port %d as \"%s\" (%s)", cfg.port, cfg.name, cfg.mac);
    return 0;
}

int api_poll_prepare(struct pollfd *fds, int max)
{
    int n = 0;

    if (max < MAX_CONN + 1)
        return 0;
    fds[n].fd = listen_fd;
    fds[n].events = POLLIN;
    fds[n++].revents = 0;
    for (int i = 0; i < MAX_CONN; i++) {
        struct conn *c = &conns[i];
        if (c->fd < 0)
            continue;
        fds[n].fd = c->fd;
        fds[n].events = POLLIN | (c->tx_off < c->tx.len ? POLLOUT : 0);
        fds[n++].revents = 0;
    }
    return n;
}

void api_poll_handle(struct pollfd *fds, int n)
{
    for (int k = 0; k < n; k++) {
        if (!fds[k].revents)
            continue;
        if (fds[k].fd == listen_fd) {
            accept_client();
            continue;
        }
        for (int i = 0; i < MAX_CONN; i++) {
            struct conn *c = &conns[i];
            if (c->fd != fds[k].fd)
                continue;
            if (fds[k].revents & POLLOUT)
                conn_flush(c);
            if (c->fd >= 0 && (fds[k].revents & (POLLIN | POLLHUP | POLLERR)))
                conn_read(c);
            break;
        }
    }
}

void api_housekeeping(void)
{
    uint64_t now = now_ms();

    for (int i = 0; i < MAX_CONN; i++) {
        struct conn *c = &conns[i];
        if (c->fd < 0)
            continue;
        if (now - c->last_rx > DEAD_AFTER_MS) {
            conn_close(c, "no data from client");
        } else if (now - c->last_rx > PING_AFTER_MS && now - c->last_ping > PING_AFTER_MS) {
            c->last_ping = now;
            conn_send(c, MSG_PING_REQ, NULL);
        }
    }
}
