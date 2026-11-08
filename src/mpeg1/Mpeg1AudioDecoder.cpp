// Portions adapted from PL_MPEG (MIT, Copyright (c) 2019 Dominic Szablewski), see THIRD_PARTY_NOTICES.md

#include "Mpeg1AudioDecoder.h"

#include <cstring>

// 4-byte header and the 2-byte CRC that may follow it
#define AUDIO_HEADER_SIZE 6

enum {
    DECODE_HEADER,
    DECODE_FRAME
} AUDIO_STATE;

void Mpeg1AudioDecoder::setFrameCallback(const std::function<void(const uint32_t frequency, const uint32_t channels,
                                                                  AudioSampleBuffer *soundBuffer)>
    &frameCallback) {
    m_frameCallback = frameCallback;
}

int32_t Mpeg1AudioDecoder::getPts(const int32_t frameTimeMillis) {
    return m_stream.getPts(frameTimeMillis);
}

void Mpeg1AudioDecoder::setPts(const int32_t ptsMillis) {
    m_stream.setPts(ptsMillis);
}

void Mpeg1AudioDecoder::writeByte(const uint8_t data) {
    m_stream.writeByte(data);
}

[[nodiscard]] bool Mpeg1AudioDecoder::isFull() {
    return m_stream.isFull();
}

void Mpeg1AudioDecoder::executeCallbacks() {
    m_stream.executeCallbacks();
}

void Mpeg1AudioDecoder::flush() {
    m_stream.flush();
}

static constexpr uint32_t AUDIO_MPEG_2_5 = 0x0;
static constexpr uint32_t AUDIO_MPEG_2 = 0x2;
static constexpr uint32_t AUDIO_MPEG_1 = 0x3;

static constexpr uint32_t AUDIO_LAYER_III = 0x1;
static constexpr uint32_t AUDIO_LAYER_II = 0x2;
static constexpr uint32_t AUDIO_LAYER_I = 0x3;

static constexpr uint32_t AUDIO_MODE_STEREO = 0x0;
static constexpr uint32_t AUDIO_MODE_JOINT_STEREO = 0x1;
static constexpr uint32_t AUDIO_MODE_DUAL_CHANNEL = 0x2;
static constexpr uint32_t AUDIO_MODE_MONO = 0x3;

static constexpr std::array<const std::string, 4> ChannelModeDesc = {"STEREO", "JOINT_STEREO", "DUAL_CHANNEL", "MONO"};

static constexpr uint32_t AUDIO_SAMPLE_RATE[] = {
    44100, 48000, 32000, 0, // MPEG-1
};

static const uint32_t AUDIO_BIT_RATE[] = {
    0,
    32000, 48000, 56000, 64000, 80000, 96000, 112000, 128000,
    160000, 192000, 224000, 256000, 320000, 384000, 416000, 448000, // MPEG-1 V1,L1
};

// 2.0 * 2^(-i/3) in Q24, for scale factors 0..2. Each further 3 halves it.
static constexpr int AUDIO_SCALEFACTOR_BASE[] = {
    0x02000000, 0x01965FEA, 0x01428A30
};

// Synthesis window D[] in Q15
static constexpr float AUDIO_SYNTHESIS_WINDOW[] = {
    0.0, -0.5, -0.5, -0.5, -0.5, -0.5,
    -0.5, -1.0, -1.0, -1.0, -1.0, -1.5,
    -1.5, -2.0, -2.0, -2.5, -2.5, -3.0,
    -3.5, -3.5, -4.0, -4.5, -5.0, -5.5,
    -6.5, -7.0, -8.0, -8.5, -9.5, -10.5,
    -12.0, -13.0, -14.5, -15.5, -17.5, -19.0,
    -20.5, -22.5, -24.5, -26.5, -29.0, -31.5,
    -34.0, -36.5, -39.5, -42.5, -45.5, -48.5,
    -52.0, -55.5, -58.5, -62.5, -66.0, -69.5,
    -73.5, -77.0, -80.5, -84.5, -88.0, -91.5,
    -95.0, -98.0, -101.0, -104.0, 106.5, 109.0,
    111.0, 112.5, 113.5, 114.0, 114.0, 113.5,
    112.0, 110.5, 107.5, 104.0, 100.0, 94.5,
    88.5, 81.5, 73.0, 63.5, 53.0, 41.5,
    28.5, 14.5, -1.0, -18.0, -36.0, -55.5,
    -76.5, -98.5, -122.0, -147.0, -173.5, -200.5,
    -229.5, -259.5, -290.5, -322.5, -355.5, -389.5,
    -424.0, -459.5, -495.5, -532.0, -568.5, -605.0,
    -641.5, -678.0, -714.0, -749.0, -783.5, -817.0,
    -849.0, -879.5, -908.5, -935.0, -959.5, -981.0,
    -1000.5, -1016.0, -1028.5, -1037.5, -1042.5, -1043.5,
    -1040.0, -1031.5, 1018.5, 1000.0, 976.0, 946.5,
    911.0, 869.5, 822.0, 767.5, 707.0, 640.0,
    565.5, 485.0, 397.0, 302.5, 201.0, 92.5,
    -22.5, -144.0, -272.5, -407.0, -547.5, -694.0,
    -846.0, -1003.0, -1165.0, -1331.5, -1502.0, -1675.5,
    -1852.5, -2031.5, -2212.5, -2394.0, -2576.5, -2758.5,
    -2939.5, -3118.5, -3294.5, -3467.5, -3635.5, -3798.5,
    -3955.0, -4104.5, -4245.5, -4377.5, -4499.0, -4609.5,
    -4708.0, -4792.5, -4863.5, -4919.0, -4958.0, -4979.5,
    -4983.0, -4967.5, -4931.5, -4875.0, -4796.0, -4694.5,
    -4569.5, -4420.0, -4246.0, -4046.0, -3820.0, -3567.0,
    3287.0, 2979.5, 2644.0, 2280.5, 1888.0, 1467.5,
    1018.5, 541.0, 35.0, -499.0, -1061.0, -1650.0,
    -2266.5, -2909.0, -3577.0, -4270.0, -4987.5, -5727.5,
    -6490.0, -7274.0, -8077.5, -8899.5, -9739.0, -10594.5,
    -11464.5, -12347.0, -13241.0, -14144.5, -15056.0, -15973.5,
    -16895.5, -17820.0, -18744.5, -19668.0, -20588.0, -21503.0,
    -22410.5, -23308.5, -24195.0, -25068.5, -25926.5, -26767.0,
    -27589.0, -28389.0, -29166.5, -29919.0, -30644.5, -31342.0,
    -32009.5, -32645.0, -33247.0, -33814.5, -34346.0, -34839.5,
    -35295.0, -35710.0, -36084.5, -36417.5, -36707.5, -36954.0,
    -37156.5, -37315.0, -37428.0, -37496.0, 37519.0, 37496.0,
    37428.0, 37315.0, 37156.5, 36954.0, 36707.5, 36417.5,
    36084.5, 35710.0, 35295.0, 34839.5, 34346.0, 33814.5,
    33247.0, 32645.0, 32009.5, 31342.0, 30644.5, 29919.0,
    29166.5, 28389.0, 27589.0, 26767.0, 25926.5, 25068.5,
    24195.0, 23308.5, 22410.5, 21503.0, 20588.0, 19668.0,
    18744.5, 17820.0, 16895.5, 15973.5, 15056.0, 14144.5,
    13241.0, 12347.0, 11464.5, 10594.5, 9739.0, 8899.5,
    8077.5, 7274.0, 6490.0, 5727.5, 4987.5, 4270.0,
    3577.0, 2909.0, 2266.5, 1650.0, 1061.0, 499.0,
    -35.0, -541.0, -1018.5, -1467.5, -1888.0, -2280.5,
    -2644.0, -2979.5, 3287.0, 3567.0, 3820.0, 4046.0,
    4246.0, 4420.0, 4569.5, 4694.5, 4796.0, 4875.0,
    4931.5, 4967.5, 4983.0, 4979.5, 4958.0, 4919.0,
    4863.5, 4792.5, 4708.0, 4609.5, 4499.0, 4377.5,
    4245.5, 4104.5, 3955.0, 3798.5, 3635.5, 3467.5,
    3294.5, 3118.5, 2939.5, 2758.5, 2576.5, 2394.0,
    2212.5, 2031.5, 1852.5, 1675.5, 1502.0, 1331.5,
    1165.0, 1003.0, 846.0, 694.0, 547.5, 407.0,
    272.5, 144.0, 22.5, -92.5, -201.0, -302.5,
    -397.0, -485.0, -565.5, -640.0, -707.0, -767.5,
    -822.0, -869.5, -911.0, -946.5, -976.0, -1000.0,
    1018.5, 1031.5, 1040.0, 1043.5, 1042.5, 1037.5,
    1028.5, 1016.0, 1000.5, 981.0, 959.5, 935.0,
    908.5, 879.5, 849.0, 817.0, 783.5, 749.0,
    714.0, 678.0, 641.5, 605.0, 568.5, 532.0,
    495.5, 459.5, 424.0, 389.5, 355.5, 322.5,
    290.5, 259.5, 229.5, 200.5, 173.5, 147.0,
    122.0, 98.5, 76.5, 55.5, 36.0, 18.0,
    1.0, -14.5, -28.5, -41.5, -53.0, -63.5,
    -73.0, -81.5, -88.5, -94.5, -100.0, -104.0,
    -107.5, -110.5, -112.0, -113.5, -114.0, -114.0,
    -113.5, -112.5, -111.0, -109.0, 106.5, 104.0,
    101.0, 98.0, 95.0, 91.5, 88.0, 84.5,
    80.5, 77.0, 73.5, 69.5, 66.0, 62.5,
    58.5, 55.5, 52.0, 48.5, 45.5, 42.5,
    39.5, 36.5, 34.0, 31.5, 29.0, 26.5,
    24.5, 22.5, 20.5, 19.0, 17.5, 15.5,
    14.5, 13.0, 12.0, 10.5, 9.5, 8.5,
    8.0, 7.0, 6.5, 5.5, 5.0, 4.5,
    4.0, 3.5, 3.5, 3.0, 2.5, 2.5,
    2.0, 2.0, 1.5, 1.5, 1.0, 1.0,
    1.0, 1.0, 0.5, 0.5, 0.5, 0.5,
    0.5, 0.5,
};

#if MD_MPEG_AUDIO_FIXED_POINT

// Fixed point arithmetic, using nothing wider than 32 bits.
//
// A decoded subband sample has 15 fractional bits. Inside the synthesis filter it gets AUDIO_GUARD_BITS more,
// so rounding errors stay below the 16-bit output's LSB. 4 is the most that can't overflow for any input,
// including corrupt frames: samples are limited to +/-65536 (see audioLimitSample), which the matrix transform
// grows to at most 2^24 before guard bits. The window sum multiplies that by at most 2.73 (the largest sum of
// a window's 16 taps), and each tap's first product (see audioWindowMul) needs
// 2^(24 + guard - 14) * 75038 < 2^31.
static constexpr int AUDIO_GUARD_BITS = 4;

// +/-65536 is the most a valid code can reach (1.0 at the largest scale factor, 2.0). Only invalid codes in
// corrupt frames go past it.
static inline int32_t audioLimitSample(const int32_t value) {
    return value > 65536 ? 65536 : value < -65536 ? -65536 : value;
}

// Q16, always positive. Precise to about -96 dB, the limit of 16-bit output.
static constexpr int32_t audioCosQ16(const float coefficient) {
    return static_cast<int32_t>(static_cast<double>(coefficient) * 65536.0 + 0.5);
}

// Returns round(a * c / 65536). The partial products are split so that each one fits in 32 bits.
template <int32_t c>
static inline int32_t audioCosMul(const int32_t a) {
    constexpr int32_t cHigh = c >> 16;
    constexpr uint32_t cLow = static_cast<uint32_t>(c) & 0xFFFF;
    const uint32_t aLow = static_cast<uint32_t>(a) & 0xFFFF;
    return (a >> 16) * c + static_cast<int32_t>(aLow) * cHigh + static_cast<int32_t>((aLow * cLow + 0x8000) >> 16);
}

#define AUDIO_COS_MUL(value, coefficient) audioCosMul<audioCosQ16(coefficient)>(value)

static inline audio_real_t audioSubband(const int32_t value) {
    return value * (1 << AUDIO_GUARD_BITS);
}

// The window in Q16. Its values are all multiples of 0.5 in Q15, so this is exact and at most 17 bits + sign.
// Only computed at compile time, the float table isn't in the binary.
struct AudioWindowQ16 {
    int32_t values[512];
};

static constexpr AudioWindowQ16 audioMakeWindowQ16() {
    static_assert(sizeof(AUDIO_SYNTHESIS_WINDOW) / sizeof(AUDIO_SYNTHESIS_WINDOW[0]) == 512);
    AudioWindowQ16 window{};
    for (int i = 0; i < 512; i++) {
        window.values[i] = static_cast<int32_t>(AUDIO_SYNTHESIS_WINDOW[i] * 2.0f);
    }
    return window;
}

static constexpr AudioWindowQ16 AUDIO_SYNTHESIS_WINDOW_Q16 = audioMakeWindowQ16();

// Returns round(v * window / 65536), so the sum keeps v's format. Splitting v at bit 14 keeps both products in
// 32 bits: the low one is under 2^14 * 2^17, the bound on the high one is explained at AUDIO_GUARD_BITS.
static const int32_t *const AUDIO_WINDOW = AUDIO_SYNTHESIS_WINDOW_Q16.values;

static inline audio_real_t audioWindowMul(const int32_t window, const audio_real_t v) {
    return ((v >> 14) * window + (((v & 0x3FFF) * window) >> 14) + 2) >> 2;
}

// The sum has 15 + AUDIO_GUARD_BITS fractional bits, so 1.0 is 32768 and full scale peaks can clip
static inline AudioSample audioOutputSample(const audio_real_t value) {
    const int32_t rounded = (value + (1 << (AUDIO_GUARD_BITS - 1))) >> AUDIO_GUARD_BITS;
    return static_cast<AudioSample>(rounded > 32767 ? 32767 : rounded < -32768 ? -32768 : rounded);
}

#else

#define AUDIO_COS_MUL(value, coefficient) ((value) * (coefficient))

static inline int32_t audioLimitSample(const int32_t value) {
    return value;
}

static inline audio_real_t audioSubband(const int32_t value) {
    return static_cast<float>(value);
}

static const float *const AUDIO_WINDOW = AUDIO_SYNTHESIS_WINDOW;

static inline audio_real_t audioWindowMul(const float window, const audio_real_t v) {
    return window * v;
}

static inline AudioSample audioOutputSample(const audio_real_t value) {
    // The sum has 30 fractional bits: 15 from the sample, 15 from the window
    return value / 1073741824.0f;
}

#endif

// Quantizer lookup, step 1: bitrate classes
static const uint8_t AUDIO_QUANT_LUT_STEP_1[2][16] = {
    // 32, 48, 56, 64, 80, 96,112,128,160,192,224,256,320,384 <- bitrate
    {0, 0, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2}, // mono
    // 16, 24, 28, 32, 40, 48, 56, 64, 80, 96,112,128,160,192 <- bitrate / chan
    {0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 2} // stereo
};


// Quantizer lookup, step 2: bitrate class, sample rate -> B2 table idx, sblimit
static const uint8_t AUDIO_QUANT_TAB_A = (27 | 64); // Table 3-B.2a: high-rate, sblimit = 27
static const uint8_t AUDIO_QUANT_TAB_B = (30 | 64); // Table 3-B.2b: high-rate, sblimit = 30
static const uint8_t AUDIO_QUANT_TAB_C = 8; // Table 3-B.2c:  low-rate, sblimit =  8
static const uint8_t AUDIO_QUANT_TAB_D = 12; // Table 3-B.2d:  low-rate, sblimit = 12

static const uint8_t QUANT_LUT_STEP_2[3][3] = {
    //44.1 kHz,              48 kHz,                32 kHz
    {AUDIO_QUANT_TAB_C, AUDIO_QUANT_TAB_C, AUDIO_QUANT_TAB_D}, // 32 - 48 kbit/sec/ch
    {AUDIO_QUANT_TAB_A, AUDIO_QUANT_TAB_A, AUDIO_QUANT_TAB_A}, // 56 - 80 kbit/sec/ch
    {AUDIO_QUANT_TAB_B, AUDIO_QUANT_TAB_A, AUDIO_QUANT_TAB_B} // 96+	 kbit/sec/ch
};

// Quantizer lookup, step 3: B2 table, subband -> nbal, row index
// (upper 4 bits: nbal, lower 4 bits: row index)
static const uint8_t AUDIO_QUANT_LUT_STEP_3[3][32] = {
    // Low-rate table (3-B.2c and 3-B.2d)
    {
        0x44, 0x44,
        0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34
    },
    // High-rate table (3-B.2a and 3-B.2b)
    {
        0x43, 0x43, 0x43,
        0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42,
        0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31,
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20
    },
    // MPEG-2 LSR table (B.2 in ISO 13818-3)
    {
        0x45, 0x45, 0x45, 0x45,
        0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34,
        0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24,
        0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24
    }
};

// Quantizer lookup, step 4: table row, allocation[] value -> quant table index
static const uint8_t AUDIO_QUANT_LUT_STEP4[6][16] = {
    {0, 1, 2, 17},
    {0, 1, 2, 3, 4, 5, 6, 17},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 17},
    {0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17},
    {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}
};

static const quantizer_spec_t AUDIO_QUANT_TAB[] = {
    {3, 1, 5}, //  1
    {5, 1, 7}, //  2
    {7, 0, 3}, //  3
    {9, 1, 10}, //  4
    {15, 0, 4}, //  5
    {31, 0, 5}, //  6
    {63, 0, 6}, //  7
    {127, 0, 7}, //  8
    {255, 0, 8}, //  9
    {511, 0, 9}, // 10
    {1023, 0, 10}, // 11
    {2047, 0, 11}, // 12
    {4095, 0, 12}, // 13
    {8191, 0, 13}, // 14
    {16383, 0, 14}, // 15
    {32767, 0, 15}, // 16
    {65535, 0, 16} // 17
};

Mpeg1AudioDecoder::Mpeg1AudioDecoder()
    : m_stream([this]()-> void { decode(); })
      , m_bitStream(&m_stream)
      , m_decoderState(DECODE_HEADER) {
    m_stream.setSizeRequired(AUDIO_HEADER_SIZE);
}

bool Mpeg1AudioDecoder::hasPendingFrame() {
    return m_decoderState == DECODE_FRAME && m_stream.isFull();
}

uint32_t Mpeg1AudioDecoder::channels() const {
    return m_channelMode == AUDIO_MODE_MONO ? 1 : 2;
}

void Mpeg1AudioDecoder::decode() {
    switch (m_decoderState) {
        case DECODE_HEADER:
            decodeHeader();
            break;
        case DECODE_FRAME:
            // Backpressure: if the output can't take a whole frame, leave the frame's bytes buffered.
            // isFull() stays true, so the demuxer stops feeding us and retries via executeCallbacks().
            if (m_audioDisabled) {
                resync();
                return;
            }
            if (m_soundBuffer.free() < SAMPLES_PER_FRAME * channels()) {
                return;
            }
            try {
                decodeFrame();
            } catch (const MStream::EndOfData &) {
                // Corrupt frame data read past the end of the frame
                std::cout << "Corrupt audio frame, skipping" << std::endl;
                resync();
            }
            break;
        default:
            throw std::runtime_error("Unknown state encountered");
    }
}

/**
 * Checks a 32-bit frame header can be decoded, so corrupt data isn't mistaken for a frame.
 * Disables audio after 3 headers of an unsupported format before any valid frame.
 */
bool Mpeg1AudioDecoder::isValidHeader(const uint32_t header) {
    const auto sync = header >> 21;
    const auto version = (header >> 19) & 3;
    const auto layer = (header >> 17) & 3;
    const auto bitRateIndex = (header >> 12) & 15;
    const auto samplingFrequencyIndex = (header >> 10) & 3;
    const auto channelMode = (header >> 6) & 3;

    if (sync != 0x7FF) {
        return false;
    }
    if (version != AUDIO_MPEG_1 || layer != AUDIO_LAYER_II) {
        // Several of these before any valid frame means the stream is another format (e.g. MP3).
        // Disable audio rather than risk decoding noise from data that happens to look like a header.
        if (!m_hasLoggedHeaderInfo && ++m_unsupportedHeaderCount == 3) {
            m_audioDisabled = true;
            std::cout << "Unsupported audio format, only MPEG-1 Layer II is supported. Audio disabled." << std::endl;
        }
        return false;
    }
    // Bit rate 0 is free format (unsupported), 15 is invalid, sample rate 3 is reserved
    if (bitRateIndex == 0 || bitRateIndex == 15 || samplingFrequencyIndex == 3) {
        return false;
    }
    // The audio output is opened for the first frame's format, so it can't change
    if (m_hasLoggedHeaderInfo
        && (samplingFrequencyIndex != m_samplingFrequencyIndex
            || (channelMode == AUDIO_MODE_MONO) != (m_channelMode == AUDIO_MODE_MONO))) {
        return false;
    }
    return true;
}

/**
 * Drops all buffered data and goes back to looking for a frame header.
 */
void Mpeg1AudioDecoder::resync() {
    m_bitStream.reset();
    while (!m_stream.isEmpty()) {
        m_stream.pull(1);
        m_resyncSkippedBytes++;
    }
    m_stream.setSizeRequired(AUDIO_HEADER_SIZE);
    m_decoderState = DECODE_HEADER;
}

void Mpeg1AudioDecoder::reset() {
    m_stream.clear();
    m_bitStream.reset();
    m_stream.setSizeRequired(AUDIO_HEADER_SIZE);
    m_decoderState = DECODE_HEADER;
    m_firstFramePtsMillis = -1;
    m_resyncSkippedBytes = 0;
    m_expectResync = true;
    std::memset(V, 0, sizeof(V));
    v_pos = 0;
}

void Mpeg1AudioDecoder::decodeHeader() {
    if (m_audioDisabled) {
        resync(); // discards the buffered data
        m_resyncSkippedBytes = 0;
        return;
    }
    m_bitStream.align8();
    // Search byte by byte for a valid header. Skipping a byte leaves fewer than AUDIO_HEADER_SIZE bytes
    // buffered, so the demuxer feeds another byte and calls us again.
    if (!isValidHeader(m_stream.peek(4))) {
        m_stream.skip(1);
        m_resyncSkippedBytes++;
        return;
    }
    if (m_resyncSkippedBytes && !m_expectResync) {
        std::cout << "Corrupt audio, skipped " << m_resyncSkippedBytes << " bytes" << std::endl;
    }
    m_resyncSkippedBytes = 0;
    m_expectResync = false;

    m_bitStream.pull(11); // frame sync
    m_audioVersion = m_bitStream.pull(2);
    m_layerDescription = m_bitStream.pull(2);

    m_protectionBit = m_bitStream.pull(1);
    m_bitRateIndex = m_bitStream.pull(4);

    m_samplingFrequencyIndex = m_bitStream.pull(2);
    const auto paddingBit = m_bitStream.pull(1);
    // skip private bit
    m_bitStream.pull(1);
    m_channelMode = m_bitStream.pull(2);

    if (m_channelMode == AUDIO_MODE_JOINT_STEREO) {
        m_bound = (m_bitStream.pull(2) + 1) << 2;
    } else {
        m_bitStream.skip(2);
        m_bound = m_channelMode == AUDIO_MODE_MONO ? 0 : 32;
    }

    m_copyright = m_bitStream.pull(1);
    m_original = m_bitStream.pull(1);
    // TODO implement emphasis filter
    m_emphasis = m_bitStream.pull(2);

    // 0 means a CRC follows. It isn't checked.
    if (!m_protectionBit) {
        m_stream.skip(2);
    }

    if (m_firstFramePtsMillis < 0) {
        // The audio clock is anchored on the first frame's PTS; drop frames until one is known
        if (m_stream.getPts(0) < 0) {
            resync();
            m_resyncSkippedBytes = 0;
            return;
        }
        m_firstFramePtsMillis = m_stream.getPts(0);
    }

    m_bitRate = AUDIO_BIT_RATE[m_bitRateIndex];
    m_samplingFrequency = AUDIO_SAMPLE_RATE[m_samplingFrequencyIndex];
    m_frameSize = (144 * m_bitRate / m_samplingFrequency) + paddingBit;
    // Bytes left in the frame: the 4-byte header, and the 2-byte CRC if present, have been read
    m_stream.setSizeRequired(m_frameSize - (m_protectionBit ? 4 : 6));

    m_decoderState = DECODE_FRAME;

    if (!m_hasLoggedHeaderInfo) {
        /*info_out << "========================" << std::endl;
        info_out << ">>> Audio info <<<" << std::endl;
        info_out << "Channel Mode : " << ChannelModeDesc[m_channelMode] << std::endl;
        info_out << "SampleRate   : " << m_samplingFrequency << " Hz" << std::endl;
        info_out << "Bitrate      : " << (m_bitRate / 1000) << " Kbps" << std::endl;
        info_out << "Frame size   : " << m_frameSize << " bytes" << std::endl;
        info_out << "========================" << std::endl;*/
        m_hasLoggedHeaderInfo = true;
    }
}

const quantizer_spec_t *Mpeg1AudioDecoder::audio_read_allocation(const int sb, const int tab3) {
    const int tab4 = AUDIO_QUANT_LUT_STEP_3[tab3][sb];
    const int qtab = AUDIO_QUANT_LUT_STEP4[tab4 & 15][m_bitStream.pull(tab4 >> 4)];
    return qtab ? (&AUDIO_QUANT_TAB[qtab - 1]) : nullptr;
}


void Mpeg1AudioDecoder::audio_read_samples(const int ch, const int sb, const int part) {
    const quantizer_spec_t *q = allocation[ch][sb];
    int32_t scale_factor = m_scale_factor[ch][sb][part];
    int32_t *sampled = sample[ch][sb];
    int32_t val = 0;

    if (!q) {
        // No bits allocated for this subband
        sampled[0] = sampled[1] = sampled[2] = 0;
        return;
    }

    if (scale_factor == 63) {
        scale_factor = 0;
    } else {
        const auto shift = scale_factor / 3;
        scale_factor = (AUDIO_SCALEFACTOR_BASE[scale_factor % 3] + ((1 << shift) >> 1)) >> shift;
    }

    // Decode samples
    int adj = q->levels;
    if (q->group) {
        // Decode grouped samples
        val = m_bitStream.pull(q->bits);
        sampled[0] = val % adj;
        val /= adj;
        sampled[1] = val % adj;
        sampled[2] = val / adj;
    } else {
        // Decode direct samples
        sampled[0] = m_bitStream.pull(q->bits);
        sampled[1] = m_bitStream.pull(q->bits);
        sampled[2] = m_bitStream.pull(q->bits);
    }

    // Postmultiply samples: code 0 is the most negative value, the range is +/-(levels - 1) / levels
    // The scale is 65536 / levels with 12 fractional bits, so large level counts keep their precision
    const int32_t scale = ((1 << 28) + (adj >> 1)) / adj;
    adj = ((adj + 1) >> 1) - 1;

    val = ((sampled[0] - adj) * scale + 2048) >> 12;
    sampled[0] = audioLimitSample((val * (scale_factor >> 12) + ((val * (scale_factor & 4095) + 2048) >> 12)) >> 12);

    val = ((sampled[1] - adj) * scale + 2048) >> 12;
    sampled[1] = audioLimitSample((val * (scale_factor >> 12) + ((val * (scale_factor & 4095) + 2048) >> 12)) >> 12);

    val = ((sampled[2] - adj) * scale + 2048) >> 12;
    sampled[2] = audioLimitSample((val * (scale_factor >> 12) + ((val * (scale_factor & 4095) + 2048) >> 12)) >> 12);
}

// The matrix transform's 64 outputs v[0..63] are symmetric: v[16] = 0, v[32 - i] = -v[i] for i = 1..15,
// v[32] = -v[0] and v[48 - i] = v[48 + i] for i = 1..15. So a block only stores 32 of them, d[k] = v[k] and
// d[16 + k] = v[48 + k] for k = 0..15, and the windowing rebuilds the rest. This halves V without changing the
// output: the mirrored values are negated before the multiply, exactly as when they were stored.

// u[j] += w[j] * v[j] for j = 0..31, from block d. Each stored value is loaded once for both of its uses.
template <typename Window>
static inline void audioWindowFirstHalf(audio_real_t *u, const audio_real_t *d, const Window *w) {
    u[0] += audioWindowMul(w[0], d[0]);
    // v[16] is 0
    for (int k = 1; k < 16; k++) {
        const audio_real_t v = d[k];
        u[k] += audioWindowMul(w[k], v);
        u[32 - k] += audioWindowMul(w[32 - k], -v);
    }
}

// u[j] += w[j] * v[32 + j] for j = 0..31, from block d
template <typename Window>
static inline void audioWindowSecondHalf(audio_real_t *u, const audio_real_t *d, const Window *w) {
    u[0] += audioWindowMul(w[0], -d[0]);
    u[16] += audioWindowMul(w[16], d[16]);
    for (int k = 1; k < 16; k++) {
        const audio_real_t v = d[16 + k];
        u[16 + k] += audioWindowMul(w[16 + k], v);
        u[16 - k] += audioWindowMul(w[16 - k], v);
    }
}

// 32-point matrix transform. s: subband samples, ss: sample index 0..2, d: the block in V, 32 values written.
void audio_matrix_transform(int32_t s[32][3], int ss, audio_real_t *d) {
    audio_real_t t01, t02, t03, t04, t05, t06, t07, t08, t09, t10, t11, t12,
            t13, t14, t15, t16, t17, t18, t19, t20, t21, t22, t23, t24,
            t25, t26, t27, t28, t29, t30, t31, t32, t33;

    t01 = audioSubband(s[0][ss] + s[31][ss]);
    t02 = AUDIO_COS_MUL(audioSubband(s[0][ss] - s[31][ss]), 0.500602998235f);
    t03 = audioSubband(s[1][ss] + s[30][ss]);
    t04 = AUDIO_COS_MUL(audioSubband(s[1][ss] - s[30][ss]), 0.505470959898f);
    t05 = audioSubband(s[2][ss] + s[29][ss]);
    t06 = AUDIO_COS_MUL(audioSubband(s[2][ss] - s[29][ss]), 0.515447309923f);
    t07 = audioSubband(s[3][ss] + s[28][ss]);
    t08 = AUDIO_COS_MUL(audioSubband(s[3][ss] - s[28][ss]), 0.53104259109f);
    t09 = audioSubband(s[4][ss] + s[27][ss]);
    t10 = AUDIO_COS_MUL(audioSubband(s[4][ss] - s[27][ss]), 0.553103896034f);
    t11 = audioSubband(s[5][ss] + s[26][ss]);
    t12 = AUDIO_COS_MUL(audioSubband(s[5][ss] - s[26][ss]), 0.582934968206f);
    t13 = audioSubband(s[6][ss] + s[25][ss]);
    t14 = AUDIO_COS_MUL(audioSubband(s[6][ss] - s[25][ss]), 0.622504123036f);
    t15 = audioSubband(s[7][ss] + s[24][ss]);
    t16 = AUDIO_COS_MUL(audioSubband(s[7][ss] - s[24][ss]), 0.674808341455f);
    t17 = audioSubband(s[8][ss] + s[23][ss]);
    t18 = AUDIO_COS_MUL(audioSubband(s[8][ss] - s[23][ss]), 0.744536271002f);
    t19 = audioSubband(s[9][ss] + s[22][ss]);
    t20 = AUDIO_COS_MUL(audioSubband(s[9][ss] - s[22][ss]), 0.839349645416f);
    t21 = audioSubband(s[10][ss] + s[21][ss]);
    t22 = AUDIO_COS_MUL(audioSubband(s[10][ss] - s[21][ss]), 0.972568237862f);
    t23 = audioSubband(s[11][ss] + s[20][ss]);
    t24 = AUDIO_COS_MUL(audioSubband(s[11][ss] - s[20][ss]), 1.16943993343f);
    t25 = audioSubband(s[12][ss] + s[19][ss]);
    t26 = AUDIO_COS_MUL(audioSubband(s[12][ss] - s[19][ss]), 1.48416461631f);
    t27 = audioSubband(s[13][ss] + s[18][ss]);
    t28 = AUDIO_COS_MUL(audioSubband(s[13][ss] - s[18][ss]), 2.05778100995f);
    t29 = audioSubband(s[14][ss] + s[17][ss]);
    t30 = AUDIO_COS_MUL(audioSubband(s[14][ss] - s[17][ss]), 3.40760841847f);
    t31 = audioSubband(s[15][ss] + s[16][ss]);
    t32 = AUDIO_COS_MUL(audioSubband(s[15][ss] - s[16][ss]), 10.1900081235f);

    t33 = t01 + t31;
    t31 = AUDIO_COS_MUL(t01 - t31, 0.502419286188f);
    t01 = t03 + t29;
    t29 = AUDIO_COS_MUL(t03 - t29, 0.52249861494f);
    t03 = t05 + t27;
    t27 = AUDIO_COS_MUL(t05 - t27, 0.566944034816f);
    t05 = t07 + t25;
    t25 = AUDIO_COS_MUL(t07 - t25, 0.64682178336f);
    t07 = t09 + t23;
    t23 = AUDIO_COS_MUL(t09 - t23, 0.788154623451f);
    t09 = t11 + t21;
    t21 = AUDIO_COS_MUL(t11 - t21, 1.06067768599f);
    t11 = t13 + t19;
    t19 = AUDIO_COS_MUL(t13 - t19, 1.72244709824f);
    t13 = t15 + t17;
    t17 = AUDIO_COS_MUL(t15 - t17, 5.10114861869f);
    t15 = t33 + t13;
    t13 = AUDIO_COS_MUL(t33 - t13, 0.509795579104f);
    t33 = t01 + t11;
    t01 = AUDIO_COS_MUL(t01 - t11, 0.601344886935f);
    t11 = t03 + t09;
    t09 = AUDIO_COS_MUL(t03 - t09, 0.899976223136f);
    t03 = t05 + t07;
    t07 = AUDIO_COS_MUL(t05 - t07, 2.56291544774f);
    t05 = t15 + t03;
    t15 = AUDIO_COS_MUL(t15 - t03, 0.541196100146f);
    t03 = t33 + t11;
    t11 = AUDIO_COS_MUL(t33 - t11, 1.30656296488f);
    t33 = t05 + t03;
    t05 = AUDIO_COS_MUL(t05 - t03, 0.707106781187f);
    t03 = t15 + t11;
    t15 = AUDIO_COS_MUL(t15 - t11, 0.707106781187f);
    t03 += t15;
    t11 = t13 + t07;
    t13 = AUDIO_COS_MUL(t13 - t07, 0.541196100146f);
    t07 = t01 + t09;
    t09 = AUDIO_COS_MUL(t01 - t09, 1.30656296488f);
    t01 = t11 + t07;
    t07 = AUDIO_COS_MUL(t11 - t07, 0.707106781187f);
    t11 = t13 + t09;
    t13 = AUDIO_COS_MUL(t13 - t09, 0.707106781187f);
    t11 += t13;
    t01 += t11;
    t11 += t07;
    t07 += t13;
    t09 = t31 + t17;
    t31 = AUDIO_COS_MUL(t31 - t17, 0.509795579104f);
    t17 = t29 + t19;
    t29 = AUDIO_COS_MUL(t29 - t19, 0.601344886935f);
    t19 = t27 + t21;
    t21 = AUDIO_COS_MUL(t27 - t21, 0.899976223136f);
    t27 = t25 + t23;
    t23 = AUDIO_COS_MUL(t25 - t23, 2.56291544774f);
    t25 = t09 + t27;
    t09 = AUDIO_COS_MUL(t09 - t27, 0.541196100146f);
    t27 = t17 + t19;
    t19 = AUDIO_COS_MUL(t17 - t19, 1.30656296488f);
    t17 = t25 + t27;
    t27 = AUDIO_COS_MUL(t25 - t27, 0.707106781187f);
    t25 = t09 + t19;
    t19 = AUDIO_COS_MUL(t09 - t19, 0.707106781187f);
    t25 += t19;
    t09 = t31 + t23;
    t31 = AUDIO_COS_MUL(t31 - t23, 0.541196100146f);
    t23 = t29 + t21;
    t21 = AUDIO_COS_MUL(t29 - t21, 1.30656296488f);
    t29 = t09 + t23;
    t23 = AUDIO_COS_MUL(t09 - t23, 0.707106781187f);
    t09 = t31 + t21;
    t31 = AUDIO_COS_MUL(t31 - t21, 0.707106781187f);
    t09 += t31;
    t29 += t09;
    t09 += t23;
    t23 += t31;
    t17 += t29;
    t29 += t25;
    t25 += t09;
    t09 += t27;
    t27 += t23;
    t23 += t19;
    t19 += t31;
    t21 = t02 + t32;
    t02 = AUDIO_COS_MUL(t02 - t32, 0.502419286188f);
    t32 = t04 + t30;
    t04 = AUDIO_COS_MUL(t04 - t30, 0.52249861494f);
    t30 = t06 + t28;
    t28 = AUDIO_COS_MUL(t06 - t28, 0.566944034816f);
    t06 = t08 + t26;
    t08 = AUDIO_COS_MUL(t08 - t26, 0.64682178336f);
    t26 = t10 + t24;
    t10 = AUDIO_COS_MUL(t10 - t24, 0.788154623451f);
    t24 = t12 + t22;
    t22 = AUDIO_COS_MUL(t12 - t22, 1.06067768599f);
    t12 = t14 + t20;
    t20 = AUDIO_COS_MUL(t14 - t20, 1.72244709824f);
    t14 = t16 + t18;
    t16 = AUDIO_COS_MUL(t16 - t18, 5.10114861869f);
    t18 = t21 + t14;
    t14 = AUDIO_COS_MUL(t21 - t14, 0.509795579104f);
    t21 = t32 + t12;
    t32 = AUDIO_COS_MUL(t32 - t12, 0.601344886935f);
    t12 = t30 + t24;
    t24 = AUDIO_COS_MUL(t30 - t24, 0.899976223136f);
    t30 = t06 + t26;
    t26 = AUDIO_COS_MUL(t06 - t26, 2.56291544774f);
    t06 = t18 + t30;
    t18 = AUDIO_COS_MUL(t18 - t30, 0.541196100146f);
    t30 = t21 + t12;
    t12 = AUDIO_COS_MUL(t21 - t12, 1.30656296488f);
    t21 = t06 + t30;
    t30 = AUDIO_COS_MUL(t06 - t30, 0.707106781187f);
    t06 = t18 + t12;
    t12 = AUDIO_COS_MUL(t18 - t12, 0.707106781187f);
    t06 += t12;
    t18 = t14 + t26;
    t26 = AUDIO_COS_MUL(t14 - t26, 0.541196100146f);
    t14 = t32 + t24;
    t24 = AUDIO_COS_MUL(t32 - t24, 1.30656296488f);
    t32 = t18 + t14;
    t14 = AUDIO_COS_MUL(t18 - t14, 0.707106781187f);
    t18 = t26 + t24;
    t24 = AUDIO_COS_MUL(t26 - t24, 0.707106781187f);
    t18 += t24;
    t32 += t18;
    t18 += t14;
    t26 = t14 + t24;
    t14 = t02 + t16;
    t02 = AUDIO_COS_MUL(t02 - t16, 0.509795579104f);
    t16 = t04 + t20;
    t04 = AUDIO_COS_MUL(t04 - t20, 0.601344886935f);
    t20 = t28 + t22;
    t22 = AUDIO_COS_MUL(t28 - t22, 0.899976223136f);
    t28 = t08 + t10;
    t10 = AUDIO_COS_MUL(t08 - t10, 2.56291544774f);
    t08 = t14 + t28;
    t14 = AUDIO_COS_MUL(t14 - t28, 0.541196100146f);
    t28 = t16 + t20;
    t20 = AUDIO_COS_MUL(t16 - t20, 1.30656296488f);
    t16 = t08 + t28;
    t28 = AUDIO_COS_MUL(t08 - t28, 0.707106781187f);
    t08 = t14 + t20;
    t20 = AUDIO_COS_MUL(t14 - t20, 0.707106781187f);
    t08 += t20;
    t14 = t02 + t10;
    t02 = AUDIO_COS_MUL(t02 - t10, 0.541196100146f);
    t10 = t04 + t22;
    t22 = AUDIO_COS_MUL(t04 - t22, 1.30656296488f);
    t04 = t14 + t10;
    t10 = AUDIO_COS_MUL(t14 - t10, 0.707106781187f);
    t14 = t02 + t22;
    t02 = AUDIO_COS_MUL(t02 - t22, 0.707106781187f);
    t14 += t02;
    t04 += t14;
    t14 += t10;
    t10 += t02;
    t16 += t04;
    t04 += t08;
    t08 += t14;
    t14 += t28;
    t28 += t10;
    t10 += t20;
    t20 += t02;
    t21 += t16;
    t16 += t32;
    t32 += t04;
    t04 += t06;
    t06 += t08;
    t08 += t18;
    t18 += t14;
    t14 += t30;
    t30 += t28;
    t28 += t26;
    t26 += t10;
    t10 += t12;
    t12 += t20;
    t20 += t24;
    t24 += t02;

    // The first 16 values of the block, the 16 others of its first half mirror them
    d[0] = t05;
    d[1] = t30;
    d[2] = t27;
    d[3] = t28;
    d[4] = t07;
    d[5] = t26;
    d[6] = t23;
    d[7] = t10;
    d[8] = t15;
    d[9] = t12;
    d[10] = t19;
    d[11] = t20;
    d[12] = t13;
    d[13] = t24;
    d[14] = t31;
    d[15] = t02;
    // The last 16 values of the block, the 16 others of its second half mirror them
    d[16] = -t33;
    d[17] = -t21;
    d[18] = -t17;
    d[19] = -t16;
    d[20] = -t01;
    d[21] = -t32;
    d[22] = -t29;
    d[23] = -t04;
    d[24] = -t03;
    d[25] = -t06;
    d[26] = -t25;
    d[27] = -t08;
    d[28] = -t11;
    d[29] = -t18;
    d[30] = -t09;
    d[31] = -t14;
}


void Mpeg1AudioDecoder::decodeFrame() {
    // Prepare the quantizer table lookups
    int tab3 = 0;
    int sblimit = 0;

    const auto tab1 = (m_channelMode == AUDIO_MODE_MONO) ? 0 : 1;
    const auto tab2 = AUDIO_QUANT_LUT_STEP_1[tab1][m_bitRateIndex - 1];
    tab3 = QUANT_LUT_STEP_2[tab2][m_samplingFrequencyIndex];
    sblimit = tab3 & 63;
    tab3 >>= 6;

    if (m_bound > sblimit) {
        m_bound = sblimit;
    }

    // Read the allocation information
    for (int sb = 0; sb < m_bound; sb++) {
        allocation[0][sb] = audio_read_allocation(sb, tab3);
        allocation[1][sb] = audio_read_allocation(sb, tab3);
    }

    for (int sb = m_bound; sb < sblimit; sb++) {
        allocation[0][sb] = allocation[1][sb] = audio_read_allocation(sb, tab3);
    }

    // Read scale factor selector information
    int channels = (m_channelMode == AUDIO_MODE_MONO) ? 1 : 2;
    for (int sb = 0; sb < sblimit; sb++) {
        for (int ch = 0; ch < channels; ch++) {
            if (allocation[ch][sb]) {
                scale_factor_info[ch][sb] = m_bitStream.pull(2);
            }
        }
        if (m_channelMode == AUDIO_MODE_MONO) {
            scale_factor_info[1][sb] = scale_factor_info[0][sb];
        }
    }

    // Read scale factors
    for (int sb = 0; sb < sblimit; sb++) {
        for (int ch = 0; ch < channels; ch++) {
            if (allocation[ch][sb]) {
                auto *sf = m_scale_factor[ch][sb];
                switch (scale_factor_info[ch][sb]) {
                    case 0:
                        sf[0] = m_bitStream.pull(6);
                        sf[1] = m_bitStream.pull(6);
                        sf[2] = m_bitStream.pull(6);
                        break;
                    case 1:
                        sf[0] = sf[1] = m_bitStream.pull(6);
                        sf[2] = m_bitStream.pull(6);
                        break;
                    case 2:
                        sf[0] = sf[1] = sf[2] = m_bitStream.pull(6);
                        break;
                    case 3:
                        sf[0] = m_bitStream.pull(6);
                        sf[1] = sf[2] = m_bitStream.pull(6);
                        break;
                }
            }
        }
        if (m_channelMode == AUDIO_MODE_MONO) {
            m_scale_factor[1][sb][0] = m_scale_factor[0][sb][0];
            m_scale_factor[1][sb][1] = m_scale_factor[0][sb][1];
            m_scale_factor[1][sb][2] = m_scale_factor[0][sb][2];
        }
    }

    // Coefficient input and reconstruction
    int out_pos = 0;
    for (int part = 0; part < 3; part++) {
        for (int granule = 0; granule < 4; granule++) {
            // Read the samples
            for (int sb = 0; sb < m_bound; sb++) {
                audio_read_samples(0, sb, part);
                audio_read_samples(1, sb, part);
            }
            for (int sb = m_bound; sb < sblimit; sb++) {
                audio_read_samples(0, sb, part);
                sample[1][sb][0] = sample[0][sb][0];
                sample[1][sb][1] = sample[0][sb][1];
                sample[1][sb][2] = sample[0][sb][2];
            }
            for (int sb = sblimit; sb < 32; sb++) {
                sample[0][sb][0] = 0;
                sample[0][sb][1] = 0;
                sample[0][sb][2] = 0;
                sample[1][sb][0] = 0;
                sample[1][sb][1] = 0;
                sample[1][sb][2] = 0;
            }

            // Synthesis loop
            if (m_soundBuffer.free() >= 32 * 3 * channels) {
                for (int p = 0; p < 3; p++) {
                    // Shifting step
                    v_pos = (v_pos - 64) & 1023;

                    for (int ch = 0; ch < channels; ch++) {
                        audio_matrix_transform(sample[ch], p, &V[ch][v_pos >> 1]);

                        // Build U, windowing, calculate output
                        memset(U, 0, sizeof(U));

                        // Each block gives half of its values, alternating halves as the blocks age: the even
                        // blocks' first halves and the odd blocks' second halves, or the other way round
                        // Block b uses window taps from 512 - v_pos / 2 + 32 * b, modulo 512. That's a multiple
                        // of 32, so a block's 32 taps never wrap.
                        const int window = 512 - (v_pos >> 1);
                        const bool firstHalfOfEven = (v_pos & 64) == 0;
                        for (int block = 0; block < 16; block += 2) {
                            const audio_real_t *d = &V[ch][block * 32];
                            const auto *w = &AUDIO_WINDOW[(window + block * 32) & 511];
                            if (firstHalfOfEven) {
                                audioWindowFirstHalf(U, d, w);
                            } else {
                                audioWindowSecondHalf(U, d, w);
                            }
                        }
                        for (int block = 1; block < 16; block += 2) {
                            const audio_real_t *d = &V[ch][block * 32];
                            const auto *w = &AUDIO_WINDOW[(window + block * 32) & 511];
                            if (firstHalfOfEven) {
                                audioWindowSecondHalf(U, d, w);
                            } else {
                                audioWindowFirstHalf(U, d, w);
                            }
                        }

                        // Output samples
                        for (const audio_real_t j: U) {
                            m_soundBuffer.writeByte(audioOutputSample(j));
                        }
                    } // End of synthesis channel loop
                    out_pos += 32;
                } // End of synthesis sub-block loop
            }
        } // Decoding of the granule finished
    }

    m_bitStream.align8();
    while (!m_stream.isEmpty()) {
        const auto val = m_stream.pull(1);
        assert(!val); // Padding or ancillary data up to the end of the frame
    }

    if (m_frameCallback) {
        m_frameCallback(m_samplingFrequency, channels, &m_soundBuffer);
    }

    m_stream.setSizeRequired(AUDIO_HEADER_SIZE);
    m_decoderState = DECODE_HEADER;
}
