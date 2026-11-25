/*
 * echo-satellite: Home Assistant voice satellite for the Amazon Echo gen 1
 * running echoroot. See README.md in this directory.
 */
#ifndef SAT_H
#define SAT_H

#include <stddef.h>
#include <stdint.h>

#define SAT_VERSION "0.1.0"
#define ESPHOME_VERSION "2026.11.0"   /* release line of the api.proto we follow */

/* ---- util.c ----------------------------------------------------------- */

enum { LOG_ERR_ = 0, LOG_WARN_, LOG_INFO_, LOG_DEBUG_ };
extern int log_level;
void log_init(int use_syslog);
void logmsg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define loge(...) logmsg(LOG_ERR_, __VA_ARGS__)
#define logw(...) logmsg(LOG_WARN_, __VA_ARGS__)
#define logi(...) logmsg(LOG_INFO_, __VA_ARGS__)
#define logd(...) logmsg(LOG_DEBUG_, __VA_ARGS__)

uint64_t now_ms(void);                       /* monotonic */
void strcopy(char *dst, size_t cap, const char *src);

/* ---- config ----------------------------------------------------------- */

struct config {
    char name[32];             /* ESPHome node name (hostname style) */
    char friendly_name[121];
    char iface[16];            /* interface whose MAC identifies the device */
    char mac[18];
    int port;
    char allow[256];           /* client addresses allowed to connect; empty: any */
    char mdns_file[128];       /* avahi service file to write; empty: none */

    char mic_shm[64];
    char pulse_server[128];    /* empty: libpulse default */
    char pulse_sink[64];       /* empty: default sink */
    int tts_rate;              /* sample rate requested from Home Assistant */
    int tls_insecure;

    char locale[16];           /* wake-word model locale, e.g. en-US */
    char wake_word[16];        /* default wake word id */
    char state_file[128];
    char model_dir[128];       /* asrd's wake-word models; empty: do not store the choice */
    char asr_restart_cmd[128]; /* run when asrd has to be restarted to load a model */
    int asr_runtime_switch;    /* try asrd's WWModel property before restarting it */

    int mic_gain_db;           /* applied to the microphone samples before sending */
    int noise_suppression;     /* passed to Home Assistant with each request */
    int auto_gain;
    double volume_multiplier;

    int start_timeout_ms;      /* request sent -> Home Assistant accepts */
    int listen_timeout_ms;     /* streaming microphone */
    int think_timeout_ms;      /* end of speech -> response */
    int conversation_timeout_s;
    int ring_seconds;          /* how long a finished timer rings */
    int ring_gap_ms;

    char led_idle[48], led_wake[48], led_listening[48], led_thinking[48];
    char led_speaking[48], led_error[48], led_timer[48], led_muted[48];

    char sound_wake[160], sound_done[160], sound_error[160];
    char sound_timer[160], sound_mute[160], sound_unmute[160];
};
extern struct config cfg;

/* ---- pb.c: minimal protobuf ------------------------------------------- */

struct pbuf {
    uint8_t *p;
    size_t len, cap;
};
void pb_reset(struct pbuf *b);
void pb_free(struct pbuf *b);
void pb_raw(struct pbuf *b, const void *data, size_t len);
void pb_varint(struct pbuf *b, uint64_t v);
void pb_uint(struct pbuf *b, int field, uint64_t v);         /* omitted if 0 */
void pb_str(struct pbuf *b, int field, const char *s);       /* omitted if empty */
void pb_bytes(struct pbuf *b, int field, const void *d, size_t len);
void pb_fixed32(struct pbuf *b, int field, uint32_t v);      /* always written */
void pb_float(struct pbuf *b, int field, float v);           /* omitted if 0 */
void pb_sub(struct pbuf *b, int field, const struct pbuf *sub);

struct pbr {
    const uint8_t *p, *end;
};
struct pbfield {
    uint32_t num;
    int wire;               /* 0 varint, 1 fixed64, 2 bytes, 5 fixed32 */
    uint64_t v;             /* varint or fixed value */
    const uint8_t *data;    /* wire type 2 */
    size_t len;
};
int pb_read_varint(struct pbr *r, uint64_t *v);
int pb_next(struct pbr *r, struct pbfield *f);  /* 1 field, 0 end, -1 error */
void pb_field_str(const struct pbfield *f, char *dst, size_t cap);

/* ---- mic.c: DSP output ring (/BMicsOut.shm) ---------------------------- */

int mic_open(void);
int mic_seek(uint64_t seq);      /* start reading at sample sequence number */
void mic_seek_now(void);
int mic_read(int16_t *buf, int max_samples);   /* samples read, 0 if none */

/* ---- bus.c: D-Bus system bus (Amazon "lipc") --------------------------- */

#define SVC_ASR     "com.doppler.asr"
#define SVC_LEDD    "com.doppler.ledd"
#define SVC_BUTTOND "com.doppler.buttond"
#define SVC_AUDIOD  "com.doppler.audiod"

int bus_init(void);
int bus_fd(void);
int bus_dispatch(void);          /* -1 if the bus connection was lost */
void bus_set_str(const char *svc, const char *prop, const char *val);
void bus_set_int(const char *svc, const char *prop, int val);
int bus_get_int(const char *svc, const char *prop, int *out);
int bus_get_str(const char *svc, const char *prop, char *out, size_t cap);

/* ---- player.c: playback thread ----------------------------------------- */

enum { PLAYER_STARTED = 1, PLAYER_FINISHED = 2 };
enum { PLAY_OK = 0, PLAY_FAILED = 1, PLAY_CANCELLED = 2 };
#define PLAYER_MAX_ITEMS 3

int player_init(void);           /* returns fd that becomes readable on events */
unsigned player_play(const char *const *items, int n, int loop, int gap_ms);
void player_stop(void);
int player_next_event(unsigned *job, int *what, int *status);

/* ---- api.c: ESPHome native API server ---------------------------------- */

struct pollfd;
int api_init(void);
int api_poll_prepare(struct pollfd *fds, int max);
void api_poll_handle(struct pollfd *fds, int n);
void api_housekeeping(void);
int api_voice_client(void);      /* 1 if Home Assistant is subscribed */

void api_va_request(int start, const char *conversation_id, const char *wake_word_phrase);
int api_va_ready(void);          /* 0 while the connection is backed up */
void api_va_audio(const int16_t *samples, int n);
void api_va_announce_finished(int success);
void api_media_state(void);
void api_mute_state(void);       /* tell Home Assistant the "Mute" switch's state */
void api_ring_state(void);       /* ... and the "Timer ringing" sensor's */

/* ---- main.c: callbacks into the state machine -------------------------- */

struct va_kv {
    char name[32];
    char value[1024];
};

/* from api.c */
void sat_voice_client_changed(int subscribed);
void sat_va_response(int error);
void sat_va_event(int type, const struct va_kv *kv, int n);
void sat_va_timer(int event, const char *id, const char *name, unsigned total, unsigned left, int active);
void sat_va_announce(const char *media_id, const char *text, const char *preannounce, int start_conversation);
void sat_set_wake_word(const char *id);
const char *sat_wake_word_id(void);
void sat_media_command(int has_command, int command, int has_volume, float volume,
                       int has_url, const char *url, int announcement);
int sat_media_state(float *volume, int *muted);   /* returns MediaPlayerState */
int sat_mic_muted(void);
void sat_mute_command(int muted);                 /* Home Assistant's "Mute" switch */
int sat_ringing(void);
void sat_stop_ringing(void);                      /* Home Assistant's "Stop ringing" button */

struct wake_word {
    const char *id, *phrase, *model;
};
extern const struct wake_word wake_words[];
extern const int n_wake_words;

/* from bus.c */
void sat_keyword(uint64_t stream_start, uint64_t ww_begin, uint64_t ww_end);
void sat_local_command(const char *cmd);
void sat_asr_ready(int ready);
void sat_button(const char *name, int state);
void sat_volume_changed(int volume);
void sat_audio_mute_changed(int muted);

#endif
