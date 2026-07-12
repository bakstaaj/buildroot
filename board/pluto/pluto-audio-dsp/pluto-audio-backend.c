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

#define DEFAULT_DEVICE "cf-ad9361-lpc"
#define DEFAULT_I_CHAN "voltage0"
#define DEFAULT_Q_CHAN "voltage1"
#define DEFAULT_IN_RATE 600000
#define DEFAULT_AUDIO_RATE 12000
#define DEFAULT_BUFFER_SAMPLES 4096
#define INT16_CLIP 32767.0f
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum demod_mode {
    DEMOD_NFM,
    DEMOD_WFM,
    DEMOD_AM,
    DEMOD_CW,
};

enum agc_mode {
    AGC_OFF,
    AGC_SLOW,
    AGC_FAST,
};

static volatile sig_atomic_t keep_running = 1;
static const char *backend_phase = "starting";

struct dsp_state {
    enum demod_mode mode;
    enum agc_mode agc;
    freqdem fm;
    unsigned decim;
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
    bool deemphasis_enabled;
    float deemphasis_alpha;
    float deemphasis_lp;
    float output_gain;
    bool noise_gate_enabled;
    float noise_gate_threshold_db;
    float audio_level;
    uint64_t pcm_samples;
    double pcm_power_acc;
    unsigned pcm_power_count;
    unsigned iio_refills;
    time_t last_status_time;
    float agc_gain;
    float agc_target;
    float agc_attack;
    float agc_release;
};

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

static void write_audio_running_report(struct dsp_state *dsp)
{
    const char *path = env_default("PLUTO_AUDIO_BACKEND_STATUS_FILE", "/var/run/pluto-radio/audio-backend-status.json");
    char tmp[512];
    char now[32];
    FILE *out;
    int written;
    float rms = 0.0f;

    if (!path || !path[0])
        return;
    if (dsp->pcm_power_count > 0)
        rms = sqrtf((float)(dsp->pcm_power_acc / (double)dsp->pcm_power_count)) / INT16_CLIP;

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
    fprintf(out, "  \"last_error\": null,\n");
    json_string_field(out, "phase", backend_phase, true);
    fprintf(out, "  \"pcm_bytes\": %" PRIu64 ",\n", dsp->pcm_samples * 2U);
    fprintf(out, "  \"pid\": %ld,\n", (long)getpid());
    json_string_field(out, "profile", getenv("PLUTO_AUDIO_PROFILE"), true);
    fprintf(out, "  \"rms_level\": %.8g,\n", rms);
    json_string_field(out, "squelch_state", dsp->squelch_enabled ? (dsp->squelch_open ? "open" : "closed") : "disabled", true);
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

static FILE *open_audio_sink(const char *fifo)
{
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
                     float output_gain, float noise_gate_db, bool dc_block)
{
    float tau = deemphasis_tau(deemphasis);
    float filter_cutoff = (float)filter_width_hz * 0.5f;

    memset(dsp, 0, sizeof(*dsp));
    dsp->mode = mode;
    dsp->agc = agc;
    dsp->decim = (unsigned)lroundf((float)input_rate / (float)audio_rate);
    if (dsp->decim < 1)
        dsp->decim = 1;
    dsp->am_avg = 0.01f;
    dsp->cw_step = 2.0f * (float)M_PI * (float)cw_bfo / (float)audio_rate;
    dsp->squelch_threshold_db = squelch_db;
    dsp->squelch_enabled = squelch_db > -119.0f;
    dsp->squelch_open = !dsp->squelch_enabled;
    dsp->squelch_hang = (unsigned)((float)input_rate * 0.18f);
    if (dsp->squelch_hang < dsp->decim)
        dsp->squelch_hang = dsp->decim;
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

    if (mode == DEMOD_WFM) {
        dsp->fm = freqdem_create(0.08f);
        dsp->scale = 15500.0f;
        dsp->alpha = 0.18f;
    } else if (mode == DEMOD_NFM) {
        dsp->fm = freqdem_create(0.42f);
        dsp->scale = 10500.0f;
        dsp->alpha = 0.12f;
    } else if (mode == DEMOD_CW) {
        dsp->scale = 24000.0f;
        dsp->alpha = 0.04f;
    } else {
        dsp->scale = 18000.0f;
        dsp->alpha = 0.06f;
    }
    if (filter_cutoff > 0.0f)
        dsp->alpha = one_pole_alpha(filter_cutoff, (float)input_rate);
}

static void dsp_destroy(struct dsp_state *dsp)
{
    if (dsp->fm)
        freqdem_destroy(dsp->fm);
}

static int dsp_emit(struct dsp_state *dsp, FILE *sink, float sample)
{
    dsp->lp += (sample - dsp->lp) * dsp->alpha;
    dsp->acc += dsp->lp;
    dsp->count++;
    if (dsp->count < dsp->decim)
        return 0;

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
    float mag = sqrtf(i_val * i_val + q_val * q_val);
    float sample = 0.0f;

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
        liquid_float_complex x = i_val + q_val * _Complex_I;
        freqdem_demodulate(dsp->fm, x, &sample);
        sample *= dsp->scale;
    } else {
        dsp->am_avg += (mag - dsp->am_avg) * 0.0015f;
        if (dsp->mode == DEMOD_CW)
            sample = fmaxf(0.0f, mag - dsp->am_avg * 0.72f) * dsp->scale;
        else
            sample = (mag - dsp->am_avg) * dsp->scale;
    }
    return dsp_emit(dsp, sink, sample);
}

static int run_synthetic(FILE *sink, struct dsp_state *dsp, long input_rate, long seconds)
{
    long total = seconds > 0 ? input_rate * seconds : input_rate;
    float phase = 0.0f;
    float step = 2.0f * (float)M_PI * 1200.0f / (float)input_rate;

    for (long n = 0; keep_running && n < total; n++) {
        int16_t i_val = (int16_t)(cosf(phase) * 18000.0f);
        int16_t q_val = (int16_t)(sinf(phase) * 18000.0f);
        if (dsp_process_iq(dsp, sink, i_val, q_val) < 0)
            return -1;
        phase += step;
        if (phase > (float)M_PI)
            phase -= 2.0f * (float)M_PI;
    }
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

    backend_phase = "iio_refill_wait";
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
        backend_phase = "iio_streaming";
        dsp->iio_refills++;
        for (; keep_running && i_ptr < end && q_ptr < end; i_ptr += step, q_ptr += step) {
            int16_t i_sample;
            int16_t q_sample;
            memcpy(&i_sample, i_ptr, sizeof(i_sample));
            memcpy(&q_sample, q_ptr, sizeof(q_sample));
            if (dsp_process_iq(dsp, sink, i_sample, q_sample) < 0) {
                fprintf(stderr, "audio sink write failed after %" PRIu64 " PCM bytes: %s\n",
                        dsp->pcm_samples * 2U, strerror(errno));
                goto out;
            }
        }
        fflush(sink);
        backend_phase = "iio_refill_wait";
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
    long test_seconds = env_long("PLUTO_AUDIO_TEST_SECONDS", 0, 0, 3600);
    float squelch_db = env_float("PLUTO_AUDIO_SQUELCH_DB", -120.0f, -120.0f, 0.0f);
    float output_gain = env_float("PLUTO_AUDIO_OUTPUT_GAIN", 1.0f, 0.0f, 16.0f);
    float noise_gate_db = env_float("PLUTO_AUDIO_NOISE_GATE_DB", -120.0f, -120.0f, 0.0f);
    bool dc_block = env_bool("PLUTO_AUDIO_DC_BLOCK", true);
    enum demod_mode mode = parse_demod(env_default("PLUTO_AUDIO_DEMOD", "nfm"));
    enum agc_mode agc = parse_agc(env_default("PLUTO_AUDIO_AGC", "manual"));
    const char *deemphasis = env_default("PLUTO_AUDIO_DEEMPHASIS", "none");
    struct dsp_state dsp;
    FILE *sink = NULL;
    int ret;

    if (!fifo || !fifo[0]) {
        fprintf(stderr, "PLUTO_AUDIO_FIFO is required\n");
        write_audio_error("audio_fifo_missing", "PLUTO_AUDIO_FIFO is required", 0);
        return 2;
    }
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    backend_phase = "fifo_wait_for_reader";
    sink = open_audio_sink(fifo);
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
             squelch_db, deemphasis, agc, output_gain, noise_gate_db, dc_block);
    backend_phase = "running";
    if (!strcmp(source, "synthetic"))
        ret = run_synthetic(sink, &dsp, input_rate, test_seconds);
    else
        ret = run_iio(sink, &dsp);
    maybe_write_audio_running_report(&dsp, true);
    dsp_destroy(&dsp);
    if (ret != 0 && keep_running)
        write_audio_error("audio_backend_stream_failed", "audio backend failed while streaming", errno);
    fclose(sink);
    return ret == 0 ? 0 : 1;
}
