#define _POSIX_C_SOURCE 200809L

/*
 * Live FT8 adapter for the Pluto audio backend.
 *
 * The candidate search and message-unpack flow follows kgoba/ft8_lib's
 * decode_ft8 example. ft8_lib is MIT licensed and pinned by Buildroot.
 */

#include "pluto-ft8-decoder.h"

#include <common/monitor.h>
#include <ft8/decode.h>
#include <ft8/message.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FT8_MIN_SCORE 10
#define FT8_MAX_CANDIDATES 140
#define FT8_LDPC_ITERATIONS 25
#define FT8_MAX_MESSAGES 50
#define FT8_HASH_SIZE 256
#define FT8_TIME_OSR 2
#define FT8_FREQ_OSR 2

struct ft8_result {
    float time_offset_s;
    float audio_frequency_hz;
    int sync_score;
    char text[FTX_MAX_MESSAGE_LENGTH];
};

struct callsign_hash_entry {
    char callsign[12];
    uint32_t hash;
};

struct pluto_ft8_decoder {
    monitor_t monitors[2];
    int capture_index;
    int ready_index;
    long capture_slot;
    long ready_slot;
    bool capture_partial;
    float *frame;
    size_t frame_used;

    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    bool stop;
    bool pending;
    bool decoding;

    unsigned slots_completed;
    unsigned slots_dropped;
    long last_slot;
    double last_decode_ms;
    int candidate_count;
    int message_count;
    struct ft8_result messages[FT8_MAX_MESSAGES];
};

static struct callsign_hash_entry callsign_hashes[FT8_HASH_SIZE];

static void callsign_hash_reset(void)
{
    memset(callsign_hashes, 0, sizeof(callsign_hashes));
}

static void callsign_hash_save(const char *callsign, uint32_t hash)
{
    int index = (int)(((hash >> 12) & 0x3ffU) * 23U) % FT8_HASH_SIZE;
    int probes;

    for (probes = 0; probes < FT8_HASH_SIZE; probes++) {
        struct callsign_hash_entry *entry = &callsign_hashes[index];
        if (!entry->callsign[0] ||
            ((entry->hash & 0x3fffffU) == hash && !strcmp(entry->callsign, callsign))) {
            strncpy(entry->callsign, callsign, sizeof(entry->callsign) - 1);
            entry->callsign[sizeof(entry->callsign) - 1] = '\0';
            entry->hash = hash & 0x3fffffU;
            return;
        }
        index = (index + 1) % FT8_HASH_SIZE;
    }
}

static bool callsign_hash_lookup(ftx_callsign_hash_type_t type, uint32_t hash,
                                 char *callsign)
{
    uint8_t shift = type == FTX_CALLSIGN_HASH_10_BITS ? 12 :
                    (type == FTX_CALLSIGN_HASH_12_BITS ? 10 : 0);
    uint16_t hash10 = (hash >> (12 - shift)) & 0x3ffU;
    int index = (hash10 * 23) % FT8_HASH_SIZE;
    int probes;

    for (probes = 0; probes < FT8_HASH_SIZE; probes++) {
        struct callsign_hash_entry *entry = &callsign_hashes[index];
        if (!entry->callsign[0])
            break;
        if (((entry->hash & 0x3fffffU) >> shift) == hash) {
            strcpy(callsign, entry->callsign);
            return true;
        }
        index = (index + 1) % FT8_HASH_SIZE;
    }
    callsign[0] = '\0';
    return false;
}

static ftx_callsign_hash_interface_t callsign_hash_interface = {
    .lookup_hash = callsign_hash_lookup,
    .save_hash = callsign_hash_save,
};

static double monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static bool result_is_duplicate(const ftx_message_t *message,
                                const ftx_message_t *decoded, int decoded_count)
{
    int i;
    for (i = 0; i < decoded_count; i++) {
        if (decoded[i].hash == message->hash &&
            !memcmp(decoded[i].payload, message->payload, sizeof(message->payload)))
            return true;
    }
    return false;
}

static void decode_waterfall(struct pluto_ft8_decoder *decoder, monitor_t *monitor,
                             long slot)
{
    ftx_candidate_t candidates[FT8_MAX_CANDIDATES];
    ftx_message_t decoded[FT8_MAX_MESSAGES];
    struct ft8_result results[FT8_MAX_MESSAGES];
    int candidate_count;
    int decoded_count = 0;
    int i;
    double started = monotonic_ms();

    candidate_count = ftx_find_candidates(&monitor->wf, FT8_MAX_CANDIDATES,
                                          candidates, FT8_MIN_SCORE);
    for (i = 0; i < candidate_count && decoded_count < FT8_MAX_MESSAGES; i++) {
        const ftx_candidate_t *candidate = &candidates[i];
        ftx_message_t message;
        ftx_decode_status_t status;
        ftx_message_offsets_t offsets;
        struct ft8_result *result;

        if (!ftx_decode_candidate(&monitor->wf, candidate, FT8_LDPC_ITERATIONS,
                                  &message, &status))
            continue;
        if (result_is_duplicate(&message, decoded, decoded_count))
            continue;

        decoded[decoded_count] = message;
        result = &results[decoded_count];
        result->time_offset_s =
            (candidate->time_offset +
             (float)candidate->time_sub / monitor->wf.time_osr) *
            monitor->symbol_period;
        result->audio_frequency_hz =
            (monitor->min_bin + candidate->freq_offset +
             (float)candidate->freq_sub / monitor->wf.freq_osr) /
            monitor->symbol_period;
        result->sync_score = candidate->score;
        if (ftx_message_decode(&message, &callsign_hash_interface, result->text,
                               &offsets) != FTX_MESSAGE_RC_OK)
            strcpy(result->text, "<unpack error>");
        decoded_count++;
    }

    pthread_mutex_lock(&decoder->lock);
    decoder->candidate_count = candidate_count;
    decoder->message_count = decoded_count;
    memcpy(decoder->messages, results, (size_t)decoded_count * sizeof(results[0]));
    decoder->last_slot = slot;
    decoder->last_decode_ms = monotonic_ms() - started;
    decoder->slots_completed++;
    decoder->decoding = false;
    pthread_mutex_unlock(&decoder->lock);
}

static void *decoder_worker(void *opaque)
{
    struct pluto_ft8_decoder *decoder = opaque;

    for (;;) {
        int index;
        long slot;

        pthread_mutex_lock(&decoder->lock);
        while (!decoder->pending && !decoder->stop)
            pthread_cond_wait(&decoder->wake, &decoder->lock);
        if (decoder->stop) {
            pthread_mutex_unlock(&decoder->lock);
            break;
        }
        index = decoder->ready_index;
        slot = decoder->ready_slot;
        decoder->pending = false;
        decoder->decoding = true;
        pthread_mutex_unlock(&decoder->lock);

        decode_waterfall(decoder, &decoder->monitors[index], slot);
    }
    return NULL;
}

struct pluto_ft8_decoder *pluto_ft8_decoder_create(unsigned sample_rate)
{
    struct pluto_ft8_decoder *decoder;
    monitor_config_t config = {
        .f_min = 200,
        .f_max = 3000,
        .sample_rate = (int)sample_rate,
        .time_osr = FT8_TIME_OSR,
        .freq_osr = FT8_FREQ_OSR,
        .protocol = FTX_PROTOCOL_FT8,
    };

    if (sample_rate != 12000)
        return NULL;
    decoder = calloc(1, sizeof(*decoder));
    if (!decoder)
        return NULL;

    monitor_init(&decoder->monitors[0], &config);
    monitor_init(&decoder->monitors[1], &config);
    decoder->frame = calloc((size_t)decoder->monitors[0].block_size, sizeof(float));
    if (!decoder->frame) {
        monitor_free(&decoder->monitors[0]);
        monitor_free(&decoder->monitors[1]);
        free(decoder);
        return NULL;
    }
    decoder->capture_slot = -1;
    decoder->last_slot = -1;
    decoder->capture_partial = true;
    pthread_mutex_init(&decoder->lock, NULL);
    pthread_cond_init(&decoder->wake, NULL);
    callsign_hash_reset();
    if (pthread_create(&decoder->worker, NULL, decoder_worker, decoder) != 0) {
        pthread_cond_destroy(&decoder->wake);
        pthread_mutex_destroy(&decoder->lock);
        monitor_free(&decoder->monitors[0]);
        monitor_free(&decoder->monitors[1]);
        free(decoder->frame);
        free(decoder);
        return NULL;
    }
    return decoder;
}

void pluto_ft8_decoder_destroy(struct pluto_ft8_decoder *decoder)
{
    if (!decoder)
        return;
    pthread_mutex_lock(&decoder->lock);
    decoder->stop = true;
    pthread_cond_signal(&decoder->wake);
    pthread_mutex_unlock(&decoder->lock);
    pthread_join(decoder->worker, NULL);
    pthread_cond_destroy(&decoder->wake);
    pthread_mutex_destroy(&decoder->lock);
    monitor_free(&decoder->monitors[0]);
    monitor_free(&decoder->monitors[1]);
    free(decoder->frame);
    free(decoder);
}

static void submit_slot(struct pluto_ft8_decoder *decoder)
{
    pthread_mutex_lock(&decoder->lock);
    if (!decoder->pending && !decoder->decoding) {
        decoder->ready_index = decoder->capture_index;
        decoder->ready_slot = decoder->capture_slot;
        decoder->capture_index = 1 - decoder->capture_index;
        monitor_reset(&decoder->monitors[decoder->capture_index]);
        decoder->pending = true;
        pthread_cond_signal(&decoder->wake);
    } else {
        decoder->slots_dropped++;
        monitor_reset(&decoder->monitors[decoder->capture_index]);
    }
    pthread_mutex_unlock(&decoder->lock);
}

void pluto_ft8_decoder_add_audio(struct pluto_ft8_decoder *decoder, float sample)
{
    monitor_t *monitor;

    if (!decoder)
        return;
    monitor = &decoder->monitors[decoder->capture_index];
    decoder->frame[decoder->frame_used++] = sample;
    if (decoder->frame_used < (size_t)monitor->block_size)
        return;

    {
        struct timespec now;
        long slot;
        clock_gettime(CLOCK_REALTIME, &now);
        slot = (long)(now.tv_sec / 15);
        if (decoder->capture_slot < 0)
            decoder->capture_slot = slot;
        else if (slot != decoder->capture_slot) {
            if (!decoder->capture_partial)
                submit_slot(decoder);
            else
                monitor_reset(&decoder->monitors[decoder->capture_index]);
            decoder->capture_partial = false;
            decoder->capture_slot = slot;
            monitor = &decoder->monitors[decoder->capture_index];
        }
    }

    monitor_process(monitor, decoder->frame);
    decoder->frame_used = 0;
}

static void json_string(FILE *out, const char *value)
{
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    fputc('"', out);
    while (*p) {
        switch (*p) {
        case '"': fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (*p < 0x20)
                fprintf(out, "\\u%04x", *p);
            else
                fputc(*p, out);
        }
        p++;
    }
    fputc('"', out);
}

static void slot_utc(long slot, char *buffer, size_t size)
{
    time_t raw = (time_t)(slot * 15);
    struct tm tm;
    if (slot < 0 || !gmtime_r(&raw, &tm)) {
        buffer[0] = '\0';
        return;
    }
    strftime(buffer, size, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

void pluto_ft8_decoder_write_json(struct pluto_ft8_decoder *decoder, FILE *out)
{
    char timestamp[32];
    int i;

    if (!decoder) {
        fputs("null", out);
        return;
    }
    pthread_mutex_lock(&decoder->lock);
    slot_utc(decoder->last_slot, timestamp, sizeof(timestamp));
    fputs("{\n", out);
    fputs("    \"decode_supported\": true,\n", out);
    fputs("    \"protocol\": \"FT8\",\n", out);
    fputs("    \"state\": ", out);
    json_string(out, decoder->decoding ? "decoding" : "collecting");
    fputs(",\n    \"slot_utc\": ", out);
    if (timestamp[0]) json_string(out, timestamp); else fputs("null", out);
    fprintf(out, ",\n    \"slots_completed\": %u,\n", decoder->slots_completed);
    fprintf(out, "    \"slots_dropped\": %u,\n", decoder->slots_dropped);
    fprintf(out, "    \"decode_time_ms\": %.3f,\n", decoder->last_decode_ms);
    fprintf(out, "    \"candidate_count\": %d,\n", decoder->candidate_count);
    fprintf(out, "    \"message_count\": %d,\n", decoder->message_count);
    fputs("    \"messages\": [", out);
    for (i = 0; i < decoder->message_count; i++) {
        const struct ft8_result *result = &decoder->messages[i];
        fputs(i ? ",\n      {" : "\n      {", out);
        fprintf(out, "\"time_offset_s\": %.3f, ", result->time_offset_s);
        fprintf(out, "\"audio_frequency_hz\": %.1f, ", result->audio_frequency_hz);
        fprintf(out, "\"sync_score\": %d, \"text\": ", result->sync_score);
        json_string(out, result->text);
        fputc('}', out);
    }
    fputs(decoder->message_count ? "\n    ]\n" : "]\n", out);
    fputs("  }", out);
    pthread_mutex_unlock(&decoder->lock);
}
