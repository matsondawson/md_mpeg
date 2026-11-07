#pragma once

// A push button from a GPIO to ground, using the internal pull-up, debounced by polling.

#include <cstdint>

#include "pico/stdlib.h"

class Button {
	// Contacts bounce for a few milliseconds; the level must hold this long to count
	static constexpr uint32_t SETTLE_US = 20000;

	const uint m_pin;
	bool m_stablePressed = false;
	bool m_lastPressed = false;
	uint32_t m_changedUs = 0;

public:
	explicit Button(const uint pin) : m_pin(pin) {
	}

	void init() {
		gpio_init(m_pin);
		gpio_set_dir(m_pin, GPIO_IN);
		gpio_pull_up(m_pin);
	}

	/**
	 * Call often, at least every few milliseconds for a quick response.
	 * @return true once for each press, when it has settled. Holding the button doesn't repeat.
	 */
	bool pressed() {
		const bool down = !gpio_get(m_pin);
		const uint32_t now = time_us_32();
		if (down != m_lastPressed) {
			m_lastPressed = down;
			m_changedUs = now;
			return false;
		}
		if (down != m_stablePressed && now - m_changedUs >= SETTLE_US) {
			m_stablePressed = down;
			return down;
		}
		return false;
	}
};
