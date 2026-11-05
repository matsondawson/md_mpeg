#pragma once

#include "../common.h"
#include <exception>

namespace MStream {
	struct EndOfData : public std::exception {
		[[nodiscard]] const char* what() const throw () {
			return "End of data";
		}
	};

	class IReadByteStream {
	public:
		virtual ~IReadByteStream() = default;

		// Throws EndOfData when no data is available
		virtual uint8_t nextByte() = 0;
		[[nodiscard]] virtual bool isEmpty() = 0;
	};

	class IReadBitStream {
	public:
		virtual ~IReadBitStream() = default;

		// Throws EndOfData when no data is available
		[[nodiscard]] virtual uint32_t pull(int bitCount) = 0;
		[[nodiscard]] virtual uint32_t peek(int bitCount) = 0;
		virtual void skip(int bitCount) = 0;
	};

	class IWriteByteStream {
	public:
		virtual ~IWriteByteStream() = default;

		// frameTimeMillis isn't used by any implementation
		virtual int32_t getPts(int32_t frameTimeMillis) = 0;
		// Sets the PTS of the packet whose bytes are written next
		virtual void setPts(int32_t ptsMillis) = 0;
		virtual void writeByte(uint8_t data) = 0;
		// True if the stream can't take more data until the decoder makes room
		[[nodiscard]] virtual bool isFull() = 0;
		// Lets the decoder process what is buffered
		virtual void executeCallbacks() = 0;
		// End of input
		virtual void flush() = 0;
	};

	/*
	class IRingBuffer : public IWriteByteStream {
		virtual uint32_t pull(uint32_t byteCount) = 0;
		virtual uint32_t peek(uint32_t byteCount) = 0;
		virtual void skip(uint32_t count) = 0;
	};

	class IReadByteArray {
	public:
		virtual const bool isEmpty() const = 0;
		virtual uint32_t read(uint8_t *data, uint32_t length) = 0;
	};*/
}
