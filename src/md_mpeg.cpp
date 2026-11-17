#include "md_mpeg.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>

#include "DisplayTimestamper.h"
#include "PlaybackClock.h"
#include "Time.h"
#include "VideoFrameQueue.h"
#include "stream/MStream.h"
#include "stream/FileStream.h"
#include "stream/RingBufferWithStartCodeEvent.h"
#include "mpeg1/MpegProgramStream.h"
#include "mpeg1/Mpeg1VideoDecoder.h"
#include "mpeg1/AudioDecoders.h"
#include "sdl2/screenContext.h"

int main(int argc, char *argv[]) {
	if (argc < 2) {
		std::cerr << "Usage: " << argv[0] << " video.mpg\n";
		return 1;
	}

	ScreenContextRgbSDL sdl;
	sdl.setTitle(std::filesystem::path(argv[1]).filename().string() + " - md_mpeg");
	uint32_t screenWidth = 0;
	uint32_t screenHeight = 0;
	double screenAspect = 0;
	bool audioInited = false;

	const auto playbackStartMs = now();
	bool streamFailed = false;

	MStream::FileStreamRead fs(argv[1]);
	Mpeg1::MpegProgramStream mps;
	Mpeg1VideoDecoder videoDecoder;
	mps.setVideoDecoder(&videoDecoder);
	AudioDecoders audioDecoder;
	audioDecoder.attach(mps);
	mps.buildTimeline(fs);

	// Audio is the master clock when there is audio, otherwise wall time
	PlaybackClock clock;
	clock.setAudioSource([&] { return sdl.audioClockMs(); });

	DisplayTimestamper displayTimestamper;
	// 0.4-1 s of video at 24-60 fps, enough to cover how far ahead of audio the video is muxed
	VideoFrameQueue frameQueue(24);

	// User requests. Seek and quit are carried out by the main loop, outside any decoder call.
	bool quitRequested = false;
	int32_t seekRequestSeconds = 0;
	double seekRequestFraction = -1;
	auto actionPending = [&] { return quitRequested || seekRequestSeconds != 0 || seekRequestFraction >= 0; };

	// Position shown on the control bar, on the file's timeline; held at the seek target until playback resumes
	// after a seek
	int64_t positionMs = 0;
	// Timeline time minus clock time. The clock follows the stream's PTS, made continuous where they jump, so
	// this changes where a new program starts in the file.
	int64_t timelineOffsetMs = 0;
	bool timelineOffsetKnown = false;

	// Starts audio once ~100ms is buffered, so the device doesn't immediately run dry
	auto maybeStartAudio = [&](const bool force) {
		if (!sdl.hasAudio() || sdl.audioPlaying() || audioDecoder.firstFramePtsMillis() < 0) {
			return;
		}
		const auto *soundBuffer = sdl.m_soundBuffer;
		const uint32_t prebuffer = std::min(sdl.m_wav_spec.freq * sdl.m_wav_spec.channels / 10, static_cast<int>(soundBuffer->size() / 2));
		if (soundBuffer->used() >= prebuffer || (force && !soundBuffer->isEmpty())) {
			sdl.startAudio(audioDecoder.firstFramePtsMillis());
			// Audio only: there are no frames to give the position
			const int64_t timelineMs = mps.timelineMillis(audioDecoder.firstFramePtsMillis());
			if (!timelineOffsetKnown && timelineMs >= 0) {
				timelineOffsetMs = timelineMs - audioDecoder.firstFramePtsMillis();
				timelineOffsetKnown = true;
			}
		}
	};

	auto handleInput = [&] {
		const auto input = sdl.eventLoop();
		quitRequested |= input.quit;
		seekRequestSeconds += input.seekSeconds;
		if (input.seekFraction >= 0) {
			seekRequestFraction = input.seekFraction;
		}
		if (input.togglePause) {
			const bool paused = !clock.paused();
			clock.setPaused(paused);
			sdl.setAudioPaused(paused);
		}

		if (clock.started() && timelineOffsetKnown) {
			positionMs = clock.ms() + timelineOffsetMs;
		}
		sdl.updateControlBar({clock.paused(), positionMs, mps.timelineDurationMillis()});
	};

	// Shows the frame that is due now, if any
	auto presentDueFrame = [&] {
		if (frameQueue.empty()) {
			return;
		}
		clock.startAt(frameQueue.front().ptsMs);
		if (!frameQueue.frontIsDue(clock.ms())) {
			return;
		}
		const auto &frame = frameQueue.front();
		// The decoder's pel aspect ratio is pixel height / width, 0 if unknown
		const double pelAspect = videoDecoder.aspect_ratio;
		const double displayAspect = pelAspect > 0 ? frame.width / (frame.height * pelAspect) : 0;
		if (frame.width != screenWidth || frame.height != screenHeight || displayAspect != screenAspect) {
			screenWidth = frame.width;
			screenHeight = frame.height;
			screenAspect = displayAspect;
			sdl.reinitScreen(frame.width, frame.height, 1, false, displayAspect);
		}
		std::memcpy(sdl.rgbData(), frame.rgb.get(), static_cast<size_t>(frame.width) * frame.height * sizeof(uint32_t));
		sdl.blitSurface();
		if (frame.timelineMs >= 0) {
			timelineOffsetMs = frame.timelineMs - frame.ptsMs;
			timelineOffsetKnown = true;
		}
		frameQueue.pop();
	};

	// Continues playback from a byte position in the file
	auto seekToPosition = [&](int64_t position) {
		position = std::clamp<int64_t>(position, 0, static_cast<int64_t>(fs.size()));
		fs.seek(position);
		mps.reset();
		mps.setInputPosition(position);
		videoDecoder.reset();
		audioDecoder.reset();
		sdl.resetAudio();
		frameQueue.clear();
		displayTimestamper.reset();
		clock.reset();
		timelineOffsetKnown = false;
	};

	// Jumps by deltaSeconds from the current position
	auto seekBy = [&](const int32_t deltaSeconds) {
		int64_t targetMs = std::max<int64_t>(0, positionMs + static_cast<int64_t>(deltaSeconds) * 1000);
		if (mps.timelineDurationMillis() > 0) {
			targetMs = std::min(targetMs, mps.timelineDurationMillis());
		}
		const int64_t position = mps.findTimelinePosition(fs, targetMs);
		if (position < 0) {
			return; // Nothing known yet to estimate from
		}
		seekToPosition(position);
		positionMs = targetMs;
	};

	// Jumps to a fraction of the way through the file (the seek bar)
	auto seekToFraction = [&](const double fraction) {
		const auto position = static_cast<int64_t>(fraction * static_cast<double>(fs.size()));
		seekToPosition(position);
		if (const int64_t timelineMs = mps.timelineAtPosition(position); timelineMs >= 0) {
			positionMs = timelineMs;
		}
	};

	videoDecoder.setFrameCallback(
		[&](const Frame& frame) {
			if (actionPending()) {
				return; // About to seek or quit, the frame isn't needed
			}
			const int64_t ptsMs = displayTimestamper.next(frame.m_ptsMillis, videoDecoder.m_frame_period_millis);
			if (ptsMs < 0) {
				return; // No timing yet: no PTS since the start or a seek
			}

			// Queue full: keep presenting until there is room
			while (frameQueue.full()) {
				presentDueFrame();
				if (!frameQueue.full()) {
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				handleInput();
				if (actionPending()) {
					return;
				}
			}

			const int64_t timelineMs = frame.m_ptsMillis >= 0 ? mps.timelineMillis(frame.m_ptsMillis) : -1;
			videoDecoder.generateRgbFrameData(frameQueue.push(ptsMs, videoDecoder.m_width, videoDecoder.m_height, timelineMs),
			                                  frame);
		});

	audioDecoder.setFrameCallback(
		[&](const uint32_t frequency, const uint32_t channels, AudioSampleBuffer *soundBuffer) {
			if (!audioInited) {
				audioInited = true;
				sdl.initAudio(frequency, channels, soundBuffer);
			}
			maybeStartAudio(false);
		});

	// Still playing audio at the end of the stream
	auto audioStillPlaying = [&] {
		if (!sdl.hasAudio()) {
			return false;
		}
		maybeStartAudio(true);
		if (!sdl.audioPlaying()) {
			return false;
		}
		return (!streamFailed && audioDecoder.hasPendingFrame()) || sdl.audioClockMs().has_value();
	};

	bool endOfStream = false;
	while (!quitRequested) {
		handleInput();
		if (seekRequestSeconds != 0 || seekRequestFraction >= 0) {
			if (seekRequestFraction >= 0) {
				seekToFraction(seekRequestFraction);
			} else {
				seekBy(seekRequestSeconds);
			}
			seekRequestSeconds = 0;
			seekRequestFraction = -1;
			endOfStream = false;
			streamFailed = false;
			continue;
		}

		// Sleep at the end of this iteration only if there was nothing to demux
		bool idle = true;
		try {
			if (!endOfStream) {
				// Demux in bursts. A call can handle as little as a pack header, so one call per 1 ms sleep caps the
				// input below DVD bit rates. A burst ends when input or space in the sound buffer runs out, or after
				// a few ms, so frames are presented and input handled on time.
				const auto burstEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
				do {
					const auto result = mps.nextPacket(fs);
					if (result == Mpeg1::DemuxResult::EndOfStream) {
						endOfStream = true;
						break;
					}
					if (result == Mpeg1::DemuxResult::WaitingForInput || audioDecoder.hasPendingFrame()) {
						break;
					}
					idle = false;
				} while (!actionPending() && std::chrono::steady_clock::now() < burstEnd);
			} else if (!streamFailed) {
				// Decode audio frames that were waiting for space in the sound buffer
				audioDecoder.executeCallbacks();
			}
		} catch (const std::exception &ex) {
			std::cout << ex.what() << std::endl;
			streamFailed = true;
			endOfStream = true;
		}

		presentDueFrame();
		if (endOfStream && !clock.paused() && frameQueue.empty() && !audioStillPlaying()) {
			break;
		}
		if (idle) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	if (!quitRequested && sdl.audioPlaying()) {
		// Let the audio device play its last buffer
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	std::cout << "Dropped frames: " << frameQueue.droppedFrames() << "\n";
	std::cout << "Playback time: " << (now() - playbackStartMs) << " ms\n";

	return 0;
}
