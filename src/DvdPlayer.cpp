#include "DvdPlayer.h"

#include "mpeg1/MpegProgramStream.h"

#if MD_MPEG_ENABLE_MPEG2

#include <algorithm>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "DisplayTimestamper.h"
#include "PlaybackClock.h"
#include "Time.h"
#include "VideoFrameQueue.h"
#include "dvd/NavPack.h"
#include "dvd/Navigator.h"
#include "dvd/Overlay.h"
#include "dvd/SubPicture.h"
#include "dvd/VobReader.h"
#include "mpeg1/AudioDecoders.h"
#include "mpeg1/Mpeg1VideoDecoder.h"
#include "mpeg1/MpegProgramStream.h"
#include "sdl2/screenContext.h"

namespace {
	// What happens after the current cell's sectors have all been read
	enum class Phase {
		// Reading the cell
		Feeding,
		// Playing out what has been decoded, before a still or a jump
		Draining,
		// Holding the last picture for the cell's or program chain's still time
		Still,
	};
}

int playDvd(ScreenContextRgbSDL &sdl, const std::string &path) {
	Dvd::Disc disc(path);
	Dvd::Navigator nav(disc);

	Mpeg1::MpegProgramStream mps;
	auto video = std::make_unique<Mpeg1VideoDecoder>();
	mps.setVideoDecoder(video.get());
	AudioDecoders audio;
	audio.attach(mps);
	Dvd::NavPackDecoder navPacks;
	mps.setNavigationDecoder(&navPacks);
	Dvd::SubPictureDecoder subpictureDecoder;
	Dvd::VobReader reader;

	PlaybackClock clock;
	clock.setAudioSource([&] { return sdl.audioClockMs(); });
	DisplayTimestamper timestamper;
	VideoFrameQueue frameQueue(24);
	bool audioInited = false;

	uint32_t screenWidth = 0, screenHeight = 0;
	double screenAspect = 0;
	// The picture last shown, without overlays, to redraw them over
	std::vector<uint32_t> shownFrame;
	uint32_t shownWidth = 0, shownHeight = 0;

	// Menu buttons of the latest navigation pack, and sub-pictures waiting or showing
	Dvd::Highlight highlight;
	bool haveButtons = false;
	std::vector<Dvd::SubPicture> subpictures;
	bool subpicturesDisplayAll = false;
	// The highlighted button is shown activated until then
	long long activatedUntilMs = 0;
	bool overlayDirty = false;

	Phase phase = Phase::Feeding;
	Dvd::CellLocation cell;
	uint64_t cellJump = 0;
	bool haveCell = false;
	// The opened VOB set
	Dvd::Domain readerDomain = Dvd::Domain::Stopped;
	int readerTitleSet = -1;
	// Still: when it started, and how long (seconds, 255 = until the user acts)
	long long stillStartMs = 0;
	int stillSeconds = 0;
	bool pgcStill = false;
	// A seek starts the next cell part way through
	int64_t seekSector = -1;

	bool quit = false;

	auto maybeStartAudio = [&](const bool force) {
		if (!sdl.hasAudio() || sdl.audioPlaying() || audio.firstFramePtsMillis() < 0) {
			return;
		}
		const auto *soundBuffer = sdl.m_soundBuffer;
		const uint32_t prebuffer = std::min(sdl.m_wav_spec.freq * sdl.m_wav_spec.channels / 10, static_cast<int>(soundBuffer->size() / 2));
		if (soundBuffer->used() >= prebuffer || (force && !soundBuffer->isEmpty())) {
			sdl.startAudio(audio.firstFramePtsMillis());
		}
	};

	// Redraws the last picture with the sub-picture and button highlight over it
	auto drawPicture = [&] {
		if (shownFrame.empty()) {
			return;
		}
		std::memcpy(sdl.rgbData(), shownFrame.data(), shownFrame.size() * sizeof(uint32_t));
		const auto *palette = nav.palette();
		const int64_t now = clock.started() ? clock.ms() : 0;
		const Dvd::SubPicture *showing = nullptr;
		for (const auto &picture: subpictures) {
			if (picture.ptsMs < 0 || picture.ptsMs + picture.startMs <= now) {
				showing = &picture;
			}
		}
		if (showing && palette && !showing->empty()) {
			const bool expired = showing->stopMs >= 0 && showing->ptsMs >= 0 && now >= showing->ptsMs + showing->stopMs;
			if (haveButtons) {
				drawSubPicture(sdl.rgbData(), static_cast<int>(shownWidth), static_cast<int>(shownHeight), *showing, *palette,
				               &highlight, nav.highlightedButton(), ::now() < activatedUntilMs);
			} else if (!expired && (subpicturesDisplayAll || showing->forced)) {
				drawSubPicture(sdl.rgbData(), static_cast<int>(shownWidth), static_cast<int>(shownHeight), *showing, *palette,
				               nullptr, 0, false);
			}
		}
		sdl.blitSurface();
		overlayDirty = false;
	};

	auto presentDueFrame = [&] {
		if (frameQueue.empty()) {
			return;
		}
		clock.startAt(frameQueue.front().ptsMs);
		if (!frameQueue.frontIsDue(clock.ms())) {
			return;
		}
		const auto &frame = frameQueue.front();
		const double pelAspect = frame.pelAspect;
		const double displayAspect = pelAspect > 0 ? frame.width / (frame.height * pelAspect) : 0;
		if (frame.width != screenWidth || frame.height != screenHeight || displayAspect != screenAspect) {
			screenWidth = frame.width;
			screenHeight = frame.height;
			screenAspect = displayAspect;
			sdl.reinitScreen(frame.width, frame.height, 1, false, displayAspect);
		}
		shownWidth = frame.width;
		shownHeight = frame.height;
		shownFrame.assign(frame.rgb.get(), frame.rgb.get() + static_cast<size_t>(frame.width) * frame.height);
		frameQueue.pop();
		drawPicture();
	};

	video->setFrameCallback([&](const Frame &frame) {
		const int64_t ptsMs = timestamper.next(frame.m_ptsMillis, video->m_frame_period_millis);
		if (ptsMs < 0) {
			return;
		}
		while (frameQueue.full()) {
			presentDueFrame();
			if (!frameQueue.full()) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			if (sdl.eventLoop().quit) {
				quit = true;
				return;
			}
		}
		video->generateRgbFrameData(frameQueue.push(ptsMs, frame.m_width, frame.m_height, frame.m_aspectRatio), frame);
	});

	audio.setFrameCallback([&](const uint32_t frequency, const uint32_t channels, AudioSampleBuffer *soundBuffer) {
		if (!audioInited) {
			audioInited = true;
			sdl.initAudio(frequency, channels, soundBuffer);
		}
		maybeStartAudio(false);
	});

	// Forgets everything decoded, for a jump
	auto resetPipeline = [&] {
		mps.reset();
		video->reset();
		audio.reset();
		sdl.resetAudio();
		frameQueue.clear();
		timestamper.reset();
		clock.reset();
		subpictureDecoder.reset();
		navPacks.discontinuity();
		navPacks.take();
		highlight = {};
		haveButtons = false;
		subpictures.clear();
	};

	// Starts reading the navigator's current cell
	auto beginCell = [&] {
		pgcStill = false;
		cellJump = nav.jumpCount();
		const auto location = nav.cell();
		if (!location) {
			haveCell = false;
			if (nav.inPgcStill()) {
				phase = Phase::Still;
				pgcStill = true;
				stillStartMs = now();
				stillSeconds = nav.pgc()->stillTime;
			}
			return;
		}
		cell = *location;
		haveCell = true;
		if (cell.domain != readerDomain || cell.titleSet != readerTitleSet) {
			if (!reader.open(disc, cell.domain, cell.titleSet)) {
				std::cout << "DVD: missing VOB files for title set " << cell.titleSet << std::endl;
			}
			readerDomain = cell.domain;
			readerTitleSet = cell.titleSet;
		}
		const auto first = static_cast<uint32_t>(std::clamp<int64_t>(seekSector, cell.firstSector, cell.lastSector));
		reader.setRange(seekSector >= 0 ? first : cell.firstSector, cell.lastSector);
		seekSector = -1;
		const int audioStream = nav.audioStreamId();
		mps.setAudioStream(audioStream >= 0 ? static_cast<uint32_t>(audioStream) : 0);
		const int subpictureStream = nav.subpictureStreamId(nav.wideScreen(), subpicturesDisplayAll);
		mps.setSubpictureDecoder(&subpictureDecoder, subpictureStream);
		phase = Phase::Feeding;
	};

	// The cell's sectors have all been read: continue into the next cell if playback runs on, else play out
	auto cellRead = [&] {
		if (cell.stillTime) {
			phase = Phase::Draining;
			return;
		}
		nav.cellFinished();
		const auto next = nav.cell();
		if (nav.jumpCount() == cellJump && next && next->domain == cell.domain && next->titleSet == cell.titleSet
		    && !nav.inPgcStill()) {
			// The next cell, read straight on
			cell = *next;
			reader.setRange(cell.firstSector, cell.lastSector);
			return;
		}
		// A jump, a still or the end: play out first, then start afresh
		cellJump = nav.jumpCount();
		phase = Phase::Draining;
	};

	// Everything read has been shown and heard
	auto drained = [&] {
		if (!frameQueue.empty() || audio.hasPendingFrame()) {
			return false;
		}
		maybeStartAudio(true);
		return !sdl.audioPlaying() || !sdl.audioClockMs().has_value();
	};

	// --- menu buttons

	auto buttonAt = [&](const int x, const int y) {
		for (size_t i = 0; i < highlight.buttons.size(); i++) {
			const auto &b = highlight.buttons[i];
			if (x >= b.x1 && x <= b.x2 && y >= b.y1 && y <= b.y2) {
				return static_cast<int>(i) + 1;
			}
		}
		return 0;
	};
	auto activate = [&](const int button) {
		if (button < 1 || button > static_cast<int>(highlight.buttons.size())) {
			return;
		}
		nav.setHighlightedButton(button);
		activatedUntilMs = now() + 250;
		drawPicture();
		nav.buttonActivated(highlight.buttons[button - 1].command);
	};
	auto select = [&](const int button) {
		if (button < 1 || button > static_cast<int>(highlight.buttons.size()) || button == nav.highlightedButton()) {
			return;
		}
		nav.setHighlightedButton(button);
		overlayDirty = true;
		if (highlight.buttons[button - 1].autoAction) {
			activate(button);
		}
	};

	// --- position in a title, for the control bar and seeking

	auto cellStartMs = [&](const int cellNumber) {
		int64_t ms = 0;
		for (int i = 1; i < cellNumber && i <= static_cast<int>(nav.pgc()->cells.size()); i++) {
			ms += nav.pgc()->cells[i - 1].playbackTimeMs;
		}
		return ms;
	};
	auto titlePositionMs = [&]() -> int64_t {
		if (nav.domain() != Dvd::Domain::Title || !nav.pgc() || !haveCell) {
			return 0;
		}
		const auto &c = nav.pgc()->cells[nav.cellNumber() - 1];
		const double fraction = c.lastSector > c.firstSector
			                        ? std::clamp(static_cast<double>(reader.sector() - c.firstSector) / (c.lastSector - c.firstSector), 0.0, 1.0)
			                        : 0.0;
		return cellStartMs(nav.cellNumber()) + static_cast<int64_t>(fraction * c.playbackTimeMs);
	};
	auto seekTitle = [&](int64_t targetMs) {
		if (nav.domain() != Dvd::Domain::Title || !nav.pgc() || nav.pgc()->cells.empty()) {
			return;
		}
		const auto &cells = nav.pgc()->cells;
		targetMs = std::clamp<int64_t>(targetMs, 0, nav.pgc()->playbackTimeMs);
		int64_t start = 0;
		for (size_t i = 0; i < cells.size(); i++) {
			const auto &c = cells[i];
			if (targetMs < start + c.playbackTimeMs || i + 1 == cells.size()) {
				const double fraction = c.playbackTimeMs ? std::clamp(static_cast<double>(targetMs - start) / c.playbackTimeMs, 0.0, 1.0) : 0.0;
				nav.jumpToCell(static_cast<int>(i) + 1);
				seekSector = c.firstSector + static_cast<int64_t>(fraction * (c.lastSector - c.firstSector));
				return;
			}
			start += c.playbackTimeMs;
		}
	};

	nav.start();
	beginCell();

	while (!quit && !nav.stopped()) {
		// --- input
		const auto input = sdl.eventLoop();
		quit |= input.quit;
		if (input.togglePause) {
			const bool paused = !clock.paused();
			clock.setPaused(paused);
			sdl.setAudioPaused(paused);
		}
		if (haveButtons && !highlight.buttons.empty()) {
			int button = std::clamp(nav.highlightedButton(), 1, static_cast<int>(highlight.buttons.size()));
			const auto move = [&](const int count, uint8_t Dvd::Button::*direction) {
				for (int i = 0; i < count; i++) {
					const int target = highlight.buttons[button - 1].*direction;
					if (target >= 1 && target <= static_cast<int>(highlight.buttons.size())) {
						select(target);
						button = nav.highlightedButton();
					}
				}
			};
			move(input.up, &Dvd::Button::up);
			move(input.down, &Dvd::Button::down);
			move(input.left, &Dvd::Button::left);
			move(input.right, &Dvd::Button::right);
			if (input.mouseMoved || input.mouseClicked) {
				if (const int under = buttonAt(input.mouseX, input.mouseY)) {
					select(under);
					if (input.mouseClicked) {
						activate(under);
					}
				}
			}
			if (input.activate) {
				activate(button);
			}
		} else if (nav.domain() == Dvd::Domain::Title) {
			if (input.seekSeconds) {
				seekTitle(titlePositionMs() + input.seekSeconds * 1000);
			} else if (input.seekFraction >= 0 && nav.pgc()) {
				seekTitle(static_cast<int64_t>(input.seekFraction * nav.pgc()->playbackTimeMs));
			}
			for (int i = 0; i < input.chapterSkip; i++) nav.nextChapter();
			for (int i = 0; i > input.chapterSkip; i--) nav.previousChapter();
		}
		if (input.rootMenu) {
			nav.menuCall(Dvd::ROOT_MENU);
		}
		if (input.titleMenu) {
			nav.menuCall(Dvd::TITLE_MENU);
		}
		if (nav.stopped()) {
			break;
		}

		// --- a jump by the user (button, menu, chapter, seek) abandons what is playing
		if (nav.jumpCount() != cellJump && phase != Phase::Draining) {
			resetPipeline();
			beginCell();
		}

		// --- reading
		bool idle = true;
		try {
			if (phase == Phase::Feeding && haveCell) {
				const auto burstEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
				do {
					if (reader.isEmpty()) {
						cellRead();
						break;
					}
					const auto result = mps.nextPacket(reader);
					if (result == Mpeg1::DemuxResult::WaitingForInput || audio.hasPendingFrame()) {
						break;
					}
					idle = false;
				} while (!quit && std::chrono::steady_clock::now() < burstEnd);
			} else if (phase == Phase::Draining) {
				// The demuxer's last buffered bytes, then the held picture
				if (mps.nextPacket(reader) != Mpeg1::DemuxResult::EndOfStream) {
					idle = false;
				} else if (drained()) {
					if (cell.stillTime && nav.jumpCount() == cellJump) {
						phase = Phase::Still;
						stillStartMs = now();
						stillSeconds = cell.stillTime;
					} else {
						resetPipeline();
						beginCell();
					}
				} else {
					audio.executeCallbacks();
				}
			} else if (phase == Phase::Still) {
				if (stillSeconds != 255 && now() - stillStartMs >= stillSeconds * 1000LL) {
					if (pgcStill) {
						nav.pgcStillFinished();
					} else {
						nav.cellFinished();
					}
					resetPipeline();
					beginCell();
				}
			}
		} catch (const std::exception &ex) {
			std::cout << "DVD: " << ex.what() << std::endl;
			resetPipeline();
			nav.cellFinished();
			beginCell();
		}

		// --- navigation packs and sub-pictures read
		for (auto &pci: navPacks.take()) {
			const auto &h = pci.highlight;
			if (h.status == 0) {
				if (haveButtons) overlayDirty = true;
				haveButtons = false;
				highlight = {};
			} else if (h.status == 1 || !haveButtons) {
				highlight = h;
				haveButtons = !h.buttons.empty();
				overlayDirty = true;
				if (h.forcedSelect >= 1 && h.forcedSelect <= h.buttons.size()) {
					nav.setHighlightedButton(h.forcedSelect);
				} else if (nav.highlightedButton() > static_cast<int>(h.buttons.size())) {
					nav.setHighlightedButton(1);
				}
				if (h.forcedActivate >= 1 && h.forcedActivate <= h.buttons.size()) {
					activate(h.forcedActivate);
				}
			}
		}
		for (auto &picture: subpictureDecoder.take()) {
			subpictures.push_back(std::move(picture));
			overlayDirty = true;
		}
		if (subpictures.size() > 8) {
			subpictures.erase(subpictures.begin(), subpictures.end() - 8);
		}

		presentDueFrame();
		if (overlayDirty || (activatedUntilMs && now() >= activatedUntilMs)) {
			if (activatedUntilMs && now() >= activatedUntilMs) {
				activatedUntilMs = 0;
			}
			drawPicture();
		}

		const int64_t durationMs = nav.domain() == Dvd::Domain::Title && nav.pgc() ? nav.pgc()->playbackTimeMs : -1;
		sdl.updateControlBar({clock.paused(), titlePositionMs(), durationMs});

		if (idle) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	return 0;
}

#endif
