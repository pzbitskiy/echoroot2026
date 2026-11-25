/*
 * Native fuzz of the API server's framing and message parsing, with the
 * sanitizers on. Build and run on the PC, from this directory:
 *
 *   gcc -std=gnu99 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
 *       -I.. -o fuzz fuzz.c ../api.c ../pb.c
 *   ./fuzz 60000 1          # iterations, seed: includes garbage and lying lengths
 *   ./fuzz 150000 2 deep    # mostly well-framed messages, reaches the handlers
 */
#include <arpa/inet.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "sat.h"
struct config cfg = { .name = "fuzz", .mac = "02:00:00:00:00:01", .port = 16053, .tts_rate = 48000, .friendly_name = "F" };
int log_level = -1;
const struct wake_word wake_words[] = { { "computer", "Computer", "COMPUTER" } };
const int n_wake_words = 1;
static unsigned long calls;
void logmsg(int l, const char *f, ...) { (void)l; (void)f; }
uint64_t now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000ull + ts.tv_nsec / 1000000; }
void strcopy(char *d, size_t c, const char *s) { size_t n = strlen(s); if (n >= c) n = c - 1; memcpy(d, s, n); d[n] = 0; }
void sat_voice_client_changed(int s) { (void)s; calls++; }
void sat_va_response(int e) { (void)e; calls++; }
void sat_va_event(int t, const struct va_kv *kv, int n) { (void)t; for (int i = 0; i < n; i++) calls += strlen(kv[i].name) + strlen(kv[i].value); api_va_request(0, "", ""); }
void sat_va_timer(int e, const char *id, const char *nm, unsigned t, unsigned l, int a) { (void)e; (void)t; (void)l; (void)a; calls += strlen(id) + strlen(nm); }
void sat_va_announce(const char *m, const char *t, const char *p, int s) { (void)s; calls += strlen(m) + strlen(t) + strlen(p); api_va_announce_finished(1); }
void sat_set_wake_word(const char *id) { calls += strlen(id); }
const char *sat_wake_word_id(void) { return "computer"; }
void sat_media_command(int a, int b, int c, float v, int d, const char *u, int e) { (void)a; (void)b; (void)c; (void)v; (void)d; (void)e; calls += strlen(u); api_media_state(); }
int sat_media_state(float *v, int *m) { *v = 0.5f; *m = 0; return 1; }
int sat_mic_muted(void) { return 0; }
void sat_mute_command(int m) { calls += m; api_mute_state(); }
int sat_ringing(void) { return 1; }
void sat_stop_ringing(void) { calls++; api_ring_state(); }

static int client(void)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(16053) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("connect"); exit(1); }
    return fd;
}
static void pump(void)
{
    struct pollfd fds[16];
    for (int k = 0; k < 3; k++) {
        int n = api_poll_prepare(fds, 16);
        if (poll(fds, n, 0) > 0) api_poll_handle(fds, n);
    }
}
static size_t varint(uint8_t *p, uint64_t v) { size_t n = 0; do { p[n] = v & 0x7f; v >>= 7; if (v) p[n] |= 0x80; n++; } while (v); return n; }

int main(int argc, char **argv)
{
    static const int types[] = { 1, 3, 5, 6, 7, 8, 9, 11, 20, 28, 33, 62, 65, 89, 91, 92, 106, 115, 119, 121, 123, 200 };
    unsigned iters = argc > 1 ? atoi(argv[1]) : 20000;
    uint8_t buf[70000], sink[65536];
    srand(argc > 2 ? atoi(argv[2]) : 1);
    if (api_init() < 0) return 1;
    int fd = -1;
    for (unsigned it = 0; it < iters; it++) {
        if (fd < 0) { fd = client(); pump(); uint8_t hello[] = { 0, 0, 1 }; if (argc > 3 || rand() % 4) send(fd, hello, 3, MSG_NOSIGNAL); pump(); }
        size_t n = 0;
        int mode = argc > 3 ? 2 + rand() % 8 : rand() % 10;
        if (mode == 0) {                       /* pure garbage */
            n = rand() % 64; for (size_t i = 0; i < n; i++) buf[i] = rand();
        } else {                               /* framed message, random or structured payload */
            uint8_t pl[4096]; size_t pn = 0, want = rand() % (mode == 1 ? 4000 : 80);
            while (pn < want) {
                if (mode < 5) { pl[pn++] = rand(); continue; }
                int field = 1 + rand() % 12, wire = (int[]){ 0, 2, 5, 0, 2, 1 }[rand() % 6];
                pn += varint(pl + pn, (field << 3) | wire);
                if (wire == 0) pn += varint(pl + pn, rand() % 3 ? rand() % 4 : ((uint64_t)rand() << 32) | rand());
                else if (wire == 5) { memset(pl + pn, rand(), 4); pn += 4; }
                else if (wire == 1) { memset(pl + pn, rand(), 8); pn += 8; }
                else { size_t l = rand() % 5 ? rand() % 40 : rand() % 1500; if (pn + l + 3 > sizeof(pl)) l = 0; pn += varint(pl + pn, rand() % 50 ? l : l + rand() % 9);
                       for (size_t i = 0; i < l; i++) pl[pn++] = rand() % 3 ? 'a' + rand() % 26 : rand(); }
            }
            buf[n++] = 0;
            n += varint(buf + n, (argc > 3 ? rand() % 2000 : rand() % 40) ? pn : (uint64_t)rand() * (rand() % 3));   /* sometimes a lying length */
            n += varint(buf + n, types[rand() % (sizeof(types) / sizeof(types[0]))]);
            memcpy(buf + n, pl, pn); n += pn;
        }
        size_t cut = rand() % 5 ? n : rand() % (n + 1);                                 /* sometimes split the write */
        if (send(fd, buf, cut, MSG_NOSIGNAL) < 0) { close(fd); fd = -1; continue; }
        pump();
        if (cut < n && send(fd, buf + cut, n - cut, MSG_NOSIGNAL) < 0) { close(fd); fd = -1; continue; }
        pump();
        while (recv(fd, sink, sizeof(sink), MSG_DONTWAIT) > 0) ;
        char c; if (recv(fd, &c, 1, MSG_DONTWAIT | MSG_PEEK) == 0 || rand() % 300 == 0) { close(fd); fd = -1; pump(); }
        if (rand() % 50 == 0) api_housekeeping();
    }
    printf("ok: %u iterations, %lu callback units\n", iters, calls);
    return 0;
}
