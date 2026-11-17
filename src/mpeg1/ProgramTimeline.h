#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "../stream/FileStream.h"

// Set to 0 to build without MPEG-2 video, MPEG-2 program streams and A/52 audio, see MpegProgramStream.h
#ifndef MD_MPEG_ENABLE_MPEG2
#define MD_MPEG_ENABLE_MPEG2 1
#endif

namespace Mpeg1 {
	/**
	 * Reads the SCR of a pack header in ms.
	 * @param b The 6 bytes after the pack start code 00 00 01 BA
	 * @return false if they aren't a valid MPEG-1 (or, with MPEG-2 enabled, MPEG-2) pack header
	 */
	inline bool packHeaderScr(const uint8_t *b, int64_t &scrMillis) {
#if MD_MPEG_ENABLE_MPEG2
		// MPEG-2: '01' SCR[32..30] marker SCR[29..28] / [27..20] / [19..15] marker [14..13] / [12..5] / [4..0] marker
		if ((b[0] >> 6) == 1) {
			if (!(b[0] & 4) || !(b[2] & 4) || !(b[4] & 4) || !(b[5] & 1)) {
				return false;
			}
			const uint64_t scr = static_cast<uint64_t>((b[0] >> 3) & 7) << 30
			                     | static_cast<uint64_t>(b[0] & 3) << 28
			                     | static_cast<uint64_t>(b[1]) << 20
			                     | static_cast<uint64_t>(b[2] >> 3) << 15
			                     | static_cast<uint64_t>(b[2] & 3) << 13
			                     | static_cast<uint64_t>(b[3]) << 5
			                     | static_cast<uint64_t>(b[4] >> 3);
			scrMillis = static_cast<int64_t>(scr / 90);
			return true;
		}
#endif
		// MPEG-1: '0010' SCR[32..30] marker, SCR[29..15] marker, SCR[14..0] marker
		if ((b[0] >> 4) != 2 || !(b[0] & 1) || !(b[2] & 1) || !(b[4] & 1)) {
			return false;
		}
		const uint64_t scr = static_cast<uint64_t>((b[0] >> 1) & 7) << 30
		                     | static_cast<uint64_t>(((b[1] << 8) | b[2]) >> 1) << 15
		                     | static_cast<uint64_t>(((b[3] << 8) | b[4]) >> 1);
		scrMillis = static_cast<int64_t>(scr / 90);
		return true;
	}

	/**
	 * Maps byte positions in a program stream file to a timeline running from 0 at the first pack to the
	 * duration at the last, and back, from the SCR of packs sampled across the file.
	 *
	 * The SCR alone isn't a timeline: a file can hold several programs one after another, each starting its
	 * clock again (DVD .vob files often do, as do concatenated files). Where the SCR between two samples
	 * doesn't advance in step with the bytes between them, the gap is bridged using the average byte rate.
	 */
	class ProgramTimeline {
	public:
		struct Point {
			int64_t position;
			int64_t scrMillis;
			int64_t timelineMillis;
		};

	private:
		std::vector<Point> m_points;
		// Average bytes per ms, of intervals where the SCR runs normally
		double m_bytesPerMillis = 0;
		// By interval: its SCR advances in step with its bytes, so it is a single program
		std::vector<bool> m_continuous;

		// Finds the first valid pack header in buffer, returns its offset or -1
		static int64_t findPack(const std::vector<uint8_t> &buffer, int64_t &scrMillis) {
			for (size_t i = 0; i + 10 <= buffer.size(); i++) {
				if (buffer[i] == 0 && buffer[i + 1] == 0 && buffer[i + 2] == 1 && buffer[i + 3] == 0xBA
				    && packHeaderScr(&buffer[i + 4], scrMillis)) {
					return static_cast<int64_t>(i);
				}
			}
			return -1;
		}

		static std::vector<uint8_t> read(MStream::FileStreamRead &file, const int64_t position, const size_t bytes) {
			std::vector<uint8_t> buffer;
			buffer.reserve(bytes);
			file.seek(static_cast<size_t>(position));
			while (buffer.size() < bytes && !file.isEmpty()) {
				buffer.push_back(file.nextByte());
			}
			return buffer;
		}

		// Bytes per ms of interval i, from its SCR, or 0 if the SCR doesn't advance
		[[nodiscard]] double intervalRate(const size_t i) const {
			const int64_t scr = m_points[i + 1].scrMillis - m_points[i].scrMillis;
			return scr > 0 ? static_cast<double>(m_points[i + 1].position - m_points[i].position) / scr : 0;
		}

		// Index of the interval holding position (the last point at or before it, but not the last point)
		[[nodiscard]] size_t intervalAt(const int64_t position) const {
			const auto next = std::upper_bound(m_points.begin(), m_points.end(), position,
			                                   [](const int64_t p, const Point &point) { return p < point.position; });
			const auto index = static_cast<size_t>(std::max<std::ptrdiff_t>(next - m_points.begin() - 1, 0));
			return std::min(index, m_points.size() - 2);
		}

		[[nodiscard]] double localRate(const size_t interval) const {
			return m_continuous[interval] ? intervalRate(interval) : m_bytesPerMillis;
		}

	public:
		/**
		 * Samples the file and builds the timeline. Leaves the file positioned at the start.
		 * @param samples Number of places sampled, more for a more precise map at the cost of reading more
		 */
		void build(MStream::FileStreamRead &file, const int samples = 128) {
			m_points.clear();
			m_continuous.clear();
			const auto size = static_cast<int64_t>(file.size());
			// A pack every 2 KB is usual, MPEG-1 packs can be longer
			constexpr size_t SEARCH_BYTES = 32 * 1024;
			for (int i = 0; i <= samples; i++) {
				const int64_t start = size * i / samples;
				int64_t scr;
				const auto buffer = read(file, i == samples ? std::max<int64_t>(0, size - 256 * 1024) : start,
				                         i == samples ? 256 * 1024 : SEARCH_BYTES);
				if (i == samples) {
					// The last pack in the file
					for (size_t j = buffer.size() >= 10 ? buffer.size() - 10 : 0; j-- > 0;) {
						if (buffer[j] == 0 && buffer[j + 1] == 0 && buffer[j + 2] == 1 && buffer[j + 3] == 0xBA
						    && packHeaderScr(&buffer[j + 4], scr)) {
							const int64_t position = std::max<int64_t>(0, size - 256 * 1024) + static_cast<int64_t>(j);
							if (m_points.empty() || position > m_points.back().position) {
								m_points.push_back({position, scr, 0});
							}
							break;
						}
					}
					continue;
				}
				const int64_t offset = findPack(buffer, scr);
				if (offset >= 0 && (m_points.empty() || start + offset > m_points.back().position)) {
					m_points.push_back({start + offset, scr, 0});
				}
			}
			file.seek(0);
			if (m_points.size() < 2) {
				m_points.clear();
				return;
			}

			// The usual byte rate: the median of the intervals' rates
			std::vector<double> rates;
			for (size_t i = 0; i + 1 < m_points.size(); i++) {
				if (const double rate = intervalRate(i); rate > 0) {
					rates.push_back(rate);
				}
			}
			if (rates.empty()) {
				m_points.clear();
				return;
			}
			std::nth_element(rates.begin(), rates.begin() + static_cast<std::ptrdiff_t>(rates.size() / 2), rates.end());
			m_bytesPerMillis = rates[rates.size() / 2];

			// Variable bit rates stay well within a factor of 4 of the usual rate over a sample interval
			m_continuous.resize(m_points.size() - 1);
			for (size_t i = 0; i + 1 < m_points.size(); i++) {
				const double rate = intervalRate(i);
				m_continuous[i] = rate > m_bytesPerMillis / 4 && rate < m_bytesPerMillis * 4;
				const int64_t elapsed = m_continuous[i]
					                        ? m_points[i + 1].scrMillis - m_points[i].scrMillis
					                        : std::llround((m_points[i + 1].position - m_points[i].position) / m_bytesPerMillis);
				m_points[i + 1].timelineMillis = m_points[i].timelineMillis + elapsed;
			}
		}

		[[nodiscard]] bool valid() const {
			return m_points.size() >= 2;
		}

		[[nodiscard]] int64_t durationMillis() const {
			return valid() ? m_points.back().timelineMillis : -1;
		}

		[[nodiscard]] const std::vector<Point> &points() const {
			return m_points;
		}

		/**
		 * Timeline time of a pack, from its position and SCR. Within a single program this is exact; across the
		 * start of a new program, the SCR of the program the pack belongs to is followed.
		 */
		[[nodiscard]] int64_t timelineAt(const int64_t position, const int64_t scrMillis) const {
			if (!valid()) {
				return 0;
			}
			const size_t i = intervalAt(position);
			const Point &a = m_points[i];
			const Point &b = m_points[i + 1];
			const double rate = localRate(i);
			// The SCR expected from the bytes, with leeway for variable bit rates
			const auto fits = [rate](const int64_t scrDifference, const int64_t bytes) {
				const double expected = static_cast<double>(bytes) / rate;
				return std::abs(static_cast<double>(scrDifference) - expected) <= 2000.0 + expected / 2;
			};
			if (fits(scrMillis - a.scrMillis, position - a.position)) {
				return a.timelineMillis + (scrMillis - a.scrMillis);
			}
			if (fits(b.scrMillis - scrMillis, b.position - position)) {
				return b.timelineMillis - (b.scrMillis - scrMillis);
			}
			return timelineAtPosition(position);
		}

		// Timeline time at a byte position, interpolated between the samples
		[[nodiscard]] int64_t timelineAtPosition(const int64_t position) const {
			if (!valid()) {
				return 0;
			}
			const size_t i = intervalAt(position);
			const Point &a = m_points[i];
			const Point &b = m_points[i + 1];
			const double fraction = static_cast<double>(position - a.position) / static_cast<double>(b.position - a.position);
			return a.timelineMillis + std::llround(fraction * static_cast<double>(b.timelineMillis - a.timelineMillis));
		}

		// Byte position of a timeline time, interpolated between the samples
		[[nodiscard]] int64_t positionAt(const int64_t timelineMillis) const {
			if (!valid()) {
				return 0;
			}
			const auto next = std::upper_bound(m_points.begin(), m_points.end(), timelineMillis,
			                                   [](const int64_t t, const Point &point) { return t < point.timelineMillis; });
			size_t i = static_cast<size_t>(std::max<std::ptrdiff_t>(next - m_points.begin() - 1, 0));
			i = std::min(i, m_points.size() - 2);
			const Point &a = m_points[i];
			const Point &b = m_points[i + 1];
			if (b.timelineMillis <= a.timelineMillis) {
				return a.position;
			}
			const double fraction = std::clamp(static_cast<double>(timelineMillis - a.timelineMillis)
			                                   / static_cast<double>(b.timelineMillis - a.timelineMillis), 0.0, 1.0);
			return a.position + std::llround(fraction * static_cast<double>(b.position - a.position));
		}

		/**
		 * Finds the byte position of a pack at timeline time timelineMillis: interpolates between the samples, then
		 * reads the packs there and corrects, for variable bit rates. Leaves the file at an undefined position.
		 * @return position, or -1 if the timeline isn't known
		 */
		int64_t findPosition(MStream::FileStreamRead &file, const int64_t timelineMillis) const {
			if (!valid()) {
				return -1;
			}
			int64_t position = positionAt(timelineMillis);
			for (int attempt = 0; attempt < 4; attempt++) {
				int64_t scr;
				const auto buffer = read(file, position, 32 * 1024);
				const int64_t offset = findPack(buffer, scr);
				if (offset < 0) {
					break;
				}
				const int64_t packPosition = position + offset;
				const int64_t error = timelineMillis - timelineAt(packPosition, scr);
				if (std::abs(error) < 250) {
					return packPosition;
				}
				const double rate = localRate(intervalAt(packPosition));
				position = std::clamp<int64_t>(packPosition + std::llround(static_cast<double>(error) * rate), 0,
				                               static_cast<int64_t>(file.size()));
			}
			return position;
		}
	};
}
