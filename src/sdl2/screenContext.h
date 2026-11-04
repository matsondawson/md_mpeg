#pragma once

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_surface.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "../stream/RingBufferTyped.h"
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
};

class ScreenContextRgbSDL : public ScreenContext {
private:
	std::unique_ptr<uint32_t[]> m_imageData;

	SDL_Window* m_window;
	SDL_Surface* m_renderSurface;
	int32_t m_zoom;
	ControlBar m_controlBar;

public:
	SDL_AudioStream* m_soundStream{};
	SDL_AudioSpec m_wav_spec{};
	MStream::RingBufferTyped<float, 32768>* m_soundBuffer{};

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

public:
	ScreenContextRgbSDL();
	~ScreenContextRgbSDL();

	void reinitScreen(uint32_t width, uint32_t height, int32_t zoom, bool fullScreen);

	void clear() const;

	// Opens the audio device paused; call startAudio() once enough audio is buffered
	void initAudio(uint32_t frequencyHz, uint32_t channels, MStream::RingBufferTyped<float, 32768>* soundBuffer);
	[[nodiscard]] bool hasAudio() const { return m_soundStream != nullptr; }
	[[nodiscard]] bool audioPlaying() const { return m_audioPlaying; }
	// Starts playback. firstPtsMs is the PTS of the first sample in the sound buffer.
	void startAudio(int32_t firstPtsMs);
	// Pauses or resumes the audio device; startAudio() won't resume it while paused
	void setAudioPaused(bool paused);
	// Stops playback and discards all buffered audio (e.g. after seeking). startAudio() restarts it.
	void resetAudio();
	// PTS (ms) of the sample currently audible, or nullopt if audio isn't playing or has run dry
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