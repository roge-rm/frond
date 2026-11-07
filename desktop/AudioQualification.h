#pragma once
#include "SessionStore.h"
#include <QCryptographicHash>
#include <zlib.h>

namespace frondAudio {
// Read only the compressed metadata and NOR prefix, with fixed-size buffers and bounded input.
inline QByteArray sessionNorMd5(const QString& path) {
    const auto frond = QFileInfo(path).isDir() ? frondSessions::sessionFrondIn(path) : path;
    const auto header = frondSessions::readHeader(frond);
    if (!frondSessions::openable(header)) return {};
    QFile file(frond);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(48)) return {};
    quint64 skip = 0, left = 0;
    if (header.version >= 7) {
        const auto sizes = file.read(40);
        if (sizes.size() != 40) return {};
        auto size64 = [&](int offset) {
            quint64 n = 0;
            for (int i = 0; i < 8; ++i) n |= quint64(uchar(sizes[offset + i])) << (8 * i);
            return n;
        };
        skip = size64(0); left = size64(8);
        const auto count = file.read(4);
        if (count.size() != 4) return {};
        const quint32 media = quint32(uchar(count[0])) | (quint32(uchar(count[1])) << 8) |
                              (quint32(uchar(count[2])) << 16) | (quint32(uchar(count[3])) << 24);
        const qint64 descriptorSize = header.version >= 8 ? 100 : 92;
        if (media && !file.seek(file.pos() + qint64(media) * descriptorSize)) return {};
    } else {
        const auto sizes = file.read(24);
        if (sizes.size() != 24) return {};
        auto sizeAt = [&](int offset) {
            return quint32(uchar(sizes[offset])) | (quint32(uchar(sizes[offset + 1])) << 8) |
                   (quint32(uchar(sizes[offset + 2])) << 16) | (quint32(uchar(sizes[offset + 3])) << 24);
        };
        skip = sizeAt(0); left = sizeAt(4);
    }
    const auto& profile = frondFirmware::profile(frondFirmware::deviceFromId(header.device));
    if (skip > (1u << 20) || !deviceProfileNorSizeSupported5(profile.deviceType, left) || !left) return {};
    quint64 inputLeft = 2 * (skip + left) + 65536;
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) return {};
    QCryptographicHash digest(QCryptographicHash::Md5);
    char input[65536], output[65536];
    bool valid = true;
    while (left && valid) {
        if (!stream.avail_in) {
            const auto count = file.read(input, qint64(std::min<quint64>(sizeof(input), inputLeft)));
            if (count <= 0) { valid = false; break; }
            inputLeft -= quint64(count);
            stream.next_in = reinterpret_cast<Bytef*>(input); stream.avail_in = uInt(count);
        }
        stream.next_out = reinterpret_cast<Bytef*>(output);
        stream.avail_out = uInt(std::min<quint64>(sizeof(output), skip + left));
        const auto capacity = stream.avail_out;
        const auto before = stream.avail_in;
        const auto status = inflate(&stream, Z_NO_FLUSH);
        const quint64 count = capacity - stream.avail_out;
        const auto skipped = std::min(skip, count); skip -= skipped;
        const auto used = std::min(left, count - skipped);
        if (used) digest.addData(QByteArrayView(output + skipped, qsizetype(used)));
        left -= used;
        valid = (status == Z_OK || status == Z_STREAM_END) &&
                (count || stream.avail_in != before) && (status != Z_STREAM_END || !left);
    }
    inflateEnd(&stream);
    return valid && !left ? digest.result().toHex() : QByteArray();
}

inline QByteArray norMd5(frondFirmware::Device device, const QString& session, const QString& nor) {
    if (device == frondFirmware::Device::automatic) return {};
    if (!session.isEmpty()) return sessionNorMd5(session);
    QFile file(nor);
    if (!deviceProfileNorSizeSupported5(int(device), QFileInfo(nor).size()) || !file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash digest(QCryptographicHash::Md5);
    if (!digest.addData(&file)) return {};
    return digest.result().toHex();
}
inline bool matchesNor(frondFirmware::Device device, const QString& session, const QString& nor,
                       const QByteArray& expected) {
    return !expected.isEmpty() && norMd5(device, session, nor) == expected;
}
inline bool qualified(frondFirmware::Device device, const QString& session, const QString& nor) {
    const auto& profile = frondFirmware::profile(device);
    if (device == frondFirmware::deviceFromId("sony-clie-tg50") || !profile.capabilities.fastAudio) return false;
    const auto md5 = norMd5(device, session, nor);
    if (md5.isEmpty()) return false;
    if (md5 == profile.frontend.fastAudioNorMd5) return true;
    for (const auto& variant : deviceFirmwareVariants5)
        if (int(variant.deviceType) == int(device) && md5 == variant.fastAudioNorMd5) return true;
    return false;
}
// ARM devices use the preview on qualified firmware, otherwise the queue limits only.
inline bool lowLatencyAvailable(frondFirmware::Device device) {
    if (device == frondFirmware::Device::automatic) return false;
    const auto caps = frondFirmware::capabilities(device);
    return caps.sound && deviceProfileRunnerEligible5(int(device));
}
inline QString lowLatencyOption(frondFirmware::Device device, const QString& session, const QString& nor) {
    if (!lowLatencyAvailable(device)) return {};
    return qualified(device, session, nor) ? QStringLiteral("--fast-audio") : QStringLiteral("--low-latency-queue");
}
}
