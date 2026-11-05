#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>

#include "MStream.h"
#include "../MException.h"

namespace MStream {
	/**
	 * Single-producer / single-consumer ring buffer.
	 * One thread may write while another reads (e.g. decoder thread writes, audio thread reads).
	 */
	template <class DataType, uint32_t RingBufferSize>
	class RingBufferTyped {
	private:
		std::vector<DataType> m_buffer;
		std::atomic<uint32_t> m_readIndex;
		std::atomic<uint32_t> m_writeIndex;

	public:
		RingBufferTyped()
			: m_buffer(RingBufferSize)
			, m_readIndex(0), m_writeIndex(0)  {
			// empty
		}

		[[nodiscard]] uint32_t size() const noexcept {
			return m_buffer.size();
		}

		// ************ Producer **********

		void writeByte(const DataType data) {
			const auto writeIndex = m_writeIndex.load(std::memory_order_relaxed);
			auto nextWriteIndex = writeIndex + 1;
			if (nextWriteIndex == size()) nextWriteIndex = 0;
			// buffer is full
			if (nextWriteIndex == m_readIndex.load(std::memory_order_acquire)) {
				cthrow(StringException, "Buffer is full");
			}
			m_buffer[writeIndex] = data;
			m_writeIndex.store(nextWriteIndex, std::memory_order_release);
		}

		[[nodiscard]] bool isFull() const {
			auto temp = m_writeIndex.load(std::memory_order_acquire) + 1;
			if (temp >= size()) temp = 0;
			return temp == m_readIndex.load(std::memory_order_acquire);
		}

		// ************ Consumer **********

		DataType nextByte() {
			const auto readIndex = m_readIndex.load(std::memory_order_relaxed);
			if (readIndex == m_writeIndex.load(std::memory_order_acquire)) {
				throw MStream::EndOfData();
			}
			auto result = m_buffer[readIndex];
			auto nextReadIndex = readIndex + 1;
			if (nextReadIndex == size()) nextReadIndex = 0;
			m_readIndex.store(nextReadIndex, std::memory_order_release);

			return result;
		}

		[[nodiscard]] bool isEmpty() const {
			return m_readIndex.load(std::memory_order_acquire) == m_writeIndex.load(std::memory_order_acquire);
		}

		[[nodiscard]] uint32_t used() const noexcept {
			const auto readIndex = m_readIndex.load(std::memory_order_acquire);
			const auto writeIndex = m_writeIndex.load(std::memory_order_acquire);
			return ((writeIndex < readIndex) ? writeIndex + size() : writeIndex) - readIndex;
		}

		[[nodiscard]] uint32_t free() const noexcept {
			assert(used() < size());
			return size() - used() - 1; // -1: one slot stays empty, to tell full from empty
		}
	};
};
