#pragma once

// Prints the demuxer's and decoders' diagnostic messages, for the desktop players

#include <iostream>

#include "Diagnostics.h"
#include "mpeg1/AudioDecoders.h"
#include "mpeg1/Mpeg1VideoDecoder.h"
#include "mpeg1/MpegProgramStream.h"

inline void printDiagnostic(void *, const Diagnostic diagnostic, const uint32_t amount, const char *const message) {
	std::cout << message;
	if (diagnostic == Diagnostic::SyncBytesSkipped || diagnostic == Diagnostic::AudioBytesSkipped) {
		std::cout << " (" << amount << " bytes)";
	}
	std::cout << std::endl;
}

inline void printDiagnostics(Mpeg1::MpegProgramStream &demux, Mpeg1VideoDecoder &video, AudioDecoders &audio) {
	demux.diagnostics().setCallback(printDiagnostic, nullptr);
	video.diagnostics().setCallback(printDiagnostic, nullptr);
	audio.setDiagnosticCallback(printDiagnostic, nullptr);
}
