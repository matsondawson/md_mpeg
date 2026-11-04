#include "ControlBar.h"

#include <algorithm>
#include <cmath>

#include "../Time.h"

namespace {
	constexpr long long HIDE_AFTER_MS = 2500;

	constexpr uint32_t BACKGROUND = 0xB0101010;
	constexpr uint32_t WHITE = 0xFFFFFFFF;
	constexpr uint32_t TRACK = 0x60FFFFFF;
	constexpr uint32_t TRACK_HOVER = 0x90FFFFFF;
	constexpr uint32_t PROGRESS = 0xFFE03C3C;

	// 3x5 pixel glyphs, one row per entry, bit 2 is the left column
	struct Glyph {
		char c;
		uint8_t rows[5];
	};
	constexpr Glyph GLYPHS[] = {
		{'0', {7, 5, 5, 5, 7}}, {'1', {2, 6, 2, 2, 7}}, {'2', {7, 1, 7, 4, 7}}, {'3', {7, 1, 7, 1, 7}},
		{'4', {5, 5, 7, 1, 1}}, {'5', {7, 4, 7, 1, 7}}, {'6', {7, 4, 7, 5, 7}}, {'7', {7, 1, 1, 1, 1}},
		{'8', {7, 5, 7, 5, 7}}, {'9', {7, 5, 7, 1, 7}}, {':', {0, 2, 0, 2, 0}}, {'/', {1, 1, 2, 4, 4}},
		{'-', {0, 0, 7, 0, 0}}, {' ', {0, 0, 0, 0, 0}},
	};

	std::string formatTime(const int64_t ms, const bool withHours) {
		const int64_t total = std::max<int64_t>(0, ms) / 1000;
		const auto two = [](const int64_t v) { return std::string(v < 10 ? "0" : "") + std::to_string(v); };
		if (withHours) {
			return std::to_string(total / 3600) + ":" + two(total / 60 % 60) + ":" + two(total % 60);
		}
		return std::to_string(total / 60) + ":" + two(total % 60);
	}
}

ControlBar::~ControlBar() {
	if (m_bar) {
		SDL_DestroySurface(m_bar);
	}
	if (m_tooltip) {
		SDL_DestroySurface(m_tooltip);
	}
}

SDL_Surface *ControlBar::ensureSurface(SDL_Surface *&surface, const int w, const int h) {
	if (!surface || surface->w != w || surface->h != h) {
		if (surface) {
			SDL_DestroySurface(surface);
		}
		surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
		if (surface) {
			SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND);
		}
	}
	return surface;
}

void ControlBar::setScale(const float scale) {
	m_scale = std::max(1.0f, scale);
}

void ControlBar::setState(const ControlBarState &state) {
	m_state = state;
}

void ControlBar::mouseMoved(const float x, const float y) {
	m_mouseX = x;
	m_mouseY = y;
	m_mouseInside = true;
	m_lastMouseMoveMs = now();
}

void ControlBar::mouseLeft() {
	m_mouseInside = false;
}

bool ControlBar::visible() const {
	if (m_state.paused) {
		return true;
	}
	return m_mouseInside && now() - m_lastMouseMoveMs < HIDE_AFTER_MS;
}

ControlBar::Layout ControlBar::layout(const int width, const int height) const {
	Layout l{};
	l.barHeight = static_cast<int>(std::lround(36 * m_scale));
	l.barY = height - l.barHeight;
	l.buttonWidth = l.barHeight;
	const bool withHours = m_state.durationMs >= 3600 * 1000;
	// Room for "h:mm:ss / h:mm:ss" or "mm:ss / mm:ss"
	const int glyphAdvance = static_cast<int>(std::lround(8 * m_scale));
	const int textWidth = (withHours ? 17 : 13) * glyphAdvance;
	l.textX = width - textWidth - static_cast<int>(std::lround(10 * m_scale));
	l.trackX = l.buttonWidth + static_cast<int>(std::lround(6 * m_scale));
	l.trackWidth = std::max(1, l.textX - l.trackX - static_cast<int>(std::lround(14 * m_scale)));
	return l;
}

double ControlBar::hoverFraction(const Layout &l) const {
	if (!m_mouseInside || m_mouseY < l.barY || m_mouseX < l.trackX || m_mouseX > l.trackX + l.trackWidth) {
		return -1;
	}
	return std::clamp((m_mouseX - l.trackX) / static_cast<double>(l.trackWidth), 0.0, 1.0);
}

ControlBar::Click ControlBar::click(const float x, const float y, const int surfaceWidth, const int surfaceHeight,
                                    double &seekFraction) {
	mouseMoved(x, y);
	const auto l = layout(surfaceWidth, surfaceHeight);
	if (y < l.barY) {
		return Click::None;
	}
	if (x < l.buttonWidth) {
		return Click::TogglePause;
	}
	// Generous horizontal hit area: anywhere between the button and the text
	if (x >= l.trackX - 4 * m_scale && x <= l.trackX + l.trackWidth + 4 * m_scale) {
		seekFraction = std::clamp((x - l.trackX) / static_cast<double>(l.trackWidth), 0.0, 1.0);
		return Click::Seek;
	}
	return Click::None;
}

std::string ControlBar::timeText() const {
	const bool withHours = m_state.durationMs >= 3600 * 1000;
	const std::string duration = m_state.durationMs >= 0 ? formatTime(m_state.durationMs, withHours) : "-:--";
	return formatTime(m_state.positionMs, withHours) + " / " + duration;
}

std::string ControlBar::signature(const int width, const int height) const {
	if (!visible()) {
		return "hidden";
	}
	const auto l = layout(width, height);
	const auto hover = hoverFraction(l);
	return std::to_string(width) + "x" + std::to_string(height)
	       + (m_state.paused ? " paused " : " playing ")
	       + std::to_string(m_state.positionMs / 1000) + " "
	       + std::to_string(m_state.durationMs / 1000) + " "
	       + std::to_string(hover < 0 ? -1 : static_cast<int>(hover * l.trackWidth));
}

bool ControlBar::needsRedraw(const int surfaceWidth, const int surfaceHeight) const {
	return signature(surfaceWidth, surfaceHeight) != m_drawnSignature;
}

void ControlBar::fillRect(SDL_Surface *surface, int x, int y, int w, int h, const uint32_t argb) {
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	w = std::min(w, surface->w - x);
	h = std::min(h, surface->h - y);
	for (int row = y; row < y + h; row++) {
		auto *pixels = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(surface->pixels) + row * surface->pitch);
		std::fill(pixels + x, pixels + x + std::max(0, w), argb);
	}
}

int ControlBar::textWidth(const std::string &text) const {
	const int pixel = std::max(1, static_cast<int>(std::lround(2 * m_scale)));
	return text.empty() ? 0 : static_cast<int>(text.size()) * 4 * pixel - pixel;
}

void ControlBar::drawText(SDL_Surface *surface, const std::string &text, int x, const int y, const uint32_t argb) const {
	const int pixel = std::max(1, static_cast<int>(std::lround(2 * m_scale)));
	for (const char c : text) {
		for (const auto &glyph : GLYPHS) {
			if (glyph.c != c) {
				continue;
			}
			for (int row = 0; row < 5; row++) {
				for (int column = 0; column < 3; column++) {
					if (glyph.rows[row] & (4 >> column)) {
						fillRect(surface, x + column * pixel, y + row * pixel, pixel, pixel, argb);
					}
				}
			}
			break;
		}
		x += 4 * pixel;
	}
}

void ControlBar::draw(SDL_Surface *target) {
	m_drawnSignature = signature(target->w, target->h);
	if (!visible()) {
		return;
	}
	const auto l = layout(target->w, target->h);
	if (!ensureSurface(m_bar, target->w, l.barHeight)) {
		return;
	}

	fillRect(m_bar, 0, 0, m_bar->w, m_bar->h, BACKGROUND);

	// Play/pause button
	const int iconSize = l.barHeight / 2;
	const int iconX = (l.buttonWidth - iconSize) / 2;
	const int iconY = (l.barHeight - iconSize) / 2;
	if (m_state.paused) {
		// Play triangle
		for (int row = 0; row < iconSize; row++) {
			const int half = iconSize / 2;
			const int width = (half - std::abs(row - half)) * 2 * iconSize / std::max(1, iconSize);
			fillRect(m_bar, iconX, iconY + row, std::max(1, width * 9 / 10), 1, WHITE);
		}
	} else {
		const int barWidth = std::max(2, iconSize / 3);
		fillRect(m_bar, iconX, iconY, barWidth, iconSize, WHITE);
		fillRect(m_bar, iconX + iconSize - barWidth, iconY, barWidth, iconSize, WHITE);
	}

	// Seek track
	const double progress = m_state.durationMs > 0
		                        ? std::clamp(static_cast<double>(m_state.positionMs) / m_state.durationMs, 0.0, 1.0)
		                        : 0.0;
	const double hover = hoverFraction(l);
	const int trackHeight = std::max(2, static_cast<int>(std::lround((hover >= 0 ? 6 : 4) * m_scale)));
	const int trackY = (l.barHeight - trackHeight) / 2;
	fillRect(m_bar, l.trackX, trackY, l.trackWidth, trackHeight, TRACK);
	if (hover >= 0) {
		fillRect(m_bar, l.trackX, trackY, static_cast<int>(hover * l.trackWidth), trackHeight, TRACK_HOVER);
	}
	const int progressWidth = static_cast<int>(progress * l.trackWidth);
	fillRect(m_bar, l.trackX, trackY, progressWidth, trackHeight, PROGRESS);
	if (hover >= 0) {
		// Knob at the current position
		const int knob = trackHeight * 2;
		fillRect(m_bar, l.trackX + progressWidth - knob / 2, (l.barHeight - knob) / 2, knob, knob, PROGRESS);
	}

	if (hover >= 0) {
		// Tick where a click would seek to
		const int tick = std::max(2, static_cast<int>(std::lround(2 * m_scale)));
		fillRect(m_bar, l.trackX + static_cast<int>(hover * l.trackWidth) - tick / 2, trackY - trackHeight / 2, tick,
		         trackHeight * 2, WHITE);
	}

	// Current position / duration
	const int textY = (l.barHeight - static_cast<int>(std::lround(10 * m_scale))) / 2;
	drawText(m_bar, timeText(), l.textX, textY, WHITE);

	SDL_Rect destination = {0, l.barY, m_bar->w, m_bar->h};
	SDL_BlitSurface(m_bar, nullptr, target, &destination);

	if (hover >= 0 && m_state.durationMs > 0) {
		drawTooltip(target, l, hover);
	}
}

void ControlBar::drawTooltip(SDL_Surface *target, const Layout &l, const double hover) {
	const bool withHours = m_state.durationMs >= 3600 * 1000;
	const std::string text = formatTime(static_cast<int64_t>(hover * m_state.durationMs), withHours);
	const int padding = static_cast<int>(std::lround(6 * m_scale));
	const int textHeight = static_cast<int>(std::lround(10 * m_scale));
	const int width = textWidth(text) + padding * 2;
	const int height = textHeight + padding * 2;
	if (!ensureSurface(m_tooltip, width, height)) {
		return;
	}
	fillRect(m_tooltip, 0, 0, width, height, BACKGROUND);
	drawText(m_tooltip, text, padding, padding, WHITE);

	// Centred on the mouse, kept inside the window, just above the bar
	const int mouseX = l.trackX + static_cast<int>(hover * l.trackWidth);
	const int x = std::clamp(mouseX - width / 2, 0, std::max(0, target->w - width));
	const int y = l.barY - height - static_cast<int>(std::lround(4 * m_scale));
	SDL_Rect destination = {x, y, width, height};
	SDL_BlitSurface(m_tooltip, nullptr, target, &destination);
}
