#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <iio.h>
#include <math.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DEVICE "cf-ad9361-lpc"
#define DEFAULT_I_CHAN "voltage0"
#define DEFAULT_Q_CHAN "voltage1"
#define DEFAULT_SAMPLE_RATE 2400000L
#define DEFAULT_BUFFER_SAMPLES 4096L
#define INT16_SCALE 32768.0

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct point {
    long long frequency_hz;
    double power_dbfs;
};

struct capture_context {
    struct iio_context *ctx;
    struct iio_device *dev;
    struct iio_channel *i_chan;
    struct iio_channel *q_chan;
    struct iio_buffer *buf;
    long samples;
};

struct doppler_point {
    long long offset_ms;
    long long frequency_hz;
};

struct doppler_plan {
    long long start_epoch_ms;
    size_t count;
    struct doppler_point *points;
};

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
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static long long env_ll(const char *name, long long fallback, long long minimum, long long maximum)
{
    const char *raw = getenv(name);
    char *end = NULL;
    long long value = fallback;

    if (raw && raw[0]) {
        errno = 0;
        value = strtoll(raw, &end, 10);
        if (errno || end == raw)
            value = fallback;
    }
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static double dbfs(double power)
{
    if (power < 1.0e-18)
        power = 1.0e-18;
    return 10.0 * log10(power);
}

static long long now_epoch_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return ((long long)ts.tv_sec * 1000LL) + ((long long)ts.tv_nsec / 1000000LL);
}

static void free_doppler_plan(struct doppler_plan *plan)
{
    if (!plan)
        return;
    free(plan->points);
    memset(plan, 0, sizeof(*plan));
}

static int load_doppler_plan(struct doppler_plan *plan)
{
    const char *path = getenv("PLUTO_SPECTRUM_DOPPLER_FILE");
    long long start_epoch_ms = env_ll("PLUTO_SPECTRUM_DOPPLER_START_EPOCH_MS", 0LL, 0LL, 4102444800000LL);
    FILE *fp;
    size_t capacity = 0;

    memset(plan, 0, sizeof(*plan));
    if (!path || !path[0] || start_epoch_ms <= 0)
        return 0;

    fp = fopen(path, "r");
    if (!fp)
        return 0;

    plan->start_epoch_ms = start_epoch_ms;
    for (;;) {
        long long offset_ms = 0;
        long long frequency_hz = 0;
        int rc = fscanf(fp, "%lld %lld", &offset_ms, &frequency_hz);
        if (rc == EOF)
            break;
        if (rc != 2) {
            int ch;
            while ((ch = fgetc(fp)) != '\n' && ch != EOF)
                ;
            continue;
        }
        if (frequency_hz < 70000000LL || frequency_hz > 6000000000LL)
            continue;
        if (plan->count >= capacity) {
            size_t next_capacity = capacity ? capacity * 2u : 64u;
            struct doppler_point *next = (struct doppler_point *)realloc(plan->points, next_capacity * sizeof(*next));
            if (!next) {
                fclose(fp);
                free_doppler_plan(plan);
                return 1;
            }
            plan->points = next;
            capacity = next_capacity;
        }
        plan->points[plan->count].offset_ms = offset_ms;
        plan->points[plan->count].frequency_hz = frequency_hz;
        plan->count++;
    }

    fclose(fp);
    return 0;
}

static long long doppler_center_hz(const struct doppler_plan *plan, long long fallback_hz)
{
    long long elapsed_ms;
    size_t i;

    if (!plan || !plan->points || plan->count == 0 || plan->start_epoch_ms <= 0)
        return fallback_hz;

    elapsed_ms = now_epoch_ms() - plan->start_epoch_ms;
    if (elapsed_ms <= plan->points[0].offset_ms)
        return plan->points[0].frequency_hz;

    for (i = 1; i < plan->count; i++) {
        const struct doppler_point *before = &plan->points[i - 1];
        const struct doppler_point *after = &plan->points[i];
        if (elapsed_ms <= after->offset_ms) {
            long long span_ms = after->offset_ms - before->offset_ms;
            double ratio = span_ms > 0 ? (double)(elapsed_ms - before->offset_ms) / (double)span_ms : 0.0;
            double freq = (double)before->frequency_hz + ((double)(after->frequency_hz - before->frequency_hz) * ratio);
            return (long long)llround(freq);
        }
    }

    return plan->points[plan->count - 1].frequency_hz;
}

static int point_cmp_desc(const void *a, const void *b)
{
    const struct point *pa = (const struct point *)a;
    const struct point *pb = (const struct point *)b;

    if (pa->power_dbfs < pb->power_dbfs)
        return 1;
    if (pa->power_dbfs > pb->power_dbfs)
        return -1;
    return 0;
}

static void destroy_capture_context(struct capture_context *cap)
{
    if (!cap)
        return;
    if (cap->buf)
        iio_buffer_destroy(cap->buf);
    if (cap->ctx)
        iio_context_destroy(cap->ctx);
    memset(cap, 0, sizeof(*cap));
}

static int init_capture_context(struct capture_context *cap, long samples)
{
    const char *device_name = env_default("PLUTO_IIO_DEVICE", DEFAULT_DEVICE);
    const char *i_name = env_default("PLUTO_IIO_I_CHANNEL", DEFAULT_I_CHAN);
    const char *q_name = env_default("PLUTO_IIO_Q_CHANNEL", DEFAULT_Q_CHAN);

    memset(cap, 0, sizeof(*cap));
    cap->samples = samples;

    cap->ctx = iio_create_default_context();
    if (!cap->ctx) {
        fprintf(stderr, "could not create IIO context\n");
        goto fail;
    }
    cap->dev = iio_context_find_device(cap->ctx, device_name);
    if (!cap->dev) {
        fprintf(stderr, "could not find IIO device: %s\n", device_name);
        goto fail;
    }
    cap->i_chan = iio_device_find_channel(cap->dev, i_name, false);
    cap->q_chan = iio_device_find_channel(cap->dev, q_name, false);
    if (!cap->i_chan || !cap->q_chan) {
        fprintf(stderr, "could not find IIO channels: %s/%s\n", i_name, q_name);
        goto fail;
    }

    iio_channel_enable(cap->i_chan);
    iio_channel_enable(cap->q_chan);
    cap->buf = iio_device_create_buffer(cap->dev, (size_t)samples, false);
    if (!cap->buf) {
        fprintf(stderr, "could not create IIO buffer\n");
        goto fail;
    }

    return 0;

fail:
    destroy_capture_context(cap);
    return 1;
}

static int refill_iq(struct capture_context *cap, int16_t *iq)
{
    long copied = 0;

    while (copied < cap->samples) {
        ssize_t refill = iio_buffer_refill(cap->buf);
        char *ptr;
        char *end;
        ptrdiff_t step;

        if (refill < 0) {
            fprintf(stderr, "IIO buffer refill failed: %zd\n", refill);
            return 1;
        }
        ptr = iio_buffer_first(cap->buf, cap->i_chan);
        end = iio_buffer_end(cap->buf);
        step = iio_buffer_step(cap->buf);
        for (; ptr < end && copied < cap->samples; ptr += step) {
            const int16_t *sample = (const int16_t *)ptr;
            iq[copied * 2] = sample[0];
            iq[copied * 2 + 1] = sample[1];
            copied++;
        }
    }

    return 0;
}

static int capture_iq(int16_t *iq, long samples)
{
    struct capture_context cap;
    int ret;

    if (init_capture_context(&cap, samples) != 0)
        return 1;
    ret = refill_iq(&cap, iq);
    destroy_capture_context(&cap);
    return ret;
}

static void compute_points(const int16_t *iq, long samples, long sample_rate,
                           long long center_hz, long span_hz, int bins,
                           struct point *points, double *noise_floor)
{
    double step_hz = bins > 1 ? (double)span_hz / (double)(bins - 1) : 0.0;
    double start_hz = (double)center_hz - ((double)span_hz / 2.0);
    double sum_power = 0.0;
    int b;

    for (b = 0; b < bins; b++) {
        double freq = start_hz + (step_hz * (double)b);
        double offset_hz = freq - (double)center_hz;
        double phase_step = -2.0 * M_PI * offset_hz / (double)sample_rate;
        double phase = 0.0;
        double acc_i = 0.0;
        double acc_q = 0.0;
        long n;

        for (n = 0; n < samples; n++) {
            double i_val = (double)iq[n * 2] / INT16_SCALE;
            double q_val = (double)iq[n * 2 + 1] / INT16_SCALE;
            double c = cos(phase);
            double s = sin(phase);

            acc_i += i_val * c - q_val * s;
            acc_q += i_val * s + q_val * c;
            phase += phase_step;
            if (phase > M_PI || phase < -M_PI)
                phase = fmod(phase, 2.0 * M_PI);
        }

        points[b].frequency_hz = (long long)llround(freq);
        points[b].power_dbfs = dbfs((acc_i * acc_i + acc_q * acc_q) / ((double)samples * (double)samples));
        sum_power += points[b].power_dbfs;
    }

    *noise_floor = bins > 0 ? sum_power / (double)bins : -120.0;
}

static void print_spectrum_json(long sequence, int stream_mode, long samples, long sample_rate,
                                long long center_hz, long span_hz, int bins, int top_n,
                                int follow_doppler, const struct point *points, const struct point *peaks,
                                double noise_floor)
{
    int i;

    if (stream_mode) {
        printf("{\"ok\":true,\"type\":\"spectrum_row\",\"sequence\":%ld,\"time_epoch\":%ld,"
               "\"sample_count\":%ld,\"sample_rate_hz\":%ld,\"center_frequency_hz\":%lld,"
               "\"span_hz\":%ld,\"bins\":%d,\"backend\":\"external_stream\",\"bounded\":false,"
               "\"follow_doppler\":%s,"
               "\"points\":[",
               sequence, (long)time(NULL), samples, sample_rate, center_hz, span_hz, bins,
               follow_doppler ? "true" : "false");
    } else {
        printf("{\"sample_count\":%ld,\"sample_rate_hz\":%ld,\"points\":[", samples, sample_rate);
    }

    for (i = 0; i < bins; i++) {
        printf("%s{\"frequency_hz\":%lld,\"power_dbfs\":%.2f}",
               i ? "," : "", points[i].frequency_hz, points[i].power_dbfs);
    }
    printf("],\"peaks\":[");
    for (i = 0; i < top_n; i++) {
        printf("%s{\"frequency_hz\":%lld,\"power_dbfs\":%.2f,\"snr_db\":%.2f}",
               i ? "," : "", peaks[i].frequency_hz, peaks[i].power_dbfs,
               peaks[i].power_dbfs - noise_floor);
    }
    printf("],\"noise_floor_dbfs\":%.2f}\n", noise_floor);
    fflush(stdout);
}

static void sleep_ms(long interval_ms)
{
    struct timespec req;

    if (interval_ms <= 0)
        return;
    req.tv_sec = interval_ms / 1000;
    req.tv_nsec = (interval_ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
        ;
}

int main(void)
{
    long long center_hz = env_ll("PLUTO_SPECTRUM_CENTER_HZ", 145800000LL, 70000000LL, 6000000000LL);
    long span_hz = env_long("PLUTO_SPECTRUM_SPAN_HZ", 200000L, 1000L, 56000000L);
    long sample_rate = env_long("PLUTO_SPECTRUM_SAMPLE_RATE_HZ", DEFAULT_SAMPLE_RATE, 520000L, 61440000L);
    long samples = env_long("PLUTO_SPECTRUM_SAMPLES", DEFAULT_BUFFER_SAMPLES, 512L, 65536L);
    int bins = (int)env_long("PLUTO_SPECTRUM_BINS", 256L, 16L, 1024L);
    int top_n = (int)env_long("PLUTO_SPECTRUM_TOP_N", 5L, 1L, 20L);
    int stream_mode = (int)env_long("PLUTO_SPECTRUM_STREAM", 0L, 0L, 1L);
    long frames = env_long("PLUTO_SPECTRUM_FRAMES", stream_mode ? 120L : 1L, 1L, 3600L);
    long interval_ms = env_long("PLUTO_SPECTRUM_INTERVAL_MS", 250L, 0L, 60000L);
    int16_t *iq = NULL;
    struct point *points = NULL;
    struct point *peaks = NULL;
    struct capture_context cap;
    struct doppler_plan doppler;
    long sequence;
    int cap_ready = 0;

    memset(&doppler, 0, sizeof(doppler));

    iq = (int16_t *)calloc((size_t)samples * 2u, sizeof(*iq));
    points = (struct point *)calloc((size_t)bins, sizeof(*points));
    peaks = (struct point *)calloc((size_t)bins, sizeof(*peaks));
    if (!iq || !points || !peaks) {
        fprintf(stderr, "out of memory\n");
        free(iq);
        free(points);
        free(peaks);
        return 1;
    }

    if (top_n > bins)
        top_n = bins;

    if (load_doppler_plan(&doppler) != 0)
        goto fail;

    if (stream_mode) {
        signal(SIGPIPE, SIG_DFL);
        if (init_capture_context(&cap, samples) != 0)
            goto fail;
        cap_ready = 1;
    }

    for (sequence = 0; sequence < frames; sequence++) {
        double noise_floor = -120.0;
        long long frame_center_hz = doppler_center_hz(&doppler, center_hz);
        int ret;

        ret = stream_mode ? refill_iq(&cap, iq) : capture_iq(iq, samples);
        if (ret != 0)
            goto fail;

        compute_points(iq, samples, sample_rate, frame_center_hz, span_hz, bins, points, &noise_floor);
        memcpy(peaks, points, (size_t)bins * sizeof(*points));
        qsort(peaks, (size_t)bins, sizeof(*peaks), point_cmp_desc);
        print_spectrum_json(sequence, stream_mode, samples, sample_rate, frame_center_hz,
                            span_hz, bins, top_n, doppler.count > 0, points, peaks, noise_floor);
        if (!stream_mode)
            break;
        if (sequence + 1 < frames)
            sleep_ms(interval_ms);
    }

    if (cap_ready)
        destroy_capture_context(&cap);
    free(iq);
    free(points);
    free(peaks);
    free_doppler_plan(&doppler);
    return 0;

fail:
    if (cap_ready)
        destroy_capture_context(&cap);
    free(iq);
    free(points);
    free(peaks);
    free_doppler_plan(&doppler);
    return 1;
}
