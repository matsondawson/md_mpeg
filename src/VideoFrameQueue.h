#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "Deinterlace.h"

/**
 * Decoded RGB frames waiting for their presentation time.
 *
 * Decoding into a queue lets the demuxer keep running while frames wait to be shown. That matters
 * because a program stream muxes video ahead of the audio it plays with: if presentation blocked
 * the demuxer, the audio packets behind the video would not be read in time and audio would run dry.
 *
 * An interlaced frame is deinterlaced (see Deinterlace.h): its two fields are shown one after the other, half a
 * frame period apart, each made into a full picture. That keeps the motion of interlaced video smooth, at twice
 * the frame rate, without the combing of showing both fields at once. Yadif, the default, also uses the frames
 * before and after, so the queue keeps the frame last shown.
 */
class VideoFrameQueue {
public:
	struct QueuedFrame {
		int64_t ptsMs = 0;
		// Time on the file's timeline (see MpegProgramStream::timelineMillis), -1 if not known
		int64_t timelineMs = -1;
		uint32_t width = 0;
		uint32_t height = 0;
		// Pixel height / width, 0 if unknown
		float pelAspect = 0;
		// Both fields, woven together
		std::unique_ptr<uint32_t[]> rgb;
		size_t capacity = 0;
		// Shown field by field (see setInterlaced): which field is first, and when the second is due
		bool interlaced = false;
		bool topFieldFirst = true;
		int64_t secondFieldMs = 0;
		// Fields of it shown so far
		int fieldsShown = 0;
	};

	// What to show of the front frame
	enum class Show {
		Frame,
		FirstField,
		SecondField
	};

	explicit VideoFrameQueue(const size_t maxFrames) : m_maxFrames(maxFrames) {
		// empty
	}

	[[nodiscard]] bool empty() const { return m_frames.empty(); }
	[[nodiscard]] bool full() const { return m_frames.size() >= m_maxFrames; }
	[[nodiscard]] const QueuedFrame &front() const { return m_frames.front(); }
	[[nodiscard]] uint64_t droppedFrames() const { return m_droppedFrames; }

	// How interlaced frames are shown (see the class comment); yadif by default
	void setDeinterlaceMode(const Deinterlace::Mode mode) { m_deinterlaceMode = mode; }
	[[nodiscard]] Deinterlace::Mode deinterlaceMode() const { return m_deinterlaceMode; }

	/**
	 * Adds a frame to the back of the queue.
	 * @return RGB buffer of width * height pixels for the caller to fill
	 */
	uint32_t *push(const int64_t ptsMs, const uint32_t width, const uint32_t height, const float pelAspect,
	               const int64_t timelineMs = -1) {
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
		frame.timelineMs = timelineMs;
		frame.width = width;
		frame.height = height;
		frame.pelAspect = pelAspect;
		frame.interlaced = false;
		frame.fieldsShown = 0;
		m_frames.push_back(std::move(frame));
		return m_frames.back().rgb.get();
	}

	// The frame last pushed is interlaced: show its fields fieldMs apart, starting with the top field if topFieldFirst
	void setInterlaced(const bool topFieldFirst, const int64_t fieldMs) {
		auto &frame = m_frames.back();
		frame.interlaced = true;
		frame.topFieldFirst = topFieldFirst;
		frame.secondFieldMs = frame.ptsMs + fieldMs;
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

	/**
	 * What to show of the front frame at clockMs, if anything new is due (see frontIsDue). Call presented()
	 * after showing it.
	 */
	bool nextToShow(const int64_t clockMs, Show &show) {
		if (!frontIsDue(clockMs)) {
			return false;
		}
		const auto &frame = m_frames.front();
		if (!frame.interlaced || m_deinterlaceMode == Deinterlace::Mode::Off) {
			show = Show::Frame;
			return true;
		}
		const int field = clockMs >= frame.secondFieldMs ? 2 : 1;
		if (field <= frame.fieldsShown) {
			return false;
		}
		show = field == 1 ? Show::FirstField : Show::SecondField;
		return true;
	}

	// The front frame, or its first field, has been shown
	void presented(const Show show) {
		if (show == Show::FirstField) {
			m_frames.front().fieldsShown = 1;
		} else {
			pop();
		}
	}

	/**
	 * Writes the front frame as shown: the whole frame, or one field made into a full picture
	 * @param out width * height pixels
	 */
	void render(const Show show, uint32_t *out) const {
		const auto &frame = m_frames.front();
		const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
		if (show == Show::Frame) {
			std::copy(frame.rgb.get(), frame.rgb.get() + pixels, out);
			return;
		}
		const bool first = show == Show::FirstField;
		const bool top = first == frame.topFieldFirst;
		if (m_deinterlaceMode == Deinterlace::Mode::Bob) {
			Deinterlace::bobField(frame.rgb.get(), out, frame.width, frame.height, top);
			return;
		}
		// The frames either side, if they are the same size; else this one stands in
		const auto sameSize = [&](const QueuedFrame &other) {
			return other.width == frame.width && other.height == frame.height;
		};
		const uint32_t *previous = m_previousValid && sameSize(m_previous) ? m_previous.rgb.get() : frame.rgb.get();
		const uint32_t *next = m_frames.size() >= 2 && sameSize(m_frames[1]) ? m_frames[1].rgb.get() : frame.rgb.get();
		Deinterlace::yadifField(previous, frame.rgb.get(), next, out, frame.width, frame.height, top, first);
	}

	// Drops all queued frames, e.g. for a seek: the next frame shown has no frame before it
	void clear() {
		while (!m_frames.empty()) {
			pop();
		}
		m_previousValid = false;
	}

	// Removes the front frame, which becomes the frame before the next one
	void pop() {
		if (m_previous.rgb) {
			m_spare.push_back(std::move(m_previous));
		}
		m_previous = std::move(m_frames.front());
		m_previousValid = true;
		m_frames.pop_front();
	}

private:
	size_t m_maxFrames;
	std::deque<QueuedFrame> m_frames;
	std::vector<QueuedFrame> m_spare;
	uint64_t m_droppedFrames = 0;
	Deinterlace::Mode m_deinterlaceMode = Deinterlace::Mode::Yadif;
	// The frame last removed, for yadif to compare with
	QueuedFrame m_previous;
	bool m_previousValid = false;
};
