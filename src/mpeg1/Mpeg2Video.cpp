// MPEG-2 video (ISO/IEC 13818-2) main profile decoding: the parts that differ from MPEG-1. 4:2:0 only.
// Tables are from the standard (Table B-15, the alternate scan, the non-linear quantiser scale).

#include "Mpeg1VideoDecoder.h"

#if MD_MPEG_ENABLE_MPEG2

#include "VideoInternal.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace {
	// Alternate scan, by scan position, giving the raster position
	const uint8_t ALTERNATE_SCAN[64] = {
		 0,  8, 16, 24,  1,  9,  2, 10,
		17, 25, 32, 40, 48, 56, 57, 49,
		41, 33, 26, 18,  3, 11,  4, 12,
		19, 27, 34, 42, 50, 58, 35, 43,
		51, 59, 20, 28,  5, 13,  6, 14,
		21, 29, 36, 44, 52, 60, 37, 45,
		53, 61, 22, 30,  7, 15, 23, 31,
		38, 46, 54, 62, 39, 47, 55, 63
	};

	// quantiser_scale by quantiser_scale_code when q_scale_type is 1
	const uint8_t NON_LINEAR_QUANTISER_SCALE[32] = {
		 0,  1,  2,  3,  4,  5,   6,   7,
		 8, 10, 12, 14, 16, 18,  20,  22,
		24, 28, 32, 36, 40, 44,  48,  52,
		56, 64, 72, 80, 88, 96, 104, 112
	};

	// Table B-15, the intra_vlc_format 1 DCT coefficients: code, length (without the sign bit), run, level
	struct VlcCode {
		uint16_t code;
		uint8_t length;
		uint8_t run;
		uint8_t level;
	};

	constexpr VlcCode TABLE_B15[] = {
	{0x02, 2, 0, 1},
	{0x06, 3, 0, 2},
	{0x07, 4, 0, 3},
	{0x1c, 5, 0, 4},
	{0x1d, 5, 0, 5},
	{0x05, 6, 0, 6},
	{0x04, 6, 0, 7},
	{0x7b, 7, 0, 8},
	{0x7c, 7, 0, 9},
	{0x23, 8, 0, 10},
	{0x22, 8, 0, 11},
	{0xfa, 8, 0, 12},
	{0xfb, 8, 0, 13},
	{0xfe, 8, 0, 14},
	{0xff, 8, 0, 15},
	{0x1f, 14, 0, 16},
	{0x1e, 14, 0, 17},
	{0x1d, 14, 0, 18},
	{0x1c, 14, 0, 19},
	{0x1b, 14, 0, 20},
	{0x1a, 14, 0, 21},
	{0x19, 14, 0, 22},
	{0x18, 14, 0, 23},
	{0x17, 14, 0, 24},
	{0x16, 14, 0, 25},
	{0x15, 14, 0, 26},
	{0x14, 14, 0, 27},
	{0x13, 14, 0, 28},
	{0x12, 14, 0, 29},
	{0x11, 14, 0, 30},
	{0x10, 14, 0, 31},
	{0x18, 15, 0, 32},
	{0x17, 15, 0, 33},
	{0x16, 15, 0, 34},
	{0x15, 15, 0, 35},
	{0x14, 15, 0, 36},
	{0x13, 15, 0, 37},
	{0x12, 15, 0, 38},
	{0x11, 15, 0, 39},
	{0x10, 15, 0, 40},
	{0x02, 3, 1, 1},
	{0x06, 5, 1, 2},
	{0x79, 7, 1, 3},
	{0x27, 8, 1, 4},
	{0x20, 8, 1, 5},
	{0x16, 13, 1, 6},
	{0x15, 13, 1, 7},
	{0x1f, 15, 1, 8},
	{0x1e, 15, 1, 9},
	{0x1d, 15, 1, 10},
	{0x1c, 15, 1, 11},
	{0x1b, 15, 1, 12},
	{0x1a, 15, 1, 13},
	{0x19, 15, 1, 14},
	{0x13, 16, 1, 15},
	{0x12, 16, 1, 16},
	{0x11, 16, 1, 17},
	{0x10, 16, 1, 18},
	{0x05, 5, 2, 1},
	{0x07, 7, 2, 2},
	{0xfc, 8, 2, 3},
	{0x0c, 10, 2, 4},
	{0x14, 13, 2, 5},
	{0x07, 5, 3, 1},
	{0x26, 8, 3, 2},
	{0x1c, 12, 3, 3},
	{0x13, 13, 3, 4},
	{0x06, 6, 4, 1},
	{0xfd, 8, 4, 2},
	{0x12, 12, 4, 3},
	{0x07, 6, 5, 1},
	{0x04, 9, 5, 2},
	{0x12, 13, 5, 3},
	{0x06, 7, 6, 1},
	{0x1e, 12, 6, 2},
	{0x14, 16, 6, 3},
	{0x04, 7, 7, 1},
	{0x15, 12, 7, 2},
	{0x05, 7, 8, 1},
	{0x11, 12, 8, 2},
	{0x78, 7, 9, 1},
	{0x11, 13, 9, 2},
	{0x7a, 7, 10, 1},
	{0x10, 13, 10, 2},
	{0x21, 8, 11, 1},
	{0x1a, 16, 11, 2},
	{0x25, 8, 12, 1},
	{0x19, 16, 12, 2},
	{0x24, 8, 13, 1},
	{0x18, 16, 13, 2},
	{0x05, 9, 14, 1},
	{0x17, 16, 14, 2},
	{0x07, 9, 15, 1},
	{0x16, 16, 15, 2},
	{0x0d, 10, 16, 1},
	{0x15, 16, 16, 2},
	{0x1f, 12, 17, 1},
	{0x1a, 12, 18, 1},
	{0x19, 12, 19, 1},
	{0x17, 12, 20, 1},
	{0x16, 12, 21, 1},
	{0x1f, 13, 22, 1},
	{0x1e, 13, 23, 1},
	{0x1d, 13, 24, 1},
	{0x1c, 13, 25, 1},
	{0x1b, 13, 26, 1},
	{0x1f, 16, 27, 1},
	{0x1e, 16, 28, 1},
	{0x1d, 16, 29, 1},
	{0x1c, 16, 30, 1},
	{0x1b, 16, 31, 1}
	};

	constexpr uint8_t RUN_ESCAPE = 64;
	constexpr uint8_t RUN_END_OF_BLOCK = 65;

	// Decoding entry; length 0 = invalid code, or a code longer than 8 bits for the second level
	struct VlcEntry {
		uint8_t run;
		uint8_t level;
		uint8_t length;
	};

	// Codes up to 8 bits by their first 8 bits; the longer ones all start 0000 00xx, and are found by those
	// 2 bits and the next 8
	struct IntraVlc1Tables {
		VlcEntry first[256];
		VlcEntry second[4][256];
	};

	constexpr void addCode(IntraVlc1Tables &tables, const uint32_t code, const uint32_t length, const VlcEntry entry) {
		if (length <= 8) {
			const uint32_t base = code << (8 - length);
			for (uint32_t i = 0; i < 1u << (8 - length); i++) {
				tables.first[base + i] = entry;
			}
		} else {
			const uint32_t base = code << (16 - length);
			for (uint32_t i = 0; i < 1u << (16 - length); i++) {
				tables.second[(base + i) >> 8][(base + i) & 255] = entry;
			}
		}
	}

	constexpr IntraVlc1Tables buildIntraVlc1Tables() {
		IntraVlc1Tables tables{};
		for (const auto &code : TABLE_B15) {
			addCode(tables, code.code, code.length, {code.run, code.level, code.length});
		}
		addCode(tables, 0x01, 6, {RUN_ESCAPE, 0, 6});
		addCode(tables, 0x06, 4, {RUN_END_OF_BLOCK, 0, 4});
		return tables;
	}

	constexpr IntraVlc1Tables INTRA_VLC1 = buildIntraVlc1Tables();

	// Reads dmvector: 0, '10' 1, '11' -1
	int32_t pullDmVector(Mpeg1VideoDecoder::BitStream &bitStream) {
		if (!bitStream.pull(1)) {
			return 0;
		}
		return bitStream.pull(1) ? -1 : 1;
	}

	// (value * m) // 2, rounding half away from zero, as dual prime vectors are scaled
	int32_t scaleDualPrime(const int32_t value, const int32_t m) {
		return (value * m + (value > 0)) >> 1;
	}

	// Predicts a w x h block at (x, y) of a plane view (stride, width, height) from the reference view, motion
	// (mvx, mvy) in half pels. Blocks reaching outside the reference repeat its edge pixels.
	template <bool AVERAGE>
	void predictBlock(uint8_t *dst, const uint8_t *src, const uint32_t stride, const int32_t width,
	                  const int32_t height, const int32_t x, const int32_t y, const uint32_t w, const uint32_t h,
	                  const int32_t mvx, const int32_t mvy) {
		const int32_t sx = x + (mvx >> 1);
		const int32_t sy = y + (mvy >> 1);
		const int32_t hx = mvx & 1;
		const int32_t hy = mvy & 1;
		uint8_t *d = dst + y * stride + x;
		if (sx >= 0 && sy >= 0 && sx + static_cast<int32_t>(w) + hx <= width && sy + static_cast<int32_t>(h) + hy <= height) {
			const uint8_t *s = src + sy * stride + sx;
			const uint32_t halfPel = (hx << 1) | hy;
			if (w == 16) {
				videoPredictBlockHalfPel<16, AVERAGE>(d, s, stride, halfPel, h);
			} else {
				videoPredictBlockHalfPel<8, AVERAGE>(d, s, stride, halfPel, h);
			}
			return;
		}
		// Outside the reference: only from corrupt data or encoders that rely on edge extension
		const auto pixel = [&](const int32_t px, const int32_t py) -> uint32_t {
			return src[std::clamp(py, 0, height - 1) * stride + std::clamp(px, 0, width - 1)];
		};
		for (uint32_t row = 0; row < h; row++) {
			for (uint32_t column = 0; column < w; column++) {
				const int32_t px = sx + static_cast<int32_t>(column);
				const int32_t py = sy + static_cast<int32_t>(row);
				uint32_t value = pixel(px, py);
				if (hx && hy) {
					value = (value + pixel(px + 1, py) + pixel(px, py + 1) + pixel(px + 1, py + 1) + 2) >> 2;
				} else if (hx) {
					value = (value + pixel(px + 1, py) + 1) >> 1;
				} else if (hy) {
					value = (value + pixel(px, py + 1) + 1) >> 1;
				}
				uint8_t &out = d[row * stride + column];
				out = AVERAGE ? (out + value + 1) >> 1 : value;
			}
		}
	}

	// Writes an 8x8 block of samples: replacing what is there (intra), or added to the prediction
	void storeBlock(uint8_t *d, const uint32_t stride, int32_t *block, const bool intra, const bool dcOnly) {
		if (dcOnly) {
			const int32_t value = videoDcValue(block[0]);
			block[0] = 0;
			for (int y = 0; y < 8; y++, d += stride) {
				for (int x = 0; x < 8; x++) {
					d[x] = intra ? clampToByte(value) : clampToByte(d[x] + value);
				}
			}
			return;
		}
		video_idct(block);
		for (int y = 0; y < 8; y++, d += stride) {
			const int32_t *row = block + y * 8;
			for (int x = 0; x < 8; x++) {
				d[x] = intra ? clampToByte(row[x]) : clampToByte(d[x] + row[x]);
			}
		}
		std::memset(block, 0, 64 * sizeof(int32_t));
	}
}

void Mpeg1VideoDecoder::decodeSequenceExtension() {
	// 8:profile_and_level_indication / 1:progressive_sequence / 2:chroma_format
	m_bitStream.skip(8);
	m_progressiveSequence = m_bitStream.pull(1);
	if (m_bitStream.pull(2) != 1) {
		throw std::runtime_error("Only 4:2:0 MPEG-2 video is supported");
	}
	// 2:horizontal_size_extension / 2:vertical_size_extension / 12:bit_rate_extension / 1:marker_bit
	m_width |= m_bitStream.pull(2) << 12;
	m_height |= m_bitStream.pull(2) << 12;
	const uint32_t bitRateExtension = m_bitStream.pull(12);
	if (bit_rate != 0) {
		bit_rate += bitRateExtension * (400u << 18);
	}
	m_bitStream.skip(1);
	// 8:vbv_buffer_size_extension / 1:low_delay / 2:frame_rate_extension_n / 5:frame_rate_extension_d
	vbv_buffer_size += m_bitStream.pull(8) << 11;
	m_bitStream.skip(1);
	m_frameRateExtN = m_bitStream.pull(2);
	m_frameRateExtD = m_bitStream.pull(5);
	m_mpeg2 = true;
}

void Mpeg1VideoDecoder::decodeQuantMatrixExtension() {
	// Always in the default zig-zag order. The chroma matrices that follow are only for 4:2:2 and 4:4:4.
	if (m_bitStream.pull(1)) {
		for (int i = 0; i < 64; i++) {
			intra_quant_matrix[VIDEO_ZIG_ZAG[i]] = m_bitStream.pull(8);
		}
	}
	if (m_bitStream.pull(1)) {
		for (int i = 0; i < 64; i++) {
			non_intra_quant_matrix[VIDEO_ZIG_ZAG[i]] = m_bitStream.pull(8);
		}
	}
}

void Mpeg1VideoDecoder::decodePictureCodingExtension() {
	for (auto &fCodes : m_fCode) {
		for (auto &fCode : fCodes) {
			fCode = m_bitStream.pull(4);
		}
	}
	m_intraDcPrecision = m_bitStream.pull(2);
	m_pictureStructure = m_bitStream.pull(2);
	if (m_pictureStructure == 0) {
		throw std::runtime_error("Reserved picture_structure");
	}
	m_topFieldFirst = m_bitStream.pull(1);
	m_framePredFrameDct = m_bitStream.pull(1);
	m_concealmentMotionVectors = m_bitStream.pull(1);
	m_qScaleType = m_bitStream.pull(1);
	m_intraVlcFormat = m_bitStream.pull(1);
	m_alternateScan = m_bitStream.pull(1);
	m_repeatFirstField = m_bitStream.pull(1);
	m_bitStream.skip(1); // chroma_420_type
	m_progressiveFrame = m_bitStream.pull(1);
	// composite_display_flag and its fields are skipped with the rest of the extension
}

void Mpeg1VideoDecoder::processSliceMpeg2(const uint32_t slice) {
	if (m_macroblockWidth == 0) {
		// No sequence header yet
		throw std::runtime_error("Slice before a sequence header");
	}
	const uint32_t mbRows = m_pictureStructure == FRAME_PICTURE ? m_macroblockHeight : m_macroblockHeight / 2;
	if (slice == 0 || slice > mbRows) {
		throw std::runtime_error("Slice position out of range");
	}
	macroblock_address = (slice - 1) * m_macroblockWidth - 1;

	std::memset(m_pmv, 0, sizeof(m_pmv));
	dc_predictor[0] = dc_predictor[1] = dc_predictor[2] = 1 << (7 + m_intraDcPrecision);

	// 5:quantiser_scale_code, then the intra slice flags and extra information, read as MPEG-1's extra
	// information: the first extra_bit_slice is followed by intra_slice_flag, intra_slice and 7 reserved bits
	uint32_t isExtraSlice;
	{
		const uint32_t t = m_bitStream.pull(6);
		quantizer_scale = t >> 1;
		isExtraSlice = t & 1;
	}
	while (isExtraSlice) {
		isExtraSlice = m_bitStream.pull(9) >> 8;
	}

	processMacroblockMpeg2(true, mbRows);
	while (macroblock_address < m_macroblockWidth * mbRows - 1 && m_bitStream.peek(23)) {
		processMacroblockMpeg2(false, mbRows);
	}
}

void Mpeg1VideoDecoder::processMacroblockMpeg2(const bool slice_begin, const uint32_t mbRows) {
	const bool framePicture = m_pictureStructure == FRAME_PICTURE;
	const uint32_t parity = m_pictureStructure == BOTTOM_FIELD ? 1 : 0;
	const uint32_t mbCount = m_macroblockWidth * mbRows;

	uint32_t macroblock_address_increment = 0;
	auto t = pullMacroblockAddressIncrement();
	while (t == 34) {
		t = pullMacroblockAddressIncrement();
	}
	while (t == 35) {
		macroblock_address_increment += 33;
		t = pullMacroblockAddressIncrement();
	}
	macroblock_address_increment += t;

	if (slice_begin) {
		macroblock_address += macroblock_address_increment;
	} else {
		if (macroblock_address + macroblock_address_increment >= mbCount) {
			throw std::runtime_error("Macroblock address out of range");
		}
		if (macroblock_address_increment > 1) {
			dc_predictor[0] = dc_predictor[1] = dc_predictor[2] = 1 << (7 + m_intraDcPrecision);
			if (picture_coding_type == VideoPictureType_PREDICTIVE) {
				// Skipped in a P-picture: zero motion from the same parity, motion vector predictors reset
				std::memset(m_pmv, 0, sizeof(m_pmv));
				std::memset(m_mv, 0, sizeof(m_mv));
				macroblock_motion_forward = true;
				macroblock_motion_backward = false;
				m_motionType = framePicture ? 2 : 1;
				m_fieldSelect[0][0] = parity;
			}
			// Skipped in a B-picture: the previous macroblock's prediction, vectors and predictors are kept
		}
		while (macroblock_address_increment > 1) {
			macroblock_address++;
			mb_row = macroblock_address / m_macroblockWidth;
			mb_col = macroblock_address % m_macroblockWidth;
			predictMacroblockMpeg2();
			macroblock_address_increment--;
		}
		macroblock_address++;
	}

	mb_row = macroblock_address / m_macroblockWidth;
	mb_col = macroblock_address % m_macroblockWidth;
	if (mb_col >= m_macroblockWidth || mb_row >= mbRows) {
		throw std::runtime_error("Corrupt stream");
	}

	decodeMacroblockType(false);

	// macroblock_modes
	if (macroblock_motion_forward || macroblock_motion_backward) {
		m_motionType = framePicture && m_framePredFrameDct ? 2 : m_bitStream.pull(2);
		if (m_motionType == 0) {
			throw std::runtime_error("Reserved motion type");
		}
		if (m_motionType == 3 && picture_coding_type != VideoPictureType_PREDICTIVE) {
			throw std::runtime_error("Dual prime outside a P-picture");
		}
	}
	m_fieldDct = framePicture && !m_framePredFrameDct && (macroblock_intra || macroblock_pattern)
		             ? m_bitStream.pull(1)
		             : false;
	if (macroblock_quant) {
		quantizer_scale = m_bitStream.pull(5);
	}

	if (macroblock_intra) {
		if (m_concealmentMotionVectors) {
			// Only for error concealment, which this decoder doesn't do, but they update the predictors
			m_motionType = framePicture ? 2 : 1;
			decodeMotionVectorsMpeg2(0);
			if (!m_bitStream.pull(1)) {
				throw std::runtime_error("Marker bit not 1");
			}
		} else {
			std::memset(m_pmv, 0, sizeof(m_pmv));
		}
	} else {
		dc_predictor[0] = dc_predictor[1] = dc_predictor[2] = 1 << (7 + m_intraDcPrecision);
		if (picture_coding_type == VideoPictureType_PREDICTIVE && !macroblock_motion_forward) {
			// No motion compensation: zero motion from the same parity, predictors reset
			std::memset(m_pmv, 0, sizeof(m_pmv));
			std::memset(m_mv, 0, sizeof(m_mv));
			macroblock_motion_forward = true;
			m_motionType = framePicture ? 2 : 1;
			m_fieldSelect[0][0] = parity;
		} else {
			if (macroblock_motion_forward) {
				decodeMotionVectorsMpeg2(0);
			}
			if (macroblock_motion_backward) {
				decodeMotionVectorsMpeg2(1);
			}
		}
		predictMacroblockMpeg2();
	}

	const uint32_t coded_block_pattern = macroblock_pattern
		                                     ? decodeCodedBlockPattern()
		                                     : (macroblock_intra ? 0x3f : 0);
	if (coded_block_pattern) {
		decodeBlocksMpeg2(coded_block_pattern);
	}
}

int32_t Mpeg1VideoDecoder::decodeMotionComponent(const uint32_t fCode, const int32_t prediction) {
	if (fCode == 0 || fCode > 9) {
		m_diagnostics.count(Diagnostic::InvalidHeader);
		throw std::runtime_error("Invalid f_code");
	}
	return updateMotionVector(static_cast<int32_t>(fCode) - 1, prediction);
}

void Mpeg1VideoDecoder::decodeMotionVectorsMpeg2(const uint32_t s) {
	const uint32_t fCodeH = m_fCode[s][0];
	const uint32_t fCodeV = m_fCode[s][1];
	if (m_pictureStructure == FRAME_PICTURE) {
		switch (m_motionType) {
			case 2: {
				// Frame prediction: one vector
				m_mv[0][s][0] = m_pmv[1][s][0] = m_pmv[0][s][0] = decodeMotionComponent(fCodeH, m_pmv[0][s][0]);
				m_mv[0][s][1] = m_pmv[1][s][1] = m_pmv[0][s][1] = decodeMotionComponent(fCodeV, m_pmv[0][s][1]);
				break;
			}
			case 1: {
				// Field prediction: a vector for each field, vertical in field lines, predicted from frame lines
				for (uint32_t r = 0; r < 2; r++) {
					m_fieldSelect[r][s] = m_bitStream.pull(1);
					m_mv[r][s][0] = m_pmv[r][s][0] = decodeMotionComponent(fCodeH, m_pmv[r][s][0]);
					m_mv[r][s][1] = decodeMotionComponent(fCodeV, m_pmv[r][s][1] >> 1);
					m_pmv[r][s][1] = m_mv[r][s][1] * 2;
				}
				break;
			}
			default: {
				// Dual prime
				const int32_t mx = decodeMotionComponent(fCodeH, m_pmv[0][s][0]);
				const int32_t dmx = pullDmVector(m_bitStream);
				const int32_t my = decodeMotionComponent(fCodeV, m_pmv[0][s][1] >> 1);
				const int32_t dmy = pullDmVector(m_bitStream);
				m_pmv[0][s][0] = m_pmv[1][s][0] = mx;
				m_pmv[0][s][1] = m_pmv[1][s][1] = my * 2;
				m_mv[0][s][0] = mx;
				m_mv[0][s][1] = my;
				// Each field is also predicted from the opposite parity field, with a scaled vector
				int32_t m = m_topFieldFirst ? 1 : 3;
				m_dmv[0][0] = scaleDualPrime(mx, m) + dmx;
				m_dmv[0][1] = scaleDualPrime(my, m) + dmy - 1;
				m = 4 - m;
				m_dmv[1][0] = scaleDualPrime(mx, m) + dmx;
				m_dmv[1][1] = scaleDualPrime(my, m) + dmy + 1;
				break;
			}
		}
		return;
	}

	switch (m_motionType) {
		case 1: {
			// Field prediction
			m_fieldSelect[0][s] = m_bitStream.pull(1);
			m_mv[0][s][0] = m_pmv[1][s][0] = m_pmv[0][s][0] = decodeMotionComponent(fCodeH, m_pmv[0][s][0]);
			m_mv[0][s][1] = m_pmv[1][s][1] = m_pmv[0][s][1] = decodeMotionComponent(fCodeV, m_pmv[0][s][1]);
			break;
		}
		case 2: {
			// 16x8: a vector for the upper and lower halves
			for (uint32_t r = 0; r < 2; r++) {
				m_fieldSelect[r][s] = m_bitStream.pull(1);
				m_mv[r][s][0] = m_pmv[r][s][0] = decodeMotionComponent(fCodeH, m_pmv[r][s][0]);
				m_mv[r][s][1] = m_pmv[r][s][1] = decodeMotionComponent(fCodeV, m_pmv[r][s][1]);
			}
			break;
		}
		default: {
			// Dual prime
			const int32_t mx = decodeMotionComponent(fCodeH, m_pmv[0][s][0]);
			const int32_t dmx = pullDmVector(m_bitStream);
			const int32_t my = decodeMotionComponent(fCodeV, m_pmv[0][s][1]);
			const int32_t dmy = pullDmVector(m_bitStream);
			m_pmv[0][s][0] = m_pmv[1][s][0] = mx;
			m_pmv[0][s][1] = m_pmv[1][s][1] = my;
			m_mv[0][s][0] = mx;
			m_mv[0][s][1] = my;
			m_dmv[0][0] = scaleDualPrime(mx, 1) + dmx;
			m_dmv[0][1] = scaleDualPrime(my, 1) + dmy + (m_pictureStructure == TOP_FIELD ? -1 : 1);
			break;
		}
	}
}

const Frame *Mpeg1VideoDecoder::referenceFrame(const uint32_t s, const uint32_t fieldSelect) const {
	if (s == 1) {
		return m_ptrFrameFuture;
	}
	// The second field of a P frame predicts from the frame's first field, which has the opposite parity
	const uint32_t parity = m_pictureStructure == BOTTOM_FIELD ? 1 : 0;
	if (m_secondField && fieldSelect != parity && picture_coding_type == VideoPictureType_PREDICTIVE) {
		return m_ptrFrameCurrent;
	}
	return m_ptrFramePast;
}

void Mpeg1VideoDecoder::predictPart(const Frame *ref, const int srcField, const int dstField, const int x, const int y,
                                    const uint32_t h, const int mvx, const int mvy, const bool average) const {
	const Frame *current = m_ptrFrameCurrent;
	const bool field = dstField >= 0;
	const auto plane = [&](uint8_t *dst, const uint8_t *src, const uint32_t width, const uint32_t height,
	                       const uint32_t w, const int px, const int py, const uint32_t ph, const int vx, const int vy) {
		const uint32_t stride = field ? width * 2 : width;
		const int32_t viewHeight = static_cast<int32_t>(field ? height / 2 : height);
		uint8_t *d = dst + (dstField > 0 ? width : 0);
		const uint8_t *s = src + (srcField > 0 ? width : 0);
		if (average) {
			predictBlock<true>(d, s, stride, static_cast<int32_t>(width), viewHeight, px, py, w, ph, vx, vy);
		} else {
			predictBlock<false>(d, s, stride, static_cast<int32_t>(width), viewHeight, px, py, w, ph, vx, vy);
		}
	};
	const FrameSize &size = m_frameSize;
	plane(current->m_yData, ref->m_yData, size.luma_width, size.luma_height, 16, x, y, h, mvx, mvy);
	// Chroma vectors are half the luma ones, truncated towards zero
	plane(current->m_cbData, ref->m_cbData, size.chroma_width, size.chroma_height, 8, x / 2, y / 2, h / 2, mvx / 2, mvy / 2);
	plane(current->m_crData, ref->m_crData, size.chroma_width, size.chroma_height, 8, x / 2, y / 2, h / 2, mvx / 2, mvy / 2);
}

void Mpeg1VideoDecoder::predictMacroblockMpeg2() {
	const int x = static_cast<int>(mb_col) * 16;
	bool average = false;
	for (uint32_t s = 0; s < 2; s++) {
		if (!(s == 0 ? macroblock_motion_forward : macroblock_motion_backward)) {
			continue;
		}
		if (m_pictureStructure == FRAME_PICTURE) {
			const Frame *ref = s == 0 ? m_ptrFramePast : m_ptrFrameFuture;
			const int y = static_cast<int>(mb_row) * 16;
			switch (m_motionType) {
				case 2:
					predictPart(ref, -1, -1, x, y, 16, m_mv[0][s][0], m_mv[0][s][1], average);
					break;
				case 1:
					for (int f = 0; f < 2; f++) {
						predictPart(ref, m_fieldSelect[f][s], f, x, y / 2, 8, m_mv[f][s][0], m_mv[f][s][1], average);
					}
					break;
				default:
					for (int f = 0; f < 2; f++) {
						predictPart(ref, f, f, x, y / 2, 8, m_mv[0][s][0], m_mv[0][s][1], average);
						predictPart(ref, 1 - f, f, x, y / 2, 8, m_dmv[f][0], m_dmv[f][1], true);
					}
					break;
			}
		} else {
			const int parity = m_pictureStructure == BOTTOM_FIELD ? 1 : 0;
			const int y = static_cast<int>(mb_row) * 16;
			switch (m_motionType) {
				case 1: {
					const uint32_t select = m_fieldSelect[0][s];
					predictPart(referenceFrame(s, select), select, parity, x, y, 16, m_mv[0][s][0], m_mv[0][s][1], average);
					break;
				}
				case 2:
					for (uint32_t r = 0; r < 2; r++) {
						const uint32_t select = m_fieldSelect[r][s];
						predictPart(referenceFrame(s, select), select, parity, x, y + 8 * static_cast<int>(r), 8,
						            m_mv[r][s][0], m_mv[r][s][1], average);
					}
					break;
				default:
					predictPart(referenceFrame(0, parity), parity, parity, x, y, 16, m_mv[0][0][0], m_mv[0][0][1], average);
					predictPart(referenceFrame(0, 1 - parity), 1 - parity, parity, x, y, 16, m_dmv[0][0], m_dmv[0][1], true);
					break;
			}
		}
		average = true;
	}
}

RunLevel Mpeg1VideoDecoder::decodeDCTCoeffIntraVlc1() {
	const uint32_t bits = m_bitStream.peek(16);
	VlcEntry entry = INTRA_VLC1.first[bits >> 8];
	if (entry.length == 0 && (bits >> 8) < 4) {
		entry = INTRA_VLC1.second[bits >> 8][bits & 255];
	}
	if (entry.length == 0) {
		throw std::runtime_error("Corrupt video data: invalid DCT coefficient code");
	}
	m_bitStream.skip(entry.length);
	if (entry.run == RUN_END_OF_BLOCK) {
		return {RUN_END_OF_BLOCK, 0, 0};
	}
	if (entry.run == RUN_ESCAPE) {
		const uint32_t runLevel = m_bitStream.pull(18);
		const auto level = static_cast<int16_t>(static_cast<int32_t>(runLevel << 20) >> 20);
		if (level == 0 || level == -2048) {
			throw std::runtime_error("Corrupt video data: forbidden escape level");
		}
		return {static_cast<uint8_t>(runLevel >> 12), level, 0};
	}
	const int16_t level = m_bitStream.pull(1) ? -entry.level : entry.level;
	return {entry.run, level, 0};
}

void Mpeg1VideoDecoder::decodeBlocksMpeg2(uint32_t cbp) {
	const uint8_t *scan = m_alternateScan ? ALTERNATE_SCAN : VIDEO_ZIG_ZAG;
	const int32_t qscale = m_qScaleType ? NON_LINEAR_QUANTISER_SCALE[quantizer_scale & 31] : 2 * (quantizer_scale & 31);
	const bool framePicture = m_pictureStructure == FRAME_PICTURE;
	const uint32_t parity = m_pictureStructure == BOTTOM_FIELD ? 1 : 0;

	for (uint32_t i = 0; i < 6; i++) {
		cbp <<= 1;
		if (!(cbp & 0x40)) {
			continue;
		}
		const uint8_t *quantMatrix;
		uint32_t n;
		// Sum of the coefficients and the last one, for mismatch control
		int32_t sum;
		int32_t last = 0;

		const auto store = [&](const uint32_t position, int32_t value) {
			value = std::clamp(value, -2048, 2047);
			sum += value;
			if (position == 63) {
				last = value;
			}
			block_data[position] = videoPremultiply(value, position);
		};

		if (macroblock_intra) {
			const uint32_t plane = i > 3 ? i - 3 : 0;
			const uint32_t dctDcSize = i < 4 ? decodeDCTDCSizeLuminance() : decodeDCTDCSizeChrominance();
			int32_t dc = dc_predictor[plane];
			if (dctDcSize != 0) {
				const auto differential = static_cast<int32_t>(m_bitStream.pull(dctDcSize));
				dc += differential & (1 << (dctDcSize - 1)) ? differential : -(1 << dctDcSize) | (differential + 1);
			}
			dc_predictor[plane] = dc;
			// intra_dc_mult is 8 >> intra_dc_precision, then premultiplied for the IDCT
			sum = dc << (3 - m_intraDcPrecision);
			block_data[0] = sum << VIDEO_DC_PREMULTIPLY_SHIFT;
			n = 1;
			quantMatrix = intra_quant_matrix;

			for (;;) {
				RunLevel runLevel;
				if (m_intraVlcFormat) {
					runLevel = decodeDCTCoeffIntraVlc1();
					if (runLevel.run == RUN_END_OF_BLOCK) {
						break;
					}
				} else {
					if (m_bitStream.peek(2) == 2) {
						m_bitStream.skip(2);
						break;
					}
					runLevel = decodeDCTCoeff(false);
				}
				n += runLevel.run;
				if (n >= 64) {
					throw std::runtime_error("Invalid macroblock index");
				}
				const uint32_t position = scan[n++];
				// (2 * level * W * quantiser_scale) / 32, truncated towards zero
				const int32_t product = runLevel.level * quantMatrix[position] * qscale;
				store(position, product >= 0 ? product >> 4 : -(-product >> 4));
			}
		} else {
			quantMatrix = non_intra_quant_matrix;
			sum = 0;
			n = 0;
			bool first = true;
			for (;;) {
				if (!first && m_bitStream.peek(2) == 2) {
					m_bitStream.skip(2);
					break;
				}
				const RunLevel runLevel = decodeDCTCoeff(first);
				first = false;
				n += runLevel.run;
				if (n >= 64) {
					throw std::runtime_error("Invalid macroblock index");
				}
				const uint32_t position = scan[n++];
				// ((2 * level + sign) * W * quantiser_scale) / 32, truncated towards zero
				const int32_t level = runLevel.level;
				const int32_t product = (2 * level + (level > 0 ? 1 : -1)) * quantMatrix[position] * qscale;
				store(position, product >= 0 ? product >> 5 : -(-product >> 5));
			}
		}

		// Mismatch control: make the sum odd by toggling the last coefficient's lowest bit. For a block with
		// only a first (DC) coefficient the toggle is skipped: +-1 at (7, 7) is far too small to change any sample.
		const bool dcOnly = n == 1;
		if (!dcOnly && !(sum & 1)) {
			block_data[63] = videoPremultiply(last ^ 1, 63);
		}

		uint8_t *d;
		uint32_t stride;
		if (i < 4) {
			const uint32_t width = m_frameSize.luma_width;
			const uint32_t x = mb_col * 16 + (i & 1) * 8;
			if (!framePicture) {
				stride = width * 2;
				d = m_ptrFrameCurrent->m_yData + parity * width + (mb_row * 16 + (i & 2 ? 8 : 0)) * stride + x;
			} else if (m_fieldDct) {
				// Blocks 0 and 1 are the top field lines, 2 and 3 the bottom
				stride = width * 2;
				d = m_ptrFrameCurrent->m_yData + (mb_row * 16 + (i >> 1)) * width + x;
			} else {
				stride = width;
				d = m_ptrFrameCurrent->m_yData + (mb_row * 16 + (i & 2 ? 8 : 0)) * width + x;
			}
		} else {
			const uint32_t width = m_frameSize.chroma_width;
			uint8_t *planeData = i == 4 ? m_ptrFrameCurrent->m_cbData : m_ptrFrameCurrent->m_crData;
			if (framePicture) {
				stride = width;
				d = planeData + mb_row * 8 * width + mb_col * 8;
			} else {
				stride = width * 2;
				d = planeData + parity * width + mb_row * 8 * stride + mb_col * 8;
			}
		}
		storeBlock(d, stride, block_data, macroblock_intra, dcOnly);
	}
}

#endif
