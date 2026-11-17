#pragma once

#include "MpegProgramStream.h"

class Sequence;
enum VideoPictureType {
	VideoPictureType_INTRA = 1,
	VideoPictureType_PREDICTIVE = 2,
	VideoPictureType_B = 3,
	VideoPictureType_D = 4
};

typedef struct {
	uint8_t run;
	int16_t level;
	uint8_t discard; // code length in bits
} RunLevel;


typedef struct FrameSizeType {
	uint32_t luma_width, luma_height;
	uint32_t chroma_width, chroma_height;

	auto operator<=>(const FrameSizeType&) const = default;
} FrameSize;

class Frame
{
public:
	// Presentation time, -1 if the stream doesn't give one for this picture
	int32_t m_ptsMillis;
	uint8_t* m_yData;
	uint8_t* m_crData;
	uint8_t* m_cbData;

	FrameSize m_frameSize;
	// Fields this frame is displayed for: 2, more with MPEG-2 repeat_first_field
	uint8_t m_fields = 2;

	Frame();
	~Frame();
	Frame(const Frame&) = delete;
	Frame& operator=(const Frame&) = delete;

	void init(int32_t ptsMillis, FrameSize frameSize);
	// For a reference picture that wasn't decoded at this size: decoding started mid-stream, or the size changed.
	// Allocates it at the size, black, so predictions from it read valid memory.
	void ensureSize(FrameSize frameSize);
};

