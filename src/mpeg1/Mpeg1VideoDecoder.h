#pragma once

#include "../common.h"
#include "../stream/MStream.h"
#include "../stream/RingBufferWithStartCodeEvent.h"
#include "../stream/ReadBitStream.h"
#include "Frame.h"

#include <functional>

static constexpr uint32_t PICTURE_START_CODE = 0x100;
static constexpr uint32_t SLICE_START_CODE_START = 0x101;
static constexpr uint32_t SLICE_START_CODE_END = 0x1AF;
static constexpr uint32_t USER_START_CODE = 0x1B2;
static constexpr uint32_t SEQUENCE_HEADER_CODE = 0x1B3;
static constexpr uint32_t SEQUENCE_ERROR_CODE = 0x1B4;
static constexpr uint32_t EXTENSION_START_CODE = 0x1B5;
static constexpr uint32_t SEQUENCE_END_CODE = 0x1B7;
static constexpr uint32_t GROUP_START_CODE = 0x1B8;
static constexpr uint32_t SYSTEM_START_CODE_START = 0x1B9;
static constexpr uint32_t SYSTEM_START_CODE_END = 0x1FF;


static const uint8_t VIDEO_ZIG_ZAG[] = {
	 0,  1,  8, 16,  9,  2,  3, 10,
	17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63
};

class Mpeg1VideoDecoder final : public MStream::IWriteByteStream
{
	std::function<void(const Frame &frame)> m_frameCallback;

public:
	MStream::RingBufferWithStartCodeEvent<256 * 1024, PICTURE_START_CODE> m_stream;
private:
	MStream::ReadBitStream m_bitStream;

	uint32_t next_start_code;
	uint32_t last_start_code;
	uint32_t m_decodeState;
	bool m_hasSequence = false;
	// After a reset, discard data until a group of pictures or sequence header (an I-frame follows)
	bool m_waitForKeyframe = false;
	// After a reset, B-frames before the first anchor frame reference pictures from before the seek
	bool m_dropLeadingBFrames = false;

public:
	// sequence
	uint32_t m_width, m_height;
	uint32_t aspect_ratio_enum;
	float aspect_ratio;
	uint32_t frame_rate_enum;
	float m_frame_rate;
	float m_frame_period_millis;
	uint32_t bit_rate;

	FrameSize m_frameSize;

	// Temporal reference of the next picture to display
	uint32_t nextFrameIndex = 0;
	// Temporal reference of the anchor frame held back for display, -1 if none
	uint32_t forwardFrameIndex = -1;

	// private:
	uint32_t vbv_buffer_size;
	bool load_intra_matrix, load_non_intra_matrix;
	uint8_t intra_quant_matrix[64]{ 0 };
	uint8_t non_intra_quant_matrix[64]{ 0 };

	uint32_t m_macroblockWidth, m_macroblockHeight, mb_size;

private:
	bool full_pel_forward_vector, full_pel_backward_vector;
	int8_t forward_r_size, backward_r_size;

	// 5 bits unsigned, but will be multiplied by signed
	int8_t quantizer_scale;
	int32_t motion_backward_h, motion_forward_h;
	int32_t motion_backward_v, motion_forward_v;

	bool macroblock_motion_forward;
	bool macroblock_motion_backward;
	bool macroblock_pattern;
	bool macroblock_intra;

	// slice processing
	uint32_t mb_row, mb_col;
	// Coefficients are 18 bits: 12-bit level, 6-bit premultiplier
	int32_t block_data[64]{ 0 };

public:
	int8_t picture_coding_type;

	uint32_t macroblock_address;
	int32_t dc_predictor[3];

public:
	// Reference for forward motion vectors
	Frame* m_ptrFramePast;
	// Being decoded
	Frame* m_ptrFrameCurrent;
	// Reference for backward motion vectors
	Frame* m_ptrFrameFuture;
	Frame* m_ptrRenderFrame;

	int8_t last_picture_coding_type;

public:
	Mpeg1VideoDecoder();

public:
	// IWriteByteStream
	int32_t getPts(int32_t frameTimeMillis) override;
	void setPts(int32_t ptsMillis) override;
	void writeByte(uint8_t data) override;
	[[nodiscard]] bool isFull() override;
	void executeCallbacks() override;
	void flush() override;

public:
	bool nextStartCode(uint32_t expected_start_code);
	bool nextStartCodeRange(uint32_t start_code, uint32_t endCode);
	[[nodiscard]] uint32_t lastStartCode() const;

	void setFrameCallback(const std::function<void(const Frame &frame)> &frameCallback);

	void decodeSequence();
	// Skips any extension data, then any user data, each up to the next start code
	void skipExtensionAndUserData();

	void generateRgbFrameData(uint32_t *rgbData, const Frame &frame) const;

	// Frame
	// Decodes one picture, returns its temporal reference
	uint32_t decodeFrame();
	// Advances to and decodes the next picture, returns true if one was decoded or the stream ended
	bool decodeFrame2();
	// Decodes, recovering from corrupt data by skipping to the next picture
	void decodeOrResync();
	void resync();
	// Discards all buffered data and decoding state, e.g. after seeking. Waits for the next I-frame.
	void reset();

	// slice: low byte of the slice start code, the 1-based macroblock row
	void processSlice(uint32_t slice);
	// slice_begin: first macroblock of the slice, whose address increment is relative to the row start
	void processMacroblock(bool slice_begin);
	// i: coded block pattern, bit 5 is block 0, bit 0 is block 5
	void decodeBlock(uint32_t i);
	uint32_t decodeDCTDCSizeLuminance();
	int decodeDCTDCSizeChrominance();
	uint32_t pullMacroblockAddressIncrement();
	RunLevel decodeDCTCoeff(bool first);
	uint32_t decodeCodedBlockPattern();
	void decodeMacroblockType();
	// Reads a motion code and returns motion with it applied, wrapped to the range r_size allows
	int32_t updateMotionVector(int32_t r_size, int32_t motion);
	void decodeMotionVectors();
	void predictMacroblock() const;
	// Predicts the current macroblock from frame d. motion_h and motion_v are luma half pels.
	void videoCopyMacroblock(int motion_h, int motion_v, const Frame* d) const;
	// Same, averaging with what is already in the current macroblock
	void videoInterpolateMacroblock(int motion_h, int motion_v, const Frame* d) const;
	// One plane of the above: d is the destination plane, s the source plane, block_size 16 for luma, 8 for chroma
	void videoProcessMacroblock(uint8_t* d, const uint8_t* s, int motion_h, int motion_v, uint32_t block_size) const;
	void videoProcessMacroblockInterpolate(uint8_t* d, const uint8_t* s, int motion_h, int motion_v, uint32_t block_size) const;
};
