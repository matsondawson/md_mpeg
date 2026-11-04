#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include "Time.h"

/**
 * Master playback clock, in milliseconds on the stream's PTS timeline.
 *
 * Audio drives the clock whenever it is actually playing. Otherwise (no audio stream, audio not started
 * yet, audio device unavailable, audio starved or finished) it falls back to the wall clock. When it
 * falls back it carries on from where the audio clock left off, so video doesn't jump.
 */
class PlaybackClock {
public:
	// Returns the audio position in ms, or nullopt when audio isn't currently playing
	using AudioSource = std::function<std::optional<int64_t>()>;

	void setAudioSource(AudioSource audioSource) {
		m_audioSource = std::move(audioSource);
	}

	// Starts the wall clock at ptsMs, unless the clock is already running
	void startAt(const int64_t ptsMs) {
		if (!m_started) {
			m_wallAnchorMs = now() - ptsMs;
			m_pausedAtMs = ptsMs;
			m_started = true;
		}
	}

	// Stops the clock until the next startAt() or audio position, e.g. after seeking. Pause is kept.
	void reset() {
		m_started = false;
		m_usingAudio = false;
	}

	// While paused the clock holds its value
	void setPaused(const bool paused) {
		if (paused == m_paused) {
			return;
		}
		if (paused) {
			m_pausedAtMs = m_started ? ms() : 0;
			m_paused = true;
		} else {
			m_paused = false;
			m_wallAnchorMs = now() - m_pausedAtMs;
			// Continue on the wall clock from the paused position until audio reports again
			m_usingAudio = false;
		}
	}

	[[nodiscard]] bool paused() const {
		return m_paused;
	}

	[[nodiscard]] bool started() const {
		return m_started;
	}

	[[nodiscard]] bool usingAudio() const {
		return m_usingAudio;
	}

	int64_t ms() {
		if (m_paused) {
			return m_pausedAtMs;
		}
		if (m_audioSource) {
			if (const auto audioMs = m_audioSource()) {
				m_usingAudio = true;
				m_started = true;
				m_lastAudioMs = *audioMs;
				return *audioMs;
			}
		}
		if (m_usingAudio) {
			// Audio stopped driving the clock, continue from where it was
			m_wallAnchorMs = now() - m_lastAudioMs;
			m_usingAudio = false;
		}
		return now() - m_wallAnchorMs;
	}

private:
	AudioSource m_audioSource;
	bool m_started = false;
	bool m_usingAudio = false;
	int64_t m_wallAnchorMs = 0;
	int64_t m_lastAudioMs = 0;
	bool m_paused = false;
	int64_t m_pausedAtMs = 0;
};
