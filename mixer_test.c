#include "mixer.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static void fill(int16_t *samples,int value){for(unsigned i=0;i<12000;i++)samples[i]=(int16_t)value;}
int main(void) {
    int16_t samples[12000],output[960];
    mixer_source *a=mixer_source_create(1,48000),*b=mixer_source_create(2,44100);
    assert(a&&b);fill(samples,1000);mixer_source_push(a,samples,6000,2);mixer_source_playing(a,1);
    fill(samples,2000);mixer_source_push(b,samples,6000,2);mixer_source_playing(b,1);
    mixer_render(output,480);for(unsigned i=0;i<960;i++)assert(output[i]==3000);
    mixer_source_volume(b,50,127);mixer_render(output,480);assert(output[0]==2000);
    mixer_source_destroy(b);mixer_set_master(100,200);mixer_render(output,480);assert(output[0]==2000);
    mixer_set_master(0,200);mixer_render(output,480);for(unsigned i=0;i<960;i++)assert(output[i]==0);
    mixer_source_playing(a,0);mixer_source_rate(a,48000);fill(samples,30000);
    mixer_source_push(a,samples,6000,2);mixer_source_playing(a,1);mixer_set_master(100,400);
    mixer_render(output,480);assert(output[0]==32767&&mixer_clipped_samples()>0);
    mixer_source_destroy(a);mixer_render(output,480);for(unsigned i=0;i<960;i++)assert(output[i]==0);
    /* Confirm two distinct tones survive a real mixed output, not a source switch. */
    a=mixer_source_create(3,48000);b=mixer_source_create(4,44100);mixer_set_master(100,100);
    for(unsigned i=0;i<1440;i++)samples[2*i]=samples[2*i+1]=(int16_t)(4000*sin(2*3.141592653589793*440*i/48000));
    mixer_source_push(a,samples,1440,2);
    for(unsigned i=0;i<1323;i++)samples[2*i]=samples[2*i+1]=(int16_t)(4000*sin(2*3.141592653589793*880*i/44100));
    mixer_source_push(b,samples,1323,2);mixer_source_playing(a,1);mixer_source_playing(b,1);
    int16_t result[4800*2];
    for(unsigned block=0;block<10;block++) {
        mixer_render(result+block*960,480);
        for(unsigned i=0;i<480;i++)samples[2*i]=samples[2*i+1]=(int16_t)(4000*sin(2*3.141592653589793*440*(1440+block*480+i)/48000));
        mixer_source_push(a,samples,480,2);
        for(unsigned i=0;i<441;i++)samples[2*i]=samples[2*i+1]=(int16_t)(4000*sin(2*3.141592653589793*880*(1323+block*441+i)/44100));
        mixer_source_push(b,samples,441,2);
    }
    double real_a=0,imag_a=0,real_b=0,imag_b=0;
    for(unsigned i=0;i<4800;i++) {
        double pa=2*3.141592653589793*440*i/48000,pb=2*3.141592653589793*880*i/48000;
        real_a+=result[2*i]*cos(pa);imag_a+=result[2*i]*sin(pa);
        real_b+=result[2*i]*cos(pb);imag_b+=result[2*i]*sin(pb);
    }
    assert(hypot(real_a,imag_a)/2400>3000);assert(hypot(real_b,imag_b)/2400>3000);
    mixer_source_destroy(a);mixer_source_destroy(b);
    /* An old burst must not become a slowly draining half-second delay. */
    a=mixer_source_create(5,48000);b=mixer_source_create(6,44100);
    fill(samples,1000);mixer_source_push(a,samples,6000,2);
    fill(samples,5000);mixer_source_push(a,samples,6000,2);
    mixer_source_statistics stats;mixer_source_get_statistics(a,&stats);
    assert(stats.queued_ms<=160 && stats.dropped_frames>0);
    mixer_source_playing(a,1);mixer_source_playing(b,1);
    mixer_render(output,480);assert(output[0]==5000);
    /* Stop one source: immediately trim the survivor to the live target. */
    mixer_flush();fill(samples,5000);mixer_source_push(a,samples,3120,2);
    mixer_source_get_statistics(a,&stats);assert(stats.queued_ms>30);
    mixer_source_playing(b,0);mixer_source_get_statistics(a,&stats);assert(stats.queued_ms<=30);
    mixer_render(output,480);assert(output[0]==5000);
    /* Disconnecting a playing source has the same recovery behavior. */
    mixer_source_playing(b,1);mixer_source_push(a,samples,2400,2);
    mixer_source_destroy(b);mixer_source_get_statistics(a,&stats);assert(stats.queued_ms<=30);
    /* A manual refresh preserves the source and live controls, but no old PCM. */
    mixer_source_volume(a,50,127);mixer_set_master(100,200);mixer_flush();
    mixer_source_get_statistics(a,&stats);assert(stats.queued_ms==0);
    mixer_render(output,480);for(unsigned i=0;i<960;i++)assert(output[i]==0);
    fill(samples,2000);mixer_source_push(a,samples,1440,2);
    mixer_render(output,480);assert(output[0]==2000);
    mixer_source_destroy(a);
    /* 100 ms delivery bursts used to be cut to 30 ms, starving every cycle.
     * Adapt only the affected source, without unbounded latency growth. */
    a=mixer_source_create(7,48000);b=mixer_source_create(8,44100);
    mixer_set_master(100,100);mixer_source_playing(a,1);mixer_source_playing(b,1);
    int16_t burst_output[9600];fill(samples,1000);
    for(unsigned i=0;i<30;i++) {
        mixer_source_push(a,samples,4800,2);mixer_render(burst_output,4800);
    }
    mixer_source_get_statistics(a,&stats);
    assert(stats.underruns<=3 && stats.dropped_frames==0);
    assert(stats.target_ms>30 && stats.target_ms<=90);
    for(unsigned i=0;i<9600;i++)assert(burst_output[i]==1000);
    mixer_source_push(a,samples,3000,2);
    mixer_source_playing(b,0);mixer_source_get_statistics(a,&stats);
    assert(stats.target_ms==30 && stats.queued_ms<=30);
    mixer_flush();mixer_source_get_statistics(a,&stats);
    assert(stats.queued_ms==0 && stats.target_ms==30);
    mixer_source_destroy(a);mixer_source_destroy(b);
    puts("PASS: simultaneous 48k/44.1k mixing, two-tone spectrum, per-source volume, stream removal, live gain, mute, clipping and silence");
    puts("PASS: stale burst bounded, single-source recovery on pause/disconnect, manual refresh removes old audio and preserves controls");
    puts("PASS: burst delivery adapts without repeated starvation or trimming; pause/refresh restores low-latency target");
    return 0;
}
