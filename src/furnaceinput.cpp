/*
 * Furnace tracker module input plugin for fooyin
 */

#include "furnaceinput.h"
#include "furnaceparser.h"

#include <engine/engine.h>
#include <ta-log.h>

// Stub required by Furnace engine (normally in main.cpp)
void reportError(String what) {
    logE("%s", what);
}

#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

#include <algorithm>
#include <cstring>

Q_LOGGING_CATEGORY(FUR_INPUT, "fy.furnace")

using namespace Qt::StringLiterals;

namespace {

constexpr int kSampleRate = 44100;
constexpr int kChannels = 2;
constexpr int kRenderBufSize = 2048;

QStringList fileExtensions()
{
    static const QStringList extensions = {u"fur"_s};
    return extensions;
}
} // namespace

namespace Fooyin::FurnacePlugin {

// ── Decoder ──────────────────────────────────────────────────────────────────

FurnaceDecoder::FurnaceDecoder() = default;

FurnaceDecoder::~FurnaceDecoder()
{
    stop();
}

QStringList FurnaceDecoder::extensions() const
{
    return fileExtensions();
}

bool FurnaceDecoder::isSeekable() const
{
    return true;
}

std::optional<AudioFormat> FurnaceDecoder::init(const AudioSource& source, const Track& track,
                                                 DecoderOptions /*options*/)
{
    // Read the raw .fur file into memory
    QByteArray fileData;
    if(source.device) {
        fileData = source.device->readAll();
    } else {
        QFile file(source.filepath);
        if(!file.open(QIODevice::ReadOnly)) {
            qCWarning(FUR_INPUT) << "Failed to open" << source.filepath;
            return {};
        }
        fileData = file.readAll();
    }

    if(fileData.isEmpty()) {
        qCWarning(FUR_INPUT) << "Empty file" << source.filepath;
        return {};
    }

    // Create and initialize DivEngine
    m_engine = std::make_unique<DivEngine>();
    m_engine->prePreInit();
    m_engine->preInit(true);

    // DivEngine::load() takes ownership of the buffer (calls delete[]),
    // so we must allocate a copy with new[]
    const auto fileSize = static_cast<size_t>(fileData.size());
    auto* data = new unsigned char[fileSize];
    std::memcpy(data, fileData.constData(), fileSize);

    if(!m_engine->load(data, fileSize, source.filepath.toStdString().c_str())) {
        qCWarning(FUR_INPUT) << "DivEngine failed to load" << source.filepath;
        m_engine.reset();
        return {};
    }

    // Initialize the engine (creates chip emulators, processes samples)
    if(!m_engine->init()) {
        qCWarning(FUR_INPUT) << "DivEngine init failed for" << source.filepath;
        m_engine.reset();
        return {};
    }

    // Allocate render buffers
    m_outBuf[0] = new float[kRenderBufSize];
    m_outBuf[1] = new float[kRenderBufSize];

    // Configure playback: 2 loops then stop
    m_engine->setLoops(2);

    // Start playback
    m_engine->play();

    m_format = AudioFormat{SampleFormat::F32, kSampleRate, kChannels};
    m_currentPos = 0;
    m_isDecoding = true;

    qCInfo(FUR_INPUT) << "Initialized DivEngine:" << source.filepath;

    return m_format;
}

void FurnaceDecoder::stop()
{
    m_isDecoding = false;

    if(m_engine) {
        m_engine->stop();
        m_engine->quit(false);
        m_engine.reset();
    }

    delete[] m_outBuf[0];
    delete[] m_outBuf[1];
    m_outBuf[0] = nullptr;
    m_outBuf[1] = nullptr;
    m_currentPos = 0;
}

void FurnaceDecoder::seek(uint64_t pos)
{
    if(!m_engine || !m_isDecoding) {
        return;
    }

    // Restart and fast-forward to target position
    m_engine->setLoops(2);
    m_engine->play();
    m_currentPos = 0;

    uint64_t targetSamples = pos * kSampleRate / 1000;

    // Render and discard audio until we reach the target position
    while(m_currentPos < targetSamples && m_engine->isPlaying()) {
        unsigned int toRender = std::min(
            static_cast<unsigned int>(kRenderBufSize),
            static_cast<unsigned int>(targetSamples - m_currentPos));
        m_engine->nextBuf(nullptr, m_outBuf, 0, kChannels, toRender);
        m_currentPos += toRender;
    }
}

AudioBuffer FurnaceDecoder::readBuffer(size_t bytes)
{
    if(!m_isDecoding || !m_engine || !m_engine->isPlaying()) {
        return {};
    }

    // Calculate how many frames the caller wants
    const int bytesPerFrame = m_format.bytesPerFrame();
    if(bytesPerFrame <= 0) {
        return {};
    }

    unsigned int framesRequested = static_cast<unsigned int>(bytes) / static_cast<unsigned int>(bytesPerFrame);
    if(framesRequested > kRenderBufSize) {
        framesRequested = kRenderBufSize;
    }

    // Render audio from DivEngine
    m_engine->nextBuf(nullptr, m_outBuf, 0, kChannels, framesRequested);

    // Calculate start time in ms
    uint64_t startTimeMs = m_currentPos * 1000 / kSampleRate;

    // Create AudioBuffer and fill with interleaved float data
    const size_t outputBytes = static_cast<size_t>(framesRequested) * static_cast<size_t>(bytesPerFrame);
    AudioBuffer buffer{m_format, startTimeMs};
    buffer.resize(outputBytes);

    auto* dst = reinterpret_cast<float*>(buffer.data());
    for(unsigned int i = 0; i < framesRequested; ++i) {
        dst[i * 2] = std::clamp(m_outBuf[0][i], -1.0f, 1.0f);
        dst[i * 2 + 1] = std::clamp(m_outBuf[1][i], -1.0f, 1.0f);
    }

    m_currentPos += framesRequested;

    return buffer;
}

// ── Reader ───────────────────────────────────────────────────────────────────

QStringList FurnaceReader::extensions() const
{
    return fileExtensions();
}

bool FurnaceReader::canReadCover() const
{
    return false;
}

bool FurnaceReader::canWriteMetaData() const
{
    return false;
}

bool FurnaceReader::readTrack(const AudioSource& source, Track& track)
{
    auto data = source.device ? Furnace::decompressFur(source.device)
                              : Furnace::decompressFur(source.filepath);
    if(!data) {
        qCWarning(FUR_INPUT) << "Failed to decompress" << track.filepath();
        return false;
    }

    auto info = Furnace::parseSongInfo(*data);
    if(!info) {
        qCWarning(FUR_INPUT) << "Failed to parse" << track.filepath();
        return false;
    }

    // Set track metadata
    track.setTitle(info->songName);
    track.setArtists({info->author});
    track.setCodec(u"Furnace"_s);
    track.setChannels(2); // stereo output
    track.setSampleRate(44100);
    track.setBitDepth(16);
    track.setEncoding(u"Tracker"_s);
    track.setFileSize(QFileInfo{track.filepath()}.size());

    uint64_t durationMs = Furnace::estimateDuration(*info);
    track.setDuration(durationMs);

    // Store chip/system info as genre
    track.setGenres({info->systemName});

    // Store extra tags
    track.addExtraTag(u"SYSTEM"_s, info->systemName);
    track.addExtraTag(u"FORMAT_VERSION"_s, QString::number(info->formatVersion));
    track.addExtraTag(u"TUNING"_s, QStringLiteral("%1 Hz").arg(info->tuning));
    track.addExtraTag(u"SPEED"_s, QStringLiteral("%1/%2").arg(info->speed1).arg(info->speed2));
    track.addExtraTag(u"PATTERN_LENGTH"_s, QString::number(info->patternLength));
    track.addExtraTag(u"ORDERS"_s, QString::number(info->ordersLength));
    track.addExtraTag(u"INSTRUMENTS"_s, QString::number(info->instrumentCount));
    track.addExtraTag(u"CHANNELS"_s, QString::number(info->channelCount));

    // Chip list
    QStringList chipNames;
    for(const auto& chip : info->chips) {
        chipNames.append(chip.name);
    }
    track.addExtraTag(u"CHIPS"_s, chipNames.join(u", "_s));

    return true;
}

} // namespace Fooyin::FurnacePlugin
