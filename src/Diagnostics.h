#pragma once

// Reports of corrupt or unsupported input from the demuxer and decoders. Each keeps its own Diagnostics, so
// nothing is shared between threads or cores: counters that are always on and cost an add, for tests and
// stats, and an optional callback with a message for each report, which does nothing unless set. The desktop
// player sets one that prints them.
//
// MD_MPEG_DIAGNOSTICS (default on, off on the Pico) compiles in the messages and the callback. Off, the reports
// only count, and their message strings and the code building them aren't compiled at all.

#include <cstddef>
#include <cstdint>

#ifndef MD_MPEG_DIAGNOSTICS
#define MD_MPEG_DIAGNOSTICS 1
#endif

enum class Diagnostic : uint8_t {
	// Demuxer: bytes skipped to find the next start code (counts bytes)
	SyncBytesSkipped,
	// Demuxer: a packet with a corrupt or impossible header, dropped
	CorruptPacket,
	// Video: a sequence or picture header with forbidden values
	InvalidHeader,
	// Video: a picture abandoned at an error; decoding resumes at the next picture
	CorruptVideo,
	// Video: a motion vector pointing outside the reference picture; the macroblock isn't predicted
	BadMotionVector,
	// Audio: bytes skipped to find the next frame (counts bytes)
	AudioBytesSkipped,
	// Audio: a frame skipped or muted
	CorruptAudio,
	// Audio: a format the decoder doesn't handle, so audio is disabled
	UnsupportedAudio,
	Count
};

constexpr size_t DIAGNOSTIC_COUNT = static_cast<size_t>(Diagnostic::Count);

// Short name for a diagnostic, e.g. for stats
constexpr const char *diagnosticName(const Diagnostic diagnostic) {
	switch (diagnostic) {
		case Diagnostic::SyncBytesSkipped: return "sync bytes skipped";
		case Diagnostic::CorruptPacket: return "corrupt packets";
		case Diagnostic::InvalidHeader: return "invalid headers";
		case Diagnostic::CorruptVideo: return "corrupt pictures";
		case Diagnostic::BadMotionVector: return "bad motion vectors";
		case Diagnostic::AudioBytesSkipped: return "audio bytes skipped";
		case Diagnostic::CorruptAudio: return "corrupt audio frames";
		case Diagnostic::UnsupportedAudio: return "unsupported audio";
		default: return "?";
	}
}

// Called for each report. amount is what the counter went up by (bytes for the skipped bytes diagnostics).
using DiagnosticCallback = void (*)(void *context, Diagnostic diagnostic, uint32_t amount, const char *message);

class Diagnostics {
public:
	[[nodiscard]] uint32_t operator[](const Diagnostic diagnostic) const {
		return m_counts[static_cast<size_t>(diagnostic)];
	}

	// All the counters added up
	[[nodiscard]] uint32_t total() const {
		uint32_t sum = 0;
		for (const uint32_t count : m_counts) {
			sum += count;
		}
		return sum;
	}

	void clear() {
		for (uint32_t &count : m_counts) {
			count = 0;
		}
	}

	// Adds another component's counters to these, e.g. to total the audio decoders
	void add(const Diagnostics &other) {
		for (size_t i = 0; i < DIAGNOSTIC_COUNT; i++) {
			m_counts[i] += other.m_counts[i];
		}
	}

	void count(const Diagnostic diagnostic, const uint32_t amount = 1) {
		m_counts[static_cast<size_t>(diagnostic)] += amount;
	}

#if MD_MPEG_DIAGNOSTICS
	void setCallback(const DiagnosticCallback callback, void *const context) {
		m_callback = callback;
		m_context = context;
	}

	void report(const Diagnostic diagnostic, const uint32_t amount, const char *const message) {
		count(diagnostic, amount);
		if (m_callback) {
			m_callback(m_context, diagnostic, amount, message);
		}
	}
#else
	// Nothing to call without the messages
	void setCallback(DiagnosticCallback, void *) {}
#endif

private:
	uint32_t m_counts[DIAGNOSTIC_COUNT] = {};
#if MD_MPEG_DIAGNOSTICS
	DiagnosticCallback m_callback = nullptr;
	void *m_context = nullptr;
#endif
};

// Reports to a Diagnostics. Without MD_MPEG_DIAGNOSTICS the message expression isn't evaluated or compiled in.
#if MD_MPEG_DIAGNOSTICS
#define MD_MPEG_REPORT(diagnostics, diagnostic, amount, message) (diagnostics).report((diagnostic), (amount), (message))
#else
#define MD_MPEG_REPORT(diagnostics, diagnostic, amount, message) (diagnostics).count((diagnostic), (amount))
#endif
