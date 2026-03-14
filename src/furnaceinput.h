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
    std::unique_ptr<DivEngine> m_engine;
    float* m_outBuf[2]{nullptr, nullptr};
    uint64_t m_currentPos{0}; // current position in samples
    bool m_isDecoding{false};
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
