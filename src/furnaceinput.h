/*
 * Furnace tracker module input plugin for fooyin
 *
 * Provides:
 *  - FurnaceReader:  metadata extraction (title, author, system, duration)
 *  - FurnaceDecoder: audio decoding via Furnace's DivEngine (streaming)
 */

#pragma once

#include <core/engine/audioinput.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class DivEngine;

namespace Fooyin::FurnacePlugin {

class FurnaceDecoder : public AudioDecoder
{
public:
    FurnaceDecoder();
    ~FurnaceDecoder() override;

    [[nodiscard]] QStringList extensions() const override;
    [[nodiscard]] bool isSeekable() const override;

    std::optional<AudioFormat> init(const AudioSource& source, const Track& track,
                                    DecoderOptions options) override;
    void stop() override;
    void seek(uint64_t pos) override;

    AudioBuffer readBuffer(size_t bytes) override;

private:
    void renderThread();

    AudioFormat m_format;
    uint64_t m_currentPos{0};
    bool m_isDecoding{false};

    // File data for render thread (set before thread launch)
    unsigned char* m_fileData{nullptr};
    size_t m_fileSize{0};
    std::string m_filePath;

    // Streaming buffer from render thread to readBuffer()
    std::mutex m_bufMutex;
    std::condition_variable m_bufReady;
    std::vector<float> m_audioData;   // interleaved L/R
    size_t m_writeOffset{0};          // render thread writes here
    size_t m_readOffset{0};           // readBuffer() reads from here
    bool m_renderDone{false};         // render thread finished

    // Render thread
    std::thread m_renderWorker;
    std::atomic<bool> m_stopRender{false};
};

class FurnaceReader : public AudioReader
{
public:
    [[nodiscard]] QStringList extensions() const override;
    [[nodiscard]] bool canReadCover() const override;
    [[nodiscard]] bool canWriteMetaData() const override;

    bool readTrack(const AudioSource& source, Track& track) override;
};

} // namespace Fooyin::FurnacePlugin
