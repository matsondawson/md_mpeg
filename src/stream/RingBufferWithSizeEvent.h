#pragma once

#include <cstdint>
#include <utility>
#include <vector>
#include <vector>
#include <functional>
#include <iostream>

#include "MStream.h"
#include "../MException.h"

namespace MStream {
	/**
	 * Ring buffer that reports full once setSizeRequired() bytes are buffered, so the writer stops and
	 * calls executeCallbacks(), which runs the callback when exactly that many are buffered.
	 */
	template<uint32_t RingBufferSize>
	class RingBufferWithSizeEvent : public IWriteByteStream, public IReadByteStream {
		// PTS of the latest packet, -1 if none yet
		int32_t m_ptsMillis;

		std::vector<uint8_t> m_buffer;
		uint32_t m_readIndex;
		uint32_t m_writeIndex;

		uint32_t m_sizeRequired;
		std::function<void()> m_matchCallback;

	public:
		explicit RingBufferWithSizeEvent(std::function<void()> matchCallback = nullptr)
			: m_ptsMillis(-1), m_matchCallback(std::move(matchCallback))
			  , m_readIndex(0), m_writeIndex(0)
			  , m_buffer(RingBufferSize), m_sizeRequired(0) {
			// empty
		}

		[[nodiscard]] uint32_t size() const noexcept {
			return m_buffer.size();
		}

		// Discards all buffered data
		void clear() {
			m_readIndex = m_writeIndex = 0;
			m_ptsMillis = -1;
		}

		void setSizeRequired(const uint32_t sizeRequired) {
			m_sizeRequired = sizeRequired;
		}

		// ************ IWriteByteStream **********

		[[nodiscard]] int32_t getPts(const int32_t frameTimeMillis) override {
			return m_ptsMillis;
		}

		void setPts(const int32_t ptsMillis) override {
			m_ptsMillis = ptsMillis;
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
		}

		[[nodiscard]] bool isFull() override {
			if (used() == m_sizeRequired) {
				return true;
			}
			// One slot stays empty, to tell full from empty
			auto temp = m_writeIndex + 1;
			if (temp >= size()) temp = 0;
			return temp == m_readIndex;
		}

		void executeCallbacks() {
			if (used() == m_sizeRequired) {
				m_matchCallback();
			}
		}

		void flush() override {
			if (m_matchCallback) {
				m_matchCallback();
			}
		}

		// ************ IReadByteStream **********

		uint8_t nextByte() override {
			if (m_readIndex == m_writeIndex) {
				throw MStream::EndOfData();
			}
			auto result = m_buffer[m_readIndex++];
			if (m_readIndex == size()) {
				m_readIndex = 0;
			}

			return result;
		}

		bool isEmpty() override {
			return m_readIndex == m_writeIndex;
		}

		uint32_t used() const noexcept {
			return ((m_writeIndex < m_readIndex) ? m_writeIndex + size() : m_writeIndex) - m_readIndex;
		}

		uint32_t free() const noexcept {
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

		uint32_t peek(uint32_t bytes) {
			uint32_t markIndex = m_readIndex;
			auto result = pull(bytes);
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
