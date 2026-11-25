/*
 * Playback thread. Plays WAV (PCM 16-bit) from http(s) URLs or local files
 * through PulseAudio, while it downloads, so streaming TTS starts as soon as
 * the first audio arrives.
 *
 * One job at a time: a job is a short list of items played in order,
 * optionally looped (timer ringing). Starting a job or calling player_stop()
 * cancels the running one. Results come back to the main loop as events,
 * signalled through a pipe.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <pulse/error.h>
#include <pulse/simple.h>

#include "sat.h"

#define ITEM_MAX 1100
#define HDR_MAX 4096      /* WAV header incl. extra chunks; ffmpeg writes ~78 bytes */
#define WRITE_CHUNK 4096  /* bytes per pa_simple_write: bounds the stop latency */

struct job {
    unsigned id;
    int n, loop, gap_ms;
    char items[PLAYER_MAX_ITEMS][ITEM_MAX];
};

struct event {
    unsigned job;
    int what, status;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static struct job pending;
static int have_pending;
static unsigned next_id = 1;
static volatile unsigned wanted;   /* id of the job that should be playing, 0 = none */

static struct event events[32];
static int ev_head, ev_count;
static int ev_pipe[2] = { -1, -1 };

/* Thread-local state of the item being played. */
struct stream {
    unsigned job;
    pa_simple *pa;
    pa_sample_spec spec;
    uint8_t hdr[HDR_MAX];
    size_t hdr_len;
    int hdr_done;
    uint64_t data_left;       /* bytes of the data chunk still expected; ~0 = until EOF */
    uint8_t carry[8];         /* partial frame between writes */
    size_t carry_len;
    int started_sent;
    int failed;
};

static void post(unsigned job, int what, int status)
{
    pthread_mutex_lock(&lock);
    if (ev_count < (int)(sizeof(events) / sizeof(events[0]))) {
        struct event *e = &events[(ev_head + ev_count++) % (sizeof(events) / sizeof(events[0]))];
        e->job = job;
        e->what = what;
        e->status = status;
    }
    pthread_mutex_unlock(&lock);
    if (write(ev_pipe[1], "e", 1) < 0 && errno != EAGAIN)
        loge("player: event pipe: %s", strerror(errno));
}

static int cancelled(const struct stream *s) { return wanted != s->job; }

static void close_pa(struct stream *s, int drain)
{
    int err;

    if (!s->pa)
        return;
    if (drain)
        pa_simple_drain(s->pa, &err);
    else
        pa_simple_flush(s->pa, &err);
    pa_simple_free(s->pa);
    s->pa = NULL;
}

static int open_pa(struct stream *s, const pa_sample_spec *spec)
{
    pa_buffer_attr attr;
    int err;

    if (s->pa && pa_sample_spec_equal(&s->spec, spec))
        return 0;
    close_pa(s, 1);

    /* Small buffer: this is speech, a late start is worse than a rare gap. */
    attr.maxlength = (uint32_t)-1;
    attr.tlength = (uint32_t)pa_usec_to_bytes(120 * 1000, spec);
    attr.prebuf = (uint32_t)pa_usec_to_bytes(30 * 1000, spec);
    attr.minreq = (uint32_t)-1;
    attr.fragsize = (uint32_t)-1;

    s->pa = pa_simple_new(cfg.pulse_server[0] ? cfg.pulse_server : NULL, "echo-satellite",
                          PA_STREAM_PLAYBACK, cfg.pulse_sink[0] ? cfg.pulse_sink : NULL,
                          "voice", spec, NULL, &attr, &err);
    if (!s->pa) {
        loge("player: cannot open PulseAudio stream: %s", pa_strerror(err));
        return -1;
    }
    s->spec = *spec;
    return 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

/*
 * Parse the RIFF header once enough of it has arrived.
 * Returns the offset of the first sample, 0 if more bytes are needed,
 * -1 if this is not PCM WAV we can play.
 */
static int parse_header(struct stream *s)
{
    const uint8_t *h = s->hdr;
    size_t n = s->hdr_len, pos = 12;
    int have_fmt = 0;
    pa_sample_spec spec = { .format = PA_SAMPLE_S16LE };

    if (n < 12)
        return 0;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        loge("player: not a WAV file (starts with %02x %02x %02x %02x)", h[0], h[1], h[2], h[3]);
        return -1;
    }
    for (;;) {
        if (pos + 8 > n)
            return n >= HDR_MAX ? -1 : 0;
        uint32_t size = le32(h + pos + 4);
        if (!memcmp(h + pos, "data", 4)) {
            if (!have_fmt)
                return -1;
            /* Streamed files have size 0 or 0xffffffff: play until the end. */
            s->data_left = (size == 0 || size >= 0xffffff00u) ? ~(uint64_t)0 : size;
            if (open_pa(s, &spec) < 0)
                return -1;
            return (int)(pos + 8);
        }
        if (!memcmp(h + pos, "fmt ", 4)) {
            if (pos + 8 + 16 > n)
                return n >= HDR_MAX ? -1 : 0;
            uint32_t tag = le16(h + pos + 8), bits = le16(h + pos + 22);
            spec.channels = (uint8_t)le16(h + pos + 10);
            spec.rate = le32(h + pos + 12);
            if ((tag != 1 && tag != 0xfffe) || bits != 16 || !pa_sample_spec_valid(&spec)) {
                loge("player: unsupported WAV: format tag %u, %u bit, %u channels, %u Hz",
                     tag, bits, spec.channels, spec.rate);
                return -1;
            }
            have_fmt = 1;
        }
        pos += 8 + (size_t)size + (size & 1);
        if (pos > HDR_MAX - 8 && pos > n)
            return -1;   /* a chunk before the audio that does not fit our buffer */
    }
}

static int write_pcm(struct stream *s, const uint8_t *p, size_t len)
{
    size_t frame = pa_frame_size(&s->spec);
    int err;

    if (s->data_left != ~(uint64_t)0) {
        if (len > s->data_left)
            len = (size_t)s->data_left;
        s->data_left -= len;
    }
    while (len) {
        uint8_t tmp[WRITE_CHUNK];
        size_t n = 0;

        if (cancelled(s))
            return -1;
        if (s->carry_len) {
            memcpy(tmp, s->carry, s->carry_len);
            n = s->carry_len;
            s->carry_len = 0;
        }
        size_t take = len < sizeof(tmp) - n ? len : sizeof(tmp) - n;
        memcpy(tmp + n, p, take);
        n += take;
        p += take;
        len -= take;

        size_t whole = n - n % frame;
        if (n > whole) {
            s->carry_len = n - whole;
            memcpy(s->carry, tmp + whole, s->carry_len);
        }
        if (!whole)
            continue;
        if (pa_simple_write(s->pa, tmp, whole, &err) < 0) {
            loge("player: write failed: %s", pa_strerror(err));
            s->failed = 1;
            return -1;
        }
        if (!s->started_sent) {
            s->started_sent = 1;
            post(s->job, PLAYER_STARTED, PLAY_OK);
        }
    }
    return 0;
}

/* Feed bytes of the file as they arrive. Returns -1 to abort the transfer. */
static int feed(struct stream *s, const uint8_t *p, size_t len)
{
    if (cancelled(s))
        return -1;
    if (!s->hdr_done) {
        size_t before = s->hdr_len;
        size_t take = len < HDR_MAX - before ? len : HDR_MAX - before;
        memcpy(s->hdr + before, p, take);
        s->hdr_len += take;
        int off = parse_header(s);
        if (off < 0) {
            s->failed = 1;
            return -1;
        }
        if (off == 0)
            return 0;
        s->hdr_done = 1;
        /* Samples that arrived in the same buffer as the header. */
        size_t in_buf = s->hdr_len - (size_t)off;
        if (write_pcm(s, s->hdr + off, in_buf) < 0)
            return -1;
        p += take;
        len -= take;
        if (!len)
            return 0;
    }
    return write_pcm(s, p, len);
}

static size_t curl_write(char *ptr, size_t size, size_t nmemb, void *user)
{
    return feed(user, (const uint8_t *)ptr, size * nmemb) < 0 ? 0 : size * nmemb;
}

static int curl_progress(void *user, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
    (void)a; (void)b; (void)c; (void)d;
    return cancelled(user);
}

static int is_url(const char *s) { return !strncmp(s, "http://", 7) || !strncmp(s, "https://", 8); }

/* Returns PLAY_OK, PLAY_FAILED or PLAY_CANCELLED. */
static int play_item(struct stream *s, CURL *curl, const char *item)
{
    uint64_t t0 = now_ms();

    s->hdr_len = 0;
    s->hdr_done = 0;
    s->carry_len = 0;
    s->failed = 0;
    logd("player: job %u: %s", s->job, item);

    if (is_url(item)) {
        char errbuf[CURL_ERROR_SIZE] = "";
        if (!curl)
            return PLAY_FAILED;
        curl_easy_setopt(curl, CURLOPT_URL, item);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, s);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, s);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
        CURLcode rc = curl_easy_perform(curl);
        if (cancelled(s))
            return PLAY_CANCELLED;
        if (rc != CURLE_OK && !(rc == CURLE_WRITE_ERROR && s->failed)) {
            loge("player: %s: %s", item, errbuf[0] ? errbuf : curl_easy_strerror(rc));
            return PLAY_FAILED;
        }
    } else {
        uint8_t buf[WRITE_CHUNK];
        size_t n;
        FILE *f = fopen(item, "rb");
        if (!f) {
            loge("player: %s: %s", item, strerror(errno));
            return PLAY_FAILED;
        }
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            if (feed(s, buf, n) < 0)
                break;
        fclose(f);
        if (cancelled(s))
            return PLAY_CANCELLED;
    }
    if (s->failed || !s->hdr_done) {
        if (!s->failed)
            loge("player: %s: no audio received", item);
        return PLAY_FAILED;
    }
    logd("player: job %u: item done after %llu ms", s->job, (unsigned long long)(now_ms() - t0));
    return PLAY_OK;
}

/* Sleep between loop rounds; returns early when the job is cancelled. */
static void gap(const struct stream *s, int ms)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&lock);
    while (!cancelled(s))
        if (pthread_cond_timedwait(&cond, &lock, &ts) == ETIMEDOUT)
            break;
    pthread_mutex_unlock(&lock);
}

static void *player_thread(void *arg)
{
    (void)arg;
    static struct stream s;   /* static: large, and there is exactly one thread */
    CURL *curl = curl_easy_init();

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        /* A streamed answer can pause while the LLM thinks; give up after 30 s of nothing. */
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
        if (cfg.tls_insecure) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        }
    } else {
        loge("player: curl_easy_init failed, URLs cannot be played");
    }

    for (;;) {
        struct job job;
        int status = PLAY_OK;

        pthread_mutex_lock(&lock);
        while (!have_pending)
            pthread_cond_wait(&cond, &lock);
        job = pending;
        have_pending = 0;
        pthread_mutex_unlock(&lock);

        s.job = job.id;
        s.started_sent = 0;
        do {
            for (int i = 0; i < job.n && status == PLAY_OK; i++)
                status = play_item(&s, curl, job.items[i]);
            if (job.loop && status == PLAY_OK)
                gap(&s, job.gap_ms);
            if (cancelled(&s))
                status = PLAY_CANCELLED;
        } while (job.loop && status == PLAY_OK);

        close_pa(&s, status == PLAY_OK);
        if (cancelled(&s))
            status = PLAY_CANCELLED;
        post(job.id, PLAYER_FINISHED, status);
    }
    return NULL;
}

int player_init(void)
{
    pthread_t t;
    pthread_attr_t attr;

    if (pipe(ev_pipe) < 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        fcntl(ev_pipe[i], F_SETFL, fcntl(ev_pipe[i], F_GETFL) | O_NONBLOCK);
        fcntl(ev_pipe[i], F_SETFD, FD_CLOEXEC);
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, player_thread, NULL) != 0)
        return -1;
    pthread_attr_destroy(&attr);
    return ev_pipe[0];
}

unsigned player_play(const char *const *items, int n, int loop, int gap_ms)
{
    unsigned id;

    if (n > PLAYER_MAX_ITEMS)
        n = PLAYER_MAX_ITEMS;
    pthread_mutex_lock(&lock);
    id = next_id++;
    if (!next_id)
        next_id = 1;
    pending.id = id;
    pending.n = n;
    pending.loop = loop;
    pending.gap_ms = gap_ms;
    for (int i = 0; i < n; i++)
        strcopy(pending.items[i], ITEM_MAX, items[i]);
    have_pending = 1;
    wanted = id;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    return id;
}

void player_stop(void)
{
    pthread_mutex_lock(&lock);
    wanted = 0;
    have_pending = 0;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
}

int player_next_event(unsigned *job, int *what, int *status)
{
    char buf[32];
    int got = 0;

    while (read(ev_pipe[0], buf, sizeof(buf)) > 0)
        ;
    pthread_mutex_lock(&lock);
    if (ev_count) {
        struct event *e = &events[ev_head];
        *job = e->job;
        *what = e->what;
        *status = e->status;
        ev_head = (ev_head + 1) % (int)(sizeof(events) / sizeof(events[0]));
        ev_count--;
        got = 1;
    }
    pthread_mutex_unlock(&lock);
    return got;
}
