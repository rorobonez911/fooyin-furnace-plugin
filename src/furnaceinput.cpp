/*
 * Furnace tracker module input plugin for fooyin
 *
 * Strategy: DivEngine has global state so we use a single shared engine
 * protected by a global mutex. init() grabs the engine, kicks off a
 * background render thread that streams audio into a growing buffer,
 * and returns immediately. readBuffer() consumes from that buffer,
 * waiting briefly if the render thread hasn't caught up yet.
 * This gives near-instant playback start regardless of track length.
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
#include <chrono>
#include <cstring>

Q_LOGGING_CATEGORY(FUR_INPUT, "fy.furnace")

using namespace Qt::StringLiterals;

namespace {

constexpr int kSampleRate = 44100;
constexpr int kChannels = 2;
constexpr int kRenderBufSize = 2048;
// Max render length: 10 minutes
constexpr size_t kMaxRenderFrames = kSampleRate * 60 * 10;

QStringList fileExtensions()
{
    static const QStringList extensions = {u"fur"_s};
    return extensions;
}

// ── Global shared engine ─────────────────────────────────────────────────────
std::mutex g_engineMutex;
DivEngine* g_engine = nullptr;
bool g_engineReady = false;

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

void FurnaceDecoder::renderThread()
{
    // Acquire the shared engine and render into our buffer
    const std::lock_guard engineLock(g_engineMutex);

    if(m_stopRender.load()) {
        return;
    }

    DivEngine* engine = getSharedEngine();

    // Tear down previous module
    if(g_engineReady) {
        engine->quitDispatch();
        g_engineReady = false;
    }

    // Load module from the data we stashed
    // m_fileData is set before the thread is launched
    if(!engine->load(m_fileData, m_fileSize, m_filePath.c_str())) {
        std::lock_guard bufLock(m_bufMutex);
        m_renderDone = true;
        m_bufReady.notify_all();
        return;
    }

    if(!g_engineReady) {
        engine->init();
    }
    engine->initDispatch();
    engine->renderSamples();
    g_engineReady = true;

    engine->setLoops(1);
    engine->play();

    float* outBuf[2];
    outBuf[0] = new float[kRenderBufSize];
    outBuf[1] = new float[kRenderBufSize];

    size_t totalFrames = 0;
    while(engine->isPlaying() && totalFrames < kMaxRenderFrames && !m_stopRender.load()) {
        engine->nextBuf(nullptr, outBuf, 0, kChannels, kRenderBufSize);

        {
            std::lock_guard bufLock(m_bufMutex);
            for(int i = 0; i < kRenderBufSize; ++i) {
                m_audioData.push_back(std::clamp(outBuf[0][i], -1.0f, 1.0f));
                m_audioData.push_back(std::clamp(outBuf[1][i], -1.0f, 1.0f));
            }
            m_writeOffset = m_audioData.size();
        }
        m_bufReady.notify_all();

        totalFrames += kRenderBufSize;
    }

    engine->stop();
    delete[] outBuf[0];
    delete[] outBuf[1];

    {
        std::lock_guard bufLock(m_bufMutex);
        m_renderDone = true;
    }
    m_bufReady.notify_all();
}

std::optional<AudioFormat> FurnaceDecoder::init(const AudioSource& source, const Track& track,
                                                 DecoderOptions /*options*/)
{
    // Stop any previous render/playback
    stop();

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

    // Prepare data for the render thread
    // DivEngine::load() takes ownership (calls delete[]), so allocate with new[]
    m_fileSize = static_cast<size_t>(fileData.size());
    m_fileData = new unsigned char[m_fileSize];
    std::memcpy(m_fileData, fileData.constData(), m_fileSize);
    m_filePath = source.filepath.toStdString();

    // Reset state
    m_audioData.clear();
    m_audioData.reserve(kSampleRate * 60 * 2); // ~1 min
    m_writeOffset = 0;
    m_readOffset = 0;
    m_renderDone = false;
    m_stopRender = false;
    m_currentPos = 0;

    m_format = AudioFormat{SampleFormat::F32, kSampleRate, kChannels};
    m_isDecoding = true;

    // Launch render thread
    m_renderWorker = std::thread(&FurnaceDecoder::renderThread, this);

    qCInfo(FUR_INPUT) << "Initialized DivEngine:" << source.filepath;

    return m_format;
}

void FurnaceDecoder::stop()
{
    m_isDecoding = false;

    // Signal render thread to stop and wait for it
    m_stopRender = true;
    if(m_renderWorker.joinable()) {
        m_renderWorker.join();
    }

    m_currentPos = 0;
    m_readOffset = 0;
    m_writeOffset = 0;
    m_renderDone = false;
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

    std::lock_guard bufLock(m_bufMutex);

    if(targetOffset >= m_writeOffset) {
        targetOffset = m_writeOffset > 0 ? m_writeOffset - kChannels : 0;
    }

    m_readOffset = targetOffset;
    m_currentPos = targetOffset / kChannels;
}

AudioBuffer FurnaceDecoder::readBuffer(size_t bytes)
{
    if(!m_isDecoding) {
        return {};
    }

    const int bytesPerFrame = m_format.bytesPerFrame();
    if(bytesPerFrame <= 0) {
        return {};
    }

    size_t framesRequested = bytes / static_cast<size_t>(bytesPerFrame);

    // Wait for data to be available (up to 500ms)
    {
        std::unique_lock bufLock(m_bufMutex);
        m_bufReady.wait_for(bufLock, std::chrono::milliseconds(500), [&] {
            return m_readOffset + (framesRequested * kChannels) <= m_writeOffset
                   || m_renderDone
                   || !m_isDecoding;
        });

        if(!m_isDecoding) {
            return {};
        }

        size_t samplesAvailable = m_writeOffset - m_readOffset;
        size_t framesAvailable = samplesAvailable / kChannels;

        if(framesAvailable == 0) {
            // Render done and no more data
            if(m_renderDone) {
                return {};
            }
            // Timeout but no data yet - return silence to avoid stall
            return {};
        }

        size_t framesToReturn = std::min(framesRequested, framesAvailable);

        uint64_t startTimeMs = m_currentPos * 1000 / kSampleRate;

        const size_t outputBytes = framesToReturn * static_cast<size_t>(bytesPerFrame);
        AudioBuffer buffer{m_format, startTimeMs};
        buffer.resize(outputBytes);

        std::memcpy(buffer.data(), &m_audioData[m_readOffset],
                    framesToReturn * kChannels * sizeof(float));

        m_readOffset += framesToReturn * kChannels;
        m_currentPos += framesToReturn;

        return buffer;
    }
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
