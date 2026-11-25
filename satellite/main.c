/*
 * echo-satellite: configuration, event loop and the voice state machine.
 *
 *   IDLE -> STARTING -> LISTENING -> THINKING -> SPEAKING -> IDLE
 *
 * STARTING   request sent to Home Assistant, waiting for it to accept
 * LISTENING  microphone audio is streamed
 * THINKING   Home Assistant found the end of speech
 * SPEAKING   response or announcement is playing
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sat.h"

#define EARCON "/usr/local/share/earcon/doppler/base/"

struct config cfg = {
    .friendly_name = "Echo",
    .iface = "wlan0",
    .port = 6053,
    .mic_shm = "/BMicsOut.shm",
    .pulse_sink = "lab126-sink",
    .tts_rate = 48000,
    .locale = "en-US",
    .wake_word = "alexa",
    .state_file = "/etc/echo-satellite.state",
    .model_dir = "/usr/local/data/models/keyword/doppler",
    .asr_restart_cmd = "killall asrd",
    .asr_runtime_switch = 1,
    .mic_gain_db = 20,
    .volume_multiplier = 1.0,
    .start_timeout_ms = 5000,
    .listen_timeout_ms = 30000,
    .think_timeout_ms = 60000,
    .conversation_timeout_s = 300,
    .ring_seconds = 300,
    .ring_gap_ms = 600,
    .led_idle = "off",
    .led_wake = "active_start",
    .led_listening = "alexa_user-talking",
    .led_thinking = "alexa_thinking",
    .led_speaking = "dialog-talk",
    .led_error = "error",
    .led_timer = "ready_timer",
    .led_muted = "mics-off_on",
    .sound_timer = EARCON "system_alerts_melodic_01_short.wav",
    .sound_mute = EARCON "controls_buttons_mic_off.wav",
    .sound_unmute = EARCON "controls_buttons_mic_on.wav",
};

/* Amazon's wake-word models on the image; see ASRD.md. */
const struct wake_word wake_words[] = {
    { "alexa", "Alexa", "ALEXA" },
    { "computer", "Computer", "COMPUTER" },
    { "echo", "Echo", "ECHO" },
    { "amazon", "Amazon", "AMAZON" },
};
const int n_wake_words = sizeof(wake_words) / sizeof(wake_words[0]);

/* VoiceAssistantEvent */
enum {
    EV_ERROR = 0, EV_RUN_START = 1, EV_RUN_END = 2, EV_STT_START = 3, EV_STT_END = 4,
    EV_INTENT_START = 5, EV_INTENT_END = 6, EV_TTS_START = 7, EV_TTS_END = 8,
    EV_STT_VAD_START = 11, EV_STT_VAD_END = 12, EV_INTENT_PROGRESS = 100,
};

/* ButtonState values broadcast by buttond, observed on the stock daemon. */
enum { BTN_DOWN = 1, BTN_UP = 2, BTN_MUTE_ON = 6, BTN_MUTE_OFF = 7 };

enum state { ST_IDLE, ST_STARTING, ST_LISTENING, ST_THINKING, ST_SPEAKING, ST_ERROR };
static const char *const state_names[] = { "idle", "starting", "listening", "thinking", "speaking", "error" };

enum play_kind { PK_NONE, PK_EARCON, PK_TTS, PK_ANNOUNCE, PK_MEDIA, PK_RING };

static enum state state;
static uint64_t deadline;            /* monotonic ms, 0 = none */

static unsigned play_job;
static enum play_kind play_kind;
static int media_announcing;
static int announce_then_listen;

static int expect_run_start;         /* request sent, its RUN_START not seen yet */
static int discard_events;           /* run aborted locally: ignore what is left of it */
static int tts_end_seen;
static int stream_started;           /* response already playing from the early URL */
static char stream_url[1024];
static char conversation_id[128];
static uint64_t conversation_expires;
static int continue_conversation;

static int mic_muted;                /* microphones off: mute button or Home Assistant's switch */
static int mute_want = -1;           /* state Home Assistant asked buttond for; -1: none pending */
static uint64_t mute_want_until;
static int audio_muted, volume = -1; /* speaker, from audioCtrl */
static int ringing, ring_pending;
static uint64_t ring_until;
static int asr_ready;
static const struct wake_word *active_ww = &wake_words[0];
static char led_current[48];
static uint64_t t_wake, t_request;   /* latency bookkeeping */
static volatile sig_atomic_t quit;

/* ---- util ---------------------------------------------------------------- */

int log_level = LOG_INFO_;
static int log_syslog;

void log_init(int use_syslog)
{
    log_syslog = use_syslog;
    if (use_syslog)
        openlog("echo-satellite", LOG_PID, LOG_DAEMON);
}

void logmsg(int level, const char *fmt, ...)
{
    static const int prio[] = { LOG_ERR, LOG_WARNING, LOG_INFO, LOG_DEBUG };
    static const char tag[] = "EWID";
    va_list ap;

    if (level > log_level)
        return;
    va_start(ap, fmt);
    if (log_syslog) {
        vsyslog(prio[level], fmt, ap);
    } else {
        char line[1400];
        struct timespec ts;
        /* One write per line: the player thread logs too. */
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int n = snprintf(line, sizeof(line), "%6ld.%03ld %c ", (long)ts.tv_sec, ts.tv_nsec / 1000000, tag[level]);
        n += vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
        if (n > (int)sizeof(line) - 2)
            n = sizeof(line) - 2;
        line[n++] = '\n';
        if (write(2, line, (size_t)n) < 0) {
        }
    }
    va_end(ap);
}

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void strcopy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* ---- configuration ------------------------------------------------------- */

enum { T_STR, T_INT, T_DBL };
#define S(k) { #k, T_STR, cfg.k, sizeof(cfg.k) }
#define I(k) { #k, T_INT, &cfg.k, 0 }
static const struct {
    const char *key;
    int type;
    void *ptr;
    size_t cap;
} options[] = {
    S(name), S(friendly_name), S(iface), S(mac), I(port), S(allow), S(mdns_file),
    S(mic_shm), S(pulse_server), S(pulse_sink), I(tts_rate), I(tls_insecure),
    S(locale), S(wake_word), S(state_file), S(model_dir), S(asr_restart_cmd), I(asr_runtime_switch),
    I(mic_gain_db), I(noise_suppression), I(auto_gain), { "volume_multiplier", T_DBL, &cfg.volume_multiplier, 0 },
    I(start_timeout_ms), I(listen_timeout_ms), I(think_timeout_ms), I(conversation_timeout_s),
    I(ring_seconds), I(ring_gap_ms),
    S(led_idle), S(led_wake), S(led_listening), S(led_thinking),
    S(led_speaking), S(led_error), S(led_timer), S(led_muted),
    S(sound_wake), S(sound_done), S(sound_error), S(sound_timer), S(sound_mute), S(sound_unmute),
};
#undef S
#undef I

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

static int config_set(char *line, const char *where)
{
    char *eq = strchr(line, '=');
    if (!eq) {
        loge("%s: expected key = value: %s", where, line);
        return -1;
    }
    *eq = 0;
    char *key = trim(line), *val = trim(eq + 1);
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        if (strcmp(options[i].key, key))
            continue;
        if (options[i].type == T_STR)
            strcopy(options[i].ptr, options[i].cap, val);
        else if (options[i].type == T_INT)
            *(int *)options[i].ptr = atoi(val);
        else
            *(double *)options[i].ptr = atof(val);
        return 0;
    }
    loge("%s: unknown option \"%s\"", where, key);
    return -1;
}

static int config_load(const char *path, int must_exist)
{
    char line[512];
    FILE *f = fopen(path, "r");
    int rc = 0;

    if (!f) {
        if (must_exist)
            loge("%s: %s", path, strerror(errno));
        return must_exist ? -1 : 0;
    }
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s && *s != '#' && config_set(s, path) < 0)
            rc = -1;
    }
    fclose(f);
    return rc;
}

static int read_mac(const char *iface, char *out)
{
    char path[96], buf[32] = "";
    FILE *f;

    snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);
    if (!(f = fopen(path, "r")))
        return -1;
    if (!fgets(buf, sizeof(buf), f))
        buf[0] = 0;
    fclose(f);
    char *s = trim(buf);
    if (strlen(s) != 17 || !strcmp(s, "00:00:00:00:00:00"))
        return -1;
    for (int i = 0; i < 17; i++)
        out[i] = (char)toupper((unsigned char)s[i]);
    out[17] = 0;
    return 0;
}

static void config_finish(void)
{
    if (!cfg.name[0]) {
        char host[64] = "";
        size_t n = 0;
        gethostname(host, sizeof(host) - 1);
        for (const char *p = host; *p && *p != '.' && n < sizeof(cfg.name) - 1; p++)
            cfg.name[n++] = isalnum((unsigned char)*p) ? (char)tolower((unsigned char)*p) : '-';
        cfg.name[n] = 0;
        if (!n || !strcmp(cfg.name, "localhost") || !strcmp(cfg.name, "-none-"))
            strcopy(cfg.name, sizeof(cfg.name), "echo");
    }
    /* Home Assistant identifies the device by this address. */
    if (!cfg.mac[0] && read_mac(cfg.iface, cfg.mac) < 0) {
        logw("no MAC address on %s; set \"mac\" in the configuration", cfg.iface);
        strcopy(cfg.mac, sizeof(cfg.mac), "02:EC:40:00:00:01");
    }
    if (cfg.tts_rate < 8000 || cfg.tts_rate > 48000)
        cfg.tts_rate = 48000;
}

/*
 * Announce the device the way ESPHome does, so Home Assistant discovers it.
 * avahi-daemon picks the file up by itself.
 */
static void mdns_publish(void)
{
    char want[1024], have[1024] = "", name[400] = "", mac[13] = "";
    size_t n = 0;
    FILE *f;

    if (!cfg.mdns_file[0])
        return;
    for (const char *p = cfg.friendly_name; *p && n < sizeof(name) - 6; p++) {
        const char *esc = *p == '&' ? "&amp;" : *p == '<' ? "&lt;" : *p == '>' ? "&gt;" : NULL;
        if (esc)
            n += (size_t)sprintf(name + n, "%s", esc);
        else
            name[n++] = *p;
    }
    name[n] = 0;
    n = 0;
    for (const char *p = cfg.mac; *p && n < 12; p++)
        if (*p != ':')
            mac[n++] = (char)tolower((unsigned char)*p);
    snprintf(want, sizeof(want),
             "<?xml version=\"1.0\" standalone='no'?>\n"
             "<!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">\n"
             "<!-- written by echo-satellite -->\n"
             "<service-group>\n"
             "  <name>%s</name>\n"
             "  <service>\n"
             "    <type>_esphomelib._tcp</type>\n"
             "    <port>%d</port>\n"
             "    <txt-record>mac=%s</txt-record>\n"
             "    <txt-record>version=" ESPHOME_VERSION "</txt-record>\n"
             "    <txt-record>friendly_name=%s</txt-record>\n"
             "    <txt-record>network=wifi</txt-record>\n"
             "  </service>\n"
             "</service-group>\n",
             cfg.name, cfg.port, mac, name);
    if ((f = fopen(cfg.mdns_file, "r"))) {
        n = fread(have, 1, sizeof(have) - 1, f);
        have[n] = 0;
        fclose(f);
    }
    if (!strcmp(want, have))
        return;
    if (!(f = fopen(cfg.mdns_file, "w"))) {
        logw("%s: %s", cfg.mdns_file, strerror(errno));
        return;
    }
    fputs(want, f);
    fclose(f);
    logi("mDNS service written to %s", cfg.mdns_file);
}

static const struct wake_word *find_wake_word(const char *id)
{
    for (int i = 0; i < n_wake_words; i++)
        if (!strcasecmp(wake_words[i].id, id))
            return &wake_words[i];
    return NULL;
}

static void state_load(void)
{
    char line[64] = "";
    const struct wake_word *w = find_wake_word(cfg.wake_word);
    FILE *f = fopen(cfg.state_file, "r");

    if (!w)
        logw("unknown wake word \"%s\", using \"%s\"", cfg.wake_word, active_ww->id);
    else
        active_ww = w;
    if (!f)
        return;
    if (fgets(line, sizeof(line), f) && !strncmp(line, "wake_word=", 10) &&
        (w = find_wake_word(trim(line + 10))))
        active_ww = w;
    fclose(f);
}

static void state_save(void)
{
    FILE *f = fopen(cfg.state_file, "w");
    if (!f) {
        logw("%s: %s", cfg.state_file, strerror(errno));
        return;
    }
    fprintf(f, "wake_word=%s\n", active_ww->id);
    fclose(f);
}

/* ---- LED ring ------------------------------------------------------------ */

static void led_update(void)
{
    const char *p;

    if (ringing) {
        p = cfg.led_timer;
    } else {
        switch (state) {
        case ST_STARTING:  p = cfg.led_wake; break;
        case ST_LISTENING: p = cfg.led_listening; break;
        case ST_THINKING:  p = cfg.led_thinking; break;
        case ST_SPEAKING:  p = cfg.led_speaking; break;
        case ST_ERROR:     p = cfg.led_error; break;
        default:           p = mic_muted ? cfg.led_muted : cfg.led_idle; break;
        }
    }
    if (!*p || !strcmp(p, led_current))
        return;
    strcopy(led_current, sizeof(led_current), p);
    bus_set_str(SVC_LEDD, "Pattern", p);
}

/* ---- playback bookkeeping ------------------------------------------------ */

static void play_start(enum play_kind kind, const char *const *items, int n, int loop)
{
    play_job = player_play(items, n, loop, cfg.ring_gap_ms);
    play_kind = kind;
}

/* The current job is over (finished, failed or stopped by us): tell whoever waits for it. */
static void playback_ended(int ok)
{
    enum play_kind kind = play_kind;

    play_kind = PK_NONE;
    play_job = 0;
    switch (kind) {
    case PK_TTS:
    case PK_ANNOUNCE:
        api_va_announce_finished(ok);   /* Home Assistant waits for this after TTS too */
        break;
    case PK_MEDIA:
        api_media_state();
        break;
    default:
        break;
    }
}

static void set_ringing(int on)
{
    if (on == ringing)
        return;
    ringing = on;
    api_ring_state();
}

static void stop_playback(void)
{
    if (play_kind == PK_NONE)
        return;
    if (play_kind == PK_RING)
        set_ringing(0);
    player_stop();
    playback_ended(1);
}

/* Short local sound. Never interrupts speech, media or a ringing timer. */
static void play_earcon(const char *path)
{
    if (!path[0] || (play_kind != PK_NONE && play_kind != PK_EARCON))
        return;
    play_start(PK_EARCON, &path, 1, 0);
}

static void ring_start(void)
{
    const char *item = cfg.sound_timer;

    ring_pending = 0;
    stop_playback();
    set_ringing(1);
    ring_until = now_ms() + (uint64_t)cfg.ring_seconds * 1000;
    if (item[0])
        play_start(PK_RING, &item, 1, 1);
    logi("timer: ringing");
    led_update();
}

static void ring_stop(void)
{
    if (!ringing)
        return;
    if (play_kind == PK_RING)
        stop_playback();
    set_ringing(0);
    logi("timer: silenced");
    led_update();
}

/* ---- voice state machine ------------------------------------------------- */

static void set_state(enum state s)
{
    if (s != state)
        logi("state: %s -> %s", state_names[state], state_names[s]);
    state = s;
    switch (s) {
    case ST_STARTING:  deadline = now_ms() + (uint64_t)cfg.start_timeout_ms; break;
    case ST_LISTENING: deadline = now_ms() + (uint64_t)cfg.listen_timeout_ms; break;
    case ST_THINKING:  deadline = now_ms() + (uint64_t)cfg.think_timeout_ms; break;
    case ST_ERROR:     deadline = now_ms() + 1500; break;
    default:           deadline = 0; break;
    }
    if (s == ST_IDLE && ring_pending)
        ring_start();
    led_update();
}

/* Tell Home Assistant to drop the run in progress. Nothing more will be
 * played for it, whatever events are still on their way. */
static void abort_run(void)
{
    api_va_request(0, "", "");
    discard_events = 1;
    expect_run_start = 0;
    continue_conversation = 0;
}

static void fail(const char *why)
{
    logw("request failed: %s", why);
    set_state(ST_ERROR);
    play_earcon(cfg.sound_error);
}

static int response_unfinished(void)
{
    /* A streamed response is playing while Home Assistant still generates it. */
    return state == ST_SPEAKING && play_kind == PK_TTS && !tts_end_seen;
}

static void start_pipeline(int woken, uint64_t seq)
{
    if (!api_voice_client()) {
        fail("Home Assistant is not connected");
        return;
    }
    if (mic_open() < 0) {
        fail("microphone buffer not available");
        return;
    }
    if (woken)
        mic_seek(seq);
    else
        mic_seek_now();
    if (conversation_id[0] && now_ms() > conversation_expires)
        conversation_id[0] = 0;

    expect_run_start = 1;
    discard_events = 0;
    tts_end_seen = 0;
    stream_started = 0;
    stream_url[0] = 0;
    continue_conversation = 0;
    t_request = now_ms();
    if (!woken)
        t_wake = t_request;
    api_va_request(1, conversation_id, woken ? active_ww->phrase : "");
    set_state(ST_STARTING);
    play_earcon(cfg.sound_wake);
}

static void speak(const char *url)
{
    if (state != ST_STARTING && state != ST_LISTENING && state != ST_THINKING)
        return;
    stop_playback();
    logi("timing: response URL %llu ms after the trigger", (unsigned long long)(now_ms() - t_wake));
    play_start(PK_TTS, &url, 1, 0);
    set_state(ST_SPEAKING);
}

/* Stop a response or announcement the user does not want to hear out. */
static void cancel_response(void)
{
    if (response_unfinished())
        abort_run();
    continue_conversation = 0;
    announce_then_listen = 0;
    stop_playback();
    set_state(ST_IDLE);
}

static void player_event(unsigned job, int what, int status)
{
    if (job != play_job || play_kind == PK_NONE)
        return;   /* a job we already replaced or stopped */
    if (what == PLAYER_STARTED) {
        if (play_kind == PK_TTS)
            logi("timing: first response audio %llu ms after the trigger",
                 (unsigned long long)(now_ms() - t_wake));
        return;
    }

    enum play_kind kind = play_kind;
    int ok = status != PLAY_FAILED;
    playback_ended(ok);
    switch (kind) {
    case PK_TTS:
        if (ok && continue_conversation && !mic_muted)
            start_pipeline(0, 0);
        else
            set_state(ok ? ST_IDLE : ST_ERROR);
        break;
    case PK_ANNOUNCE:
        if (ok && announce_then_listen && !mic_muted)
            start_pipeline(0, 0);
        else
            set_state(ok ? ST_IDLE : ST_ERROR);
        announce_then_listen = 0;
        break;
    default:
        break;   /* media, earcon, ring: nothing waits for them */
    }
}

static void pump_microphone(void)
{
    int16_t buf[1024];

    /* Several chunks per pass: after a wake word the ring already holds the
     * start of the command, and Home Assistant should get it at once. */
    for (int i = 0; i < 32 && state == ST_LISTENING && api_va_ready(); i++) {
        int n = mic_read(buf, 1024);
        if (n <= 0)
            break;
        api_va_audio(buf, n);
    }
}

static void on_deadline(void)
{
    switch (state) {
    case ST_STARTING:
        abort_run();
        fail("Home Assistant did not accept the request");
        break;
    case ST_LISTENING:
        abort_run();
        fail("no end of speech detected");
        break;
    case ST_THINKING:
        abort_run();
        fail("no response from Home Assistant");
        break;
    case ST_ERROR:
        set_state(ST_IDLE);
        break;
    default:
        deadline = 0;
        break;
    }
}

/* ---- callbacks from api.c ------------------------------------------------ */

void sat_voice_client_changed(int subscribed)
{
    logi("Home Assistant voice client %s", subscribed ? "subscribed" : "gone");
    if (subscribed)
        return;
    expect_run_start = 0;
    discard_events = 1;
    continue_conversation = 0;
    announce_then_listen = 0;
    if (play_kind == PK_TTS || play_kind == PK_ANNOUNCE)
        stop_playback();
    if (state != ST_IDLE)
        set_state(ST_IDLE);
}

void sat_va_response(int error)
{
    if (state != ST_STARTING)
        return;
    if (error) {
        expect_run_start = 0;
        fail("Home Assistant refused the request");
        return;
    }
    logi("timing: request accepted after %llu ms", (unsigned long long)(now_ms() - t_request));
    set_state(ST_LISTENING);
    pump_microphone();
}

static const char *kv_get(const struct va_kv *kv, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(kv[i].name, name))
            return kv[i].value;
    return "";
}

void sat_va_event(int type, const struct va_kv *kv, int n)
{
    const char *s;

    if (discard_events) {
        logd("event %d ignored: run was aborted", type);
        return;
    }
    if (expect_run_start) {
        /* Anything else still belongs to the run before this one. */
        if (type != EV_RUN_START && type != EV_ERROR) {
            logd("event %d ignored: waiting for run start", type);
            return;
        }
        expect_run_start = 0;
    }

    switch (type) {
    case EV_RUN_START:
        /* With streaming TTS the response URL is known before there is any text. */
        strcopy(stream_url, sizeof(stream_url), kv_get(kv, n, "url"));
        break;
    case EV_STT_VAD_END:
    case EV_STT_END:
        if (type == EV_STT_END)
            logi("heard: \"%s\"", kv_get(kv, n, "text"));
        if (state == ST_LISTENING || state == ST_STARTING) {
            logi("timing: end of speech %llu ms after the trigger", (unsigned long long)(now_ms() - t_wake));
            set_state(ST_THINKING);
            play_earcon(cfg.sound_done);
        }
        break;
    case EV_INTENT_PROGRESS:
        if (!strcmp(kv_get(kv, n, "tts_start_streaming"), "1") && stream_url[0] && !stream_started) {
            stream_started = 1;
            speak(stream_url);
        }
        break;
    case EV_INTENT_END:
        strcopy(conversation_id, sizeof(conversation_id), kv_get(kv, n, "conversation_id"));
        conversation_expires = now_ms() + (uint64_t)cfg.conversation_timeout_s * 1000;
        continue_conversation = !strcmp(kv_get(kv, n, "continue_conversation"), "1");
        break;
    case EV_TTS_START:
        logi("response: \"%s\"", kv_get(kv, n, "text"));
        break;
    case EV_TTS_END:
        tts_end_seen = 1;
        s = kv_get(kv, n, "url");
        if (!stream_started && s[0])
            speak(s);
        break;
    case EV_RUN_END:
        /* Without a response ("never mind") there is nothing to wait for. */
        if (state == ST_STARTING || state == ST_LISTENING || state == ST_THINKING)
            set_state(ST_IDLE);
        break;
    case EV_ERROR:
        s = kv_get(kv, n, "code");
        if (!strcmp(s, "wake-word-timeout") || !strcmp(s, "wake_word_detection_aborted") ||
            !strcmp(s, "no_wake_word"))
            break;
        logw("Home Assistant: %s: %s", s, kv_get(kv, n, "message"));
        if (state == ST_IDLE || state == ST_ERROR || state == ST_SPEAKING)
            break;   /* a response that is already playing ends on its own */
        if (!strcmp(s, "stt-no-text-recognized"))
            set_state(ST_IDLE);   /* silence after the wake word is not an error worth a red ring */
        else
            fail(s);
        break;
    default:
        break;
    }
}

void sat_va_timer(int event, const char *id, const char *name, unsigned total, unsigned left, int active)
{
    static const char *const names[] = { "started", "updated", "cancelled", "finished" };

    logi("timer %s \"%s\": %s, %u of %u s left%s", id, name,
         event >= 0 && event <= 3 ? names[event] : "?", left, total, active ? "" : " (paused)");
    if (event != 3)
        return;
    if (state == ST_IDLE)
        ring_start();
    else
        ring_pending = 1;   /* rings once the conversation is over */
}

void sat_va_announce(const char *media_id, const char *text, const char *preannounce, int start_conversation)
{
    const char *items[2];
    int n = 0;

    logi("announcement: \"%s\"%s", text, start_conversation ? " (then listen)" : "");
    /* Policy: an announcement wins over a request in progress. */
    if (state == ST_STARTING || state == ST_LISTENING || state == ST_THINKING || response_unfinished())
        abort_run();
    if (ringing)
        ring_pending = 1;
    stop_playback();

    if (preannounce[0])
        items[n++] = preannounce;
    if (media_id[0])
        items[n++] = media_id;
    announce_then_listen = start_conversation;
    if (!n) {
        api_va_announce_finished(1);
        if (start_conversation && !mic_muted)
            start_pipeline(0, 0);
        else
            set_state(ST_IDLE);
        return;
    }
    play_start(PK_ANNOUNCE, items, n, 0);
    set_state(ST_SPEAKING);
}

const char *sat_wake_word_id(void) { return active_ww->id; }

/*
 * Selecting asrd's wake-word model. asrd loads the model named by the
 * dynconfig value asrd.model when it starts. Its WWModel property switches
 * the model at run time under emulation, but on a real Echo the write was
 * refused (lipcErrInvalidArg). So: store the choice in dynconfig, try the
 * property, and if asrd still runs another model a few seconds later, have
 * it restarted (the init script starts it again).
 */
#define MODEL_CHECK_MS 6000
#define MODEL_MAX_RESTARTS 2
static uint64_t model_check_at;
static int model_restarts;

static void model_store(const char *model)
{
    char path[256];
    pid_t pid;
    int status;

    if (!cfg.model_dir[0])
        return;
    snprintf(path, sizeof(path), "%s/%s/pryon.manifest", cfg.model_dir, model);
    if (access(path, R_OK) != 0) {
        logw("asr: %s: %s", path, strerror(errno));
        return;
    }
    pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
        }
        execl("/usr/local/bin/set-dynconf-value", "set-dynconf-value", "asrd.model", path, (char *)NULL);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        logw("asr: could not store the model with set-dynconf-value");
    else
        logd("asr: stored asrd.model = %s", path);
}

static void asr_restart(const char *cur, const char *want)
{
    if (!cfg.asr_restart_cmd[0] || model_restarts >= MODEL_MAX_RESTARTS) {
        logw("asr: asrd runs model %s, wanted %s", cur, want);
        return;
    }
    model_restarts++;
    logi("asr: restarting asrd to load %s", want);
    asr_ready = 0;
    if (system(cfg.asr_restart_cmd) != 0)
        logw("asr: \"%s\" failed", cfg.asr_restart_cmd);
}

static void asr_setup(void)
{
    char want[64], cur[64] = "";

    /* Broadcast is off after every start of asrd. */
    snprintf(want, sizeof(want), "%s/%s", cfg.locale, active_ww->model);
    model_check_at = 0;
    asr_ready = 1;
    if (bus_get_str(SVC_ASR, "WWModel", cur, sizeof(cur)) == 0) {
        if (!strcmp(cur, want)) {
            model_restarts = 0;
        } else {
            logi("asr: wake-word model %s -> %s", cur, want);
            model_store(want);
            if (cfg.asr_runtime_switch) {
                bus_set_str(SVC_ASR, "WWModel", want);
                model_check_at = now_ms() + MODEL_CHECK_MS;
            } else {
                asr_restart(cur, want);
            }
        }
    }
    if (asr_ready) {
        bus_set_str(SVC_ASR, "Broadcast", "Enabled");
        bus_set_str(SVC_ASR, "StopBroadcast", "Enabled");
    }
}

/* Did the run-time switch take effect? */
static void model_check(void)
{
    char want[64], cur[64] = "";

    model_check_at = 0;
    snprintf(want, sizeof(want), "%s/%s", cfg.locale, active_ww->model);
    if (bus_get_str(SVC_ASR, "WWModel", cur, sizeof(cur)) != 0)
        return;                 /* asrd is away; asr_setup() runs when it is back */
    if (!strcmp(cur, want))
        model_restarts = 0;
    else
        asr_restart(cur, want);
}

void sat_set_wake_word(const char *id)
{
    const struct wake_word *w = find_wake_word(id);

    if (!w) {
        logw("Home Assistant selected unknown wake word \"%s\"", id);
        return;
    }
    if (w == active_ww)
        return;
    logi("wake word: %s", w->phrase);
    active_ww = w;
    model_restarts = 0;
    state_save();
    if (asr_ready)
        asr_setup();
}

int sat_ringing(void) { return ringing; }

void sat_stop_ringing(void)
{
    logi("timer: stop requested by Home Assistant");
    ring_pending = 0;   /* also a timer that waits for the conversation to end */
    ring_stop();
}

int sat_media_state(float *vol, int *muted)
{
    *vol = volume < 0 ? 0.0f : (float)volume / 100.0f;
    *muted = audio_muted;
    if (play_kind != PK_MEDIA)
        return 1;                      /* MEDIA_PLAYER_STATE_IDLE */
    return media_announcing ? 4 : 2;   /* ANNOUNCING : PLAYING */
}

static void set_volume(int v)
{
    if (v < 0)
        v = 0;
    if (v > 100)
        v = 100;
    bus_set_int(SVC_AUDIOD, "MainVolume", v);   /* audioCtrl answers with MainVolumeChanged */
}

void sat_media_command(int has_command, int command, int has_volume, float vol,
                       int has_url, const char *url, int announcement)
{
    if (has_volume)
        set_volume((int)lroundf(vol * 100.0f));
    if (has_command) {
        switch (command) {
        case 1:   /* PAUSE: there is no resume, so it stops */
        case 2:   /* STOP */
            if (play_kind == PK_MEDIA)
                stop_playback();
            break;
        case 3: bus_set_int(SVC_AUDIOD, "Mute", 1); break;
        case 4: bus_set_int(SVC_AUDIOD, "Mute", 0); break;
        case 6: set_volume((volume < 0 ? 40 : volume) + 5); break;
        case 7: set_volume((volume < 0 ? 40 : volume) - 5); break;
        default: logd("media: command %d not supported", command); break;
        }
    }
    if (has_url && url[0]) {
        if (state != ST_IDLE || ringing) {
            logw("media: busy, not playing %s", url);
            return;
        }
        stop_playback();
        media_announcing = announcement;
        play_start(PK_MEDIA, &url, 1, 0);
        logi("media: %s", url);
        api_media_state();
    }
}

/* ---- callbacks from bus.c ------------------------------------------------ */

void sat_keyword(uint64_t stream_start, uint64_t ww_begin, uint64_t ww_end)
{
    logi("wake word \"%s\" at sample %llu (%llu ms long)", active_ww->phrase,
         (unsigned long long)(stream_start + ww_begin), (unsigned long long)((ww_end - ww_begin) / 16));
    if (mic_muted) {
        /* The mute line cuts the microphones, so this should not happen.
         * Whatever let it through, nothing leaves the device while muted. */
        logw("wake word ignored: microphones are muted");
        return;
    }
    if (state == ST_STARTING || state == ST_LISTENING)
        return;   /* already listening */
    t_wake = now_ms();
    ring_stop();
    if (state == ST_THINKING || response_unfinished())
        abort_run();
    announce_then_listen = 0;
    stop_playback();
    /* Stream from the end of the wake word, so the command needs no pause. */
    start_pipeline(1, stream_start + ww_end);
}

void sat_local_command(const char *cmd)
{
    logi("local command \"%s\"", cmd);
    if (strcmp(cmd, "STOP"))
        return;
    /* "Computer, stop": the wake word already silenced whatever was playing
     * and opened a request. Drop that request, nothing was asked. */
    ring_stop();
    if (state == ST_STARTING || state == ST_LISTENING || state == ST_THINKING) {
        abort_run();
        set_state(ST_IDLE);
    } else if (state == ST_SPEAKING) {
        cancel_response();
    } else if (play_kind == PK_MEDIA) {
        stop_playback();
    }
}

void sat_asr_ready(int ready)
{
    logi("asr: %s", ready ? "ready" : "not ready");
    asr_ready = 0;
    if (ready)
        asr_setup();
}

static void action_button(void)
{
    if (ringing) {
        ring_stop();
        return;
    }
    switch (state) {
    case ST_IDLE:
        if (play_kind == PK_MEDIA)
            stop_playback();
        else if (!mic_muted)
            start_pipeline(0, 0);
        break;
    case ST_STARTING:
    case ST_LISTENING:
    case ST_THINKING:
        abort_run();
        set_state(ST_IDLE);
        break;
    case ST_SPEAKING:
        cancel_response();
        break;
    case ST_ERROR:
        set_state(ST_IDLE);
        break;
    }
}

static void set_mic_muted(int muted, int announce)
{
    if (muted == mic_muted)
        return;
    mic_muted = muted;
    logi("microphones %s", muted ? "off" : "on");
    if (muted && (state == ST_STARTING || state == ST_LISTENING)) {
        abort_run();
        set_state(ST_IDLE);
    }
    if (announce)
        play_earcon(muted ? cfg.sound_mute : cfg.sound_unmute);
    led_update();
    api_mute_state();
}

int sat_mic_muted(void) { return mic_muted; }

/*
 * Home Assistant's "Mute" switch. The mute state belongs to buttond: it
 * drives the line that cuts the microphones and remembers the state. Its
 * fakeButton property takes "<button> <state>" and acts as if the button had
 * been pressed, so the change comes back as the same "button" signal.
 */
#define MUTE_WAIT_MS 2000

void sat_mute_command(int muted)
{
    logi("Home Assistant: microphones %s", muted ? "off" : "on");
    if (muted == mic_muted) {
        mute_want = -1;
        api_mute_state();
        return;
    }
    mute_want = muted;
    mute_want_until = now_ms() + MUTE_WAIT_MS;
    bus_set_str(SVC_BUTTOND, "fakeButton", muted ? "mute enabled" : "mute disabled");
}

void sat_button(const char *name, int st)
{
    logd("button %s %d", name, st);
    if (!strcmp(name, "uber")) {
        if (st == BTN_DOWN)
            action_button();
    } else if (!strcmp(name, "mute")) {
        if (st == BTN_MUTE_ON || st == BTN_MUTE_OFF) {
            /* No sound when Home Assistant asked for it: the ring shows it. */
            int from_ha = mute_want == (st == BTN_MUTE_ON);
            mute_want = -1;
            set_mic_muted(st == BTN_MUTE_ON, !from_ha);
        }
    }
    /* "volume": audioCtrl handles the ring itself and reports the result. */
}

void sat_volume_changed(int v)
{
    if (v == volume)
        return;
    volume = v;
    api_media_state();
}

void sat_audio_mute_changed(int muted)
{
    audio_muted = !!muted;
    api_media_state();
}

/* ---- main ---------------------------------------------------------------- */

static void on_signal(int sig)
{
    (void)sig;
    quit = 1;
}

/* Services that were not up yet when we started. */
static void probe_services(void)
{
    char s[16];
    int v;

    if (!asr_ready && bus_get_str(SVC_ASR, "Ready", s, sizeof(s)) == 0 && !strcmp(s, "Yes"))
        asr_setup();
    if (volume < 0 && bus_get_int(SVC_AUDIOD, "MainVolume", &v) == 0) {
        volume = v;
        if (bus_get_int(SVC_AUDIOD, "Mute", &v) == 0)
            audio_muted = !!v;
        api_media_state();
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-c config] [-o key=value]... [-v] [-s]\n"
            "  -c file   configuration file (default /etc/echo-satellite.conf, optional)\n"
            "  -o k=v    set one option, overrides the file\n"
            "  -v        debug logging\n"
            "  -s        log to syslog instead of stderr\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *conf = "/etc/echo-satellite.conf";
    char *overrides[32];
    int n_over = 0, use_syslog = 0, conf_given = 0, opt, v;
    struct pollfd fds[16];
    uint64_t next_house = 0, next_probe = 0;

    while ((opt = getopt(argc, argv, "c:o:vsh")) != -1) {
        switch (opt) {
        case 'c': conf = optarg; conf_given = 1; break;
        case 'o': if (n_over < 32) overrides[n_over++] = optarg; break;
        case 'v': log_level = LOG_DEBUG_; break;
        case 's': use_syslog = 1; break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    log_init(use_syslog);
    if (config_load(conf, conf_given) < 0)
        return 2;
    for (int i = 0; i < n_over; i++)
        if (config_set(overrides[i], "-o") < 0)
            return 2;
    config_finish();
    state_load();
    logi("echo-satellite " SAT_VERSION ", wake word \"%s\"", active_ww->phrase);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (bus_init() < 0 || api_init() < 0)
        return 1;
    mdns_publish();
    int player_fd = player_init();
    if (player_fd < 0) {
        loge("cannot start the playback thread");
        return 1;
    }
    mic_open();   /* retried on the first request if the buffer is not there yet */

    if (bus_get_int(SVC_BUTTOND, "muteState", &v) == 0)
        mic_muted = v == BTN_MUTE_ON;
    led_update();

    while (!quit) {
        int n_api = api_poll_prepare(fds, 14), n = n_api, timeout = 1000;
        int bus = n, player;
        uint64_t now = now_ms();

        fds[n].fd = bus_fd();
        fds[n].events = POLLIN;
        fds[n++].revents = 0;
        player = n;
        fds[n].fd = player_fd;
        fds[n].events = POLLIN;
        fds[n++].revents = 0;

        if (state == ST_LISTENING)
            timeout = 32;   /* 512 samples, the chunk size ESPHome devices use */
        if (deadline && deadline < now + (uint64_t)timeout)
            timeout = deadline > now ? (int)(deadline - now) : 0;
        if (ringing && ring_until < now + (uint64_t)timeout)
            timeout = ring_until > now ? (int)(ring_until - now) : 0;

        if (poll(fds, (nfds_t)n, timeout) < 0 && errno != EINTR) {
            loge("poll: %s", strerror(errno));
            break;
        }
        api_poll_handle(fds, n_api);
        if ((fds[bus].revents & (POLLHUP | POLLERR)) || bus_dispatch() < 0) {
            loge("lost the D-Bus connection");
            break;
        }
        if (fds[player].revents) {
            unsigned job;
            int what, status;
            while (player_next_event(&job, &what, &status))
                player_event(job, what, status);
        }
        if (state == ST_LISTENING)
            pump_microphone();

        now = now_ms();
        if (deadline && now >= deadline)
            on_deadline();
        if (ringing && now >= ring_until)
            ring_stop();
        if (mute_want >= 0 && now >= mute_want_until) {
            logw("buttond did not turn the microphones %s", mute_want ? "off" : "on");
            mute_want = -1;
            api_mute_state();   /* the switch in Home Assistant goes back */
        }
        if (now >= next_house) {
            next_house = now + 1000;
            api_housekeeping();
        }
        if (model_check_at && now >= model_check_at && state == ST_IDLE)
            model_check();
        if (now >= next_probe && state == ST_IDLE) {   /* the probes can block briefly */
            next_probe = now + 10000;
            probe_services();
        }
    }

    /* Leave the device quiet and dark. */
    player_stop();
    strcopy(led_current, sizeof(led_current), "");
    if (cfg.led_idle[0])
        bus_set_str(SVC_LEDD, "Pattern", cfg.led_idle);
    logi("exiting");
    return quit ? 0 : 1;
}
