// Decoder tests. Test clips are generated with ffmpeg (lavfi test sources), so no media is stored in
// the repository. Exits with 77 (skipped) if ffmpeg isn't available.
//
// Usage: md_mpeg_tests <directory for generated media>

#include <algorithm>
#include <climits>
#include <cmath>
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

#include "DisplayTimestamper.h"
#include "stream/FileStream.h"
#include "mpeg1/MpegProgramStream.h"
#include "mpeg1/Mpeg1VideoDecoder.h"
#include "mpeg1/Mpeg1AudioDecoder.h"

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

static const std::string VIDEO_320 = "-f lavfi -i testsrc=size=320x240:rate=25";
static const std::string TONE = "-f lavfi -i sine=frequency=440:sample_rate=44100";

// ---------------------------------------------------------------- decoding harness

struct Pipeline {
	MStream::FileStreamRead file;
	Mpeg1::MpegProgramStream demux;
	Mpeg1VideoDecoder video;
	Mpeg1AudioDecoder audio;
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

	bool finished = false;
	bool threw = false;

	explicit Pipeline(const fs::path &path) : file(path.c_str()) {
		demux.setVideoDecoder(&video);
		demux.setAudioDecoder(&audio);
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
			streamPts.push_back(frame.m_ptsMillis);
			width = video.m_width;
			height = video.m_height;
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

	// Same sequence the player uses
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

static void checkAudioMatchesFfmpeg(const fs::path &clip, const std::string &referenceName, const uint32_t channels) {
	const auto reference = media(referenceName);
	if (!fs::exists(reference)) {
		run("ffmpeg -nostdin -y -i \"" + clip.string() + "\" -vn -f s16le -acodec pcm_s16le \"" + reference.string() + "\"");
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
		CHECK(snrDb >= MIN_SNR_DB);
	}
}

static void testAudioAccuracy() {
	checkAudioMatchesFfmpeg(generate("tone.mpg", TONE + " -t 3 -c:a mp2"), "tone.pcm", 1);
	// Loud and wide-band, stereo: harder on the synthesis filter's range and precision
	const auto stereo = generate("stereo.mpg", "-f lavfi -i \"aevalsrc=0.9*sin(2*PI*(200+2000*t)*t)|0.9*random(0)-0.45:s=48000\""
	                                           " -t 3 -c:a mp2 -b:a 384k");
	checkAudioMatchesFfmpeg(stereo, "stereo.pcm", 2);
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
	std::ostringstream log;
	auto *const coutBuffer = std::cout.rdbuf(log.rdbuf());
	for (const auto byte : edited) {
		video->writeByte(byte);
	}
	video->flush();
	std::cout.rdbuf(coutBuffer);
	CHECK(frames == 50);
	CHECK(log.str().empty());
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
		const uint32_t w = video->m_width, h = video->m_height;
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
// pictures, so this is a PSNR floor rather than exact equality.
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
		{"unsupported audio", testUnsupportedAudio},
		{"truncated", testTruncated},
		{"corrupt", testCorrupt},
		{"concatenated", testConcatenated},
		{"seek", testSeek},
#if MD_MPEG_ENABLE_MPEG2
		{"mpeg-1 matches ffmpeg", testMpeg1MatchesFfmpeg},
		{"mpeg-2 progressive", testMpeg2Progressive},
		{"mpeg-2 interlaced", testMpeg2Interlaced},
		{"mpeg-2 coding options", testMpeg2CodingOptions},
		{"mpeg-2 in mixed system layers", testMpeg2MixedLayers},
		{"mpeg-2 dual prime", testMpeg2DualPrime},
		{"mpeg-2 seek", testMpeg2Seek},
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
