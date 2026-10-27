// AAC decoding for the movie decoder (dm/host/hle_movie.c) on Android's MediaCodec, in place of AudioToolbox's converter there
#pragma once
#include <stdint.h>
typedef struct maac maac;
maac *maac_new(int rate, int ch, const uint8_t *asc, int asc_n);   // NULL when the device has no AAC decoder or it refuses the configuration (logged)
int maac_decode(maac *m, const uint8_t *p, int n, int16_t *out, int cap_frames);   // one raw AAC frame → interleaved PCM16; the frames written (0 while the decoder primes)
void maac_free(maac *m);
