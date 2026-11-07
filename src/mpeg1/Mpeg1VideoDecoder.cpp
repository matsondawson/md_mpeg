#include "Mpeg1VideoDecoder.h"

#include <algorithm>
#include <new>


enum {
	IDLE,
	DECODE_SEQUENCE,
	DECODE_GROUP_OF_PICTURES,
	DECODE_PICTURE
} DecodeStates;

Mpeg1VideoDecoder::Mpeg1VideoDecoder()
	: m_stream([this]()-> void { decodeOrResync(); })
	  , m_bitStream(&m_stream)
	  , m_decodeState(IDLE)
	  , m_frameCallback(nullptr)
	  , m_frameSize({0, 0, 0, 0})
	  , m_ptrFramePast(new Frame()), m_ptrFrameCurrent(new Frame()), m_ptrFrameFuture(new Frame())
	  , m_width(-1), m_height(-1)
	  // picture
	  , forward_r_size(0), backward_r_size(0), dc_predictor(), full_pel_backward_vector(false),
	  full_pel_forward_vector(false)
	  , macroblock_address(0), macroblock_intra(false), macroblock_motion_backward(false),
	  macroblock_motion_forward(false)
	  , next_start_code(-1) {
}

int32_t Mpeg1VideoDecoder::getPts(const int32_t frameTimeMillis) {
	return m_stream.getPts(frameTimeMillis);
}

void Mpeg1VideoDecoder::setPts(const int32_t ptsMillis) {
	m_stream.setPts(ptsMillis);
};

void Mpeg1VideoDecoder::writeByte(const uint8_t data) {
	m_stream.writeByte(data);
}

[[nodiscard]] bool Mpeg1VideoDecoder::isFull() {
	return m_stream.isFull();
}

void Mpeg1VideoDecoder::executeCallbacks() {
	m_stream.executeCallbacks();
}

void Mpeg1VideoDecoder::reset() {
	m_stream.clear();
	m_bitStream.reset();
	next_start_code = -1;
	nextFrameIndex = 0;
	forwardFrameIndex = -1;
	m_ptrRenderFrame = nullptr;
	m_decodeState = m_hasSequence ? DECODE_GROUP_OF_PICTURES : IDLE;
	m_waitForKeyframe = true;
	m_dropLeadingBFrames = true;
}

void Mpeg1VideoDecoder::decodeOrResync() {
	if (m_waitForKeyframe) {
		// Discard up to the next group of pictures or sequence header, leaving it unread
		while (m_stream.used() >= 4) {
			const auto code = m_stream.peek(4);
			if (code == GROUP_START_CODE || code == SEQUENCE_HEADER_CODE) {
				m_waitForKeyframe = false;
				break;
			}
			m_stream.nextByte();
		}
		if (m_waitForKeyframe) {
			return;
		}
	}
	// The pictures that triggered this call may have been discarded above; wait for a complete one
	if (m_stream.scanCodeCount() < 2) {
		return;
	}

	// Called when a picture start code is written and at least two are buffered, i.e. at least one
	// complete picture. Keep going while that holds: a pass that doesn't consume a picture (e.g. an error
	// before reading it) would otherwise leave it behind, and the backlog would grow until the ring fills.
	uint32_t usedBefore;
	do {
		usedBefore = m_stream.used();
		try {
			decodeFrame2();
		} catch (const std::bad_alloc &) {
			// Not the stream's fault, and every later picture would fail the same way
			throw;
		} catch (const std::exception &ex) {
			std::cout << "Corrupt video, skipping to next picture: " << ex.what() << std::endl;
			resync();
		}
	} while (m_stream.scanCodeCount() >= 2 && m_stream.used() != usedBefore);
}

/**
 * Discards data up to the next picture, group of pictures or sequence header (only a sequence header
 * if none has been seen yet), leaving that start code unread. If none is buffered yet, all but the last
 * 3 bytes are discarded and decoding resumes once more data arrives.
 * Pictures that predict from a corrupt frame show errors until the next I-frame.
 */
void Mpeg1VideoDecoder::resync() {
	m_bitStream.reset();
	next_start_code = -1;
	while (m_stream.used() >= 4) {
		const auto code = m_stream.peek(4);
		if (code == SEQUENCE_HEADER_CODE
		    || (m_hasSequence && (code == PICTURE_START_CODE || code == GROUP_START_CODE))) {
			break;
		}
		m_stream.nextByte();
	}
	m_decodeState = m_hasSequence ? DECODE_PICTURE : IDLE;
}

void Mpeg1VideoDecoder::flush() {
	// End of stream. Decoding is normally triggered by the start code of the following picture, which the
	// last picture doesn't have, so decode what's left directly. A held-back anchor frame may need a
	// second pass. The zero padding gives the start code reader something to stop on.
	if (m_decodeState == IDLE) {
		return;
	}
	for (int i = 0; i < 8 && !m_stream.isFull(); i++) {
		m_stream.writeByte(0);
	}
	try {
		for (int pass = 0; pass < 4 && m_decodeState != IDLE; pass++) {
			decodeFrame2();
		}
	} catch (const MStream::EndOfData &) {
		// Final picture was truncated
	} catch (const std::exception &ex) {
		std::cout << "Video end of stream: " << ex.what() << std::endl;
	}
}

void Mpeg1VideoDecoder::setFrameCallback(const std::function<void(const Frame &frame)> &frameCallback) {
	m_frameCallback = frameCallback;
}

bool Mpeg1VideoDecoder::nextStartCode(const uint32_t expected_start_code) {
	if (next_start_code == -1) {
		m_bitStream.align8();
		next_start_code = m_bitStream.pull(32);
		// std::cout << "next_start_code=" << std::hex << next_start_code << std::endl;
		// Throws EndOfData if no more data
	}
	if (next_start_code == expected_start_code) {
		last_start_code = next_start_code;
		next_start_code = -1;
		return true;
	}
	return false;
}

bool Mpeg1VideoDecoder::nextStartCodeRange(uint32_t start_code, uint32_t endCode) {
	m_bitStream.align8();
	if (next_start_code == -1) {
		next_start_code = m_bitStream.pull(32);
	}
	if (next_start_code >= start_code && next_start_code <= endCode) {
		last_start_code = next_start_code;
		next_start_code = -1;
		return true;
	}
	return false;
}

uint32_t Mpeg1VideoDecoder::lastStartCode() const {
	return last_start_code;
}

/**
 * Converts a decoded frame to 0x00RRGGBB, m_width x m_height pixels packed with no padding.
 * The decoded planes are padded to whole macroblocks, so each plane is read with its own stride.
 */
void Mpeg1VideoDecoder::generateRgbFrameData(uint32_t *rgbData, const Frame &frame) const {
	const uint32_t width = m_width;
	const uint32_t height = m_height;
	const uint32_t lumaStride = frame.m_frameSize.luma_width;
	const uint32_t chromaStride = frame.m_frameSize.chroma_width;

	// BT.601 limited range YCbCr to RGB, constants are Q16
	const auto toRgb = [](const uint8_t luma, const int32_t r, const int32_t g, const int32_t b) -> uint32_t {
		const int32_t y = (static_cast<int32_t>(luma) - 16) * 76309;
		const uint32_t rr = std::clamp<int32_t>((y + r) >> 16, 0, 255);
		const uint32_t gg = std::clamp<int32_t>((y - g) >> 16, 0, 255);
		const uint32_t bb = std::clamp<int32_t>((y + b) >> 16, 0, 255);
		return (rr << 16) | (gg << 8) | bb;
	};

	// Each chroma sample covers a 2x2 block of luma; odd sizes have a partial last row/column
	for (uint32_t cy = 0; cy < (height + 1) / 2; cy++) {
		const uint8_t *crRow = frame.m_crData + cy * chromaStride;
		const uint8_t *cbRow = frame.m_cbData + cy * chromaStride;
		for (uint32_t cx = 0; cx < (width + 1) / 2; cx++) {
			const int32_t cr = static_cast<int32_t>(crRow[cx]) - 128;
			const int32_t cb = static_cast<int32_t>(cbRow[cx]) - 128;
			const int32_t r = cr * 104597;
			const int32_t g = cb * 25674 + cr * 53278;
			const int32_t b = cb * 132201;

			for (uint32_t y = cy * 2; y < cy * 2 + 2 && y < height; y++) {
				const uint8_t *lumaRow = frame.m_yData + y * lumaStride;
				uint32_t *outRow = rgbData + y * width;
				for (uint32_t x = cx * 2; x < cx * 2 + 2 && x < width; x++) {
					outRow[x] = toRgb(lumaRow[x], r, g, b);
				}
			}
		}
	}
}

void Mpeg1VideoDecoder::skipExtensionAndUserData() {
	if (nextStartCode(EXTENSION_START_CODE)) {
		while (m_bitStream.peek(24) != 1) {
			m_bitStream.pull(8);
		}
	}

	if (nextStartCode(USER_START_CODE)) {
		while (m_bitStream.peek(24) != 1) {
			m_bitStream.pull(8);
		}
	}
}

bool Mpeg1VideoDecoder::decodeFrame2() {
	m_ptrRenderFrame = nullptr;
	bool decodedAFrame = false;

idle:
	if (m_decodeState == IDLE) {
		if (nextStartCode(0)) {
			// Zero padding from flush(): end of stream
			return true;
		}
		if (!nextStartCode(SEQUENCE_HEADER_CODE)) {
			if (m_hasSequence) {
				cthrow(StringException, "Unexpected start code " << streamHex(8) << next_start_code);
			}
#ifdef MDEBUG
			cthrow(StringException, "Missing sequence header" << m_bitStream.m_readTotal);
#else
			cthrow(StringException, "Missing sequence header");
#endif
		}
		m_decodeState = DECODE_SEQUENCE;
	}
decodeSequence:
	if (m_decodeState == DECODE_SEQUENCE) {
		// std::cout << "SEQUENCE_HEADER_CODE" << std::endl;
		decodeSequence();
		skipExtensionAndUserData();
		m_decodeState = DECODE_GROUP_OF_PICTURES;
	}
decodeGroupOfPictures:
	if (m_decodeState == DECODE_GROUP_OF_PICTURES) {
		// std::cout << "DECODE_GROUP_OF_PICTURES" << std::endl;
		if (nextStartCode(GROUP_START_CODE)) {
			// TODO decode group time code
			// 25:time_code / 1:closed_gop / 1:broken_link
			m_bitStream.skip(27);

			skipExtensionAndUserData();

			m_decodeState = DECODE_PICTURE;
		} else {
			if (nextStartCode(SEQUENCE_HEADER_CODE)) {
				m_decodeState = DECODE_SEQUENCE;
				goto decodeSequence;
			} else {
				nextStartCode(SEQUENCE_END_CODE);
				m_decodeState = IDLE;
				goto idle;
			}
		}
	}
	if (m_decodeState == DECODE_PICTURE) {
		//std::cout << "DECODE_PICTURE" << std::endl;
		if (nextFrameIndex == forwardFrameIndex) {
			// Use I/P anchor frame
			m_ptrRenderFrame = m_ptrFrameFuture;
			std::swap(m_ptrFrameFuture, m_ptrFramePast);
			nextFrameIndex++;

			if (m_ptrRenderFrame && m_frameCallback) {
				m_frameCallback(*m_ptrRenderFrame);
			}
			m_ptrRenderFrame = nullptr;
			m_dropLeadingBFrames = false;
			// Fall through and decode the next picture too. This call was triggered by a complete picture
			// arriving; if we only emitted the anchor, that picture would be left behind, and the backlog
			// grows by one per anchor until the input ring buffer fills and the demuxer stalls.
		}
		if (nextStartCode(PICTURE_START_CODE)) {
			//std::cout << "PICTURE_START_CODE" << std::endl;
			// Decode a new frame
			const auto tr = decodeFrame();
			decodedAFrame = true;

			// Temporal reference restarts in each group of pictures
			if (tr < nextFrameIndex) {
				forwardFrameIndex = -1;
				nextFrameIndex = 0;
			}

			if (tr == nextFrameIndex) {
				m_ptrRenderFrame = m_ptrFrameCurrent;
				// B and D frames cannot be part of prediction
				if (last_picture_coding_type == VideoPictureType_INTRA || last_picture_coding_type ==
				    VideoPictureType_PREDICTIVE) {
					std::swap(m_ptrFrameCurrent, m_ptrFramePast);
				}
				nextFrameIndex++;
			} else if (tr > nextFrameIndex) {
				// I/P frame displayed after the B frames that follow it: hold it back
				forwardFrameIndex = tr;
				std::swap(m_ptrFrameCurrent, m_ptrFrameFuture);
			}

			if (m_ptrRenderFrame) {
				if (last_picture_coding_type == VideoPictureType_B) {
					if (m_dropLeadingBFrames) {
						m_ptrRenderFrame = nullptr;
					}
				} else {
					m_dropLeadingBFrames = false;
				}
			}
			if (m_ptrRenderFrame && m_frameCallback) {
				m_frameCallback(*m_ptrRenderFrame);
			}
		} else {
			m_decodeState = DECODE_GROUP_OF_PICTURES;
			goto decodeGroupOfPictures;
		}
	}

	return decodedAFrame;
}
