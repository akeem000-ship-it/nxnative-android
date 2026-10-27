// AAudio output with the shape of the CoreAudio AudioUnit calls dm/host/hle_audio.c makes (create with a render callback, start, stop, dispose): hle_audio.c keeps its queues,
// watchdog and silent sink and only swaps the device. One low-latency 48 kHz 16-bit stream per output; the render callback gets an AudioBufferList with one interleaved buffer.
// A disconnected stream (headphones unplugged, Bluetooth switched, another app took exclusive output) is reopened on a helper thread with the same format and started again if it
// was playing, as AAudio requires (no stream calls from its own error callback)
#pragma once
#include <stdint.h>

typedef int32_t OSStatus;
typedef uint32_t UInt32;
typedef uint32_t AudioUnitRenderActionFlags;
typedef struct { double mSampleTime; uint64_t mHostTime; } AudioTimeStamp;
typedef struct { UInt32 mNumberChannels, mDataByteSize; void *mData; } AudioBuffer;
typedef struct { UInt32 mNumberBuffers; AudioBuffer mBuffers[1]; } AudioBufferList;
typedef OSStatus (*AURenderCallback)(void *ctx, AudioUnitRenderActionFlags *f, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *bl);
typedef struct dm_aaudio *AudioUnit;
enum { noErr = 0 };

OSStatus dm_aaudio_create(AudioUnit *out, int rate, int channels, AURenderCallback cb, void *ctx);   // 0, or the AAudio result (negative)
OSStatus AudioOutputUnitStart(AudioUnit au);
OSStatus AudioOutputUnitStop(AudioUnit au);
void dm_aaudio_dispose(AudioUnit au);
