#pragma once

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_surface.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "../AudioStallDetector.h"
#include "../mpeg1/AudioSample.h"
#include "ControlBar.h"

struct free_delete {
	void operator()(void* x) { free(x); }
};


class ScreenContext {
public:
	static bool global_exit;

	int32_t m_width;
	int32_t m_height;
	int32_t m_windowWidth;
	int32_t m_windowHeight;

public:
	ScreenContext(int32_t width, int32_t height, int32_t windowWidth, int32_t windowHeight);
};

// Input gathered by eventLoop()
struct InputEvents {
	bool quit = false;
	bool togglePause = false;
	int32_t seekSeconds = 0;
	// 0..1 of the way through the file, -1 if no seek bar click
	double seekFraction = -1;

	// Arrow keys pressed (they also seek, see seekSeconds), for DVD menus
	int up = 0, down = 0, left = 0, right = 0;
	// Enter: activate the selected menu button
	bool activate = false;
	// M: DVD root menu, T: DVD title menu
	bool rootMenu = false;
	bool titleMenu = false;
	// Page Down / Page Up: next or previous chapter
	int chapterSkip = 0;
	// N: skip what is playing on a DVD (e.g. a warning), to where the disc goes after it
	bool skip = false;
	// Mouse over the picture: its position in picture pixels, and whether it was clicked (not on the control bar)
	bool mouseMoved = false;
	bool mouseClicked = false;
	int mouseX = -1, mouseY = -1;
};

class ScreenContextRgbSDL : public ScreenContext {
private:
	std::unique_ptr<uint32_t[]> m_imageData;

	SDL_Window* m_window;
	SDL_Surface* m_renderSurface;
	int32_t m_zoom;
	// Display width / height of the picture, 0 to show its pixels square
	double m_displayAspect = 0;
	ControlBar m_controlBar;
	// Where the picture was last drawn in the window surface
	SDL_Rect m_pictureRect{};

	// Converts window coordinates to picture pixels, false if outside the picture
	bool pictureCoordinates(float windowX, float windowY, int &x, int &y) const;

public:
	SDL_AudioStream* m_soundStream{};
	SDL_AudioSpec m_wav_spec{};
	AudioSampleBuffer* m_soundBuffer{};

	// Audio clock state. Written by the SDL audio thread, read by the main thread.
	std::mutex m_audioPositionMutex;
	uint64_t m_framesBeforeLastCallback = 0; // sample frames handed to SDL before the last callback
	uint64_t m_framesAfterLastCallback = 0;  // ... and after it
	long long m_lastCallbackTimeMs = 0;

private:
	bool m_audioPlaying = false;
	bool m_audioPaused = false;
	int32_t m_audioStartPtsMs = 0;
	int32_t m_deviceLatencyMs = 0;
	// The output stopped asking for audio; it is then discarded and the clock falls back to wall time
	AudioStallDetector m_stallDetector;

public:
	ScreenContextRgbSDL();
	~ScreenContextRgbSDL();

	// displayAspect: display width / height of the picture, 0 if its pixels are square
	void reinitScreen(uint32_t width, uint32_t height, int32_t zoom, bool fullScreen, double displayAspect = 0);

	void clear() const;

	// Opens the audio device paused; call startAudio() once enough audio is buffered
	void initAudio(uint32_t frequencyHz, uint32_t channels, AudioSampleBuffer* soundBuffer);
	[[nodiscard]] bool hasAudio() const { return m_soundStream != nullptr; }
	[[nodiscard]] bool audioPlaying() const { return m_audioPlaying; }
	// Starts playback. firstPtsMs is the PTS of the first sample in the sound buffer.
	void startAudio(int32_t firstPtsMs);
	// Pauses or resumes the audio device; startAudio() won't resume it while paused
	void setAudioPaused(bool paused);
	// Stops playback and discards all buffered audio (e.g. after seeking). startAudio() restarts it.
	void resetAudio();
	// Closes the audio output, so its thread no longer reads the sound buffer. initAudio() opens it again.
	void closeAudio();
	// PTS (ms) of the sample currently audible, or nullopt if audio isn't playing, has run dry, or the output
	// has stalled. Once stalled, buffered audio is discarded so decoding carries on, until resetAudio().
	[[nodiscard]] std::optional<int64_t> audioClockMs();

	[[nodiscard]] uint32_t* rgbData() const;

	void setTitle(const std::string &title) const;

	// Draws the current frame and the control bar
	void blitSurface();
	// Updates what the control bar shows, redrawing if it changed
	void updateControlBar(const ControlBarState &state);
	// Handles window events, returns the user's input
	InputEvents eventLoop();
};

/**
 * Closes the audio output when it goes out of scope. Declare it after the decoder that owns the sound buffer, so
 * the audio thread has stopped reading the buffer before the buffer is freed.
 */
class AudioOutputCloser {
public:
	explicit AudioOutputCloser(ScreenContextRgbSDL &sdl) : m_sdl(sdl) {
	}
	~AudioOutputCloser() {
		m_sdl.closeAudio();
	}
	AudioOutputCloser(const AudioOutputCloser &) = delete;
	AudioOutputCloser &operator=(const AudioOutputCloser &) = delete;

private:
	ScreenContextRgbSDL &m_sdl;
};