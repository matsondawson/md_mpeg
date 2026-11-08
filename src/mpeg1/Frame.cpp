// Portions adapted from PL_MPEG (MIT, Copyright (c) 2019 Dominic Szablewski), see THIRD_PARTY_NOTICES.md

#include "Mpeg1VideoDecoder.h"
#include "../mpeg1/Frame.h"
#include "PixelWords.h"

#include <cstring>

#define PULL2(_v1, _b1, _v2, _b2) \
	{ \
	uint32_t t = m_bitStream.pull(_b1 + _b2); \
	_v1 = t >> _b2; \
	_v2 = t & ((1<<_b2)-1); \
	}

#define PULL2dump1(_v1, _b1, _v2, _b2, _b3) \
	{ \
	uint32_t t = m_bitStream.pull(_b1 + _b2 + _b3); \
	_v1 = t >> (_b2 + _b3); \
	_v2 = (t >> _b3) & ((1<<_b2)-1); \
	}

const static uint16_t us_motionVector[] = {
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x100b, 0xf00b, 0x0f0b, 0xf10b, 0x0e0b, 0xf20b, 0x0d0b, 0xf30b,
	0x0c0b, 0xf40b, 0x0b0b, 0xf50b, 0x0a0a, 0x0a0a, 0xf60a, 0xf60a,
	0x090a, 0x090a, 0xf70a, 0xf70a, 0x080a, 0x080a, 0xf80a, 0xf80a,
	0x0708, 0x0708, 0x0708, 0x0708, 0x0708, 0x0708, 0x0708, 0x0708,
	0xf908, 0xf908, 0xf908, 0xf908, 0xf908, 0xf908, 0xf908, 0xf908,
	0x0608, 0x0608, 0x0608, 0x0608, 0x0608, 0x0608, 0x0608, 0x0608,
	0xfa08, 0xfa08, 0xfa08, 0xfa08, 0xfa08, 0xfa08, 0xfa08, 0xfa08,
	0x0508, 0x0508, 0x0508, 0x0508, 0x0508, 0x0508, 0x0508, 0x0508,
	0xfb08, 0xfb08, 0xfb08, 0xfb08, 0xfb08, 0xfb08, 0xfb08, 0xfb08,
	0x0407, 0x0407, 0x0407, 0x0407, 0x0407, 0x0407, 0x0407, 0x0407,
	0x0407, 0x0407, 0x0407, 0x0407, 0x0407, 0x0407, 0x0407, 0x0407,
	0xfc07, 0xfc07, 0xfc07, 0xfc07, 0xfc07, 0xfc07, 0xfc07, 0xfc07,
	0xfc07, 0xfc07, 0xfc07, 0xfc07, 0xfc07, 0xfc07, 0xfc07, 0xfc07,
};

const static int16_t *s_motionVector = (int16_t *) us_motionVector;

const static uint16_t us_motionVector1[] =
{
	0x0000, 0x0000, 0x0305, 0xfd05, 0x0204, 0x0204, 0xfe04, 0xfe04,
	0x0103, 0x0103, 0x0103, 0x0103, 0xff03, 0xff03, 0xff03, 0xff03,
	0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
	0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001
};

const static int16_t *s_motionVector1 = (int16_t *) us_motionVector1;

static const uint8_t VIDEO_PREMULTIPLIER_MATRIX[] = {
	32, 44, 42, 38, 32, 25, 17, 9,
	44, 62, 58, 52, 44, 35, 24, 12,
	42, 58, 55, 49, 42, 33, 23, 12,
	38, 52, 49, 44, 38, 30, 20, 10,
	32, 44, 42, 38, 32, 25, 17, 9,
	25, 35, 33, 30, 25, 20, 14, 7,
	17, 24, 23, 20, 17, 14, 9, 5,
	9, 12, 12, 10, 9, 7, 5, 2
};

static inline uint8_t clampToByte(int32_t n) {
	return (n > 255) ? 255 : (n < 0) ? 0 : n;
}

// In-place 8x8 inverse DCT. block holds 64 premultiplied coefficients in raster order, and receives the samples.
void video_idct(int32_t *block) {
	int b1, b3, b4, b6, b7, tmp1, tmp2, m0,
			x0, x1, x2, x3, x4, y3, y4, y5, y6, y7;

	// Transform columns
	for (int i = 0; i < 8; ++i) {
		b1 = block[4 * 8 + i];
		b3 = block[2 * 8 + i] + block[6 * 8 + i];
		b4 = block[5 * 8 + i] - block[3 * 8 + i];
		tmp1 = block[1 * 8 + i] + block[7 * 8 + i];
		tmp2 = block[3 * 8 + i] + block[5 * 8 + i];
		b6 = block[1 * 8 + i] - block[7 * 8 + i];
		b7 = tmp1 + tmp2;
		m0 = block[0 * 8 + i];
		x4 = ((b6 * 473 - b4 * 196 + 128) >> 8) - b7;
		x0 = x4 - (((tmp1 - tmp2) * 362 + 128) >> 8);
		x1 = m0 - b1;
		x2 = (((block[2 * 8 + i] - block[6 * 8 + i]) * 362 + 128) >> 8) - b3;
		x3 = m0 + b1;
		y3 = x1 + x2;
		y4 = x3 + b3;
		y5 = x1 - x2;
		y6 = x3 - b3;
		y7 = -x0 - ((b4 * 473 + b6 * 196 + 128) >> 8);
		block[0 * 8 + i] = b7 + y4;
		block[1 * 8 + i] = x4 + y3;
		block[2 * 8 + i] = y5 - x0;
		block[3 * 8 + i] = y6 - y7;
		block[4 * 8 + i] = y6 + y7;
		block[5 * 8 + i] = x0 + y5;
		block[6 * 8 + i] = y3 - x4;
		block[7 * 8 + i] = y4 - b7;
	}

	// Transform rows
	for (int i = 0; i < 64; i += 8) {
		b1 = block[4 + i];
		b3 = block[2 + i] + block[6 + i];
		b4 = block[5 + i] - block[3 + i];
		tmp1 = block[1 + i] + block[7 + i];
		tmp2 = block[3 + i] + block[5 + i];
		b6 = block[1 + i] - block[7 + i];
		b7 = tmp1 + tmp2;
		m0 = block[0 + i];
		x4 = ((b6 * 473 - b4 * 196 + 128) >> 8) - b7;
		x0 = x4 - (((tmp1 - tmp2) * 362 + 128) >> 8);
		x1 = m0 - b1;
		x2 = (((block[2 + i] - block[6 + i]) * 362 + 128) >> 8) - b3;
		x3 = m0 + b1;
		y3 = x1 + x2;
		y4 = x3 + b3;
		y5 = x1 - x2;
		y6 = x3 - b3;
		y7 = -x0 - ((b4 * 473 + b6 * 196 + 128) >> 8);
		block[0 + i] = (b7 + y4 + 128) >> 8;
		block[1 + i] = (x4 + y3 + 128) >> 8;
		block[2 + i] = (y5 - x0 + 128) >> 8;
		block[3 + i] = (y6 - y7 + 128) >> 8;
		block[4 + i] = (y6 + y7 + 128) >> 8;
		block[5 + i] = (x0 + y5 + 128) >> 8;
		block[6 + i] = (y3 - x4 + 128) >> 8;
		block[7 + i] = (y4 - b7 + 128) >> 8;
	}
}

Frame::Frame() : m_ptsMillis(-1), m_yData(nullptr), m_crData(nullptr), m_cbData(nullptr), m_frameSize({0, 0, 0, 0}) {
	// empty
}

Frame::~Frame() {
	delete[] m_yData;
	delete[] m_crData;
	delete[] m_cbData;
}

void Frame::init(const int32_t ptsMillis, const FrameSize frameSize) {
	if (frameSize != m_frameSize) {
		// Cleared before allocating, so a failed allocation leaves nothing freed twice
		delete[] m_yData;
		delete[] m_crData;
		delete[] m_cbData;
		m_yData = m_crData = m_cbData = nullptr;
		m_frameSize = {};
		m_yData = new uint8_t[frameSize.luma_width * frameSize.luma_height]();
		m_crData = new uint8_t[frameSize.chroma_width * frameSize.chroma_height]();
		m_cbData = new uint8_t[frameSize.chroma_width * frameSize.chroma_height]();
		m_frameSize = frameSize;
	}
	m_ptsMillis = ptsMillis;
}

uint32_t Mpeg1VideoDecoder::decodeFrame() {
	// PTS of this picture's start code, -1 if it has none (presentation time is then extrapolated)
	m_ptrFrameCurrent->init(m_stream.getPts(0), m_frameSize);

	uint32_t m_temporal_reference;
	PULL2dump1(m_temporal_reference, 10, picture_coding_type, 3, 16);
	last_picture_coding_type = picture_coding_type;

	full_pel_forward_vector = full_pel_backward_vector = false;

	if (picture_coding_type == 0 || picture_coding_type > VideoPictureType_B) {
		throw std::runtime_error("Unknown picture coding type");
	}

	// P or B frame
	if (picture_coding_type == VideoPictureType_PREDICTIVE ||
	    picture_coding_type == VideoPictureType_B) {
		int8_t forward_f_code;
		PULL2(full_pel_forward_vector, 1, forward_f_code, 3);

		assert(forward_f_code != 0);
		// f_code is 1..7, so r_size is 0..6
		forward_r_size = forward_f_code - 1;

		if (picture_coding_type == VideoPictureType_B) {
			int8_t backward_f_code;
			PULL2(full_pel_backward_vector, 1, backward_f_code, 3);

			assert(backward_f_code != 0);
			backward_r_size = backward_f_code - 1;
		}

		while (m_bitStream.pull(1)) {
			m_bitStream.skip(8); // extra_information_picture
		}
	}

	skipExtensionAndUserData();

	while (nextStartCodeRange(SLICE_START_CODE_START, SLICE_START_CODE_END)) {
		processSlice(lastStartCode() & 255);

		// TODO should this be here? It's not in the spec
		//if (macroblock_address == m_sequence->mb_size - 1) break;
	}

	return m_temporal_reference;
}

void Mpeg1VideoDecoder::processSlice(uint32_t slice) {
	// std::cout << "slice: " << std::hex << lastStartCode() << "\n";
	assert(slice > 0);
	macroblock_address = (slice - 1) * m_macroblockWidth - 1;
	debug_out << "macroblock_address: " << macroblock_address << std::endl;

	motion_backward_h = motion_forward_h = motion_backward_v = motion_forward_v = 0;
	dc_predictor[0] = dc_predictor[1] = dc_predictor[2] = 128;

	// quantizer_scale = m_bitStream.pull(5);
	// while (m_bitStream.pull(1)) m_bitStream.pull(8); // extra information slice
	uint32_t isExtraSlice;
	PULL2(quantizer_scale, 5, isExtraSlice, 1);
	while (isExtraSlice) {
		//m_bitStream.pull(8);
		//t = m_bitStream.pull(1);
		isExtraSlice = m_bitStream.pull(9) >> 8;
	}

	processMacroblock(true);
	while (
		macroblock_address < (mb_size - 1) &&
		m_bitStream.peek(23)) {
		processMacroblock(false);
	}
}

void Mpeg1VideoDecoder::processMacroblock(const bool slice_begin) {
	debug_out << "macroblock: slice_begin=" << slice_begin << std::endl;

	uint32_t macroblock_address_increment = 0;
	auto t = pullMacroblockAddressIncrement();
	while (t == 34) {
		// stuffing
		t = pullMacroblockAddressIncrement();
	}
	while (t == 35) {
		// escape
		macroblock_address_increment += 33;
		t = pullMacroblockAddressIncrement();
	}
	debug_out << "macroblock_address_increment = " << macroblock_address_increment << std::endl;

	if (slice_begin) {
		macroblock_address += t;
		debug_out << "macroblock_address=" << macroblock_address << std::endl;
	} else {
		macroblock_address_increment += t;
		if (macroblock_address + macroblock_address_increment >= mb_size) {
			throw std::runtime_error("Macroblock address out of range");
		}
		if (macroblock_address_increment > 1) {
			// Skipped macroblocks reset DC predictors
			dc_predictor[0] = dc_predictor[1] = dc_predictor[2] = 128;

			// Skipped macroblocks in P-pictures reset motion vectors
			if (picture_coding_type == VideoPictureType_PREDICTIVE) {
				motion_forward_h = motion_forward_v = 0;
			}
		}

		// Predict skipped macroblocks
		while (macroblock_address_increment > 1) {
			macroblock_address++;
			mb_row = macroblock_address / m_macroblockWidth;
			mb_col = macroblock_address % m_macroblockWidth;

			predictMacroblock();
			macroblock_address_increment--;
		}
		macroblock_address++;
	}

	mb_row = macroblock_address / m_macroblockWidth;
	mb_col = macroblock_address % m_macroblockWidth;

	debug_out << "mb_row=" << mb_row << " mb_col=" << mb_col << std::endl;

	if (mb_col >= m_macroblockWidth || mb_row >= m_macroblockHeight) {
		throw std::runtime_error("Corrupt stream");
	}

	decodeMacroblockType();

	if (macroblock_intra) {
		// Intra-coded macroblocks reset motion vectors
		motion_backward_h = motion_forward_h = motion_backward_v = motion_forward_v = 0;
	} else {
		// Non-intra macroblocks reset DC predictors
		dc_predictor[0] = dc_predictor[1] = dc_predictor[2] = 128;

		decodeMotionVectors();
		predictMacroblock();
	}

	// Decode blocks
	const uint32_t coded_block_pattern = macroblock_pattern
		                               ? decodeCodedBlockPattern()
		                               : (macroblock_intra ? 0x3f : 0);

	if (coded_block_pattern) {
		decodeBlock(coded_block_pattern);
		/*if (coded_block_pattern & 0x20) decodeBlock(0);
		if (coded_block_pattern & 0x10) decodeBlock(1);
		if (coded_block_pattern & 0x08) decodeBlock(2);
		if (coded_block_pattern & 0x04) decodeBlock(3);
		if (coded_block_pattern & 0x02) decodeBlock(4);
		if (coded_block_pattern & 0x01) decodeBlock(5);*/
	}

	if (picture_coding_type == VideoPictureType_D) {
		if (!m_bitStream.pull(1)) {
			throw std::runtime_error("VideoPictureType.D end of macroblock should be 1");
		}
	}
}

void Mpeg1VideoDecoder::decodeMotionVectors() {
	if (macroblock_motion_forward) {
		motion_forward_h = updateMotionVector(forward_r_size, motion_forward_h);
		motion_forward_v = updateMotionVector(forward_r_size, motion_forward_v);
	} else if (picture_coding_type == VideoPictureType_PREDICTIVE) {
		// No motion information in P-picture, reset vectors
		motion_forward_h = motion_forward_v = 0;
	}

	if (macroblock_motion_backward) {
		motion_backward_h = updateMotionVector(backward_r_size, motion_backward_h);
		motion_backward_v = updateMotionVector(backward_r_size, motion_backward_v);
	}
}

int32_t Mpeg1VideoDecoder::updateMotionVector(int32_t r_size, int32_t motion) {
	const auto index = static_cast<int32_t>(m_bitStream.peek(11 + 6)); // + up to 6 r_size
	const int32_t value = (index >> (7 + 6))
		                      ? s_motionVector1[index >> (6 + 6)] // 5 bits
		                      : s_motionVector[index >> (6)]; // 11 bits

	int dropBits = value & 0xFF;
	const int32_t m_code = value >> 8;
	int32_t fscale = 1 << r_size;
	if (m_code && r_size) {
		dropBits += r_size;
		const int32_t r = (index >> ((11 + 6) - dropBits)) & (fscale - 1);
		const int32_t d = ((abs(m_code) - 1) << r_size) + r + 1;
		if (m_code < 0) {
			motion -= d;
		} else {
			motion += d;
		}
	} else {
		motion += m_code;
	}
	m_bitStream.skip(dropBits);

	fscale <<= 4;
	if (motion >= fscale) {
		motion -= fscale << 1;
	} else if (motion < -fscale) {
		motion += fscale << 1;
	}
	return motion;
}

void Mpeg1VideoDecoder::predictMacroblock() const {
	int fw_h = motion_forward_h;
	int fw_v = motion_forward_v;

	if (full_pel_forward_vector) {
		fw_h <<= 1;
		fw_v <<= 1;
	}
	if (picture_coding_type == VideoPictureType_B) {
		int bw_h = motion_backward_h;
		int bw_v = motion_backward_v;

		if (full_pel_backward_vector) {
			bw_h <<= 1;
			bw_v <<= 1;
		}

		if (macroblock_motion_forward) {
			videoCopyMacroblock(fw_h, fw_v, m_ptrFramePast);
			if (macroblock_motion_backward) {
				videoInterpolateMacroblock(bw_h, bw_v, m_ptrFrameFuture);
			}
		} else {
			videoCopyMacroblock(bw_h, bw_v, m_ptrFrameFuture);
		}
	} else {
		// P pictures predict from the past frame, using forward vectors
		videoCopyMacroblock(fw_h, fw_v, m_ptrFramePast);
	}
}

void Mpeg1VideoDecoder::videoCopyMacroblock(const int motion_h, const int motion_v, const Frame *d) const {
	const auto frame_current = m_ptrFrameCurrent;
	videoProcessMacroblock(frame_current->m_yData, d->m_yData, motion_h, motion_v, 16);
	videoProcessMacroblock(frame_current->m_crData, d->m_crData, motion_h / 2, motion_v / 2, 8);
	videoProcessMacroblock(frame_current->m_cbData, d->m_cbData, motion_h / 2, motion_v / 2, 8);
}

void Mpeg1VideoDecoder::videoInterpolateMacroblock(const int motion_h, const int motion_v, const Frame *d) const {
	const auto frame_current = m_ptrFrameCurrent;
	videoProcessMacroblockInterpolate(frame_current->m_yData, d->m_yData, motion_h, motion_v, 16);
	videoProcessMacroblockInterpolate(frame_current->m_crData, d->m_crData, motion_h / 2, motion_v / 2, 8);
	videoProcessMacroblockInterpolate(frame_current->m_cbData, d->m_cbData, motion_h / 2, motion_v / 2, 8);
}

#define BLOCK_SET(DEST, DEST_INDEX, DEST_WIDTH, SOURCE_INDEX, SOURCE_WIDTH, BLOCK_SIZE, OP) { \
	uint32_t dest_scan = DEST_WIDTH - BLOCK_SIZE; \
	uint32_t source_scan = SOURCE_WIDTH - BLOCK_SIZE; \
	for (uint32_t y = 0; y < BLOCK_SIZE; y++) { \
		for (uint32_t x = 0; x < BLOCK_SIZE; x++) { \
			uint8_t op = OP; \
			SOURCE_INDEX++; \
			DEST[DEST_INDEX++] = op; \
		} \
		SOURCE_INDEX += source_scan; \
		DEST_INDEX += dest_scan; \
	}}

// Predicts one SIZE x SIZE block from s into d, a word at a time. HALF_PEL is the half pel flags, horizontal in
// bit 1 and vertical in bit 0. INTERPOLATE averages the prediction with what d already holds.
template <uint32_t SIZE, uint32_t HALF_PEL, bool INTERPOLATE>
static void videoPredictBlock(uint8_t *d, const uint8_t *s, const uint32_t dw) {
	constexpr int WORDS = SIZE / 4;
	for (uint32_t y = 0; y < SIZE; y++) {
		uint32_t row[WORDS];
		if constexpr (HALF_PEL == 0) {
			PixelWords::loadRow<WORDS>(s, row);
		} else if constexpr (HALF_PEL == 3) {
			uint32_t right[WORDS], below[WORDS], belowRight[WORDS];
			PixelWords::loadRow<WORDS>(s, row);
			PixelWords::loadRow<WORDS>(s + 1, right);
			PixelWords::loadRow<WORDS>(s + dw, below);
			PixelWords::loadRow<WORDS>(s + dw + 1, belowRight);
			for (int i = 0; i < WORDS; i++) {
				row[i] = PixelWords::average4(row[i], right[i], below[i], belowRight[i]);
			}
		} else {
			uint32_t other[WORDS];
			PixelWords::loadRow<WORDS>(s, row);
			PixelWords::loadRow<WORDS>(HALF_PEL == 1 ? s + dw : s + 1, other);
			for (int i = 0; i < WORDS; i++) {
				row[i] = PixelWords::average2(row[i], other[i]);
			}
		}
		for (int i = 0; i < WORDS; i++) {
			uint32_t value = row[i];
			if constexpr (INTERPOLATE) {
				value = PixelWords::average2(PixelWords::loadAligned(d + 4 * i), value);
			}
			PixelWords::storeAligned(d + 4 * i, value);
		}
		s += dw;
		d += dw;
	}
}

template <uint32_t SIZE, bool INTERPOLATE>
static void videoPredictBlock(uint8_t *d, const uint8_t *s, const uint32_t dw, const uint32_t halfPel) {
	// halfPel is always 0..3
	switch (halfPel) {
		case 0: videoPredictBlock<SIZE, 0, INTERPOLATE>(d, s, dw); break;
		case 1: videoPredictBlock<SIZE, 1, INTERPOLATE>(d, s, dw); break;
		case 2: videoPredictBlock<SIZE, 2, INTERPOLATE>(d, s, dw); break;
		default: videoPredictBlock<SIZE, 3, INTERPOLATE>(d, s, dw); break;
	}
}

// One plane of a macroblock's prediction. d starts at a multiple of 8 pixels in a plane whose width is a
// multiple of 8, so its rows are word aligned.
template <bool INTERPOLATE>
static void videoPredictPlane(uint8_t *d, const uint8_t *s, const uint32_t mb_row, const uint32_t mb_col,
                              const uint32_t macroblockWidth, const uint32_t macroblockHeight, const int motion_h,
                              const int motion_v, const uint32_t block_size) {
	const uint32_t dw = macroblockWidth * block_size;

	const int32_t hp = motion_h >> 1, vp = motion_v >> 1;
	const uint32_t si = ((mb_row * block_size) + vp) * dw + (mb_col * block_size) + hp;
	const uint32_t di = (mb_row * dw + mb_col) * block_size;

	const uint32_t max_address = (dw * (macroblockHeight * block_size - block_size + 1) - block_size);
	if (si > max_address || di > max_address) {
		// Motion vector points outside the frame (corrupt data), leave the block as it is
		debug_out << "Motion vector out of range" << std::endl;
		return;
	}

	const uint32_t halfPel = ((motion_h & 1) << 1) | (motion_v & 1);
	if (block_size == 16) {
		videoPredictBlock<16, INTERPOLATE>(d + di, s + si, dw, halfPel);
	} else {
		videoPredictBlock<8, INTERPOLATE>(d + di, s + si, dw, halfPel);
	}
}

void Mpeg1VideoDecoder::videoProcessMacroblock(uint8_t *d, const uint8_t *s, const int motion_h, const int motion_v,
                                               const uint32_t block_size) const {
	videoPredictPlane<false>(d, s, mb_row, mb_col, m_macroblockWidth, m_macroblockHeight, motion_h, motion_v, block_size);
}

void Mpeg1VideoDecoder::videoProcessMacroblockInterpolate(uint8_t *d, const uint8_t *s, const int motion_h, const int motion_v,
                                                          const uint32_t block_size) const {
	videoPredictPlane<true>(d, s, mb_row, mb_col, m_macroblockWidth, m_macroblockHeight, motion_h, motion_v, block_size);
}


void Mpeg1VideoDecoder::decodeBlock(uint32_t cbp) {
	for (int i = 0; i < 6; i++) {
		cbp <<= 1;
		if (!(cbp & 0x40)) continue;
		unsigned int n = 0;
		uint8_t *quant_matrix;

		//dout << "decodeBlock " << std::dec << "\n";
		if (macroblock_intra) {
			int plane_index = i > 3 ? i - 3 : 0;
			// first 4 plane indexes are 0 (luma blocks), next 2 are chromas
			auto predictor = dc_predictor[plane_index];

			uint32_t dct_dc_size = i < 4
				                       ? decodeDCTDCSizeLuminance()
				                       : decodeDCTDCSizeChrominance();

			// block_data[0] is the block's DC coefficient
			if (dct_dc_size != 0) {
				auto dct_dc_differential = static_cast<int32_t>(m_bitStream.pull(dct_dc_size));
				if (dct_dc_differential & (1 << (dct_dc_size - 1))) {
					block_data[0] = predictor + dct_dc_differential;
				} else {
					block_data[0] = predictor + (-(1 << dct_dc_size) | (dct_dc_differential + 1));
				}
			} else {
				block_data[0] = predictor;
			}

			// Save predictor value
			dc_predictor[plane_index] = block_data[0];

			// Dequantize + premultiply
			block_data[0] <<= (3 + 5);

			n = 1;
			quant_matrix = intra_quant_matrix;
		} else {
			quant_matrix = non_intra_quant_matrix;
			RunLevel runLevel = decodeDCTCoeff(true);

			int32_t level = runLevel.level;

			n = runLevel.run;
			if (n >= 64) {
				throw std::runtime_error("Invalid macroblock index");
			}

			int de_zig_zagged = VIDEO_ZIG_ZAG[n];
			n++;

			// Dequantize, oddify, clip
			level <<= 1;
			level += (level < 0 ? -1 : 1);

			level *= (quantizer_scale * quant_matrix[de_zig_zagged]) >> 4;
			if ((level & 1) == 0) {
				level -= level > 0 ? 1 : -1;
			}
			// Level clamp
			if (level > 2047) level = 2047;
			else if (level < -2048) level = -2048;

			// level is 12 bits, premul is 6bits = 18bits so block data has to be int32
			// Save premultiplied coefficient
			block_data[de_zig_zagged] = level * VIDEO_PREMULTIPLIER_MATRIX[de_zig_zagged];
		}

		if (picture_coding_type != VideoPictureType_D) {
			// '10' is end_of_block
			while (m_bitStream.peek(2) != 2) {
				// TODO not sure dct_coeff_next needs to be returned
				RunLevel runLevel = decodeDCTCoeff(false);

				int level = runLevel.level;

				n += runLevel.run;
				if (n >= 64) {
					throw std::runtime_error("Invalid macroblock index");
				}

				int de_zig_zagged = VIDEO_ZIG_ZAG[n];
				n++;

				// Dequantize, oddify, clip
				level <<= 1;
				if (!macroblock_intra) {
					level += (level < 0 ? -1 : 1);
				}
				level *= (quantizer_scale * quant_matrix[de_zig_zagged]) >> 4;
				if ((level & 1) == 0) {
					level -= level > 0 ? 1 : -1;
				}
				if (level > 2047) {
					level = 2047;
				} else if (level < -2048) {
					level = -2048;
				}
				// level is 12 bits, premul is 6bits = 18bits so block data has to be int32
				// Save premultiplied coefficient
				block_data[de_zig_zagged] = level * VIDEO_PREMULTIPLIER_MATRIX[de_zig_zagged];
			}
			m_bitStream.skip(2);
		}

		/*dout << "block=[";
		for (int i = 0; i < 64; i++) dout << std::hex << block_data[i] << std::dec << ",";
		dout << std::endl;*/

		// Move block to its place
		uint8_t *d;
		uint32_t dw;
		uint32_t di;

		if (i < 4) {
			d = m_ptrFrameCurrent->m_yData;
			dw = m_frameSize.luma_width;
			di = (mb_row * m_frameSize.luma_width + mb_col) << 4; // *16
			// luma blocks are encoded in groups of 2x2
			if (i & 1) di += 8;
			if (i & 2) di += m_frameSize.luma_width << 3; // *8
		} else {
			// Blocks 4 and 5 are Cb then Cr
			d = (i == 4) ? m_ptrFrameCurrent->m_cbData : m_ptrFrameCurrent->m_crData;
			dw = m_frameSize.chroma_width;
			di = ((mb_row * m_frameSize.luma_width) << 2) + (mb_col << 3);
		}

		debug_out << "di=" << di << " dw=" << dw << std::endl;

		int32_t *s = block_data;
		int si = 0;
		if (macroblock_intra) {
			// Overwrite (no prediction)
			if (n == 1) {
				int clamped = clampToByte((s[0] + 128) >> 8);
				BLOCK_SET(d, di, dw, si, 8, 8, clamped);
				s[0] = 0;
			} else {
				video_idct(s);
				BLOCK_SET(d, di, dw, si, 8, 8, clampToByte(s[si]));
				memset(block_data, 0, sizeof(block_data));
			}
		} else {
			// Add data to the predicted macroblock
			if (n == 1) {
				int value = (s[0] + 128) >> 8;
				BLOCK_SET(d, di, dw, si, 8, 8, clampToByte(d[di] + value));
				s[0] = 0;
			} else {
				video_idct(s);
				BLOCK_SET(d, di, dw, si, 8, 8, clampToByte(d[di] + s[si]));
				memset(block_data, 0, sizeof(block_data));
			}
		}
	}
}

const uint8_t s_dctDcSizeLuminance[] =
{
	0x12, 0x12, 0x12, 0x12, 0x22, 0x22, 0x22, 0x22,
	0x03, 0x03, 0x33, 0x33, 0x43, 0x43, 0x54, 0x00
};

const uint8_t s_dctDcSizeLuminance1[] =
{
	0x65, 0x65, 0x65, 0x65, 0x76, 0x76, 0x87, 0x00
};

uint32_t Mpeg1VideoDecoder::decodeDCTDCSizeLuminance() {
	uint32_t index = m_bitStream.peek(7);
	uint32_t value = s_dctDcSizeLuminance[index >> 3];

	if (value == 0) {
		value = s_dctDcSizeLuminance1[index & 0x07];
	}

	m_bitStream.skip(value & 0xf);

	return value >> 4;
}

// 93-100% of lookups end here on the test files
const uint8_t s_dctDcSizeChrominance[] =
{
	0x02, 0x02, 0x02, 0x02, 0x12, 0x12, 0x12, 0x12,
	0x22, 0x22, 0x22, 0x22, 0x33, 0x33, 0x44, 0x00
};

// 0.1-7.3% of lookups on the test files
const uint8_t s_dctDcSizeChrominance1[] =
{
	0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
	0x66, 0x66, 0x66, 0x66, 0x77, 0x77, 0x88, 0x00
};

int Mpeg1VideoDecoder::decodeDCTDCSizeChrominance() {
	uint32_t index = m_bitStream.peek(8);
	uint32_t value = s_dctDcSizeChrominance[index >> 4];
	if (value == 0) {
		value = s_dctDcSizeChrominance1[index & 0xf];
	}
	m_bitStream.skip(value & 0xf);
	return value >> 4;
}

const uint16_t s_codedBlockPattern[] =
{
	0x0000, 0x0000, 0x2709, 0x1b09, 0x3b09, 0x3709, 0x2f09, 0x1f09,
	0x3a08, 0x3a08, 0x3608, 0x3608, 0x2e08, 0x2e08, 0x1e08, 0x1e08,
	0x3908, 0x3908, 0x3508, 0x3508, 0x2d08, 0x2d08, 0x1d08, 0x1d08,
	0x2608, 0x2608, 0x1a08, 0x1a08, 0x2508, 0x2508, 0x1908, 0x1908,

	0x2b08, 0x2b08, 0x1708, 0x1708, 0x3308, 0x3308, 0x0f08, 0x0f08,
	0x2a08, 0x2a08, 0x1608, 0x1608, 0x3208, 0x3208, 0x0e08, 0x0e08,
	0x2908, 0x2908, 0x1508, 0x1508, 0x3108, 0x3108, 0x0d08, 0x0d08,
	0x2308, 0x2308, 0x1308, 0x1308, 0x0b08, 0x0b08, 0x0708, 0x0708,

	0x2207, 0x2207, 0x2207, 0x2207, 0x1207, 0x1207, 0x1207, 0x1207,
	0x0a07, 0x0a07, 0x0a07, 0x0a07, 0x0607, 0x0607, 0x0607, 0x0607,
	0x2107, 0x2107, 0x2107, 0x2107, 0x1107, 0x1107, 0x1107, 0x1107,
	0x0907, 0x0907, 0x0907, 0x0907, 0x0507, 0x0507, 0x0507, 0x0507,

	0x3f06, 0x3f06, 0x3f06, 0x3f06, 0x3f06, 0x3f06, 0x3f06, 0x3f06,
	0x0306, 0x0306, 0x0306, 0x0306, 0x0306, 0x0306, 0x0306, 0x0306,
	0x2406, 0x2406, 0x2406, 0x2406, 0x2406, 0x2406, 0x2406, 0x2406,
	0x1806, 0x1806, 0x1806, 0x1806, 0x1806, 0x1806, 0x1806, 0x1806,

	0x3e05, 0x3e05, 0x3e05, 0x3e05, 0x3e05, 0x3e05, 0x3e05, 0x3e05,
	0x3e05, 0x3e05, 0x3e05, 0x3e05, 0x3e05, 0x3e05, 0x3e05, 0x3e05,
	0x0205, 0x0205, 0x0205, 0x0205, 0x0205, 0x0205, 0x0205, 0x0205,
	0x0205, 0x0205, 0x0205, 0x0205, 0x0205, 0x0205, 0x0205, 0x0205,

	0x3d05, 0x3d05, 0x3d05, 0x3d05, 0x3d05, 0x3d05, 0x3d05, 0x3d05,
	0x3d05, 0x3d05, 0x3d05, 0x3d05, 0x3d05, 0x3d05, 0x3d05, 0x3d05,
	0x0105, 0x0105, 0x0105, 0x0105, 0x0105, 0x0105, 0x0105, 0x0105,
	0x0105, 0x0105, 0x0105, 0x0105, 0x0105, 0x0105, 0x0105, 0x0105,

	0x3805, 0x3805, 0x3805, 0x3805, 0x3805, 0x3805, 0x3805, 0x3805,
	0x3805, 0x3805, 0x3805, 0x3805, 0x3805, 0x3805, 0x3805, 0x3805,
	0x3405, 0x3405, 0x3405, 0x3405, 0x3405, 0x3405, 0x3405, 0x3405,
	0x3405, 0x3405, 0x3405, 0x3405, 0x3405, 0x3405, 0x3405, 0x3405,

	0x2c05, 0x2c05, 0x2c05, 0x2c05, 0x2c05, 0x2c05, 0x2c05, 0x2c05,
	0x2c05, 0x2c05, 0x2c05, 0x2c05, 0x2c05, 0x2c05, 0x2c05, 0x2c05,
	0x1c05, 0x1c05, 0x1c05, 0x1c05, 0x1c05, 0x1c05, 0x1c05, 0x1c05,
	0x1c05, 0x1c05, 0x1c05, 0x1c05, 0x1c05, 0x1c05, 0x1c05, 0x1c05,

	0x2805, 0x2805, 0x2805, 0x2805, 0x2805, 0x2805, 0x2805, 0x2805,
	0x2805, 0x2805, 0x2805, 0x2805, 0x2805, 0x2805, 0x2805, 0x2805,
	0x1405, 0x1405, 0x1405, 0x1405, 0x1405, 0x1405, 0x1405, 0x1405,
	0x1405, 0x1405, 0x1405, 0x1405, 0x1405, 0x1405, 0x1405, 0x1405,

	0x3005, 0x3005, 0x3005, 0x3005, 0x3005, 0x3005, 0x3005, 0x3005,
	0x3005, 0x3005, 0x3005, 0x3005, 0x3005, 0x3005, 0x3005, 0x3005,
	0x0c05, 0x0c05, 0x0c05, 0x0c05, 0x0c05, 0x0c05, 0x0c05, 0x0c05,
	0x0c05, 0x0c05, 0x0c05, 0x0c05, 0x0c05, 0x0c05, 0x0c05, 0x0c05,

	0x2004, 0x2004, 0x2004, 0x2004, 0x2004, 0x2004, 0x2004, 0x2004,
	0x2004, 0x2004, 0x2004, 0x2004, 0x2004, 0x2004, 0x2004, 0x2004,
	0x2004, 0x2004, 0x2004, 0x2004, 0x2004, 0x2004, 0x2004, 0x2004,
	0x2004, 0x2004, 0x2004, 0x2004, 0x2004, 0x2004, 0x2004, 0x2004,

	0x1004, 0x1004, 0x1004, 0x1004, 0x1004, 0x1004, 0x1004, 0x1004,
	0x1004, 0x1004, 0x1004, 0x1004, 0x1004, 0x1004, 0x1004, 0x1004,
	0x1004, 0x1004, 0x1004, 0x1004, 0x1004, 0x1004, 0x1004, 0x1004,
	0x1004, 0x1004, 0x1004, 0x1004, 0x1004, 0x1004, 0x1004, 0x1004,

	0x0804, 0x0804, 0x0804, 0x0804, 0x0804, 0x0804, 0x0804, 0x0804,
	0x0804, 0x0804, 0x0804, 0x0804, 0x0804, 0x0804, 0x0804, 0x0804,
	0x0804, 0x0804, 0x0804, 0x0804, 0x0804, 0x0804, 0x0804, 0x0804,
	0x0804, 0x0804, 0x0804, 0x0804, 0x0804, 0x0804, 0x0804, 0x0804,

	0x0404, 0x0404, 0x0404, 0x0404, 0x0404, 0x0404, 0x0404, 0x0404,
	0x0404, 0x0404, 0x0404, 0x0404, 0x0404, 0x0404, 0x0404, 0x0404,
	0x0404, 0x0404, 0x0404, 0x0404, 0x0404, 0x0404, 0x0404, 0x0404,
	0x0404, 0x0404, 0x0404, 0x0404, 0x0404, 0x0404, 0x0404, 0x0404,

	0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03,
	0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03,
	0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03,
	0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03,

	0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03,
	0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03,
	0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03,
	0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03, 0x3c03
};


uint32_t Mpeg1VideoDecoder::decodeCodedBlockPattern() {
	uint32_t v = s_codedBlockPattern[m_bitStream.peek(9)];
	m_bitStream.skip(v & 0xff);
	return v >> 0x8;
}

/*
2 - B.2 Macroblock Type
Table 2 - B.2a.Variable length codes for macroblock_type in intra - coded pictures(I - pictures).

VLC code	macroblock_quant	macroblock_motion_forward	macroblock_motion_backward	macroblock_pattern	macroblock_intra
1			0					0							0							0					1
01			1					0							0							0					1

Table 2 - B.2b.Variable length codes for macroblock_type in predictive - coded pictures(P - pictures).
VLC code	macroblock_quant	macroblock_motion_forward	macroblock_motion_backward	macroblock_pattern	macroblock_intra
1			0					1							0							1					0
01			0					0							0							1					0
001			0					1							0							0					0
00011		0					0							0							0					1
00010		1					1							0							1					0
00001		1					0							0							1					0
000001		1					0							0							0					1

Table 2 - B.2c.Variable length codes for macroblock_type in bidirectionally predictive - coded pictures(B - pictures).
VLC code	macroblock_quant	macroblock_motion_forward	macroblock_motion_backward	macroblock_pattern	macroblock_intra
10			0					1							1							0					0
11			0					1							1							1					0
010			0					0							1							0					0
011			0					0							1							1					0
0010		0					1							0							0					0
0011		0					1							0							1					0
00011		0					0							0							0					1
00010		1					1							1							1					0
000011		1					1							0							1					0
000010		1					0							1							1					0
000001		1					0							0							0					1
*/

/*const uint8_t s_macroblockTypeI[] =
{
	0x00, 0x12, 0x01, 0x01
};*/
// One byte per 2-bit code, by code value: bits 0-1 code length, bit 2 macroblock_quant
const static uint32_t s_macroblockTypeI = 0 + (((1 << 2) + 2) << 8) + (1 << 16) + (1 << 24);

// Indexed by the next 6 bits, codes starting with 1 are handled in decodeMacroblockType.
// High byte: bit 4 macroblock_quant, 3 forward, 2 backward, 1 pattern, 0 intra. Low byte: code length.
const static uint16_t s_macroblockTypeP[] =
{
	0x0000, 0x1106, 0x1205, 0x1205, 0x1a05, 0x1a05, 0x0105, 0x0105,
	0x0803, 0x0803, 0x0803, 0x0803, 0x0803, 0x0803, 0x0803, 0x0803,
	0x0202, 0x0202, 0x0202, 0x0202, 0x0202, 0x0202, 0x0202, 0x0202,
	0x0202, 0x0202, 0x0202, 0x0202, 0x0202, 0x0202, 0x0202, 0x0202
};

// Same format as s_macroblockTypeP
const static uint16_t s_macroblockTypeB[] =
{
	0x0000, 0x1106, 0x1606, 0x1a06, 0x1e05, 0x1e05, 0x0105, 0x0105,
	0x0804, 0x0804, 0x0804, 0x0804, 0x0a04, 0x0a04, 0x0a04, 0x0a04,
	0x0403, 0x0403, 0x0403, 0x0403, 0x0403, 0x0403, 0x0403, 0x0403,
	0x0603, 0x0603, 0x0603, 0x0603, 0x0603, 0x0603, 0x0603, 0x0603
};

void Mpeg1VideoDecoder::decodeMacroblockType() {
	switch (picture_coding_type) {
		case VideoPictureType_INTRA: {
			uint32_t index = m_bitStream.peek(2);
			// This has to be uint8_t to mask a byte
			uint8_t value = s_macroblockTypeI >> (index << 3);

			macroblock_motion_forward = false;
			macroblock_motion_backward = false;
			macroblock_pattern = false;
			macroblock_intra = true;

			m_bitStream.skip(value & 3);

			// Quantizer scale
			if (value >> 2) {
				quantizer_scale = m_bitStream.pull(5);
			}

			break;
		}
		case VideoPictureType_PREDICTIVE: {
			const uint32_t index = m_bitStream.peek(6);

			// Handle special case: highest bit is 1
			const auto t = index < 0x20 ? s_macroblockTypeP[index] : ((0x0a << 8) + 1);
			const uint32_t value = t >> 8;
			m_bitStream.skip(t & 0xFF);

			macroblock_motion_forward = (value & 0x08);
			macroblock_motion_backward = (value & 0x04);
			macroblock_pattern = (value & 0x02);
			macroblock_intra = (value & 0x01);

			// Quantizer scale
			if (value & 0x10) {
				quantizer_scale = m_bitStream.pull(5);
			}
			break;
		}
		case VideoPictureType_B: {
			const uint32_t index = m_bitStream.peek(6);

			// Handle 2 special cases: highest bit 1
			const auto t = index < 0x20 ? s_macroblockTypeB[index] : index < 0x30 ? ((0x0c << 8) + 2) : ((0x0e << 8) + 2);
			const uint32_t value = t >> 8;
			m_bitStream.skip(t & 0xFF);

			macroblock_motion_forward = (value & 0x08);
			macroblock_motion_backward = (value & 0x04);
			macroblock_pattern = (value & 0x02);
			macroblock_intra = (value & 0x01);

			// Quantizer scale
			if (value & 0x10) {
				quantizer_scale = m_bitStream.pull(5);
			}
			break;
		}
		case VideoPictureType_D:
		default:
			break;
	}

	debug_out << //"macroblockType(m_macroblockQuant=" << m_macroblockQuant <<
		", macroblock_motion_forward=" << macroblock_motion_forward <<
		", macroblock_motion_backward=" << macroblock_motion_backward <<
		", macroblock_pattern=" << macroblock_pattern <<
		", macroblock_intra=" << macroblock_intra << std::endl;
}

// Address increment tables. High byte: increment, or 34 for stuffing, 35 for escape. Low byte: code length.
// Next1 and Next2 continue in s_macroblockAddressIncrement1 and s_macroblockAddressIncrement2.
static constexpr uint16_t Next1 = (uint16_t) 0xfffe;
static constexpr uint16_t Next2 = (uint16_t) 0xffff;
static constexpr uint16_t Escape = (uint16_t) 0x230b;
static constexpr uint16_t Stuffing = (uint16_t) 0x220b;

// Indexed by the top 5 of 11 bits. 95-99% of lookups end here on the test files.
constexpr uint16_t s_macroblockAddressIncrement[] =
{
	Next1, Next2, 0x0705, 0x0605, 0x0504, 0x0504, 0x0404, 0x0404,
	0x0303, 0x0303, 0x0303, 0x0303, 0x0203, 0x0203, 0x0203, 0x0203,
	0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101,
	0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101, 0x0101
};

// Next2, indexed by the low 6 of 11 bits. 0-2.2% of lookups on the test files.
constexpr uint16_t s_macroblockAddressIncrement2[] =
{
	0x0d08, 0x0d08, 0x0d08, 0x0d08, 0x0d08, 0x0d08, 0x0d08, 0x0d08,
	0x0c08, 0x0c08, 0x0c08, 0x0c08, 0x0c08, 0x0c08, 0x0c08, 0x0c08,
	0x0b08, 0x0b08, 0x0b08, 0x0b08, 0x0b08, 0x0b08, 0x0b08, 0x0b08,
	0x0a08, 0x0a08, 0x0a08, 0x0a08, 0x0a08, 0x0a08, 0x0a08, 0x0a08,
	0x0907, 0x0907, 0x0907, 0x0907, 0x0907, 0x0907, 0x0907, 0x0907,
	0x0907, 0x0907, 0x0907, 0x0907, 0x0907, 0x0907, 0x0907, 0x0907,
	0x0807, 0x0807, 0x0807, 0x0807, 0x0807, 0x0807, 0x0807, 0x0807,
	0x0807, 0x0807, 0x0807, 0x0807, 0x0807, 0x0807, 0x0807, 0x0807
};

// Next1, indexed by the low 6 of 11 bits. 0-3.1% of lookups on the test files.
constexpr uint16_t s_macroblockAddressIncrement1[] =
{
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	Escape, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, Stuffing,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x210b, 0x200b, 0x1f0b, 0x1e0b, 0x1d0b, 0x1c0b, 0x1b0b, 0x1a0b,
	0x190b, 0x180b, 0x170b, 0x160b, 0x150a, 0x150a, 0x140a, 0x140a,
	0x130a, 0x130a, 0x120a, 0x120a, 0x110a, 0x110a, 0x100a, 0x100a,
	0x0f08, 0x0f08, 0x0f08, 0x0f08, 0x0f08, 0x0f08, 0x0f08, 0x0f08,
	0x0e08, 0x0e08, 0x0e08, 0x0e08, 0x0e08, 0x0e08, 0x0e08, 0x0e08
};

uint32_t Mpeg1VideoDecoder::pullMacroblockAddressIncrement() {
	const auto index = m_bitStream.peek(11);
	auto value = s_macroblockAddressIncrement[index >> 6];
	if (value >= Next1) {
		value = (value == Next1 ? s_macroblockAddressIncrement1 : s_macroblockAddressIncrement2)[index & 0x3f];
	}

	m_bitStream.skip(value & 255);
	return value >> 8;
}

// Decoding tables for dct_coeff_first & dct_coeff_next
//
// This was originally 15 arrays. Now they are merged to form a single array,
// and using a scale to retrieve data from the correct array of coefficients.
//
// Total number of entries 16 * 32 = 512
// First 32 entries are not used in fact
const RunLevel s_dctCoefficients[] =
{
	// 0000 0000 0000 xxxx x
	{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
	{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
	{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
	{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
	{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
	{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
	{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
	{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},

	// 0000 0000 0001 xxxx s
	{1, 18, 17}, {1, -18, 17}, {1, 17, 17}, {1, -17, 17},
	{1, 16, 17}, {1, -16, 17}, {1, 15, 17}, {1, -15, 17},
	{6, 3, 17}, {6, -3, 17}, {16, 2, 17}, {16, -2, 17},
	{15, 2, 17}, {15, -2, 17}, {14, 2, 17}, {14, -2, 17},
	{13, 2, 17}, {13, -2, 17}, {12, 2, 17}, {12, -2, 17},
	{11, 2, 17}, {11, -2, 17}, {31, 1, 17}, {31, -1, 17},
	{30, 1, 17}, {30, -1, 17}, {29, 1, 17}, {29, -1, 17},
	{28, 1, 17}, {28, -1, 17}, {27, 1, 17}, {27, -1, 17},

	// 0000 0000 0010 xxxs x
	{0, 40, 16}, {0, 40, 16}, {0, -40, 16}, {0, -40, 16},
	{0, 39, 16}, {0, 39, 16}, {0, -39, 16}, {0, -39, 16},
	{0, 38, 16}, {0, 38, 16}, {0, -38, 16}, {0, -38, 16},
	{0, 37, 16}, {0, 37, 16}, {0, -37, 16}, {0, -37, 16},
	{0, 36, 16}, {0, 36, 16}, {0, -36, 16}, {0, -36, 16},
	{0, 35, 16}, {0, 35, 16}, {0, -35, 16}, {0, -35, 16},
	{0, 34, 16}, {0, 34, 16}, {0, -34, 16}, {0, -34, 16},
	{0, 33, 16}, {0, 33, 16}, {0, -33, 16}, {0, -33, 16},

	// 0000 0000 0011 xxxs x
	{0, 32, 16}, {0, 32, 16}, {0, -32, 16}, {0, -32, 16},
	{1, 14, 16}, {1, 14, 16}, {1, -14, 16}, {1, -14, 16},
	{1, 13, 16}, {1, 13, 16}, {1, -13, 16}, {1, -13, 16},
	{1, 12, 16}, {1, 12, 16}, {1, -12, 16}, {1, -12, 16},
	{1, 11, 16}, {1, 11, 16}, {1, -11, 16}, {1, -11, 16},
	{1, 10, 16}, {1, 10, 16}, {1, -10, 16}, {1, -10, 16},
	{1, 9, 16}, {1, 9, 16}, {1, -9, 16}, {1, -9, 16},
	{1, 8, 16}, {1, 8, 16}, {1, -8, 16}, {1, -8, 16},

	// 0000 0000 0100 xxsx x
	{0, 31, 15}, {0, 31, 15}, {0, 31, 15}, {0, 31, 15},
	{0, -31, 15}, {0, -31, 15}, {0, -31, 15}, {0, -31, 15},
	{0, 30, 15}, {0, 30, 15}, {0, 30, 15}, {0, 30, 15},
	{0, -30, 15}, {0, -30, 15}, {0, -30, 15}, {0, -30, 15},
	{0, 29, 15}, {0, 29, 15}, {0, 29, 15}, {0, 29, 15},
	{0, -29, 15}, {0, -29, 15}, {0, -29, 15}, {0, -29, 15},
	{0, 28, 15}, {0, 28, 15}, {0, 28, 15}, {0, 28, 15},
	{0, -28, 15}, {0, -28, 15}, {0, -28, 15}, {0, -28, 15},

	// 0000 0000 0101 xxsx x
	{0, 27, 15}, {0, 27, 15}, {0, 27, 15}, {0, 27, 15},
	{0, -27, 15}, {0, -27, 15}, {0, -27, 15}, {0, -27, 15},
	{0, 26, 15}, {0, 26, 15}, {0, 26, 15}, {0, 26, 15},
	{0, -26, 15}, {0, -26, 15}, {0, -26, 15}, {0, -26, 15},
	{0, 25, 15}, {0, 25, 15}, {0, 25, 15}, {0, 25, 15},
	{0, -25, 15}, {0, -25, 15}, {0, -25, 15}, {0, -25, 15},
	{0, 24, 15}, {0, 24, 15}, {0, 24, 15}, {0, 24, 15},
	{0, -24, 15}, {0, -24, 15}, {0, -24, 15}, {0, -24, 15},

	// 0000 0000 0110 xxsx x
	{0, 23, 15}, {0, 23, 15}, {0, 23, 15}, {0, 23, 15},
	{0, -23, 15}, {0, -23, 15}, {0, -23, 15}, {0, -23, 15},
	{0, 22, 15}, {0, 22, 15}, {0, 22, 15}, {0, 22, 15},
	{0, -22, 15}, {0, -22, 15}, {0, -22, 15}, {0, -22, 15},
	{0, 21, 15}, {0, 21, 15}, {0, 21, 15}, {0, 21, 15},
	{0, -21, 15}, {0, -21, 15}, {0, -21, 15}, {0, -21, 15},
	{0, 20, 15}, {0, 20, 15}, {0, 20, 15}, {0, 20, 15},
	{0, -20, 15}, {0, -20, 15}, {0, -20, 15}, {0, -20, 15},

	// 0000 0000 0111 xxsx x
	{0, 19, 15}, {0, 19, 15}, {0, 19, 15}, {0, 19, 15},
	{0, -19, 15}, {0, -19, 15}, {0, -19, 15}, {0, -19, 15},
	{0, 18, 15}, {0, 18, 15}, {0, 18, 15}, {0, 18, 15},
	{0, -18, 15}, {0, -18, 15}, {0, -18, 15}, {0, -18, 15},
	{0, 17, 15}, {0, 17, 15}, {0, 17, 15}, {0, 17, 15},
	{0, -17, 15}, {0, -17, 15}, {0, -17, 15}, {0, -17, 15},
	{0, 16, 15}, {0, 16, 15}, {0, 16, 15}, {0, 16, 15},
	{0, -16, 15}, {0, -16, 15}, {0, -16, 15}, {0, -16, 15},

	// 0000 0000 1000 xsxx x
	{10, 2, 14}, {10, 2, 14}, {10, 2, 14}, {10, 2, 14},
	{10, 2, 14}, {10, 2, 14}, {10, 2, 14}, {10, 2, 14},
	{10, -2, 14}, {10, -2, 14}, {10, -2, 14}, {10, -2, 14},
	{10, -2, 14}, {10, -2, 14}, {10, -2, 14}, {10, -2, 14},
	{9, 2, 14}, {9, 2, 14}, {9, 2, 14}, {9, 2, 14},
	{9, 2, 14}, {9, 2, 14}, {9, 2, 14}, {9, 2, 14},
	{9, -2, 14}, {9, -2, 14}, {9, -2, 14}, {9, -2, 14},
	{9, -2, 14}, {9, -2, 14}, {9, -2, 14}, {9, -2, 14},

	// 0000 0000 1001 xsxx x
	{5, 3, 14}, {5, 3, 14}, {5, 3, 14}, {5, 3, 14},
	{5, 3, 14}, {5, 3, 14}, {5, 3, 14}, {5, 3, 14},
	{5, -3, 14}, {5, -3, 14}, {5, -3, 14}, {5, -3, 14},
	{5, -3, 14}, {5, -3, 14}, {5, -3, 14}, {5, -3, 14},
	{3, 4, 14}, {3, 4, 14}, {3, 4, 14}, {3, 4, 14},
	{3, 4, 14}, {3, 4, 14}, {3, 4, 14}, {3, 4, 14},
	{3, -4, 14}, {3, -4, 14}, {3, -4, 14}, {3, -4, 14},
	{3, -4, 14}, {3, -4, 14}, {3, -4, 14}, {3, -4, 14},

	// 0000 0000 1010 xsxx x
	{2, 5, 14}, {2, 5, 14}, {2, 5, 14}, {2, 5, 14},
	{2, 5, 14}, {2, 5, 14}, {2, 5, 14}, {2, 5, 14},
	{2, -5, 14}, {2, -5, 14}, {2, -5, 14}, {2, -5, 14},
	{2, -5, 14}, {2, -5, 14}, {2, -5, 14}, {2, -5, 14},
	{1, 7, 14}, {1, 7, 14}, {1, 7, 14}, {1, 7, 14},
	{1, 7, 14}, {1, 7, 14}, {1, 7, 14}, {1, 7, 14},
	{1, -7, 14}, {1, -7, 14}, {1, -7, 14}, {1, -7, 14},
	{1, -7, 14}, {1, -7, 14}, {1, -7, 14}, {1, -7, 14},

	// 0000 0000 1011 xsxx x
	{1, 6, 14}, {1, 6, 14}, {1, 6, 14}, {1, 6, 14},
	{1, 6, 14}, {1, 6, 14}, {1, 6, 14}, {1, 6, 14},
	{1, -6, 14}, {1, -6, 14}, {1, -6, 14}, {1, -6, 14},
	{1, -6, 14}, {1, -6, 14}, {1, -6, 14}, {1, -6, 14},
	{0, 15, 14}, {0, 15, 14}, {0, 15, 14}, {0, 15, 14},
	{0, 15, 14}, {0, 15, 14}, {0, 15, 14}, {0, 15, 14},
	{0, -15, 14}, {0, -15, 14}, {0, -15, 14}, {0, -15, 14},
	{0, -15, 14}, {0, -15, 14}, {0, -15, 14}, {0, -15, 14},

	// 0000 0000 1100 xsxx x
	{0, 14, 14}, {0, 14, 14}, {0, 14, 14}, {0, 14, 14},
	{0, 14, 14}, {0, 14, 14}, {0, 14, 14}, {0, 14, 14},
	{0, -14, 14}, {0, -14, 14}, {0, -14, 14}, {0, -14, 14},
	{0, -14, 14}, {0, -14, 14}, {0, -14, 14}, {0, -14, 14},
	{0, 13, 14}, {0, 13, 14}, {0, 13, 14}, {0, 13, 14},
	{0, 13, 14}, {0, 13, 14}, {0, 13, 14}, {0, 13, 14},
	{0, -13, 14}, {0, -13, 14}, {0, -13, 14}, {0, -13, 14},
	{0, -13, 14}, {0, -13, 14}, {0, -13, 14}, {0, -13, 14},

	// 0000 0000 1101 xsxx x
	{0, 12, 14}, {0, 12, 14}, {0, 12, 14}, {0, 12, 14},
	{0, 12, 14}, {0, 12, 14}, {0, 12, 14}, {0, 12, 14},
	{0, -12, 14}, {0, -12, 14}, {0, -12, 14}, {0, -12, 14},
	{0, -12, 14}, {0, -12, 14}, {0, -12, 14}, {0, -12, 14},
	{26, 1, 14}, {26, 1, 14}, {26, 1, 14}, {26, 1, 14},
	{26, 1, 14}, {26, 1, 14}, {26, 1, 14}, {26, 1, 14},
	{26, -1, 14}, {26, -1, 14}, {26, -1, 14}, {26, -1, 14},
	{26, -1, 14}, {26, -1, 14}, {26, -1, 14}, {26, -1, 14},

	// 0000 0000 1110 xsxx x
	{25, 1, 14}, {25, 1, 14}, {25, 1, 14}, {25, 1, 14},
	{25, 1, 14}, {25, 1, 14}, {25, 1, 14}, {25, 1, 14},
	{25, -1, 14}, {25, -1, 14}, {25, -1, 14}, {25, -1, 14},
	{25, -1, 14}, {25, -1, 14}, {25, -1, 14}, {25, -1, 14},
	{24, 1, 14}, {24, 1, 14}, {24, 1, 14}, {24, 1, 14},
	{24, 1, 14}, {24, 1, 14}, {24, 1, 14}, {24, 1, 14},
	{24, -1, 14}, {24, -1, 14}, {24, -1, 14}, {24, -1, 14},
	{24, -1, 14}, {24, -1, 14}, {24, -1, 14}, {24, -1, 14},

	// 0000 0000 1111 xsxx x
	{23, 1, 14}, {23, 1, 14}, {23, 1, 14}, {23, 1, 14},
	{23, 1, 14}, {23, 1, 14}, {23, 1, 14}, {23, 1, 14},
	{23, -1, 14}, {23, -1, 14}, {23, -1, 14}, {23, -1, 14},
	{23, -1, 14}, {23, -1, 14}, {23, -1, 14}, {23, -1, 14},
	{22, 1, 14}, {22, 1, 14}, {22, 1, 14}, {22, 1, 14},
	{22, 1, 14}, {22, 1, 14}, {22, 1, 14}, {22, 1, 14},
	{22, -1, 14}, {22, -1, 14}, {22, -1, 14}, {22, -1, 14},
	{22, -1, 14}, {22, -1, 14}, {22, -1, 14}, {22, -1, 14}
};

// 0000 0001 xxxx s??? ?
const RunLevel s_dctCoefficients1[] =
{
	{0, 11, 13}, {0, -11, 13}, {8, 2, 13}, {8, -2, 13},
	{4, 3, 13}, {4, -3, 13}, {0, 10, 13}, {0, -10, 13},
	{2, 4, 13}, {2, -4, 13}, {7, 2, 13}, {7, -2, 13},
	{21, 1, 13}, {21, -1, 13}, {20, 1, 13}, {20, -1, 13},
	{0, 9, 13}, {0, -9, 13}, {19, 1, 13}, {19, -1, 13},
	{18, 1, 13}, {18, -1, 13}, {1, 5, 13}, {1, -5, 13},
	{3, 3, 13}, {3, -3, 13}, {0, 8, 13}, {0, -8, 13},
	{6, 2, 13}, {6, -2, 13}, {17, 1, 13}, {17, -1, 13}
};

// 0000 0010 xxs? ???? ?
const RunLevel s_dctCoefficients2[] =
{
	{16, 1, 11}, {16, -1, 11}, {5, 2, 11}, {5, -2, 11},
	{0, 7, 11}, {0, -7, 11}, {2, 3, 11}, {2, -3, 11}
};

// 0000 0011 xxs? ???? ?
const RunLevel s_dctCoefficients3[] =
{
	{1, 4, 11}, {1, -4, 11}, {15, 1, 11}, {15, -1, 11},
	{14, 1, 11}, {14, -1, 11}, {4, 2, 11}, {4, -2, 11}
};

// 0000 xxxs ???? ???? ?
const RunLevel s_dctCoefficients4[] =
{
	{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
	{0, 0, 6}, {0, 0, 6}, {0, 0, 6}, {0, 0, 6}, // ESCAPE
	{2, 2, 8}, {2, -2, 8}, {9, 1, 8}, {9, -1, 8},
	{0, 4, 8}, {0, -4, 8}, {8, 1, 8}, {8, -1, 8}
};

// 0001 xxs? ???? ???? ?
const RunLevel dctCoefficientsA[] =
{
	{7, 1, 7}, {7, -1, 7}, {6, 1, 7}, {6, -1, 7},
	{1, 2, 7}, {1, -2, 7}, {5, 1, 7}, {5, -1, 7}
};

// 0010 xxxx s??? ???? ?
const RunLevel dctCoefficientsB[] =
{
	{13, 1, 9}, {13, -1, 9}, {0, 6, 9}, {0, -6, 9}, {12, 1, 9}, {12, -1, 9}, {11, 1, 9}, {11, -1, 9},
	{3, 2, 9}, {3, -2, 9}, {1, 3, 9}, {1, -3, 9}, {0, 5, 9}, {0, -5, 9}, {10, 1, 9}, {10, -1, 9},
	{0, 3, 6}, {0, 3, 6}, {0, 3, 6}, {0, 3, 6}, {0, 3, 6}, {0, 3, 6}, {0, 3, 6}, {0, 3, 6},
	{0, -3, 6}, {0, -3, 6}, {0, -3, 6}, {0, -3, 6}, {0, -3, 6}, {0, -3, 6}, {0, -3, 6}, {0, -3, 6},
};

// 0011 xs?? ???? ???? ?
const RunLevel dctCoefficientsC[] =
{
	{4, 1, 6}, {4, -1, 6}, {3, 1, 6}, {3, -1, 6}
};

RunLevel Mpeg1VideoDecoder::decodeDCTCoeff(const bool first) {
	const int value = m_bitStream.peek(17);
	int index = (value >> 5);
	RunLevel result;
	if (index >= 0x1 && index <= 0xf) {
		// 1.9-4.7% of lookups on the test files
		int offset = (index << 5); // Multiply by 32, the size of each original array
		offset += (value & 0x1f); // Get 5 least significant bits, which determine the offset within each array
		result = s_dctCoefficients[offset];
	} else if (index >= 0x10 && index <= 0x1f) {
		// 2.2-3.9%
		// Discard 4 least significant bits
		const int offset = (value >> 4) & 0x1f;
		result = s_dctCoefficients1[offset];
	} else if (index >= 0x20 && index <= 0x2f) {
		// 1.2-1.7%
		// Discard 6 least significant bits
		const int offset = (value >> 6) & 0x7;
		result = s_dctCoefficients2[offset];
	} else if (index >= 0x30 && index <= 0x3f) {
		// 0.5-1.5%
		// Discard 6 least significant bits
		const int offset = (value >> 6) & 0x7;
		result = s_dctCoefficients3[offset];
	} else if (index >= 0x40 && index <= 0xff) {
		// 5-7%
		// Discard 9 least significant bits
		const int offset = (value >> 9) & 0xf;
		result = s_dctCoefficients4[offset];
		m_bitStream.skip(result.discard);

		// !!!!!!!!!!!! ESCAPE !!!!!!!!!!!!!!!!!!
		if (offset >= 4 && offset <= 7) {
			result.run = m_bitStream.pull(6);
			result.level = m_bitStream.pull(8);

			if (result.level == 0) {
				// positive < 256
				result.level = m_bitStream.pull(8);
			} else if (result.level == 0x80) {
				// negative > -256
				result.level = ((int16_t) m_bitStream.pull(8)) - 256;
			} else if (result.level > 128) {
				result.level -= 256; // signed 8 bit
			}
		}
		return result;
	} else {
		index = (value >> 13) & 0xf;

		if (index == 0x1) {
			int offset = (value >> 10) & 0x7;
			result = dctCoefficientsA[offset];
		} else if (index == 0x2) {
			int offset = (value >> 8) & 0x1f;
			result = dctCoefficientsB[offset];
		} else if (index == 0x3) {
			int offset = (value >> 11) & 0x3;
			result = dctCoefficientsC[offset];
		} else if (index == 0x4) {
			result = {0, ((value & 0x1000) != 0 ? (int16_t) -2 : (int16_t) 2), 5};
		} else if (index == 0x5) {
			result = {2, ((value & 0x1000) != 0 ? (int16_t) -1 : (int16_t) 1), 5};
		} else if (index == 0x6) {
			result = {1, 1, 4};
		} else if (index == 0x7) {
			result = {1, -1, 4};
		}
		// Only for CoeffFirst
		else if (first && index >= 0x8 && index <= 0xb) {
			result = {0, 1, 2};
		} else if (first && index >= 0xc && index <= 0xf) {
			result = {0, -1, 2};
		}
		// Only for CoeffNext
		else if (index >= 0xc && index <= 0xd) {
			result = {0, 1, 3};
		} else if (index >= 0xe && index <= 0xf) {
			result = {0, -1, 3};
		} else {
			throw std::runtime_error("Corrupt video data: invalid DCT coefficient code");
			//return; // false
		}
	}

	m_bitStream.skip(result.discard);

	return result; // { run, level };
}
