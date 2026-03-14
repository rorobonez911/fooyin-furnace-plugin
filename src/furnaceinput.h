/*
 * Furnace tracker module input plugin for fooyin
 *
 * Provides:
 *  - FurnaceReader:  metadata extraction (title, author, system, duration)
 *  - FurnaceDecoder: audio decoding via Furnace's DivEngine
 */

#pragma once

#include <core/engine/audioinput.h>

#include <memory>
#include <mutex>
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
    AudioFormat m_format;
    uint64_t m_currentPos{0};
    bool m_isDecoding{false};

    // Pre-rendered audio buffer for thread safety:
    // init() renders the entire track upfront so readBuffer() just
    // serves from the buffer without touching DivEngine.
    std::vector<float> m_audioData; // interleaved L/R
    size_t m_audioOffset{0};
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
