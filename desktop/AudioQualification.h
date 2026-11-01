#pragma once
#include "SessionStore.h"
#include <QCryptographicHash>
#include <zlib.h>

namespace frondAudio {
// Read only the compressed metadata and NOR prefix, with fixed-size buffers and bounded input.
inline QByteArray sessionNorMd5(const QString& path) {
    const auto header = frondSessions::readHeader(path);
    if (!frondSessions::openable(header)) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(48)) return {};
    const auto sizes = file.read(24);
    if (sizes.size() != 24) return {};
    auto sizeAt = [&](int offset) {
        return quint32(uchar(sizes[offset])) | (quint32(uchar(sizes[offset + 1])) << 8) |
               (quint32(uchar(sizes[offset + 2])) << 16) | (quint32(uchar(sizes[offset + 3])) << 24);
    };
    quint64 skip = sizeAt(0), left = sizeAt(4);
    const auto& profile = frondFirmware::profile(frondFirmware::deviceFromId(header.device));
    if (skip > (1u << 20) || left != profile.norSize || !left) return {};
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

inline bool qualified(frondFirmware::Device device, const QString& session, const QString& nor) {
    const auto& profile = frondFirmware::profile(device);
    if (device == frondFirmware::Device::automatic || !profile.capabilities.fastAudio || !profile.frontend.fastAudioNorMd5[0]) return false;
    QByteArray md5;
    if (!session.isEmpty()) md5 = sessionNorMd5(session);
    else {
        QFile file(nor);
        if (quint64(QFileInfo(nor).size()) != profile.norSize || !file.open(QIODevice::ReadOnly)) return false;
        QCryptographicHash digest(QCryptographicHash::Md5);
        if (!digest.addData(&file)) return false;
        md5 = digest.result().toHex();
    }
    return md5 == profile.frontend.fastAudioNorMd5;
}
}
