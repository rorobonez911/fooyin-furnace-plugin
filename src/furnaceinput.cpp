/*
 * Furnace tracker module input plugin for fooyin
 *
 * Strategy: DivEngine has extensive global state (log thread, system registry,
 * static sysDefs) that makes it unsafe to run multiple instances or even
 * init/destroy repeatedly. Instead, we use a single process-wide engine
 * protected by a global mutex. The init() method renders the entire track
 * upfront into a memory buffer, then readBuffer() just serves from that
 * buffer without touching the engine. This makes playback fully thread-safe
 * and allows fooyin's NextTrackPrepareWorker to prepare tracks concurrently.
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
#include <atomic>
#include <cstring>

Q_LOGGING_CATEGORY(FUR_INPUT, "fy.furnace")

using namespace Qt::StringLiterals;

namespace {

constexpr int kSampleRate = 44100;
constexpr int kChannels = 2;
constexpr int kRenderBufSize = 2048;
// Max render length: 10 minutes (safety cap)
constexpr size_t kMaxRenderFrames = kSampleRate * 60 * 10;

QStringList fileExtensions()
{
    static const QStringList extensions = {u"fur"_s};
    return extensions;
}

// ── Global shared engine ─────────────────────────────────────────────────────
// Single DivEngine instance shared across all decoder instances.
// Protected by g_engineMutex. Only used during init() to render tracks.

std::mutex g_engineMutex;
DivEngine* g_engine = nullptr;
bool g_engineReady = false;
std::atomic<unsigned int> g_renderGeneration{0};

DivEngine* getSharedEngine()
{
    if(!g_engine) {
        g_engine = new DivEngine;
        g_engine->prePreInit();
        g_engine->preInit(true);
    }
    return g_engine;
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
    m_isDecoding = false;
    m_currentPos = 0;
    m_audioData.clear();
    m_audioOffset = 0;

    // Signal any in-progress render to abort
    const unsigned int myGeneration = ++g_renderGeneration;

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

    // Render the entire track using the shared engine under lock.
    // This ensures only one thread touches DivEngine at a time.
    {
        const std::lock_guard lock(g_engineMutex);
        DivEngine* engine = getSharedEngine();

        // Tear down previous module's chip emulators
        if(g_engineReady) {
            engine->quitDispatch();
            g_engineReady = false;
        }

        // DivEngine::load() takes ownership of the buffer (calls delete[])
        const auto fileSize = static_cast<size_t>(fileData.size());
        auto* data = new unsigned char[fileSize];
        std::memcpy(data, fileData.constData(), fileSize);

        if(!engine->load(data, fileSize, source.filepath.toStdString().c_str())) {
            qCWarning(FUR_INPUT) << "DivEngine failed to load" << source.filepath;
            return {};
        }

        // Full init on first use, dispatch-only on subsequent
        if(!g_engineReady) {
            engine->init();
        }
        engine->initDispatch();
        engine->renderSamples();
        g_engineReady = true;

        // Configure and start playback
        engine->setLoops(2);
        engine->play();

        // Render the entire track into our buffer
        float* outBuf[2];
        outBuf[0] = new float[kRenderBufSize];
        outBuf[1] = new float[kRenderBufSize];

        m_audioData.reserve(kSampleRate * 60 * 2); // ~1 min initial reserve

        bool aborted = false;
        size_t totalFrames = 0;
        while(engine->isPlaying() && totalFrames < kMaxRenderFrames) {
            // If another init() was called, abort this render
            if(g_renderGeneration.load() != myGeneration) {
                aborted = true;
                break;
            }

            engine->nextBuf(nullptr, outBuf, 0, kChannels, kRenderBufSize);

            for(int i = 0; i < kRenderBufSize; ++i) {
                m_audioData.push_back(std::clamp(outBuf[0][i], -1.0f, 1.0f));
                m_audioData.push_back(std::clamp(outBuf[1][i], -1.0f, 1.0f));
            }
            totalFrames += kRenderBufSize;
        }

        engine->stop();
        delete[] outBuf[0];
        delete[] outBuf[1];

        if(aborted) {
            m_audioData.clear();
            return {};
        }
    }
    // Engine lock released - readBuffer() is now independent

    if(m_audioData.empty()) {
        qCWarning(FUR_INPUT) << "No audio rendered for" << source.filepath;
        return {};
    }

    m_format = AudioFormat{SampleFormat::F32, kSampleRate, kChannels};
    m_currentPos = 0;
    m_audioOffset = 0;
    m_isDecoding = true;

    qCInfo(FUR_INPUT) << "Initialized DivEngine:" << source.filepath
                      << "rendered" << (m_audioData.size() / 2) << "frames";

    return m_format;
}

void FurnaceDecoder::stop()
{
    m_isDecoding = false;
    m_currentPos = 0;
    m_audioOffset = 0;
    m_audioData.clear();
    m_audioData.shrink_to_fit();
}

void FurnaceDecoder::seek(uint64_t pos)
{
    if(!m_isDecoding) {
        return;
    }

    uint64_t targetFrame = pos * kSampleRate / 1000;
    size_t targetOffset = static_cast<size_t>(targetFrame) * kChannels;

    if(targetOffset >= m_audioData.size()) {
        targetOffset = m_audioData.size();
    }

    m_audioOffset = targetOffset;
    m_currentPos = targetFrame;
}

AudioBuffer FurnaceDecoder::readBuffer(size_t bytes)
{
    if(!m_isDecoding || m_audioOffset >= m_audioData.size()) {
        return {};
    }

    const int bytesPerFrame = m_format.bytesPerFrame();
    if(bytesPerFrame <= 0) {
        return {};
    }

    size_t framesRequested = bytes / static_cast<size_t>(bytesPerFrame);
    size_t samplesAvailable = m_audioData.size() - m_audioOffset;
    size_t framesAvailable = samplesAvailable / kChannels;
    size_t framesToReturn = std::min(framesRequested, framesAvailable);

    if(framesToReturn == 0) {
        return {};
    }

    uint64_t startTimeMs = m_currentPos * 1000 / kSampleRate;

    const size_t outputBytes = framesToReturn * static_cast<size_t>(bytesPerFrame);
    AudioBuffer buffer{m_format, startTimeMs};
    buffer.resize(outputBytes);

    std::memcpy(buffer.data(), &m_audioData[m_audioOffset],
                framesToReturn * kChannels * sizeof(float));

    m_audioOffset += framesToReturn * kChannels;
    m_currentPos += framesToReturn;

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
