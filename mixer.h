#ifndef MIXER_H
#define MIXER_H
#include <stddef.h>
#include <stdint.h>
typedef struct mixer_source mixer_source;
typedef struct {
    double queued_ms, peak_queued_ms;
    unsigned target_ms;
    uint64_t underruns, dropped_frames;
} mixer_source_statistics;
mixer_source *mixer_source_create(unsigned id, unsigned rate);
void mixer_source_destroy(mixer_source *source);
void mixer_source_rate(mixer_source *source, unsigned rate);
void mixer_source_volume(mixer_source *source, unsigned percent, unsigned remote);
void mixer_source_playing(mixer_source *source, int playing);
void mixer_source_push(mixer_source *source, const int16_t *samples, size_t frames, unsigned channels);
void mixer_source_get_statistics(mixer_source *source, mixer_source_statistics *statistics);
void mixer_flush(void);
void mixer_set_master(unsigned volume, unsigned gain);
void mixer_render(int16_t *output, size_t frames);
int mixer_audio_open(const char *speaker_name);
void mixer_audio_close(void);
uint64_t mixer_clipped_samples(void);
double mixer_audio_latency_ms(void);
uint64_t mixer_audio_underruns(void);
uint64_t mixer_audio_contentions(void);
#endif
