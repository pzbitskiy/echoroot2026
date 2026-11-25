/*
 * Microphone input: the DSP's beamformed, echo-cancelled 16 kHz mono stream,
 * read from the shared ring /BMicsOut.shm through Amazon's libshmbuffer.so.
 *
 * The library has no header. These prototypes were recovered from its
 * disassembly and from how shmbuf_tool calls it:
 *   - a reader keeps its own position (a 64-bit sample sequence number),
 *     the writer never blocks, and a reader that falls a whole ring behind
 *     gets -2 ("overwritten");
 *   - mode 0 opens for reading.
 */
#include "sat.h"

extern void *ShmbufOpen(const char *name, int mode);
extern int ShmbufGetMsgSize(void *h);
extern int ShmbufRead(void *h, void *out, int num_msgs);              /* non-blocking */
extern uint64_t ShmbufReadStart(void *h, int preroll_msgs);           /* reader := writer - preroll */
extern int ShmbufSetReaderSeqNum(void *h, uint64_t seq);

static void *mic;
static int gain_q8 = 256;   /* linear gain, 256 = unity */

/* dB to a linear factor in 1/256 steps, without libm; -20..48 dB. */
static int gain_from_db(int db)
{
    long g = 256;
    if (db > 48)
        db = 48;
    if (db < -20)
        db = -20;
    for (; db > 0; db--)
        g = g * 1122 / 1000;
    for (; db < 0; db++)
        g = g * 1000 / 1122;
    return (int)g;
}

int mic_open(void)
{
    if (mic)
        return 0;
    mic = ShmbufOpen(cfg.mic_shm, 0);
    if (!mic) {
        loge("mic: cannot open %s", cfg.mic_shm);
        return -1;
    }
    int size = ShmbufGetMsgSize(mic);
    if (size != 2) {
        /* Not closed: the handle is tiny and this is fatal for audio anyway. */
        loge("mic: %s has %d-byte samples, expected 2", cfg.mic_shm, size);
        mic = NULL;
        return -1;
    }
    gain_q8 = gain_from_db(cfg.mic_gain_db);
    logi("mic: opened %s, gain %d dB", cfg.mic_shm, cfg.mic_gain_db);
    return 0;
}

void mic_seek_now(void)
{
    if (mic_open() == 0)
        ShmbufReadStart(mic, 0);
}

int mic_seek(uint64_t seq)
{
    if (mic_open() < 0)
        return -1;
    if (ShmbufSetReaderSeqNum(mic, seq) != 0) {
        logw("mic: sequence number %llu is out of range, starting at the live position",
             (unsigned long long)seq);
        ShmbufReadStart(mic, 0);
        return -1;
    }
    return 0;
}

int mic_read(int16_t *buf, int max_samples)
{
    if (!mic)
        return 0;
    int n = ShmbufRead(mic, buf, max_samples);
    if (n == -2) {
        logw("mic: reader position lost, resuming at the live position");
        ShmbufReadStart(mic, 0);
        return 0;
    }
    if (n <= 0)
        return 0;
    if (gain_q8 != 256) {
        for (int i = 0; i < n; i++) {
            int v = (buf[i] * gain_q8) >> 8;
            buf[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
        }
    }
    return n;
}
