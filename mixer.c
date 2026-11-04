#include "mixer.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifndef MIXER_NO_AUDIO
#include <portaudio.h>
#endif

#define QUEUE_FRAMES 32768u
#define OUTPUT_RATE 48000u
#define TARGET_BUFFER_MS 30u
#define MAX_TARGET_BUFFER_MS 90u
#define MAX_BUFFER_MS 160u
struct mixer_source {
    struct mixer_source *next;
    unsigned id, rate, volume, remote, target_ms;
    int playing, primed;
    size_t read, count, peak_count;
    uint64_t underruns, dropped_frames;
    double phase;
    float *queue;
};
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static mixer_source *sources;
static unsigned master_volume = 100, master_gain = 100;
static _Atomic uint64_t clipped;
static _Atomic unsigned output_latency_us;
static _Atomic uint64_t output_underruns;
static _Atomic uint64_t output_contentions;

/* Keep independent source clocks, but never preserve an old burst indefinitely. */
static void trim_locked(mixer_source *source, size_t keep) {
    if (source->count <= keep) return;
    size_t drop = source->count - keep;
    source->read = (source->read + drop) % QUEUE_FRAMES;
    source->count = keep;
    source->phase = 0;
    source->dropped_frames += drop;
}

static void recover_single_source_locked(void) {
    mixer_source *single = NULL;
    for (mixer_source *s = sources; s; s = s->next) {
        if (!s->playing) continue;
        if (single) return;
        single = s;
    }
    if (single) {
        single->target_ms = TARGET_BUFFER_MS;
        trim_locked(single, single->rate * TARGET_BUFFER_MS / 1000);
    }
}

mixer_source *mixer_source_create(unsigned id, unsigned rate) {
    if (rate < 8000 || rate > 96000) return NULL;
    mixer_source *source = calloc(1, sizeof(*source));
    if (!source) return NULL;
    source->queue = calloc(QUEUE_FRAMES * 2, sizeof(float));
    if (!source->queue) { free(source); return NULL; }
    source->id = id; source->rate = rate; source->volume = 100; source->remote = 127;
    source->target_ms = TARGET_BUFFER_MS;
    pthread_mutex_lock(&mutex); source->next = sources; sources = source; pthread_mutex_unlock(&mutex);
    return source;
}

void mixer_source_destroy(mixer_source *source) {
    if (!source) return;
    pthread_mutex_lock(&mutex);
    mixer_source **entry = &sources;
    while (*entry && *entry != source) entry = &(*entry)->next;
    if (*entry) *entry = source->next;
    if (source->playing) recover_single_source_locked();
    pthread_mutex_unlock(&mutex);
    free(source->queue); free(source);
}

void mixer_source_rate(mixer_source *source, unsigned rate) {
    if (!source || rate < 8000 || rate > 96000) return;
    pthread_mutex_lock(&mutex);
    source->rate = rate; source->read = source->count = 0; source->phase = 0; source->primed = 0;
    source->target_ms = TARGET_BUFFER_MS;
    pthread_mutex_unlock(&mutex);
}

void mixer_source_volume(mixer_source *source, unsigned percent, unsigned remote) {
    if (!source) return;
    pthread_mutex_lock(&mutex);
    source->volume = percent > 100 ? 100 : percent;
    source->remote = remote > 127 ? 127 : remote;
    pthread_mutex_unlock(&mutex);
}

void mixer_source_playing(mixer_source *source, int playing) {
    if (!source) return;
    pthread_mutex_lock(&mutex);
    int was_playing = source->playing;
    source->playing = playing;
    if (!playing) {
        source->read = source->count = 0; source->phase = 0; source->primed = 0;
        source->target_ms = TARGET_BUFFER_MS;
    }
    if (was_playing && !playing) recover_single_source_locked();
    pthread_mutex_unlock(&mutex);
}

void mixer_source_push(mixer_source *source, const int16_t *samples, size_t frames, unsigned channels) {
    if (!source || !samples || channels < 1 || channels > 2 || !frames) return;
    if (frames > QUEUE_FRAMES) { samples += (frames - QUEUE_FRAMES) * channels; frames = QUEUE_FRAMES; }
    pthread_mutex_lock(&mutex);
    if (source->count + frames > QUEUE_FRAMES) {
        size_t drop = source->count + frames - QUEUE_FRAMES;
        source->read = (source->read + drop) % QUEUE_FRAMES;
        source->count -= drop; source->phase = 0;
        source->dropped_frames += drop;
    }
    size_t write = (source->read + source->count) % QUEUE_FRAMES;
    for (size_t i = 0; i < frames; i++) {
        source->queue[2 * write] = samples[i * channels];
        source->queue[2 * write + 1] = samples[i * channels + (channels == 2 ? 1 : 0)];
        write = (write + 1) % QUEUE_FRAMES;
    }
    source->count += frames;
    if (source->count > source->peak_count) source->peak_count = source->count;
    if (source->count > source->rate * MAX_BUFFER_MS / 1000)
        trim_locked(source, source->rate * source->target_ms / 1000);
    pthread_mutex_unlock(&mutex);
}

void mixer_source_get_statistics(mixer_source *source, mixer_source_statistics *statistics) {
    memset(statistics, 0, sizeof(*statistics));
    if (!source) return;
    pthread_mutex_lock(&mutex);
    statistics->queued_ms = source->count * 1000.0 / source->rate;
    statistics->peak_queued_ms = source->peak_count * 1000.0 / source->rate;
    statistics->underruns = source->underruns;
    statistics->dropped_frames = source->dropped_frames;
    statistics->target_ms = source->target_ms;
    pthread_mutex_unlock(&mutex);
}

void mixer_flush(void) {
    pthread_mutex_lock(&mutex);
    for (mixer_source *s = sources; s; s = s->next) {
        trim_locked(s, 0);
        s->read = 0; s->phase = 0; s->primed = 0;
        s->target_ms = TARGET_BUFFER_MS;
    }
    pthread_mutex_unlock(&mutex);
}

void mixer_set_master(unsigned volume, unsigned gain) {
    pthread_mutex_lock(&mutex);
    master_volume = volume > 100 ? 100 : volume;
    master_gain = gain < 100 ? 100 : gain > 400 ? 400 : gain;
    pthread_mutex_unlock(&mutex);
}

void mixer_render(int16_t *output, size_t frames) {
    memset(output, 0, frames * 2 * sizeof(*output));
    /* Never wait behind producer work on the real-time output callback. */
    if (pthread_mutex_trylock(&mutex)) {
        atomic_fetch_add_explicit(&output_contentions, 1, memory_order_relaxed);
        return;
    }
    double total_gain = master_volume * master_gain / 10000.0;
    uint64_t limited = 0;
    for (size_t i = 0; i < frames; i++) {
        double left = 0, right = 0;
        for (mixer_source *s = sources; s; s = s->next) {
            if (!s->playing) continue;
            size_t target = s->rate * s->target_ms / 1000;
            if (!s->primed) {
                if (s->count < target) continue;
                s->primed = 1;
            }
            if (s->count < 2) {
                s->underruns++; s->primed = 0; s->phase = 0;
                /* Add headroom only after this source actually starves. */
                if (s->target_ms < MAX_TARGET_BUFFER_MS) {
                    s->target_ms += 15;
                    if (s->target_ms > MAX_TARGET_BUFFER_MS) s->target_ms = MAX_TARGET_BUFFER_MS;
                }
                continue;
            }
            size_t next = (s->read + 1) % QUEUE_FRAMES;
            double volume = (s->volume / 100.0) * (s->remote / 127.0);
            left += (s->queue[2*s->read] * (1-s->phase) + s->queue[2*next] * s->phase) * volume;
            right += (s->queue[2*s->read+1] * (1-s->phase) + s->queue[2*next+1] * s->phase) * volume;
            /* Gently follow each independent source clock using queue depth. */
            double error = ((double)s->count - target) / (double)target;
            double drift = fmax(-0.004, fmin(0.004, error * 0.001));
            s->phase += (s->rate / (double)OUTPUT_RATE) * (1 + drift);
            size_t advance = (size_t)s->phase;
            if (advance >= s->count) advance = s->count - 1;
            s->read = (s->read + advance) % QUEUE_FRAMES;
            s->count -= advance; s->phase -= advance;
        }
        double values[2] = {left * total_gain, right * total_gain};
        for (unsigned c = 0; c < 2; c++) {
            if (values[c] > 32767) { values[c] = 32767; limited++; }
            if (values[c] < -32768) { values[c] = -32768; limited++; }
            output[2*i+c] = (int16_t)lrint(values[c]);
        }
    }
    pthread_mutex_unlock(&mutex);
    atomic_fetch_add_explicit(&clipped, limited, memory_order_relaxed);
}

uint64_t mixer_clipped_samples(void) { return atomic_load_explicit(&clipped, memory_order_relaxed); }
double mixer_audio_latency_ms(void) { return atomic_load_explicit(&output_latency_us, memory_order_relaxed) / 1000.0; }
uint64_t mixer_audio_underruns(void) { return atomic_load_explicit(&output_underruns, memory_order_relaxed); }
uint64_t mixer_audio_contentions(void) { return atomic_load_explicit(&output_contentions, memory_order_relaxed); }

#ifndef MIXER_NO_AUDIO
static PaStream *stream;
static int pa_initialized;
static int output_callback(const void *input, void *output, unsigned long frames,
    const PaStreamCallbackTimeInfo *timing, PaStreamCallbackFlags flags, void *context) {
    (void)input; (void)context;
    double remaining = timing->outputBufferDacTime - timing->currentTime;
    if (remaining >= 0 && remaining < 2)
        atomic_store_explicit(&output_latency_us, (unsigned)llround(remaining * 1000000), memory_order_relaxed);
    if (flags & paOutputUnderflow)
        atomic_fetch_add_explicit(&output_underruns, 1, memory_order_relaxed);
    mixer_render(output, frames); return paContinue;
}

int mixer_audio_open(const char *speaker_name) {
    PaError error = Pa_Initialize();
    if (error != paNoError) return error;
    pa_initialized = 1;
    PaDeviceIndex device = paNoDevice;
    for (PaDeviceIndex i = 0; i < Pa_GetDeviceCount(); i++) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        if (info && info->maxOutputChannels >= 2 && strcmp(info->name, speaker_name) == 0) { device = i; break; }
    }
    if (device == paNoDevice) { mixer_audio_close(); return paInvalidDevice; }
    const PaDeviceInfo *info = Pa_GetDeviceInfo(device);
    PaStreamParameters params = {.device=device, .channelCount=2, .sampleFormat=paInt16,
        .suggestedLatency=info->defaultLowOutputLatency};
    error = Pa_OpenStream(&stream, NULL, &params, OUTPUT_RATE, 480, paClipOff, output_callback, NULL);
    if (error == paNoError) {
        const PaStreamInfo *stream_info = Pa_GetStreamInfo(stream);
        if (stream_info) atomic_store(&output_latency_us, (unsigned)llround(stream_info->outputLatency * 1000000));
        atomic_store(&output_underruns, 0);
        error = Pa_StartStream(stream);
    }
    if (error != paNoError) mixer_audio_close();
    return error;
}

void mixer_audio_close(void) {
    if (stream) { Pa_StopStream(stream); Pa_CloseStream(stream); stream = NULL; }
    if (pa_initialized) { Pa_Terminate(); pa_initialized = 0; }
    atomic_store(&output_latency_us, 0);
}
#else
int mixer_audio_open(const char *speaker_name) { (void)speaker_name; return 0; }
void mixer_audio_close(void) {}
#endif
