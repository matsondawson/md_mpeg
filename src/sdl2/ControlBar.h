#pragma once

#include <SDL3/SDL_surface.h>

#include <cstdint>
#include <string>

// What the control bar shows
struct ControlBarState {
	bool paused = false;
	int64_t positionMs = 0;
	int64_t durationMs = -1; // -1 if unknown
};

/**
 * Play/pause button, seek track and time, drawn over the bottom of the video.
 * Shown while the mouse moves over the window (and while paused), hidden after a short idle time.
 * All coordinates are window surface pixels.
 */
class ControlBar {
public:
	enum class Click { None, TogglePause, Seek };

	ControlBar() = default;
	~ControlBar();
	ControlBar(const ControlBar &) = delete;
	ControlBar &operator=(const ControlBar &) = delete;

	// Size multiplier for high DPI displays
	void setScale(float scale);
	void setState(const ControlBarState &state);

	void mouseMoved(float x, float y);
	void mouseLeft();

	/**
	 * Handles a click at (x, y) on a surface of the given size.
	 * @param seekFraction Set to 0..1 for Click::Seek
	 */
	Click click(float x, float y, int surfaceWidth, int surfaceHeight, double &seekFraction);

	[[nodiscard]] bool visible() const;
	// True if what would be drawn now differs from what was last drawn
	[[nodiscard]] bool needsRedraw(int surfaceWidth, int surfaceHeight) const;
	// Draws onto the bottom of target, if visible
	void draw(SDL_Surface *target);

private:
	struct Layout {
		int barY, barHeight;
		int buttonWidth;
		int trackX, trackWidth;
		int textX;
	};

	[[nodiscard]] Layout layout(int width, int height) const;
	[[nodiscard]] std::string timeText() const;
	// Fraction 0..1 of the track under the mouse, or -1
	[[nodiscard]] double hoverFraction(const Layout &l) const;
	[[nodiscard]] std::string signature(int width, int height) const;

	static void fillRect(SDL_Surface *surface, int x, int y, int w, int h, uint32_t argb);
	void drawText(SDL_Surface *surface, const std::string &text, int x, int y, uint32_t argb) const;
	[[nodiscard]] int textWidth(const std::string &text) const;
	// Time under the mouse, drawn just above the bar at the mouse position
	void drawTooltip(SDL_Surface *target, const Layout &l, double hover);
	// Reuses or (re)creates an ARGB surface of the given size
	static SDL_Surface *ensureSurface(SDL_Surface *&surface, int w, int h);

	ControlBarState m_state;
	float m_scale = 1.0f;
	float m_mouseX = -1, m_mouseY = -1;
	bool m_mouseInside = false;
	long long m_lastMouseMoveMs = -100000;

	// ARGB, blended onto the window
	SDL_Surface *m_bar = nullptr;
	SDL_Surface *m_tooltip = nullptr;
	std::string m_drawnSignature;
};
