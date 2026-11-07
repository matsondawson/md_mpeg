#pragma once

#include <cstdint>

#include "../stream/RingBufferTyped.h"

// Selects the MP2 audio decoder's arithmetic:
//   0: floating point, outputs 32-bit float samples
//   1: fixed point (default), no floats and no integer wider than 32 bits, outputs 16-bit signed samples
// Set it with the CMake option of the same name, it must be the same for everything that uses the decoder.
#ifndef MD_MPEG_AUDIO_FIXED_POINT
#define MD_MPEG_AUDIO_FIXED_POINT 1
#endif

#if MD_MPEG_AUDIO_FIXED_POINT
using AudioSample = int16_t;
#else
using AudioSample = float;
#endif

// Samples of decoded audio buffered. Must hold more than one frame, override for small targets
#ifndef MD_MPEG_SOUND_RING_SIZE
#define MD_MPEG_SOUND_RING_SIZE 32768
#endif

using AudioSampleBuffer = MStream::RingBufferTyped<AudioSample, MD_MPEG_SOUND_RING_SIZE>;
