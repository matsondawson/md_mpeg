#pragma once

#include "../common.h"

#include <cassert>
#include <stdexcept>
#include "MStream.h"

namespace MStream {
	class ReadBitStream {
#ifdef MDEBUG
	public:
		uint32_t m_readTotal = 0;
#endif
	private:
		IReadByteStream* m_stream;

		uint32_t next_data;
		uint32_t next_data_bitcount;

	public:
		explicit ReadBitStream(IReadByteStream* stream)
			: m_stream(stream), next_data(0), next_data_bitcount(0) {
			// empty
		}

		// Yup, you can't copy me
		ReadBitStream(const ReadBitStream& cfileBuffer) = delete;

		/**
		 * Pulls bits, most significant first.
		 * @param bitcount 1..25, or up to 32 if byte aligned
		 * @throws EndOfData if the stream runs out
		 */
		uint32_t pull(const uint32_t bitcount) {
			assert(bitcount > 0 && bitcount <= 32);

			while (next_data_bitcount < bitcount) {
				next_data |= m_stream->nextByte() << (32 - 8 - next_data_bitcount);
#ifdef MDEBUG
				m_readTotal++;
#endif
				next_data_bitcount += 8;
				assert(next_data_bitcount <= 32);
			}

			uint32_t result = SHR(next_data, 32 - bitcount);
			next_data_bitcount -= bitcount;
			next_data = SHL(next_data, bitcount);
			return result;
		}

		int32_t pulls(const uint32_t bitcount) {
			return static_cast<int32_t>(pull(bitcount));
		}

		void push(uint32_t data, uint32_t databitcount) {
			uint32_t available = 32 - next_data_bitcount;
			if (available >= databitcount) {
				// add all the data to next_data
				next_data = SHR(next_data, databitcount) | SHL(data, 32 - databitcount);
				next_data_bitcount += databitcount;
				return;
			}

			throw std::runtime_error("Not enough space to push");
		}

		// Same limits as pull(), without consuming the bits
		uint32_t peek(uint32_t bitcount) {
			assert(bitcount > 0);
			assert(bitcount <= 32);

			while (next_data_bitcount < bitcount) {
				next_data |= m_stream->nextByte() << (32 - 8 - next_data_bitcount);
#ifdef MDEBUG
				m_readTotal++;
#endif
				next_data_bitcount += 8;
				assert(next_data_bitcount <= 32);
			}

			return SHR(next_data, 32 - bitcount);
		}

		// Discard any buffered bits
		void reset() {
			next_data = 0;
			next_data_bitcount = 0;
		}

		// Drops bits up to the next byte boundary, returns how many
		uint32_t align8() {
			const uint32_t dropped_bits = next_data_bitcount & 7;
			//std::cout << "next_data_bitcount=" << next_data_bitcount << " dropbits=" << dropped_bits << std::endl;
			// No need to use SHL as dropped_bits is always less than 8
			next_data <<= dropped_bits;
			next_data_bitcount -= dropped_bits;
			return dropped_bits;
		}

		void skip(uint32_t bitcount) {
			// Attempt to skip quickly
			if (next_data_bitcount >= bitcount) {
				next_data_bitcount -= bitcount;
				next_data = SHL(next_data, bitcount);
				return;
			}

			// consume buffered bits
			if (next_data_bitcount) {
				next_data = SHL(next_data, next_data_bitcount);
				bitcount -= next_data_bitcount;
				next_data_bitcount = 0;
			}

			// consume bytes
			while (bitcount >= 8) {
				m_stream->nextByte();
				bitcount -= 8;
#ifdef MDEBUG
				m_readTotal++;
#endif
			}

			// consume remainder bits
			if (bitcount) {
				pull(bitcount);
			}
		}
	};
}
