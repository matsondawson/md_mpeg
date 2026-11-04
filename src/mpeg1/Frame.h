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
	uint8_t discard;
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

	Frame();
	~Frame();
	Frame(const Frame&) = delete;
	Frame& operator=(const Frame&) = delete;

	void init(int32_t ptsMillis, FrameSize frameSize);
};

