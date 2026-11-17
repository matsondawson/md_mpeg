#pragma once

#include <algorithm>
#include <cstdint>

/**
 * Detects an audio output that has stopped asking for samples while it is meant to be playing.
 *
 * Some outputs (seen with a Bluetooth headset under PipeWire) stay open and report no error, but stop
 * calling the audio callback. The audio clock then stands still, and video timed from it waits forever.
 * Once no callback has come for STALL_MS while playing, the output counts as stalled until reset().
 */
class AudioStallDetector {
public:
	// Longer than any device period, and than an output (e.g. Bluetooth) takes to start
	static constexpr int64_t STALL_MS = 500;

	// Playback started or resumed at nowMs: callbacks are only expected from then on
	void started(const int64_t nowMs) {
		m_expectedSinceMs = nowMs;
	}

	// Forgets a stall, e.g. when the output is restarted after a seek
	void reset() {
		m_expectedSinceMs = 0;
		m_stalled = false;
	}

	/**
	 * Call while playing and not paused.
	 * @param lastCallbackMs time of the last callback, 0 if none yet
	 * @return true once the output has stalled
	 */
	bool update(const int64_t nowMs, const int64_t lastCallbackMs) {
		if (!m_stalled && nowMs - std::max(m_expectedSinceMs, lastCallbackMs) > STALL_MS) {
			m_stalled = true;
		}
		return m_stalled;
	}

	[[nodiscard]] bool stalled() const {
		return m_stalled;
	}

private:
	int64_t m_expectedSinceMs = 0;
	bool m_stalled = false;
};
