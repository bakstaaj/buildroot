#define _POSIX_C_SOURCE 200809L

#include <complex.h>
#include <errno.h>
#include <iio.h>
#include <liquid/liquid.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "pluto-ft8-decoder.h"

#define DEFAULT_DEVICE "cf-ad9361-lpc"
#define DEFAULT_I_CHAN "voltage0"
#define DEFAULT_Q_CHAN "voltage1"
#define DEFAULT_IN_RATE 600000
#define DEFAULT_AUDIO_RATE 12000
#define DEFAULT_BUFFER_SAMPLES 4096
#define INT16_CLIP 32767.0f
#define CW_LIVE_TEXT_MAX 256
#define CW_LIVE_SYMBOL_MAX 32
#define CW_LIVE_SYMBOLS_MAX 512
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum demod_mode {
    DEMOD_NFM,
    DEMOD_WFM,
    DEMOD_AM,
    DEMOD_CW,
    DEMOD_FT8,
};

enum agc_mode {
    AGC_OFF,
    AGC_SLOW,
    AGC_FAST,
};

enum iq_mode {
    IQ_NORMAL,
    IQ_SWAP,
    IQ_INVERT_I,
    IQ_INVERT_Q,
    IQ_INVERT_BOTH,
    IQ_SWAP_INVERT_I,
    IQ_SWAP_INVERT_Q,
    IQ_SWAP_INVERT_BOTH,
};

struct cw_live_decoder {
    bool enabled;
    bool have_level;
    bool have_state;
    bool current_keyed;
    long requested_wpm;
    int unit_ms;
    int min_unit_ms;
    int max_unit_ms;
    unsigned audio_rate;
    unsigned samples_in_ms;
    unsigned ms_samples;
    double ms_sum;
    double floor;
    double peak;
    double threshold;
    unsigned run_ms;
    unsigned keying_segments;
    unsigned keyed_ms;
    unsigned total_ms;
    int pattern_len;
    char pattern[CW_LIVE_SYMBOL_MAX];
    char decoded_text[CW_LIVE_TEXT_MAX];
    char decoded_symbols[CW_LIVE_SYMBOLS_MAX];
};

static volatile sig_atomic_t keep_running = 1;
static const char *backend_phase = "starting";

static const char *iq_mode_name(enum iq_mode mode);

struct dsp_state {
    enum demod_mode mode;
    enum agc_mode agc;
    enum iq_mode iq_mode;
    unsigned iq_decim;
    unsigned iq_count;
    unsigned output_rate_acc;
    unsigned audio_rate;
    unsigned processing_rate;
    unsigned count;
    unsigned squelch_hold;
    unsigned squelch_hang;
    float acc;
    float am_avg;
    float lp;
    float alpha;
    float scale;
    float cw_phase;
    float cw_step;
    float signal_level;
    float squelch_threshold_db;
    bool squelch_enabled;
    bool squelch_open;
    bool dc_block;
    float dc;
    float iq_shift_step;
    float iq_shift_cos;
    float iq_shift_sin;
    float iq_shift_step_cos;
    float iq_shift_step_sin;
    unsigned iq_shift_renorm;
    bool fm_channel_filter_enabled;
    float fm_channel_alpha;
    float fm_channel_i;
    float fm_channel_q;
    bool fm_channel_ready;
    bool fm_limiter_enabled;
    float fm_limiter_floor;
    float prev_i;
    float prev_q;
    bool have_prev_iq;
    bool deemphasis_enabled;
    float deemphasis_alpha;
    float deemphasis_lp;
    float output_gain;
    bool noise_gate_enabled;
    float noise_gate_threshold_db;
    float audio_level;
    uint64_t pcm_samples;
    uint64_t last_report_pcm_samples;
    double pcm_power_acc;
    double pcm_measured_rate_hz;
    unsigned pcm_power_count;
    unsigned iio_refills;
    time_t last_status_time;
    double last_report_monotonic;
    float agc_gain;
    float agc_target;
    float agc_attack;
    float agc_release;
    int64_t i_acc;
    int64_t q_acc;
    struct cw_live_decoder cw_live;
    struct pluto_ft8_decoder *ft8;
};

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

static bool env_bool(const char *name, bool fallback)
{
    const char *raw = getenv(name);

    if (!raw || !raw[0])
        return fallback;
    if (!strcmp(raw, "0") || !strcmp(raw, "false") || !strcmp(raw, "off") || !strcmp(raw, "no"))
        return false;
    if (!strcmp(raw, "1") || !strcmp(raw, "true") || !strcmp(raw, "on") || !strcmp(raw, "yes"))
        return true;
    return fallback;
}

static char morse_decode_char(const char *pattern)
{
    struct morse_entry {
        const char *pattern;
        char value;
    };
    static const struct morse_entry table[] = {
        {".-", 'A'}, {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'}, {".", 'E'},
        {"..-.", 'F'}, {"--.", 'G'}, {"....", 'H'}, {"..", 'I'}, {".---", 'J'},
        {"-.-", 'K'}, {".-..", 'L'}, {"--", 'M'}, {"-.", 'N'}, {"---", 'O'},
        {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'}, {"...", 'S'}, {"-", 'T'},
        {"..-", 'U'}, {"...-", 'V'}, {".--", 'W'}, {"-..-", 'X'}, {"-.--", 'Y'},
        {"--..", 'Z'}, {"-----", '0'}, {".----", '1'}, {"..---", '2'},
        {"...--", '3'}, {"....-", '4'}, {".....", '5'}, {"-....", '6'},
        {"--...", '7'}, {"---..", '8'}, {"----.", '9'},
    };
    size_t i;

    if (!pattern || !pattern[0])
        return '\0';
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (!strcmp(pattern, table[i].pattern))
            return table[i].value;
    }
    return '?';
}

static void append_char(char *buf, size_t len, char value)
{
    size_t used = strlen(buf);

    if (used + 1 >= len) {
        size_t keep = len / 2;
        if (keep == 0 || used < keep)
            return;
        memmove(buf, buf + (used - keep), keep);
        buf[keep] = '\0';
        used = keep;
    }
    buf[used] = value;
    buf[used + 1] = '\0';
}

static void append_text(char *buf, size_t len, const char *value)
{
    size_t used = strlen(buf);
    size_t remaining;

    if (!value || !value[0] || used + 1 >= len)
        append_char(buf, len, ' ');
    used = strlen(buf);
    if (used + 1 >= len)
        return;
    remaining = len - used - 1;
    strncat(buf, value, remaining);
}

static int units_from_ms(unsigned run_ms, int unit_ms)
{
    if (unit_ms <= 0)
        return 1;
    return (int)(((int)run_ms + unit_ms / 2) / unit_ms);
}

static void cw_live_flush_symbol(struct cw_live_decoder *cw)
{
    char decoded;

    if (!cw || cw->pattern_len <= 0)
        return;
    cw->pattern[cw->pattern_len] = '\0';
    decoded = morse_decode_char(cw->pattern);
    append_char(cw->decoded_text, sizeof(cw->decoded_text), decoded);
    append_text(cw->decoded_symbols, sizeof(cw->decoded_symbols), cw->pattern);
    append_char(cw->decoded_symbols, sizeof(cw->decoded_symbols), ' ');
    cw->pattern_len = 0;
}

static void cw_live_finish_run(struct cw_live_decoder *cw, bool keyed, unsigned run_ms)
{
    int units;
    unsigned min_run_ms;

    if (!cw || !cw->enabled || run_ms == 0)
        return;
    min_run_ms = (unsigned)(cw->unit_ms / 4);
    if (min_run_ms < 8)
        min_run_ms = 8;
    if (run_ms < min_run_ms)
        return;

    units = units_from_ms(run_ms, cw->unit_ms);
    if (keyed) {
        cw->keying_segments++;
        cw->keyed_ms += run_ms;
        if (cw->pattern_len + 1 < CW_LIVE_SYMBOL_MAX)
            cw->pattern[cw->pattern_len++] = units >= 2 ? '-' : '.';
    } else if (units >= 7) {
        cw_live_flush_symbol(cw);
        if (cw->decoded_text[0] && cw->decoded_text[strlen(cw->decoded_text) - 1] != ' ')
            append_char(cw->decoded_text, sizeof(cw->decoded_text), ' ');
        append_text(cw->decoded_symbols, sizeof(cw->decoded_symbols), "/ ");
    } else if (units >= 3) {
        cw_live_flush_symbol(cw);
    }
}

static void cw_live_init(struct cw_live_decoder *cw, enum demod_mode mode, unsigned audio_rate)
{
    long requested_wpm;

    memset(cw, 0, sizeof(*cw));
    cw->enabled = mode == DEMOD_CW && env_bool("PLUTO_CW_DECODE_ENABLED", true);
    cw->audio_rate = audio_rate;
    cw->samples_in_ms = audio_rate >= 1000 ? audio_rate / 1000U : 1U;
    requested_wpm = env_long("PLUTO_CW_DECODE_WPM", 0, 0, 60);
    cw->requested_wpm = requested_wpm;
    cw->unit_ms = requested_wpm > 0 ? (int)(1200L / requested_wpm) : 100;
    if (cw->unit_ms < 20)
        cw->unit_ms = 20;
    if (cw->unit_ms > 300)
        cw->unit_ms = 300;
    if (requested_wpm > 0) {
        cw->min_unit_ms = (cw->unit_ms * 3) / 4;
        cw->max_unit_ms = (cw->unit_ms * 3) / 2;
    } else {
        cw->min_unit_ms = 20;
        cw->max_unit_ms = 300;
    }
}

static void cw_live_update_unit(struct cw_live_decoder *cw, unsigned run_ms)
{
    int candidate;

    if (!cw || !cw->enabled || run_ms == 0)
        return;
    candidate = (int)run_ms;
    if (candidate > cw->unit_ms * 2)
        candidate = (candidate + 1) / 3;
    if (candidate < cw->min_unit_ms)
        candidate = cw->min_unit_ms;
    if (candidate > cw->max_unit_ms)
        candidate = cw->max_unit_ms;
    cw->unit_ms = (cw->unit_ms * 7 + candidate) / 8;
}

static void cw_live_process_ms(struct cw_live_decoder *cw, double level)
{
    bool keyed;

    if (!cw || !cw->enabled)
        return;
    if (!cw->have_level) {
        cw->floor = level;
        cw->peak = level;
        cw->threshold = level;
        cw->have_level = true;
    }
    if (level < cw->floor)
        cw->floor += (level - cw->floor) * 0.08;
    else
        cw->floor += (level - cw->floor) * 0.002;
    if (level > cw->peak)
        cw->peak += (level - cw->peak) * 0.08;
    else
        cw->peak += (level - cw->peak) * 0.004;
    cw->threshold = cw->floor + ((cw->peak - cw->floor) * 0.45);
    keyed = level >= cw->threshold && cw->peak > cw->floor * 1.5;

    cw->total_ms++;
    if (!cw->have_state) {
        cw->current_keyed = keyed;
        cw->run_ms = 1;
        cw->have_state = true;
        return;
    }
    if (keyed == cw->current_keyed) {
        cw->run_ms++;
        return;
    }
    if (cw->current_keyed)
        cw_live_update_unit(cw, cw->run_ms);
    cw_live_finish_run(cw, cw->current_keyed, cw->run_ms);
    cw->current_keyed = keyed;
    cw->run_ms = 1;
}

static void cw_live_add_audio(struct cw_live_decoder *cw, float audio)
{
    if (!cw || !cw->enabled)
        return;
    cw->ms_sum += fabs((double)audio);
    cw->ms_samples++;
    if (cw->ms_samples >= cw->samples_in_ms) {
        cw_live_process_ms(cw, cw->ms_sum / (double)cw->ms_samples);
        cw->ms_sum = 0.0;
        cw->ms_samples = 0;
    }
}

static void json_string(FILE *out, const char *value)
{
    fputc('"', out);
    for (; value && *value; value++) {
        unsigned char ch = (unsigned char)*value;

        switch (ch) {
        case '\\':
            fputs("\\\\", out);
            break;
        case '"':
            fputs("\\\"", out);
            break;
        case '\b':
            fputs("\\b", out);
            break;
        case '\f':
            fputs("\\f", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (ch < 0x20)
                fprintf(out, "\\u%04x", ch);
            else
                fputc(ch, out);
            break;
        }
    }
    fputc('"', out);
}

static void json_string_field(FILE *out, const char *key, const char *value, bool comma)
{
    fprintf(out, "  \"%s\": ", key);
    if (value && value[0])
        json_string(out, value);
    else
        fputs("null", out);
    fputs(comma ? ",\n" : "\n", out);
}

static void utc_now(char *buf, size_t len)
{
    time_t raw = time(NULL);
    struct tm tm_now;

    if (gmtime_r(&raw, &tm_now))
        strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_now);
    else
        snprintf(buf, len, "1970-01-01T00:00:00Z");
}

static void write_audio_error_file(const char *path, const char *code, const char *message, int errnum)
{
    char tmp[512];
    char now[32];
    FILE *out;
    int written;

    if (!path || !path[0])
        return;

    written = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    if (written <= 0 || (size_t)written >= sizeof(tmp))
        return;

    out = fopen(tmp, "w");
    if (!out)
        return;

    utc_now(now, sizeof(now));
    fputs("{\n", out);
    fprintf(out, "  \"audio_rate_hz\": %ld,\n", env_long("PLUTO_AUDIO_RATE_HZ", DEFAULT_AUDIO_RATE, 4000, 48000));
    json_string_field(out, "backend", env_default("PLUTO_AUDIO_BACKEND_KIND", "external"), true);
    json_string_field(out, "backend_path", env_default("PLUTO_AUDIO_BACKEND_PATH", "/usr/sbin/pluto-audio-backend"), true);
    json_string_field(out, "demod_mode", getenv("PLUTO_AUDIO_DEMOD"), true);
    json_string_field(out, "fifo_path", getenv("PLUTO_AUDIO_FIFO"), true);
    json_string_field(out, "phase", backend_phase, true);
    fprintf(out, "  \"last_error\": {\n");
    json_string_field(out, "code", code, true);
    fprintf(out, "    \"errno\": %d,\n", errnum);
    json_string_field(out, "message", message, true);
    json_string_field(out, "time_utc", now, false);
    fprintf(out, "  },\n");
    fprintf(out, "  \"pid\": %ld,\n", (long)getpid());
    json_string_field(out, "profile", getenv("PLUTO_AUDIO_PROFILE"), true);
    fprintf(out, "  \"rms_level\": null,\n");
    json_string_field(out, "squelch_state", "unknown", true);
    json_string_field(out, "started_utc", getenv("PLUTO_AUDIO_STARTED_UTC"), true);
    json_string_field(out, "state", "error", true);
    json_string_field(out, "stopped_utc", now, true);
    json_string_field(out, "stream_format", env_default("PLUTO_AUDIO_STREAM_FORMAT", "pcm_s16le"), true);
    json_string_field(out, "updated_utc", now, false);
    fputs("}\n", out);

    if (fclose(out) == 0) {
        if (rename(tmp, path) != 0)
            unlink(tmp);
    } else {
        unlink(tmp);
    }
}

static void write_audio_error(const char *code, const char *message, int errnum)
{
    write_audio_error_file(env_default("PLUTO_AUDIO_BACKEND_STATUS_FILE", "/var/run/pluto-radio/audio-backend-status.json"), code, message, errnum);
    write_audio_error_file(env_default("PLUTO_AUDIO_STATE_FILE", "/var/run/pluto-radio/audio.json"), code, message, errnum);
}

static double monotonic_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void write_audio_running_report(struct dsp_state *dsp)
{
    const char *path = env_default("PLUTO_AUDIO_BACKEND_STATUS_FILE", "/var/run/pluto-radio/audio-backend-status.json");
    char tmp[512];
    char now[32];
    FILE *out;
    int written;
    float rms = 0.0f;
    double raw;
    double elapsed;

    if (!path || !path[0])
        return;
    if (dsp->pcm_power_count > 0)
        rms = sqrtf((float)(dsp->pcm_power_acc / (double)dsp->pcm_power_count)) / INT16_CLIP;
    raw = monotonic_seconds();
    elapsed = raw - dsp->last_report_monotonic;
    if (dsp->last_report_monotonic > 0.0 && elapsed >= 0.25) {
        dsp->pcm_measured_rate_hz = (double)(dsp->pcm_samples - dsp->last_report_pcm_samples) /
            elapsed;
        dsp->last_report_pcm_samples = dsp->pcm_samples;
        dsp->last_report_monotonic = raw;
    } else if (dsp->last_report_monotonic <= 0.0) {
        dsp->last_report_pcm_samples = dsp->pcm_samples;
        dsp->last_report_monotonic = raw;
    }

    written = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    if (written <= 0 || (size_t)written >= sizeof(tmp))
        return;
    out = fopen(tmp, "w");
    if (!out)
        return;

    utc_now(now, sizeof(now));
    fputs("{\n", out);
    json_string_field(out, "backend", env_default("PLUTO_AUDIO_BACKEND_KIND", "external"), true);
    json_string_field(out, "backend_path", env_default("PLUTO_AUDIO_BACKEND_PATH", "/usr/sbin/pluto-audio-backend"), true);
    json_string_field(out, "demod_mode", getenv("PLUTO_AUDIO_DEMOD"), true);
    json_string_field(out, "fifo_path", getenv("PLUTO_AUDIO_FIFO"), true);
    fprintf(out, "  \"iio_refills\": %u,\n", dsp->iio_refills);
    fprintf(out, "  \"input_sample_rate_hz\": %ld,\n", env_long("PLUTO_DSP_INPUT_RATE_HZ", DEFAULT_IN_RATE, 4000, 5000000));
    fprintf(out, "  \"frequency_shift_hz\": %ld,\n", env_long("PLUTO_DSP_FREQUENCY_SHIFT_HZ", 0, -2500000, 2500000));
    fprintf(out, "  \"fm_channel_filter\": %s,\n", dsp->fm_channel_filter_enabled ? "true" : "false");
    fprintf(out, "  \"fm_limiter\": %s,\n", dsp->fm_limiter_enabled ? "true" : "false");
    json_string_field(out, "iq_mode", iq_mode_name(dsp->iq_mode), true);
    fprintf(out, "  \"iq_decimation\": %u,\n", dsp->iq_decim);
    fprintf(out, "  \"last_error\": null,\n");
    json_string_field(out, "phase", backend_phase, true);
    fprintf(out, "  \"pcm_bytes\": %" PRIu64 ",\n", dsp->pcm_samples * 2U);
    fprintf(out, "  \"pcm_rate_hz\": %u,\n", dsp->audio_rate);
    fprintf(out, "  \"pcm_measured_rate_hz\": %.3f,\n", dsp->pcm_measured_rate_hz);
    fprintf(out, "  \"pid\": %ld,\n", (long)getpid());
    json_string_field(out, "profile", getenv("PLUTO_AUDIO_PROFILE"), true);
    fprintf(out, "  \"processing_sample_rate_hz\": %ld,\n",
            env_long("PLUTO_DSP_INPUT_RATE_HZ", DEFAULT_IN_RATE, 4000, 5000000) / (long)dsp->iq_decim);
    fprintf(out, "  \"rms_level\": %.8g,\n", rms);
    json_string_field(out, "squelch_state", dsp->squelch_enabled ? (dsp->squelch_open ? "open" : "closed") : "disabled", true);
    if (dsp->cw_live.enabled) {
        char current_symbol[CW_LIVE_SYMBOL_MAX];
        int estimated_wpm = dsp->cw_live.unit_ms > 0 ? (1200 + dsp->cw_live.unit_ms / 2) / dsp->cw_live.unit_ms : 0;
        double keyed_percent = dsp->cw_live.total_ms > 0
            ? (100.0 * (double)dsp->cw_live.keyed_ms / (double)dsp->cw_live.total_ms)
            : 0.0;

        memcpy(current_symbol, dsp->cw_live.pattern, sizeof(current_symbol));
        current_symbol[sizeof(current_symbol) - 1] = '\0';
        fprintf(out, "  \"cw_decode\": {\n");
        fprintf(out, "    \"decode_supported\": true,\n");
        fprintf(out, "    \"requested_wpm\": %ld,\n", dsp->cw_live.requested_wpm);
        fprintf(out, "    \"estimated_wpm\": %d,\n", estimated_wpm);
        fprintf(out, "    \"estimated_unit_ms\": %d,\n", dsp->cw_live.unit_ms);
        fprintf(out, "    \"keyed_percent\": %.2f,\n", keyed_percent);
        fprintf(out, "    \"keying_segments\": %u,\n", dsp->cw_live.keying_segments);
        fprintf(out, "    \"envelope_floor\": %.8g,\n", dsp->cw_live.floor);
        fprintf(out, "    \"envelope_peak\": %.8g,\n", dsp->cw_live.peak);
        fprintf(out, "    \"envelope_threshold\": %.8g,\n", dsp->cw_live.threshold);
        fprintf(out, "    \"current_symbol\": ");
        json_string(out, current_symbol);
        fputs(",\n", out);
        fprintf(out, "    \"decoded_symbols\": ");
        json_string(out, dsp->cw_live.decoded_symbols);
        fputs(",\n", out);
        fprintf(out, "    \"decoded_text\": ");
        json_string(out, dsp->cw_live.decoded_text);
        fputs(",\n", out);
        fprintf(out, "    \"timing_source\": \"streaming_auto\"\n");
        fprintf(out, "  },\n");
    } else {
        fprintf(out, "  \"cw_decode\": null,\n");
    }
    fprintf(out, "  \"ft8_decode\": ");
    pluto_ft8_decoder_write_json(dsp->ft8, out);
    fputs(",\n", out);
    json_string_field(out, "started_utc", getenv("PLUTO_AUDIO_STARTED_UTC"), true);
    json_string_field(out, "state", "running", true);
    json_string_field(out, "stream_format", env_default("PLUTO_AUDIO_STREAM_FORMAT", "pcm_s16le"), true);
    json_string_field(out, "updated_utc", now, false);
    fputs("}\n", out);

    if (fclose(out) == 0) {
        if (rename(tmp, path) != 0)
            unlink(tmp);
    } else {
        unlink(tmp);
    }
    dsp->pcm_power_acc = 0.0;
    dsp->pcm_power_count = 0;
}

static void maybe_write_audio_running_report(struct dsp_state *dsp, bool force)
{
    time_t raw = time(NULL);

    if (!force && raw == dsp->last_status_time)
        return;
    dsp->last_status_time = raw;
    write_audio_running_report(dsp);
}

static FILE *open_audio_sink(const char *fifo, bool regular_file)
{
    if (regular_file)
        return fopen(fifo, "wb");

    for (;;) {
        FILE *sink;

        errno = 0;
        sink = fopen(fifo, "wb");
        if (sink)
            return sink;
        if (errno == EINTR && keep_running) {
            fprintf(stderr, "audio sink open interrupted for %s; retrying\n", fifo);
            continue;
        }
        return NULL;
    }
}

static enum demod_mode parse_demod(const char *value)
{
    if (!value)
        return DEMOD_NFM;
    if (!strcmp(value, "wfm"))
        return DEMOD_WFM;
    if (!strcmp(value, "am"))
        return DEMOD_AM;
    if (!strcmp(value, "cw"))
        return DEMOD_CW;
    if (!strcmp(value, "ft8"))
        return DEMOD_FT8;
    return DEMOD_NFM;
}

static enum agc_mode parse_agc(const char *value)
{
    if (!value || !value[0] || !strcmp(value, "manual") || !strcmp(value, "off"))
        return AGC_OFF;
    if (!strcmp(value, "fast_attack"))
        return AGC_FAST;
    if (!strcmp(value, "slow_attack") || !strcmp(value, "normalize"))
        return AGC_SLOW;
    return AGC_OFF;
}

static enum iq_mode parse_iq_mode(const char *value)
{
    if (!value || !value[0] || !strcmp(value, "normal"))
        return IQ_NORMAL;
    if (!strcmp(value, "swap"))
        return IQ_SWAP;
    if (!strcmp(value, "invert_i"))
        return IQ_INVERT_I;
    if (!strcmp(value, "invert_q") || !strcmp(value, "conjugate"))
        return IQ_INVERT_Q;
    if (!strcmp(value, "invert_both"))
        return IQ_INVERT_BOTH;
    if (!strcmp(value, "swap_invert_i"))
        return IQ_SWAP_INVERT_I;
    if (!strcmp(value, "swap_invert_q"))
        return IQ_SWAP_INVERT_Q;
    if (!strcmp(value, "swap_invert_both"))
        return IQ_SWAP_INVERT_BOTH;
    return IQ_NORMAL;
}

static const char *iq_mode_name(enum iq_mode mode)
{
    switch (mode) {
    case IQ_SWAP:
        return "swap";
    case IQ_INVERT_I:
        return "invert_i";
    case IQ_INVERT_Q:
        return "invert_q";
    case IQ_INVERT_BOTH:
        return "invert_both";
    case IQ_SWAP_INVERT_I:
        return "swap_invert_i";
    case IQ_SWAP_INVERT_Q:
        return "swap_invert_q";
    case IQ_SWAP_INVERT_BOTH:
        return "swap_invert_both";
    case IQ_NORMAL:
    default:
        return "normal";
    }
}

static float deemphasis_tau(const char *value)
{
    if (!value)
        return 0.0f;
    if (!strcmp(value, "75us"))
        return 75.0e-6f;
    if (!strcmp(value, "50us"))
        return 50.0e-6f;
    return 0.0f;
}

static float one_pole_alpha(float cutoff_hz, float sample_rate_hz)
{
    float alpha;

    if (cutoff_hz <= 0.0f || sample_rate_hz <= 0.0f)
        return 0.0f;
    alpha = 1.0f - expf(-2.0f * (float)M_PI * cutoff_hz / sample_rate_hz);
    if (alpha < 0.001f)
        return 0.001f;
    if (alpha > 1.0f)
        return 1.0f;
    return alpha;
}

static float dbfs(float value)
{
    if (value < 1.0e-6f)
        value = 1.0e-6f;
    return 20.0f * log10f(value);
}

static int16_t clamp_pcm(float value)
{
    if (value > INT16_CLIP)
        return 32767;
    if (value < -INT16_CLIP)
        return -32767;
    return (int16_t)value;
}

static int write_pcm(FILE *sink, int16_t sample)
{
    uint8_t bytes[2];

    bytes[0] = (uint8_t)(sample & 0xff);
    bytes[1] = (uint8_t)((sample >> 8) & 0xff);
    return fwrite(bytes, 1, sizeof(bytes), sink) == sizeof(bytes) ? 0 : -1;
}

static void dsp_init(struct dsp_state *dsp, enum demod_mode mode, long input_rate,
                     long audio_rate, long cw_bfo, long filter_width_hz,
                     float squelch_db, const char *deemphasis, enum agc_mode agc,
                     long frequency_shift_hz, float output_gain, float noise_gate_db,
                     bool dc_block, enum iq_mode iq_mode, bool fm_limiter_enabled,
                     bool fm_channel_filter_enabled)
{
    float tau = deemphasis_tau(deemphasis);
    float filter_cutoff = (float)filter_width_hz * 0.5f;
    long processing_rate;
    long minimum_processing_rate = audio_rate * 4;

    /*
     * The AD9361 runs the NOAA and satellite profiles at 2.4 MS/s, but
     * demodulating every raw IQ sample costs enough CPU to fall behind the
     * DMA stream.  Keep enough complex-sample bandwidth for the requested
     * audio filter, then do the expensive demodulation at that lower rate.
     */
    if (filter_width_hz > 0 && filter_width_hz * 3 > minimum_processing_rate)
        minimum_processing_rate = filter_width_hz * 3;
    if (mode == DEMOD_WFM && minimum_processing_rate < 480000)
        minimum_processing_rate = 480000;

    memset(dsp, 0, sizeof(*dsp));
    dsp->mode = mode;
    dsp->agc = agc;
    dsp->iq_mode = iq_mode;
    dsp->iq_decim = (unsigned)(input_rate / minimum_processing_rate);
    /*
     * FM must discriminate consecutive complex samples before audio-rate
     * decimation. Averaging I/Q first smears the instantaneous phase delta and
     * turns weak NFM voice into static even when the RF samples are valid.
     */
    if (mode == DEMOD_NFM || mode == DEMOD_WFM)
        dsp->iq_decim = 1;
    if (dsp->iq_decim < 1)
        dsp->iq_decim = 1;
    processing_rate = input_rate / (long)dsp->iq_decim;
    dsp->audio_rate = (unsigned)audio_rate;
    dsp->processing_rate = (unsigned)processing_rate;
    dsp->iq_shift_step = -2.0f * (float)M_PI * (float)frequency_shift_hz / (float)processing_rate;
    dsp->iq_shift_cos = 1.0f;
    dsp->iq_shift_sin = 0.0f;
    dsp->iq_shift_step_cos = cosf(dsp->iq_shift_step);
    dsp->iq_shift_step_sin = sinf(dsp->iq_shift_step);
    dsp->fm_limiter_enabled = fm_limiter_enabled;
    dsp->fm_channel_filter_enabled = fm_channel_filter_enabled &&
        (mode == DEMOD_NFM || mode == DEMOD_WFM) && filter_cutoff > 0.0f;
    dsp->fm_channel_alpha = dsp->fm_channel_filter_enabled
        ? one_pole_alpha(filter_cutoff, (float)processing_rate)
        : 1.0f;
    dsp->fm_limiter_floor = 1.0e-5f;
    dsp->am_avg = 0.01f;
    dsp->cw_step = 2.0f * (float)M_PI * (float)cw_bfo / (float)audio_rate;
    dsp->squelch_threshold_db = squelch_db;
    dsp->squelch_enabled = squelch_db > -119.0f;
    dsp->squelch_open = !dsp->squelch_enabled;
    dsp->squelch_hang = (unsigned)((float)processing_rate * 0.18f);
    if (dsp->squelch_hang < 1)
        dsp->squelch_hang = 1;
    dsp->dc_block = dc_block;
    dsp->deemphasis_enabled = tau > 0.0f;
    dsp->deemphasis_alpha = dsp->deemphasis_enabled
        ? ((1.0f / (float)audio_rate) / (tau + (1.0f / (float)audio_rate)))
        : 1.0f;
    dsp->output_gain = output_gain;
    dsp->noise_gate_threshold_db = noise_gate_db;
    dsp->noise_gate_enabled = noise_gate_db > -119.0f;
    dsp->agc_gain = 1.0f;
    dsp->agc_target = 9000.0f;
    dsp->agc_attack = agc == AGC_FAST ? 0.020f : 0.004f;
    dsp->agc_release = agc == AGC_FAST ? 0.004f : 0.001f;
    cw_live_init(&dsp->cw_live, mode, dsp->audio_rate);
    if (mode == DEMOD_FT8)
        dsp->ft8 = pluto_ft8_decoder_create(dsp->audio_rate);

    if (mode == DEMOD_WFM) {
        dsp->scale = 15500.0f;
        dsp->alpha = 0.18f;
    } else if (mode == DEMOD_NFM) {
        dsp->scale = 10500.0f;
        dsp->alpha = 0.12f;
    } else if (mode == DEMOD_CW) {
        dsp->scale = 24000.0f;
    } else if (mode == DEMOD_FT8) {
        dsp->scale = 24000.0f;
        dsp->alpha = 0.04f;
    } else {
        dsp->scale = 18000.0f;
        dsp->alpha = 0.06f;
    }
    if (filter_cutoff > 0.0f)
        dsp->alpha = one_pole_alpha(filter_cutoff, (float)processing_rate);
}

static void dsp_destroy(struct dsp_state *dsp)
{
    pluto_ft8_decoder_destroy(dsp->ft8);
    dsp->ft8 = NULL;
}

static int dsp_emit(struct dsp_state *dsp, FILE *sink, float sample)
{
    dsp->lp += (sample - dsp->lp) * dsp->alpha;
    dsp->acc += dsp->lp;
    dsp->count++;
    dsp->output_rate_acc += dsp->audio_rate;
    if (dsp->output_rate_acc < dsp->processing_rate)
        return 0;

    dsp->output_rate_acc -= dsp->processing_rate;

    float audio = dsp->acc / (float)dsp->count;
    dsp->acc = 0.0f;
    dsp->count = 0;

    if (dsp->squelch_enabled && !dsp->squelch_open)
        audio = 0.0f;

    if (dsp->mode == DEMOD_CW) {
        audio *= sinf(dsp->cw_phase);
        dsp->cw_phase += dsp->cw_step;
        if (dsp->cw_phase > (float)M_PI)
            dsp->cw_phase -= 2.0f * (float)M_PI;
    }

    if (dsp->dc_block) {
        dsp->dc += (audio - dsp->dc) * 0.0025f;
        audio -= dsp->dc;
    }
    if (dsp->deemphasis_enabled) {
        dsp->deemphasis_lp += (audio - dsp->deemphasis_lp) * dsp->deemphasis_alpha;
        audio = dsp->deemphasis_lp;
    }
    if (dsp->agc != AGC_OFF) {
        float abs_audio = fabsf(audio);
        float desired;

        dsp->audio_level += (abs_audio - dsp->audio_level) * 0.0025f;
        desired = dsp->agc_target / fmaxf(dsp->audio_level, 1.0f);
        if (desired > 12.0f)
            desired = 12.0f;
        if (desired < 0.1f)
            desired = 0.1f;
        dsp->agc_gain += (desired - dsp->agc_gain) *
            (desired < dsp->agc_gain ? dsp->agc_attack : dsp->agc_release);
        audio *= dsp->agc_gain;
    }
    audio *= dsp->output_gain;
    if (dsp->noise_gate_enabled && dbfs(fabsf(audio) / INT16_CLIP) < dsp->noise_gate_threshold_db)
        audio = 0.0f;
    cw_live_add_audio(&dsp->cw_live, audio);
    pluto_ft8_decoder_add_audio(dsp->ft8, audio);
    int16_t pcm = clamp_pcm(audio);
    if (write_pcm(sink, pcm) < 0)
        return -1;
    dsp->pcm_samples++;
    dsp->pcm_power_acc += (double)pcm * (double)pcm;
    dsp->pcm_power_count++;
    maybe_write_audio_running_report(dsp, false);
    return 0;
}

static int dsp_process_iq(struct dsp_state *dsp, FILE *sink, int16_t i_raw, int16_t q_raw)
{
    float i_val = (float)i_raw / 32768.0f;
    float q_val = (float)q_raw / 32768.0f;
    float mag;
    float sample = 0.0f;

    switch (dsp->iq_mode) {
    case IQ_SWAP: {
        float tmp = i_val;
        i_val = q_val;
        q_val = tmp;
        break;
    }
    case IQ_INVERT_I:
        i_val = -i_val;
        break;
    case IQ_INVERT_Q:
        q_val = -q_val;
        break;
    case IQ_INVERT_BOTH:
        i_val = -i_val;
        q_val = -q_val;
        break;
    case IQ_SWAP_INVERT_I: {
        float tmp = i_val;
        i_val = -q_val;
        q_val = tmp;
        break;
    }
    case IQ_SWAP_INVERT_Q: {
        float tmp = i_val;
        i_val = q_val;
        q_val = -tmp;
        break;
    }
    case IQ_SWAP_INVERT_BOTH: {
        float tmp = i_val;
        i_val = -q_val;
        q_val = -tmp;
        break;
    }
    case IQ_NORMAL:
    default:
        break;
    }

    if (dsp->iq_shift_step != 0.0f) {
        float c = dsp->iq_shift_cos;
        float s = dsp->iq_shift_sin;
        float mixed_i = i_val * c - q_val * s;
        float mixed_q = i_val * s + q_val * c;
        float next_c = c * dsp->iq_shift_step_cos - s * dsp->iq_shift_step_sin;
        float next_s = s * dsp->iq_shift_step_cos + c * dsp->iq_shift_step_sin;
        i_val = mixed_i;
        q_val = mixed_q;
        dsp->iq_shift_cos = next_c;
        dsp->iq_shift_sin = next_s;
        dsp->iq_shift_renorm++;
        if (dsp->iq_shift_renorm >= 4096U) {
            float norm = sqrtf(dsp->iq_shift_cos * dsp->iq_shift_cos +
                               dsp->iq_shift_sin * dsp->iq_shift_sin);
            if (norm > 0.0f) {
                dsp->iq_shift_cos /= norm;
                dsp->iq_shift_sin /= norm;
            }
            dsp->iq_shift_renorm = 0;
        }
    }

    if ((dsp->mode == DEMOD_NFM || dsp->mode == DEMOD_WFM) && dsp->fm_channel_filter_enabled) {
        if (!dsp->fm_channel_ready) {
            dsp->fm_channel_i = i_val;
            dsp->fm_channel_q = q_val;
            dsp->fm_channel_ready = true;
        } else {
            dsp->fm_channel_i += (i_val - dsp->fm_channel_i) * dsp->fm_channel_alpha;
            dsp->fm_channel_q += (q_val - dsp->fm_channel_q) * dsp->fm_channel_alpha;
        }
        i_val = dsp->fm_channel_i;
        q_val = dsp->fm_channel_q;
    }

    mag = sqrtf(i_val * i_val + q_val * q_val);
    if ((dsp->mode == DEMOD_NFM || dsp->mode == DEMOD_WFM) && dsp->fm_limiter_enabled) {
        float limit_mag = mag;
        if (limit_mag < dsp->fm_limiter_floor)
            limit_mag = dsp->fm_limiter_floor;
        i_val /= limit_mag;
        q_val /= limit_mag;
        mag = 1.0f;
    }

    dsp->signal_level += (mag - dsp->signal_level) * 0.0015f;
    if (dsp->squelch_enabled) {
        if (dbfs(dsp->signal_level) >= dsp->squelch_threshold_db) {
            dsp->squelch_open = true;
            dsp->squelch_hold = dsp->squelch_hang;
        } else if (dsp->squelch_hold > 0) {
            dsp->squelch_hold--;
            dsp->squelch_open = true;
        } else {
            dsp->squelch_open = false;
        }
    }

    if (dsp->mode == DEMOD_NFM || dsp->mode == DEMOD_WFM) {
        if (dsp->have_prev_iq) {
            float cross = dsp->prev_i * q_val - dsp->prev_q * i_val;
            float dot = dsp->prev_i * i_val + dsp->prev_q * q_val;
            sample = atan2f(cross, dot);
        }
        dsp->prev_i = i_val;
        dsp->prev_q = q_val;
        dsp->have_prev_iq = true;
        sample *= dsp->scale;
    } else if (dsp->mode == DEMOD_FT8) {
        /*
         * The Pluto is tuned to the FT8 dial frequency, so positive complex
         * baseband is the USB audio passband. Taking I produces the real
         * audio waveform after the existing channel filter/decimator.
         */
        sample = i_val * dsp->scale;
    } else {
        dsp->am_avg += (mag - dsp->am_avg) * 0.0015f;
        if (dsp->mode == DEMOD_CW)
            sample = fmaxf(0.0f, mag - dsp->am_avg * 0.72f) * dsp->scale;
        else
            sample = (mag - dsp->am_avg) * dsp->scale;
    }
    return dsp_emit(dsp, sink, sample);
}

static int dsp_push_iq(struct dsp_state *dsp, FILE *sink, int16_t i_raw, int16_t q_raw)
{
    int16_t i_average;
    int16_t q_average;

    dsp->i_acc += i_raw;
    dsp->q_acc += q_raw;
    dsp->iq_count++;
    if (dsp->iq_count < dsp->iq_decim)
        return 0;

    i_average = (int16_t)(dsp->i_acc / (int64_t)dsp->iq_count);
    q_average = (int16_t)(dsp->q_acc / (int64_t)dsp->iq_count);
    dsp->i_acc = 0;
    dsp->q_acc = 0;
    dsp->iq_count = 0;
    return dsp_process_iq(dsp, sink, i_average, q_average);
}

static int run_synthetic(FILE *sink, struct dsp_state *dsp, long input_rate, long seconds)
{
    long total = seconds > 0 ? input_rate * seconds : input_rate;
    float phase = 0.0f;
    float step = 2.0f * (float)M_PI * 1200.0f / (float)input_rate;

    for (long n = 0; keep_running && n < total; n++) {
        int16_t i_val = (int16_t)(cosf(phase) * 18000.0f);
        int16_t q_val = (int16_t)(sinf(phase) * 18000.0f);
        if (dsp_push_iq(dsp, sink, i_val, q_val) < 0)
            return -1;
        phase += step;
        if (phase > (float)M_PI)
            phase -= 2.0f * (float)M_PI;
    }
    return 0;
}

static int run_synthetic_fm(FILE *sink, struct dsp_state *dsp, long input_rate, long seconds)
{
    long processing_rate = (long)dsp->processing_rate;
    long total;
    long audio_tone_hz = env_long("PLUTO_AUDIO_SYNTHETIC_TONE_HZ", 1000, 20, 3000);
    long fm_deviation_hz = env_long("PLUTO_AUDIO_SYNTHETIC_FM_DEVIATION_HZ", 5000, 100, 25000);
    long carrier_hz = env_long("PLUTO_AUDIO_SYNTHETIC_CARRIER_HZ", 0, -input_rate / 2, input_rate / 2);
    float phase = 0.0f;
    float audio_phase = 0.0f;
    float audio_step;
    float carrier_step;
    float deviation_scale;

    if (processing_rate < 1)
        processing_rate = input_rate;
    total = seconds > 0 ? processing_rate * seconds : processing_rate;
    audio_step = 2.0f * (float)M_PI * (float)audio_tone_hz / (float)processing_rate;
    carrier_step = 2.0f * (float)M_PI * (float)carrier_hz / (float)processing_rate;
    deviation_scale = 2.0f * (float)M_PI * (float)fm_deviation_hz / (float)processing_rate;

    for (long n = 0; keep_running && n < total; n++) {
        float audio = sinf(audio_phase);
        int16_t i_val;
        int16_t q_val;

        phase += carrier_step + deviation_scale * audio;
        if (phase > (float)M_PI || phase < -(float)M_PI)
            phase = fmodf(phase, 2.0f * (float)M_PI);
        audio_phase += audio_step;
        if (audio_phase > (float)M_PI)
            audio_phase -= 2.0f * (float)M_PI;

        i_val = (int16_t)(cosf(phase) * 18000.0f);
        q_val = (int16_t)(sinf(phase) * 18000.0f);
        if (dsp_process_iq(dsp, sink, i_val, q_val) < 0)
            return -1;
    }
    return 0;
}

static int run_iq_file(FILE *sink, struct dsp_state *dsp)
{
    const char *path = getenv("PLUTO_AUDIO_IQ_FILE");
    const char *format = env_default("PLUTO_AUDIO_IQ_FILE_FORMAT", "s12");
    long max_samples = env_long("PLUTO_AUDIO_IQ_FILE_MAX_SAMPLES", 0, 0, 1000000000L);
    FILE *in;
    uint8_t bytes[4];
    long samples = 0;

    if (!path || !path[0]) {
        fprintf(stderr, "PLUTO_AUDIO_IQ_FILE is required for iq_file source\n");
        return -1;
    }
    in = fopen(path, "rb");
    if (!in) {
        fprintf(stderr, "could not open IQ file %s: %s\n", path, strerror(errno));
        return -1;
    }

    backend_phase = "iq_file_streaming";
    maybe_write_audio_running_report(dsp, true);
    while (keep_running && fread(bytes, 1, sizeof(bytes), in) == sizeof(bytes)) {
        int16_t i_sample;
        int16_t q_sample;

        if (!strcmp(format, "s16")) {
            memcpy(&i_sample, bytes, sizeof(i_sample));
            memcpy(&q_sample, bytes + sizeof(uint16_t), sizeof(q_sample));
        } else {
            i_sample = unpack_ad9361_s12_sample(bytes);
            q_sample = unpack_ad9361_s12_sample(bytes + sizeof(uint16_t));
        }
        if (dsp_push_iq(dsp, sink, i_sample, q_sample) < 0) {
            fprintf(stderr, "audio sink write failed while processing IQ file: %s\n", strerror(errno));
            fclose(in);
            return -1;
        }
        samples++;
        if (max_samples > 0 && samples >= max_samples)
            break;
    }
    if (ferror(in)) {
        fprintf(stderr, "could not read IQ file %s: %s\n", path, strerror(errno));
        fclose(in);
        return -1;
    }
    fclose(in);
    fflush(sink);
    maybe_write_audio_running_report(dsp, true);
    return 0;
}

static int run_iio(FILE *sink, struct dsp_state *dsp)
{
    const char *device_name = env_default("PLUTO_IIO_DEVICE", DEFAULT_DEVICE);
    const char *i_name = env_default("PLUTO_IIO_I_CHANNEL", DEFAULT_I_CHAN);
    const char *q_name = env_default("PLUTO_IIO_Q_CHANNEL", DEFAULT_Q_CHAN);
    long buffer_samples = env_long("PLUTO_IIO_BUFFER_SAMPLES", DEFAULT_BUFFER_SAMPLES, 256, 65536);
    struct iio_context *ctx = NULL;
    struct iio_device *dev = NULL;
    struct iio_channel *i_chan = NULL;
    struct iio_channel *q_chan = NULL;
    struct iio_buffer *buf = NULL;
    long timeout_ms = env_long("PLUTO_IIO_TIMEOUT_MS", 3000, 100, 30000);
    int ret = 1;

    backend_phase = "iio_context_create";
    maybe_write_audio_running_report(dsp, true);
    ctx = iio_create_default_context();
    if (!ctx) {
        fprintf(stderr, "could not create IIO context\n");
        goto out;
    }
    iio_context_set_timeout(ctx, (unsigned int)timeout_ms);
    backend_phase = "iio_find_device";
    maybe_write_audio_running_report(dsp, true);
    dev = iio_context_find_device(ctx, device_name);
    if (!dev) {
        fprintf(stderr, "could not find IIO device: %s\n", device_name);
        goto out;
    }
    backend_phase = "iio_find_channels";
    maybe_write_audio_running_report(dsp, true);
    i_chan = iio_device_find_channel(dev, i_name, false);
    q_chan = iio_device_find_channel(dev, q_name, false);
    if (!i_chan || !q_chan) {
        fprintf(stderr, "could not find IIO channels: %s/%s\n", i_name, q_name);
        goto out;
    }
    backend_phase = "iio_enable_channels";
    maybe_write_audio_running_report(dsp, true);
    iio_channel_enable(i_chan);
    iio_channel_enable(q_chan);
    backend_phase = "iio_create_buffer";
    maybe_write_audio_running_report(dsp, true);
    buf = iio_device_create_buffer(dev, (size_t)buffer_samples, false);
    if (!buf) {
        fprintf(stderr, "could not create IIO buffer\n");
        goto out;
    }

    backend_phase = "streaming";
    maybe_write_audio_running_report(dsp, true);
    while (keep_running) {
        ssize_t refill = iio_buffer_refill(buf);
        if (refill < 0) {
            fprintf(stderr, "IIO buffer refill failed: %zd\n", refill);
            goto out;
        }
        if (refill == 0) {
            fprintf(stderr, "IIO buffer refill returned no bytes\n");
            goto out;
        }
        char *i_ptr = iio_buffer_first(buf, i_chan);
        char *q_ptr = iio_buffer_first(buf, q_chan);
        char *end = iio_buffer_end(buf);
        ptrdiff_t step = iio_buffer_step(buf);
        if (!i_ptr || !q_ptr || !end || step <= 0) {
            fprintf(stderr, "invalid IIO buffer layout: i=%p q=%p end=%p step=%td\n",
                    (void *)i_ptr, (void *)q_ptr, (void *)end, step);
            goto out;
        }
        dsp->iio_refills++;
        for (; keep_running && i_ptr < end && q_ptr < end; i_ptr += step, q_ptr += step) {
            int16_t i_sample = unpack_ad9361_s12_sample(i_ptr);
            int16_t q_sample = unpack_ad9361_s12_sample(q_ptr);
            if (dsp_push_iq(dsp, sink, i_sample, q_sample) < 0) {
                fprintf(stderr, "audio sink write failed after %" PRIu64 " PCM bytes: %s\n",
                        dsp->pcm_samples * 2U, strerror(errno));
                goto out;
            }
        }
        fflush(sink);
        maybe_write_audio_running_report(dsp, true);
    }
    ret = 0;

out:
    if (buf)
        iio_buffer_destroy(buf);
    if (ctx)
        iio_context_destroy(ctx);
    return ret;
}

int main(void)
{
    const char *fifo = getenv("PLUTO_AUDIO_FIFO");
    const char *source = env_default("PLUTO_AUDIO_SOURCE", "iio");
    long audio_rate = env_long("PLUTO_AUDIO_RATE_HZ", DEFAULT_AUDIO_RATE, 4000, 48000);
    long input_rate = env_long("PLUTO_DSP_INPUT_RATE_HZ", DEFAULT_IN_RATE, audio_rate, 5000000);
    long filter_width_hz = env_long("PLUTO_AUDIO_FILTER_WIDTH_HZ", 0, 0, input_rate);
    long cw_bfo = env_long("PLUTO_AUDIO_CW_BFO_HZ", 700, 100, 3000);
    long frequency_shift_hz = env_long("PLUTO_DSP_FREQUENCY_SHIFT_HZ", 0, -input_rate / 2, input_rate / 2);
    enum iq_mode iq_mode = parse_iq_mode(env_default("PLUTO_DSP_IQ_MODE", "normal"));
    long test_seconds = env_long("PLUTO_AUDIO_TEST_SECONDS", 0, 0, 3600);
    float squelch_db = env_float("PLUTO_AUDIO_SQUELCH_DB", -120.0f, -120.0f, 0.0f);
    float output_gain = env_float("PLUTO_AUDIO_OUTPUT_GAIN", 1.0f, 0.0f, 16.0f);
    float noise_gate_db = env_float("PLUTO_AUDIO_NOISE_GATE_DB", -120.0f, -120.0f, 0.0f);
    bool dc_block = env_bool("PLUTO_AUDIO_DC_BLOCK", true);
    bool fm_limiter_enabled = env_bool("PLUTO_AUDIO_FM_LIMITER", true);
    bool fm_channel_filter_enabled = env_bool("PLUTO_AUDIO_FM_CHANNEL_FILTER", true);
    enum demod_mode mode = parse_demod(env_default("PLUTO_AUDIO_DEMOD", "nfm"));
    enum agc_mode agc = parse_agc(env_default("PLUTO_AUDIO_AGC", "manual"));
    const char *deemphasis = env_default("PLUTO_AUDIO_DEEMPHASIS", "none");
    struct dsp_state dsp;
    FILE *sink = NULL;
    int ret;
    bool regular_sink = !strcmp(source, "iq_file");

    if (!fifo || !fifo[0]) {
        fprintf(stderr, "PLUTO_AUDIO_FIFO is required\n");
        write_audio_error("audio_fifo_missing", "PLUTO_AUDIO_FIFO is required", 0);
        return 2;
    }
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    backend_phase = "fifo_wait_for_reader";
    sink = open_audio_sink(fifo, regular_sink);
    if (!sink) {
        int errnum = errno;
        char message[256];
        if (!keep_running)
            return 0;
        snprintf(message, sizeof(message), "could not open audio sink %s: %s", fifo, strerror(errnum));
        fprintf(stderr, "%s\n", message);
        write_audio_error("audio_sink_open_failed", message, errnum);
        return 2;
    }

    backend_phase = "dsp_init";
    dsp_init(&dsp, mode, input_rate, audio_rate, cw_bfo, filter_width_hz,
             squelch_db, deemphasis, agc, frequency_shift_hz, output_gain, noise_gate_db,
             dc_block, iq_mode, fm_limiter_enabled, fm_channel_filter_enabled);
    backend_phase = "running";
    if (!strcmp(source, "synthetic"))
        ret = run_synthetic(sink, &dsp, input_rate, test_seconds);
    else if (!strcmp(source, "synthetic_fm"))
        ret = run_synthetic_fm(sink, &dsp, input_rate, test_seconds);
    else if (!strcmp(source, "iq_file"))
        ret = run_iq_file(sink, &dsp);
    else if (!strcmp(source, "iio"))
        ret = run_iio(sink, &dsp);
    else {
        fprintf(stderr, "unsupported PLUTO_AUDIO_SOURCE: %s\n", source);
        write_audio_error("unsupported_audio_source", "unsupported PLUTO_AUDIO_SOURCE", 0);
        ret = -1;
    }
    maybe_write_audio_running_report(&dsp, true);
    dsp_destroy(&dsp);
    if (ret != 0 && keep_running)
        write_audio_error("audio_backend_stream_failed", "audio backend failed while streaming", errno);
    fclose(sink);
    return ret == 0 ? 0 : 1;
}
