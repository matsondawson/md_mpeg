#pragma once

#include <algorithm>
#include <iostream>
#include <iomanip>
#include <utility>
#include <vector>
#include <sstream>

#include "../common.h"
#include "../Diagnostics.h"
#include "../MException.h"
#include "../stream/MStream.h"
#include "../stream/FileStream.h"
#include "../stream/RingBufferWithStartCodeEvent.h"
#include "ProgramTimeline.h"

// Set to 0 to build without MPEG-2 video, MPEG-2 program streams and A/52 audio, for a smaller binary
#ifndef MD_MPEG_ENABLE_MPEG2
#define MD_MPEG_ENABLE_MPEG2 1
#endif

// Bytes of input buffered, override for small targets
#ifndef MD_MPEG_DEMUX_RING_SIZE
#define MD_MPEG_DEMUX_RING_SIZE 2048
#endif

namespace Mpeg1 {
	customException(CorruptPacketException);

	constexpr uint32_t PACK_START_CODE = 0x000001BA;
	constexpr uint32_t SYSTEM_START_CODE = 0x000001BB;
	constexpr uint32_t PADDING_STREAM_START_CODE = 0x000001BE;
	// Carries A/52 audio, among other things, in sub-streams
	constexpr uint32_t PRIVATE_STREAM_1_START_CODE = 0x000001BD;
	// Carries DVD navigation packs
	constexpr uint32_t PRIVATE_STREAM_2_START_CODE = 0x000001BF;
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
		Diagnostics m_diagnostics;

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
		// The video and audio streams decoded, 0 until seen. Only the first video stream is decoded, see
		// selectAudioStream for audio. An A/52 or LPCM audio stream's code is (PRIVATE_STREAM_1_START_CODE << 8)
		// + its sub-stream id.
		uint32_t m_videoStreamCode = 0;
		uint32_t m_audioStreamCode = 0;
		// Packs since the last packet of the selected audio stream, and the lowest numbered other stream of its
		// kind seen since
		uint32_t m_packsSinceSelectedAudio = 0;
		uint32_t m_lowestOtherAudioStreamCode = 0;

		// Time across the whole file, see buildTimeline. Timeline time minus SCR, for the latest pack.
		ProgramTimeline m_timeline;
		int64_t m_timelineOffsetMillis = 0;
		bool m_timelineOffsetKnown = false;

		NullDecoderStream m_nullDecoderStream;
		MStream::IWriteByteStream *m_ptrCurrentDecoderStream;
		MStream::IWriteByteStream *m_ptrVideoDecoderStream;
		MStream::IWriteByteStream *m_ptrAudioDecoderStream;
		MStream::IWriteByteStream *m_ptrA52DecoderStream;
		MStream::IWriteByteStream *m_ptrLpcmDecoderStream;
		// DVD: navigation packs, and the sub-picture stream shown (sub-stream 0x20-0x3F, -1 none)
		MStream::IWriteByteStream *m_ptrNavigationDecoderStream = nullptr;
		MStream::IWriteByteStream *m_ptrSubpictureDecoderStream = nullptr;
		int m_subpictureStreamId = -1;
		// The audio stream to decode, chosen by the caller (e.g. a DVD's navigation), 0 to choose automatically
		uint32_t m_forcedAudioStreamCode = 0;
		// The decoder of the audio stream being decoded
		MStream::IWriteByteStream *m_ptrSelectedAudioDecoderStream;

	public:
		MpegProgramStream()
			: m_bitCount(0)
			  , m_bitsLeft(0)
			  , m_streamingPacketBytesRemaining(0)
			  , m_ptrCurrentDecoderStream(nullptr)
			  , m_ptrVideoDecoderStream(&m_nullDecoderStream)
			  , m_ptrAudioDecoderStream(&m_nullDecoderStream)
			  , m_ptrA52DecoderStream(&m_nullDecoderStream)
			  , m_ptrLpcmDecoderStream(&m_nullDecoderStream)
			  , m_ptrSelectedAudioDecoderStream(&m_nullDecoderStream) {
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
			m_ptrSelectedAudioDecoderStream->executeCallbacks();

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
					MD_MPEG_REPORT(m_diagnostics, Diagnostic::SyncBytesSkipped, m_syncSkippedBytes,
					               "Stream corruption, skipped bytes to the next start code");
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
							m_packsSinceSelectedAudio++;
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
								setPacketPts(decodeStreamHeader(false));
							} else if (isAudioStream(startCode) && selectAudioStream(startCode)) {
								m_ptrCurrentDecoderStream = m_ptrSelectedAudioDecoderStream = m_ptrAudioDecoderStream;
								setPacketPts(decodeStreamHeader(true));
#if MD_MPEG_ENABLE_MPEG2
							} else if (startCode == PRIVATE_STREAM_1_START_CODE
							           && (m_ptrA52DecoderStream != &m_nullDecoderStream
							               || m_ptrLpcmDecoderStream != &m_nullDecoderStream || m_ptrSubpictureDecoderStream)) {
								decodePrivateStream1Header();
							} else if (startCode == PRIVATE_STREAM_2_START_CODE && m_ptrNavigationDecoderStream) {
								m_streamingPacketBytesRemaining = m_buffer.pull(2);
								m_ptrCurrentDecoderStream = m_ptrNavigationDecoderStream;
								m_ptrCurrentDecoderStream->startPacket(m_streamingPacketBytesRemaining);
#endif
							} else {
								// Padding, private, additional or unsupported streams: skip the packet
								m_streamingPacketBytesRemaining = m_buffer.pull(2);
								m_ptrCurrentDecoderStream = &m_nullDecoderStream;
							}
					}

					m_buffer.clearMark();
					if (m_streamingPacketBytesRemaining == 0) {
						// An empty packet, or a header taking the whole packet: back to scanning for start codes.
						// Left selected, the stream would wait forever for bytes that aren't coming.
						m_ptrCurrentDecoderStream = nullptr;
					}
				} catch ([[maybe_unused]] const MStream::EndOfData &ex) {
					// Header not fully buffered: rewind and retry on the next call
					m_buffer.reset();
					m_streamingPacketBytesRemaining = m_streamingPacketBytesRemainingMark;
					// The header may have selected a stream before running out, so parse it again next time
					m_ptrCurrentDecoderStream = nullptr;

					if (m_buffer.isFull()) {
						// The header is longer than the whole buffer, so it will never be complete. Real headers are
						// far shorter, so it's corrupt: drop its start code and scan for the next.
						m_buffer.nextByte();
						MD_MPEG_REPORT(m_diagnostics, Diagnostic::CorruptPacket, 1, "Corrupt packet, header too long");
						return DemuxResult::Continue;
					}

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
					MD_MPEG_REPORT(m_diagnostics, Diagnostic::CorruptPacket, 1, ex.what());
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
				if (m_ptrCurrentDecoderStream == m_ptrSelectedAudioDecoderStream) m_ptrCurrentDecoderStream->executeCallbacks();

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

		/**
		 * Samples the file to map byte positions to a timeline across the whole file, which stays right when the
		 * SCR starts again part way through (see ProgramTimeline). Leaves the file positioned at the start.
		 */
		void buildTimeline(MStream::FileStreamRead &file) {
			m_timeline.build(file);
			if (m_timeline.valid()) {
				setEndPack(m_timeline.points().back().position, m_timeline.points().back().scrMillis);
			}
		}

#if MD_MPEG_ENABLE_MPEG2
		// DVD navigation packs (private stream 2) go to decoder, which is told where each packet starts
		void setNavigationDecoder(MStream::IWriteByteStream *decoder) {
			m_ptrNavigationDecoderStream = decoder;
		}

		// Sub-picture sub-stream id (0x20-0x3F) to send to decoder, -1 for none
		void setSubpictureDecoder(MStream::IWriteByteStream *decoder, const int subStreamId) {
			m_ptrSubpictureDecoderStream = decoder;
			m_subpictureStreamId = subStreamId;
		}
#endif

		/**
		 * Decodes only this audio stream: an MPEG audio stream id (0xC0-0xDF), or an A/52 (0x80-0x87) or LPCM
		 * (0xA0-0xA7) sub-stream id.
		 * 0 chooses automatically (see selectAudioStream).
		 */
		void setAudioStream(const uint32_t streamId) {
			m_forcedAudioStreamCode = streamId >= 0xC0 ? 0x100 | streamId
			                          : streamId ? PRIVATE_STREAM_1_START_CODE << 8 | streamId : 0;
		}

		[[nodiscard]] const ProgramTimeline &timeline() const {
			return m_timeline;
		}

		/**
		 * Converts a PTS of the stream being demuxed to timeline time, which starts at 0 at the start of the file.
		 * Uses the latest pack's program, so it applies to PTS of data demuxed recently.
		 * @return ms, or -1 if not known yet
		 */
		[[nodiscard]] int64_t timelineMillis(const int64_t ptsMillis) const {
			if (m_timelineOffsetKnown) {
				return ptsMillis + m_timelineOffsetMillis;
			}
			return m_firstScrMillis >= 0 ? ptsMillis - m_firstScrMillis : -1;
		}

		// Length of the timeline, -1 if not known
		[[nodiscard]] int64_t timelineDurationMillis() const {
			return m_timeline.valid() ? m_timeline.durationMillis() : durationMillis();
		}

		// Timeline time at a byte position, -1 if not known
		[[nodiscard]] int64_t timelineAtPosition(const int64_t position) const {
			if (m_timeline.valid()) {
				return m_timeline.timelineAtPosition(position);
			}
			const int64_t duration = durationMillis();
			return duration > 0 && m_endPackPosition > 0 ? position * duration / m_endPackPosition : -1;
		}

		/**
		 * Finds the file position holding data for timeline time targetMillis, reading packs from the file to
		 * correct the estimate. Leaves the file at an undefined position.
		 * @return byte position, or -1 if nothing is known yet
		 */
		int64_t findTimelinePosition(MStream::FileStreamRead &file, const int64_t targetMillis) const {
			// Video is muxed ahead of its presentation time
			const int64_t videoLeadMillis = m_lastVideoPtsMillis >= 0 && m_lastScrMillis >= 0
				                                ? std::clamp<int64_t>(m_lastVideoPtsMillis - m_lastScrMillis, 0, 3000)
				                                : 0;
			if (m_timeline.valid()) {
				return m_timeline.findPosition(file, std::max<int64_t>(0, targetMillis - videoLeadMillis));
			}
			return m_firstScrMillis >= 0 ? estimateBytePosition(targetMillis + m_firstScrMillis) : -1;
		}

		// Corrupt and unsupported input this has met
		Diagnostics &diagnostics() { return m_diagnostics; }
		[[nodiscard]] const Diagnostics &diagnostics() const { return m_diagnostics; }

		void setVideoDecoder(MStream::IWriteByteStream *decoder) {
			m_ptrVideoDecoderStream = decoder;
		}

		// Decoder for MPEG audio streams
		void setAudioDecoder(MStream::IWriteByteStream *decoder) {
			m_ptrAudioDecoderStream = decoder;
		}

		// Decoder for A/52 audio, in private stream 1. Without one, those streams are skipped.
		void setA52AudioDecoder(MStream::IWriteByteStream *decoder) {
			m_ptrA52DecoderStream = decoder;
		}

		// Decoder for DVD LPCM audio, in private stream 1. Without one, those streams are skipped.
		void setLpcmAudioDecoder(MStream::IWriteByteStream *decoder) {
			m_ptrLpcmDecoderStream = decoder;
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
#if MD_MPEG_ENABLE_MPEG2
			if (m_buffer.peek(1) >> 6 == 1) {
				decodeMpeg2PackHeader();
				return;
			}
#endif
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
			setPackTiming(system_clock_reference_base_millis, program_mux_rate);
		}

		/**
		 * Decides whether to decode a packet of audio stream code. The lowest numbered stream wins, as DVDs
		 * number their main soundtrack first. A file can hold several programs one after another with different
		 * audio streams (DVD .vob files often do), so when the selected stream stops, for about half a second of
		 * packs, while another of its kind carries on, the lowest numbered of those takes over. Only streams of
		 * the kind first selected (MPEG audio, A/52 or LPCM) are switched to, as each has its own decoder and
		 * output.
		 * @return true if the packet is of the selected stream
		 */
		bool selectAudioStream(const uint32_t code) {
			constexpr uint32_t MISSING_PACKS = 256;
			if (m_forcedAudioStreamCode) {
				if (code != m_forcedAudioStreamCode) {
					return false;
				}
				if (m_audioStreamCode && m_audioStreamCode != code
				    && audioDecoderFor(code) == audioDecoderFor(m_audioStreamCode)) {
					audioDecoderFor(code)->discontinuity();
				}
				m_audioStreamCode = code;
				return true;
			}
			if (!m_audioStreamCode || code == m_audioStreamCode) {
				m_audioStreamCode = code;
				m_packsSinceSelectedAudio = 0;
				m_lowestOtherAudioStreamCode = 0;
				return true;
			}
			if (audioDecoderFor(code) != audioDecoderFor(m_audioStreamCode)) {
				return false;
			}
			if (!m_lowestOtherAudioStreamCode || code < m_lowestOtherAudioStreamCode) {
				m_lowestOtherAudioStreamCode = code;
			}
			if (code < m_audioStreamCode
			    || (m_packsSinceSelectedAudio >= MISSING_PACKS && code == m_lowestOtherAudioStreamCode)) {
				m_audioStreamCode = code;
				m_packsSinceSelectedAudio = 0;
				m_lowestOtherAudioStreamCode = 0;
				// What the decoder has buffered of the previous stream isn't continued by this one
				audioDecoderFor(code)->discontinuity();
				return true;
			}
			return false;
		}

		// The decoder of an audio stream code: MPEG audio (0x1C0-0x1DF), or a private stream 1 sub-stream: A/52
		// (0x80-0x87) or LPCM (0xA0-0xA7)
		[[nodiscard]] MStream::IWriteByteStream *audioDecoderFor(const uint32_t code) const {
			if (code <= 0xFFFF) {
				return m_ptrAudioDecoderStream;
			}
			return (code & 0xF8) == 0xA0 ? m_ptrLpcmDecoderStream : m_ptrA52DecoderStream;
		}

		void setPackTiming(const int32_t scrMillis, const uint32_t programMuxRate) {
			m_lastScrMillis = scrMillis;
			if (m_timeline.valid() && m_lastPackPosition >= 0) {
				m_timelineOffsetMillis = m_timeline.timelineAt(m_lastPackPosition, scrMillis) - scrMillis;
				m_timelineOffsetKnown = true;
			}
			if (m_firstPackPosition < 0) {
				m_firstPackPosition = m_lastPackPosition;
				m_firstScrMillis = m_lastScrMillis;
			}
			m_muxRateBytesPerSecond = programMuxRate * 50; // units of 50 bytes/s
		}

#if MD_MPEG_ENABLE_MPEG2
		// '01' SCR[32..30] marker SCR[29..15] marker SCR[14..0] marker SCR_ext(9) marker, 22:program_mux_rate,
		// 2 markers, 5 reserved, 3:pack_stuffing_length, then that many stuffing bytes
		void decodeMpeg2PackHeader() {
			const uint32_t high = m_buffer.pull(3);
			const uint32_t low = m_buffer.pull(3);
			// high: 01 s32..s30 m s29..s28 | s27..s20 | s19..s15 m s14..s13. low: s12..s5 | s4..s0 m e8..e7 | e6..e0 m
			if (!(high & (1 << 18)) || !(high & (1 << 2)) || !(low & (1 << 10)) || !(low & 1)) {
				throw CorruptPacketException("Marker bit not 1");
			}
			const uint64_t scr = static_cast<uint64_t>((high >> 19) & 7) << 30
			                     | static_cast<uint64_t>((high >> 3) & 0x7FFF) << 15
			                     | static_cast<uint64_t>((high & 3) << 13 | (low >> 11));
			const uint32_t muxRate = m_buffer.pull(3);
			if ((muxRate & 3) != 3) {
				throw CorruptPacketException("Marker bit not 1");
			}
			const uint32_t stuffing = m_buffer.pull(1) & 7;
			m_buffer.skip(stuffing);
			m_streamingPacketBytesRemaining = 0;
			setPackTiming(static_cast<int32_t>(scr / 90), muxRate >> 2);
		}

		// After the packet length: '10' and flags, PTS_DTS_flags and more flags, PES_header_data_length, the fields
		// those flags select, then stuffing up to the header length
		int32_t decodeMpeg2PesHeader(const bool audio) {
			pull(1);
			const uint32_t ptsDtsFlags = pull(1) >> 6;
			const uint32_t headerLength = pull(1);
			if (headerLength > static_cast<uint32_t>(m_streamingPacketBytesRemaining)) {
				throw CorruptPacketException("PES header longer than the packet");
			}
			uint32_t read = 0;
			int32_t pts = -1;
			if (ptsDtsFlags & 2) {
				if (headerLength < 5) {
					throw CorruptPacketException("PES header too short for its PTS");
				}
				m_bitsLeft = m_bitCount = 0;
				// '0010' or '0011', then the PTS
				pullBits(4);
				pts = decodeTime();
				read = 5;
				if (!audio) m_lastVideoPtsMillis = pts;
			}
			skip(headerLength - read);
			return pts;
		}

		// The PES header has been read: an A/52 packet starts with its sub-stream id (0x80 to 0x87), the number of
		// frames that start in it and the position of the first, which aren't needed as the decoder finds frames
		// itself. An LPCM packet (0xA0 to 0xA7) has the same fields and 3 more, which its decoder reads, as it needs
		// them to find frames. Other sub-streams (DTS, SDDS) are skipped.
		void decodePrivateStream1Header() {
			m_ptrCurrentDecoderStream = &m_nullDecoderStream;
			const int32_t pts = decodeStreamHeader(true);
			if (m_streamingPacketBytesRemaining < 1) {
				return;
			}
			const uint32_t subStream = peek();
			if ((subStream & 0xE0) == 0x20) {
				// Sub-picture: just the sub-stream id before the data
				if (m_ptrSubpictureDecoderStream && static_cast<int>(subStream) == m_subpictureStreamId) {
					skip(1);
					m_ptrCurrentDecoderStream = m_ptrSubpictureDecoderStream;
					setPacketPts(pts);
				}
				return;
			}
			const bool lpcm = (subStream & 0xF8) == 0xA0;
			if ((subStream & 0xF8) != 0x80 && !lpcm) {
				return;
			}
			const uint32_t code = PRIVATE_STREAM_1_START_CODE << 8 | subStream;
			MStream::IWriteByteStream *decoder = audioDecoderFor(code);
			if (m_streamingPacketBytesRemaining < (lpcm ? 7u : 4u) || decoder == &m_nullDecoderStream
			    || !selectAudioStream(code)) {
				return;
			}
			skip(lpcm ? 1 : 4);
			m_ptrCurrentDecoderStream = m_ptrSelectedAudioDecoderStream = decoder;
			if (lpcm) {
				decoder->startPacket(m_streamingPacketBytesRemaining);
			}
			setPacketPts(pts);
		}
#endif

		// Gives the current stream the packet's PTS, if it has one
		void setPacketPts(const int32_t pts) {
			if (pts >= 0) {
				m_ptrCurrentDecoderStream->setPts(pts);
			}
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

		// Reads the packet length and PES header, returns the PTS in ms or -1 if there is none
		int32_t decodeStreamHeader(const bool audio) {
			m_streamingPacketBytesRemaining = m_buffer.pull(2);

#if MD_MPEG_ENABLE_MPEG2
			if (peek() >> 6 == 2) {
				return decodeMpeg2PesHeader(audio);
			}
#endif

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
					return -1;
				}
				case 0x02: {
					const int32_t pts = decodeTime();
					if (!audio) m_lastVideoPtsMillis = pts;
					return pts;
				}
				case 0x03: {
					const int32_t pts = decodeTime(); // 36 bits
					if (pullBits(4) != 1) {
						throw CorruptPacketException("Invalid pts marker bit");
					}
					if (!audio) m_lastVideoPtsMillis = pts;
					decodeTime(); // DTS, not used
					return pts;
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

		for (size_t i = tail.size() >= 10 ? tail.size() - 10 : 0; i-- > 0;) {
			int64_t scrMillis;
			if (tail[i] == 0 && tail[i + 1] == 0 && tail[i + 2] == 1 && tail[i + 3] == 0xBA
			    && packHeaderScr(&tail[i + 4], scrMillis)) {
				return {static_cast<int64_t>(start + i), scrMillis};
			}
		}
		return {-1, -1};
	}
}
