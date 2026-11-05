#pragma once

#include <cmath>
#include <cstdint>

/**
 * Turns per-picture PTS from the video decoder into a monotonic display timeline.
 *
 * Pictures without a PTS (the stream only has to give one every so often) are placed one frame
 * period after the previous picture. A jump over 1 s is a stream discontinuity (e.g. concatenated files,
 * where PTS restart); the timeline stays continuous across it, matching the audio clock, which counts
 * samples played.
 */
class DisplayTimestamper {
	double m_lastPtsMs = -1;
	double m_offsetMs = 0;

public:
	/**
	 * @param decoderPtsMs Picture PTS, or -1 if it has none
	 * @param framePeriodMs Time between frames
	 * @return Presentation time in ms, or -1 if not known yet (no PTS seen since the start or a reset);
	 *         such frames can't be timed and should be dropped
	 */
	int64_t next(const int64_t decoderPtsMs, const double framePeriodMs) {
		if (decoderPtsMs < 0) {
			if (m_lastPtsMs < 0) {
				return -1;
			}
			m_lastPtsMs += framePeriodMs;
		} else {
			const double ptsMs = static_cast<double>(decoderPtsMs) + m_offsetMs;
			if (m_lastPtsMs < 0) {
				m_lastPtsMs = ptsMs;
			} else {
				const double expectedMs = m_lastPtsMs + framePeriodMs;
				const double diffMs = ptsMs - expectedMs;
				if (std::abs(diffMs) > 1000.0) {
					m_offsetMs -= diffMs;
					m_lastPtsMs = expectedMs;
				} else {
					m_lastPtsMs = ptsMs;
				}
			}
		}
		return std::llround(m_lastPtsMs);
	}

	// Starts a new timeline, e.g. after seeking
	void reset() {
		m_lastPtsMs = -1;
		m_offsetMs = 0;
	}
};
