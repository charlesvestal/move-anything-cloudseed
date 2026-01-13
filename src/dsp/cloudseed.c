/*
 * CloudSeed Audio FX Plugin
 *
 * Modern algorithmic reverb based on CloudSeedCore by Ghost Note Audio.
 * Features allpass diffusion and modulated delay networks.
 *
 * Signal Flow:
 * Input -> Pre-delay -> Diffuser Network (4x APF) -> Delay Network (4x) -> Output
 *                              ^                            |
 *                              |_____ Hadamard feedback ____|
 *
 * Parameters:
 * - decay: Feedback amount (reverb tail length)
 * - mix: Dry/wet blend
 * - predelay: Initial delay before reverb onset
 * - size: Room size (scales delay times)
 * - damping: High-frequency absorption
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "audio_fx_api_v1.h"

#define SAMPLE_RATE 44100

/* Buffer sizes (all power of 2 for efficient masking) */
#define PREDELAY_SIZE 8192
#define PREDELAY_MASK (PREDELAY_SIZE - 1)

#define DIFF_SIZE 512
#define DIFF_MASK (DIFF_SIZE - 1)

#define DELAY_SIZE 8192
#define DELAY_MASK (DELAY_SIZE - 1)

/* Diffuser delay times (prime-ish numbers for good diffusion) */
#define DIFF1_DELAY 142
#define DIFF2_DELAY 107
#define DIFF3_DELAY 379
#define DIFF4_DELAY 277

/* Delay network base delays (primes for density) */
#define DELAY1_BASE 2473
#define DELAY2_BASE 3119
#define DELAY3_BASE 3947
#define DELAY4_BASE 4643

/* Allpass coefficient - higher = more diffusion/smearing */
#define APF_COEFF 0.7f

/* LFO settings */
#define LFO_FREQ 0.3f           /* ~0.3 Hz */
#define LFO_DEPTH_SAMPLES 132   /* ~3ms at 44.1kHz */

/* Max pre-delay: 100ms = 4410 samples */
#define MAX_PREDELAY_SAMPLES 4410

/* PI constant */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Plugin state */
static const host_api_v1_t *g_host = NULL;
static audio_fx_api_v1_t g_fx_api;

/* Parameters */
static float g_decay = 0.5f;     /* Feedback amount */
static float g_mix = 0.3f;       /* Dry/wet */
static float g_predelay = 0.1f;  /* Pre-delay (0-1 maps to 0-100ms) */
static float g_size = 0.5f;      /* Room size */
static float g_damping = 0.5f;   /* High-frequency damping */

/* Pre-delay buffers (stereo) */
static float g_predelay_l[PREDELAY_SIZE];
static float g_predelay_r[PREDELAY_SIZE];
static unsigned int g_predelay_pos = 0;

/* Diffuser buffers (4 per channel) */
static float g_diff1_l[DIFF_SIZE];
static float g_diff1_r[DIFF_SIZE];
static float g_diff2_l[DIFF_SIZE];
static float g_diff2_r[DIFF_SIZE];
static float g_diff3_l[DIFF_SIZE];
static float g_diff3_r[DIFF_SIZE];
static float g_diff4_l[DIFF_SIZE];
static float g_diff4_r[DIFF_SIZE];
static unsigned int g_diff_pos = 0;

/* Delay network buffers (4 per channel) */
static float g_delay1_l[DELAY_SIZE];
static float g_delay1_r[DELAY_SIZE];
static float g_delay2_l[DELAY_SIZE];
static float g_delay2_r[DELAY_SIZE];
static float g_delay3_l[DELAY_SIZE];
static float g_delay3_r[DELAY_SIZE];
static float g_delay4_l[DELAY_SIZE];
static float g_delay4_r[DELAY_SIZE];
static unsigned int g_delay_pos = 0;

/* Feedback state (for Hadamard mixing) */
static float g_fb1_l = 0.0f, g_fb1_r = 0.0f;
static float g_fb2_l = 0.0f, g_fb2_r = 0.0f;
static float g_fb3_l = 0.0f, g_fb3_r = 0.0f;
static float g_fb4_l = 0.0f, g_fb4_r = 0.0f;

/* Damping lowpass state (one-pole per delay line) */
static float g_damp1_l = 0.0f, g_damp1_r = 0.0f;
static float g_damp2_l = 0.0f, g_damp2_r = 0.0f;
static float g_damp3_l = 0.0f, g_damp3_r = 0.0f;
static float g_damp4_l = 0.0f, g_damp4_r = 0.0f;

/* LFO phase (different for L/R for stereo width) */
static float g_lfo_phase_l = 0.0f;
static float g_lfo_phase_r = 0.0f;

/* Logging helper */
static void fx_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[cloudseed] %s", msg);
        g_host->log(buf);
    }
}

/*
 * Allpass filter (CloudSeed topology)
 * Returns filtered output and updates buffer
 */
static inline float allpass(float in, float *buf, int delay, unsigned int pos) {
    unsigned int read_idx = (pos + DIFF_SIZE - delay) & DIFF_MASK;
    float delayed = buf[read_idx];
    float temp = in + delayed * APF_COEFF;
    buf[pos & DIFF_MASK] = temp;
    return delayed - temp * APF_COEFF;
}

/*
 * One-pole lowpass filter
 * coeff: 0 = no filtering, 1 = max filtering
 */
static inline float lowpass(float in, float *state, float coeff) {
    *state = *state + coeff * (in - *state);
    return *state;
}

/*
 * Calculate damping coefficient from parameter
 * One-pole lowpass: y = y + coeff * (x - y)
 * Higher coeff = less filtering (passes more high freq)
 * damping=0 -> coeff ~0.95 (bright, minimal filtering)
 * damping=1 -> coeff ~0.15 (dark, strong filtering)
 */
static inline float calc_damp_coeff(float damping) {
    return 0.95f - damping * 0.80f;
}

/*
 * Calculate actual delay times from size parameter
 * size scales delay times: actual = base * (0.3 + size * 1.2)
 * At size=0: 30% of base (small room)
 * At size=1: 150% of base (huge hall)
 */
static inline int scale_delay(int base, float size) {
    float scale = 0.3f + size * 1.2f;
    int result = (int)(base * scale);
    /* Clamp to valid range */
    if (result < 1) result = 1;
    if (result > DELAY_SIZE - 1) result = DELAY_SIZE - 1;
    return result;
}

/* === Audio FX API Implementation === */

static int fx_on_load(const char *module_dir, const char *config_json) {
    char msg[256];
    snprintf(msg, sizeof(msg), "CloudSeed loading from: %s", module_dir);
    fx_log(msg);

    /* Clear pre-delay buffers */
    memset(g_predelay_l, 0, sizeof(g_predelay_l));
    memset(g_predelay_r, 0, sizeof(g_predelay_r));
    g_predelay_pos = 0;

    /* Clear diffuser buffers */
    memset(g_diff1_l, 0, sizeof(g_diff1_l));
    memset(g_diff1_r, 0, sizeof(g_diff1_r));
    memset(g_diff2_l, 0, sizeof(g_diff2_l));
    memset(g_diff2_r, 0, sizeof(g_diff2_r));
    memset(g_diff3_l, 0, sizeof(g_diff3_l));
    memset(g_diff3_r, 0, sizeof(g_diff3_r));
    memset(g_diff4_l, 0, sizeof(g_diff4_l));
    memset(g_diff4_r, 0, sizeof(g_diff4_r));
    g_diff_pos = 0;

    /* Clear delay network buffers */
    memset(g_delay1_l, 0, sizeof(g_delay1_l));
    memset(g_delay1_r, 0, sizeof(g_delay1_r));
    memset(g_delay2_l, 0, sizeof(g_delay2_l));
    memset(g_delay2_r, 0, sizeof(g_delay2_r));
    memset(g_delay3_l, 0, sizeof(g_delay3_l));
    memset(g_delay3_r, 0, sizeof(g_delay3_r));
    memset(g_delay4_l, 0, sizeof(g_delay4_l));
    memset(g_delay4_r, 0, sizeof(g_delay4_r));
    g_delay_pos = 0;

    /* Clear feedback state */
    g_fb1_l = g_fb1_r = 0.0f;
    g_fb2_l = g_fb2_r = 0.0f;
    g_fb3_l = g_fb3_r = 0.0f;
    g_fb4_l = g_fb4_r = 0.0f;

    /* Clear damping state */
    g_damp1_l = g_damp1_r = 0.0f;
    g_damp2_l = g_damp2_r = 0.0f;
    g_damp3_l = g_damp3_r = 0.0f;
    g_damp4_l = g_damp4_r = 0.0f;

    /* Initialize LFO phases (offset for stereo width) */
    g_lfo_phase_l = 0.0f;
    g_lfo_phase_r = 0.25f;  /* 90 degree offset for stereo width */

    fx_log("CloudSeed initialized");
    return 0;
}

static void fx_on_unload(void) {
    fx_log("CloudSeed unloading");
}

static void fx_process_block(int16_t *audio_inout, int frames) {
    /* Calculate pre-delay in samples (0-100ms) */
    int predelay_samples = (int)(g_predelay * MAX_PREDELAY_SAMPLES);
    if (predelay_samples < 1) predelay_samples = 1;

    /* Calculate scaled delay times */
    int delay1 = scale_delay(DELAY1_BASE, g_size);
    int delay2 = scale_delay(DELAY2_BASE, g_size);
    int delay3 = scale_delay(DELAY3_BASE, g_size);
    int delay4 = scale_delay(DELAY4_BASE, g_size);

    /* Calculate feedback amount from decay (0.5 to 0.995) */
    /* Higher max feedback allows much longer, lush reverb tails */
    float feedback = 0.5f + g_decay * 0.495f;

    /* Calculate damping coefficient */
    float damp_coeff = calc_damp_coeff(g_damping);

    /* LFO increment per sample (~0.3Hz) */
    float lfo_inc = LFO_FREQ / SAMPLE_RATE;

    for (int i = 0; i < frames; i++) {
        /* Convert input to float (-1.0 to 1.0) */
        float in_l = audio_inout[i * 2] / 32768.0f;
        float in_r = audio_inout[i * 2 + 1] / 32768.0f;

        /* === Pre-delay === */
        /* Write input to pre-delay buffer */
        g_predelay_l[g_predelay_pos & PREDELAY_MASK] = in_l;
        g_predelay_r[g_predelay_pos & PREDELAY_MASK] = in_r;

        /* Read from pre-delay */
        unsigned int pd_read = (g_predelay_pos + PREDELAY_SIZE - predelay_samples) & PREDELAY_MASK;
        float pd_l = g_predelay_l[pd_read];
        float pd_r = g_predelay_r[pd_read];

        g_predelay_pos++;

        /* === Diffuser Network (4 cascaded allpass filters) === */
        /* Diffuser input is just pre-delayed signal (feedback goes to delay writes) */
        float diff_in_l = pd_l;
        float diff_in_r = pd_r;

        float d1_l = allpass(diff_in_l, g_diff1_l, DIFF1_DELAY, g_diff_pos);
        float d1_r = allpass(diff_in_r, g_diff1_r, DIFF1_DELAY, g_diff_pos);

        float d2_l = allpass(d1_l, g_diff2_l, DIFF2_DELAY, g_diff_pos);
        float d2_r = allpass(d1_r, g_diff2_r, DIFF2_DELAY, g_diff_pos);

        float d3_l = allpass(d2_l, g_diff3_l, DIFF3_DELAY, g_diff_pos);
        float d3_r = allpass(d2_r, g_diff3_r, DIFF3_DELAY, g_diff_pos);

        float d4_l = allpass(d3_l, g_diff4_l, DIFF4_DELAY, g_diff_pos);
        float d4_r = allpass(d3_r, g_diff4_r, DIFF4_DELAY, g_diff_pos);

        g_diff_pos++;

        /* === LFO modulation for delay network === */
        float lfo_l = sinf(g_lfo_phase_l * 2.0f * M_PI);
        float lfo_r = sinf(g_lfo_phase_r * 2.0f * M_PI);

        /* Advance LFO phase */
        g_lfo_phase_l += lfo_inc;
        if (g_lfo_phase_l >= 1.0f) g_lfo_phase_l -= 1.0f;
        g_lfo_phase_r += lfo_inc;
        if (g_lfo_phase_r >= 1.0f) g_lfo_phase_r -= 1.0f;

        /* Calculate modulated delay times */
        int mod_delay1_l = delay1 + (int)roundf(lfo_l * LFO_DEPTH_SAMPLES);
        int mod_delay2_l = delay2 + (int)roundf(lfo_r * LFO_DEPTH_SAMPLES);  /* Alternate LFO */
        int mod_delay3_l = delay3 + (int)roundf(lfo_l * LFO_DEPTH_SAMPLES);
        int mod_delay4_l = delay4 + (int)roundf(lfo_r * LFO_DEPTH_SAMPLES);

        int mod_delay1_r = delay1 + (int)roundf(lfo_r * LFO_DEPTH_SAMPLES);
        int mod_delay2_r = delay2 + (int)roundf(lfo_l * LFO_DEPTH_SAMPLES);
        int mod_delay3_r = delay3 + (int)roundf(lfo_r * LFO_DEPTH_SAMPLES);
        int mod_delay4_r = delay4 + (int)roundf(lfo_l * LFO_DEPTH_SAMPLES);

        /* Clamp modulated delays to valid range */
        if (mod_delay1_l < 1) mod_delay1_l = 1;
        if (mod_delay1_l > DELAY_SIZE - 2) mod_delay1_l = DELAY_SIZE - 2;
        if (mod_delay2_l < 1) mod_delay2_l = 1;
        if (mod_delay2_l > DELAY_SIZE - 2) mod_delay2_l = DELAY_SIZE - 2;
        if (mod_delay3_l < 1) mod_delay3_l = 1;
        if (mod_delay3_l > DELAY_SIZE - 2) mod_delay3_l = DELAY_SIZE - 2;
        if (mod_delay4_l < 1) mod_delay4_l = 1;
        if (mod_delay4_l > DELAY_SIZE - 2) mod_delay4_l = DELAY_SIZE - 2;
        if (mod_delay1_r < 1) mod_delay1_r = 1;
        if (mod_delay1_r > DELAY_SIZE - 2) mod_delay1_r = DELAY_SIZE - 2;
        if (mod_delay2_r < 1) mod_delay2_r = 1;
        if (mod_delay2_r > DELAY_SIZE - 2) mod_delay2_r = DELAY_SIZE - 2;
        if (mod_delay3_r < 1) mod_delay3_r = 1;
        if (mod_delay3_r > DELAY_SIZE - 2) mod_delay3_r = DELAY_SIZE - 2;
        if (mod_delay4_r < 1) mod_delay4_r = 1;
        if (mod_delay4_r > DELAY_SIZE - 2) mod_delay4_r = DELAY_SIZE - 2;

        /* === Delay Network (4 modulated delay lines) === */
        /* Write diffuser output plus Hadamard feedback to delay buffers */
        g_delay1_l[g_delay_pos & DELAY_MASK] = d4_l + g_fb1_l;
        g_delay1_r[g_delay_pos & DELAY_MASK] = d4_r + g_fb1_r;
        g_delay2_l[g_delay_pos & DELAY_MASK] = d4_l + g_fb2_l;
        g_delay2_r[g_delay_pos & DELAY_MASK] = d4_r + g_fb2_r;
        g_delay3_l[g_delay_pos & DELAY_MASK] = d4_l + g_fb3_l;
        g_delay3_r[g_delay_pos & DELAY_MASK] = d4_r + g_fb3_r;
        g_delay4_l[g_delay_pos & DELAY_MASK] = d4_l + g_fb4_l;
        g_delay4_r[g_delay_pos & DELAY_MASK] = d4_r + g_fb4_r;

        /* Read from delay lines with modulation */
        unsigned int read1_l = (g_delay_pos + DELAY_SIZE - mod_delay1_l) & DELAY_MASK;
        unsigned int read2_l = (g_delay_pos + DELAY_SIZE - mod_delay2_l) & DELAY_MASK;
        unsigned int read3_l = (g_delay_pos + DELAY_SIZE - mod_delay3_l) & DELAY_MASK;
        unsigned int read4_l = (g_delay_pos + DELAY_SIZE - mod_delay4_l) & DELAY_MASK;

        unsigned int read1_r = (g_delay_pos + DELAY_SIZE - mod_delay1_r) & DELAY_MASK;
        unsigned int read2_r = (g_delay_pos + DELAY_SIZE - mod_delay2_r) & DELAY_MASK;
        unsigned int read3_r = (g_delay_pos + DELAY_SIZE - mod_delay3_r) & DELAY_MASK;
        unsigned int read4_r = (g_delay_pos + DELAY_SIZE - mod_delay4_r) & DELAY_MASK;

        float del1_l = g_delay1_l[read1_l];
        float del2_l = g_delay2_l[read2_l];
        float del3_l = g_delay3_l[read3_l];
        float del4_l = g_delay4_l[read4_l];

        float del1_r = g_delay1_r[read1_r];
        float del2_r = g_delay2_r[read2_r];
        float del3_r = g_delay3_r[read3_r];
        float del4_r = g_delay4_r[read4_r];

        g_delay_pos++;

        /* === Damping (one-pole lowpass per delay line) === */
        del1_l = lowpass(del1_l, &g_damp1_l, damp_coeff);
        del2_l = lowpass(del2_l, &g_damp2_l, damp_coeff);
        del3_l = lowpass(del3_l, &g_damp3_l, damp_coeff);
        del4_l = lowpass(del4_l, &g_damp4_l, damp_coeff);

        del1_r = lowpass(del1_r, &g_damp1_r, damp_coeff);
        del2_r = lowpass(del2_r, &g_damp2_r, damp_coeff);
        del3_r = lowpass(del3_r, &g_damp3_r, damp_coeff);
        del4_r = lowpass(del4_r, &g_damp4_r, damp_coeff);

        /* === Hadamard Feedback Matrix === */
        /* Mix delay outputs before feeding back to delay network */
        /* 0.5f is proper normalization for 4x4 Hadamard (1/sqrt(4)) */
        g_fb1_l = (del1_l + del2_l + del3_l + del4_l) * 0.5f * feedback;
        g_fb2_l = (del1_l - del2_l + del3_l - del4_l) * 0.5f * feedback;
        g_fb3_l = (del1_l + del2_l - del3_l - del4_l) * 0.5f * feedback;
        g_fb4_l = (del1_l - del2_l - del3_l + del4_l) * 0.5f * feedback;

        g_fb1_r = (del1_r + del2_r + del3_r + del4_r) * 0.5f * feedback;
        g_fb2_r = (del1_r - del2_r + del3_r - del4_r) * 0.5f * feedback;
        g_fb3_r = (del1_r + del2_r - del3_r - del4_r) * 0.5f * feedback;
        g_fb4_r = (del1_r - del2_r - del3_r + del4_r) * 0.5f * feedback;

        /* === Output === */
        /* Sum delay outputs for wet signal */
        float wet_l = (del1_l + del2_l + del3_l + del4_l) * 0.25f;
        float wet_r = (del1_r + del2_r + del3_r + del4_r) * 0.25f;

        /* === Mix dry and wet === */
        float out_l = in_l * (1.0f - g_mix) + wet_l * g_mix;
        float out_r = in_r * (1.0f - g_mix) + wet_r * g_mix;

        /* Clamp output and convert back to int16 */
        if (out_l > 1.0f) out_l = 1.0f;
        if (out_l < -1.0f) out_l = -1.0f;
        if (out_r > 1.0f) out_r = 1.0f;
        if (out_r < -1.0f) out_r = -1.0f;

        audio_inout[i * 2] = (int16_t)(out_l * 32767.0f);
        audio_inout[i * 2 + 1] = (int16_t)(out_r * 32767.0f);
    }
}

static void fx_set_param(const char *key, const char *val) {
    if (strcmp(key, "decay") == 0) {
        float v = atof(val);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_decay = v;
    } else if (strcmp(key, "mix") == 0) {
        float v = atof(val);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_mix = v;
    } else if (strcmp(key, "predelay") == 0) {
        float v = atof(val);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_predelay = v;
    } else if (strcmp(key, "size") == 0) {
        float v = atof(val);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_size = v;
    } else if (strcmp(key, "damping") == 0) {
        float v = atof(val);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        g_damping = v;
    }
}

static int fx_get_param(const char *key, char *buf, int buf_len) {
    if (strcmp(key, "decay") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_decay);
    } else if (strcmp(key, "mix") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_mix);
    } else if (strcmp(key, "predelay") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_predelay);
    } else if (strcmp(key, "size") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_size);
    } else if (strcmp(key, "damping") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_damping);
    } else if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "CloudSeed");
    }
    return -1;
}

/* === Entry Point === */

audio_fx_api_v1_t* move_audio_fx_init_v1(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_fx_api, 0, sizeof(g_fx_api));
    g_fx_api.api_version = AUDIO_FX_API_VERSION;
    g_fx_api.on_load = fx_on_load;
    g_fx_api.on_unload = fx_on_unload;
    g_fx_api.process_block = fx_process_block;
    g_fx_api.set_param = fx_set_param;
    g_fx_api.get_param = fx_get_param;

    fx_log("CloudSeed plugin initialized");

    return &g_fx_api;
}
