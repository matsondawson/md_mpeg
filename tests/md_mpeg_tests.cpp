// Decoder tests. Test clips are generated with ffmpeg (lavfi test sources), so no media is stored in
// the repository. Exits with 77 (skipped) if ffmpeg isn't available.
//
// Usage: md_mpeg_tests <directory for generated media>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "AudioStallDetector.h"
#include "DisplayTimestamper.h"
#include "VideoFrameQueue.h"
#include "stream/FileStream.h"
#include "mpeg1/MpegProgramStream.h"
#include "mpeg1/Mpeg1VideoDecoder.h"
#include "mpeg1/AudioDecoders.h"
#include "mpeg1/VideoInternal.h"
#if MD_MPEG_ENABLE_MPEG2
#include "dvd/IntroSkipper.h"
#include "dvd/Navigator.h"
#include "dvd/SubPicture.h"
#endif

namespace fs = std::filesystem;

static int g_failures = 0;
static std::string g_currentTest;

#define CHECK(cond) do { \
	if (!(cond)) { \
		g_failures++; \
		std::cout << "  FAIL " << g_currentTest << ": " #cond " (line " << __LINE__ << ")" << std::endl; \
	} \
} while (0)

#define CHECK_RANGE(value, lo, hi) do { \
	const auto v_ = (value); \
	if (v_ < (lo) || v_ > (hi)) { \
		g_failures++; \
		std::cout << "  FAIL " << g_currentTest << ": " #value " = " << v_ << ", expected " << (lo) << ".." << (hi) \
			<< " (line " << __LINE__ << ")" << std::endl; \
	} \
} while (0)

// ---------------------------------------------------------------- media generation

static fs::path g_mediaDir;

static bool run(const std::string &command) {
	return std::system((command + " >/dev/null 2>&1").c_str()) == 0;
}

static fs::path media(const std::string &name) {
	return g_mediaDir / name;
}

// Generates a clip with ffmpeg if it doesn't exist yet. Format "mpeg" has MPEG-1 pack headers, "vob" MPEG-2.
static fs::path generate(const std::string &name, const std::string &ffmpegArgs, const std::string &format = "mpeg") {
	const auto path = media(name);
	if (!fs::exists(path)) {
		if (!run("ffmpeg -nostdin -y " + ffmpegArgs + " -f " + format + " \"" + path.string() + "\"")) {
			std::cout << "  ffmpeg failed to generate " << name << std::endl;
			g_failures++;
		}
	}
	return path;
}

static std::vector<uint8_t> readFile(const fs::path &path) {
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

static fs::path writeFile(const std::string &name, const std::vector<uint8_t> &data) {
	const auto path = media(name);
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
	return path;
}

// video_idct's output on testIdctAccuracy's blocks; changes only if the IDCT's arithmetic does
static constexpr uint64_t IDCT_OUTPUT_HASH = 0x27ca6aa946670e95;

static const std::string VIDEO_320 = "-f lavfi -i testsrc=size=320x240:rate=25";
static const std::string TONE = "-f lavfi -i sine=frequency=440:sample_rate=44100";

// ---------------------------------------------------------------- decoding harness

struct Pipeline {
	MStream::FileStreamRead file;
	Mpeg1::MpegProgramStream demux;
	Mpeg1VideoDecoder video;
	AudioDecoders audio;
	DisplayTimestamper timestamper;

	int frames = 0;
	int untimedFrames = 0;
	std::vector<int64_t> displayPts; // presentation times, display order
	std::vector<int32_t> streamPts;  // picture PTS from the stream (-1 = none), display order
	uint32_t width = 0, height = 0;
	std::vector<uint32_t> firstFrameRgb;

	uint64_t audioValues = 0; // sample values decoded (samples x channels)
	uint32_t audioFrequency = 0, audioChannels = 0;
	std::vector<AudioSample> audioSamples; // in decoder order: stereo is 32 left, 32 right, ...
	std::vector<int64_t> timelinePts; // pictures with a PTS: their time on the file's timeline

	bool finished = false;
	bool threw = false;

	explicit Pipeline(const fs::path &path) : file(path.c_str()) {
		demux.setVideoDecoder(&video);
		audio.attach(demux);
		const auto [endPackPosition, endScrMillis] = Mpeg1::findLastPack(file);
		demux.setEndPack(endPackPosition, endScrMillis);
		video.setFrameCallback([this](const Frame &frame) {
			const auto pts = timestamper.next(frame.m_ptsMillis, video.m_frame_period_millis);
			if (pts < 0) {
				untimedFrames++; // the player drops these
				return;
			}
			frames++;
			displayPts.push_back(pts);
			if (frame.m_ptsMillis >= 0) {
				timelinePts.push_back(demux.timelineMillis(frame.m_ptsMillis));
			}
			streamPts.push_back(frame.m_ptsMillis);
			width = frame.m_width;
			height = frame.m_height;
			if (firstFrameRgb.empty()) {
				firstFrameRgb.resize(static_cast<size_t>(width) * height);
				video.generateRgbFrameData(firstFrameRgb.data(), frame);
			}
		});
		audio.setFrameCallback([this](const uint32_t frequency, const uint32_t channels,
		                              AudioSampleBuffer *soundBuffer) {
			audioFrequency = frequency;
			audioChannels = channels;
			// Play it all instantly
			while (!soundBuffer->isEmpty()) {
				audioSamples.push_back(soundBuffer->nextByte());
				audioValues++;
			}
		});
	}

	// Everything the demuxer and decoders reported
	[[nodiscard]] Diagnostics diagnostics() const {
		Diagnostics total = demux.diagnostics();
		total.add(video.diagnostics());
		total.add(audio.diagnostics());
		return total;
	}

	[[nodiscard]] double audioSeconds() const {
		return audioFrequency && audioChannels ? static_cast<double>(audioValues) / audioChannels / audioFrequency : 0.0;
	}

	// Demuxes until end of stream, or until stopAfterFrames frames in total. Guards against hangs.
	void runUntil(const int stopAfterFrames = INT_MAX) {
		for (long iteration = 0; iteration < 20'000'000; iteration++) {
			try {
				if (demux.nextPacket(file) == Mpeg1::DemuxResult::EndOfStream) {
					while (audio.hasPendingFrame()) {
						audio.executeCallbacks();
					}
					finished = true;
					return;
				}
			} catch (const std::exception &ex) {
				std::cout << "  exception: " << ex.what() << std::endl;
				threw = true;
				return;
			}
			if (frames >= stopAfterFrames) {
				return;
			}
		}
		std::cout << "  gave up: decoding didn't finish (hang?)" << std::endl;
	}

	// Same sequence the player uses, to a time on the file's timeline (needs demux.buildTimeline)
	void seekToTimeline(const int64_t targetMs) {
		const int64_t position = demux.findTimelinePosition(file, targetMs);
		CHECK(position >= 0);
		file.seek(position);
		demux.reset();
		demux.setInputPosition(position);
		video.reset();
		audio.reset();
		timestamper.reset();
	}

	// Same sequence the player used before timelines, to a PTS
	void seekTo(const int64_t targetMs) {
		const int64_t position = std::min<int64_t>(demux.estimateBytePosition(targetMs),
		                                           static_cast<int64_t>(file.size()));
		CHECK(position >= 0);
		file.seek(position);
		demux.reset();
		demux.setInputPosition(position);
		video.reset();
		audio.reset();
		timestamper.reset();
	}
};

static std::unique_ptr<Pipeline> decode(const fs::path &path) {
	auto pipeline = std::make_unique<Pipeline>(path);
	pipeline->runUntil();
	return pipeline;
}

// Every step between consecutive presentation times is within [lo, hi] ms
static void checkSteps(const std::vector<int64_t> &pts, const size_t from, const int64_t lo, const int64_t hi) {
	for (size_t i = from + 1; i < pts.size(); i++) {
		const auto step = pts[i] - pts[i - 1];
		if (step < lo || step > hi) {
			g_failures++;
			std::cout << "  FAIL " << g_currentTest << ": frame " << i << " step " << step << "ms, expected "
				<< lo << ".." << hi << std::endl;
			return;
		}
	}
}

// ---------------------------------------------------------------- tests

static void testClean() {
	const auto clip = generate("clean.mpg", VIDEO_320 + " " + TONE + " -t 4 -c:v mpeg1video -bf 0 -c:a mp2");
	const auto p = decode(clip);
	CHECK(p->finished);
	CHECK(p->frames == 100);
	CHECK(p->width == 320 && p->height == 240);
	checkSteps(p->displayPts, 0, 39, 41);
	CHECK(p->audioChannels == 1);
	CHECK(p->audioFrequency == 44100);
	CHECK_RANGE(p->audioSeconds(), 3.9, 4.1);
}

// Pictures are decoded out of order; each must keep its own PTS
static void testBFrames() {
	const auto clip = generate("bframes.mpg", VIDEO_320 + " " + TONE + " -t 4 -c:v mpeg1video -bf 2 -g 12 -c:a mp2");
	const auto p = decode(clip);
	CHECK(p->finished);
	CHECK(p->frames == 100);
	checkSteps(p->displayPts, 0, 39, 41);
	int32_t last = -1;
	int withPts = 0;
	for (const auto pts : p->streamPts) {
		if (pts >= 0) {
			CHECK(pts > last);
			last = pts;
			withPts++;
		}
	}
	// Several small pictures can share a packet, and only the first gets the packet's PTS
	CHECK(withPts > 20);
}

// Width/height not a multiple of 16: compare colours with ffmpeg's decode
static void testOddSize() {
	const auto clip = generate("odd.mpg", "-f lavfi -i testsrc2=size=344x250:rate=25 -t 2 -c:v mpeg1video -q:v 2 -an");
	const auto reference = media("odd.rgb");
	if (!fs::exists(reference)) {
		run("ffmpeg -nostdin -y -i \"" + clip.string() + "\" -frames:v 1 -f rawvideo -pix_fmt rgb24 \"" + reference.string() + "\"");
	}
	const auto p = decode(clip);
	CHECK(p->frames == 50);
	CHECK(p->width == 344 && p->height == 250);
	const auto expected = readFile(reference);
	CHECK(expected.size() == static_cast<size_t>(344) * 250 * 3);
	if (expected.size() == p->firstFrameRgb.size() * 3) {
		double totalDiff = 0;
		for (size_t i = 0; i < p->firstFrameRgb.size(); i++) {
			const uint32_t pixel = p->firstFrameRgb[i];
			totalDiff += std::abs(static_cast<int>((pixel >> 16) & 255) - expected[i * 3]);
			totalDiff += std::abs(static_cast<int>((pixel >> 8) & 255) - expected[i * 3 + 1]);
			totalDiff += std::abs(static_cast<int>(pixel & 255) - expected[i * 3 + 2]);
		}
		CHECK_RANGE(totalDiff / static_cast<double>(expected.size()), 0.0, 4.0);
	}
}

static void testAudioOnly() {
	const auto clip = generate("audioonly.mpg", TONE + " -t 3 -c:a mp2");
	const auto p = decode(clip);
	CHECK(p->finished);
	CHECK(p->frames == 0);
	CHECK_RANGE(p->audioSeconds(), 2.9, 3.1);
}

static void testVideoOnly() {
	const auto clip = generate("videoonly.mpg", VIDEO_320 + " -t 2 -c:v mpeg1video -an");
	const auto p = decode(clip);
	CHECK(p->finished);
	CHECK(p->frames == 50);
	CHECK(p->audioValues == 0);
}

// The decoded waveform, including its level and polarity, must match ffmpeg's decode. With MD_MPEG_AUDIO_MONO
// a stereo clip must match the average of ffmpeg's two channels.
static constexpr double MIN_SNR_DB = 55.0;
// A/52 fills bits with no mantissa bits with random noise, differently from ffmpeg
static constexpr double MIN_A52_SNR_DB = 35.0;

static void checkAudioMatchesFfmpeg(const fs::path &clip, const std::string &referenceName, const uint32_t channels,
                                    const double minSnrDb = MIN_SNR_DB, const std::string &decoderOptions = "",
                                    const std::string &outputOptions = "") {
	const auto reference = media(referenceName);
	if (!fs::exists(reference)) {
		run("ffmpeg -nostdin -y " + decoderOptions + " -i \"" + clip.string() + "\" -vn " + outputOptions + " -f s16le -acodec pcm_s16le \""
		    + reference.string() + "\"");
	}
	const auto bytes = readFile(reference);
	const auto p = decode(clip);
	CHECK(p->finished);
	const bool mixed = MD_MPEG_AUDIO_MONO && channels == 2;
	CHECK(p->audioChannels == (mixed ? 1 : channels));
	CHECK(bytes.size() >= 2);

	const auto referenceSample = [&bytes](const size_t i) {
		return static_cast<double>(static_cast<int16_t>(bytes[i * 2] | (bytes[i * 2 + 1] << 8)));
	};
	// Reference samples per decoded sample
	const size_t step = mixed ? 2 : 1;
	// Interleave to match the reference
	const size_t count = std::min(p->audioSamples.size() - p->audioSamples.size() % 64, bytes.size() / 2 / step);
	CHECK_RANGE(static_cast<double>(count * step) / static_cast<double>(bytes.size() / 2), 0.99, 1.01);
	const double toS16 = MD_MPEG_AUDIO_FIXED_POINT ? 1.0 : 32768.0;
	double sumReference = 0, sumError = 0;
	for (size_t i = 0; i < count; i++) {
		size_t decodedIndex = i;
		if (channels == 2 && !mixed) {
			const size_t frame = i / 2;
			decodedIndex = frame / 32 * 64 + (i & 1) * 32 + frame % 32;
		}
		const double decoded = p->audioSamples[decodedIndex] * toS16;
		const double expected = mixed ? (referenceSample(2 * i) + referenceSample(2 * i + 1)) / 2 : referenceSample(i);
		sumReference += expected * expected;
		sumError += (decoded - expected) * (decoded - expected);
	}
	CHECK(sumReference > 0);
	if (sumReference > 0) {
		const double snrDb = sumError > 0 ? 10.0 * std::log10(sumReference / sumError) : 200.0;
		std::cout << "  " << referenceName << ": " << snrDb << " dB from ffmpeg" << std::endl;
		CHECK(snrDb >= minSnrDb);
	}
}

static void testAudioAccuracy() {
	checkAudioMatchesFfmpeg(generate("tone.mpg", TONE + " -t 3 -c:a mp2"), "tone.pcm", 1);
	// Loud and wide-band, stereo: harder on the synthesis filter's range and precision
	const auto stereo = generate("stereo.mpg", "-f lavfi -i \"aevalsrc=0.9*sin(2*PI*(200+2000*t)*t)|0.9*random(0)-0.45:s=48000\""
	                                           " -t 3 -c:a mp2 -b:a 384k");
	checkAudioMatchesFfmpeg(stereo, "stereo.pcm", 2);
}

// MP2 against the ISO/IEC 11172-4 compliance limits, with ffmpeg's floating point decode as the reference: limited
// accuracy, RMS error below 2^-11/sqrt(12) of full scale. Full accuracy (RMS below 2^-15/sqrt(12), no sample more
// than 2^-14 out) isn't met, by the floating point build either: both are about 8e-5 RMS from ffmpeg. Mono output
// (mixed from stereo) is compared with the mean of the two channels.
static void checkMp2Compliance(const fs::path &clip, const std::string &referenceName, const uint32_t channels) {
	const auto reference = media(referenceName);
	if (!fs::exists(reference)) {
		run("ffmpeg -nostdin -y -i \"" + clip.string() + "\" -vn -f f32le -acodec pcm_f32le \"" + reference.string() + "\"");
	}
	const auto bytes = readFile(reference);
	const auto p = decode(clip);
	CHECK(p->finished);
	const bool mixed = MD_MPEG_AUDIO_MONO && channels == 2;
	const auto referenceSample = [&bytes](const size_t i) {
		float value;
		std::memcpy(&value, &bytes[i * 4], 4);
		return static_cast<double>(value);
	};
	const size_t step = mixed ? 2 : 1;
	const size_t count = std::min(p->audioSamples.size() - p->audioSamples.size() % 64, bytes.size() / 4 / step);
	CHECK(count > 10000);
	const double toUnit = MD_MPEG_AUDIO_FIXED_POINT ? 1.0 / 32768.0 : 1.0;
	double sumError = 0, maxError = 0;
	for (size_t i = 0; i < count; i++) {
		size_t decodedIndex = i;
		if (channels == 2 && !mixed) {
			const size_t frame = i / 2;
			decodedIndex = frame / 32 * 64 + (i & 1) * 32 + frame % 32;
		}
		const double decoded = p->audioSamples[decodedIndex] * toUnit;
		const double expected = mixed ? (referenceSample(2 * i) + referenceSample(2 * i + 1)) / 2 : referenceSample(i);
		const double error = decoded - expected;
		sumError += error * error;
		maxError = std::max(maxError, std::abs(error));
	}
	const double rms = std::sqrt(sumError / static_cast<double>(std::max<size_t>(count, 1)));
	std::cout << "  " << referenceName << ": RMS error " << rms << ", max " << maxError << std::endl;
	CHECK(rms < std::ldexp(1.0, -11) / std::sqrt(12.0));
}

static void testMp2Compliance() {
	checkMp2Compliance(generate("tone.mpg", TONE + " -t 3 -c:a mp2"), "tone.f32", 1);
	const auto stereo = generate("stereo.mpg", "-f lavfi -i \"aevalsrc=0.9*sin(2*PI*(200+2000*t)*t)|0.9*random(0)-0.45:s=48000\""
	                                           " -t 3 -c:a mp2 -b:a 384k");
	checkMp2Compliance(stereo, "stereo.f32", 2);
}

// The IDCT on the IEEE 1180-1990 (now ISO/IEC 23002-1) test: random blocks in three ranges and their negations,
// forward transformed in double precision, compared with the double precision inverse, must meet its limits.
// The output hash checks the sparse shortcuts (MD_MPEG_SPARSE_IDCT) give exactly the full IDCT's result, and that
// the IDCT's arithmetic hasn't changed: pico/idct_m0.S must stay bit-exact with it (pico/asm_test.cpp checks).
static void testIdctAccuracy() {
	double cosines[8][8];
	for (int u = 0; u < 8; u++) {
		for (int x = 0; x < 8; x++) {
			cosines[u][x] = (u ? 0.5 : std::sqrt(0.125)) * std::cos((2 * x + 1) * u * M_PI / 16);
		}
	}
	struct Range {
		int low, high, sign;
	};
	const Range ranges[] = {{256, 255, 1}, {5, 5, 1}, {300, 300, 1}, {256, 255, -1}, {5, 5, -1}, {300, 300, -1}};
	// IEEE 1180's limits
	constexpr int MAX_PPE = 1;
	constexpr double MAX_PMSE = 0.06, MAX_OMSE = 0.02, MAX_PME = 0.015, MAX_OME = 0.0015;
	uint64_t hash = 14695981039346656037ull;
	for (const auto &range : ranges) {
		// IEEE 1180's generator
		int64_t randx = 1;
		const auto random = [&randx](const int low, const int high) {
			randx = (randx * 1103515245 + 12345) & 0xffffffff;
			const double x = static_cast<double>(randx & 0x7ffffffe) / 0x7fffffff * (low + high + 1);
			return static_cast<int>(x) - low;
		};
		constexpr int BLOCKS = 10000;
		double squareError[64] = {}, error[64] = {};
		int ppe = 0;
		for (int n = 0; n < BLOCKS; n++) {
			double samples[64];
			for (double &sample : samples) {
				sample = range.sign * random(range.low, range.high);
			}
			int32_t coefficients[64];
			for (int u = 0; u < 8; u++) {
				for (int v = 0; v < 8; v++) {
					double sum = 0;
					for (int y = 0; y < 8; y++) {
						for (int x = 0; x < 8; x++) {
							sum += cosines[u][y] * cosines[v][x] * samples[y * 8 + x];
						}
					}
					coefficients[u * 8 + v] = std::clamp(static_cast<int32_t>(std::lround(sum)), -2048, 2047);
				}
			}
			int32_t block[64];
			for (int i = 0; i < 64; i++) {
				block[i] = videoPremultiply(coefficients[i], static_cast<uint32_t>(i));
			}
			video_idct(block);
			for (int y = 0; y < 8; y++) {
				for (int x = 0; x < 8; x++) {
					double sum = 0;
					for (int u = 0; u < 8; u++) {
						for (int v = 0; v < 8; v++) {
							sum += cosines[u][y] * cosines[v][x] * coefficients[u * 8 + v];
						}
					}
					const int i = y * 8 + x;
					const int expected = std::clamp(static_cast<int>(std::lround(sum)), -256, 255);
					const int actual = std::clamp(block[i], -256, 255);
					const int e = actual - expected;
					ppe = std::max(ppe, std::abs(e));
					squareError[i] += e * e;
					error[i] += e;
					hash = (hash ^ static_cast<uint32_t>(block[i])) * 1099511628211ull;
				}
			}
		}
		double pmse = 0, omse = 0, pme = 0, ome = 0;
		for (int i = 0; i < 64; i++) {
			pmse = std::max(pmse, squareError[i] / BLOCKS);
			pme = std::max(pme, std::abs(error[i] / BLOCKS));
			omse += squareError[i];
			ome += error[i];
		}
		omse /= 64.0 * BLOCKS;
		ome = std::abs(ome / (64.0 * BLOCKS));
		std::cout << "  range -" << range.low << ".." << range.high << (range.sign < 0 ? " negated" : "") << ": ppe " << ppe
			<< ", pmse " << pmse << ", omse " << omse << ", pme " << pme << ", ome " << ome << std::endl;
		CHECK(ppe <= MAX_PPE);
		CHECK(pmse <= MAX_PMSE);
		CHECK(omse <= MAX_OMSE);
		CHECK(pme <= MAX_PME);
		CHECK(ome <= MAX_OME);
	}
	std::cout << "  output hash " << std::hex << hash << std::dec << std::endl;
	CHECK(hash == IDCT_OUTPUT_HASH);

	// Intra DC values are premultiplied by a shift
	CHECK(VIDEO_PREMULTIPLIER_MATRIX[0] == 1 << (VIDEO_DC_PREMULTIPLY_SHIFT + 8));

	int32_t zeros[64] = {};
	video_idct(zeros);
	CHECK(std::all_of(std::begin(zeros), std::end(zeros), [](const int32_t v) { return v == 0; }));
}

// The decoder only checks MPEG-1 motion vectors against the luma plane: wherever the luma prediction is inside the
// picture, the chroma one (the vector halved, truncated) must be inside the chroma plane, for every position and
// vector that could reach the edges. Also checks the luma bounds themselves at the corners.
static void testMotionVectorRange() {
	uint64_t inRange = 0, failures = 0;
	for (uint32_t width = 1; width <= 4; width++) {
		for (uint32_t height = 1; height <= 3; height++) {
			for (uint32_t row = 0; row < height; row++) {
				for (uint32_t col = 0; col < width; col++) {
					for (int32_t h = -2 * 16 * 4 - 4; h <= 2 * 16 * 4 + 4; h++) {
						for (int32_t v = -2 * 16 * 3 - 4; v <= 2 * 16 * 3 + 4; v++) {
							if (!videoMotionInRange(col, row, width, height, h, v)) {
								continue;
							}
							inRange++;
							if (!videoMotionInRange(col, row, width, height, h / 2, v / 2, 8)) {
								failures++;
							}
						}
					}
				}
			}
		}
	}
	CHECK(inRange > 100000);
	CHECK(failures == 0);
	// One macroblock picture: only the zero vector fits; half pel needs a pixel beyond the edge
	CHECK(videoMotionInRange(0, 0, 1, 1, 0, 0));
	CHECK(!videoMotionInRange(0, 0, 1, 1, 1, 0));
	CHECK(!videoMotionInRange(0, 0, 1, 1, 0, 1));
	CHECK(!videoMotionInRange(0, 0, 1, 1, -1, 0));
	// Two wide: right by up to 16 whole pixels, or 15.5
	CHECK(videoMotionInRange(0, 0, 2, 1, 32, 0));
	CHECK(videoMotionInRange(0, 0, 2, 1, 31, 0));
	CHECK(!videoMotionInRange(0, 0, 2, 1, 33, 0));
	CHECK(!videoMotionInRange(2, 0, 2, 1, 0, 0));
}

// Extension and user data after sequence, group and picture headers must be skipped, without being reported as
// corruption. Fed straight to the video decoder, as this needs an elementary stream edited at start codes.
static void testExtensionAndUserData() {
	const auto path = media("video.m1v");
	if (!fs::exists(path)) {
		run("ffmpeg -nostdin -y " + VIDEO_320 + " -t 2 -c:v mpeg1video -bf 2 -g 12 -f mpeg1video \"" + path.string() + "\"");
	}
	const auto stream = readFile(path);

	const std::vector<uint8_t> extra = {
		0, 0, 1, 0xB5, 'e', 'x', 't', 0xFF,
		0, 0, 1, 0xB2, 'u', 's', 'e', 'r', 0xFF,
	};
	std::vector<uint8_t> edited;
	int inserted = 0;
	bool pictureHeader = false;
	bool sequenceHeader = false;
	for (size_t i = 0; i < stream.size(); i++) {
		const bool startCode = i + 3 < stream.size() && stream[i] == 0 && stream[i + 1] == 0 && stream[i + 2] == 1;
		if (startCode && sequenceHeader) {
			// The sequence header ends at the next start code
			sequenceHeader = false;
			edited.insert(edited.end(), extra.begin(), extra.end());
			inserted++;
		}
		if (startCode && stream[i + 3] == 0xB3) {
			sequenceHeader = true;
		} else if (startCode && stream[i + 3] == 0x00) {
			pictureHeader = true;
		} else if (startCode && stream[i + 3] == 0x01 && pictureHeader) {
			// First slice of a picture
			pictureHeader = false;
			edited.insert(edited.end(), extra.begin(), extra.end());
			inserted++;
		} else if (startCode && stream[i + 3] == 0xB8) {
			// The group header is 4 bytes after its start code
			edited.insert(edited.end(), stream.begin() + static_cast<std::ptrdiff_t>(i), stream.begin() + static_cast<std::ptrdiff_t>(i + 8));
			edited.insert(edited.end(), extra.begin(), extra.end());
			inserted++;
			i += 7;
			continue;
		}
		edited.push_back(stream[i]);
	}
	CHECK(inserted > 50);

	const auto video = std::make_unique<Mpeg1VideoDecoder>();
	int frames = 0;
	video->setFrameCallback([&frames](const Frame &) { frames++; });
	for (const auto byte : edited) {
		video->writeByte(byte);
	}
	video->flush();
	CHECK(frames == 50);
	// Nothing reported as corrupt
	CHECK(video->diagnostics().total() == 0);
}

// Only MP2 is supported; other audio is ignored rather than decoded as noise
static void testUnsupportedAudio() {
	const auto clip = generate("mp3audio.mpg", VIDEO_320 + " " + TONE + " -t 2 -c:v mpeg1video -c:a libmp3lame");
	const auto p = decode(clip);
	CHECK(p->finished);
	CHECK(p->frames == 50);
	CHECK(p->audioValues == 0);
}

static void testTruncated() {
	auto data = readFile(generate("bframes.mpg", ""));
	data.resize(data.size() * 7 / 10 + 13);
	const auto p = decode(writeFile("truncated.mpg", data));
	CHECK(p->finished);
	CHECK(!p->threw);
	CHECK_RANGE(p->frames, 50, 90);
}

// Random bytes overwritten: playback must carry on to the end
static void testCorrupt() {
	auto data = readFile(generate("bframes.mpg", ""));
	uint32_t seed = 12345;
	auto random = [&seed] { seed = seed * 1103515245 + 12345; return seed >> 8; };
	for (int i = 0; i < 300; i++) {
		data[data.size() / 10 + random() % (data.size() * 8 / 10)] = static_cast<uint8_t>(random());
	}
	const auto p = decode(writeFile("corrupt.mpg", data));
	CHECK(p->finished);
	CHECK(!p->threw);
	CHECK_RANGE(p->frames, 20, 100);
	CHECK(p->audioSeconds() > 2.0);
	// The damage is counted, and a clean decode reports none
	CHECK(p->diagnostics().total() > 0);
	CHECK(decode(generate("bframes.mpg", ""))->diagnostics().total() == 0);
}

// Two files joined: PTS restart in the middle, the timeline must stay continuous
static void testConcatenated() {
	auto data = readFile(generate("clean.mpg", ""));
	const auto second = data;
	data.insert(data.end(), second.begin(), second.end());
	const auto p = decode(writeFile("concat.mpg", data));
	CHECK(p->finished);
	CHECK(p->frames == 200);
	checkSteps(p->displayPts, 0, 39, 41);
}

// An audio output that stops calling back counts as stalled once STALL_MS passes, and stays so until reset.
// Pauses and the wait for the first callback don't count.
static void testAudioStall() {
	constexpr int64_t stall = AudioStallDetector::STALL_MS;
	AudioStallDetector detector;
	detector.started(1000);
	// Waiting for the first callback
	CHECK(!detector.update(1000 + stall, 0));
	CHECK(detector.update(1001 + stall, 0));
	CHECK(detector.stalled());
	// Sticky, even if callbacks come back
	CHECK(detector.update(2000 + stall, 2000 + stall));

	detector.reset();
	CHECK(!detector.stalled());
	detector.started(5000);
	// Regular callbacks
	for (int64_t t = 5000; t < 10000; t += 20) {
		CHECK(!detector.update(t + 15, t));
	}
	// Resumed after a long pause: nothing expected from before
	detector.started(20000);
	CHECK(!detector.update(20000 + stall, 9980));
	// Callbacks stop
	CHECK(!detector.update(20100 + stall, 20100));
	CHECK(detector.update(20101 + stall, 20100));
}

static void testSeek() {
	const auto clip = generate("long.mpg", VIDEO_320 + " " + TONE + " -t 20 -c:v mpeg1video -bf 2 -g 12 -c:a mp2");
	Pipeline p(clip);

	p.runUntil(50); // ~2s in
	CHECK(p.frames >= 50);

	// Forward
	p.seekTo(12000);
	const size_t forwardIndex = p.displayPts.size();
	p.runUntil(p.frames + 10);
	CHECK(p.displayPts.size() > forwardIndex);
	if (p.displayPts.size() > forwardIndex) {
		CHECK_RANGE(p.displayPts[forwardIndex], 10500, 13500);
		checkSteps(p.displayPts, forwardIndex, 39, 41);
	}
	CHECK_RANGE(p.audio.firstFramePtsMillis(), 10500, 13500);
	CHECK(p.untimedFrames <= 12);

	// Backward
	p.seekTo(3000);
	const size_t backIndex = p.displayPts.size();
	p.runUntil();
	CHECK(p.finished);
	CHECK(p.displayPts.size() > backIndex);
	if (p.displayPts.size() > backIndex) {
		CHECK_RANGE(p.displayPts[backIndex], 1500, 4500);
		checkSteps(p.displayPts, backIndex, 39, 41);
		CHECK_RANGE(p.displayPts.back(), 19000, 20500);
	}
}

// A sequence header that changes the picture size, with no sequence end code before it, after a picture is missing
// (as when a stream is cut). The last anchor picture of the first sequence is then still held back for display order
// when that header is read, and is output after it, so it must keep its own size.
static void testSizeChange() {
	std::vector<uint8_t> stream;
	// 52 pictures end with P B B in coded order. Without the last B, the P is held back until the next group.
	for (const std::string size: {"320x240", "480x360"}) {
		const auto path = media("size" + size + ".m1v");
		if (!fs::exists(path)) {
			run("ffmpeg -nostdin -y -f lavfi -i testsrc=size=" + size + ":rate=25 -frames:v 52 -c:v mpeg1video -bf 2"
			    " -g 12 -f mpeg1video \"" + path.string() + "\"");
		}
		auto clip = readFile(path);
		const std::vector<uint8_t> sequenceEnd = {0, 0, 1, 0xB7};
		if (clip.size() >= 4 && std::equal(sequenceEnd.begin(), sequenceEnd.end(), clip.end() - 4)) {
			clip.resize(clip.size() - 4);
		}
		if (stream.empty()) {
			const std::vector<uint8_t> pictureStart = {0, 0, 1, 0};
			const auto last = std::find_end(clip.begin(), clip.end(), pictureStart.begin(), pictureStart.end());
			CHECK(last != clip.end());
			clip.erase(last, clip.end());
		}
		stream.insert(stream.end(), clip.begin(), clip.end());
	}

	const auto video = std::make_unique<Mpeg1VideoDecoder>();
	std::vector<std::pair<uint32_t, uint32_t>> sizes;
	bool decoderAhead = false;
	std::vector<uint32_t> rgb;
	video->setFrameCallback([&](const Frame &frame) {
		sizes.emplace_back(frame.m_width, frame.m_height);
		CHECK(frame.m_width <= frame.m_frameSize.luma_width && frame.m_height <= frame.m_frameSize.luma_height);
		decoderAhead |= frame.m_width != video->m_width;
		rgb.assign(static_cast<size_t>(frame.m_width) * frame.m_height, 0);
		video->generateRgbFrameData(rgb.data(), frame);
	});
	for (const auto byte: stream) {
		video->writeByte(byte);
	}
	video->flush();
	// The case this tests: a picture output after the decoder moved on to the next size
	CHECK(decoderAhead);
	CHECK(sizes.size() == 103);
	for (size_t i = 0; i < sizes.size(); i++) {
		const auto expected = i < 51 ? std::pair<uint32_t, uint32_t>{320, 240} : std::pair<uint32_t, uint32_t>{480, 360};
		if (sizes[i] != expected) {
			g_failures++;
			std::cout << "  FAIL " << g_currentTest << ": picture " << i << " is " << sizes[i].first << "x"
				<< sizes[i].second << std::endl;
			break;
		}
	}
}

#if MD_MPEG_ENABLE_MPEG2
// ---------------------------------------------------------------- MPEG-2

// Decodes the video, every picture in display order as yuv420p cropped to the picture size, like ffmpeg's rawvideo
static std::vector<std::vector<uint8_t>> decodeYuv(const fs::path &path, bool &mpeg2) {
	MStream::FileStreamRead file(path.c_str());
	Mpeg1::MpegProgramStream demux;
	const auto video = std::make_unique<Mpeg1VideoDecoder>();
	demux.setVideoDecoder(video.get());
	std::vector<std::vector<uint8_t>> frames;
	video->setFrameCallback([&](const Frame &frame) {
		const uint32_t w = frame.m_width, h = frame.m_height;
		std::vector<uint8_t> yuv;
		yuv.reserve(w * h * 3 / 2);
		const auto copy = [&](const uint8_t *plane, const uint32_t stride, const uint32_t pw, const uint32_t ph) {
			for (uint32_t y = 0; y < ph; y++) {
				yuv.insert(yuv.end(), plane + y * stride, plane + y * stride + pw);
			}
		};
		copy(frame.m_yData, frame.m_frameSize.luma_width, w, h);
		copy(frame.m_cbData, frame.m_frameSize.chroma_width, (w + 1) / 2, (h + 1) / 2);
		copy(frame.m_crData, frame.m_frameSize.chroma_width, (w + 1) / 2, (h + 1) / 2);
		frames.push_back(std::move(yuv));
	});
	for (long i = 0; i < 20'000'000; i++) {
		try {
			if (demux.nextPacket(file) == Mpeg1::DemuxResult::EndOfStream) {
				break;
			}
		} catch (const std::exception &ex) {
			std::cout << "  exception: " << ex.what() << std::endl;
			g_failures++;
			break;
		}
	}
	mpeg2 = video->m_mpeg2;
	return frames;
}

// Every picture must match ffmpeg's decode. The IDCTs differ, and the difference builds up through predicted
// pictures, so this is a PSNR floor rather than exact equality. The mean is about 64 to 74 dB with the IEEE 1180
// accurate IDCT, and was 58 to 62 dB before it, so the mean floor catches a less accurate IDCT.
static constexpr double MIN_MEAN_PSNR_DB = 63.0;

static void checkVideoMatchesFfmpeg(const fs::path &clip, const int expectedFrames, const bool expectMpeg2,
                                    const double minPsnrDb = 40.0) {
	if (!fs::exists(clip)) {
		return;
	}
	const auto reference = fs::path(clip.string() + ".yuv");
	if (!fs::exists(reference)) {
		run("ffmpeg -nostdin -y -i \"" + clip.string() + "\" -an -fps_mode passthrough -f rawvideo -pix_fmt yuv420p \""
		    + reference.string() + "\"");
	}
	const auto expected = readFile(reference);
	bool mpeg2 = false;
	const auto frames = decodeYuv(clip, mpeg2);
	CHECK(mpeg2 == expectMpeg2);
	CHECK(static_cast<int>(frames.size()) == expectedFrames);
	if (frames.empty()) {
		return;
	}
	const size_t frameBytes = frames[0].size();
	CHECK(expected.size() == frameBytes * expectedFrames);
	double minPsnr = 200, sumPsnr = 0;
	size_t minPsnrIndex = 0;
	size_t compared = 0;
	for (size_t i = 0; i < frames.size() && (i + 1) * frameBytes <= expected.size(); i++) {
		double sumSquares = 0;
		for (size_t j = 0; j < frameBytes; j++) {
			const double d = static_cast<double>(frames[i][j]) - expected[i * frameBytes + j];
			sumSquares += d * d;
		}
		const double mse = sumSquares / static_cast<double>(frameBytes);
		const double psnr = mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 100.0;
		if (psnr < minPsnr) {
			minPsnr = psnr;
			minPsnrIndex = i;
		}
		sumPsnr += psnr;
		compared++;
	}
	std::cout << "  " << clip.filename().string() << ": " << compared << " pictures, PSNR from ffmpeg min " << minPsnr
		<< " dB (picture " << minPsnrIndex << "), mean " << sumPsnr / static_cast<double>(compared) << " dB" << std::endl;
	CHECK(minPsnr >= minPsnrDb);
	CHECK(sumPsnr / static_cast<double>(compared) >= MIN_MEAN_PSNR_DB);
}

static const std::string VIDEO_MOVING = "-f lavfi -i testsrc2=size=352x288:rate=25";

// The same comparison for MPEG-1, the baseline for the MPEG-2 PSNR floors
static void testMpeg1MatchesFfmpeg() {
	checkVideoMatchesFfmpeg(generate("m1.mpg", VIDEO_MOVING + " -t 2 -c:v mpeg1video -bf 2 -g 12 -an"), 50, false);
}

static void testMpeg2Progressive() {
	const auto clip = generate("m2.vob", VIDEO_MOVING + " " + TONE + " -t 2 -c:v mpeg2video -bf 2 -g 12 -c:a mp2", "vob");
	checkVideoMatchesFfmpeg(clip, 50, true);
	// The player's path, with audio
	const auto p = decode(clip);
	CHECK(p->finished);
	CHECK(p->frames == 50);
	checkSteps(p->displayPts, 0, 39, 41);
	CHECK_RANGE(p->audioSeconds(), 1.9, 2.1);
}

// Field DCT and field motion compensation in frame pictures
static void testMpeg2Interlaced() {
	checkVideoMatchesFfmpeg(generate("m2interlaced.vob", VIDEO_MOVING + ",setfield=tff -t 2 -c:v mpeg2video"
	                                                     " -flags +ildct+ilme -bf 2 -g 12 -an", "vob"), 50, true);
	checkVideoMatchesFfmpeg(generate("m2interlacedbff.vob", VIDEO_MOVING + ",setfield=bff -t 2 -c:v mpeg2video"
	                                                        " -flags +ildct+ilme -bf 2 -g 12 -an", "vob"), 50, true);
}

// Interlaced frames are flagged with their field order; progressive ones aren't
static void testInterlaceFlags() {
	const auto flags = [](const fs::path &clip, int &interlaced, int &topFirst) {
		interlaced = topFirst = 0;
		if (!fs::exists(clip)) {
			g_failures++;
			std::cout << "  missing " << clip << std::endl;
			return 0;
		}
		MStream::FileStreamRead file(clip.c_str());
		Mpeg1::MpegProgramStream demux;
		Mpeg1VideoDecoder video;
		demux.setVideoDecoder(&video);
		int frames = 0;
		video.setFrameCallback([&](const Frame &frame) {
			frames++;
			interlaced += frame.m_interlaced;
			topFirst += frame.m_interlaced && frame.m_topFieldFirst;
		});
		for (int i = 0; i < 1000000 && demux.nextPacket(file) != Mpeg1::DemuxResult::EndOfStream; i++) {
		}
		return frames;
	};
	// Made by the MPEG-2 tests
	int interlaced, topFirst;
	CHECK(flags(media("m2interlaced.vob"), interlaced, topFirst) == 50);
	CHECK(interlaced == 50 && topFirst == 50);
	CHECK(flags(media("m2interlacedbff.vob"), interlaced, topFirst) == 50);
	CHECK(interlaced == 50 && topFirst == 0);
	CHECK(flags(media("m2.vob"), interlaced, topFirst) > 0);
	CHECK(interlaced == 0);
}

// Bob: an interlaced frame shows its fields one after the other, each at full height
static void testBobDeinterlace() {
	// One pixel wide, a line of each field in turn
	const uint32_t woven[] = {0x00000000, 0x10101010, 0x20202020, 0x30303030};
	uint32_t out[4];
	Deinterlace::bobField(woven, out, 1, 4, true);
	CHECK(out[0] == 0x00000000 && out[1] == 0x10101010 && out[2] == 0x20202020 && out[3] == 0x20202020);
	Deinterlace::bobField(woven, out, 1, 4, false);
	CHECK(out[0] == 0x10101010 && out[1] == 0x10101010 && out[2] == 0x20202020 && out[3] == 0x30303030);
	// Each byte averaged on its own, rounding up
	const uint32_t bytes[] = {0x01FF0080, 0, 0x02FFFF81};
	uint32_t averaged[3];
	Deinterlace::bobField(bytes, averaged, 1, 3, true);
	CHECK(averaged[1] == 0x02FF8081);

	VideoFrameQueue queue(4);
	queue.setDeinterlaceMode(Deinterlace::Mode::Bob);
	std::fill_n(queue.push(1000, 1, 4, 0), 4, 0);
	queue.setInterlaced(false, 20);
	std::copy(woven, woven + 4, queue.push(1040, 1, 4, 0));
	VideoFrameQueue::Show show;
	CHECK(!queue.nextToShow(999, show));
	CHECK(queue.nextToShow(1000, show) && show == VideoFrameQueue::Show::FirstField);
	queue.presented(show);
	CHECK(!queue.nextToShow(1019, show));
	CHECK(queue.nextToShow(1020, show) && show == VideoFrameQueue::Show::SecondField);
	queue.presented(show);
	// The next frame is progressive
	CHECK(queue.nextToShow(1040, show) && show == VideoFrameQueue::Show::Frame);
	queue.render(show, out);
	CHECK(out[3] == 0x30303030);
	queue.presented(show);
	CHECK(queue.empty());

	// Bottom field first: the bottom field's lines come first
	std::copy(woven, woven + 4, queue.push(2000, 1, 4, 0));
	queue.setInterlaced(false, 20);
	CHECK(queue.nextToShow(2000, show) && show == VideoFrameQueue::Show::FirstField);
	queue.render(show, out);
	CHECK(out[0] == 0x10101010 && out[3] == 0x30303030);
	// Late: straight to the second field
	queue.presented(show);
	std::copy(woven, woven + 4, queue.push(3000, 1, 4, 0));
	queue.setInterlaced(true, 20);
	CHECK(queue.nextToShow(3025, show) && show == VideoFrameQueue::Show::SecondField);
	queue.render(show, out);
	CHECK(out[1] == 0x10101010 && out[0] == 0x10101010);
	queue.presented(show);

	// Deinterlacing off: shown whole
	queue.setDeinterlaceMode(Deinterlace::Mode::Off);
	std::copy(woven, woven + 4, queue.push(4000, 1, 4, 0));
	queue.setInterlaced(true, 20);
	CHECK(queue.nextToShow(4000, show) && show == VideoFrameQueue::Show::Frame);
	queue.render(show, out);
	CHECK(std::equal(woven, woven + 4, out));
	queue.presented(show);
	CHECK(queue.empty());
}

// Yadif keeps still detail from the other field, so a still horizontal edge stays put in both fields, and
// interpolates where the fields differ, so combing from motion goes
static void testYadifDeinterlace() {
	constexpr uint32_t width = 8, height = 8;
	const auto picture = [&](const auto &pixel) {
		std::vector<uint32_t> p(width * height);
		for (uint32_t y = 0; y < height; y++) {
			for (uint32_t x = 0; x < width; x++) {
				p[y * width + x] = pixel(x, y);
			}
		}
		return p;
	};
	std::vector<uint32_t> out(width * height);

	// Still: a horizontal edge, and a diagonal one
	const auto still = picture([](const uint32_t x, const uint32_t y) {
		return (y >= 4 ? 0xC8C8C8u : 0x101010u) + (x + y > 8 ? 0x200000u : 0u);
	});
	for (const bool top: {true, false}) {
		for (const bool first: {true, false}) {
			Deinterlace::yadifField(still.data(), still.data(), still.data(), out.data(), width, height, top, first);
			CHECK(out == still);
		}
	}

	// Moving: the bottom field is far from the top field and from the fields before and after
	const auto combed = picture([](uint32_t, const uint32_t y) { return y & 1 ? 0xC8C8C8u : 0x101010u; });
	const auto flat = picture([](uint32_t, uint32_t) { return 0x101010u; });
	Deinterlace::yadifField(flat.data(), combed.data(), flat.data(), out.data(), width, height, true, true);
	CHECK(out == flat);

	// Through the queue: the frame before is kept for the first field of the next
	VideoFrameQueue queue(4);
	std::copy(still.begin(), still.end(), queue.push(0, width, height, 0));
	queue.setInterlaced(true, 20);
	VideoFrameQueue::Show show;
	CHECK(queue.nextToShow(0, show));
	queue.render(show, out.data());
	CHECK(out == still);
	queue.presented(show);
	CHECK(queue.nextToShow(20, show) && show == VideoFrameQueue::Show::SecondField);
	queue.render(show, out.data());
	CHECK(out == still);
	queue.presented(show);
	std::copy(still.begin(), still.end(), queue.push(40, width, height, 0));
	queue.setInterlaced(true, 20);
	CHECK(queue.nextToShow(40, show));
	queue.render(show, out.data());
	CHECK(out == still);
}

// Alternate scan, intra VLC table B-15, non-linear quantiser scale, 11-bit intra DC, custom matrices
static void testMpeg2CodingOptions() {
	checkVideoMatchesFfmpeg(generate("m2options.vob", VIDEO_MOVING + " -t 2 -c:v mpeg2video -alternate_scan 1"
	                                                  " -intra_vlc 1 -non_linear_quant 1 -qmax 28 -b:v 3M -intra_dc_precision 3 -bf 2 -g 12 -an"
	                                                  " -intra_matrix 8,16,19,22,26,27,29,34,16,16,22,24,27,29,34,37,19,22,26,27,29,34,34,38,22,22,26,27,29,34,37,40,22,26,27,29,32,35,40,48,26,27,29,32,35,40,48,58,26,27,29,34,38,46,56,69,27,29,35,38,46,56,69,83"
	                                                  " -inter_matrix 16,17,18,19,20,21,22,23,17,18,19,20,21,22,23,24,18,19,20,21,22,23,24,25,19,20,21,22,23,24,26,27,20,21,22,23,25,26,27,28,21,22,23,24,26,27,28,30,22,23,24,26,27,28,30,31,23,24,25,27,28,30,31,33",
	                                 "vob"), 50, true);
	// Big motion: f_code above 7
	checkVideoMatchesFfmpeg(generate("m2fast.vob", "-f lavfi -i \"testsrc2=size=704x576:rate=25,scroll=h=0.04:v=0.03\""
	                                               " -t 2 -c:v mpeg2video -me_range 400 -bf 2 -g 12 -an", "vob"), 50, true);
}

// Each video in the other system stream: detection is per layer
static void testMpeg2MixedLayers() {
	checkVideoMatchesFfmpeg(generate("m2inm1.mpg", VIDEO_MOVING + " -t 1 -c:v mpeg2video -bf 2 -an"), 25, true);
	checkVideoMatchesFfmpeg(generate("m1inm2.vob", VIDEO_MOVING + " -t 1 -c:v mpeg1video -bf 2 -an", "vob"), 25, false);
}

// Dual prime motion compensation, which ffmpeg's encoder doesn't use. Needs mjpegtools' mpeg2enc.
static void testMpeg2DualPrime() {
	if (!run("command -v mpeg2enc")) {
		std::cout << "  mpeg2enc not found, skipping" << std::endl;
		return;
	}
	const auto clip = media("m2dualprime.vob");
	if (!fs::exists(clip)) {
		const auto elementary = media("m2dualprime.m2v");
		run("ffmpeg -nostdin -y -f lavfi -i \"testsrc2=size=720x576:rate=25,setfield=tff\" -t 2 -pix_fmt yuv420p"
		    " -f yuv4mpegpipe - | mpeg2enc -f 8 -I 1 -b 6000 -R 0 --dualprime-mpeg2 -o \"" + elementary.string() + "\"");
		run("ffmpeg -nostdin -y -i \"" + elementary.string() + "\" -c copy -f vob \"" + clip.string() + "\"");
	}
	CHECK(fs::exists(clip));
	checkVideoMatchesFfmpeg(clip, 50, true);
}

static void testMpeg2Seek() {
	const auto clip = generate("m2long.vob", VIDEO_320 + " " + TONE + " -t 20 -c:v mpeg2video -bf 2 -g 12 -c:a mp2", "vob");
	Pipeline p(clip);
	p.runUntil(50);
	CHECK_RANGE(p.demux.durationMillis(), 19000, 20500);
	p.seekTo(12000);
	const size_t index = p.displayPts.size();
	p.runUntil(p.frames + 10);
	CHECK(p.displayPts.size() > index);
	if (p.displayPts.size() > index) {
		CHECK_RANGE(p.displayPts[index], 10500, 13500);
		checkSteps(p.displayPts, index, 39, 41);
	}
}

// Decoding that starts mid-stream, like a .vob that isn't the first of a title: the first group's B-pictures, and
// any picture predicted from one that wasn't decoded, have reference pictures that were never decoded
static void testMpeg2StartMidStream() {
	const auto data = readFile(generate("m2long.vob", VIDEO_320 + " " + TONE + " -t 20 -c:v mpeg2video -bf 2 -g 12 -c:a mp2",
	                                    "vob"));
	for (const size_t start: {data.size() * 2 / 5, data.size() / 2 + 777}) {
		const auto p = decode(writeFile("m2midstream.vob", std::vector<uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(start), data.end())));
		CHECK(p->finished);
		CHECK(!p->threw);
		CHECK_RANGE(p->frames, 200, 300);
		// Audio starting mid-frame isn't mistaken for an unsupported format
		CHECK(p->audioSeconds() > 9.0);
	}
	// Two clips of different sizes joined: the second's first pictures can't predict from the first's
	auto joined = readFile(generate("m2long.vob", ""));
	const auto other = readFile(generate("m2interlaced.vob", ""));
	joined.insert(joined.end(), other.begin() + static_cast<std::ptrdiff_t>(other.size() / 3), other.end());
	const auto p = decode(writeFile("m2sizes.vob", joined));
	CHECK(p->finished);
	CHECK(!p->threw);
}

// ---------------------------------------------------------------- A/52 audio

// ffmpeg's A/52 encoder
static const std::string A52 = " -c:a ac3";
static const std::string STEREO_SWEEP = "-f lavfi -i \"aevalsrc=0.9*sin(2*PI*(200+2000*t)*t)|0.9*random(0)-0.45:s=48000\"";

static void testA52Audio() {
	const auto stereo = generate("a52stereo.vob", VIDEO_320 + " " + STEREO_SWEEP + " -t 3 -c:v mpeg2video" + A52 + " -b:a 384k",
	                             "vob");
	checkAudioMatchesFfmpeg(stereo, "a52stereo.pcm", 2, MIN_A52_SNR_DB);
	const auto p = decode(stereo);
	CHECK(p->frames == 75);
	CHECK_RANGE(p->audioSeconds(), 2.9, 3.1);
	CHECK_RANGE(p->audio.firstFramePtsMillis() - p->displayPts.front(), -50, 50);

	// Mono, 44.1 kHz, low bit rate: coupling isn't possible, few bits per mantissa
	const auto mono = generate("a52mono.vob", TONE + " -t 3" + A52 + " -b:a 64k", "vob");
	checkAudioMatchesFfmpeg(mono, "a52mono.pcm", 1, MIN_A52_SNR_DB);
	// Stereo at a low bit rate, which uses coupling
	const auto coupled = generate("a52coupled.vob", STEREO_SWEEP + " -t 3" + A52 + " -b:a 96k", "vob");
	checkAudioMatchesFfmpeg(coupled, "a52coupled.pcm", 2, MIN_A52_SNR_DB);
	// 32 kHz, with short blocks from the clicks
	const auto clicks = generate("a52clicks.vob", "-f lavfi -i \"aevalsrc=0.8*lt(mod(t\\,0.25)\\,0.01)*sin(2*PI*3000*t)"
	                                              "|0.5*sin(2*PI*300*t):s=32000\" -t 3" + A52 + " -b:a 256k", "vob");
	checkAudioMatchesFfmpeg(clicks, "a52clicks.pcm", 2, MIN_A52_SNR_DB);
}

// 5.1 is downmixed to stereo like ffmpeg's decoder does it
static void testA52Downmix() {
	const auto clip = generate("a52surround.vob", "-f lavfi -i \"aevalsrc=0.5*sin(2*PI*300*t)|0.5*sin(2*PI*500*t)"
	                                              "|0.5*sin(2*PI*700*t)|0.3*sin(2*PI*50*t)|0.5*sin(2*PI*1100*t)"
	                                              "|0.5*sin(2*PI*1300*t):s=48000:c=5.1\" -t 3" + A52 + " -b:a 448k", "vob");
	checkAudioMatchesFfmpeg(clip, "a52surround.pcm", 2, MIN_A52_SNR_DB, "-downmix stereo");
}

static void testA52Mpeg1System() {
	const auto clip = generate("a52system.mpg", VIDEO_320 + " " + TONE + " -t 2 -c:v mpeg1video" + A52);
	const auto p = decode(clip);
	CHECK(p->finished);
	CHECK(p->frames == 50);
	CHECK_RANGE(p->audioSeconds(), 1.9, 2.1);
}

static void testA52Corrupt() {
	auto data = readFile(generate("a52long.vob", VIDEO_320 + " " + TONE + " -t 20 -c:v mpeg2video -bf 2 -g 12" + A52,
	                              "vob"));
	uint32_t seed = 4321;
	auto random = [&seed] { seed = seed * 1103515245 + 12345; return seed >> 8; };
	for (int i = 0; i < 300; i++) {
		data[data.size() / 10 + random() % (data.size() * 8 / 10)] = static_cast<uint8_t>(random());
	}
	const auto p = decode(writeFile("a52corrupt.vob", data));
	CHECK(p->finished);
	CHECK(!p->threw);
	CHECK(p->diagnostics()[Diagnostic::CorruptAudio] + p->diagnostics()[Diagnostic::AudioBytesSkipped] > 0);
	CHECK(p->audioSeconds() > 15.0);
}

// Relabels A/52 sub-stream from as to in a DVD-style program stream
static void relabelA52(std::vector<uint8_t> &data, const uint8_t from, const uint8_t to) {
	for (size_t i = 0; i + 16 < data.size(); i++) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && data[i + 3] == 0xBD && (data[i + 6] >> 6) == 2) {
			const size_t sub = i + 9 + data[i + 8];
			if (sub < data.size() && data[sub] == from) {
				data[sub] = to;
			}
		}
	}
}

// Frequency of a decoded tone from its zero crossings (first channel of stereo)
static double toneFrequency(const Pipeline &p) {
	int crossings = 0;
	size_t samples = 0;
	AudioSample previous = 0;
	for (size_t i = 0; i < p.audioSamples.size(); i++) {
		if (p.audioChannels == 2 && (i / 32) % 2) {
			continue;
		}
		const AudioSample sample = p.audioSamples[i];
		crossings += (previous < 0) != (sample < 0);
		previous = sample;
		samples++;
	}
	return p.audioFrequency ? crossings / 2.0 / (static_cast<double>(samples) / p.audioFrequency) : 0;
}

// A file holding two programs whose audio is on different sub-streams, like DVD .vob files with an intro: the
// second program's audio takes over when the first's stops
static void testA52StreamChange() {
	auto first = readFile(generate("a52intro.vob", VIDEO_320 + " " + TONE + " -t 4 -c:v mpeg2video" + A52, "vob"));
	relabelA52(first, 0x80, 0x84);
	const auto second = readFile(generate("a52long.vob", ""));
	first.insert(first.end(), second.begin(), second.end());
	const auto p = decode(writeFile("a52programs.vob", first));
	CHECK(p->finished);
	CHECK_RANGE(p->audioSeconds(), 22.5, 24.5);
}

// Of several audio streams, the lowest numbered (usually the main soundtrack) is played
static void testA52LowestStream() {
	// DVD audio is always 48 kHz; the output can't change sample rate
	auto data = readFile(generate("a52tracks48.vob", VIDEO_320 + " -f lavfi -i sine=frequency=440:sample_rate=48000"
	                                                 " -f lavfi -i sine=frequency=1000:sample_rate=48000"
	                                               " -map 0:v -map 1:a -map 2:a -t 4 -c:v mpeg2video" + A52, "vob"));
	// The first track (440 Hz) becomes 0x83, the second (1000 Hz) 0x80
	relabelA52(data, 0x80, 0x83);
	relabelA52(data, 0x81, 0x80);
	const auto p = decode(writeFile("a52tracksrelabelled.vob", data));
	CHECK(p->finished);
	CHECK_RANGE(p->audioSeconds(), 3.5, 4.5);
	CHECK_RANGE(toneFrequency(*p), 950.0, 1050.0);
}

// Seeking in a file of several programs, each with its SCR and PTS starting again
static void testTimelineSeek() {
	const auto clip = readFile(generate("a52long.vob", ""));
	std::vector<uint8_t> data;
	for (int i = 0; i < 3; i++) {
		data.insert(data.end(), clip.begin(), clip.end());
	}
	const auto path = writeFile("programs.vob", data);
	Pipeline p(path);
	p.demux.buildTimeline(p.file);
	CHECK_RANGE(p.demux.timelineDurationMillis(), 58500, 61000);
	for (const int64_t target: {5000, 25000, 45000, 15000}) {
		p.seekToTimeline(target);
		const size_t index = p.timelinePts.size();
		p.runUntil(p.frames + 10);
		CHECK(p.timelinePts.size() > index);
		if (p.timelinePts.size() > index) {
			CHECK_RANGE(p.timelinePts[index], target - 1500, target + 1500);
		}
		CHECK(p.audio.firstFramePtsMillis() >= 0);
	}
}

// ---------------------------------------------------------------- DVD

// A sub-picture unit coded by hand: 4x2 pixels, both fields' line "0 1 2 3"
static void testSubPicture() {
	const std::vector<uint8_t> unit = {
		0x00, 0x1E, // unit size
		0x00, 0x06, // control sequence offset
		// Pixel data: runs of 1 pixel, one nibble each (run << 2 | value): values 0, 1, 2, 3
		0x45, 0x67,
		// Control sequence: delay 0, next = itself, commands
		0x00, 0x00, 0x00, 0x06,
		0x01, // start display
		0x03, 0x32, 0x10, // colours: pixel 3 -> 3, 2 -> 2, 1 -> 1, 0 -> 0
		0x04, 0xFF, 0xF0, // contrast: pixel 0 transparent
		0x05, 0x00, 0x00, 0x03, 0x00, 0x00, 0x01, // area x 0-3, y 0-1
		0x06, 0x00, 0x04, 0x00, 0x04, // both fields from offset 4
		0xFF,
	};
	Dvd::SubPicture picture;
	CHECK(Dvd::decodeSubPicture(unit, picture));
	CHECK(picture.width == 4);
	CHECK(picture.height == 2);
	CHECK((picture.pixels == std::vector<uint8_t>{0, 1, 2, 3, 0, 1, 2, 3}));
	CHECK(picture.contrast[0] == 0 && picture.contrast[3] == 15);
	CHECK(picture.colors[2] == 2);
}

// Builds .IFO files: big-endian values at byte offsets
struct IfoBuilder {
	std::vector<uint8_t> data;

	explicit IfoBuilder(const size_t sectors) : data(sectors * Dvd::SECTOR_SIZE) {
	}

	void u8(const size_t offset, const uint32_t value) { data[offset] = static_cast<uint8_t>(value); }
	void u16(const size_t offset, const uint32_t value) { u8(offset, value >> 8); u8(offset + 1, value); }
	void u32(const size_t offset, const uint32_t value) { u16(offset, value >> 16); u16(offset + 2, value); }
	void bytes(const size_t offset, const std::vector<uint8_t> &values) {
		std::copy(values.begin(), values.end(), data.begin() + static_cast<std::ptrdiff_t>(offset));
	}

	struct CellSpec {
		uint32_t first, last;
		uint8_t still;
	};

	// Writes a program chain at offset: cells (one program each), commands
	void pgc(const size_t at, const std::vector<CellSpec> &cells, const std::vector<std::vector<uint8_t>> &pre,
	         const std::vector<std::vector<uint8_t>> &post) {
		u8(at + 2, cells.size());
		u8(at + 3, cells.size());
		size_t offset = 0xEC;
		u16(at + 0xE4, offset);
		u16(at + offset, pre.size());
		u16(at + offset + 2, post.size());
		size_t command = at + offset + 8;
		for (const auto &c: pre) { bytes(command, c); command += 8; }
		for (const auto &c: post) { bytes(command, c); command += 8; }
		offset = command - at;
		u16(at + 0xE6, offset);
		for (size_t i = 0; i < cells.size(); i++) u8(at + offset + i, i + 1);
		offset += cells.size() + 1;
		u16(at + 0xE8, offset);
		for (size_t i = 0; i < cells.size(); i++) {
			u8(at + offset + i * 24 + 2, cells[i].still);
			u32(at + offset + i * 24 + 8, cells[i].first);
			u32(at + offset + i * 24 + 20, cells[i].last);
		}
		offset += cells.size() * 24;
		u16(at + 0xEA, offset);
		for (size_t i = 0; i < cells.size(); i++) {
			u16(at + offset + i * 4, 1);
			u8(at + offset + i * 4 + 3, i + 1);
		}
	}

	void save(const fs::path &path) const {
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
	}
};

// A disc with First Play commands, a two chapter title and a title menu, written as .IFO files
static void testDvdNavigation() {
	const auto folder = media("dvd") / "VIDEO_TS";
	fs::create_directories(folder);

	IfoBuilder vmg(3);
	vmg.bytes(0, {'D', 'V', 'D', 'V', 'I', 'D', 'E', 'O', '-', 'V', 'M', 'G'});
	vmg.u16(0x3E, 1);
	vmg.u32(0x84, 0x400);
	vmg.u32(0xC4, 1);
	vmg.u32(0xC8, 2);
	vmg.pgc(0x400, {}, {
		        {0x71, 0x00, 0x00, 0x01, 0x00, 0x05, 0x00, 0x00}, // GPRM1 = 5
		        {0x73, 0x00, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00}, // GPRM1 += 3
		        {0x00, 0xA1, 0x00, 0x01, 0x00, 0x08, 0x00, 0x05}, // if GPRM1 == 8 goto 5
		        {0x30, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // exit (skipped)
		        {0x30, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}, // jump to title 1
	        }, {});
	// Title table: one title, 2 chapters, title set 1 title 1
	vmg.u16(0x800, 1);
	vmg.u8(0x808 + 1, 1);
	vmg.u16(0x808 + 2, 2);
	vmg.u8(0x808 + 6, 1);
	vmg.u8(0x808 + 7, 1);
	// Menus: one language unit with a title menu, a still cell
	vmg.u16(0x1000, 1);
	vmg.bytes(0x1008, {'e', 'n'});
	vmg.u32(0x1008 + 4, 16);
	vmg.u16(0x1010, 1);
	vmg.u8(0x1018, 0x82);
	vmg.u32(0x1018 + 4, 16);
	vmg.pgc(0x1020, {{0, 0, 255}}, {}, {});
	vmg.save(folder / "VIDEO_TS.IFO");

	IfoBuilder vts(3);
	vts.bytes(0, {'D', 'V', 'D', 'V', 'I', 'D', 'E', 'O', '-', 'V', 'T', 'S'});
	vts.u32(0xC8, 1);
	vts.u32(0xCC, 2);
	// Chapters of title 1: programs 1 and 2 of PGC 1
	vts.u16(0x800, 1);
	vts.u32(0x804, 19);
	vts.u32(0x808, 12);
	vts.u16(0x80C, 1);
	vts.u16(0x80E, 1);
	vts.u16(0x810, 1);
	vts.u16(0x812, 2);
	vts.u16(0x1000, 1);
	vts.u8(0x1008, 0x81);
	vts.u32(0x1008 + 4, 16);
	vts.pgc(0x1010, {{0, 9, 0}, {10, 19, 0}}, {}, {
		        {0x30, 0x06, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00}, // to the video manager's title menu
	        });
	vts.save(folder / "VTS_01_0.IFO");

	CHECK(Dvd::Disc::isDvd(media("dvd").string()));
	Dvd::Disc disc(media("dvd").string());
	Dvd::Navigator nav(disc);
	nav.start();
	CHECK(nav.domain() == Dvd::Domain::Title);
	CHECK(nav.gprm(1) == 8);
	CHECK(nav.cellNumber() == 1);
	CHECK(nav.sprm(4) == 1);
	nav.cellFinished();
	CHECK(nav.cellNumber() == 2);
	CHECK(nav.sprm(7) == 2);

	// The menu, then resume
	nav.menuCall(Dvd::TITLE_MENU);
	CHECK(nav.domain() == Dvd::Domain::VideoManagerMenu);
	CHECK(nav.cell() && nav.cell()->stillTime == 255);
	nav.buttonActivated({0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10});
	CHECK(nav.domain() == Dvd::Domain::Title);
	CHECK(nav.cellNumber() == 2);

	// The end of the title runs its post command
	nav.cellFinished();
	CHECK(nav.domain() == Dvd::Domain::VideoManagerMenu);
	// A menu button jumps to the title's second chapter
	nav.buttonActivated({0x30, 0x05, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00});
	CHECK(nav.domain() == Dvd::Domain::Title);
	CHECK(nav.cellNumber() == 2);
	nav.previousChapter();
	CHECK(nav.cellNumber() == 1);
}

// A disc whose First Play shows two warnings before its title menu. Skipping a program chain follows its post
// command, past its still cells and program chain still.
static void testDvdSkipProgramChain() {
	const auto folder = media("dvdintro") / "VIDEO_TS";
	fs::create_directories(folder);

	IfoBuilder vmg(3);
	vmg.bytes(0, {'D', 'V', 'D', 'V', 'I', 'D', 'E', 'O', '-', 'V', 'M', 'G'});
	vmg.u32(0x84, 0x400);
	vmg.u32(0xC8, 2);
	vmg.pgc(0x400, {}, {
		        {0x30, 0x06, 0x00, 0x02, 0x00, 0xC0, 0x00, 0x00}, // to video manager PGC 2
	        }, {});
	// Menus: the title menu (PGC 1), then two warnings only reached by links
	vmg.u16(0x1000, 1);
	vmg.bytes(0x1008, {'e', 'n'});
	vmg.u32(0x1008 + 4, 16);
	vmg.u16(0x1010, 3);
	vmg.u8(0x1018, 0x82);
	vmg.u32(0x1018 + 4, 0x30);
	vmg.u32(0x1020 + 4, 0x200);
	vmg.u32(0x1028 + 4, 0x400);
	vmg.pgc(0x1010 + 0x30, {{0, 0, 255}}, {}, {});
	vmg.pgc(0x1010 + 0x200, {{0, 0, 10}, {1, 1, 255}}, {}, {
		        {0x30, 0x06, 0x00, 0x03, 0x00, 0xC0, 0x00, 0x00}, // to video manager PGC 3
	        });
	vmg.u8(0x1010 + 0x400 + 0xA2, 255); // program chain still
	vmg.pgc(0x1010 + 0x400, {{2, 2, 0}}, {}, {
		        {0x30, 0x06, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00}, // to the title menu
	        });
	vmg.save(folder / "VIDEO_TS.IFO");

	Dvd::Disc disc(media("dvdintro").string());
	Dvd::Navigator nav(disc);
	nav.start();
	CHECK(nav.domain() == Dvd::Domain::VideoManagerMenu);
	CHECK(nav.cell() && nav.cell()->firstSector == 0);
	CHECK(!nav.inMenuEntryPgc());
	auto jumps = nav.jumpCount();
	nav.skipProgramChain();
	CHECK(nav.jumpCount() != jumps);
	CHECK(nav.cell() && nav.cell()->firstSector == 2);
	CHECK(!nav.inMenuEntryPgc());
	nav.skipProgramChain();
	CHECK(nav.domain() == Dvd::Domain::VideoManagerMenu);
	CHECK(nav.inMenuEntryPgc());
	CHECK(nav.cell() && nav.cell()->stillTime == 255);
	// The menu has no post commands or next program chain: skipping it ends playback, as its end would
	nav.skipProgramChain();
	CHECK(nav.stopped());
}

// Program chains before the first menu: short ones without buttons are skipped, until one with buttons or a long
// one. Menu entries aren't skipped, and each program chain is judged once.
static void testDvdIntroSkipper() {
	Dvd::Pgc warning, logo, menu, feature, menuEntry;
	warning.playbackTimeMs = 15000;
	logo.playbackTimeMs = 0;
	menuEntry.playbackTimeMs = 8000;
	menu.playbackTimeMs = 30000;
	feature.playbackTimeMs = Dvd::IntroSkipper::MAX_INTRO_MS;

	Dvd::IntroSkipper skipper(true);
	CHECK(skipper.shouldSkip(&warning, false, false));
	CHECK(!skipper.shouldSkip(&warning, false, false)); // judged already
	CHECK(skipper.shouldSkip(&logo, false, false));
	CHECK(!skipper.shouldSkip(&menuEntry, true, false)); // an opening before the buttons
	CHECK(skipper.active());
	CHECK(!skipper.shouldSkip(&menu, false, true));
	CHECK(!skipper.active());
	CHECK(!skipper.shouldSkip(&logo, false, false)); // the first menu has been reached

	Dvd::IntroSkipper noMenu(true);
	CHECK(noMenu.shouldSkip(&warning, false, false));
	CHECK(!noMenu.shouldSkip(&feature, false, false));
	CHECK(!noMenu.active());

	Dvd::IntroSkipper disabled(false);
	CHECK(!disabled.shouldSkip(&warning, false, false));
}

// A stream of one picture, like a DVD menu still, must decode after reset() even if the decoder had seen no
// sequence before: the reset's wait for a keyframe used to end only when a second picture started
static void testSinglePictureAfterReset() {
	const auto clip = generate("still.vob", "-f lavfi -i testsrc2=size=720x576:rate=25 -frames:v 1 -c:v mpeg2video", "vob");
	for (const bool reset: {false, true}) {
		MStream::FileStreamRead file(clip.c_str());
		Mpeg1::MpegProgramStream demux;
		Mpeg1VideoDecoder video;
		demux.setVideoDecoder(&video);
		int frames = 0;
		video.setFrameCallback([&](const Frame &) { frames++; });
		if (reset) {
			demux.reset();
			video.reset();
		}
		for (int i = 0; i < 100000 && demux.nextPacket(file) != Mpeg1::DemuxResult::EndOfStream; i++) {
		}
		CHECK(frames == 1);
	}
}

static void testA52Seek() {
	const auto clip = generate("a52long.vob", VIDEO_320 + " " + TONE + " -t 20 -c:v mpeg2video -bf 2 -g 12" + A52, "vob");
	Pipeline p(clip);
	p.runUntil(50);
	p.seekTo(12000);
	const size_t index = p.displayPts.size();
	p.runUntil(p.frames + 10);
	CHECK(p.displayPts.size() > index);
	CHECK_RANGE(p.audio.firstFramePtsMillis(), 10500, 13500);
	p.runUntil();
	CHECK(p.finished);
}

// ffmpeg's DVD LPCM encoder, 16 or 24 bits
static const std::string LPCM16 = " -c:a pcm_dvd -sample_fmt s16 -ar 48000";
static const std::string LPCM24 = " -c:a pcm_dvd -sample_fmt s32 -ar 48000";

static void testLpcmAudio() {
	const auto stereo16 = generate("lpcm16.vob", VIDEO_320 + " " + STEREO_SWEEP + " -t 3 -c:v mpeg2video" + LPCM16, "vob");
	checkAudioMatchesFfmpeg(stereo16, "lpcm16.pcm", 2);
	const auto stereo24 = generate("lpcm24.vob", STEREO_SWEEP + " -t 3" + LPCM24, "vob");
	checkAudioMatchesFfmpeg(stereo24, "lpcm24.pcm", 2);
	const auto mono24 = generate("lpcm24mono.vob", TONE + " -t 3" + LPCM24, "vob");
	checkAudioMatchesFfmpeg(mono24, "lpcm24mono.pcm", 1);

	const auto p = decode(generate("lpcm96k.vob", STEREO_SWEEP + " -t 3 -c:a pcm_dvd -sample_fmt s32 -ar 96000", "vob"));
	CHECK(p->finished);
	CHECK(p->audioFrequency == 96000);
	CHECK_RANGE(p->audioSeconds(), 2.9, 3.1);
}

// ffmpeg can't encode 20-bit LPCM, so this builds a DVD-style program stream of it: a sine on the left (and mono)
// channel, a quieter one on the right, in packets of 5 frames of 80 samples
static fs::path writeLpcm20(const std::string &name, const uint32_t channels) {
	constexpr uint32_t FRAME_SAMPLES = 80, FRAMES_PER_PACKET = 5, PACKETS = 180;
	constexpr double TWO_PI = 6.283185307179586;
	std::vector<uint8_t> data;
	uint32_t n = 0;
	for (uint32_t packet = 0; packet < PACKETS; packet++) {
		// MPEG-2 pack header, SCR 0
		data.insert(data.end(), {0, 0, 1, 0xBA, 0x44, 0, 4, 0, 4, 1, 0x01, 0x89, 0xC3, 0xF8});
		std::vector<uint8_t> payload;
		for (uint32_t pair = 0; pair < FRAMES_PER_PACKET * FRAME_SAMPLES / 2; pair++) {
			// 2 samples: the top 16 bits of each value, then their low nibbles
			std::vector<int32_t> values;
			for (uint32_t i = 0; i < 2; i++, n++) {
				values.push_back(static_cast<int32_t>(std::lround(0.8 * 524287 * std::sin(n * TWO_PI * 440 / 48000))));
				if (channels == 2) {
					values.push_back(static_cast<int32_t>(std::lround(0.3 * 524287 * std::sin(n * TWO_PI * 1500 / 48000))));
				}
			}
			for (const int32_t value: values) {
				payload.push_back(static_cast<uint8_t>(value >> 12));
				payload.push_back(static_cast<uint8_t>(value >> 4));
			}
			for (size_t i = 0; i < values.size(); i += 2) {
				payload.push_back(static_cast<uint8_t>((values[i] & 15) << 4 | (values[i + 1] & 15)));
			}
		}
		const uint64_t pts = 90000 + static_cast<uint64_t>(packet) * FRAMES_PER_PACKET * FRAME_SAMPLES * 90000 / 48000;
		const std::vector<uint8_t> header = {
			0x81, 0x80, 5,
			static_cast<uint8_t>(0x21 | (pts >> 29 & 0x0E)), static_cast<uint8_t>(pts >> 22),
			static_cast<uint8_t>(pts >> 14 | 1), static_cast<uint8_t>(pts >> 7), static_cast<uint8_t>(pts << 1 | 1),
			// Sub-stream 0xA0, frames, first access unit pointer, frame number, 20 bits / 48 kHz / channels,
			// dynamic range
			0xA0, FRAMES_PER_PACKET, 0, 4, 0, static_cast<uint8_t>(0x40 | (channels - 1)), 0x80};
		const size_t length = header.size() + payload.size();
		data.insert(data.end(), {0, 0, 1, 0xBD, static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)});
		data.insert(data.end(), header.begin(), header.end());
		data.insert(data.end(), payload.begin(), payload.end());
	}
	return writeFile(name, data);
}

static void testLpcm20Bit() {
	checkAudioMatchesFfmpeg(writeLpcm20("lpcm20.vob", 2), "lpcm20.pcm", 2);
	checkAudioMatchesFfmpeg(writeLpcm20("lpcm20mono.vob", 1), "lpcm20mono.pcm", 1);
}

// 5.1 is downmixed to stereo like ffmpeg's resampler does it
static void testLpcmDownmix() {
	const auto clip = generate("lpcmsurround.vob", "-f lavfi -i \"aevalsrc=0.5*sin(2*PI*300*t)|0.5*sin(2*PI*500*t)"
	                                               "|0.5*sin(2*PI*700*t)|0.3*sin(2*PI*50*t)|0.5*sin(2*PI*1100*t)"
	                                               "|0.5*sin(2*PI*1300*t):s=48000:c=5.1\" -t 3" + LPCM16, "vob");
	checkAudioMatchesFfmpeg(clip, "lpcmsurround.pcm", 2, MIN_SNR_DB, "", "-ac 2");
}

// After a seek, decoding starts at the first frame of a packet, not mid-sample
static void testLpcmSeek() {
	const auto clip = generate("lpcmlong.vob", VIDEO_320 + " " + TONE + " -t 20 -c:v mpeg2video -bf 2 -g 12" + LPCM16,
	                           "vob");
	Pipeline p(clip);
	p.runUntil(50);
	p.seekTo(12000);
	const size_t index = p.displayPts.size();
	p.runUntil(p.frames + 10);
	CHECK(p.displayPts.size() > index);
	CHECK_RANGE(p.audio.firstFramePtsMillis(), 10500, 13500);
	p.runUntil();
	CHECK(p.finished);
	CHECK_RANGE(toneFrequency(p), 430.0, 450.0);
}

// A DVD chooses its audio stream: A/52 (sub-stream 0x80) or LPCM (0xA0) from the same file
static void testLpcmChosenStream() {
	const auto clip = generate("a52lpcm.vob", VIDEO_320 + " -f lavfi -i sine=frequency=440:sample_rate=48000"
	                                          " -f lavfi -i sine=frequency=1000:sample_rate=48000"
	                                          " -map 0:v -map 1:a -map 2:a -t 4 -c:v mpeg2video -c:a:0 ac3"
	                                          " -c:a:1 pcm_dvd -sample_fmt:a:1 s16", "vob");
	for (const auto &[stream, frequency]: {std::pair{0x80u, 440.0}, std::pair{0xA0u, 1000.0}}) {
		Pipeline p(clip);
		p.demux.setAudioStream(stream);
		p.runUntil();
		CHECK(p.finished);
		CHECK_RANGE(p.audioSeconds(), 3.5, 4.5);
		CHECK_RANGE(toneFrequency(p), frequency * 0.9, frequency * 1.1);
	}
}
#endif

int main(int argc, char *argv[]) {
	g_mediaDir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "md_mpeg_test_media";
	fs::create_directories(g_mediaDir);

	if (!run("ffmpeg -version")) {
		std::cout << "ffmpeg not found, skipping" << std::endl;
		return 77;
	}

	const std::vector<std::pair<std::string, std::function<void()>>> tests = {
		{"clean", testClean},
		{"b-frames", testBFrames},
		{"odd size", testOddSize},
		{"audio only", testAudioOnly},
		{"video only", testVideoOnly},
		{"extension and user data", testExtensionAndUserData},
		{"audio accuracy", testAudioAccuracy},
		{"mp2 11172-4 compliance", testMp2Compliance},
		{"idct accuracy", testIdctAccuracy},
		{"motion vector range", testMotionVectorRange},
		{"unsupported audio", testUnsupportedAudio},
		{"size change", testSizeChange},
		{"truncated", testTruncated},
		{"corrupt", testCorrupt},
		{"concatenated", testConcatenated},
		{"seek", testSeek},
		{"audio output stall", testAudioStall},
#if MD_MPEG_ENABLE_MPEG2
		{"mpeg-1 matches ffmpeg", testMpeg1MatchesFfmpeg},
		{"mpeg-2 progressive", testMpeg2Progressive},
		{"mpeg-2 interlaced", testMpeg2Interlaced},
		{"interlace flags", testInterlaceFlags},
		{"bob deinterlace", testBobDeinterlace},
		{"yadif deinterlace", testYadifDeinterlace},
		{"mpeg-2 coding options", testMpeg2CodingOptions},
		{"mpeg-2 in mixed system layers", testMpeg2MixedLayers},
		{"mpeg-2 dual prime", testMpeg2DualPrime},
		{"mpeg-2 seek", testMpeg2Seek},
		{"mpeg-2 starting mid-stream", testMpeg2StartMidStream},
		{"a/52 audio", testA52Audio},
		{"a/52 downmix", testA52Downmix},
		{"a/52 in an mpeg-1 system stream", testA52Mpeg1System},
		{"a/52 corrupt", testA52Corrupt},
		{"a/52 seek", testA52Seek},
		{"a/52 stream change", testA52StreamChange},
		{"a/52 lowest numbered stream", testA52LowestStream},
		{"lpcm audio", testLpcmAudio},
		{"lpcm 20-bit", testLpcm20Bit},
		{"lpcm downmix", testLpcmDownmix},
		{"lpcm seek", testLpcmSeek},
		{"lpcm chosen stream", testLpcmChosenStream},
		{"seek across programs", testTimelineSeek},
		{"dvd sub-picture", testSubPicture},
		{"single picture after reset", testSinglePictureAfterReset},
		{"dvd navigation", testDvdNavigation},
		{"dvd skip program chain", testDvdSkipProgramChain},
		{"dvd intro skipping", testDvdIntroSkipper},
#endif
	};
	for (const auto &[name, test] : tests) {
		g_currentTest = name;
		const int failuresBefore = g_failures;
		std::cout << name << std::endl;
		test();
		std::cout << "  " << (g_failures == failuresBefore ? "ok" : "FAILED") << std::endl;
	}

	std::cout << (g_failures ? "FAILED: " + std::to_string(g_failures) + " check(s)" : "All tests passed") << std::endl;
	return g_failures ? 1 : 0;
}
