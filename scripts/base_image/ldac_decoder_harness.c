/* Host harness: reference LDAC encoder -> libldacdec (dlopen'd, isolated
 * symbols), called exactly like BlueALSA 5 a2dp-ldac sink (S32 output).
 *
 * Build (L = scratch/base-upgrade/ldacBT/libldac, D = patched decoder tree):
 *   gcc -shared -fPIC -O1 -DDOUBLE64 -std=gnu11 -I$L/inc -I$L/src \
 *       $D/{libldacdec,bit_allocation,huffCodes,bit_reader,utility,imdct,spectrum}.c \
 *       -lm -lpthread -o libldacdec.so
 *   gcc -O2 -I$L/inc -I$L/src ldac_decoder_harness.c $L/src/ldacBT.c $L/src/ldaclib.c \
 *       -ldl -lm -o harness
 * Run: ./harness ./libldacdec.so <rate> <eqmid 0=HQ 1=SQ 2=MQ> [bitflips] [seed]
 * Env: SIG_LOUD, SIG_LR (signal), CM_DUAL (dual channel), DEC_RATE (negotiate
 * a different rate). Each packet ends at a PROT_NONE page. Expect errors=0
 * and a stable snr on clean streams; bitflip runs must exit normally. */
#include <dlfcn.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ldacBT.h"

typedef HANDLE_LDAC_BT (*get_fn)(void);
typedef int (*init_fn)(HANDLE_LDAC_BT, int, int, int, int, int);
typedef int (*dec_fn)(HANDLE_LDAC_BT, unsigned char *, void *, LDACBT_SMPL_FMT_T, int, int *, int *);

int main(int argc, char ** argv) {
    const char * lib = argv[1];
    int rate = atoi(argv[2]);
    int eqmid = atoi(argv[3]);          /* 0 HQ, 1 SQ, 2 MQ */
    int fuzz = argc > 4 ? atoi(argv[4]) : 0;
    void * dl = dlopen(lib, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!dl) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    get_fn dget = (get_fn) dlsym(dl, "ldacBT_get_handle");
    init_fn dinit = (init_fn) dlsym(dl, "ldacBT_init_handle_decode");
    dec_fn ddec = (dec_fn) dlsym(dl, "ldacBT_decode");

    HANDLE_LDAC_BT enc = ldacBT_get_handle();
    int cm = getenv("CM_DUAL") ? LDACBT_CHANNEL_MODE_DUAL_CHANNEL : LDACBT_CHANNEL_MODE_STEREO;
    if (ldacBT_init_handle_encode(enc, 679, eqmid, cm, LDACBT_SMPL_FMT_S32, rate) != 0) {
        fprintf(stderr, "enc init %d\n", ldacBT_get_error_code(enc)); return 2;
    }
    HANDLE_LDAC_BT dec = dget();
    if (dinit(dec, cm, getenv("DEC_RATE") ? atoi(getenv("DEC_RATE")) : rate, 0, 0, 0) != 0) { fprintf(stderr, "dec init\n"); return 2; }

    int32_t pcm[LDACBT_MAX_LSU * 2];
    int32_t out[LDACBT_MAX_LSU * 2 * 4];
    unsigned char stream[1024];
    long samples = 0, decoded_total = 0, packets = 0, errors = 0;
    double peak = 0;
    /* SNR against the input: keep left-channel input and output streams. */
    static float inbuf[8000 * 512], outbuf[8000 * 512];
    long in_n = 0, out_n = 0;
    unsigned long long sum = 1469598103934665603ULL;
    srand(argc > 5 ? atoi(argv[5]) : 1);
    for (int it = 0; it < 8000; it++) {
        int lsu = LDACBT_ENC_LSU; /* the encoder consumes 128 samples per call at every rate */
        for (int i = 0; i < lsu; i++) {
            double t = (double) (samples + i) / rate;
            double v;
            if (getenv("SIG_LOUD")) {
                /* Loud broadband plus a sweep: exercises fine precision and
                 * falling gradients. */
                double f = 200.0 + 18000.0 * fmod(t / 4.0, 1.0);
                v = 0.45 * sin(2 * M_PI * f * t) + 0.5 * ((rand() / (double) RAND_MAX) - 0.5);
            } else {
                v = 0.5 * sin(2 * M_PI * 1000 * t) + 0.1 * ((rand() / (double) RAND_MAX) - 0.5);
            }
            pcm[2 * i] = pcm[2 * i + 1] = (int32_t) (v * 2147483647.0);
            if (getenv("SIG_LR")) {
                /* Loud left, near-silent right: large left/right scale factor
                 * differences in the differential (mode 2) coding. */
                pcm[2 * i] = (int32_t) (0.9 * sin(2 * M_PI * 3000 * t) * 2147483647.0);
                pcm[2 * i + 1] = (int32_t) (0.00002 * ((rand() / (double) RAND_MAX) - 0.5) * 2147483647.0);
            }
        }
        samples += lsu;
        int pcm_used = 0, stream_sz = 0, frame_num = 0;
        if (ldacBT_encode(enc, pcm, &pcm_used, stream, &stream_sz, &frame_num) != 0) {
            fprintf(stderr, "enc err %d\n", ldacBT_get_error_code(enc)); return 2;
        }
        for (int i = 0; i < pcm_used / 8; i++) inbuf[in_n++] = pcm[2 * i] / 2147483648.0f;
        if (stream_sz <= 0) continue;
        packets++;
        /* Exact-size heap copy so ASAN catches any read past the packet. */
        /* Packet ends exactly at a PROT_NONE page: any read past it faults. */
        long pg = sysconf(_SC_PAGESIZE);
        unsigned char * region = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        mprotect(region + pg, pg, PROT_NONE);
        unsigned char * p = region + pg - stream_sz; /* no slack: reads stop at the packet */

        memcpy(p, stream, stream_sz);
        if (fuzz) for (int k = 0; k < fuzz; k++) p[rand() % stream_sz] ^= (unsigned char) (1u << (rand() % 8));
        unsigned char * cur = p;
        int left = stream_sz;
        for (int f = 0; f < frame_num && left > 0; f++) {
            int used = 0, dsz = 0;
            if (ddec(dec, cur, out, LDACBT_SMPL_FMT_S32, left, &used, &dsz) != 0) { errors++; break; }
            if (used <= 0 || used > left) { errors++; break; }
            cur += used; left -= used;
            decoded_total += dsz / 8;
            for (int i = 0; i < dsz / 8; i++) outbuf[out_n++] = out[2 * i] / 2147483648.0f;
            for (int i = 0; i < dsz / 4; i++) { double a = fabs(out[i] / 2147483648.0); if (a > peak) peak = a; sum = (sum ^ (uint32_t) out[i]) * 1099511628211ULL; }
        }
        munmap(region, 2 * pg);
    }
    /* Best lag and gain (decoder output level differs by design), then SNR. */
    double best_snr = -1e9; int best_lag = 0;
    long n = (out_n < in_n ? out_n : in_n) - 8192;
    for (int lag = 0; lag < 8192 && !fuzz; lag += (lag < 2048 ? 1 : 1)) {
        double xy = 0, xx = 0, yy = 0;
        for (long i = 1024; i < n; i += 7) { double x = inbuf[i], y = outbuf[i + lag]; xy += x * y; xx += x * x; yy += y * y; }
        double g = xx > 0 ? xy / xx : 0, err = 0, sig = 0;
        for (long i = 1024; i < n; i += 7) { double x = inbuf[i] * g, y = outbuf[i + lag]; err += (y - x) * (y - x); sig += x * x; }
        double snr = err > 0 ? 10 * log10(sig / err) : 200;
        if (snr > best_snr) { best_snr = snr; best_lag = lag; }
    }
    if (!fuzz) printf("snr=%.2fdB lag=%d ", best_snr, best_lag);
    printf("rate=%d eqmid=%d fuzz=%d packets=%ld decoded_frames=%ld errors=%ld peak=%.3f hash=%016llx\n",
           rate, eqmid, fuzz, packets, decoded_total, errors, peak, sum);
    return 0;
}
