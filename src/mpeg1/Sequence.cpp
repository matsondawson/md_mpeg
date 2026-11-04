// Portions adapted from PL_MPEG (MIT, Copyright (c) 2019 Dominic Szablewski), see THIRD_PARTY_NOTICES.md

#include <cstring>

#include "Mpeg1VideoDecoder.h"

#define MARKER_BIT \
	if (m_bitStream.pull(1) != 1) { \
		throw std::runtime_error("Marker bit not 1"); \
	}

static const float ASPECT_RATIOS[] = {
	0,
	1,
	0.6735f,
	0.7031f,
	0.7615f,
	0.8055f,
	0.8437f,
	0.8935f,
	0.9375f,
	0.9815f,
	1.0255f,
	1.0695f,
	1.1250f,
	1.1575f,
	1.2015f,
	0
};

static const float FRAME_RATES[] = {
	0,
	23.976f,
	24,
	25,
	29.97f,
	30,
	50,
	59.94f,
	60,
	0,
	0,
	0,
	0,
	0,
	0,
	0
};

static const uint8_t VIDEO_INTRA_QUANT_MATRIX[] = {
	 8, 16, 19, 22, 26, 27, 29, 34,
	16, 16, 22, 24, 27, 29, 34, 37,
	19, 22, 26, 27, 29, 34, 34, 38,
	22, 22, 26, 27, 29, 34, 37, 40,
	22, 26, 27, 29, 32, 35, 40, 48,
	26, 27, 29, 32, 35, 40, 48, 58,
	26, 27, 29, 34, 38, 46, 56, 69,
	27, 29, 35, 38, 46, 56, 69, 83
};


void Mpeg1VideoDecoder::decodeSequence() {
	m_width = m_bitStream.pull(12);
	m_height = m_bitStream.pull(12);
	aspect_ratio_enum = m_bitStream.pull(4);
	aspect_ratio = ASPECT_RATIOS[aspect_ratio_enum];
	frame_rate_enum = m_bitStream.pull(4);
	m_frame_rate = FRAME_RATES[frame_rate_enum];
	m_frame_period_millis = m_frame_rate != 0 ? 1000.0f / m_frame_rate : 0.0f;
	bit_rate = m_bitStream.pull(18);
	if (bit_rate == 0x3FFFF) {
		// variable bit rate
		bit_rate = 0;
	}
	else {
		bit_rate *= 400;
	}
	MARKER_BIT;
	vbv_buffer_size = m_bitStream.pull(10) * 2;
	m_bitStream.pull(1); // constrain parameters flag
	load_intra_matrix = m_bitStream.pull(1);
	if (load_intra_matrix) {
		for (int i = 0; i < 64; i++) {
			intra_quant_matrix[VIDEO_ZIG_ZAG[i]] = m_bitStream.pull(8);
		}
	}
	else {
		std::memcpy(intra_quant_matrix, VIDEO_INTRA_QUANT_MATRIX, 64);
	}

	load_non_intra_matrix = m_bitStream.pull(1);
	if (load_non_intra_matrix) {
		for (int i = 0; i < 64; i++) {
			non_intra_quant_matrix[VIDEO_ZIG_ZAG[i]] = m_bitStream.pull(8);
		}
	}
	else {
		memset(non_intra_quant_matrix, 16, 64);
	}

	m_macroblockWidth = (m_width + 15) >> 4;
	m_macroblockHeight = (m_height + 15) >> 4;
	mb_size = m_macroblockWidth * m_macroblockHeight;

	m_frameSize = {
		m_macroblockWidth << 4 ,m_macroblockHeight << 4,
		m_macroblockWidth << 3, m_macroblockHeight << 3
	};

	debug_out << std::dec;
	debug_out << "Sequence Header(";
	debug_out << "size: " << m_width << "x" << m_height << " @ " << m_frame_rate << "fps, ";
	debug_out << "aspect_ratio: " << aspect_ratio << ", ";
	debug_out << "bit_rate: ";
	if (bit_rate) {
		debug_out << bit_rate << "bps";
	}
	else {
		debug_out << "VBR ";
	}
	debug_out << " vbv_buffer_size: " << vbv_buffer_size << "KB, ";
	debug_out << "load_matrix /intra: " << load_intra_matrix << " /non_intra: " << load_non_intra_matrix << ", ";
	debug_out << "macroblocks: " << std::dec << m_macroblockWidth << "x" << m_macroblockHeight << ", ";
	debug_out << "luma: " << m_frameSize.luma_width << "x" << m_frameSize.luma_height
		<< " chroma: " << m_frameSize.chroma_width << "x" << m_frameSize.chroma_height << "";
	debug_out << ")\n";
	m_hasSequence = true;
}

#undef MARKER_BIT