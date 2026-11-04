#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

/**
 * Decoded RGB frames waiting for their presentation time.
 *
 * Decoding into a queue lets the demuxer keep running while frames wait to be shown. That matters
 * because a program stream muxes video ahead of the audio it plays with: if presentation blocked
 * the demuxer, the audio packets behind the video would not be read in time and audio would run dry.
 */
class VideoFrameQueue {
public:
	struct QueuedFrame {
		int64_t ptsMs = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		std::unique_ptr<uint32_t[]> rgb;
		size_t capacity = 0;
	};

	explicit VideoFrameQueue(const size_t maxFrames) : m_maxFrames(maxFrames) {
		// empty
	}

	[[nodiscard]] bool empty() const { return m_frames.empty(); }
	[[nodiscard]] bool full() const { return m_frames.size() >= m_maxFrames; }
	[[nodiscard]] const QueuedFrame &front() const { return m_frames.front(); }
	[[nodiscard]] uint64_t droppedFrames() const { return m_droppedFrames; }

	/**
	 * Adds a frame to the back of the queue.
	 * @return RGB buffer of width * height pixels for the caller to fill
	 */
	uint32_t *push(const int64_t ptsMs, const uint32_t width, const uint32_t height) {
		QueuedFrame frame;
		if (!m_spare.empty()) {
			frame = std::move(m_spare.back());
			m_spare.pop_back();
		}
		const size_t pixels = static_cast<size_t>(width) * height;
		if (frame.capacity < pixels) {
			frame.rgb.reset(new uint32_t[pixels]);
			frame.capacity = pixels;
		}
		frame.ptsMs = ptsMs;
		frame.width = width;
		frame.height = height;
		m_frames.push_back(std::move(frame));
		return m_frames.back().rgb.get();
	}

	/**
	 * Finds the frame to show at clockMs. Frames that are superseded by a later frame that is also due
	 * are dropped, which is how video catches up when it falls behind.
	 * @return true if front() should be shown now; call pop() after showing it
	 */
	bool frontIsDue(const int64_t clockMs) {
		while (m_frames.size() >= 2 && m_frames[1].ptsMs <= clockMs) {
			pop();
			m_droppedFrames++;
		}
		return !m_frames.empty() && m_frames.front().ptsMs <= clockMs;
	}

	// Drops all queued frames
	void clear() {
		while (!m_frames.empty()) {
			pop();
		}
	}

	void pop() {
		m_spare.push_back(std::move(m_frames.front()));
		m_frames.pop_front();
	}

private:
	size_t m_maxFrames;
	std::deque<QueuedFrame> m_frames;
	std::vector<QueuedFrame> m_spare;
	uint64_t m_droppedFrames = 0;
};
