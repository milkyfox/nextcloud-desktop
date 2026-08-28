/*
 * Copyright (C) 2026 CrispCloud Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "deltasyncutils.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>

namespace OCC {
namespace DeltaSyncUtils {

static constexpr quint32 AdlerMod = 65521;
static quint32 GearTable[256];
static bool GearTableInitialized = false;

static void initGearTable()
{
    if (GearTableInitialized) return;
    quint32 seed = 0x8a927c3d;
    for (int i = 0; i < 256; ++i) {
        seed = static_cast<quint32>((static_cast<quint64>(seed) * 1103515245ULL + 12345ULL) & 0x7FFFFFFFULL);
        GearTable[i] = seed;
    }
    GearTableInitialized = true;
}

quint32 adler32(const QByteArray &data)
{
    quint32 a = 1, b = 0;
    for (int i = 0; i < data.size(); ++i) {
        a = (a + static_cast<quint8>(data[i])) % AdlerMod;
        b = (b + a) % AdlerMod;
    }
    return (b << 16) | a;
}

BlockMap computeLocalBlockMap(const QString &filePath, qint64 blockSize)
{
    BlockMap map;
    map.filePath = filePath;
    map.blockSize = blockSize;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return map;
    }

    map.totalSize = file.size();
    map.blockCount = map.totalSize == 0
        ? 0
        : static_cast<int>((map.totalSize + blockSize - 1) / blockSize);

    for (int i = 0; i < map.blockCount; ++i) {
        qint64 offset = static_cast<qint64>(i) * blockSize;
        qint64 remaining = qMin(blockSize, map.totalSize - offset);
        QByteArray data = file.read(remaining);
        if (data.size() < remaining) break;

        BlockSignature sig;
        sig.blockIndex = i;
        sig.offset = offset;
        sig.size = data.size();
        sig.weakHash = adler32(data);

        QCryptographicHash sha256(QCryptographicHash::Sha256);
        sha256.addData(data);
        sig.strongHash = sha256.result().toHex();

        map.signatures.append(sig);
    }

    return map;
}

BlockMap parseServerBlockMap(const QByteArray &json)
{
    BlockMap map;
    QJsonDocument doc = QJsonDocument::fromJson(json);
    if (doc.isNull() || !doc.isObject()) return map;

    QJsonObject obj = doc.object();
    map.filePath = obj[QStringLiteral("filePath")].toString();
    map.totalSize = obj[QStringLiteral("totalSize")].toVariant().toLongLong();
    map.blockSize = obj[QStringLiteral("blockSize")].toVariant().toLongLong();
    map.blockCount = obj[QStringLiteral("blockCount")].toInt();
    map.etag = obj[QStringLiteral("etag")].toString();

    QJsonArray sigs = obj[QStringLiteral("signatures")].toArray();
    for (const auto &val : sigs) {
        QJsonObject s = val.toObject();
        BlockSignature sig;
        sig.blockIndex = s[QStringLiteral("blockIndex")].toInt();
        sig.offset = s[QStringLiteral("offset")].toVariant().toLongLong();
        sig.size = s[QStringLiteral("size")].toVariant().toLongLong();
        sig.weakHash = static_cast<quint32>(s[QStringLiteral("weakHash")].toVariant().toULongLong());
        sig.strongHash = s[QStringLiteral("strongHash")].toString().toLatin1();
        map.signatures.append(sig);
    }

    return map;
}

QVector<int> findChangedBlocks(const BlockMap &local, const BlockMap &remote)
{
    QHash<int, const BlockSignature *> remoteByIndex;
    for (const auto &sig : remote.signatures) {
        remoteByIndex[sig.blockIndex] = &sig;
    }

    QVector<int> changed;
    for (const auto &localSig : local.signatures) {
        auto it = remoteByIndex.find(localSig.blockIndex);
        if (it == remoteByIndex.end()
            || localSig.weakHash != (*it)->weakHash
            || localSig.strongHash != (*it)->strongHash) {
            changed.append(localSig.blockIndex);
        }
    }
    return changed;
}

FastCdcMap computeLocalFastCdcMap(const QString &filePath, qint64 minSize, qint64 avgSize, qint64 maxSize)
{
    initGearTable();
    static constexpr quint32 maskS = 0x00007fff; // 15 bits
    static constexpr quint32 maskL = 0x00003fff; // 14 bits

    FastCdcMap map;
    map.filePath = filePath;
    map.minSize = minSize;
    map.avgSize = avgSize;
    map.maxSize = maxSize;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return map;
    }

    map.totalSize = file.size();
    if (map.totalSize == 0) {
        return map;
    }

    qint64 offset = 0;
    int chunkIdx = 0;
    QByteArray carryOver;

    while (!file.atEnd() || !carryOver.isEmpty()) {
        QByteArray chunkData = carryOver;
        carryOver.clear();

        // Ensure we have at least minSize bytes
        if (chunkData.size() < minSize && !file.atEnd()) {
            qint64 needed = minSize - chunkData.size();
            chunkData.append(file.read(needed));
        }

        if (chunkData.isEmpty()) {
            break;
        }

        // If file reached end and remaining <= minSize, this is the last chunk
        if (file.atEnd() && chunkData.size() <= minSize) {
            FastCdcChunk chunk;
            chunk.chunkIndex = chunkIdx++;
            chunk.offset = offset;
            chunk.size = chunkData.size();
            chunk.hash = QCryptographicHash::hash(chunkData, QCryptographicHash::Sha256).toHex();
            map.signatures.append(chunk);
            offset += chunk.size;
            break;
        }

        // Read up to maxSize into buffer for scanning
        if (chunkData.size() < maxSize && !file.atEnd()) {
            qint64 needed = maxSize - chunkData.size();
            chunkData.append(file.read(needed));
        }

        quint32 fp = 0;
        int cutPos = 0;
        int len = chunkData.size();
        const auto *bytes = reinterpret_cast<const quint8 *>(chunkData.constData());

        for (int j = static_cast<int>(minSize); j < len; ++j) {
            quint8 b = bytes[j];
            fp = ((fp << 1) + GearTable[b]) & 0xFFFFFFFF;
            quint32 mask = (j < avgSize) ? maskS : maskL;

            if ((fp & mask) == 0 || (j + 1) >= maxSize) {
                cutPos = j + 1;
                break;
            }
        }

        if (cutPos == 0) {
            cutPos = len;
        }

        QByteArray currentChunk = chunkData.left(cutPos);
        carryOver = chunkData.mid(cutPos);

        FastCdcChunk chunk;
        chunk.chunkIndex = chunkIdx++;
        chunk.offset = offset;
        chunk.size = currentChunk.size();
        chunk.hash = QCryptographicHash::hash(currentChunk, QCryptographicHash::Sha256).toHex();
        map.signatures.append(chunk);
        offset += chunk.size;
    }

    map.chunkCount = map.signatures.size();
    return map;
}

FastCdcMap parseServerFastCdcMap(const QByteArray &json)
{
    FastCdcMap map;
    QJsonDocument doc = QJsonDocument::fromJson(json);
    if (doc.isNull() || !doc.isObject()) return map;

    QJsonObject obj = doc.object();
    map.filePath = obj[QStringLiteral("filePath")].toString();
    map.totalSize = obj[QStringLiteral("totalSize")].toVariant().toLongLong();
    map.minSize = obj[QStringLiteral("minSize")].toVariant().toLongLong();
    map.avgSize = obj[QStringLiteral("avgSize")].toVariant().toLongLong();
    map.maxSize = obj[QStringLiteral("maxSize")].toVariant().toLongLong();
    map.chunkCount = obj[QStringLiteral("blockCount")].toInt();
    map.etag = obj[QStringLiteral("etag")].toString();

    QJsonArray sigs = obj[QStringLiteral("signatures")].toArray();
    for (const auto &val : sigs) {
        QJsonObject s = val.toObject();
        FastCdcChunk chunk;
        chunk.chunkIndex = s[QStringLiteral("chunkIndex")].toInt();
        chunk.offset = s[QStringLiteral("offset")].toVariant().toLongLong();
        chunk.size = s[QStringLiteral("size")].toVariant().toLongLong();
        chunk.hash = s[QStringLiteral("hash")].toString().toLatin1();
        map.signatures.append(chunk);
    }

    if (map.chunkCount == 0) {
        map.chunkCount = map.signatures.size();
    }
    return map;
}

QVector<int> findMissingCdcChunks(const FastCdcMap &local, const QSet<QByteArray> &remoteHashes)
{
    QVector<int> missing;
    for (int i = 0; i < local.signatures.size(); ++i) {
        if (!remoteHashes.contains(local.signatures[i].hash)) {
            missing.append(i);
        }
    }
    return missing;
}

} // namespace DeltaSyncUtils
} // namespace OCC