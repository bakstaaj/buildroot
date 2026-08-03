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

#include <ft8/constants.h>
#include <ft8/encode.h>
#include <ft8/message.h>

#define DEFAULT_RX_DEVICE "cf-ad9361-lpc"
#define DEFAULT_TX_DEVICE "cf-ad9361-dds-core-lpc"
#define DEFAULT_I_CHAN "voltage0"
#define DEFAULT_Q_CHAN "voltage1"
#define INT16_MAX_F 32767.0f
#define CW_UNITS_MAX 2048
#define CW_DECODE_MAX_MS 30000
#define CW_DECODE_SYMBOL_MAX 16
#define CW_DECODE_TEXT_MAX 128
#define CW_DECODE_RUNS_MAX 512
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signo)
{
    (void)signo;
    keep_running = 0;
}

static int16_t unpack_ad9361_s12_sample(const void *ptr)
{
    uint16_t raw;
    int32_t value;

    memcpy(&raw, ptr, sizeof(raw));
    value = (int32_t)(raw & 0x0fff);
    if (value & 0x0800)
        value -= 0x1000;
    return (int16_t)(value << 4);
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
    unsigned long long samples_read;
    unsigned long long eof_rewinds;
};

struct cw_state {
    char units[CW_UNITS_MAX];
    int unit_count;
    int unit_pos;
    long sample_in_unit;
    long samples_per_unit;
};

struct ft8_state {
    uint8_t tones[FT8_NN];
    float *pulse;
    long slot_sample;
    long slot_samples;
    long signal_start_sample;
    long signal_samples;
    long samples_per_symbol;
    float phase;
    bool valid;
};

#define FT8_GFSK_CONST_K 5.336446f
#define FT8_GFSK_BT 2.0f

struct tx_config {
    const char *mode;
    const char *audio_source;
    const char *audio_path;
    const char *cw_text;
    const char *ft8_text;
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
    struct ft8_state ft8;
};

struct tx_output_metrics {
    unsigned long long sample_count;
    unsigned long long transition_count;
    unsigned long long audio_sample_count;
    unsigned long long audio_crossing_count;
    bool audio_have_last;
    float audio_last;
    double audio_sumsq;
    double audio_peak;
    bool have_last;
    int16_t last_i;
    int16_t last_q;
    double sumsq;
    double peak;
};

static void ft8_init(struct ft8_state *state, const char *text, long sample_rate)
{
    ftx_message_t message;
    struct timespec now;
    long long utc_samples;

    memset(state, 0, sizeof(*state));
    if (ftx_message_encode(&message, NULL, text) != FTX_MESSAGE_RC_OK)
        return;
    ft8_encode(message.payload, state->tones);
    state->samples_per_symbol = (long)(FT8_SYMBOL_PERIOD * (float)sample_rate + 0.5f);
    state->pulse = malloc((size_t)(3 * state->samples_per_symbol) * sizeof(*state->pulse));
    if (!state->pulse)
        return;
    for (long i = 0; i < 3 * state->samples_per_symbol; i++) {
        float t = (float)i / (float)state->samples_per_symbol - 1.5f;
        float arg1 = FT8_GFSK_CONST_K * FT8_GFSK_BT * (t + 0.5f);
        float arg2 = FT8_GFSK_CONST_K * FT8_GFSK_BT * (t - 0.5f);
        state->pulse[i] = (erff(arg1) - erff(arg2)) * 0.5f;
    }
    state->signal_samples = FT8_NN * state->samples_per_symbol;
    state->slot_samples = 15L * sample_rate;
    state->signal_start_sample = (state->slot_samples - state->signal_samples) / 2;
    clock_gettime(CLOCK_REALTIME, &now);
    utc_samples = (long long)now.tv_sec * sample_rate +
        ((long long)now.tv_nsec * sample_rate) / 1000000000LL;
    state->slot_sample = (long)(utc_samples % state->slot_samples);
    state->valid = true;
}

static void ft8_next_iq(struct ft8_state *state, long sample_rate, float level,
                        float base_frequency_hz, float *i_val, float *q_val)
{
    long signal_sample = state->slot_sample - state->signal_start_sample;

    *i_val = 0.0f;
    *q_val = 0.0f;
    if (state->valid && signal_sample >= 0 && signal_sample < state->signal_samples) {
        long n = state->samples_per_symbol;
        long x = signal_sample + n;
        int last_symbol = (int)(x / n);
        int first_symbol = last_symbol - 2;
        float shaped_tone = 0.0f;
        float envelope = 1.0f;
        int symbol;

        if (first_symbol < 0)
            first_symbol = 0;
        if (last_symbol >= FT8_NN)
            last_symbol = FT8_NN - 1;
        for (symbol = first_symbol; symbol <= last_symbol; symbol++) {
            long pulse_index = x - (long)symbol * n;
            if (pulse_index >= 0 && pulse_index < 3 * n)
                shaped_tone += state->tones[symbol] * state->pulse[pulse_index];
        }
        if (x < 2 * n)
            shaped_tone += state->tones[0] * state->pulse[x + n];
        if (x >= state->signal_samples)
            shaped_tone += state->tones[FT8_NN - 1] * state->pulse[x - state->signal_samples];
        if (signal_sample < n / 8)
            envelope = (1.0f - cosf((float)M_PI * signal_sample / (float)(n / 8))) * 0.5f;
        else if (signal_sample >= state->signal_samples - n / 8) {
            long remaining = state->signal_samples - 1 - signal_sample;
            envelope = (1.0f - cosf((float)M_PI * remaining / (float)(n / 8))) * 0.5f;
        }
        *i_val = cosf(state->phase) * level * envelope;
        *q_val = sinf(state->phase) * level * envelope;
        state->phase += 2.0f * (float)M_PI * base_frequency_hz / (float)sample_rate +
            2.0f * (float)M_PI * shaped_tone / (float)n;
        if (state->phase > 2.0f * (float)M_PI)
            state->phase -= 2.0f * (float)M_PI;
    }
    state->slot_sample++;
    if (state->slot_sample >= state->slot_samples) {
        state->slot_sample = 0;
        state->phase = 0.0f;
    }
}

struct tone_metric {
    double i;
    double q;
    double freq_hz;
};

struct demod_metrics {
    bool enabled;
    bool have_last;
    float last_i;
    float last_q;
    long sample_rate;
    long tone_hz;
    long carrier_offset_hz;
    long sample_index;
    double sumsq;
    double peak;
    struct tone_metric tone;
    struct tone_metric refs[3];
};

struct cw_decode_metrics {
    double ms_sum[CW_DECODE_MAX_MS];
    double ms_i[CW_DECODE_MAX_MS];
    double ms_q[CW_DECODE_MAX_MS];
    unsigned ms_count[CW_DECODE_MAX_MS];
    int ms_bins;
    long sample_rate;
    long requested_wpm;
    long carrier_offset_hz;
    double expected_keyed_percent;
    double peak;
    double floor;
    double threshold;
    double keyed_percent;
    int keying_segments;
    int estimated_unit_ms;
    int estimated_wpm;
    char text[CW_DECODE_TEXT_MAX];
    char symbols[CW_DECODE_TEXT_MAX * 8];
    char runs[CW_DECODE_RUNS_MAX];
    bool enabled;
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

static char morse_decode_char(const char *pattern)
{
    if (!pattern || !pattern[0])
        return 0;
    if (!strcmp(pattern, ".-")) return 'A';
    if (!strcmp(pattern, "-...")) return 'B';
    if (!strcmp(pattern, "-.-.")) return 'C';
    if (!strcmp(pattern, "-..")) return 'D';
    if (!strcmp(pattern, ".")) return 'E';
    if (!strcmp(pattern, "..-.")) return 'F';
    if (!strcmp(pattern, "--.")) return 'G';
    if (!strcmp(pattern, "....")) return 'H';
    if (!strcmp(pattern, "..")) return 'I';
    if (!strcmp(pattern, ".---")) return 'J';
    if (!strcmp(pattern, "-.-")) return 'K';
    if (!strcmp(pattern, ".-..")) return 'L';
    if (!strcmp(pattern, "--")) return 'M';
    if (!strcmp(pattern, "-.")) return 'N';
    if (!strcmp(pattern, "---")) return 'O';
    if (!strcmp(pattern, ".--.")) return 'P';
    if (!strcmp(pattern, "--.-")) return 'Q';
    if (!strcmp(pattern, ".-.")) return 'R';
    if (!strcmp(pattern, "...")) return 'S';
    if (!strcmp(pattern, "-")) return 'T';
    if (!strcmp(pattern, "..-")) return 'U';
    if (!strcmp(pattern, "...-")) return 'V';
    if (!strcmp(pattern, ".--")) return 'W';
    if (!strcmp(pattern, "-..-")) return 'X';
    if (!strcmp(pattern, "-.--")) return 'Y';
    if (!strcmp(pattern, "--..")) return 'Z';
    if (!strcmp(pattern, "-----")) return '0';
    if (!strcmp(pattern, ".----")) return '1';
    if (!strcmp(pattern, "..---")) return '2';
    if (!strcmp(pattern, "...--")) return '3';
    if (!strcmp(pattern, "....-")) return '4';
    if (!strcmp(pattern, ".....")) return '5';
    if (!strcmp(pattern, "-....")) return '6';
    if (!strcmp(pattern, "--...")) return '7';
    if (!strcmp(pattern, "---..")) return '8';
    if (!strcmp(pattern, "----.")) return '9';
    if (!strcmp(pattern, ".-.-.-")) return '.';
    if (!strcmp(pattern, "--..--")) return ',';
    if (!strcmp(pattern, "..--..")) return '?';
    if (!strcmp(pattern, "-..-.")) return '/';
    return '?';
}

static void append_text_char(char *text, size_t text_size, char value)
{
    size_t len = strlen(text);

    if (len + 1 >= text_size)
        return;
    text[len] = value;
    text[len + 1] = '\0';
}

static void append_symbol_text(char *symbols, size_t symbols_size, const char *value)
{
    size_t len = strlen(symbols);
    size_t value_len = strlen(value);

    if (len + value_len >= symbols_size)
        return;
    memcpy(symbols + len, value, value_len + 1);
}

static void append_run_text(char *runs, size_t runs_size, bool keyed, int run_ms, int units)
{
    char part[32];

    snprintf(part, sizeof(part), "%c%dms/%du ", keyed ? 'M' : 'S', run_ms, units);
    append_symbol_text(runs, runs_size, part);
}

static bool decoded_text_matches_expected(const char *decoded, const char *expected)
{
    size_t expected_len;

    if (!decoded || !expected || !expected[0])
        return false;
    expected_len = strlen(expected);
    if (strncmp(decoded, expected, expected_len) != 0)
        return false;
    return decoded[expected_len] == '\0' || decoded[expected_len] == ' ';
}

static int units_from_ms(int run_ms, int unit_ms)
{
    if (unit_ms < 1)
        unit_ms = 1;
    return (run_ms + unit_ms / 2) / unit_ms;
}

static int compare_int_values(const void *left, const void *right)
{
    int a = *(const int *)left;
    int b = *(const int *)right;

    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

static int average_short_run_cluster(int *runs, int count)
{
    int limit;
    int sum = 0;
    int used = 0;
    int i;

    if (count <= 0)
        return 0;
    qsort(runs, (size_t)count, sizeof(runs[0]), compare_int_values);
    limit = runs[0] + runs[0] / 2 + 20;
    for (i = 0; i < count; i++) {
        if (runs[i] > limit)
            break;
        sum += runs[i];
        used++;
    }
    return used > 0 ? (sum + used / 2) / used : runs[0];
}

static void smooth_keying(const unsigned char *raw, unsigned char *smooth, int count, int radius_ms)
{
    int i;

    if (radius_ms < 3)
        radius_ms = 3;
    for (i = 0; i < count; i++) {
        int start = i - radius_ms;
        int end = i + radius_ms;
        int keyed_count = 0;
        int total = 0;
        int j;

        if (start < 0)
            start = 0;
        if (end >= count)
            end = count - 1;
        for (j = start; j <= end; j++) {
            keyed_count += raw[j] ? 1 : 0;
            total++;
        }
        smooth[i] = keyed_count * 2 >= total ? 1 : 0;
    }
}

static bool maybe_invert_keying(unsigned char *keyed, int count, int unit_ms)
{
    int longest_mark_ms = 0;
    int longest_space_ms = 0;
    int run_start = 0;
    bool run_keyed;
    int j;

    if (count <= 0)
        return false;
    run_keyed = keyed[0] != 0;
    for (j = 1; j <= count; j++) {
        bool next_keyed = j < count ? keyed[j] != 0 : !run_keyed;
        if (j < count && next_keyed == run_keyed)
            continue;
        if (run_keyed) {
            if (j - run_start > longest_mark_ms)
                longest_mark_ms = j - run_start;
        } else if (j - run_start > longest_space_ms) {
            longest_space_ms = j - run_start;
        }
        run_start = j;
        run_keyed = next_keyed;
    }
    if (longest_mark_ms >= unit_ms * 6 && longest_space_ms < unit_ms * 5) {
        for (j = 0; j < count; j++)
            keyed[j] = keyed[j] ? 0 : 1;
        return true;
    }
    return false;
}

static int estimate_cw_unit_ms(const unsigned char *keyed, int count, int fallback_unit_ms)
{
    int mark_runs[128];
    int space_runs[128];
    int mark_count = 0;
    int space_count = 0;
    int min_run_ms;
    int run_start = 0;
    bool run_keyed;
    int mark_unit;
    int space_unit;
    int unit;
    int min_unit;
    int max_unit;
    int j;

    if (count <= 0)
        return fallback_unit_ms;
    if (fallback_unit_ms < 10)
        fallback_unit_ms = 100;
    min_run_ms = fallback_unit_ms / 4;
    if (min_run_ms < 12)
        min_run_ms = 12;

    run_keyed = keyed[0] != 0;
    for (j = 1; j <= count; j++) {
        bool next_keyed = j < count ? keyed[j] != 0 : !run_keyed;
        int run_ms;

        if (j < count && next_keyed == run_keyed)
            continue;
        run_ms = j - run_start;
        if (run_ms >= min_run_ms) {
            if (run_keyed && mark_count < (int)(sizeof(mark_runs) / sizeof(mark_runs[0]))) {
                mark_runs[mark_count++] = run_ms;
            } else if (!run_keyed && space_count < (int)(sizeof(space_runs) / sizeof(space_runs[0]))) {
                space_runs[space_count++] = run_ms;
            }
        }
        run_start = j;
        run_keyed = next_keyed;
    }

    mark_unit = average_short_run_cluster(mark_runs, mark_count);
    space_unit = average_short_run_cluster(space_runs, space_count);
    if (mark_unit > 0 && space_unit > 0)
        unit = (mark_unit + space_unit + 1) / 2;
    else if (mark_unit > 0)
        unit = mark_unit;
    else if (space_unit > 0)
        unit = space_unit;
    else
        unit = fallback_unit_ms;

    if (unit < 20)
        unit = 20;
    if (unit > 300)
        unit = 300;
    min_unit = (fallback_unit_ms * 3) / 4;
    max_unit = (fallback_unit_ms * 3) / 2;
    if (min_unit < 20)
        min_unit = 20;
    if (max_unit > 300)
        max_unit = 300;
    if (unit < min_unit)
        unit = min_unit;
    if (unit > max_unit)
        unit = max_unit;
    return unit;
}

static void cw_flush_symbol(struct cw_decode_metrics *cw, char *pattern, int *pattern_len)
{
    char decoded;

    if (*pattern_len <= 0)
        return;
    pattern[*pattern_len] = '\0';
    decoded = morse_decode_char(pattern);
    append_text_char(cw->text, sizeof(cw->text), decoded ? decoded : '?');
    append_symbol_text(cw->symbols, sizeof(cw->symbols), pattern);
    append_symbol_text(cw->symbols, sizeof(cw->symbols), " ");
    *pattern_len = 0;
}

static double cw_expected_keyed_percent(const struct cw_state *cw)
{
    int keyed = 0;
    int i;

    if (!cw || cw->unit_count <= 0)
        return 50.0;
    for (i = 0; i < cw->unit_count; i++) {
        if (cw->units[i] == '1')
            keyed++;
    }
    return 100.0 * (double)keyed / (double)cw->unit_count;
}

static void cw_decode_init(struct cw_decode_metrics *cw, const char *mode, long sample_rate,
                           long requested_wpm, long carrier_offset_hz, double expected_keyed_percent)
{
    memset(cw, 0, sizeof(*cw));
    cw->enabled = mode_is(mode, "cw");
    cw->sample_rate = sample_rate;
    cw->requested_wpm = requested_wpm;
    cw->carrier_offset_hz = carrier_offset_hz;
    cw->expected_keyed_percent = expected_keyed_percent;
}

static void cw_decode_add(struct cw_decode_metrics *cw, double i_val, double q_val, unsigned long long sample_index)
{
    long samples_per_ms;
    double phase;
    double c;
    double s;
    int bin;

    if (!cw->enabled || cw->sample_rate <= 0)
        return;
    samples_per_ms = cw->sample_rate / 1000;
    if (samples_per_ms < 1)
        samples_per_ms = 1;
    bin = (int)(sample_index / (unsigned long long)samples_per_ms);
    if (bin < 0 || bin >= CW_DECODE_MAX_MS)
        return;
    phase = -2.0 * M_PI * (double)cw->carrier_offset_hz * (double)sample_index / (double)cw->sample_rate;
    c = cos(phase);
    s = sin(phase);
    cw->ms_i[bin] += (i_val * c) - (q_val * s);
    cw->ms_q[bin] += (i_val * s) + (q_val * c);
    cw->ms_count[bin]++;
    if (bin + 1 > cw->ms_bins)
        cw->ms_bins = bin + 1;
}

static double cw_decode_ms_avg(const struct cw_decode_metrics *cw, int bin)
{
    if (!cw || bin < 0 || bin >= cw->ms_bins || cw->ms_count[bin] == 0)
        return 0.0;
    return sqrt((cw->ms_i[bin] * cw->ms_i[bin]) + (cw->ms_q[bin] * cw->ms_q[bin])) /
        (double)cw->ms_count[bin];
}

static void cw_decode_finish(struct cw_decode_metrics *cw)
{
    double min_avg = 1.0e9;
    double max_avg = 0.0;
    unsigned char raw_keyed[CW_DECODE_MAX_MS];
    unsigned char smooth_keyed[CW_DECODE_MAX_MS];
    int unit_ms;
    int smooth_radius_ms;
    int min_run_ms;
    int keyed_ms = 0;
    int i;
    bool last_keyed = false;
    bool have_state = false;
    char pattern[CW_DECODE_SYMBOL_MAX];
    int pattern_len = 0;

    if (!cw->enabled || cw->ms_bins <= 0)
        return;
    for (i = 0; i < cw->ms_bins; i++) {
        double avg = cw_decode_ms_avg(cw, i);
        if (cw->ms_count[i] == 0)
            continue;
        if (avg < min_avg)
            min_avg = avg;
        if (avg > max_avg)
            max_avg = avg;
    }
    if (max_avg <= 0.0 || min_avg > max_avg)
        return;

    cw->peak = max_avg;
    cw->floor = min_avg;
    cw->threshold = min_avg + ((max_avg - min_avg) * 0.5);
    for (i = 0; i < 12; i++) {
        double low_sum = 0.0;
        double high_sum = 0.0;
        int low_count = 0;
        int high_count = 0;
        int j;

        for (j = 0; j < cw->ms_bins; j++) {
            double avg = cw_decode_ms_avg(cw, j);
            if (cw->ms_count[j] == 0)
                continue;
            if (avg < cw->threshold) {
                low_sum += avg;
                low_count++;
            } else {
                high_sum += avg;
                high_count++;
            }
        }
        if (low_count > 0 && high_count > 0) {
            double low_mean = low_sum / (double)low_count;
            double high_mean = high_sum / (double)high_count;
            double next_threshold = (low_mean + high_mean) * 0.5;
            if (fabs(next_threshold - cw->threshold) < 1.0e-9)
                break;
            cw->threshold = next_threshold;
        }
    }
    unit_ms = (int)(1200L / (cw->requested_wpm > 0 ? cw->requested_wpm : 12));
    if (unit_ms < 10)
        unit_ms = 10;

    memset(raw_keyed, 0, sizeof(raw_keyed));
    memset(smooth_keyed, 0, sizeof(smooth_keyed));
    for (i = 0; i < cw->ms_bins; i++)
        raw_keyed[i] = cw_decode_ms_avg(cw, i) >= cw->threshold ? 1 : 0;

    smooth_radius_ms = unit_ms / 3;
    smooth_keying(raw_keyed, smooth_keyed, cw->ms_bins, smooth_radius_ms);
    maybe_invert_keying(smooth_keyed, cw->ms_bins, unit_ms);
    unit_ms = estimate_cw_unit_ms(smooth_keyed, cw->ms_bins, unit_ms);
    cw->estimated_unit_ms = unit_ms;
    cw->estimated_wpm = unit_ms > 0 ? (int)((1200 + unit_ms / 2) / unit_ms) : 0;
    smooth_radius_ms = unit_ms / 3;
    smooth_keying(raw_keyed, smooth_keyed, cw->ms_bins, smooth_radius_ms);
    maybe_invert_keying(smooth_keyed, cw->ms_bins, unit_ms);

    min_run_ms = unit_ms / 3;
    if (min_run_ms < 3)
        min_run_ms = 3;

    i = 0;
    while (i < cw->ms_bins) {
        bool keyed;
        int start = i;
        int run_ms;
        int units;

        keyed = smooth_keyed[i] != 0;
        while (i < cw->ms_bins) {
            if ((smooth_keyed[i] != 0) != keyed)
                break;
            i++;
        }
        run_ms = i - start;
        if (run_ms <= 0)
            continue;
        if (run_ms < min_run_ms)
            continue;
        if (keyed)
            keyed_ms += run_ms;
        if (!have_state || keyed != last_keyed) {
            cw->keying_segments++;
            last_keyed = keyed;
            have_state = true;
        }
        units = units_from_ms(run_ms, unit_ms);
        append_run_text(cw->runs, sizeof(cw->runs), keyed, run_ms, units);
        if (keyed) {
            if (pattern_len + 1 < CW_DECODE_SYMBOL_MAX)
                pattern[pattern_len++] = units >= 2 ? '-' : '.';
        } else if (units >= 7) {
            cw_flush_symbol(cw, pattern, &pattern_len);
            if (cw->text[0] && cw->text[strlen(cw->text) - 1] != ' ')
                append_text_char(cw->text, sizeof(cw->text), ' ');
            append_symbol_text(cw->symbols, sizeof(cw->symbols), "/ ");
        } else if (units >= 3) {
            cw_flush_symbol(cw, pattern, &pattern_len);
        }
    }
    cw_flush_symbol(cw, pattern, &pattern_len);
    if (cw->text[0] && cw->text[strlen(cw->text) - 1] == ' ')
        cw->text[strlen(cw->text) - 1] = '\0';
    cw->keyed_percent = 100.0 * (double)keyed_ms / (double)cw->ms_bins;
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
        audio->eof_rewinds++;
        if (fread(raw, 1, sizeof(raw), audio->file) != sizeof(raw))
            return 0.0f;
    }
    sample = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    audio->samples_read++;
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

static void tx_output_metrics_add(struct tx_output_metrics *metrics, int16_t i_val, int16_t q_val)
{
    double i_norm;
    double q_norm;
    double mag2;
    double mag;

    if (!metrics)
        return;
    if (metrics->have_last && (i_val != metrics->last_i || q_val != metrics->last_q))
        metrics->transition_count++;
    metrics->have_last = true;
    metrics->last_i = i_val;
    metrics->last_q = q_val;

    i_norm = (double)i_val / 32768.0;
    q_norm = (double)q_val / 32768.0;
    mag2 = i_norm * i_norm + q_norm * q_norm;
    mag = sqrt(mag2);
    metrics->sumsq += mag2;
    if (mag > metrics->peak)
        metrics->peak = mag;
    metrics->sample_count++;
}

static void tx_output_metrics_add_audio(struct tx_output_metrics *metrics, float audio)
{
    double abs_audio;

    if (!metrics)
        return;
    if (metrics->audio_have_last && metrics->audio_last < 0.0f && audio >= 0.0f)
        metrics->audio_crossing_count++;
    metrics->audio_have_last = true;
    metrics->audio_last = audio;
    metrics->audio_sumsq += (double)audio * (double)audio;
    abs_audio = fabs((double)audio);
    if (abs_audio > metrics->audio_peak)
        metrics->audio_peak = abs_audio;
    metrics->audio_sample_count++;
}

static void fill_tx_modulated(struct iio_buffer *buf, struct iio_channel *i_chan,
                              const struct tx_config *cfg, struct tx_state *state,
                              struct tx_output_metrics *metrics)
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
            tx_output_metrics_add_audio(metrics, audio);
            float envelope = (1.0f + cfg->am_modulation_index * audio) / (1.0f + cfg->am_modulation_index);
            if (envelope < 0.0f)
                envelope = 0.0f;
            i_val = level * envelope;
        } else if (mode_is(cfg->mode, "fm")) {
            float audio = audio_next_sample(&state->audio, cfg);
            tx_output_metrics_add_audio(metrics, audio);
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
        } else if (mode_is(cfg->mode, "ft8")) {
            ft8_next_iq(&state->ft8, cfg->sample_rate, level,
                        (float)cfg->audio_tone_hz, &i_val, &q_val);
        } else {
            i_val = cosf(state->tone_phase) * level;
            q_val = sinf(state->tone_phase) * level;
            state->tone_phase += 2.0f * (float)M_PI * (float)cfg->tone_hz / (float)cfg->sample_rate;
            if (state->tone_phase > 2.0f * (float)M_PI)
                state->tone_phase -= 2.0f * (float)M_PI;
        }

        sample[0] = clamp16(i_val);
        sample[1] = clamp16(q_val);
        tx_output_metrics_add(metrics, sample[0], sample[1]);
    }
}

static void tone_metric_add(struct tone_metric *metric, double sample, long index, long sample_rate)
{
    double omega = 2.0 * M_PI * metric->freq_hz / (double)sample_rate;
    metric->i += sample * cos(omega * (double)index);
    metric->q -= sample * sin(omega * (double)index);
}

static void demod_metrics_init(struct demod_metrics *metrics, const char *mode, long sample_rate, long tone_hz, long carrier_offset_hz)
{
    memset(metrics, 0, sizeof(*metrics));
    metrics->enabled = mode_is(mode, "fm") || mode_is(mode, "am") || mode_is(mode, "cw");
    metrics->sample_rate = sample_rate;
    metrics->tone_hz = tone_hz;
    metrics->carrier_offset_hz = carrier_offset_hz;
    metrics->tone.freq_hz = (double)tone_hz;
    metrics->refs[0].freq_hz = (double)(tone_hz > 40 ? tone_hz / 2 : 20);
    metrics->refs[1].freq_hz = (double)(tone_hz * 2);
    metrics->refs[2].freq_hz = (double)(tone_hz * 3);
}

static void demod_metrics_add_iq(struct demod_metrics *metrics, const char *mode, float i_val, float q_val)
{
    double sample;
    int r;

    if (!metrics->enabled)
        return;
    if (mode_is(mode, "fm")) {
        if (!metrics->have_last) {
            metrics->last_i = i_val;
            metrics->last_q = q_val;
            metrics->have_last = true;
            return;
        }
        sample = atan2((double)metrics->last_i * (double)q_val - (double)metrics->last_q * (double)i_val,
                       (double)metrics->last_i * (double)i_val + (double)metrics->last_q * (double)q_val);
        sample -= 2.0 * M_PI * (double)metrics->carrier_offset_hz / (double)metrics->sample_rate;
        while (sample > M_PI)
            sample -= 2.0 * M_PI;
        while (sample < -M_PI)
            sample += 2.0 * M_PI;
        metrics->last_i = i_val;
        metrics->last_q = q_val;
    } else if (mode_is(mode, "am") || mode_is(mode, "cw")) {
        sample = sqrt((double)i_val * (double)i_val + (double)q_val * (double)q_val);
    } else {
        return;
    }

    metrics->sumsq += sample * sample;
    if (fabs(sample) > metrics->peak)
        metrics->peak = fabs(sample);
    tone_metric_add(&metrics->tone, sample, metrics->sample_index, metrics->sample_rate);
    for (r = 0; r < 3; r++)
        tone_metric_add(&metrics->refs[r], sample, metrics->sample_index, metrics->sample_rate);
    metrics->sample_index++;
}

static double tone_metric_magnitude(const struct tone_metric *metric, long count)
{
    if (count <= 0)
        return 0.0;
    return sqrt(metric->i * metric->i + metric->q * metric->q) * 2.0 / (double)count;
}

static int run_loopback(void)
{
    const char *mode = env_default("PLUTO_TX_MODE", "loopback");
    const char *rx_device_name = env_default("PLUTO_LOOPBACK_RX_DEVICE", DEFAULT_RX_DEVICE);
    const char *tx_device_name = env_default("PLUTO_LOOPBACK_TX_DEVICE", DEFAULT_TX_DEVICE);
    const char *i_name = env_default("PLUTO_LOOPBACK_I_CHANNEL", DEFAULT_I_CHAN);
    const char *q_name = env_default("PLUTO_LOOPBACK_Q_CHANNEL", DEFAULT_Q_CHAN);
    long sample_rate = env_long("PLUTO_LOOPBACK_SAMPLE_RATE_HZ", 1000000, 520000, 61440000);
    long duration_ms = env_long("PLUTO_LOOPBACK_DURATION_MS", 1000, 50, 30000);
    long tone_hz = env_long("PLUTO_LOOPBACK_TONE_HZ", 10000, 0, sample_rate / 4);
    long buffer_samples = env_long("PLUTO_LOOPBACK_BUFFER_SAMPLES", 4096, 256, 65536);
    float amplitude = env_float("PLUTO_LOOPBACK_TX_AMPLITUDE", 0.05f, 0.0f, 0.25f);
    bool tx_only = strcmp(env_default("PLUTO_TX_ONLY", "0"), "0") != 0;
    long expect_tone_hz = env_long("PLUTO_LOOPBACK_EXPECT_TONE_HZ",
                                   mode_is(mode, "cw") ? 0 : env_long("PLUTO_TX_AUDIO_TONE_HZ", 1000, 20, 3000),
                                   0, sample_rate / 4);
    long carrier_offset_hz = env_long("PLUTO_LOOPBACK_RX_IF_OFFSET_HZ", 0, -1000000, 1000000);
    struct tx_config tx_cfg = {
        .mode = strcmp(mode, "tx_only") == 0 ? "tone" : mode,
        .audio_source = env_default("PLUTO_TX_AUDIO_SOURCE", "tone"),
        .audio_path = env_default("PLUTO_TX_AUDIO_PATH", ""),
        .cw_text = env_default("PLUTO_TX_CW_TEXT", "CQ PLUTO"),
        .ft8_text = env_default("PLUTO_TX_FT8_TEXT", "CQ K1ABC FN42"),
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
    struct demod_metrics demod;
    struct cw_decode_metrics cw_decode;
    struct tx_output_metrics tx_metrics;
    unsigned long long tx_push_count = 0;
    int ret = 1;

    memset(&tx_state, 0, sizeof(tx_state));
    memset(&tx_metrics, 0, sizeof(tx_metrics));
    build_cw_units(&tx_state.cw, tx_cfg.cw_text, sample_rate, tx_cfg.cw_wpm);
    ft8_init(&tx_state.ft8, tx_cfg.ft8_text, sample_rate);
    if (mode_is(tx_cfg.mode, "ft8") && !tx_state.ft8.valid) {
        fprintf(stderr, "could not encode FT8 message: %s\n", tx_cfg.ft8_text);
        goto out;
    }
    if (audio_open(&tx_state.audio, &tx_cfg) < 0)
        goto out;
    demod_metrics_init(&demod, tx_cfg.mode, sample_rate, expect_tone_hz, carrier_offset_hz);
    cw_decode_init(&cw_decode, tx_cfg.mode, sample_rate, tx_cfg.cw_wpm,
                   carrier_offset_hz, cw_expected_keyed_percent(&tx_state.cw));

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

    tx_buf = iio_device_create_buffer(tx_dev, (size_t)buffer_samples, false);
    rx_buf = tx_only ? NULL : iio_device_create_buffer(rx_dev, (size_t)buffer_samples, false);
    if (!tx_buf || (!rx_buf && !tx_only)) {
        fprintf(stderr, "could not create loopback IIO buffers\n");
        goto out;
    }
    if (mode_is(tx_cfg.mode, "loopback"))
        fill_tx_tone(tx_buf, tx_i, sample_rate, tone_hz, amplitude);
    else
        fill_tx_modulated(tx_buf, tx_i, &tx_cfg, &tx_state, &tx_metrics);
    if (iio_buffer_push(tx_buf) < 0) {
        fprintf(stderr, "TX buffer push failed\n");
        goto out;
    }
    tx_push_count++;

    started = monotonic_seconds();
    while (keep_running && ((monotonic_seconds() - started) * 1000.0) < (double)duration_ms) {
        ssize_t refill;
        char *ptr;
        char *end;
        ptrdiff_t step;

        if (tx_only) {
            fill_tx_modulated(tx_buf, tx_i, &tx_cfg, &tx_state, &tx_metrics);
            if (iio_buffer_push(tx_buf) < 0) {
                fprintf(stderr, "TX buffer push failed\n");
                goto out;
            }
            tx_push_count++;
            continue;
        }
        if (!mode_is(tx_cfg.mode, "loopback"))
            fill_tx_modulated(tx_buf, tx_i, &tx_cfg, &tx_state, &tx_metrics);
        if (iio_buffer_push(tx_buf) < 0) {
            fprintf(stderr, "TX buffer push failed\n");
            goto out;
        }
        tx_push_count++;
        refill = iio_buffer_refill(rx_buf);
        if (refill < 0) {
            fprintf(stderr, "RX buffer refill failed: %zd\n", refill);
            goto out;
        }
        ptr = iio_buffer_first(rx_buf, rx_i);
        end = iio_buffer_end(rx_buf);
        step = iio_buffer_step(rx_buf);
        for (; ptr < end; ptr += step) {
            double i_val = (double)unpack_ad9361_s12_sample(ptr) / 32768.0;
            double q_val = (double)unpack_ad9361_s12_sample(ptr + sizeof(uint16_t)) / 32768.0;
            double mag2 = i_val * i_val + q_val * q_val;
            double mag = sqrt(mag2);

            sumsq += mag2;
            if (mag > peak)
                peak = mag;
            demod_metrics_add_iq(&demod, tx_cfg.mode, (float)i_val, (float)q_val);
            cw_decode_add(&cw_decode, i_val, q_val, samples);
            samples++;
        }
    }

    if (tx_only && (tx_push_count == 0 || tx_metrics.sample_count == 0 || tx_metrics.peak <= 0.0)) {
        fprintf(stderr, "TX backend generated no non-zero IQ samples\n");
        goto out;
    }

    ret = 0;

out:
    if (tx_buf)
        iio_buffer_destroy(tx_buf);
    if (rx_buf)
        iio_buffer_destroy(rx_buf);
    if (tx_state.audio.file)
        fclose(tx_state.audio.file);
    free(tx_state.ft8.pulse);
    if (ctx)
        iio_context_destroy(ctx);

    if (ret == 0 && samples > 0) {
        double rms = sqrt(sumsq / (double)samples);
        double rms_dbfs = 20.0 * log10(rms > 1.0e-9 ? rms : 1.0e-9);
        double peak_dbfs = 20.0 * log10(peak > 1.0e-9 ? peak : 1.0e-9);

        cw_decode_finish(&cw_decode);
        if (demod.enabled && demod.sample_index > 0) {
            double tone_mag = tone_metric_magnitude(&demod.tone, demod.sample_index);
            double ref_mag = tone_metric_magnitude(&demod.refs[0], demod.sample_index);
            int r;
            double demod_rms = sqrt(demod.sumsq / (double)demod.sample_index);
            double tone_dbfs = 20.0 * log10(tone_mag > 1.0e-9 ? tone_mag : 1.0e-9);
            double ref_dbfs;
            double tone_snr_db;
            for (r = 1; r < 3; r++) {
                double mag = tone_metric_magnitude(&demod.refs[r], demod.sample_index);
                if (mag > ref_mag)
                    ref_mag = mag;
            }
            ref_dbfs = 20.0 * log10(ref_mag > 1.0e-9 ? ref_mag : 1.0e-9);
            tone_snr_db = tone_dbfs - ref_dbfs;
            printf("{\"ok\":true,\"samples\":%llu,\"duration_ms\":%ld,"
                   "\"sample_rate_hz\":%ld,\"tx_mode\":\"%s\","
                   "\"carrier_offset_hz\":%ld,"
                   "\"tone_hz\":%ld,\"detected_tone_hz\":%ld,"
                   "\"tx_amplitude\":%.6f,\"rx_rms_dbfs\":%.2f,"
                   "\"rx_peak_dbfs\":%.2f,\"demod_sample_count\":%ld,"
                   "\"demod_rms\":%.9f,\"tone_dbfs\":%.2f,"
                   "\"reference_dbfs\":%.2f,\"tone_snr_db\":%.2f,"
                   "\"pass_snr_db_min\":6.00,"
                   "\"passed\":%s",
                   samples, duration_ms, sample_rate, tx_cfg.mode,
                   carrier_offset_hz,
                   expect_tone_hz, expect_tone_hz,
                   amplitude, rms_dbfs, peak_dbfs, demod.sample_index,
                   demod_rms, tone_dbfs, ref_dbfs, tone_snr_db,
                   (tone_snr_db >= 6.0 ? "true" : "false"));
            if (cw_decode.enabled) {
                printf(",\"cw_decode\":{\"decode_supported\":true,"
                       "\"requested_wpm\":%ld,\"estimated_wpm\":%d,"
                       "\"estimated_unit_ms\":%d,\"timing_source\":\"auto\","
                       "\"expected_keyed_percent\":%.2f,"
                       "\"keyed_percent\":%.2f,\"keying_segments\":%d,"
                       "\"envelope_floor\":%.9f,\"envelope_peak\":%.9f,"
                       "\"envelope_threshold\":%.9f,"
                       "\"decoded_runs\":\"%s\","
                       "\"decoded_symbols\":\"%s\",\"decoded_text\":\"%s\","
                       "\"expected_text\":\"%s\",\"matched_expected\":%s}",
                       cw_decode.requested_wpm, cw_decode.estimated_wpm,
                       cw_decode.estimated_unit_ms,
                       cw_decode.expected_keyed_percent,
                       cw_decode.keyed_percent, cw_decode.keying_segments,
                       cw_decode.floor, cw_decode.peak, cw_decode.threshold,
                       cw_decode.runs, cw_decode.symbols, cw_decode.text, tx_cfg.cw_text,
                       (decoded_text_matches_expected(cw_decode.text, tx_cfg.cw_text) ? "true" : "false"));
            }
            printf("}\n");
        } else {
            printf("{\"ok\":true,\"samples\":%llu,\"duration_ms\":%ld,"
                   "\"sample_rate_hz\":%ld,\"tone_hz\":%ld,"
                   "\"tx_amplitude\":%.6f,\"rx_rms_dbfs\":%.2f,"
                   "\"rx_peak_dbfs\":%.2f}\n",
                   samples, duration_ms, sample_rate, tone_hz,
                   amplitude, rms_dbfs, peak_dbfs);
        }
    } else if (ret == 0) {
        double tx_rms = sqrt(tx_metrics.sumsq / (double)tx_metrics.sample_count);
        double tx_rms_dbfs = 20.0 * log10(tx_rms > 1.0e-9 ? tx_rms : 1.0e-9);
        double tx_peak_dbfs = 20.0 * log10(tx_metrics.peak > 1.0e-9 ? tx_metrics.peak : 1.0e-9);
        double tx_audio_rms = tx_metrics.audio_sample_count > 0 ?
            sqrt(tx_metrics.audio_sumsq / (double)tx_metrics.audio_sample_count) : 0.0;
        double tx_measured_audio_tone_hz = 0.0;
        bool have_measured_audio_tone =
            tx_metrics.audio_sample_count > 0 &&
            strcmp(tx_cfg.audio_source, "file") != 0 &&
            (mode_is(tx_cfg.mode, "fm") || mode_is(tx_cfg.mode, "am"));

        if (have_measured_audio_tone) {
            tx_measured_audio_tone_hz =
                (double)tx_metrics.audio_crossing_count * (double)sample_rate /
                (double)tx_metrics.audio_sample_count;
        }

        printf("{\"ok\":true,\"mode\":\"tx_only\",\"tx_mode\":\"%s\",\"duration_ms\":%ld,"
               "\"sample_rate_hz\":%ld,\"tone_hz\":%ld,"
               "\"tx_amplitude\":%.6f,\"audio_source\":\"%s\","
               "\"tx_audio_source\":\"%s\","
               "\"audio_rate_hz\":%ld,\"tx_audio_rate_hz\":%ld,"
               "\"audio_tone_hz\":%ld,\"tx_audio_tone_hz\":%ld,"
               "\"fm_deviation_hz\":%ld,\"tx_fm_deviation_hz\":%ld,"
               "\"am_modulation_index\":%.3f,\"tx_am_modulation_index\":%.3f,"
               "\"cw_wpm\":%ld,"
               "\"tx_push_count\":%llu,\"tx_sample_count\":%llu,"
               "\"tx_transition_count\":%llu,\"tx_rms_dbfs\":%.2f,"
               "\"tx_peak_dbfs\":%.2f,"
               "\"tx_audio_sample_count\":%llu,\"tx_audio_crossing_count\":%llu,"
               "\"tx_audio_rms\":%.9f,\"tx_audio_peak\":%.9f,"
               "\"tx_audio_file_samples_read\":%llu,"
               "\"tx_audio_file_rewinds\":%llu",
               tx_cfg.mode, duration_ms, sample_rate, tone_hz,
               amplitude, tx_cfg.audio_source, tx_cfg.audio_source,
               tx_cfg.audio_rate, tx_cfg.audio_rate,
               tx_cfg.audio_tone_hz, tx_cfg.audio_tone_hz,
               tx_cfg.fm_deviation_hz, tx_cfg.fm_deviation_hz,
               tx_cfg.am_modulation_index, tx_cfg.am_modulation_index,
               tx_cfg.cw_wpm,
               tx_push_count, tx_metrics.sample_count,
               tx_metrics.transition_count, tx_rms_dbfs, tx_peak_dbfs,
               tx_metrics.audio_sample_count, tx_metrics.audio_crossing_count,
               tx_audio_rms, tx_metrics.audio_peak,
               tx_state.audio.samples_read, tx_state.audio.eof_rewinds);
        if (have_measured_audio_tone) {
            printf(",\"tx_measured_audio_tone_hz\":%.2f,"
                   "\"tx_audio_tone_error_hz\":%.2f",
                   tx_measured_audio_tone_hz,
                   tx_measured_audio_tone_hz - (double)tx_cfg.audio_tone_hz);
        }
        printf("}\n");
    }
    return ret;
}

int main(void)
{
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    return run_loopback();
}
