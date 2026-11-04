#include "screenContext.h"

#include <algorithm>
#include <iostream>
#include <SDL3/SDL.h>

#include "../Time.h"

bool ScreenContext::global_exit = false;

using std::cout;
using std::endl;

void sdlError(const bool result) {
	if (!result) {
		cout << "Error: " << SDL_GetError() << endl;
		exit(1);
	}
}

ScreenContext::ScreenContext(const int32_t width, const int32_t height, const int32_t windowWidth,
                             const int32_t windowHeight)
	: m_width(width), m_height(height), m_windowWidth(windowWidth), m_windowHeight(windowHeight) {
	// empty
}

ScreenContextRgbSDL::ScreenContextRgbSDL()
	: ScreenContext(160, 128, 160, 128)
	  , m_imageData(nullptr), m_window(nullptr), m_renderSurface(nullptr), m_zoom(1) {
	sdlError(SDL_Init(SDL_INIT_EVENTS | SDL_INIT_VIDEO | SDL_INIT_AUDIO));
	// Shown straight away, so there is something to control playback with even for audio-only files
	sdlError(m_window = SDL_CreateWindow("md_mpeg", 640, 360, SDL_WINDOW_RESIZABLE));
	clear();
	SDL_UpdateWindowSurface(m_window);
}

void ScreenContextRgbSDL::setTitle(const std::string &title) const {
	SDL_SetWindowTitle(m_window, title.c_str());
}

ScreenContextRgbSDL::~ScreenContextRgbSDL() {
	if (m_renderSurface) {
		SDL_DestroySurface(m_renderSurface);
	}
	if (m_window) {
		SDL_DestroyWindow(m_window);
	}

	SDL_Quit();
}

void ScreenContextRgbSDL::blitSurface() {
	if (!m_window) {
		return;
	}

	const auto winSurface = SDL_GetWindowSurface(m_window);
	sdlError(winSurface);

	// Clear first: the letterbox area may hold an old control bar
	const SDL_PixelFormatDetails *format = SDL_GetPixelFormatDetails(winSurface->format);
	SDL_FillSurfaceRect(winSurface, nullptr, SDL_MapRGBA(format, nullptr, 0, 0, 0, 255));
	if (!m_renderSurface) {
		// Nothing decoded yet, or audio only
		m_controlBar.setScale(SDL_GetWindowDisplayScale(m_window));
		m_controlBar.draw(winSurface);
		SDL_UpdateWindowSurface(m_window);
		return;
	}

	auto height = (m_renderSurface->h * winSurface->w) / m_renderSurface->w;
	auto width = winSurface->w;
	if (height > winSurface->h) {
		height = winSurface->h;
		width = (m_renderSurface->w * winSurface->h) / m_renderSurface->h;
	}
	const SDL_Rect rect = {(winSurface->w - width) / 2, (winSurface->h - height) / 2, width, height};
	sdlError(SDL_StretchSurface(m_renderSurface, nullptr, winSurface, &rect, SDL_SCALEMODE_LINEAR));

	m_controlBar.setScale(SDL_GetWindowDisplayScale(m_window));
	m_controlBar.draw(winSurface);

	SDL_UpdateWindowSurface(m_window);
}

void ScreenContextRgbSDL::updateControlBar(const ControlBarState &state) {
	m_controlBar.setState(state);
	const auto winSurface = SDL_GetWindowSurface(m_window);
	if (winSurface && m_controlBar.needsRedraw(winSurface->w, winSurface->h)) {
		blitSurface();
	}
}

uint32_t *ScreenContextRgbSDL::rgbData() const {
	return m_imageData.get();
}

/**
 * @return bytesFilled sizeof(float) * channels *
 */
int32_t sdl2_audio_callback(void *userdata, uint8_t *stream, int byteLength) {
	int32_t bytesFilled = 0;
	const auto sdlContext = static_cast<ScreenContextRgbSDL *>(userdata);
	const auto soundBuffer = sdlContext->m_soundBuffer;
	auto floatStreamLeft = reinterpret_cast<float *>(stream);
	if (sdlContext->m_wav_spec.channels == 2) {
		// Sound buffer must have 64 floats, which is 32 floats for left channel and 32 for right
		// Buffer must be able to accept 64 floats
		while (byteLength >= 32 * 2 * 4 && soundBuffer->used() >= 64) {
			// We read from channel blocks Left x 32,Right x 32,etc...
			// And write to interleaved channels LRLRLRLR
			// Left channel
			auto floatStreamRight = floatStreamLeft + 1;
			for (int i = 0; i < 32; i++) {
				*floatStreamLeft = soundBuffer->nextByte();
				floatStreamLeft += 2;
			}
			// Right channel
			for (int i = 0; i < 32; i++) {
				*floatStreamRight = soundBuffer->nextByte();
				floatStreamRight += 2;
			}
			byteLength -= 8 * 32;
			bytesFilled += 8 * 32;
		}
	} else if (sdlContext->m_wav_spec.channels == 1) {
		while (byteLength > 0 && soundBuffer->used()) {
			*(floatStreamLeft++) = soundBuffer->nextByte();
			byteLength -= 4;
			bytesFilled += 4;
		}
	}
	// else do nothing
	return bytesFilled;
}

void SDLCALL my_audio_callback(void *userdata, SDL_AudioStream *stream, const int additional_amount,
                               const int total_amount) {
	if (additional_amount <= 0) {
		return;
	}

	auto *data = SDL_stack_alloc(Uint8, additional_amount);
	sdlError(data);

	const auto bytesFilled = sdl2_audio_callback(userdata, data, additional_amount);
	SDL_PutAudioStreamData(stream, data, bytesFilled);
	SDL_stack_free(data);

	// Record playback position for the audio clock
	const auto sdlContext = static_cast<ScreenContextRgbSDL *>(userdata);
	const auto frames = static_cast<uint64_t>(bytesFilled) / (sizeof(float) * sdlContext->m_wav_spec.channels);
	std::lock_guard lock(sdlContext->m_audioPositionMutex);
	sdlContext->m_framesBeforeLastCallback = sdlContext->m_framesAfterLastCallback;
	sdlContext->m_framesAfterLastCallback += frames;
	sdlContext->m_lastCallbackTimeMs = now();
}

void ScreenContextRgbSDL::initAudio(const uint32_t frequencyHz, const uint32_t channels,
                                    MStream::RingBufferTyped<float, 32768> *soundBuffer) {
	if (channels > 2) {
		cthrow(StringException, "Invalid number of channels, got " << channels << " channels");
	}
	if (channels == 0) {
		// No audio
		return;
	}
	m_soundBuffer = soundBuffer;
	m_wav_spec.freq = static_cast<int>(frequencyHz);
	m_wav_spec.format = SDL_AUDIO_F32LE;
	m_wav_spec.channels = static_cast<int>(channels);

	// Device starts paused
	m_soundStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &m_wav_spec, my_audio_callback, this);
	if (!m_soundStream) {
		// Play without audio, the playback clock falls back to wall time
		cout << "Audio disabled: " << SDL_GetError() << endl;
		return;
	}

	// Samples handed to SDL are heard roughly one device buffer later
	SDL_AudioSpec deviceSpec;
	int deviceSampleFrames = 0;
	if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(m_soundStream), &deviceSpec, &deviceSampleFrames)
	    && deviceSpec.freq > 0) {
		m_deviceLatencyMs = deviceSampleFrames * 1000 / deviceSpec.freq;
	}
}

void ScreenContextRgbSDL::startAudio(const int32_t firstPtsMs) {
	if (!m_soundStream || m_audioPlaying) {
		return;
	}
	m_audioStartPtsMs = firstPtsMs;
	m_audioPlaying = true;
	if (!m_audioPaused) {
		SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(m_soundStream));
	}
}

void ScreenContextRgbSDL::setAudioPaused(const bool paused) {
	m_audioPaused = paused;
	if (!m_soundStream || !m_audioPlaying) {
		return;
	}
	if (paused) {
		SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(m_soundStream));
	} else {
		SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(m_soundStream));
	}
}

void ScreenContextRgbSDL::resetAudio() {
	if (!m_soundStream) {
		return;
	}
	SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(m_soundStream));
	// Holding the stream lock guarantees the callback isn't running, so the sound buffer can be
	// drained from this thread
	SDL_LockAudioStream(m_soundStream);
	SDL_ClearAudioStream(m_soundStream);
	while (!m_soundBuffer->isEmpty()) {
		m_soundBuffer->nextByte();
	}
	{
		std::lock_guard lock(m_audioPositionMutex);
		m_framesBeforeLastCallback = m_framesAfterLastCallback = 0;
		m_lastCallbackTimeMs = 0;
	}
	m_audioPlaying = false;
	SDL_UnlockAudioStream(m_soundStream);
}

std::optional<int64_t> ScreenContextRgbSDL::audioClockMs() {
	if (!m_audioPlaying) {
		return std::nullopt;
	}

	uint64_t framesBefore, framesAfter;
	long long callbackTimeMs;
	{
		std::lock_guard lock(m_audioPositionMutex);
		framesBefore = m_framesBeforeLastCallback;
		framesAfter = m_framesAfterLastCallback;
		callbackTimeMs = m_lastCallbackTimeMs;
	}

	// Ran dry: nothing decoded and nothing left queued in SDL
	const uint32_t minUsable = m_wav_spec.channels == 2 ? 64 : 1;
	if (m_soundBuffer->used() < minUsable && SDL_GetAudioStreamQueued(m_soundStream) <= 0) {
		return std::nullopt;
	}

	// Callbacks hand over audio in chunks; interpolate across the last chunk so the clock moves smoothly
	const double freq = m_wav_spec.freq;
	const double sinceCallbackFrames = callbackTimeMs ? static_cast<double>(now() - callbackTimeMs) * freq / 1000.0 : 0.0;
	const double frames = static_cast<double>(framesBefore)
	                      + std::min(sinceCallbackFrames, static_cast<double>(framesAfter - framesBefore));

	return m_audioStartPtsMs + static_cast<int64_t>(frames * 1000.0 / freq) - m_deviceLatencyMs;
}

void ScreenContextRgbSDL::reinitScreen(const uint32_t width, const uint32_t height, const int32_t zoom,
                                       const bool fullScreen) {
	m_width = static_cast<int32_t>(width);
	m_height = static_cast<int32_t>(height);
	m_zoom = zoom;
	m_windowWidth = m_width * m_zoom;
	m_windowHeight = m_height * m_zoom;

	m_imageData.reset(new uint32_t[width * height]);

	if (m_renderSurface) {
		SDL_DestroySurface(m_renderSurface);
		m_renderSurface = nullptr;
	}

	m_renderSurface = SDL_CreateSurfaceFrom(m_width, m_height, SDL_PIXELFORMAT_BGRX32, m_imageData.get(), m_width * 4);
	sdlError(m_renderSurface);

	// Create our window
	info_out << "SDL: Set window size: " << m_windowWidth << " x " << m_windowHeight << std::endl;
	sdlError(SDL_SetWindowSize(m_window, m_windowWidth, m_windowHeight));
	sdlError(SDL_SetWindowFullscreen(m_window, fullScreen));
	sdlError(SDL_ShowWindow(m_window));
}

void ScreenContextRgbSDL::clear() const {
	const auto winSurface = SDL_GetWindowSurface(m_window);
	sdlError(winSurface);
	const SDL_PixelFormatDetails *format = SDL_GetPixelFormatDetails(winSurface->format);
	SDL_FillSurfaceRect(winSurface, nullptr, SDL_MapRGBA(format, nullptr, 0, 0, 0, 255));
}

InputEvents ScreenContextRgbSDL::eventLoop() {
	InputEvents input;
	SDL_Event event;
	// Mouse positions are in window coordinates, drawing is in pixels
	const float density = SDL_GetWindowPixelDensity(m_window);
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
			case SDL_EVENT_MOUSE_MOTION:
				m_controlBar.mouseMoved(event.motion.x * density, event.motion.y * density);
				break;
			case SDL_EVENT_WINDOW_MOUSE_LEAVE:
				m_controlBar.mouseLeft();
				break;
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
				if (event.button.button == SDL_BUTTON_LEFT) {
					const auto winSurface = SDL_GetWindowSurface(m_window);
					double fraction = -1;
					switch (m_controlBar.click(event.button.x * density, event.button.y * density,
					                           winSurface ? winSurface->w : 0, winSurface ? winSurface->h : 0, fraction)) {
						case ControlBar::Click::TogglePause:
							input.togglePause = true;
							break;
						case ControlBar::Click::Seek:
							input.seekFraction = fraction;
							break;
						default:
							break;
					}
				}
				break;
			case SDL_EVENT_QUIT:
				global_exit = true;
				input.quit = true;
				break;
			case SDL_EVENT_WINDOW_RESIZED:
			case SDL_EVENT_WINDOW_EXPOSED:
				// Redraw the current frame, e.g. while paused
				blitSurface();
				break;
			case SDL_EVENT_KEY_DOWN:
				switch (event.key.key) {
					case SDLK_SPACE:
					case SDLK_P:
						input.togglePause = true;
						break;
					case SDLK_LEFT:
						input.seekSeconds -= 10;
						break;
					case SDLK_RIGHT:
						input.seekSeconds += 10;
						break;
					case SDLK_DOWN:
						input.seekSeconds -= 60;
						break;
					case SDLK_UP:
						input.seekSeconds += 60;
						break;
					case SDLK_ESCAPE:
					case SDLK_Q:
						input.quit = true;
						break;
					default:
						break;
				}
				break;
			default:
				break;
		}
	}
	return input;
}
