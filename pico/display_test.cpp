// Test pattern for the display module chosen at build time (see DisplayModule.h), to check the wiring,
// colours, rotation and panel offsets.
//
// Shows, on black: a 1 pixel white border on all four edges, a red square in the top left corner, a green
// square in the top right, and red, green, blue and white bars across the top and bottom. A yellow outline
// marks the area 160x128 video would fill, scaled to the screen's height and centred as the player does.
// Rows or columns of noise at an edge, or a border missing from one, mean the panel's offsets are wrong.

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "hardware/clocks.h"
#include "pico/stdlib.h"

#include "Clocks.h"
#include "DisplayModule.h"

namespace {
	constexpr uint16_t BLACK = 0x0000, WHITE = 0xFFFF, RED = 0xF800, GREEN = 0x07E0, BLUE = 0x001F, YELLOW = 0xFFE0;

	board::DisplayModule s_module;
	auto &s_display = s_module.display;

	void outline(const int x, const int y, const int width, const int height, const uint16_t color) {
		s_display.fillRect(x, y, width, 1, color);
		s_display.fillRect(x, y + height - 1, width, 1, color);
		s_display.fillRect(x, y, 1, height, color);
		s_display.fillRect(x + width - 1, y, 1, height, color);
	}

	void drawPattern() {
		const int width = s_display.width(), height = s_display.height();
		s_display.fill(BLACK);

		constexpr uint16_t bars[] = {RED, GREEN, BLUE, WHITE};
		const int margin = width / 12, barWidth = (width - margin * 2) / 4, barHeight = height / 8;
		for (int i = 0; i < 4; i++) {
			s_display.fillRect(margin + i * barWidth, height / 8, barWidth, barHeight, bars[i]);
			s_display.fillRect(margin + i * barWidth, height - height / 8 - barHeight, barWidth, barHeight, bars[i]);
		}

		const int videoWidth = std::min(width, (160 * height + 64) / 128);
		outline((width - videoWidth) / 2, 0, videoWidth, height, YELLOW);
		outline(0, 0, width, height, WHITE);
		const int square = std::max(4, std::min(width, height) / 15);
		s_display.fillRect(2, 2, square, square, RED);
		s_display.fillRect(width - 2 - square, 2, square, square, GREEN);
	}
}

int main() {
	setupClocks(true);
	stdio_init_all();
	s_display.init();

	// Full screen fills, to measure the update rate
	constexpr int FILLS = 30;
	const uint64_t start = time_us_64();
	for (int i = 0; i < FILLS; i++) {
		s_display.fill(i & 1 ? BLUE : RED);
	}
	const uint64_t elapsedUs = time_us_64() - start;

	drawPattern();

	for (;;) {
		printf("%dx%d test pattern, sys %lu MHz, display bus %lu Hz, full screen %llu us (%.1f fps)\n",
		       s_display.width(), s_display.height(), clock_get_hz(clk_sys) / 1000000, s_module.bus.clockHz(),
		       elapsedUs / FILLS, FILLS * 1e6 / static_cast<double>(elapsedUs));
		sleep_ms(1000);
	}
}
