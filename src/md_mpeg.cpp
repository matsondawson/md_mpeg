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
#include "mpeg1/Mpeg1AudioDecoder.h"
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
	bool audioInited = false;

	const auto playbackStartMs = now();
	bool streamFailed = false;

	MStream::FileStreamRead fs(argv[1]);
	Mpeg1::MpegProgramStream mps;
	Mpeg1VideoDecoder videoDecoder;
	mps.setVideoDecoder(&videoDecoder);
	Mpeg1AudioDecoder audioDecoder;
	mps.setAudioDecoder(&audioDecoder);
	const auto [endPackPosition, endScrMillis] = Mpeg1::findLastPack(fs);
	mps.setEndPack(endPackPosition, endScrMillis);

	// Audio is the master clock when there is audio, otherwise wall time
	PlaybackClock clock;
	clock.setAudioSource([&] { return sdl.audioClockMs(); });

	DisplayTimestamper displayTimestamper;
	// ~0.5-1s of video, enough to cover how far ahead of audio the video is muxed
	VideoFrameQueue frameQueue(24);

	// User requests. Seek and quit are carried out by the main loop, outside any decoder call.
	bool quitRequested = false;
	int32_t seekRequestSeconds = 0;
	double seekRequestFraction = -1;
	auto actionPending = [&] { return quitRequested || seekRequestSeconds != 0 || seekRequestFraction >= 0; };

	// Position shown on the control bar; held at the seek target until playback resumes after a seek
	int64_t positionMs = 0;

	// Starts audio once ~100ms is buffered, so the device doesn't immediately run dry
	auto maybeStartAudio = [&](const bool force) {
		if (!sdl.hasAudio() || sdl.audioPlaying() || audioDecoder.firstFramePtsMillis() < 0) {
			return;
		}
		const auto *soundBuffer = sdl.m_soundBuffer;
		const uint32_t prebuffer = std::min(sdl.m_wav_spec.freq * sdl.m_wav_spec.channels / 10, static_cast<int>(soundBuffer->size() / 2));
		if (soundBuffer->used() >= prebuffer || (force && !soundBuffer->isEmpty())) {
			sdl.startAudio(audioDecoder.firstFramePtsMillis());
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

		if (clock.started() && mps.startMillis() >= 0) {
			positionMs = clock.ms() - mps.startMillis();
		}
		sdl.updateControlBar({clock.paused(), positionMs, mps.durationMillis()});
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
		if (frame.width != screenWidth || frame.height != screenHeight) {
			screenWidth = frame.width;
			screenHeight = frame.height;
			sdl.reinitScreen(frame.width, frame.height, 1, false);
		}
		std::memcpy(sdl.rgbData(), frame.rgb.get(), static_cast<size_t>(frame.width) * frame.height * sizeof(uint32_t));
		sdl.blitSurface();
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
	};

	// Jumps by deltaSeconds from the current position
	auto seekBy = [&](const int32_t deltaSeconds) {
		const int64_t currentMs = clock.started() ? clock.ms() : 0;
		const int64_t targetMs = std::max<int64_t>(0, currentMs + static_cast<int64_t>(deltaSeconds) * 1000);
		const int64_t position = mps.estimateBytePosition(targetMs);
		if (position < 0) {
			return; // Nothing demuxed yet to estimate from
		}
		seekToPosition(position);
		if (mps.startMillis() >= 0) {
			positionMs = targetMs - mps.startMillis();
		}
	};

	// Jumps to a fraction of the way through the file (the seek bar)
	auto seekToFraction = [&](const double fraction) {
		seekToPosition(static_cast<int64_t>(fraction * static_cast<double>(fs.size())));
		if (mps.durationMillis() > 0) {
			positionMs = static_cast<int64_t>(fraction * static_cast<double>(mps.durationMillis()));
		}
	};

	videoDecoder.setFrameCallback(
		[&](const Frame& frame) {
			if (actionPending()) {
				return; // About to seek or quit, the frame isn't needed
			}
			const int64_t ptsMs = displayTimestamper.next(frame.m_ptsMillis, videoDecoder.m_frame_period_millis);
			if (ptsMs < 0) {
				return; // No timing yet (just after a seek)
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

			videoDecoder.generateRgbFrameData(frameQueue.push(ptsMs, videoDecoder.m_width, videoDecoder.m_height), frame);
		});

	audioDecoder.setFrameCallback(
		[&](const uint32_t frequency, const uint32_t channels, MStream::RingBufferTyped<float, 32768> *soundBuffer) {
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

		try {
			if (!endOfStream) {
				endOfStream = mps.nextPacket(fs) < 0;
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
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (!quitRequested && sdl.audioPlaying()) {
		// Let the audio device play its last buffer
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	std::cout << "Dropped frames: " << frameQueue.droppedFrames() << "\n";
	std::cout << "Playback time: " << (now() - playbackStartMs) << " ms\n";

	return 0;
}
