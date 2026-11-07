#pragma once

#include <algorithm>
#include <iostream>
#include <iomanip>
#include <utility>
#include <vector>
#include <sstream>

#include "../common.h"
#include "../MException.h"
#include "../stream/MStream.h"
#include "../stream/FileStream.h"
#include "../stream/RingBufferWithStartCodeEvent.h"

// Bytes of input buffered, override for small targets
#ifndef MD_MPEG_DEMUX_RING_SIZE
#define MD_MPEG_DEMUX_RING_SIZE 2048
#endif

namespace Mpeg1 {
	customException(CorruptPacketException);

	constexpr uint32_t PACK_START_CODE = 0x000001BA;
	constexpr uint32_t SYSTEM_START_CODE = 0x000001BB;
	constexpr uint32_t PADDING_STREAM_START_CODE = 0x000001BE;
	constexpr uint32_t VIDEO_STREAM_1_START_CODE = 0x000001E0;
	constexpr uint32_t AUDIO_STREAM_1_START_CODE = 0x000001C0;
	// Lowest stream id; every start code from here up is a packet with a 16-bit length
	constexpr uint32_t FIRST_STREAM_START_CODE = 0x000001BC;
	constexpr bool isVideoStream(const uint32_t startCode) { return (startCode & ~0xF) == VIDEO_STREAM_1_START_CODE; }
	constexpr bool isAudioStream(const uint32_t startCode) { return (startCode & ~0x1F) == AUDIO_STREAM_1_START_CODE; }

	// Result of MpegProgramStream::nextPacket
	enum class DemuxResult {
		// Call again: data was demuxed, or more input or decoder space may be available next time
		Continue,
		// A packet header is incomplete and this call read no input
		WaitingForInput,
		// All input has been demuxed
		EndOfStream
	};

	class IDecode {
	public:
		virtual ~IDecode() = default;

	private:
		virtual void nextFrame() = 0;
	};

	// Discards everything, for packets of streams that aren't decoded
	class NullDecoderStream final : public MStream::IWriteByteStream {
	public:
		NullDecoderStream() = default;

		[[nodiscard]] int32_t getPts(const int32_t frameTimeMillis) override {
			return 0;
		}

		void setPts(int32_t ptsMillis) override {
			// empty
		}

		void writeByte(uint8_t byte) override {
			// empty
		}

		[[nodiscard]] bool isFull() override {
			return false;
		}

		void executeCallbacks() override {
			// empty
		}

		void flush() override {
			// empty
		}
	};

	class MpegProgramStream {
		MStream::RingBufferWithStartCodeEvent<MD_MPEG_DEMUX_RING_SIZE> m_buffer;

		uint32_t m_bitCount, m_bitsLeft;
		int32_t m_streamingPacketBytesRemaining;

		bool m_endOfStreamFlushed = false;

		// For seeking: file position of the next byte read, and the position/SCR of the latest pack
		int64_t m_inputPosition = 0;
		int64_t m_lastPackPosition = -1;
		int64_t m_lastScrMillis = -1;
		// First pack seen, to measure the average byte rate (the header's mux rate is often wrong)
		int64_t m_firstPackPosition = -1;
		int64_t m_firstScrMillis = -1;
		// Last pack in the file, if known, for the average rate over the whole file
		int64_t m_endPackPosition = -1;
		int64_t m_endScrMillis = -1;
		uint32_t m_muxRateBytesPerSecond = 0;
		int64_t m_lastVideoPtsMillis = -1;
		uint32_t m_syncSkippedBytes = 0;
		// After a seek the input starts mid-packet, so skipping to the first start code isn't corruption
		bool m_expectSyncSkip = false;
		// Only the first video and audio streams are decoded, 0 until seen
		uint32_t m_videoStreamCode = 0;
		uint32_t m_audioStreamCode = 0;

		NullDecoderStream m_nullDecoderStream;
		MStream::IWriteByteStream *m_ptrCurrentDecoderStream;
		MStream::IWriteByteStream *m_ptrVideoDecoderStream;
		MStream::IWriteByteStream *m_ptrAudioDecoderStream;

	public:
		MpegProgramStream()
			: m_bitCount(0)
			  , m_bitsLeft(0)
			  , m_streamingPacketBytesRemaining(0)
			  , m_ptrCurrentDecoderStream(nullptr)
			  , m_ptrVideoDecoderStream(&m_nullDecoderStream)
			  , m_ptrAudioDecoderStream(&m_nullDecoderStream) {
			// empty
		}

	private:
		void streamMarkerBit() {
			if (pullBits(1) != 1) {
				throw CorruptPacketException("Marker bit not 1");
			}
		}

	public:
		uint32_t pull(const uint32_t byteCount) {
			assert(byteCount <= m_streamingPacketBytesRemaining);
			const auto result = m_buffer.pull(byteCount);
			m_streamingPacketBytesRemaining -= byteCount;
			return result;
		}

		[[nodiscard]] uint8_t peek() {
			return m_buffer.peek(1);
		}

		void skip(const uint32_t byteCount) {
			assert(byteCount <= m_streamingPacketBytesRemaining);
			m_buffer.skip(byteCount);
			m_streamingPacketBytesRemaining -= byteCount;
		}

		static bool validStartCode(const uint32_t startCode) {
			return startCode == PACK_START_CODE
			       || startCode == SYSTEM_START_CODE
			       || (startCode >= FIRST_STREAM_START_CODE && startCode <= 0x000001FF);
		}

		/**
		 * Demuxes the next chunk of the input into the decoders.
		 * Pacing comes from the decoders: the video frame callback can block while its consumer is busy (the
		 * player's frame queue is full), and the audio decoder reports isFull() while its output buffer can't
		 * take another frame.
		 *
		 * @param inputStream Source of program stream bytes
		 */
		DemuxResult nextPacket(MStream::IReadByteStream &inputStream) {
			// Let a decoder that was waiting on output space (audio backpressure) make progress
			m_ptrAudioDecoderStream->executeCallbacks();

			// fill buffer
			bool streamFilled = false;
			if (!inputStream.isEmpty()) {
				// Fill buffer from input stream until input stream is empty or buffer is full
				while (!m_buffer.isFull() && !inputStream.isEmpty()) {
					m_buffer.writeByte(inputStream.nextByte());
					m_inputPosition++;
				}
				// Flag that we actually read data
				// if we have an eof exception it could just mean data is not ready yet
				streamFilled = true;
			}

			if (m_ptrCurrentDecoderStream == nullptr && !m_buffer.isEmpty()) {
				// Sync to a start code: discard bytes until the next 4 are a valid start code
				while (m_buffer.used() >= 4 && !validStartCode(m_buffer.peek(4))) {
					m_buffer.nextByte();
					m_syncSkippedBytes++;
				}
				if (m_buffer.used() < 4) {
					if (inputStream.isEmpty()) {
						// Trailing bytes at the end of the file
						while (!m_buffer.isEmpty()) {
							m_buffer.nextByte();
						}
						return endOfStream();
					}
					return DemuxResult::Continue;
				}
				if (m_syncSkippedBytes && !m_expectSyncSkip) {
					std::cout << "Stream corruption, skipped " << m_syncSkippedBytes << " bytes to the next start code" << std::endl;
				}
				m_syncSkippedBytes = 0;
				m_expectSyncSkip = false;

				const auto m_streamingPacketBytesRemainingMark = m_streamingPacketBytesRemaining;
				try {
					m_buffer.mark();
					const uint32_t startCode = m_buffer.pull(4);

					switch (startCode) {
						case PACK_START_CODE: {
							// Unread bytes include the 4 just pulled
							m_lastPackPosition = m_inputPosition - m_buffer.used() - 4;
							decodePackHeader();
							break;
						}
						case SYSTEM_START_CODE: {
							decodeSystemHeader();
							break;
						}
						default:
							if (isVideoStream(startCode) && (!m_videoStreamCode || m_videoStreamCode == startCode)) {
								m_videoStreamCode = startCode;
								m_ptrCurrentDecoderStream = m_ptrVideoDecoderStream;
								decodeStreamHeader(startCode, false);
							} else if (isAudioStream(startCode) && (!m_audioStreamCode || m_audioStreamCode == startCode)) {
								m_audioStreamCode = startCode;
								m_ptrCurrentDecoderStream = m_ptrAudioDecoderStream;
								decodeStreamHeader(startCode, true);
							} else {
								// Padding, private, additional or unsupported streams: skip the packet
								m_streamingPacketBytesRemaining = m_buffer.pull(2);
								m_ptrCurrentDecoderStream = &m_nullDecoderStream;
							}
					}

					m_buffer.clearMark();
				} catch ([[maybe_unused]] const MStream::EndOfData &ex) {
					// Header not fully buffered: rewind and retry on the next call
					m_buffer.reset();
					m_streamingPacketBytesRemaining = m_streamingPacketBytesRemainingMark;
					// The header may have selected a stream before running out, so parse it again next time
					m_ptrCurrentDecoderStream = nullptr;

					if (inputStream.isEmpty()) {
						// Truncated packet header at the end of the file, discard it
						while (!m_buffer.isEmpty()) {
							m_buffer.nextByte();
						}
						return endOfStream();
					}

					if (!streamFilled) {
						return DemuxResult::WaitingForInput;
					}
					return DemuxResult::Continue;
				} catch (const CorruptPacketException &ex) {
					// Drop the packet; the next call scans for the next valid start code
					m_buffer.clearMark();
					m_ptrCurrentDecoderStream = nullptr;
					m_streamingPacketBytesRemaining = 0;
					std::cout << "Corrupt packet, skipping: " << ex.what() << std::endl;
					return DemuxResult::Continue;
				}
			}
			// Packet lengths are 16 bits, so this only catches logic errors
			assert(m_streamingPacketBytesRemaining < 0xFFFFFF);

			if (inputStream.isEmpty() && m_buffer.isEmpty()) {
				// Any remaining packet bytes are missing from a truncated file
				return endOfStream();
			}

			// If packet bytes remain there is a current stream. Pack and system headers leave none.
			uint32_t packetWriteBytes = (m_streamingPacketBytesRemaining < m_buffer.used())
				                            ? m_streamingPacketBytesRemaining
				                            : m_buffer.used();

			if (packetWriteBytes) {
				// std::cout << "Packet bytes: " << packetWriteBytes << std::endl;
				assert(packetWriteBytes <= m_streamingPacketBytesRemaining);
				// Send data to decoder
				int32_t streamed = 0;
				if (m_ptrCurrentDecoderStream->isFull()) {
					m_ptrCurrentDecoderStream->executeCallbacks();
					if (m_ptrCurrentDecoderStream->isFull()) {
						// Decoder is applying backpressure, try again later
						return DemuxResult::Continue;
					}
				}
				while (!m_buffer.isEmpty() && packetWriteBytes) {
					if (m_ptrCurrentDecoderStream->isFull()) {
						// Let the decoder consume what it has (e.g. the audio decoder decoding a frame or
						// skipping corrupt bytes), stop if it is applying backpressure
						m_ptrCurrentDecoderStream->executeCallbacks();
						if (m_ptrCurrentDecoderStream->isFull()) {
							break;
						}
					}
					m_ptrCurrentDecoderStream->writeByte(m_buffer.nextByte());
					packetWriteBytes--;
					streamed++;
				}
				if (m_ptrCurrentDecoderStream == m_ptrAudioDecoderStream) m_ptrCurrentDecoderStream->executeCallbacks();

				//assert(streamed <= packetWriteBytes);
				//assert(streamed <= m_streamingPacketBytesRemaining);
				m_streamingPacketBytesRemaining -= streamed;
				if (!m_streamingPacketBytesRemaining) {
					m_ptrCurrentDecoderStream = nullptr;
				}
			}
			assert(m_streamingPacketBytesRemaining >= 0);

			return DemuxResult::Continue;
		}

		/**
		 * All input has been demuxed. Lets the video decoder finish its last pictures.
		 * Audio frames are complete as they arrive, apart from any waiting on output space, which the
		 * caller drains via the audio decoder's executeCallbacks().
		 */
		DemuxResult endOfStream() {
			m_ptrCurrentDecoderStream = nullptr;
			m_streamingPacketBytesRemaining = 0;
			if (!m_endOfStreamFlushed) {
				m_endOfStreamFlushed = true;
				m_ptrVideoDecoderStream->flush();
			}
			return DemuxResult::EndOfStream;
		}

		/**
		 * Forgets all buffered data, ready to continue from a new input position (after a seek).
		 * The decoders must be reset separately.
		 */
		void reset() {
			m_buffer.clear();
			m_ptrCurrentDecoderStream = nullptr;
			m_streamingPacketBytesRemaining = 0;
			m_bitCount = m_bitsLeft = 0;
			m_endOfStreamFlushed = false;
			m_syncSkippedBytes = 0;
			m_expectSyncSkip = true;
		}

		/**
		 * Records the last pack in the file (see findLastPack) so seeking can use the average byte rate
		 * over the whole file.
		 */
		void setEndPack(const int64_t bytePosition, const int64_t scrMillis) {
			m_endPackPosition = bytePosition;
			m_endScrMillis = scrMillis;
		}

		// SCR of the first pack, the start of the timeline, -1 if not known yet
		[[nodiscard]] int64_t startMillis() const {
			return m_firstScrMillis;
		}

		// Duration from the first to the last pack SCR, -1 if not known yet
		[[nodiscard]] int64_t durationMillis() const {
			return m_endScrMillis >= 0 && m_firstScrMillis >= 0 ? m_endScrMillis - m_firstScrMillis : -1;
		}

		// The input has been moved to bytePosition
		void setInputPosition(const int64_t bytePosition) {
			m_inputPosition = bytePosition;
		}

		/**
		 * Estimates the file position holding data for presentation time targetPtsMillis, from the latest
		 * pack's position and SCR, the average byte rate, and how far video PTS run ahead of the SCR.
		 * @param targetPtsMillis Presentation time to seek to
		 * @return byte position, or -1 if nothing is known yet
		 */
		[[nodiscard]] int64_t estimateBytePosition(const int64_t targetPtsMillis) const {
			if (m_lastPackPosition < 0) {
				return -1;
			}
			// Average rate over the whole file if its end is known, else between the first and latest pack,
			// falling back to the header's mux rate
			int64_t rateBytes = m_muxRateBytesPerSecond;
			int64_t rateMillis = 1000;
			if (m_endScrMillis - m_firstScrMillis >= 200 && m_endPackPosition > m_firstPackPosition) {
				rateBytes = m_endPackPosition - m_firstPackPosition;
				rateMillis = m_endScrMillis - m_firstScrMillis;
			} else if (m_lastScrMillis - m_firstScrMillis >= 200) {
				rateBytes = m_lastPackPosition - m_firstPackPosition;
				rateMillis = m_lastScrMillis - m_firstScrMillis;
			}
			if (rateBytes <= 0) {
				return -1;
			}
			// Video is muxed ahead of its presentation time
			const int64_t videoLeadMillis = m_lastVideoPtsMillis >= 0 ? m_lastVideoPtsMillis - m_lastScrMillis : 0;
			const int64_t targetScrMillis = targetPtsMillis - videoLeadMillis;
			// Multiplied first to keep precision. Time is under 2^27 ms (33-bit clock), so this can't overflow
			// for files under 64 GiB.
			const int64_t position = m_lastPackPosition + (targetScrMillis - m_lastScrMillis) * rateBytes / rateMillis;
			return std::max<int64_t>(0, position);
		}

		void setVideoDecoder(MStream::IWriteByteStream *decoder) {
			m_ptrVideoDecoderStream = decoder;
		}

		void setAudioDecoder(MStream::IWriteByteStream *decoder) {
			m_ptrAudioDecoderStream = decoder;
		}

		uint32_t pullBits(const uint32_t readBitCount) {
			assert(readBitCount > 0);
			// More than 24 could overflow m_bitsLeft, which can already hold 7 bits
			assert(readBitCount <= 24);

			// TODO optimize to pull(N)
			while (m_bitCount < readBitCount) {
				m_bitsLeft = m_bitsLeft << 8 | pull(1);
				m_bitCount += 8;
			}

			m_bitCount -= readBitCount;
			return (m_bitsLeft >> m_bitCount) & ((1 << readBitCount) - 1);
		}

		// Reads a 36-bit field, a 33-bit time in 90 kHz ticks and 3 marker bits, and returns it in ms.
		// The 33-bit time needs an int64_t.
		int32_t decodeTime() {
			int64_t clock = static_cast<int64_t>(pullBits(3)) << 30;
			streamMarkerBit();
			clock |= pullBits(15) << 15;
			streamMarkerBit();
			clock |= pullBits(15);
			streamMarkerBit();
			return static_cast<int32_t>(clock / 90); // milliseconds
		}

		void decodePackHeader() {
			const uint8_t t = m_buffer.pull(1);
			if (t >> 4 != 2) {
				throw CorruptPacketException("pack header missing 0010");
			}
			if (!(t & 1)) {
				throw CorruptPacketException("Marker bit not 1");
			}
			const auto system_clock_reference_base_highest = (t >> 1) & 7;

			// 15:system_clock_reference_base[29..15] / 1:marker bit
			auto system_clock_reference_base_high = m_buffer.pull(2);
			if (!(system_clock_reference_base_high & 1)) {
				throw CorruptPacketException("Marker bit not 1");
			}
			system_clock_reference_base_high >>= 1;

			// 15:system_clock_reference_base[14..0] / 1:marker bit
			auto system_clock_reference_base_low = m_buffer.pull(2);
			if (!(system_clock_reference_base_low & 1)) {
				throw CorruptPacketException("Marker bit not 1");
			}
			system_clock_reference_base_low >>= 1;

			const auto system_clock_reference_base =
					static_cast<uint64_t>(system_clock_reference_base_highest) << 30
					| system_clock_reference_base_high << 15
					| system_clock_reference_base_low;
			const auto system_clock_reference_base_millis = static_cast<int32_t>(system_clock_reference_base / 90);

			// 1:marker_bit / 22:program_mux_rate / 1:marker bit
			auto program_mux_rate = m_buffer.pull(3);
			constexpr uint32_t mux_marker_mask = (1 << 23) + 1;
			if ((program_mux_rate & mux_marker_mask) != mux_marker_mask) {
				throw CorruptPacketException("Marker bit not 1");
			}
			program_mux_rate = (program_mux_rate >> 1) & 0x3FFFFF;
			m_streamingPacketBytesRemaining = 0;

			m_lastScrMillis = system_clock_reference_base_millis;
			if (m_firstPackPosition < 0) {
				m_firstPackPosition = m_lastPackPosition;
				m_firstScrMillis = m_lastScrMillis;
			}
			m_muxRateBytesPerSecond = program_mux_rate * 50; // units of 50 bytes/s
		}

		void decodeSystemHeader() {
			m_streamingPacketBytesRemaining = m_buffer.pull(2);

			// 1:marker_bit / 22:rate_bound / 1:marker_bit;
			if ((pull(3) & ((1 << 23) + 1)) != ((1 << 23) + 1)) {
				throw CorruptPacketException("Marker bit not 1");
			}

			const uint32_t t = pull(2);
			// 6:audio_bound, 1:fixed_flag, 1:CSPS_flag, 1:system_audio_lock_flag, 1:system_video_lock_flag / 1:marker_bit / 5:video_bound
			if (!(t & (1 << 5))) {
				throw CorruptPacketException("Marker bit not 1");
			}
			// video_bound (t & 31) and audio_bound may be anything, including 0 for audio or video only
			// files. Only the first stream of each type is decoded.

			// 1:packet_rate_restriction_flag / 7:reserved_byte(127)
			if ((pull(1) & 127) != 127) {
				throw CorruptPacketException("reserved byte not 127");
			}

			// skip stream bound entries
			while (m_streamingPacketBytesRemaining >= 3) {
				// TODO validate stream bound entries?
				skip(3);
			}

			if (m_streamingPacketBytesRemaining) {
				throw CorruptPacketException("invalid header length");
			}
		}

		void decodeStreamHeader(const uint32_t startCode, const bool audio) {
			m_streamingPacketBytesRemaining = m_buffer.pull(2);

			// remove stuffing bytes
			while (peek() == 0xFF) {
				skip(1);
			}

			m_bitsLeft = m_bitCount = 0;

			// Optional P-STD fields: '01' and 14 bits. Pulling 16 also takes the top 2 bits of the next field,
			// so the position after this matches the case without them.
			if (pullBits(2) == 1) {
				pullBits(16);
			}

			// pts_dts_marker
			switch (pullBits(2)) {
				case 0x00: {
					break;
				}
				case 0x02: {
					const int32_t pts = decodeTime();
					m_ptrCurrentDecoderStream->setPts(pts);
					if (!audio) m_lastVideoPtsMillis = pts;
					break;
				}
				case 0x03: {
					const int32_t pts = decodeTime(); // 36 bits
					if (pullBits(4) != 1) {
						throw CorruptPacketException("Invalid pts marker bit");
					}
					m_ptrCurrentDecoderStream->setPts(pts);
					if (!audio) m_lastVideoPtsMillis = pts;
					decodeTime(); // DTS, not used
					break;
				}
				default: // 0x01
					throw CorruptPacketException("Invalid PTS/DTS flags");
			}
		}
	};

	/**
	 * Finds the last pack header in the last 256 KiB of the file, scanning backwards from the end.
	 * Leaves the file positioned at the start.
	 * @return {byte position, SCR in ms}, or {-1, -1} if none was found
	 */
	inline std::pair<int64_t, int64_t> findLastPack(MStream::FileStreamRead &file) {
		constexpr size_t SEARCH_BYTES = 256 * 1024;
		const size_t start = file.size() > SEARCH_BYTES ? file.size() - SEARCH_BYTES : 0;
		std::vector<uint8_t> tail;
		tail.reserve(file.size() - start);
		file.seek(start);
		while (!file.isEmpty()) {
			tail.push_back(file.nextByte());
		}
		file.seek(0);

		// Pack header: 00 00 01 BA, then '0010' SCR[32..30] marker, SCR[29..15] marker, SCR[14..0] marker
		for (size_t i = tail.size() >= 9 ? tail.size() - 9 : 0; i-- > 0;) {
			if (tail[i] != 0 || tail[i + 1] != 0 || tail[i + 2] != 1 || tail[i + 3] != 0xBA) {
				continue;
			}
			const uint8_t *b = &tail[i + 4];
			if ((b[0] >> 4) != 2 || !(b[0] & 1) || !(b[2] & 1) || !(b[4] & 1)) {
				continue;
			}
			const uint64_t scr = static_cast<uint64_t>((b[0] >> 1) & 7) << 30
			                     | static_cast<uint64_t>(((b[1] << 8) | b[2]) >> 1) << 15
			                     | static_cast<uint64_t>(((b[3] << 8) | b[4]) >> 1);
			return {static_cast<int64_t>(start + i), static_cast<int64_t>(scr / 90)};
		}
		return {-1, -1};
	}
}
