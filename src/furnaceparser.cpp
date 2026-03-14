/*
 * Furnace module parser - C++ port
 */

#include "furnaceparser.h"

#include <QFile>
#include <QIODevice>

#include <zlib.h>

#include <cstring>
#include <unordered_map>

namespace Furnace {

// ── Chip tables ──────────────────────────────────────────────────────────────

static const std::unordered_map<uint16_t, const char*> s_chipNames = {
    {0x00, "END OF LIST"}, {0x01, "YMU759"}, {0x02, "Genesis"},
    {0x03, "SMS (SN76489)"}, {0x04, "Game Boy"}, {0x05, "PC Engine"},
    {0x06, "NES (2A03)"}, {0x07, "C64 (8580)"}, {0x08, "Arcade (YM2151+SegaPCM)"},
    {0x09, "Neo Geo CD (YM2610)"}, {0x42, "Genesis extended"},
    {0x43, "SMS + OPLL"}, {0x46, "NES + VRC7"}, {0x47, "C64 (6581)"},
    {0x49, "Neo Geo CD ext"}, {0x80, "AY-3-8910"}, {0x81, "Amiga (Paula)"},
    {0x82, "YM2151"}, {0x83, "YM2612"}, {0x84, "TIA"}, {0x85, "VIC-20"},
    {0x86, "PET (6522)"}, {0x87, "SNES"}, {0x88, "VRC6"},
    {0x89, "OPLL (YM2413)"}, {0x8A, "FDS"}, {0x8B, "MMC5"},
    {0x8C, "Namco 163"}, {0x8D, "OPN (YM2203)"}, {0x8E, "OPN extended"},
    {0x8F, "OPL (YM3526)"}, {0x90, "OPL2 (YM3812)"}, {0x91, "OPL3 (YMF262)"},
    {0x92, "MultiPCM"}, {0x93, "pcSpeaker"}, {0x94, "Pokemon Mini"},
    {0x95, "AY-3-8914"}, {0x96, "SegaSonic"}, {0x97, "YM2610B"},
    {0x98, "ZX Beeper"}, {0x99, "YM2612 ext"}, {0x9A, "SCC"},
    {0x9B, "OPL drums"}, {0x9C, "OPL2 drums"}, {0x9D, "OPL3 drums"},
    {0x9E, "OPL3 4-op"}, {0x9F, "OPL3 4-op+drums"}, {0xA0, "OPLL drums"},
    {0xA1, "Lynx"}, {0xA2, "VERA"}, {0xA3, "Bubble WSG"}, {0xA4, "SCC+"},
    {0xA5, "Sound Unit"}, {0xA6, "MSM6295"}, {0xA7, "MSM6258"},
    {0xA8, "CX16 VERA"}, {0xA9, "VRC7"}, {0xD1, "ESFM (ESS ES1xxx)"},
    {0xD4, "PowerNoise"}, {0xD5, "Dave"}, {0xD6, "Nintendo DS"},
    {0xD7, "GBA DMA"}, {0xD8, "GBA MinMod"}, {0xD9, "Bifurcator"},
    {0xDE, "YM2610B ext"}, {0xE0, "QSound"}, {0xE1, "X1-010"},
};

static const std::unordered_map<uint16_t, int> s_chipChannels = {
    {0x01, 17}, {0x02, 10}, {0x03, 4}, {0x04, 4}, {0x05, 6}, {0x06, 5},
    {0x07, 3}, {0x08, 18}, {0x09, 14}, {0x42, 13}, {0x43, 4}, {0x46, 5},
    {0x47, 3}, {0x49, 14}, {0x80, 3}, {0x81, 4}, {0x82, 8}, {0x83, 6},
    {0x84, 2}, {0x85, 4}, {0x86, 1}, {0x87, 8}, {0x88, 3}, {0x89, 9},
    {0x8A, 1}, {0x8B, 3}, {0x8C, 8}, {0x8D, 3}, {0x8E, 6}, {0x8F, 9},
    {0x90, 9}, {0x91, 18}, {0x92, 28}, {0x93, 1}, {0x94, 1}, {0x95, 3},
    {0x96, 4}, {0x97, 16}, {0x98, 6}, {0x99, 6}, {0x9A, 5}, {0x9B, 11},
    {0x9C, 11}, {0x9D, 18}, {0x9E, 18}, {0x9F, 18}, {0xA0, 11}, {0xA1, 4},
    {0xA2, 17}, {0xA3, 32}, {0xA4, 5}, {0xA5, 8}, {0xA6, 4}, {0xA7, 1},
    {0xA8, 17}, {0xA9, 6}, {0xD1, 18}, {0xD4, 4}, {0xD5, 6}, {0xD6, 16},
    {0xD7, 2}, {0xD8, 16}, {0xD9, 4}, {0xDE, 16}, {0xE0, 16}, {0xE1, 16},
};

QString chipName(uint16_t chipId)
{
    auto it = s_chipNames.find(chipId);
    if(it != s_chipNames.end()) {
        return QString::fromLatin1(it->second);
    }
    return QStringLiteral("Unknown (0x%1)").arg(chipId, 2, 16, QLatin1Char('0'));
}

int chipChannelCount(uint16_t chipId)
{
    auto it = s_chipChannels.find(chipId);
    return (it != s_chipChannels.end()) ? it->second : 4;
}

// ── Duration estimation ──────────────────────────────────────────────────────

uint64_t estimateDuration(const SongInfo& info)
{
    // duration = orders * patternLength * (speed1 + speed2) / 2 / ticksPerSecond * 1000
    double avgSpeed = (info.speed1 + info.speed2) / 2.0;
    double totalTicks = info.ordersLength * info.patternLength * avgSpeed;
    double hz = info.ticksPerSecond > 0 ? info.ticksPerSecond : 60.0;

    // Apply virtual tempo
    if(info.virtualTempoDen > 0 && info.virtualTempoNum > 0) {
        totalTicks *= static_cast<double>(info.virtualTempoDen) / info.virtualTempoNum;
    }

    return static_cast<uint64_t>(totalTicks / hz * 1000.0);
}

// ── BinaryReader ─────────────────────────────────────────────────────────────

BinaryReader::BinaryReader(const uint8_t* data, size_t size, size_t pos)
    : m_data{data}
    , m_size{size}
    , m_pos{pos}
{ }

BinaryReader::BinaryReader(const QByteArray& data, size_t pos)
    : m_data{reinterpret_cast<const uint8_t*>(data.constData())}
    , m_size{static_cast<size_t>(data.size())}
    , m_pos{pos}
{ }

uint8_t BinaryReader::u8()
{
    return m_data[m_pos++];
}

int8_t BinaryReader::s8()
{
    return static_cast<int8_t>(m_data[m_pos++]);
}

uint16_t BinaryReader::u16()
{
    uint16_t val;
    std::memcpy(&val, m_data + m_pos, 2);
    m_pos += 2;
    return val; // assumes little-endian host
}

int16_t BinaryReader::s16()
{
    int16_t val;
    std::memcpy(&val, m_data + m_pos, 2);
    m_pos += 2;
    return val;
}

uint32_t BinaryReader::u32()
{
    uint32_t val;
    std::memcpy(&val, m_data + m_pos, 4);
    m_pos += 4;
    return val;
}

int32_t BinaryReader::s32()
{
    int32_t val;
    std::memcpy(&val, m_data + m_pos, 4);
    m_pos += 4;
    return val;
}

float BinaryReader::f32()
{
    float val;
    std::memcpy(&val, m_data + m_pos, 4);
    m_pos += 4;
    return val;
}

QString BinaryReader::string()
{
    const auto* start = m_data + m_pos;
    const auto* end = static_cast<const uint8_t*>(std::memchr(start, 0, m_size - m_pos));
    if(!end) {
        auto s = QString::fromUtf8(reinterpret_cast<const char*>(start), static_cast<int>(m_size - m_pos));
        m_pos = m_size;
        return s;
    }
    auto s = QString::fromUtf8(reinterpret_cast<const char*>(start), static_cast<int>(end - start));
    m_pos = static_cast<size_t>(end - m_data) + 1;
    return s;
}

QByteArray BinaryReader::read(size_t n)
{
    QByteArray result(reinterpret_cast<const char*>(m_data + m_pos), static_cast<int>(n));
    m_pos += n;
    return result;
}

void BinaryReader::skip(size_t n) { m_pos += n; }
void BinaryReader::seek(size_t pos) { m_pos = pos; }
size_t BinaryReader::position() const { return m_pos; }
size_t BinaryReader::remaining() const { return m_size - m_pos; }
bool BinaryReader::atEnd() const { return m_pos >= m_size; }

// ── Decompression ────────────────────────────────────────────────────────────

static std::optional<QByteArray> decompressData(const QByteArray& raw)
{
    // Check if already decompressed
    if(raw.size() >= 16 && (raw.startsWith("-Furnace module-") || raw.startsWith("Furnace-B module"))) {
        return raw;
    }

    // Try zlib decompression
    // Start with 4x input size, grow as needed
    QByteArray output;
    uLongf outLen = static_cast<uLongf>(raw.size()) * 4;

    for(int attempt = 0; attempt < 8; ++attempt) {
        output.resize(static_cast<int>(outLen));
        int ret = uncompress(reinterpret_cast<Bytef*>(output.data()), &outLen,
                             reinterpret_cast<const Bytef*>(raw.constData()),
                             static_cast<uLong>(raw.size()));
        if(ret == Z_OK) {
            output.resize(static_cast<int>(outLen));
            return output;
        }
        if(ret == Z_BUF_ERROR) {
            outLen *= 2;
            continue;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<QByteArray> decompressFur(const QString& filepath)
{
    QFile file(filepath);
    if(!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return decompressData(file.readAll());
}

std::optional<QByteArray> decompressFur(QIODevice* device)
{
    if(!device || !device->isOpen()) {
        return std::nullopt;
    }
    return decompressData(device->readAll());
}

// ── Parser helpers ───────────────────────────────────────────────────────────

struct BlockHeader {
    char id[5]{};
    uint32_t size{0};
    size_t dataStart{0};
};

static BlockHeader readBlockHeader(BinaryReader& r)
{
    BlockHeader bh;
    auto idBytes = r.read(4);
    std::memcpy(bh.id, idBytes.constData(), 4);
    bh.id[4] = '\0';
    bh.size = r.u32();
    bh.dataStart = r.position();
    return bh;
}

// ── Legacy INFO parser (v100-239) ────────────────────────────────────────────

static std::optional<SongInfo> parseLegacyInfo(const QByteArray& data, int version, uint32_t infoPtr)
{
    BinaryReader r(data, infoPtr);
    auto bh = readBlockHeader(r);

    if(std::strncmp(bh.id, "INFO", 4) != 0 && std::strncmp(bh.id, "INF2", 4) != 0) {
        return std::nullopt;
    }

    SongInfo info;
    info.formatVersion = version;

    /*time_base*/ r.u8();
    info.speed1 = r.u8();
    info.speed2 = r.u8();
    /*arp_speed*/ r.u8();
    info.ticksPerSecond = r.f32();
    info.patternLength = r.u16();
    info.ordersLength = r.u16();
    info.highlightA = r.u8();
    info.highlightB = r.u8();

    info.instrumentCount = r.u16();
    info.wavetableCount = r.u16();
    info.sampleCount = r.u16();
    info.patternCount = r.u32();

    // Chip IDs + volumes + pannings
    uint8_t chipIds[32];
    uint8_t chipVols[32];
    uint8_t chipPans[32];
    for(int i = 0; i < 32; ++i) chipIds[i] = r.u8();
    for(int i = 0; i < 32; ++i) chipVols[i] = r.u8();
    for(int i = 0; i < 32; ++i) chipPans[i] = r.u8();

    for(int i = 0; i < 32; ++i) {
        if(chipIds[i] != 0) {
            ChipInfo ci;
            ci.index = i;
            ci.chipId = chipIds[i];
            ci.name = chipName(chipIds[i]);
            ci.volume = (chipVols[i] != 0) ? (chipVols[i] + 64) / 128.0f : 1.0f;
            ci.panning = (chipPans[i] != 0) ? chipPans[i] / 127.0f : 0.0f;
            ci.channels = chipChannelCount(chipIds[i]);
            info.chips.push_back(ci);
        }
    }

    r.skip(32 * 4); // chip flags

    info.songName = r.string();
    info.author = r.string();

    if(version >= 100) {
        info.tuning = r.f32();
    }

    // Compat flags batch 1 (v135-v151)
    if(version >= 135) r.skip(4);
    if(version >= 136) r.skip(1);
    if(version >= 138) r.skip(1);
    if(version >= 139) r.skip(1);
    if(version >= 140) r.skip(1);
    if(version >= 141) r.skip(1);
    if(version >= 142) r.skip(1);
    if(version >= 143) r.skip(1);
    if(version >= 144) r.skip(1);
    if(version >= 145) r.skip(1);
    if(version >= 146) r.skip(1);
    if(version >= 147) r.skip(2);
    if(version >= 148) r.skip(1);
    if(version >= 149) r.skip(1);
    if(version >= 150) r.skip(1);
    if(version >= 151) r.skip(1);

    // Skip pointer tables
    r.skip(info.instrumentCount * 4);
    r.skip(info.wavetableCount * 4);
    r.skip(info.sampleCount * 4);
    r.skip(info.patternCount * 4);

    // Channel count
    int chanCount = 0;
    for(const auto& chip : info.chips) {
        chanCount += chip.channels;
    }
    info.channelCount = chanCount;

    // Skip order table + effect columns + hide/collapse + names
    r.skip(info.ordersLength * chanCount);  // orders
    r.skip(chanCount);                       // effect columns
    r.skip(chanCount);                       // hide
    r.skip(chanCount);                       // collapse
    for(int i = 0; i < chanCount; ++i) r.string(); // channel names
    for(int i = 0; i < chanCount; ++i) r.string(); // channel short names

    // Virtual tempo
    if(version >= 160) {
        info.virtualTempoNum = r.u16();
        info.virtualTempoDen = r.u16();
    }

    info.systemName = info.chips.empty() ? QStringLiteral("Unknown") : info.chips[0].name;

    return info;
}

// ── New format INF2 parser (v240+) ───────────────────────────────────────────

static std::optional<SongInfo> parseNewInfo(const QByteArray& data, int version, uint32_t infoPtr)
{
    BinaryReader r(data, infoPtr);
    auto bh = readBlockHeader(r);
    size_t blockEnd = bh.dataStart + bh.size;

    if(std::strncmp(bh.id, "INF2", 4) != 0 && std::strncmp(bh.id, "INFO", 4) != 0) {
        return std::nullopt;
    }

    SongInfo info;
    info.formatVersion = version;

    info.songName = r.string();
    info.author = r.string();
    info.systemName = r.string();
    /*category*/ r.string();
    /*nameJ*/ r.string();
    /*authorJ*/ r.string();
    /*sysNameJ*/ r.string();
    /*catJ*/ r.string();

    info.tuning = r.f32();
    /*autoSys*/ r.u8();
    /*masterVol*/ r.f32();
    info.channelCount = r.u16();
    uint16_t sysLen = r.u16();

    for(int i = 0; i < sysLen; ++i) {
        ChipInfo ci;
        ci.index = i;
        ci.chipId = r.u16();
        ci.channels = r.u16();
        ci.volume = r.f32();
        ci.panning = r.f32();
        /*panFR*/ r.f32();
        ci.name = chipName(ci.chipId);
        info.chips.push_back(ci);
    }

    // Patchbay
    uint32_t pbConns = r.u32();
    r.skip(pbConns * 4);
    r.skip(1); // pbAuto

    // Element pointers - scan for subsong pointer
    uint32_t firstSubsongPtr = 0;
    while(r.position() < blockEnd) {
        uint8_t elemType = r.u8();
        if(elemType == 0) break; // ELEMENT_END

        uint32_t numElems = r.u32();
        for(uint32_t i = 0; i < numElems; ++i) {
            uint32_t ptr = r.u32();
            if(elemType == 1 && i == 0) { // ELEMENT_SUBSONG
                firstSubsongPtr = ptr;
            }
            if(elemType == 4) { info.instrumentCount++; }
            if(elemType == 5) { info.wavetableCount++; }
            if(elemType == 6) { info.sampleCount++; }
            if(elemType == 7) { info.patternCount++; }
        }
    }

    // Parse first subsong for timing info
    if(firstSubsongPtr > 0 && firstSubsongPtr + 8 < static_cast<uint32_t>(data.size())) {
        BinaryReader sr(data, firstSubsongPtr);
        auto sbh = readBlockHeader(sr);
        if(std::strncmp(sbh.id, "SNG2", 4) == 0) {
            info.ticksPerSecond = sr.f32();
            /*arpLen*/ sr.u8();
            /*effectDiv*/ sr.u8();
            info.patternLength = sr.u16();
            info.ordersLength = sr.u16();
            info.highlightA = sr.u8();
            info.highlightB = sr.u8();
            info.virtualTempoNum = sr.u16();
            info.virtualTempoDen = sr.u16();
            uint8_t speedsLen = sr.u8();
            uint16_t speeds[16];
            for(int i = 0; i < 16; ++i) speeds[i] = sr.u16();
            info.speed1 = (speedsLen >= 1) ? speeds[0] : 6;
            info.speed2 = (speedsLen >= 2) ? speeds[1] : info.speed1;
        }
    }

    if(info.systemName.isEmpty() && !info.chips.empty()) {
        info.systemName = info.chips[0].name;
    }

    return info;
}

// ── Main parser entry point ──────────────────────────────────────────────────

std::optional<SongInfo> parseSongInfo(const QByteArray& data)
{
    if(data.size() < 32) {
        return std::nullopt;
    }

    // Check magic
    if(!data.startsWith("-Furnace module-") && !data.startsWith("Furnace-B module")) {
        return std::nullopt;
    }

    BinaryReader r(data);
    r.skip(16); // magic
    uint16_t version = r.u16();
    r.skip(2); // reserved
    uint32_t infoPtr = r.u32();

    if(infoPtr >= static_cast<uint32_t>(data.size())) {
        return std::nullopt;
    }

    if(version >= 240) {
        return parseNewInfo(data, version, infoPtr);
    }
    return parseLegacyInfo(data, version, infoPtr);
}

} // namespace Furnace
