#pragma once

// The display module the Pico apps drive, chosen with the MD_MPEG_PICO_DISPLAY CMake option, which defines
// MD_MPEG_DISPLAY_<module>. MD_MPEG_PICO_DISPLAY_ROTATION (0, 90, 180 or 270) overrides the module's
// rotation; each defaults to landscape, which suits video. Monochrome modules only turn 0 or 180.
//
// SPI modules: SCL GP18, SDA GP19, CS GP17, DC GP20, RES GP21, on SPI0. CS and RES can be left unconnected on
// modules without them by setting their pins to -1 below.
// I2C modules: SDA GP4, SCL GP5, on I2C0, address 0x3C.

#include "PicoI2cBus.h"
#include "PicoSpiBus.h"

#include "Gc9a01.h"
#include "Ili9341.h"
#include "Ili9488.h"
#include "Sh1106.h"
#include "Ssd1306.h"
#include "Ssd1331.h"
#include "Ssd1351.h"
#include "St7735.h"
#include "St7789.h"
#include "St7796.h"

namespace board {
	using display::Rotation;

	// The SPI clock is divided from clk_peri, which runs at the system clock: 200 MHz gives 50, 33.3, 25,
	// 20 or 10 MHz. Faster than a controller's rating mostly works for writes; these are what's been widely
	// used for each. Not constexpr, as the SDK's peripheral pointers are addresses cast at run time.
	inline PicoSpiBus::Config spiBus(const uint32_t hz) {
		return {spi0, 18, 19, 17, 20, 21, hz};
	}
	inline PicoI2cBus::Config i2cBus() {
		return {i2c0, 4, 5, -1, 0x3C};
	}

	constexpr Rotation rotationOr(const Rotation fallback) {
#ifdef MD_MPEG_PICO_DISPLAY_ROTATION
		static_assert(MD_MPEG_PICO_DISPLAY_ROTATION % 90 == 0 && MD_MPEG_PICO_DISPLAY_ROTATION / 90 < 4,
		              "MD_MPEG_PICO_DISPLAY_ROTATION must be 0, 90, 180 or 270");
		(void) fallback;
		return static_cast<Rotation>(MD_MPEG_PICO_DISPLAY_ROTATION / 90);
#else
		return fallback;
#endif
	}

	// For monochrome OLEDs, which only turn 0 or 180 degrees
	constexpr bool upsideDown() {
		return rotationOr(Rotation::R0) == Rotation::R180;
	}
	constexpr bool halfTurnOnly() {
		return rotationOr(Rotation::R0) == Rotation::R0 || upsideDown();
	}

#if MD_MPEG_DISPLAY_ST7789_240X280
	// R270 is landscape with the pin header on the right
	using Display = display::St7789;
	constexpr auto PANEL = Display::panel240x280(rotationOr(Rotation::R270));
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(50000000);
	}
#elif MD_MPEG_DISPLAY_ST7789_240X320
	using Display = display::St7789;
	constexpr auto PANEL = Display::panel240x320(rotationOr(Rotation::R270));
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(50000000);
	}
#elif MD_MPEG_DISPLAY_ST7789_240X240
	using Display = display::St7789;
	constexpr auto PANEL = Display::panel240x240(rotationOr(Rotation::R0));
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(50000000);
	}
#elif MD_MPEG_DISPLAY_ST7789_172X320
	using Display = display::St7789;
	constexpr auto PANEL = Display::panel172x320(rotationOr(Rotation::R270));
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(50000000);
	}
#elif MD_MPEG_DISPLAY_ST7789_135X240
	using Display = display::St7789;
	constexpr auto PANEL = Display::panel135x240(rotationOr(Rotation::R270));
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(50000000);
	}
#elif MD_MPEG_DISPLAY_ST7735_128X160
	using Display = display::St7735;
	constexpr auto PANEL = Display::panel128x160(rotationOr(Rotation::R90));
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(25000000);
	}
#elif MD_MPEG_DISPLAY_ST7735_128X128
	using Display = display::St7735;
	constexpr auto PANEL = Display::panel128x128(rotationOr(Rotation::R0));
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(25000000);
	}
#elif MD_MPEG_DISPLAY_ST7735_80X160
	using Display = display::St7735;
	constexpr auto PANEL = Display::panel80x160(rotationOr(Rotation::R90));
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(25000000);
	}
#elif MD_MPEG_DISPLAY_ILI9341_240X320
	using Display = display::Ili9341;
	constexpr auto PANEL = Display::panel240x320(rotationOr(Rotation::R90));
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(50000000);
	}
#elif MD_MPEG_DISPLAY_ST7796_320X480
	using Display = display::St7796;
	constexpr auto PANEL = Display::panel320x480(rotationOr(Rotation::R90));
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(50000000);
	}
#elif MD_MPEG_DISPLAY_ILI9488_320X480
	using Display = display::Ili9488;
	constexpr auto PANEL = Display::panel320x480(rotationOr(Rotation::R90));
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(50000000);
	}
#elif MD_MPEG_DISPLAY_GC9A01_240X240
	using Display = display::Gc9a01;
	constexpr auto PANEL = Display::panel240x240(rotationOr(Rotation::R0));
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(50000000);
	}
#elif MD_MPEG_DISPLAY_SSD1351_128X128
	using Display = display::Ssd1351;
	constexpr auto PANEL = Display::panel128x128(rotationOr(Rotation::R0));
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(20000000);
	}
#elif MD_MPEG_DISPLAY_SSD1351_128X96
	using Display = display::Ssd1351;
	constexpr auto PANEL = Display::panel128x96(rotationOr(Rotation::R0));
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(20000000);
	}
#elif MD_MPEG_DISPLAY_SSD1331_96X64
	using Display = display::Ssd1331;
	constexpr auto PANEL = Display::panel96x64(rotationOr(Rotation::R0));
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(10000000);
	}
#elif MD_MPEG_DISPLAY_SSD1306_128X64_I2C
	using Display = display::Ssd1306;
	constexpr auto PANEL = Display::panel128x64(upsideDown());
	static_assert(halfTurnOnly(), "Monochrome OLEDs only turn 0 or 180 degrees");
	using BusType = PicoI2cBus;
	inline BusType::Config busConfig() {
		return i2cBus();
	}
#elif MD_MPEG_DISPLAY_SSD1306_128X32_I2C
	using Display = display::Ssd1306;
	constexpr auto PANEL = Display::panel128x32(upsideDown());
	static_assert(halfTurnOnly(), "Monochrome OLEDs only turn 0 or 180 degrees");
	using BusType = PicoI2cBus;
	inline BusType::Config busConfig() {
		return i2cBus();
	}
#elif MD_MPEG_DISPLAY_SSD1306_128X64_SPI
	using Display = display::Ssd1306;
	constexpr auto PANEL = Display::panel128x64(upsideDown());
	static_assert(halfTurnOnly(), "Monochrome OLEDs only turn 0 or 180 degrees");
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(10000000);
	}
#elif MD_MPEG_DISPLAY_SH1106_128X64_I2C
	using Display = display::Sh1106;
	constexpr auto PANEL = Display::panel128x64(upsideDown());
	static_assert(halfTurnOnly(), "Monochrome OLEDs only turn 0 or 180 degrees");
	using BusType = PicoI2cBus;
	inline BusType::Config busConfig() {
		return i2cBus();
	}
#elif MD_MPEG_DISPLAY_SH1106_128X64_SPI
	using Display = display::Sh1106;
	constexpr auto PANEL = Display::panel128x64(upsideDown());
	static_assert(halfTurnOnly(), "Monochrome OLEDs only turn 0 or 180 degrees");
	using BusType = PicoSpiBus;
	inline BusType::Config busConfig() {
		return spiBus(10000000);
	}
#else
#error "No display module chosen, set MD_MPEG_PICO_DISPLAY"
#endif

	constexpr uint32_t SCREEN_WIDTH = PANEL.width();
	constexpr uint32_t SCREEN_HEIGHT = PANEL.height();

	// The bus and the display on it. Call display.init() after setting the system clock.
	struct DisplayModule {
		BusType bus{busConfig()};
		Display display{bus, PANEL};
	};
}
