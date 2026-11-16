// Desktop tests of the display drivers in display/, against a bus that records what they send.
//
// Checks windows and offsets against values known to work (the ST7789 240x280 module this project runs on,
// and other libraries' offsets for each rotation), the frame buffer and dither of the monochrome OLEDs, and
// the ILI9488's colour conversion.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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

namespace {
	int s_failures = 0;

	void check(const bool ok, const std::string &what) {
		if (!ok) {
			printf("FAIL: %s\n", what.c_str());
			s_failures++;
		}
	}

	struct Op {
		enum Kind { Commands, Data } kind;
		std::vector<uint8_t> bytes;

		bool operator==(const Op &other) const {
			return kind == other.kind && bytes == other.bytes;
		}
	};

	Op commands(std::vector<uint8_t> bytes) {
		return {Op::Commands, std::move(bytes)};
	}
	Op data(std::vector<uint8_t> bytes) {
		return {Op::Data, std::move(bytes)};
	}

	std::string describe(const std::vector<Op> &ops) {
		std::string text;
		for (const Op &op : ops) {
			text += op.kind == Op::Commands ? " C[" : " D[";
			for (size_t i = 0; i < op.bytes.size(); i++) {
				char hex[8];
				snprintf(hex, sizeof(hex), i ? " %02X" : "%02X", op.bytes[i]);
				text += hex;
			}
			text += "]";
		}
		return text;
	}

	// Records commands and data, joining consecutive data such as a run of pieces
	class RecordingBus final : public display::Bus {
		bool m_inRun = false;

		void add(const Op::Kind kind, const uint8_t *bytes, const size_t size) {
			if (kind == Op::Data && !ops.empty() && ops.back().kind == Op::Data && m_inRun) {
				ops.back().bytes.insert(ops.back().bytes.end(), bytes, bytes + size);
				return;
			}
			ops.push_back({kind, std::vector<uint8_t>(bytes, bytes + size)});
		}

	public:
		std::vector<Op> ops;
		uint32_t delayedMs = 0;
		bool runOpen = false;

		void init() override {
		}
		uint32_t clockHz() const override {
			return 1;
		}
		void writeCommands(const uint8_t *bytes, const size_t size) override {
			add(Op::Commands, bytes, size);
		}
		void writeCommandWithData(const uint8_t command, const uint8_t *bytes, const size_t size) override {
			add(Op::Commands, &command, 1);
			if (size) {
				ops.push_back({Op::Data, std::vector<uint8_t>(bytes, bytes + size)});
			}
		}
		void writeData(const uint8_t *bytes, const size_t size) override {
			ops.push_back({Op::Data, std::vector<uint8_t>(bytes, bytes + size)});
		}
		void beginData() override {
			check(!runOpen, "beginData() inside a run");
			runOpen = true;
			m_inRun = false;
		}
		void sendData(const uint8_t *bytes, const size_t size) override {
			check(runOpen, "sendData() outside a run");
			add(Op::Data, bytes, size);
			m_inRun = true;
		}
		void endData() override {
			check(runOpen, "endData() outside a run");
			runOpen = false;
			m_inRun = false;
		}
		void delayMs(const uint32_t ms) override {
			delayedMs += ms;
		}
	};

	// Sends a window of one colour and returns what went on the bus
	template <typename DisplayType, typename Panel>
	std::vector<Op> window(const Panel &panel, const int x, const int y, const int width, const int height) {
		RecordingBus bus;
		DisplayType display(bus, panel);
		display.init();
		bus.ops.clear();
		display.fillRect(x, y, width, height, 0x1234);
		return bus.ops;
	}

	std::vector<Op> dcsWindow(const int x0, const int x1, const int y0, const int y1, const size_t pixels) {
		std::vector<uint8_t> pixelBytes;
		for (size_t i = 0; i < pixels; i++) {
			pixelBytes.push_back(0x12);
			pixelBytes.push_back(0x34);
		}
		return {
			commands({0x2A}), data({uint8_t(x0 >> 8), uint8_t(x0), uint8_t(x1 >> 8), uint8_t(x1)}),
			commands({0x2B}), data({uint8_t(y0 >> 8), uint8_t(y0), uint8_t(y1 >> 8), uint8_t(y1)}),
			commands({0x2C}), data(pixelBytes)
		};
	}

	void expectOps(const std::vector<Op> &actual, const std::vector<Op> &expected, const std::string &what) {
		check(actual == expected, what + "\n  got     " + describe(actual) + "\n  expected" + describe(expected));
	}

	struct Offsets {
		uint16_t width, height, x, y;
	};

	void expectPanel(const display::DcsPanel &panel, const Offsets expected, const std::string &what) {
		const bool ok = panel.width() == expected.width && panel.height() == expected.height &&
		                panel.xOffset() == expected.x && panel.yOffset() == expected.y;
		char text[160];
		snprintf(text, sizeof(text), "%s: %dx%d at %d,%d, expected %dx%d at %d,%d", what.c_str(), panel.width(),
		         panel.height(), panel.xOffset(), panel.yOffset(), expected.width, expected.height, expected.x, expected.y);
		check(ok, text);
	}

	void testDcsPanels() {
		using display::Rotation;
		using display::St7789;
		using display::St7735;

		// The module this project runs on, landscape with the header on the right: the old driver's values
		const auto landscape = St7789::panel240x280(Rotation::R270);
		check(landscape.madctl == 0xA0, "ST7789 240x280 R270 MADCTL");
		expectPanel(landscape, {280, 240, 20, 0}, "ST7789 240x280 R270");
		expectPanel(St7789::panel240x280(Rotation::R0), {240, 280, 0, 20}, "ST7789 240x280 R0");

		// TFT_eSPI's offsets for 135x240 modules
		expectPanel(St7789::panel135x240(Rotation::R0), {135, 240, 52, 40}, "ST7789 135x240 R0");
		expectPanel(St7789::panel135x240(Rotation::R90), {240, 135, 40, 53}, "ST7789 135x240 R90");
		expectPanel(St7789::panel135x240(Rotation::R180), {135, 240, 53, 40}, "ST7789 135x240 R180");
		expectPanel(St7789::panel135x240(Rotation::R270), {240, 135, 40, 52}, "ST7789 135x240 R270");
		// 240x240 panels sit at the top of memory, so turning them over moves them 80 rows
		expectPanel(St7789::panel240x240(Rotation::R0), {240, 240, 0, 0}, "ST7789 240x240 R0");
		expectPanel(St7789::panel240x240(Rotation::R180), {240, 240, 0, 80}, "ST7789 240x240 R180");
		expectPanel(St7789::panel240x240(Rotation::R90), {240, 240, 0, 0}, "ST7789 240x240 R90");
		expectPanel(St7789::panel240x240(Rotation::R270), {240, 240, 80, 0}, "ST7789 240x240 R270");

		// Adafruit's 1.44 inch green tab: 2 and 3 at its rotation 0
		expectPanel(St7735::panel128x128(Rotation::R0), {128, 128, 2, 3}, "ST7735 128x128 R0");
		expectPanel(St7735::panel80x160(Rotation::R90), {160, 80, 1, 26}, "ST7735 80x160 R90");
		check(St7735::panel80x160(Rotation::R0).madctl == 0xC8, "ST7735 80x160 R0 MADCTL with BGR");

		expectPanel(display::Ili9341::panel240x320(Rotation::R90), {320, 240, 0, 0}, "ILI9341 R90");
		check(display::Ili9341::panel240x320(Rotation::R0).madctl == 0x48, "ILI9341 R0 MADCTL");
		expectPanel(display::St7796::panel320x480(Rotation::R90), {480, 320, 0, 0}, "ST7796 R90");
		expectPanel(display::Gc9a01::panel240x240(Rotation::R0), {240, 240, 0, 0}, "GC9A01 R0");
	}

	void testDcsWindows() {
		using display::Rotation;
		// The old driver's window for the player's area of a 160x128 picture
		expectOps(window<display::St7789>(display::St7789::panel240x280(Rotation::R270), 40, 0, 200, 2),
		          dcsWindow(60, 259, 0, 1, 400), "ST7789 window");
		expectOps(window<display::St7735>(display::St7735::panel128x128(Rotation::R0), 0, 0, 1, 1),
		          dcsWindow(2, 2, 3, 3, 1), "ST7735 window");
		expectOps(window<display::Ili9341>(display::Ili9341::panel240x320(Rotation::R90), 300, 230, 20, 10),
		          dcsWindow(300, 319, 230, 239, 200), "ILI9341 window");
		// Clipped to the screen
		expectOps(window<display::Ili9341>(display::Ili9341::panel240x320(Rotation::R90), 310, -5, 20, 10),
		          dcsWindow(310, 319, 0, 4, 50), "ILI9341 clipped window");
	}

	void testDcsInit() {
		RecordingBus bus;
		display::St7789 display(bus, display::St7789::panel240x280(display::Rotation::R270));
		display.init();
		expectOps(bus.ops, {
			commands({0x11}),
			commands({0x3A}), data({0x55}),
			commands({0x26}), data({0x01}),
			commands({0x36}), data({0xA0}),
			commands({0x21}),
			commands({0x13}),
			commands({0x29}),
		}, "ST7789 init");
		check(bus.delayedMs == 140, "ST7789 init delays");
	}

	// An init sequence parses to whole commands, ending with display on
	template <typename DisplayType, typename Panel>
	void expectInitEndsWithDisplayOn(const Panel &panel, const char *name) {
		RecordingBus bus;
		DisplayType display(bus, panel);
		display.init();
		check(!bus.runOpen && !bus.ops.empty() && bus.ops.back() == commands({0x29}),
		      std::string(name) + " init ends with display on");
	}

	void testDcsInitSequences() {
		using display::Rotation;
		expectInitEndsWithDisplayOn<display::St7735>(display::St7735::panel128x160(Rotation::R0), "ST7735");
		expectInitEndsWithDisplayOn<display::Ili9341>(display::Ili9341::panel240x320(Rotation::R0), "ILI9341");
		expectInitEndsWithDisplayOn<display::St7796>(display::St7796::panel320x480(Rotation::R0), "ST7796");
		expectInitEndsWithDisplayOn<display::Ili9488>(display::Ili9488::panel320x480(Rotation::R0), "ILI9488");
		expectInitEndsWithDisplayOn<display::Gc9a01>(display::Gc9a01::panel240x240(Rotation::R0), "GC9A01");
	}

	void testIli9488() {
		RecordingBus bus;
		display::Ili9488 display(bus, display::Ili9488::panel320x480(display::Rotation::R0));
		display.init();
		bus.ops.clear();
		// More pixels than one conversion buffer holds
		std::vector<uint8_t> pixels;
		for (int i = 0; i < 1000; i++) {
			const uint16_t pixel = i % 3 == 0 ? 0xF800 : i % 3 == 1 ? 0x07E0 : 0x001F;
			pixels.push_back(static_cast<uint8_t>(pixel >> 8));
			pixels.push_back(static_cast<uint8_t>(pixel));
		}
		display.beginPixels(0, 0, 100, 10);
		display.sendPixels(pixels.data(), static_cast<uint32_t>(pixels.size()));
		display.endPixels();
		const std::vector<uint8_t> &sent = bus.ops.back().bytes;
		check(sent.size() == 3000, "ILI9488 sends 3 bytes a pixel");
		bool ok = sent.size() == 3000;
		for (int i = 0; ok && i < 1000; i++) {
			const uint8_t expected[3][3] = {{0xF8, 0, 0}, {0, 0xFC, 0}, {0, 0, 0xF8}};
			for (int c = 0; c < 3; c++) {
				ok = ok && sent[i * 3 + c] == expected[i % 3][c];
			}
		}
		check(ok, "ILI9488 RGB565 to RGB666");
	}

	void testColourOleds() {
		using display::Rotation;
		std::vector<uint8_t> four;
		for (int i = 0; i < 4; i++) {
			four.push_back(0x12);
			four.push_back(0x34);
		}
		expectOps(window<display::Ssd1351>(display::Ssd1351::panel128x128(Rotation::R0), 10, 20, 2, 2), {
			commands({0x15}), data({10, 11}),
			commands({0x75}), data({20, 21}),
			commands({0x5C}), data(four),
		}, "SSD1351 window");
		// Quarter turns swap the window
		expectOps(window<display::Ssd1351>(display::Ssd1351::panel128x96(Rotation::R90), 10, 20, 4, 1), {
			commands({0x15}), data({20, 20}),
			commands({0x75}), data({10, 13}),
			commands({0x5C}), data(four),
		}, "SSD1351 R90 window");
		expectOps(window<display::Ssd1331>(display::Ssd1331::panel96x64(Rotation::R0), 90, 60, 2, 2), {
			commands({0x15, 90, 91, 0x75, 60, 61}), data(four),
		}, "SSD1331 window");
	}

	void testSsd1306() {
		RecordingBus bus;
		display::Ssd1306 oled(bus, display::Ssd1306::panel128x64());
		oled.init();
		check(bus.ops.size() == 1 && bus.ops[0].bytes.front() == 0xAE && bus.ops[0].bytes.back() == 0xAF,
		      "SSD1306 init is one run of commands");
		check(bus.ops[0].bytes[4] == 63, "SSD1306 128x64 multiplex ratio");

		bus.ops.clear();
		oled.fill(0xFFFF);
		check(bus.ops.size() == 2 && bus.ops[0] == commands({0x21, 0, 127, 0x22, 0, 7}),
		      "SSD1306 full screen window" + describe({bus.ops[0]}));
		check(bus.ops.size() == 2 && bus.ops[1].bytes == std::vector<uint8_t>(1024, 0xFF), "SSD1306 white fills every pixel");

		// A single pixel, on page 1, sends only that page
		oled.fill(0x0000);
		bus.ops.clear();
		oled.fillRect(5, 10, 1, 1, 0xFFFF);
		std::vector<uint8_t> page(128, 0);
		page[5] = 1 << 2;
		expectOps(bus.ops, {commands({0x21, 0, 127, 0x22, 1, 1}), data(page)}, "SSD1306 pixel at 5,10");

		// Mid grey dithers to half the pixels in every 4x4 block
		oled.fill(0x8410);
		int on = 0;
		for (int y = 0; y < 4; y++) {
			for (int x = 0; x < 4; x++) {
				on += oled.pixel(x, y);
			}
		}
		check(on == 8, "SSD1306 mid grey dithers to half on, got " + std::to_string(on));

		RecordingBus shortBus;
		display::Ssd1306 shortOled(shortBus, display::Ssd1306::panel128x32(true));
		shortOled.init();
		const std::vector<uint8_t> &init = shortBus.ops[0].bytes;
		check(init[4] == 31, "SSD1306 128x32 multiplex ratio");
		check(init[12] == 0xA0 && init[13] == 0xC0, "SSD1306 upside down remaps");
		shortBus.ops.clear();
		shortOled.fill(0xFFFF);
		check(shortBus.ops.size() == 2 && shortBus.ops[0] == commands({0x21, 0, 127, 0x22, 0, 3}) &&
		      shortBus.ops[1].bytes.size() == 512, "SSD1306 128x32 sends 4 pages");
	}

	void testSh1106() {
		RecordingBus bus;
		display::Sh1106 oled(bus, display::Sh1106::panel128x64());
		oled.init();
		bus.ops.clear();
		oled.fillRect(0, 60, 128, 4, 0xFFFF);
		std::vector<uint8_t> page(128, 0xF0);
		expectOps(bus.ops, {commands({0xB7, 0x02, 0x10}), data(page)}, "SH1106 last page at column 2");
	}
}

int main() {
	testDcsPanels();
	testDcsWindows();
	testDcsInit();
	testDcsInitSequences();
	testIli9488();
	testColourOleds();
	testSsd1306();
	testSh1106();
	if (s_failures) {
		printf("%d failed\n", s_failures);
		return 1;
	}
	printf("Display driver tests passed\n");
	return 0;
}
