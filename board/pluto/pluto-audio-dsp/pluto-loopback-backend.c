#define _POSIX_C_SOURCE 200809L

#include <complex.h>
#include <errno.h>
#include <iio.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_RX_DEVICE "cf-ad9361-lpc"
#define DEFAULT_TX_DEVICE "cf-ad9361-dds-core-lpc"
#define DEFAULT_I_CHAN "voltage0"
#define DEFAULT_Q_CHAN "voltage1"
#define INT16_MAX_F 32767.0f
#define CW_UNITS_MAX 2048
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signo)
{
    (void)signo;
    keep_running = 0;
}

static const char *env_default(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return (value && value[0]) ? value : fallback;
}

static long env_long(const char *name, long fallback, long minimum, long maximum)
{
    const char *raw = getenv(name);
    char *end = NULL;
    long value = fallback;

    if (raw && raw[0]) {
        errno = 0;
        value = strtol(raw, &end, 10);
        if (errno || end == raw)
            value = fallback;
    }
    if (value < minimum)
        value = minimum;
    if (value > maximum)
        value = maximum;
    return value;
}

static float env_float(const char *name, float fallback, float minimum, float maximum)
{
    const char *raw = getenv(name);
    char *end = NULL;
    float value = fallback;

    if (raw && raw[0]) {
        errno = 0;
        value = strtof(raw, &end);
        if (errno || end == raw)
            value = fallback;
    }
    if (value < minimum)
        value = minimum;
    if (value > maximum)
        value = maximum;
    return value;
}

static int16_t clamp16(float value)
{
    if (value > 32767.0f)
        return 32767;
    if (value < -32768.0f)
        return -32768;
    return (int16_t)value;
}

struct audio_state {
    FILE *file;
    bool file_repeat;
    float current;
    float file_accum;
    float tone_phase;
};

struct cw_state {
    char units[CW_UNITS_MAX];
    int unit_count;
    int unit_pos;
    long sample_in_unit;
    long samples_per_unit;
};

struct tx_config {
    const char *mode;
    const char *audio_source;
    const char *audio_path;
    const char *cw_text;
    long sample_rate;
    long tone_hz;
    long audio_rate;
    long audio_tone_hz;
    long fm_deviation_hz;
    long cw_wpm;
    float amplitude;
    float am_modulation_index;
};

struct tx_state {
    float tone_phase;
    float fm_phase;
    struct audio_state audio;
    struct cw_state cw;
};

static double monotonic_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static bool mode_is(const char *mode, const char *expected)
{
    return strcmp(mode, expected) == 0;
}

static int enable_iq_channels(struct iio_device *dev, const char *i_name,
                              const char *q_name, bool output,
                              struct iio_channel **i_chan,
                              struct iio_channel **q_chan)
{
    *i_chan = iio_device_find_channel(dev, i_name, output);
    *q_chan = iio_device_find_channel(dev, q_name, output);
    if (!*i_chan || !*q_chan)
        return -1;
    iio_channel_enable(*i_chan);
    iio_channel_enable(*q_chan);
    return 0;
}

static void fill_tx_tone(struct iio_buffer *buf, struct iio_channel *i_chan,
                         long sample_rate, long tone_hz, float amplitude)
{
    char *ptr = iio_buffer_first(buf, i_chan);
    char *end = iio_buffer_end(buf);
    ptrdiff_t step = iio_buffer_step(buf);
    float phase = 0.0f;
    float inc = 2.0f * (float)M_PI * (float)tone_hz / (float)sample_rate;
    float level = amplitude * INT16_MAX_F;

    for (; ptr < end; ptr += step) {
        int16_t *sample = (int16_t *)ptr;
        sample[0] = clamp16(cosf(phase) * level);
        sample[1] = clamp16(sinf(phase) * level);
        phase += inc;
        if (phase > 2.0f * (float)M_PI)
            phase -= 2.0f * (float)M_PI;
    }
}

static const char *morse_pattern(char c)
{
    switch (c) {
    case 'A': case 'a': return ".-";
    case 'B': case 'b': return "-...";
    case 'C': case 'c': return "-.-.";
    case 'D': case 'd': return "-..";
    case 'E': case 'e': return ".";
    case 'F': case 'f': return "..-.";
    case 'G': case 'g': return "--.";
    case 'H': case 'h': return "....";
    case 'I': case 'i': return "..";
    case 'J': case 'j': return ".---";
    case 'K': case 'k': return "-.-";
    case 'L': case 'l': return ".-..";
    case 'M': case 'm': return "--";
    case 'N': case 'n': return "-.";
    case 'O': case 'o': return "---";
    case 'P': case 'p': return ".--.";
    case 'Q': case 'q': return "--.-";
    case 'R': case 'r': return ".-.";
    case 'S': case 's': return "...";
    case 'T': case 't': return "-";
    case 'U': case 'u': return "..-";
    case 'V': case 'v': return "...-";
    case 'W': case 'w': return ".--";
    case 'X': case 'x': return "-..-";
    case 'Y': case 'y': return "-.--";
    case 'Z': case 'z': return "--..";
    case '0': return "-----";
    case '1': return ".----";
    case '2': return "..---";
    case '3': return "...--";
    case '4': return "....-";
    case '5': return ".....";
    case '6': return "-....";
    case '7': return "--...";
    case '8': return "---..";
    case '9': return "----.";
    case '.': return ".-.-.-";
    case ',': return "--..--";
    case '?': return "..--..";
    case '/': return "-..-.";
    default: return NULL;
    }
}

static void append_units(struct cw_state *cw, char value, int count)
{
    int i;

    for (i = 0; i < count && cw->unit_count < CW_UNITS_MAX - 1; i++)
        cw->units[cw->unit_count++] = value;
}

static void build_cw_units(struct cw_state *cw, const char *text, long sample_rate, long wpm)
{
    size_t i;

    memset(cw, 0, sizeof(*cw));
    if (!text || !text[0])
        text = "CQ PLUTO";
    if (wpm < 5)
        wpm = 5;
    cw->samples_per_unit = (sample_rate * 1200L) / (wpm * 1000L);
    if (cw->samples_per_unit < 1)
        cw->samples_per_unit = 1;

    for (i = 0; text[i] && cw->unit_count < CW_UNITS_MAX - 16; i++) {
        const char *pattern;
        size_t j;

        if (text[i] == ' ') {
            append_units(cw, '0', 7);
            continue;
        }
        pattern = morse_pattern(text[i]);
        if (!pattern)
            continue;
        for (j = 0; pattern[j]; j++) {
            append_units(cw, '1', pattern[j] == '-' ? 3 : 1);
            if (pattern[j + 1])
                append_units(cw, '0', 1);
        }
        append_units(cw, '0', 3);
    }
    append_units(cw, '0', 7);
    if (cw->unit_count == 0)
        append_units(cw, '1', 1);
}

static bool cw_next_key(struct cw_state *cw)
{
    bool keyed;

    if (cw->unit_count <= 0)
        return true;
    keyed = cw->units[cw->unit_pos] == '1';
    cw->sample_in_unit++;
    if (cw->sample_in_unit >= cw->samples_per_unit) {
        cw->sample_in_unit = 0;
        cw->unit_pos++;
        if (cw->unit_pos >= cw->unit_count)
            cw->unit_pos = 0;
    }
    return keyed;
}

static int audio_open(struct audio_state *audio, const struct tx_config *cfg)
{
    memset(audio, 0, sizeof(*audio));
    audio->file_repeat = true;
    if (strcmp(cfg->audio_source, "file") != 0)
        return 0;
    audio->file = fopen(cfg->audio_path, "rb");
    if (!audio->file) {
        fprintf(stderr, "could not open TX audio path: %s\n", cfg->audio_path);
        return -1;
    }
    return 0;
}

static float audio_read_file_sample(struct audio_state *audio)
{
    unsigned char raw[2];
    int16_t sample;

    if (!audio->file)
        return 0.0f;
    if (fread(raw, 1, sizeof(raw), audio->file) != sizeof(raw)) {
        if (!audio->file_repeat || fseek(audio->file, 0, SEEK_SET) != 0)
            return 0.0f;
        if (fread(raw, 1, sizeof(raw), audio->file) != sizeof(raw))
            return 0.0f;
    }
    sample = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    return (float)sample / 32768.0f;
}

static float audio_next_sample(struct audio_state *audio, const struct tx_config *cfg)
{
    float sample;

    if (strcmp(cfg->audio_source, "file") == 0) {
        audio->file_accum += (float)cfg->audio_rate / (float)cfg->sample_rate;
        while (audio->file_accum >= 1.0f) {
            audio->current = audio_read_file_sample(audio);
            audio->file_accum -= 1.0f;
        }
        return audio->current;
    }

    sample = sinf(audio->tone_phase);
    audio->tone_phase += 2.0f * (float)M_PI * (float)cfg->audio_tone_hz / (float)cfg->sample_rate;
    if (audio->tone_phase > 2.0f * (float)M_PI)
        audio->tone_phase -= 2.0f * (float)M_PI;
    return sample;
}

static void fill_tx_modulated(struct iio_buffer *buf, struct iio_channel *i_chan,
                              const struct tx_config *cfg, struct tx_state *state)
{
    char *ptr = iio_buffer_first(buf, i_chan);
    char *end = iio_buffer_end(buf);
    ptrdiff_t step = iio_buffer_step(buf);
    float level = cfg->amplitude * INT16_MAX_F;

    for (; ptr < end; ptr += step) {
        int16_t *sample = (int16_t *)ptr;
        float i_val = 0.0f;
        float q_val = 0.0f;

        if (mode_is(cfg->mode, "carrier")) {
            i_val = level;
        } else if (mode_is(cfg->mode, "am")) {
            float audio = audio_next_sample(&state->audio, cfg);
            float envelope = (1.0f + cfg->am_modulation_index * audio) / (1.0f + cfg->am_modulation_index);
            if (envelope < 0.0f)
                envelope = 0.0f;
            i_val = level * envelope;
        } else if (mode_is(cfg->mode, "fm")) {
            float audio = audio_next_sample(&state->audio, cfg);
            state->fm_phase += 2.0f * (float)M_PI * (float)cfg->fm_deviation_hz * audio / (float)cfg->sample_rate;
            if (state->fm_phase > 2.0f * (float)M_PI)
                state->fm_phase -= 2.0f * (float)M_PI;
            if (state->fm_phase < -2.0f * (float)M_PI)
                state->fm_phase += 2.0f * (float)M_PI;
            i_val = cosf(state->fm_phase) * level;
            q_val = sinf(state->fm_phase) * level;
        } else if (mode_is(cfg->mode, "cw")) {
            if (cw_next_key(&state->cw))
                i_val = level;
        } else {
            i_val = cosf(state->tone_phase) * level;
            q_val = sinf(state->tone_phase) * level;
            state->tone_phase += 2.0f * (float)M_PI * (float)cfg->tone_hz / (float)cfg->sample_rate;
            if (state->tone_phase > 2.0f * (float)M_PI)
                state->tone_phase -= 2.0f * (float)M_PI;
        }

        sample[0] = clamp16(i_val);
        sample[1] = clamp16(q_val);
    }
}

static int run_loopback(void)
{
    const char *mode = env_default("PLUTO_TX_MODE", "loopback");
    const char *rx_device_name = env_default("PLUTO_LOOPBACK_RX_DEVICE", DEFAULT_RX_DEVICE);
    const char *tx_device_name = env_default("PLUTO_LOOPBACK_TX_DEVICE", DEFAULT_TX_DEVICE);
    const char *i_name = env_default("PLUTO_LOOPBACK_I_CHANNEL", DEFAULT_I_CHAN);
    const char *q_name = env_default("PLUTO_LOOPBACK_Q_CHANNEL", DEFAULT_Q_CHAN);
    long sample_rate = env_long("PLUTO_LOOPBACK_SAMPLE_RATE_HZ", 1000000, 520000, 61440000);
    long duration_ms = env_long("PLUTO_LOOPBACK_DURATION_MS", 1000, 50, 10000);
    long tone_hz = env_long("PLUTO_LOOPBACK_TONE_HZ", 10000, 0, sample_rate / 4);
    long buffer_samples = env_long("PLUTO_LOOPBACK_BUFFER_SAMPLES", 4096, 256, 65536);
    float amplitude = env_float("PLUTO_LOOPBACK_TX_AMPLITUDE", 0.05f, 0.0f, 0.25f);
    bool tx_only = strcmp(mode, "loopback") != 0 || strcmp(env_default("PLUTO_TX_ONLY", "0"), "1") == 0;
    struct tx_config tx_cfg = {
        .mode = strcmp(mode, "tx_only") == 0 ? "tone" : mode,
        .audio_source = env_default("PLUTO_TX_AUDIO_SOURCE", "tone"),
        .audio_path = env_default("PLUTO_TX_AUDIO_PATH", ""),
        .cw_text = env_default("PLUTO_TX_CW_TEXT", "CQ PLUTO"),
        .sample_rate = sample_rate,
        .tone_hz = tone_hz,
        .audio_rate = env_long("PLUTO_TX_AUDIO_RATE_HZ", 8000, 8000, 48000),
        .audio_tone_hz = env_long("PLUTO_TX_AUDIO_TONE_HZ", 1000, 20, 3000),
        .fm_deviation_hz = env_long("PLUTO_TX_FM_DEVIATION_HZ", 5000, 100, 25000),
        .cw_wpm = env_long("PLUTO_TX_CW_WPM", 12, 5, 40),
        .amplitude = amplitude,
        .am_modulation_index = env_float("PLUTO_TX_AM_INDEX", 0.8f, 0.0f, 1.0f),
    };
    struct tx_state tx_state;
    struct iio_context *ctx = NULL;
    struct iio_device *rx_dev = NULL;
    struct iio_device *tx_dev = NULL;
    struct iio_channel *rx_i = NULL;
    struct iio_channel *rx_q = NULL;
    struct iio_channel *tx_i = NULL;
    struct iio_channel *tx_q = NULL;
    struct iio_buffer *rx_buf = NULL;
    struct iio_buffer *tx_buf = NULL;
    double started = 0.0;
    double sumsq = 0.0;
    double peak = 0.0;
    unsigned long long samples = 0;
    int ret = 1;

    memset(&tx_state, 0, sizeof(tx_state));
    build_cw_units(&tx_state.cw, tx_cfg.cw_text, sample_rate, tx_cfg.cw_wpm);
    if (tx_only && audio_open(&tx_state.audio, &tx_cfg) < 0)
        goto out;

    ctx = iio_create_default_context();
    if (!ctx) {
        fprintf(stderr, "could not create IIO context\n");
        goto out;
    }

    tx_dev = iio_context_find_device(ctx, tx_device_name);
    rx_dev = tx_only ? NULL : iio_context_find_device(ctx, rx_device_name);
    if ((!rx_dev && !tx_only) || !tx_dev) {
        fprintf(stderr, "could not find loopback devices rx=%s tx=%s\n", rx_device_name, tx_device_name);
        goto out;
    }
    if (!tx_only && enable_iq_channels(rx_dev, i_name, q_name, false, &rx_i, &rx_q) < 0) {
        fprintf(stderr, "could not enable RX channels %s/%s\n", i_name, q_name);
        goto out;
    }
    if (enable_iq_channels(tx_dev, i_name, q_name, true, &tx_i, &tx_q) < 0) {
        fprintf(stderr, "could not enable TX channels %s/%s\n", i_name, q_name);
        goto out;
    }

    tx_buf = iio_device_create_buffer(tx_dev, (size_t)buffer_samples, !tx_only);
    rx_buf = tx_only ? NULL : iio_device_create_buffer(rx_dev, (size_t)buffer_samples, false);
    if (!tx_buf || (!rx_buf && !tx_only)) {
        fprintf(stderr, "could not create loopback IIO buffers\n");
        goto out;
    }
    if (tx_only)
        fill_tx_modulated(tx_buf, tx_i, &tx_cfg, &tx_state);
    else
        fill_tx_tone(tx_buf, tx_i, sample_rate, tone_hz, amplitude);
    if (iio_buffer_push(tx_buf) < 0) {
        fprintf(stderr, "TX buffer push failed\n");
        goto out;
    }

    started = monotonic_seconds();
    while (keep_running && ((monotonic_seconds() - started) * 1000.0) < (double)duration_ms) {
        ssize_t refill;
        char *ptr;
        char *end;
        ptrdiff_t step;

        if (tx_only) {
            fill_tx_modulated(tx_buf, tx_i, &tx_cfg, &tx_state);
            if (iio_buffer_push(tx_buf) < 0) {
                fprintf(stderr, "TX buffer push failed\n");
                goto out;
            }
            continue;
        }
        refill = iio_buffer_refill(rx_buf);
        if (refill < 0) {
            fprintf(stderr, "RX buffer refill failed: %zd\n", refill);
            goto out;
        }
        ptr = iio_buffer_first(rx_buf, rx_i);
        end = iio_buffer_end(rx_buf);
        step = iio_buffer_step(rx_buf);
        for (; ptr < end; ptr += step) {
            const int16_t *sample = (const int16_t *)ptr;
            double i_val = (double)sample[0] / 32768.0;
            double q_val = (double)sample[1] / 32768.0;
            double mag2 = i_val * i_val + q_val * q_val;
            double mag = sqrt(mag2);

            sumsq += mag2;
            if (mag > peak)
                peak = mag;
            samples++;
        }
    }

    ret = 0;

out:
    if (tx_buf)
        iio_buffer_destroy(tx_buf);
    if (rx_buf)
        iio_buffer_destroy(rx_buf);
    if (tx_state.audio.file)
        fclose(tx_state.audio.file);
    if (ctx)
        iio_context_destroy(ctx);

    if (ret == 0 && samples > 0) {
        double rms = sqrt(sumsq / (double)samples);
        double rms_dbfs = 20.0 * log10(rms > 1.0e-9 ? rms : 1.0e-9);
        double peak_dbfs = 20.0 * log10(peak > 1.0e-9 ? peak : 1.0e-9);

        printf("{\"ok\":true,\"samples\":%llu,\"duration_ms\":%ld,"
               "\"sample_rate_hz\":%ld,\"tone_hz\":%ld,"
               "\"tx_amplitude\":%.6f,\"rx_rms_dbfs\":%.2f,"
               "\"rx_peak_dbfs\":%.2f}\n",
               samples, duration_ms, sample_rate, tone_hz,
               amplitude, rms_dbfs, peak_dbfs);
    } else if (ret == 0) {
        printf("{\"ok\":true,\"mode\":\"tx_only\",\"tx_mode\":\"%s\",\"duration_ms\":%ld,"
               "\"sample_rate_hz\":%ld,\"tone_hz\":%ld,"
               "\"tx_amplitude\":%.6f,\"audio_source\":\"%s\","
               "\"audio_rate_hz\":%ld,\"audio_tone_hz\":%ld,"
               "\"fm_deviation_hz\":%ld,\"am_modulation_index\":%.3f,"
               "\"cw_wpm\":%ld}\n",
               tx_cfg.mode, duration_ms, sample_rate, tone_hz,
               amplitude, tx_cfg.audio_source, tx_cfg.audio_rate,
               tx_cfg.audio_tone_hz, tx_cfg.fm_deviation_hz,
               tx_cfg.am_modulation_index, tx_cfg.cw_wpm);
    }
    return ret;
}

int main(void)
{
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    return run_loopback();
}
