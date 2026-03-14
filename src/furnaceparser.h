/*
 * Furnace module parser - C++ port for fooyin plugin
 * Parses .fur files (v100-241+) for metadata extraction and playback
 */

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class QIODevice;

namespace Furnace {

struct ChipInfo {
    int index{0};
    uint16_t chipId{0};
    QString name;
    float volume{1.0f};
    float panning{0.0f};
    int channels{0};
};

struct SongInfo {
    QString songName;
    QString author;
    QString systemName;
    int formatVersion{0};
    float tuning{440.0f};
    float ticksPerSecond{60.0f};
    int speed1{6};
    int speed2{6};
    int patternLength{64};
    int ordersLength{1};
    int channelCount{0};
    int instrumentCount{0};
    int wavetableCount{0};
    int sampleCount{0};
    int patternCount{0};
    int virtualTempoNum{150};
    int virtualTempoDen{150};
    int highlightA{4};
    int highlightB{16};
    std::vector<ChipInfo> chips;
};

// Estimate song duration in milliseconds from metadata
uint64_t estimateDuration(const SongInfo& info);

class BinaryReader {
public:
    BinaryReader(const uint8_t* data, size_t size, size_t pos = 0);
    BinaryReader(const QByteArray& data, size_t pos = 0);

    uint8_t u8();
    int8_t s8();
    uint16_t u16();
    int16_t s16();
    uint32_t u32();
    int32_t s32();
    float f32();
    QString string();
    QByteArray read(size_t n);
    void skip(size_t n);
    void seek(size_t pos);
    size_t position() const;
    size_t remaining() const;
    bool atEnd() const;

private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_pos;
};

// Decompress a .fur file (zlib) and return raw bytes
std::optional<QByteArray> decompressFur(const QString& filepath);
std::optional<QByteArray> decompressFur(QIODevice* device);

// Parse song metadata from decompressed data
std::optional<SongInfo> parseSongInfo(const QByteArray& data);

// Get chip name from chip ID
QString chipName(uint16_t chipId);

// Get channel count for a chip ID
int chipChannelCount(uint16_t chipId);

} // namespace Furnace
