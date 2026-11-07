// ST7789V 240x280 test pattern, to check the wiring, colours and panel offset.
//
// Shows, on black: a 1 pixel white border on all four edges, a red square in the top left corner, a green
// square in the top right, and colour bars across the top and bottom. A yellow outline marks the area
// 160x128 video would fill when centred.
//
// Wiring: SCL GP18, SDA GP19, CS GP17, DC GP20, RES GP21.

#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "hardware/clocks.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

namespace {
	constexpr uint PIN_CS = 17;
	constexpr uint PIN_SCK = 18;
	constexpr uint PIN_MOSI = 19;
	constexpr uint PIN_DC = 20;
	constexpr uint PIN_RST = 21;
	constexpr uint32_t SPI_HZ = 31250000;

	constexpr int WIDTH = 240;
	constexpr int HEIGHT = 280;
	// The controller has 320 rows of memory, this panel shows the middle 280
	constexpr int ROW_OFFSET = 20;

	constexpr uint16_t BLACK = 0x0000, WHITE = 0xFFFF, RED = 0xF800, GREEN = 0x07E0, BLUE = 0x001F, YELLOW = 0xFFE0;

	void command(const uint8_t code, const uint8_t *data = nullptr, const size_t size = 0) {
		gpio_put(PIN_CS, false);
		gpio_put(PIN_DC, false);
		spi_write_blocking(spi0, &code, 1);
		if (size) {
			gpio_put(PIN_DC, true);
			spi_write_blocking(spi0, data, size);
		}
		gpio_put(PIN_CS, true);
	}

	void init() {
		gpio_put(PIN_RST, false);
		sleep_ms(20);
		gpio_put(PIN_RST, true);
		sleep_ms(150);

		command(0x11); // Sleep out
		sleep_ms(120);
		const uint8_t colorMode = 0x55; // 16 bits per pixel
		command(0x3A, &colorMode, 1);
		const uint8_t memoryAccess = 0x00; // Top to bottom, left to right, RGB
		command(0x36, &memoryAccess, 1);
		command(0x21); // Inversion on, IPS panels need it for correct colours
		command(0x13); // Normal display mode
		command(0x29); // Display on
		sleep_ms(20);
	}

	void setWindow(const int x, const int y, const int width, const int height) {
		const int x1 = x + width - 1, y0 = y + ROW_OFFSET, y1 = y0 + height - 1;
		const uint8_t columns[] = {
			static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x),
			static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1)
		};
		const uint8_t rows[] = {
			static_cast<uint8_t>(y0 >> 8), static_cast<uint8_t>(y0),
			static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1)
		};
		command(0x2A, columns, sizeof(columns));
		command(0x2B, rows, sizeof(rows));
		command(0x2C);
	}

	void fillRect(const int x, const int y, const int width, const int height, const uint16_t color) {
		static uint8_t line[WIDTH * 2];
		for (int i = 0; i < width; i++) {
			line[i * 2] = color >> 8;
			line[i * 2 + 1] = color & 0xFF;
		}
		setWindow(x, y, width, height);
		gpio_put(PIN_CS, false);
		gpio_put(PIN_DC, true);
		for (int row = 0; row < height; row++) {
			spi_write_blocking(spi0, line, width * 2);
		}
		gpio_put(PIN_CS, true);
	}

	void outline(const int x, const int y, const int width, const int height, const uint16_t color) {
		fillRect(x, y, width, 1, color);
		fillRect(x, y + height - 1, width, 1, color);
		fillRect(x, y, 1, height, color);
		fillRect(x + width - 1, y, 1, height, color);
	}

	void drawPattern() {
		fillRect(0, 0, WIDTH, HEIGHT, BLACK);

		constexpr uint16_t bars[] = {RED, GREEN, BLUE, WHITE};
		constexpr int barWidth = (WIDTH - 40) / 4;
		for (int i = 0; i < 4; i++) {
			fillRect(20 + i * barWidth, 30, barWidth, 30, bars[i]);
			fillRect(20 + i * barWidth, HEIGHT - 60, barWidth, 30, bars[i]);
		}

		outline(40, 76, 160, 128, YELLOW);
		outline(0, 0, WIDTH, HEIGHT, WHITE);
		fillRect(4, 4, 16, 16, RED);
		fillRect(WIDTH - 20, 4, 16, 16, GREEN);
	}
}

int main() {
	stdio_init_all();

	for (const uint pin : {PIN_CS, PIN_DC, PIN_RST}) {
		gpio_init(pin);
		gpio_set_dir(pin, GPIO_OUT);
		gpio_put(pin, true);
	}
	const uint actualHz = spi_init(spi0, SPI_HZ);
	spi_set_format(spi0, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
	gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
	gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

	init();

	// Full screen fills, to measure the update rate
	constexpr int FILLS = 30;
	const uint64_t start = time_us_64();
	for (int i = 0; i < FILLS; i++) {
		fillRect(0, 0, WIDTH, HEIGHT, i & 1 ? BLUE : RED);
	}
	const uint64_t elapsedUs = time_us_64() - start;

	drawPattern();

	for (;;) {
		printf("ST7789V test pattern, sys %lu MHz, SPI %u Hz, full screen %llu us (%.1f fps)\n",
		       clock_get_hz(clk_sys) / 1000000, actualHz, elapsedUs / FILLS, FILLS * 1e6 / static_cast<double>(elapsedUs));
		sleep_ms(1000);
	}
}
