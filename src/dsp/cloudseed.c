/*
 * CloudSeed Audio FX Plugin
 *
 * EXACT port from CloudSeedCore by Ghost Note Audio (MIT Licensed)
 * https://github.com/GhostNoteAudio/CloudSeedCore
 *
 * This is a direct C translation of the C++ reference implementation.
 * All algorithms, buffer sizes, and processing logic match the original.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "audio_fx_api_v1.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_RATE 48000

/* Buffer sizes - EXACT from reference */
#define DELAY_BUFFER_SIZE 384000      /* 192000 * 2 - exact from ModulatedDelay.h */
#define ALLPASS_BUFFER_SIZE 19200     /* 100ms at 192kHz - exact from ModulatedAllpass.h */
#define BUFFER_SIZE 128               /* Process block size */

/* Configuration - EXACT from reference */
#define MAX_LINE_COUNT 12             /* TotalLineCount from ReverbChannel.h */
#define MAX_DIFFUSER_STAGES 12        /* MaxStageCount from AllpassDiffuser.h */
#define MAX_TAPS 256                  /* MaxTaps from MultitapDelay.h */
#define MODULATION_UPDATE_RATE 8      /* Exact from reference */

/* ============================================================================
 * UTILITY FUNCTIONS - From Utils.h
 * ============================================================================ */

static inline float db2gain(float db) {
    return powf(10.0f, db * 0.05f);
}

static inline float resp2dec(float x) {
    /* (10^(2x) - 1) * (100/99) * 0.01 */
    return (powf(10.0f, 2.0f * x) - 1.0f) * (100.0f / 99.0f) * 0.01f;
}

static inline float resp3dec(float x) {
    /* (10^(3x) - 1) * (1000/999) * 0.001 */
    return (powf(10.0f, 3.0f * x) - 1.0f) * (1000.0f / 999.0f) * 0.001f;
}

static inline float resp4oct(float x) {
    /* (2^(4x) - 1) * (16/15) * 0.0625 */
    return (powf(2.0f, 4.0f * x) - 1.0f) * (16.0f / 15.0f) * 0.0625f;
}

/* ============================================================================
 * LCG RANDOM - Exact port from LcgRandom.h
 * ============================================================================ */

typedef struct {
    uint64_t x;
} lcg_random_t;

static const uint64_t LCG_A = 22695477;
static const uint64_t LCG_C = 1;

static void lcg_init(lcg_random_t *rng, uint64_t seed) {
    rng->x = seed;
}

static uint32_t lcg_next_uint(lcg_random_t *rng) {
    uint64_t axc = LCG_A * rng->x + LCG_C;
    rng->x = axc & 0xFFFFFFFF;
    return (uint32_t)rng->x;
}

/* ============================================================================
 * RANDOM BUFFER - Exact port from RandomBuffer.cpp
 * ============================================================================ */

static void random_buffer_generate(uint64_t seed, float *output, int count) {
    lcg_random_t rand;
    lcg_init(&rand, seed);
    for (int i = 0; i < count; i++) {
        uint32_t val = lcg_next_uint(&rand);
        output[i] = (float)val / (float)UINT32_MAX;
    }
}

static void random_buffer_generate_cross(uint64_t seed, float cross_seed,
                                          float *output, int count) {
    float *seriesA = (float*)malloc(count * sizeof(float));
    float *seriesB = (float*)malloc(count * sizeof(float));

    uint64_t seedA = seed;
    uint64_t seedB = ~seed;

    random_buffer_generate(seedA, seriesA, count);
    random_buffer_generate(seedB, seriesB, count);

    for (int i = 0; i < count; i++) {
        output[i] = seriesA[i] * (1.0f - cross_seed) + seriesB[i] * cross_seed;
    }

    free(seriesA);
    free(seriesB);
}

/* ============================================================================
 * LP1 - Exact port from Lp1.h
 * ============================================================================ */

typedef struct {
    float fs;
    float b0, a1;
    float cutoff_hz;
    float output;
} lp1_t;

static void lp1_init(lp1_t *f, int samplerate) {
    f->fs = (float)samplerate;
    f->b0 = 1.0f;
    f->a1 = 0.0f;
    f->cutoff_hz = 1000.0f;
    f->output = 0.0f;
}

static void lp1_set_samplerate(lp1_t *f, int samplerate) {
    f->fs = (float)samplerate;
}

static void lp1_update(lp1_t *f) {
    float hz = f->cutoff_hz;
    if (hz >= f->fs * 0.5f)
        hz = f->fs * 0.499f;

    float x = 2.0f * M_PI * hz / f->fs;
    float nn = 2.0f - cosf(x);
    float alpha = nn - sqrtf(nn * nn - 1.0f);

    f->a1 = alpha;
    f->b0 = 1.0f - alpha;
}

static void lp1_set_cutoff(lp1_t *f, float hz) {
    f->cutoff_hz = hz;
    lp1_update(f);
}

static float lp1_process_sample(lp1_t *f, float input) {
    if (input == 0.0f && f->output < 0.0000001f) {
        f->output = 0.0f;
    } else {
        f->output = f->b0 * input + f->a1 * f->output;
    }
    return f->output;
}

static void lp1_process(lp1_t *f, float *input, float *output, int len) {
    for (int i = 0; i < len; i++)
        output[i] = lp1_process_sample(f, input[i]);
}

static void lp1_clear(lp1_t *f) {
    f->output = 0.0f;
}

/* ============================================================================
 * HP1 - Exact port from Hp1.h
 * ============================================================================ */

typedef struct {
    float fs;
    float b0, a1;
    float lp_out;
    float cutoff_hz;
    float output;
} hp1_t;

static void hp1_init(hp1_t *f, int samplerate) {
    f->fs = (float)samplerate;
    f->b0 = 1.0f;
    f->a1 = 0.0f;
    f->lp_out = 0.0f;
    f->cutoff_hz = 100.0f;
    f->output = 0.0f;
}

static void hp1_set_samplerate(hp1_t *f, int samplerate) {
    f->fs = (float)samplerate;
}

static void hp1_update(hp1_t *f) {
    float hz = f->cutoff_hz;
    if (hz >= f->fs * 0.5f)
        hz = f->fs * 0.499f;

    float x = 2.0f * M_PI * hz / f->fs;
    float nn = 2.0f - cosf(x);
    float alpha = nn - sqrtf(nn * nn - 1.0f);

    f->a1 = alpha;
    f->b0 = 1.0f - alpha;
}

static void hp1_set_cutoff(hp1_t *f, float hz) {
    f->cutoff_hz = hz;
    hp1_update(f);
}

static float hp1_process_sample(hp1_t *f, float input) {
    if (input == 0.0f && f->lp_out < 0.000001f) {
        f->output = 0.0f;
    } else {
        f->lp_out = f->b0 * input + f->a1 * f->lp_out;
        f->output = input - f->lp_out;
    }
    return f->output;
}

static void hp1_process(hp1_t *f, float *input, float *output, int len) {
    for (int i = 0; i < len; i++)
        output[i] = hp1_process_sample(f, input[i]);
}

static void hp1_clear(hp1_t *f) {
    f->lp_out = 0.0f;
    f->output = 0.0f;
}

/* ============================================================================
 * BIQUAD - Exact port from Biquad.h/cpp for shelf filters
 * ============================================================================ */

typedef enum {
    BIQUAD_LOWSHELF,
    BIQUAD_HIGHSHELF
} biquad_type_t;

typedef struct {
    float fs;
    float fs_inv;
    float gain_db;
    float gain;
    float q;
    float frequency;
    float a0, a1, a2, b0, b1, b2;
    float x1, x2, y, y1, y2;
    biquad_type_t type;
} biquad_t;

static void biquad_update(biquad_t *bq) {
    float Fc = bq->frequency;
    float V = powf(10.0f, fabsf(bq->gain_db) / 20.0f);
    float K = tanf(M_PI * Fc * bq->fs_inv);
    double norm = 1.0;

    if (bq->type == BIQUAD_LOWSHELF) {
        if (bq->gain_db >= 0) {
            norm = 1.0 / (1.0 + sqrtf(2.0f) * K + K * K);
            bq->b0 = (1.0f + sqrtf(2.0f * V) * K + V * K * K) * norm;
            bq->b1 = 2.0f * (V * K * K - 1.0f) * norm;
            bq->b2 = (1.0f - sqrtf(2.0f * V) * K + V * K * K) * norm;
            bq->a1 = 2.0f * (K * K - 1.0f) * norm;
            bq->a2 = (1.0f - sqrtf(2.0f) * K + K * K) * norm;
        } else {
            norm = 1.0 / (1.0 + sqrtf(2.0f * V) * K + V * K * K);
            bq->b0 = (1.0f + sqrtf(2.0f) * K + K * K) * norm;
            bq->b1 = 2.0f * (K * K - 1.0f) * norm;
            bq->b2 = (1.0f - sqrtf(2.0f) * K + K * K) * norm;
            bq->a1 = 2.0f * (V * K * K - 1.0f) * norm;
            bq->a2 = (1.0f - sqrtf(2.0f * V) * K + V * K * K) * norm;
        }
    } else { /* HIGHSHELF */
        if (bq->gain_db >= 0) {
            norm = 1.0 / (1.0 + sqrtf(2.0f) * K + K * K);
            bq->b0 = (V + sqrtf(2.0f * V) * K + K * K) * norm;
            bq->b1 = 2.0f * (K * K - V) * norm;
            bq->b2 = (V - sqrtf(2.0f * V) * K + K * K) * norm;
            bq->a1 = 2.0f * (K * K - 1.0f) * norm;
            bq->a2 = (1.0f - sqrtf(2.0f) * K + K * K) * norm;
        } else {
            norm = 1.0 / (V + sqrtf(2.0f * V) * K + K * K);
            bq->b0 = (1.0f + sqrtf(2.0f) * K + K * K) * norm;
            bq->b1 = 2.0f * (K * K - 1.0f) * norm;
            bq->b2 = (1.0f - sqrtf(2.0f) * K + K * K) * norm;
            bq->a1 = 2.0f * (K * K - V) * norm;
            bq->a2 = (V - sqrtf(2.0f * V) * K + K * K) * norm;
        }
    }
}

static void biquad_init(biquad_t *bq, biquad_type_t type, int samplerate) {
    bq->type = type;
    bq->fs = (float)samplerate;
    bq->fs_inv = 1.0f / bq->fs;
    bq->gain_db = 0.0f;
    bq->gain = 1.0f;
    bq->frequency = bq->fs * 0.25f;
    bq->q = 0.5f;
    bq->x1 = bq->x2 = bq->y = bq->y1 = bq->y2 = 0.0f;
    biquad_update(bq);
}

static void biquad_set_samplerate(biquad_t *bq, int samplerate) {
    bq->fs = (float)samplerate;
    bq->fs_inv = 1.0f / bq->fs;
    biquad_update(bq);
}

static void biquad_set_gain_db(biquad_t *bq, float db) {
    if (db < -60.0f) db = -60.0f;
    if (db > 60.0f) db = 60.0f;
    bq->gain_db = db;
    bq->gain = powf(10.0f, db / 20.0f);
}

static void biquad_set_frequency(biquad_t *bq, float freq) {
    bq->frequency = freq;
    biquad_update(bq);
}

static void biquad_process(biquad_t *bq, float *input, float *output, int len) {
    for (int i = 0; i < len; i++) {
        float x = input[i];
        bq->y = bq->b0 * x + bq->b1 * bq->x1 + bq->b2 * bq->x2
              - bq->a1 * bq->y1 - bq->a2 * bq->y2;
        bq->x2 = bq->x1;
        bq->y2 = bq->y1;
        bq->x1 = x;
        bq->y1 = bq->y;
        output[i] = bq->y;
    }
}

static void biquad_clear(biquad_t *bq) {
    bq->x1 = bq->x2 = bq->y = bq->y1 = bq->y2 = 0.0f;
}

/* ============================================================================
 * MODULATED ALLPASS - Exact port from ModulatedAllpass.h
 * ============================================================================ */

typedef struct {
    float buffer[ALLPASS_BUFFER_SIZE];
    int index;
    uint64_t samples_processed;

    float mod_phase;
    int delay_a;
    int delay_b;
    float gain_a;
    float gain_b;

    int sample_delay;
    float feedback;
    float mod_amount;
    float mod_rate;
    int interpolation_enabled;
    int modulation_enabled;
} mod_allpass_t;

static void mod_allpass_update(mod_allpass_t *ap) {
    ap->mod_phase += ap->mod_rate * MODULATION_UPDATE_RATE;
    if (ap->mod_phase > 1.0f)
        ap->mod_phase = fmodf(ap->mod_phase, 1.0f);

    float mod = sinf(ap->mod_phase * 2.0f * M_PI);

    float mod_amt = ap->mod_amount;
    if (mod_amt >= ap->sample_delay)
        mod_amt = ap->sample_delay - 1;

    float total_delay = ap->sample_delay + mod_amt * mod;
    if (total_delay <= 0.0f)
        total_delay = 1.0f;

    ap->delay_a = (int)total_delay;
    ap->delay_b = (int)total_delay + 1;

    float partial = total_delay - ap->delay_a;
    ap->gain_a = 1.0f - partial;
    ap->gain_b = partial;
}

static void mod_allpass_init(mod_allpass_t *ap) {
    memset(ap->buffer, 0, sizeof(ap->buffer));
    ap->index = ALLPASS_BUFFER_SIZE - 1;
    ap->samples_processed = 0;

    ap->mod_phase = 0.01f + 0.98f * ((float)rand() / (float)RAND_MAX);
    ap->delay_a = 0;
    ap->delay_b = 0;
    ap->gain_a = 0.0f;
    ap->gain_b = 0.0f;

    ap->sample_delay = 100;
    ap->feedback = 0.5f;
    ap->mod_amount = 0.0f;
    ap->mod_rate = 0.0f;
    ap->interpolation_enabled = 1;
    ap->modulation_enabled = 1;

    mod_allpass_update(ap);
}

static void mod_allpass_process_no_mod(mod_allpass_t *ap, float *input, float *output, int count) {
    int delayed_index = ap->index - ap->sample_delay;
    if (delayed_index < 0) delayed_index += ALLPASS_BUFFER_SIZE;

    for (int i = 0; i < count; i++) {
        float buf_out = ap->buffer[delayed_index];
        float in_val = input[i] + buf_out * ap->feedback;

        ap->buffer[ap->index] = in_val;
        output[i] = buf_out - in_val * ap->feedback;

        ap->index++;
        delayed_index++;
        if (ap->index >= ALLPASS_BUFFER_SIZE) ap->index -= ALLPASS_BUFFER_SIZE;
        if (delayed_index >= ALLPASS_BUFFER_SIZE) delayed_index -= ALLPASS_BUFFER_SIZE;
        ap->samples_processed++;
    }
}

static void mod_allpass_process_with_mod(mod_allpass_t *ap, float *input, float *output, int count) {
    for (int i = 0; i < count; i++) {
        if (ap->samples_processed >= MODULATION_UPDATE_RATE) {
            mod_allpass_update(ap);
            ap->samples_processed = 0;
        }

        float buf_out;
        if (ap->interpolation_enabled) {
            int idx_a = ap->index - ap->delay_a;
            int idx_b = ap->index - ap->delay_b;
            if (idx_a < 0) idx_a += ALLPASS_BUFFER_SIZE;
            if (idx_b < 0) idx_b += ALLPASS_BUFFER_SIZE;
            buf_out = ap->buffer[idx_a] * ap->gain_a + ap->buffer[idx_b] * ap->gain_b;
        } else {
            int idx_a = ap->index - ap->delay_a;
            if (idx_a < 0) idx_a += ALLPASS_BUFFER_SIZE;
            buf_out = ap->buffer[idx_a];
        }

        float in_val = input[i] + buf_out * ap->feedback;
        ap->buffer[ap->index] = in_val;
        output[i] = buf_out - in_val * ap->feedback;

        ap->index++;
        if (ap->index >= ALLPASS_BUFFER_SIZE) ap->index -= ALLPASS_BUFFER_SIZE;
        ap->samples_processed++;
    }
}

static void mod_allpass_process(mod_allpass_t *ap, float *input, float *output, int count) {
    if (ap->modulation_enabled)
        mod_allpass_process_with_mod(ap, input, output, count);
    else
        mod_allpass_process_no_mod(ap, input, output, count);
}

static void mod_allpass_clear(mod_allpass_t *ap) {
    memset(ap->buffer, 0, sizeof(ap->buffer));
}

/* ============================================================================
 * ALLPASS DIFFUSER - Exact port from AllpassDiffuser.h
 * ============================================================================ */

typedef struct {
    mod_allpass_t filters[MAX_DIFFUSER_STAGES];
    int delay;
    float mod_rate;
    float seed_values[MAX_DIFFUSER_STAGES * 3];
    int seed;
    float cross_seed;
    int stages;
    int samplerate;
} allpass_diffuser_t;

static void diffuser_update(allpass_diffuser_t *d) {
    for (int i = 0; i < MAX_DIFFUSER_STAGES; i++) {
        float r = d->seed_values[i];
        float scale = powf(10.0f, r) * 0.1f;  /* 0.1 to 1.0 */
        d->filters[i].sample_delay = (int)(d->delay * scale);
        if (d->filters[i].sample_delay < 1)
            d->filters[i].sample_delay = 1;
    }
}

static void diffuser_update_seeds(allpass_diffuser_t *d) {
    random_buffer_generate_cross(d->seed, d->cross_seed,
                                  d->seed_values, MAX_DIFFUSER_STAGES * 3);
    diffuser_update(d);
}

/* Forward declarations */
static void diffuser_set_mod_rate(allpass_diffuser_t *d, float rate);

static void diffuser_init(allpass_diffuser_t *d, int samplerate) {
    d->samplerate = samplerate;
    d->cross_seed = 0.0f;
    d->seed = 23456;
    d->stages = 1;
    d->delay = 100;
    d->mod_rate = 0.0f;

    for (int i = 0; i < MAX_DIFFUSER_STAGES; i++) {
        mod_allpass_init(&d->filters[i]);
    }

    diffuser_update_seeds(d);
}

static void diffuser_set_samplerate(allpass_diffuser_t *d, int samplerate) {
    d->samplerate = samplerate;
    diffuser_set_mod_rate(d, d->mod_rate);
}

static void diffuser_set_seed(allpass_diffuser_t *d, int seed) {
    d->seed = seed;
    diffuser_update_seeds(d);
}

static void diffuser_set_cross_seed(allpass_diffuser_t *d, float cross_seed) {
    d->cross_seed = cross_seed;
    diffuser_update_seeds(d);
}

static void diffuser_set_interpolation(allpass_diffuser_t *d, int enabled) {
    for (int i = 0; i < MAX_DIFFUSER_STAGES; i++)
        d->filters[i].interpolation_enabled = enabled;
}

static void diffuser_set_modulation(allpass_diffuser_t *d, int enabled) {
    for (int i = 0; i < MAX_DIFFUSER_STAGES; i++)
        d->filters[i].modulation_enabled = enabled;
}

static void diffuser_set_delay(allpass_diffuser_t *d, int samples) {
    d->delay = samples;
    diffuser_update(d);
}

static void diffuser_set_feedback(allpass_diffuser_t *d, float fb) {
    for (int i = 0; i < MAX_DIFFUSER_STAGES; i++)
        d->filters[i].feedback = fb;
}

static void diffuser_set_mod_amount(allpass_diffuser_t *d, float amount) {
    for (int i = 0; i < MAX_DIFFUSER_STAGES; i++) {
        float scale = 0.85f + 0.3f * d->seed_values[MAX_DIFFUSER_STAGES + i];
        d->filters[i].mod_amount = amount * scale;
    }
}

static void diffuser_set_mod_rate(allpass_diffuser_t *d, float rate) {
    d->mod_rate = rate;
    for (int i = 0; i < MAX_DIFFUSER_STAGES; i++) {
        float scale = 0.85f + 0.3f * d->seed_values[MAX_DIFFUSER_STAGES * 2 + i];
        d->filters[i].mod_rate = rate * scale / d->samplerate;
    }
}

static void diffuser_process(allpass_diffuser_t *d, float *input, float *output, int count) {
    float temp[BUFFER_SIZE];

    mod_allpass_process(&d->filters[0], input, temp, count);
    for (int i = 1; i < d->stages; i++)
        mod_allpass_process(&d->filters[i], temp, temp, count);

    memcpy(output, temp, count * sizeof(float));
}

static void diffuser_clear(allpass_diffuser_t *d) {
    for (int i = 0; i < MAX_DIFFUSER_STAGES; i++)
        mod_allpass_clear(&d->filters[i]);
}

/* ============================================================================
 * MODULATED DELAY - Exact port from ModulatedDelay.h
 * ============================================================================ */

typedef struct {
    float *buffer;  /* Dynamically allocated */
    int write_index;
    int read_index_a;
    int read_index_b;
    uint64_t samples_processed;

    float mod_phase;
    float gain_a;
    float gain_b;

    int sample_delay;
    float mod_amount;
    float mod_rate;
} mod_delay_t;

static void mod_delay_update(mod_delay_t *d) {
    d->mod_phase += d->mod_rate * MODULATION_UPDATE_RATE;
    if (d->mod_phase > 1.0f)
        d->mod_phase = fmodf(d->mod_phase, 1.0f);

    float mod = sinf(d->mod_phase * 2.0f * M_PI);
    float total_delay = d->sample_delay + d->mod_amount * mod;

    int delay_a = (int)total_delay;
    int delay_b = (int)total_delay + 1;

    float partial = total_delay - delay_a;
    d->gain_a = 1.0f - partial;
    d->gain_b = partial;

    d->read_index_a = d->write_index - delay_a;
    d->read_index_b = d->write_index - delay_b;
    if (d->read_index_a < 0) d->read_index_a += DELAY_BUFFER_SIZE;
    if (d->read_index_b < 0) d->read_index_b += DELAY_BUFFER_SIZE;
}

static void mod_delay_init(mod_delay_t *d) {
    d->buffer = (float*)calloc(DELAY_BUFFER_SIZE, sizeof(float));
    d->write_index = 0;
    d->read_index_a = 0;
    d->read_index_b = 0;
    d->samples_processed = 0;

    d->mod_phase = 0.01f + 0.98f * ((float)rand() / (float)RAND_MAX);
    d->gain_a = 0.0f;
    d->gain_b = 0.0f;

    d->sample_delay = 100;
    d->mod_amount = 0.0f;
    d->mod_rate = 0.0f;

    mod_delay_update(d);
}

static void mod_delay_free(mod_delay_t *d) {
    if (d->buffer) {
        free(d->buffer);
        d->buffer = NULL;
    }
}

static void mod_delay_process(mod_delay_t *d, float *input, float *output, int count) {
    for (int i = 0; i < count; i++) {
        if (d->samples_processed >= MODULATION_UPDATE_RATE) {
            mod_delay_update(d);
            d->samples_processed = 0;
        }

        d->buffer[d->write_index] = input[i];
        output[i] = d->buffer[d->read_index_a] * d->gain_a +
                    d->buffer[d->read_index_b] * d->gain_b;

        d->write_index++;
        d->read_index_a++;
        d->read_index_b++;
        if (d->write_index >= DELAY_BUFFER_SIZE) d->write_index -= DELAY_BUFFER_SIZE;
        if (d->read_index_a >= DELAY_BUFFER_SIZE) d->read_index_a -= DELAY_BUFFER_SIZE;
        if (d->read_index_b >= DELAY_BUFFER_SIZE) d->read_index_b -= DELAY_BUFFER_SIZE;
        d->samples_processed++;
    }
}

static void mod_delay_clear(mod_delay_t *d) {
    if (d->buffer)
        memset(d->buffer, 0, DELAY_BUFFER_SIZE * sizeof(float));
}

/* ============================================================================
 * MULTITAP DELAY - Exact port from MultitapDelay.h
 * ============================================================================ */

typedef struct {
    float *buffer;  /* Dynamically allocated */
    float tap_gains[MAX_TAPS];
    float tap_position[MAX_TAPS];
    float seed_values[MAX_TAPS * 3];

    int write_idx;
    int seed;
    float cross_seed;
    int count;
    float length_samples;
    float decay;
} multitap_delay_t;

static void multitap_update(multitap_delay_t *mt) {
    int s = 0;
    for (int i = 0; i < MAX_TAPS; i++) {
        float phase = mt->seed_values[s++] < 0.5f ? 1.0f : -1.0f;
        mt->tap_gains[i] = db2gain(-20.0f + mt->seed_values[s++] * 20.0f) * phase;
        mt->tap_position[i] = i + mt->seed_values[s++];
    }
}

static void multitap_update_seeds(multitap_delay_t *mt) {
    random_buffer_generate_cross(mt->seed, mt->cross_seed,
                                  mt->seed_values, MAX_TAPS * 3);
    multitap_update(mt);
}

static void multitap_init(multitap_delay_t *mt) {
    mt->buffer = (float*)calloc(DELAY_BUFFER_SIZE, sizeof(float));
    mt->write_idx = 0;
    mt->seed = 0;
    mt->cross_seed = 0.0f;
    mt->count = 1;
    mt->length_samples = 1000.0f;
    mt->decay = 1.0f;

    multitap_update_seeds(mt);
}

static void multitap_free(multitap_delay_t *mt) {
    if (mt->buffer) {
        free(mt->buffer);
        mt->buffer = NULL;
    }
}

static void multitap_set_seed(multitap_delay_t *mt, int seed) {
    mt->seed = seed;
    multitap_update_seeds(mt);
}

static void multitap_set_cross_seed(multitap_delay_t *mt, float cross_seed) {
    mt->cross_seed = cross_seed;
    multitap_update_seeds(mt);
}

static void multitap_set_tap_count(multitap_delay_t *mt, int count) {
    if (count < 1) count = 1;
    mt->count = count;
    multitap_update(mt);
}

static void multitap_set_tap_length(multitap_delay_t *mt, int samples) {
    if (samples < 10) samples = 10;
    mt->length_samples = (float)samples;
    multitap_update(mt);
}

static void multitap_set_tap_decay(multitap_delay_t *mt, float decay) {
    mt->decay = decay;
}

static void multitap_process(multitap_delay_t *mt, float *input, float *output, int count) {
    float length_scaler = mt->length_samples / (float)mt->count;
    float total_gain = 3.0f / sqrtf(1.0f + mt->count);
    total_gain *= (1.0f + mt->decay * 2.0f);

    for (int i = 0; i < count; i++) {
        mt->buffer[mt->write_idx] = input[i];
        output[i] = 0.0f;

        for (int j = 0; j < mt->count; j++) {
            float offset = mt->tap_position[j] * length_scaler;
            float decay_effective = expf(-offset / mt->length_samples * 3.3f) * mt->decay
                                   + (1.0f - mt->decay);
            int read_idx = mt->write_idx - (int)offset;
            if (read_idx < 0) read_idx += DELAY_BUFFER_SIZE;

            output[i] += mt->buffer[read_idx] * mt->tap_gains[j] * decay_effective * total_gain;
        }

        mt->write_idx = (mt->write_idx + 1) % DELAY_BUFFER_SIZE;
    }
}

static void multitap_clear(multitap_delay_t *mt) {
    if (mt->buffer)
        memset(mt->buffer, 0, DELAY_BUFFER_SIZE * sizeof(float));
}

/* ============================================================================
 * CIRCULAR BUFFER - For feedback in delay lines
 * ============================================================================ */

typedef struct {
    float buffer[BUFFER_SIZE * 2];
    int idx_read;
    int idx_write;
    int count;
} circular_buffer_t;

static void circular_init(circular_buffer_t *cb) {
    memset(cb->buffer, 0, sizeof(cb->buffer));
    cb->idx_read = 0;
    cb->idx_write = 0;
    cb->count = 0;
}

static void circular_push(circular_buffer_t *cb, float *data, int size) {
    for (int i = 0; i < size; i++) {
        cb->buffer[cb->idx_write] = data[i];
        cb->idx_write = (cb->idx_write + 1) % (BUFFER_SIZE * 2);
        cb->count++;
        if (cb->count >= BUFFER_SIZE * 2) break;
    }
}

static void circular_pop(circular_buffer_t *cb, float *dest, int size) {
    for (int i = 0; i < size; i++) {
        if (cb->count > 0) {
            dest[i] = cb->buffer[cb->idx_read];
            cb->idx_read = (cb->idx_read + 1) % (BUFFER_SIZE * 2);
            cb->count--;
        } else {
            dest[i] = 0.0f;
        }
    }
}

/* ============================================================================
 * DELAY LINE - Exact port from DelayLine.h
 * ============================================================================ */

typedef struct {
    mod_delay_t delay;
    allpass_diffuser_t diffuser;
    biquad_t low_shelf;
    biquad_t high_shelf;
    lp1_t low_pass;
    circular_buffer_t feedback_buffer;
    float feedback;

    int diffuser_enabled;
    int low_shelf_enabled;
    int high_shelf_enabled;
    int cutoff_enabled;
    int tap_post_diffuser;
    int samplerate;
} delay_line_t;

static void delay_line_init(delay_line_t *dl, int samplerate) {
    dl->samplerate = samplerate;
    mod_delay_init(&dl->delay);
    diffuser_init(&dl->diffuser, samplerate);
    biquad_init(&dl->low_shelf, BIQUAD_LOWSHELF, samplerate);
    biquad_init(&dl->high_shelf, BIQUAD_HIGHSHELF, samplerate);
    lp1_init(&dl->low_pass, samplerate);
    circular_init(&dl->feedback_buffer);

    dl->feedback = 0.0f;

    biquad_set_gain_db(&dl->low_shelf, -20.0f);
    dl->low_shelf.frequency = 20.0f;
    biquad_update(&dl->low_shelf);

    biquad_set_gain_db(&dl->high_shelf, -20.0f);
    dl->high_shelf.frequency = 19000.0f;
    biquad_update(&dl->high_shelf);

    lp1_set_cutoff(&dl->low_pass, 1000.0f);
    diffuser_set_seed(&dl->diffuser, 1);
    diffuser_set_cross_seed(&dl->diffuser, 0.0f);

    dl->diffuser_enabled = 0;
    dl->low_shelf_enabled = 0;
    dl->high_shelf_enabled = 0;
    dl->cutoff_enabled = 0;
    dl->tap_post_diffuser = 0;
}

static void delay_line_free(delay_line_t *dl) {
    mod_delay_free(&dl->delay);
}

static void delay_line_set_samplerate(delay_line_t *dl, int samplerate) {
    dl->samplerate = samplerate;
    diffuser_set_samplerate(&dl->diffuser, samplerate);
    lp1_set_samplerate(&dl->low_pass, samplerate);
    biquad_set_samplerate(&dl->low_shelf, samplerate);
    biquad_set_samplerate(&dl->high_shelf, samplerate);
}

static void delay_line_set_diffuser_seed(delay_line_t *dl, int seed, float cross_seed) {
    diffuser_set_seed(&dl->diffuser, seed);
    diffuser_set_cross_seed(&dl->diffuser, cross_seed);
}

static void delay_line_set_delay(delay_line_t *dl, int samples) {
    dl->delay.sample_delay = samples;
}

static void delay_line_set_feedback(delay_line_t *dl, float fb) {
    dl->feedback = fb;
}

static void delay_line_set_diffuser_delay(delay_line_t *dl, int samples) {
    diffuser_set_delay(&dl->diffuser, samples);
}

static void delay_line_set_diffuser_feedback(delay_line_t *dl, float fb) {
    diffuser_set_feedback(&dl->diffuser, fb);
}

static void delay_line_set_diffuser_stages(delay_line_t *dl, int stages) {
    dl->diffuser.stages = stages;
}

static void delay_line_set_low_shelf_gain(delay_line_t *dl, float db) {
    biquad_set_gain_db(&dl->low_shelf, db);
    biquad_update(&dl->low_shelf);
}

static void delay_line_set_low_shelf_freq(delay_line_t *dl, float freq) {
    dl->low_shelf.frequency = freq;
    biquad_update(&dl->low_shelf);
}

static void delay_line_set_high_shelf_gain(delay_line_t *dl, float db) {
    biquad_set_gain_db(&dl->high_shelf, db);
    biquad_update(&dl->high_shelf);
}

static void delay_line_set_high_shelf_freq(delay_line_t *dl, float freq) {
    dl->high_shelf.frequency = freq;
    biquad_update(&dl->high_shelf);
}

static void delay_line_set_cutoff(delay_line_t *dl, float freq) {
    lp1_set_cutoff(&dl->low_pass, freq);
}

static void delay_line_set_line_mod_amount(delay_line_t *dl, float amount) {
    dl->delay.mod_amount = amount;
}

static void delay_line_set_line_mod_rate(delay_line_t *dl, float rate) {
    dl->delay.mod_rate = rate;
}

static void delay_line_set_diffuser_mod_amount(delay_line_t *dl, float amount) {
    diffuser_set_modulation(&dl->diffuser, amount > 0.0f);
    diffuser_set_mod_amount(&dl->diffuser, amount);
}

static void delay_line_set_diffuser_mod_rate(delay_line_t *dl, float rate) {
    diffuser_set_mod_rate(&dl->diffuser, rate);
}

static void delay_line_set_interpolation(delay_line_t *dl, int enabled) {
    diffuser_set_interpolation(&dl->diffuser, enabled);
}

static void delay_line_process(delay_line_t *dl, float *input, float *output, int count) {
    float temp[BUFFER_SIZE];
    circular_pop(&dl->feedback_buffer, temp, count);

    for (int i = 0; i < count; i++)
        temp[i] = input[i] + temp[i] * dl->feedback;

    mod_delay_process(&dl->delay, temp, temp, count);

    if (!dl->tap_post_diffuser)
        memcpy(output, temp, count * sizeof(float));

    if (dl->diffuser_enabled)
        diffuser_process(&dl->diffuser, temp, temp, count);
    if (dl->low_shelf_enabled)
        biquad_process(&dl->low_shelf, temp, temp, count);
    if (dl->high_shelf_enabled)
        biquad_process(&dl->high_shelf, temp, temp, count);
    if (dl->cutoff_enabled)
        lp1_process(&dl->low_pass, temp, temp, count);

    circular_push(&dl->feedback_buffer, temp, count);

    if (dl->tap_post_diffuser)
        memcpy(output, temp, count * sizeof(float));
}

static void delay_line_clear_diffuser(delay_line_t *dl) {
    diffuser_clear(&dl->diffuser);
}

static void delay_line_clear(delay_line_t *dl) {
    mod_delay_clear(&dl->delay);
    diffuser_clear(&dl->diffuser);
    biquad_clear(&dl->low_shelf);
    biquad_clear(&dl->high_shelf);
    lp1_clear(&dl->low_pass);
    circular_init(&dl->feedback_buffer);
}

/* ============================================================================
 * REVERB CHANNEL - Exact port from ReverbChannel.h
 * ============================================================================ */

typedef struct {
    mod_delay_t predelay;
    multitap_delay_t multitap;
    allpass_diffuser_t diffuser;
    delay_line_t lines[MAX_LINE_COUNT];
    hp1_t high_pass;
    lp1_t low_pass;

    float delay_line_seeds[MAX_LINE_COUNT * 3];
    int delay_line_seed;
    int post_diffusion_seed;
    float cross_seed;

    int line_count;
    int low_cut_enabled;
    int high_cut_enabled;
    int multitap_enabled;
    int diffuser_enabled;

    float input_mix;
    float dry_out;
    float early_out;
    float line_out;

    int is_right;
    int samplerate;
} reverb_channel_t;

static float channel_ms2samples(reverb_channel_t *ch, float ms) {
    return ms / 1000.0f * ch->samplerate;
}

static float channel_get_per_line_gain(reverb_channel_t *ch) {
    return 1.0f / sqrtf((float)ch->line_count);
}

static void channel_update_post_diffusion(reverb_channel_t *ch) {
    for (int i = 0; i < MAX_LINE_COUNT; i++)
        delay_line_set_diffuser_seed(&ch->lines[i],
                                      (ch->post_diffusion_seed) * (i + 1),
                                      ch->cross_seed);
}

static void channel_update_lines(reverb_channel_t *ch,
                                  int line_delay_samples,
                                  float line_decay_samples,
                                  float line_mod_amount,
                                  float line_mod_rate,
                                  float late_diffusion_mod_amount,
                                  float late_diffusion_mod_rate) {
    random_buffer_generate_cross(ch->delay_line_seed, ch->cross_seed,
                                  ch->delay_line_seeds, MAX_LINE_COUNT * 3);

    for (int i = 0; i < MAX_LINE_COUNT; i++) {
        float mod_amt = line_mod_amount * (0.7f + 0.3f * ch->delay_line_seeds[i]);
        float mod_rate = line_mod_rate * (0.7f + 0.3f * ch->delay_line_seeds[MAX_LINE_COUNT + i])
                         / ch->samplerate;

        float delay_samples = (0.5f + 1.0f * ch->delay_line_seeds[MAX_LINE_COUNT * 2 + i])
                              * line_delay_samples;
        if (delay_samples < mod_amt + 2)
            delay_samples = mod_amt + 2;

        float db_per_iteration = delay_samples / line_decay_samples * (-60.0f);
        float gain_per_iteration = db2gain(db_per_iteration);

        delay_line_set_delay(&ch->lines[i], (int)delay_samples);
        delay_line_set_feedback(&ch->lines[i], gain_per_iteration);
        delay_line_set_line_mod_amount(&ch->lines[i], mod_amt);
        delay_line_set_line_mod_rate(&ch->lines[i], mod_rate);
        delay_line_set_diffuser_mod_amount(&ch->lines[i], late_diffusion_mod_amount);
        delay_line_set_diffuser_mod_rate(&ch->lines[i], late_diffusion_mod_rate);
    }
}

static void channel_init(reverb_channel_t *ch, int samplerate, int is_right) {
    ch->samplerate = samplerate;
    ch->is_right = is_right;
    ch->cross_seed = 0.0f;
    ch->line_count = 8;  /* Default from reference */
    ch->delay_line_seed = 12345;
    ch->post_diffusion_seed = 12345;

    mod_delay_init(&ch->predelay);
    multitap_init(&ch->multitap);
    diffuser_init(&ch->diffuser, samplerate);
    hp1_init(&ch->high_pass, samplerate);
    lp1_init(&ch->low_pass, samplerate);

    diffuser_set_interpolation(&ch->diffuser, 1);
    hp1_set_cutoff(&ch->high_pass, 20.0f);
    lp1_set_cutoff(&ch->low_pass, 20000.0f);

    for (int i = 0; i < MAX_LINE_COUNT; i++)
        delay_line_init(&ch->lines[i], samplerate);

    ch->low_cut_enabled = 0;
    ch->high_cut_enabled = 1;
    ch->multitap_enabled = 0;
    ch->diffuser_enabled = 1;

    ch->input_mix = 1.0f;
    ch->dry_out = 0.0f;
    ch->early_out = 0.0f;
    ch->line_out = 1.0f;
}

static void channel_free(reverb_channel_t *ch) {
    mod_delay_free(&ch->predelay);
    multitap_free(&ch->multitap);
    for (int i = 0; i < MAX_LINE_COUNT; i++)
        delay_line_free(&ch->lines[i]);
}

static void channel_set_samplerate(reverb_channel_t *ch, int samplerate) {
    ch->samplerate = samplerate;
    hp1_set_samplerate(&ch->high_pass, samplerate);
    lp1_set_samplerate(&ch->low_pass, samplerate);
    diffuser_set_samplerate(&ch->diffuser, samplerate);

    for (int i = 0; i < MAX_LINE_COUNT; i++)
        delay_line_set_samplerate(&ch->lines[i], samplerate);
}

static void channel_set_cross_seed(reverb_channel_t *ch, float seed_param) {
    /* Exact from reference: Right channel uses 0.5 * seed, Left uses 1 - 0.5 * seed */
    ch->cross_seed = ch->is_right ? 0.5f * seed_param : 1.0f - 0.5f * seed_param;
    multitap_set_cross_seed(&ch->multitap, ch->cross_seed);
    diffuser_set_cross_seed(&ch->diffuser, ch->cross_seed);
}

static void channel_process(reverb_channel_t *ch, float *input, float *output, int count) {
    float temp[BUFFER_SIZE];
    float early_out_buf[BUFFER_SIZE];
    float line_out_buf[BUFFER_SIZE];
    float line_sum[BUFFER_SIZE];

    for (int i = 0; i < count; i++)
        temp[i] = input[i] * ch->input_mix;

    if (ch->low_cut_enabled)
        hp1_process(&ch->high_pass, temp, temp, count);
    if (ch->high_cut_enabled)
        lp1_process(&ch->low_pass, temp, temp, count);

    /* Denormal prevention */
    for (int i = 0; i < count; i++) {
        if (temp[i] * temp[i] < 0.000000001f)
            temp[i] = 0.0f;
    }

    mod_delay_process(&ch->predelay, temp, temp, count);

    if (ch->multitap_enabled)
        multitap_process(&ch->multitap, temp, temp, count);

    if (ch->diffuser_enabled)
        diffuser_process(&ch->diffuser, temp, temp, count);

    memcpy(early_out_buf, temp, count * sizeof(float));
    memset(line_sum, 0, count * sizeof(float));

    for (int i = 0; i < ch->line_count; i++) {
        delay_line_process(&ch->lines[i], temp, line_out_buf, count);
        for (int j = 0; j < count; j++)
            line_sum[j] += line_out_buf[j];
    }

    float per_line_gain = channel_get_per_line_gain(ch);
    for (int i = 0; i < count; i++)
        line_sum[i] *= per_line_gain;

    for (int i = 0; i < count; i++) {
        output[i] = ch->dry_out * input[i]
                  + ch->early_out * early_out_buf[i]
                  + ch->line_out * line_sum[i];
    }
}

static void channel_clear(reverb_channel_t *ch) {
    lp1_clear(&ch->low_pass);
    hp1_clear(&ch->high_pass);
    mod_delay_clear(&ch->predelay);
    multitap_clear(&ch->multitap);
    diffuser_clear(&ch->diffuser);
    for (int i = 0; i < MAX_LINE_COUNT; i++)
        delay_line_clear(&ch->lines[i]);
}

/* ============================================================================
 * V2 API - Instance-based (V1 API removed)
 * ============================================================================ */

static const host_api_v1_t *g_host = NULL;

#define AUDIO_FX_API_VERSION_2 2
#define AUDIO_FX_INIT_V2_SYMBOL "move_audio_fx_init_v2"

typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void (*destroy_instance)(void *instance);
    void (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
} audio_fx_api_v2_t;

typedef audio_fx_api_v2_t* (*audio_fx_init_v2_fn)(const host_api_v1_t *host);

/* Instance structure for v2 API */
typedef struct {
    /* Module directory */
    char module_dir[256];

    /* Parameters (0-1 normalized) */
    float input_mix;
    float predelay;
    float decay;
    float size;
    float diffusion;
    float mix;
    float low_cut;
    float high_cut;
    float cross_seed;
    float mod_rate;
    float mod_amount;

    /* Reverb channels */
    reverb_channel_t *channel_l;
    reverb_channel_t *channel_r;
} cloudseed_instance_t;

static void v2_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[cloudseed-v2] %s", msg);
        g_host->log(buf);
    }
}

static void v2_apply_parameters(cloudseed_instance_t *inst) {
    if (!inst->channel_l || !inst->channel_r) return;

    int samplerate = SAMPLE_RATE;

    /* Pre-delay: 0-500ms using Resp2dec curve */
    float predelay_ms = resp2dec(inst->predelay) * 500.0f;
    int predelay_samples = (int)(predelay_ms / 1000.0f * samplerate);
    if (predelay_samples < 1) predelay_samples = 1;
    inst->channel_l->predelay.sample_delay = predelay_samples;
    inst->channel_r->predelay.sample_delay = predelay_samples;

    /* Room size: 20-1000ms using Resp2dec curve */
    float line_size_ms = 20.0f + resp2dec(inst->size) * 980.0f;
    int line_delay_samples = (int)(line_size_ms / 1000.0f * samplerate);

    /* Decay: 0.05-60 seconds using Resp3dec curve */
    float decay_seconds = 0.05f + resp3dec(inst->decay) * 59.95f;
    float line_decay_samples = decay_seconds * samplerate;

    /* Modulation amounts */
    float line_mod_amount = inst->mod_amount * 2.5f * samplerate / 1000.0f;
    float line_mod_rate = resp2dec(inst->mod_rate) * 5.0f;

    float late_diff_mod_amount = inst->mod_amount * 2.5f * samplerate / 1000.0f;
    float late_diff_mod_rate = resp2dec(inst->mod_rate) * 5.0f;

    /* Update delay lines */
    channel_update_lines(inst->channel_l, line_delay_samples, line_decay_samples,
                          line_mod_amount, line_mod_rate,
                          late_diff_mod_amount, late_diff_mod_rate);
    channel_update_lines(inst->channel_r, line_delay_samples, line_decay_samples,
                          line_mod_amount, line_mod_rate,
                          late_diff_mod_amount, late_diff_mod_rate);

    /* Early diffuser settings */
    int diff_stages = 4 + (int)(inst->diffusion * 7.999f);
    inst->channel_l->diffuser.stages = diff_stages;
    inst->channel_r->diffuser.stages = diff_stages;

    float diff_delay_ms = 10.0f + inst->size * 90.0f;
    int diff_delay = (int)(diff_delay_ms / 1000.0f * samplerate);
    diffuser_set_delay(&inst->channel_l->diffuser, diff_delay);
    diffuser_set_delay(&inst->channel_r->diffuser, diff_delay);

    diffuser_set_feedback(&inst->channel_l->diffuser, inst->diffusion);
    diffuser_set_feedback(&inst->channel_r->diffuser, inst->diffusion);

    float diff_mod_amount = inst->mod_amount * 2.5f * samplerate / 1000.0f;
    diffuser_set_mod_amount(&inst->channel_l->diffuser, diff_mod_amount);
    diffuser_set_mod_amount(&inst->channel_r->diffuser, diff_mod_amount);

    float diff_mod_rate = resp2dec(inst->mod_rate) * 5.0f;
    diffuser_set_mod_rate(&inst->channel_l->diffuser, diff_mod_rate);
    diffuser_set_mod_rate(&inst->channel_r->diffuser, diff_mod_rate);

    /* Input filters */
    float low_cut_hz = 20.0f + resp4oct(inst->low_cut) * 980.0f;
    float high_cut_hz = 400.0f + resp4oct(inst->high_cut) * 19600.0f;
    hp1_set_cutoff(&inst->channel_l->high_pass, low_cut_hz);
    hp1_set_cutoff(&inst->channel_r->high_pass, low_cut_hz);
    lp1_set_cutoff(&inst->channel_l->low_pass, high_cut_hz);
    lp1_set_cutoff(&inst->channel_r->low_pass, high_cut_hz);

    /* Cross seed for stereo */
    channel_set_cross_seed(inst->channel_l, inst->cross_seed);
    channel_set_cross_seed(inst->channel_r, inst->cross_seed);
    channel_update_post_diffusion(inst->channel_l);
    channel_update_post_diffusion(inst->channel_r);

    /* EQ cutoff in delay lines (damping) */
    float eq_cutoff = 400.0f + resp4oct(inst->high_cut * 0.8f) * 19600.0f;
    for (int i = 0; i < MAX_LINE_COUNT; i++) {
        delay_line_set_cutoff(&inst->channel_l->lines[i], eq_cutoff);
        delay_line_set_cutoff(&inst->channel_r->lines[i], eq_cutoff);
        inst->channel_l->lines[i].cutoff_enabled = 1;
        inst->channel_r->lines[i].cutoff_enabled = 1;
    }

    /* Output mix */
    inst->channel_l->dry_out = 0.0f;
    inst->channel_r->dry_out = 0.0f;
    inst->channel_l->line_out = 1.0f;
    inst->channel_r->line_out = 1.0f;
}

static void* v2_create_instance(const char *module_dir, const char *config_json) {
    v2_log("Creating instance");

    cloudseed_instance_t *inst = (cloudseed_instance_t*)calloc(1, sizeof(cloudseed_instance_t));
    if (!inst) {
        v2_log("Failed to allocate instance");
        return NULL;
    }

    if (module_dir) {
        strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    }

    /* Set default parameters */
    inst->input_mix = 1.0f;
    inst->predelay = 0.0f;
    inst->decay = 0.5f;
    inst->size = 0.5f;
    inst->diffusion = 0.7f;
    inst->mix = 0.3f;
    inst->low_cut = 0.0f;
    inst->high_cut = 1.0f;
    inst->cross_seed = 0.5f;
    inst->mod_rate = 0.3f;
    inst->mod_amount = 0.3f;

    /* Allocate reverb channels */
    inst->channel_l = (reverb_channel_t*)malloc(sizeof(reverb_channel_t));
    inst->channel_r = (reverb_channel_t*)malloc(sizeof(reverb_channel_t));

    if (!inst->channel_l || !inst->channel_r) {
        v2_log("Failed to allocate reverb channels");
        if (inst->channel_l) free(inst->channel_l);
        if (inst->channel_r) free(inst->channel_r);
        free(inst);
        return NULL;
    }

    channel_init(inst->channel_l, SAMPLE_RATE, 0);
    channel_init(inst->channel_r, SAMPLE_RATE, 1);

    v2_apply_parameters(inst);

    v2_log("Instance created");
    return inst;
}

static void v2_destroy_instance(void *instance) {
    cloudseed_instance_t *inst = (cloudseed_instance_t*)instance;
    if (!inst) return;

    v2_log("Destroying instance");

    if (inst->channel_l) {
        channel_free(inst->channel_l);
        free(inst->channel_l);
    }
    if (inst->channel_r) {
        channel_free(inst->channel_r);
        free(inst->channel_r);
    }

    free(inst);
}

static void v2_process_block(void *instance, int16_t *audio_inout, int frames) {
    cloudseed_instance_t *inst = (cloudseed_instance_t*)instance;
    if (!inst || !inst->channel_l || !inst->channel_r) return;

    /* Process in chunks of BUFFER_SIZE */
    int offset = 0;
    while (offset < frames) {
        int chunk = frames - offset;
        if (chunk > BUFFER_SIZE) chunk = BUFFER_SIZE;

        float in_l[BUFFER_SIZE];
        float in_r[BUFFER_SIZE];
        float out_l[BUFFER_SIZE];
        float out_r[BUFFER_SIZE];

        /* Convert to float */
        for (int i = 0; i < chunk; i++) {
            in_l[i] = audio_inout[(offset + i) * 2] / 32768.0f;
            in_r[i] = audio_inout[(offset + i) * 2 + 1] / 32768.0f;
        }

        /* Process through reverb channels */
        channel_process(inst->channel_l, in_l, out_l, chunk);
        channel_process(inst->channel_r, in_r, out_r, chunk);

        /* Mix dry and wet, convert back to int16 */
        for (int i = 0; i < chunk; i++) {
            float mixed_l = in_l[i] * (1.0f - inst->mix) + out_l[i] * inst->mix;
            float mixed_r = in_r[i] * (1.0f - inst->mix) + out_r[i] * inst->mix;

            /* Soft clipping */
            if (mixed_l > 1.0f) mixed_l = 1.0f;
            if (mixed_l < -1.0f) mixed_l = -1.0f;
            if (mixed_r > 1.0f) mixed_r = 1.0f;
            if (mixed_r < -1.0f) mixed_r = -1.0f;

            audio_inout[(offset + i) * 2] = (int16_t)(mixed_l * 32767.0f);
            audio_inout[(offset + i) * 2 + 1] = (int16_t)(mixed_r * 32767.0f);
        }

        offset += chunk;
    }
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    cloudseed_instance_t *inst = (cloudseed_instance_t*)instance;
    if (!inst) return;

    int need_update = 0;
    float v = atof(val);

    /* Clamp to 0-1 */
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;

    if (strcmp(key, "decay") == 0) {
        inst->decay = v;
        need_update = 1;
    } else if (strcmp(key, "mix") == 0) {
        inst->mix = v;
    } else if (strcmp(key, "predelay") == 0) {
        inst->predelay = v;
        need_update = 1;
    } else if (strcmp(key, "size") == 0) {
        inst->size = v;
        need_update = 1;
    } else if (strcmp(key, "diffusion") == 0) {
        inst->diffusion = v;
        need_update = 1;
    } else if (strcmp(key, "low_cut") == 0) {
        inst->low_cut = v;
        need_update = 1;
    } else if (strcmp(key, "high_cut") == 0) {
        inst->high_cut = v;
        need_update = 1;
    } else if (strcmp(key, "cross_seed") == 0) {
        inst->cross_seed = v;
        need_update = 1;
    } else if (strcmp(key, "mod_rate") == 0) {
        inst->mod_rate = v;
        need_update = 1;
    } else if (strcmp(key, "mod_amount") == 0) {
        inst->mod_amount = v;
        need_update = 1;
    }

    if (need_update)
        v2_apply_parameters(inst);
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    cloudseed_instance_t *inst = (cloudseed_instance_t*)instance;
    if (!inst) return -1;

    if (strcmp(key, "decay") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->decay);
    } else if (strcmp(key, "mix") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->mix);
    } else if (strcmp(key, "predelay") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->predelay);
    } else if (strcmp(key, "size") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->size);
    } else if (strcmp(key, "diffusion") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->diffusion);
    } else if (strcmp(key, "low_cut") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->low_cut);
    } else if (strcmp(key, "high_cut") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->high_cut);
    } else if (strcmp(key, "cross_seed") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->cross_seed);
    } else if (strcmp(key, "mod_rate") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->mod_rate);
    } else if (strcmp(key, "mod_amount") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst->mod_amount);
    } else if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "CloudSeed");
    }
    return -1;
}

static audio_fx_api_v2_t g_fx_api_v2;

audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_fx_api_v2, 0, sizeof(g_fx_api_v2));
    g_fx_api_v2.api_version = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance = v2_create_instance;
    g_fx_api_v2.destroy_instance = v2_destroy_instance;
    g_fx_api_v2.process_block = v2_process_block;
    g_fx_api_v2.set_param = v2_set_param;
    g_fx_api_v2.get_param = v2_get_param;

    v2_log("CloudSeed v2 plugin initialized");

    return &g_fx_api_v2;
}
