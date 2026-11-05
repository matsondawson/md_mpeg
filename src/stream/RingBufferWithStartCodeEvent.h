#pragma once

#include <cstdint>
#include <deque>
#include <vector>
#include <functional>
#include <iostream>

#include "MStream.h"
#include "../MException.h"
#include <cassert>

namespace MStream {
	/**
	 * Ring buffer that can scan for a 4-byte start code as it is written.
	 *
	 * With scanForCode set, writeByte() runs the callback each time the code is written and at least two
	 * are unread. Two picture start codes mean one complete picture is buffered.
	 * With the default of 0xFFFFFFFF nothing is scanned, and there must be no callback.
	 */
	template <uint32_t RingBufferSize, uint32_t scanForCode = 0xFFFFFFFF>
	class RingBufferWithStartCodeEvent final : public IWriteByteStream, public IReadByteStream {

		// PTS handling: a packet's PTS belongs to the first scan code (picture) that starts in that packet.
		// Each scan code written is tagged with the pending PTS (or -1 for none), and the tag is handed
		// back when that scan code is read.
		int32_t m_pendingPtsMillis = -1;
		// Bytes written so far, and the count when the pending PTS arrived. A scan code belongs to the
		// packet its first byte is in, so one straddling the packet boundary doesn't take the new PTS.
		uint64_t m_bytesWritten = 0;
		uint64_t m_pendingPtsFromByte = 0;
		std::deque<int32_t> m_scanCodePts;
		int32_t m_lastReadScanCodePts = -1;

		std::vector<uint8_t> m_buffer;
		int32_t m_readIndex;
		int32_t m_writeIndex;
		int32_t m_markIndex;

		uint32_t m_scanCodeMatchCount;
		uint32_t m_scanCodeWriteMatchValue;
		uint32_t m_scanCodeReadMatchValue;
		std::function<void()> m_scanCodeMatchCallback;

	public:
		explicit RingBufferWithStartCodeEvent(std::function<void()> scanCodeMatchCallback = nullptr)
			: m_buffer(RingBufferSize)
			, m_readIndex(0), m_writeIndex(0)
			, m_markIndex(-1), m_scanCodeMatchCount(0)
			, m_scanCodeWriteMatchValue(~1), m_scanCodeReadMatchValue(~1), m_scanCodeMatchCallback(scanCodeMatchCallback) {
			assert((scanForCode == 0xFFFFFFFF) != !!scanCodeMatchCallback);
		}

		[[nodiscard]] uint32_t size() const noexcept {
			return m_buffer.size();
		}

		// Number of scan codes written but not yet read
		[[nodiscard]] uint32_t scanCodeCount() const noexcept {
			return m_scanCodeMatchCount;
		}

		// ************ IWriteByteStream **********

		/**
		 * @return PTS of the scan code most recently read, or -1 if its packet had none
		 */
		[[nodiscard]] int32_t getPts(const int32_t /*frameTimeMillis*/) override {
			return m_lastReadScanCodePts;
		}

		void setPts(const int32_t ptsMillis) override {
			m_pendingPtsMillis = ptsMillis;
			m_pendingPtsFromByte = m_bytesWritten;
		}

		// Discards all data and state
		void clear() {
			m_readIndex = m_writeIndex = 0;
			m_markIndex = -1;
			m_scanCodeMatchCount = 0;
			m_scanCodeWriteMatchValue = m_scanCodeReadMatchValue = ~1;
			m_pendingPtsMillis = -1;
			m_bytesWritten = m_pendingPtsFromByte = 0;
			m_scanCodePts.clear();
			m_lastReadScanCodePts = -1;
		}

		void writeByte(const uint8_t data) override {
			// buffer is full
			if (isFull()) {
			/*	if (true) {
					m_buffer.resize(m_buffer.size() + 4096);
				} else {*/
					cthrow(StringException, "Buffer is full");
				/* }*/
			};
			m_buffer[m_writeIndex++] = data;
			if (m_writeIndex == size()) {
				m_writeIndex = 0;
			}
			m_bytesWritten++;

			if (scanForCode != 0XFFFFFFFF) {
				m_scanCodeWriteMatchValue = (m_scanCodeWriteMatchValue << 8) | data;
				// event callback
				if (m_scanCodeWriteMatchValue == scanForCode) {
					m_scanCodeMatchCount++;
					// m_bytesWritten - 4 is the position of the scan code's first byte
					if (m_pendingPtsMillis >= 0 && m_bytesWritten - 4 >= m_pendingPtsFromByte) {
						m_scanCodePts.push_back(m_pendingPtsMillis);
						m_pendingPtsMillis = -1;
					} else {
						m_scanCodePts.push_back(-1);
					}

					if (m_scanCodeMatchCount >= 2) {
						m_scanCodeMatchCallback();
					}
				}
			}
		}

		[[nodiscard]] bool isFull() override {
			/*if (scanForCode != 0XFFFFFFFF) {
				if (m_scanCodeMatchCount >= 2) {
					m_scanCodeMatchCallback();
				}
			}*/
			// One slot stays empty, to tell full from empty
			auto temp = m_writeIndex + 1;
			if (temp >= size()) temp = 0;
			return temp == m_readIndex;
		}

		void executeCallbacks() override {
			/*if (m_scanCodeMatchCount >= 2) {
				m_scanCodeMatchCallback();
			}*/
		}

		void flush() override {
			/*if (m_scanCodeMatchCallback) {
				m_scanCodeMatchCallback();
			}*/
		}

		// ************ IReadByteStream **********

		uint8_t nextByte() override {
			if (m_readIndex == m_writeIndex) {
				throw MStream::EndOfData();
			}
			const auto result = m_buffer[m_readIndex++];
			if (m_readIndex == size()) {
				m_readIndex = 0;
			}

			if (scanForCode != 0XFFFFFFFF) {
				m_scanCodeReadMatchValue = (m_scanCodeReadMatchValue << 8) | result;
				// Read past a scan code: make its PTS current
				if (m_scanCodeReadMatchValue == scanForCode) {
					m_scanCodeMatchCount--;
					if (!m_scanCodePts.empty()) {
						m_lastReadScanCodePts = m_scanCodePts.front();
						m_scanCodePts.pop_front();
					}
				}
			}

			return result;
		}

		[[nodiscard]] bool isEmpty() override {
			return m_readIndex == m_writeIndex;
		}

		// ================ MARK ================

		// Remembers the read position for reset()
		void mark() noexcept {
			assert(m_markIndex == -1);
			m_markIndex = m_readIndex;
		}

		void markInc() noexcept {
			assert(m_markIndex != -1);
			m_markIndex++;
			if (m_markIndex == static_cast<int32_t>(size())) {
				m_markIndex = 0;
			}
		}

		void clearMark() noexcept {
			assert(m_markIndex != -1);
			m_markIndex = -1;
		}

		// Rewinds to the position saved by mark()
		void reset() noexcept {
			assert(m_markIndex != -1);
			m_readIndex = m_markIndex;
			m_markIndex = -1;
		}

		[[nodiscard]] uint32_t used() const noexcept {
			return ((m_writeIndex < m_readIndex) ? m_writeIndex + size() : m_writeIndex) - m_readIndex;
		}

		[[nodiscard]] uint32_t free() const noexcept {
			assert(used() < size());
			return size() - used() - 1; // -1: one slot stays empty, to tell full from empty
		}


		uint32_t pull(uint32_t bytes) {
			uint32_t result = 0;
			while (bytes--) {
				if (isEmpty()) {
					throw EndOfData();
				}
				result = (result << 8) | m_buffer[m_readIndex++];
				if (m_readIndex == size()) {
					m_readIndex = 0;
				}
			}
			return result;
		}

		uint32_t peek(const uint32_t bytes) {
			const auto markIndex = m_readIndex;
			const auto result = pull(bytes);
			m_readIndex = markIndex;
			return result;
		}

		// TODO optimize
		void skip(uint32_t bytes) {
			while (bytes--) {
				if (isEmpty()) {
					throw EndOfData();
				}
				m_readIndex++;
				if (m_readIndex == size()) {
					m_readIndex = 0;
				}
			}
		}
	};
};
