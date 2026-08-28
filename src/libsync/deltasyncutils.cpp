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

namespace OCC {
namespace DeltaSyncUtils {

static constexpr quint32 AdlerMod = 65521;

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

} // namespace DeltaSyncUtils
} // namespace OCC
