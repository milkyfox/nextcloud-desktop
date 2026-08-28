/*
 * Copyright (C) 2026 CrispCloud Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Pure algorithmic utilities for block-level and FastCDC delta sync.
 * No QObject/MOC dependencies — safe to include in tests directly.
 */

#pragma once

#include "owncloudlib.h"

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QSet>

namespace OCC {

struct BlockSignature {
    int blockIndex = 0;
    qint64 offset = 0;
    qint64 size = 0;
    quint32 weakHash = 0;
    QByteArray strongHash;
};

struct BlockMap {
    QString filePath;
    qint64 totalSize = 0;
    qint64 blockSize = 0;
    int blockCount = 0;
    QVector<BlockSignature> signatures;
    QString etag;
};

struct FastCdcChunk {
    int chunkIndex = 0;
    qint64 offset = 0;
    qint64 size = 0;
    QByteArray hash; // SHA-256 hex string
};

struct FastCdcMap {
    QString filePath;
    qint64 totalSize = 0;
    qint64 minSize = 262144;  // 256 KB
    qint64 avgSize = 1048576; // 1 MB
    qint64 maxSize = 4194304; // 4 MB
    int chunkCount = 0;
    QVector<FastCdcChunk> signatures;
    QString etag;
};

namespace DeltaSyncUtils {

/// Adler-32 checksum (RFC 1950), matching the server's PHP implementation.
OWNCLOUDSYNC_EXPORT quint32 adler32(const QByteArray &data);

/// Compute the local fixed 4MB block map for a file at the given path.
OWNCLOUDSYNC_EXPORT BlockMap computeLocalBlockMap(const QString &filePath, qint64 blockSize);

/// Parse server JSON fixed block map response.
OWNCLOUDSYNC_EXPORT BlockMap parseServerBlockMap(const QByteArray &json);

/// Compare local vs remote block maps, return list of changed block indices.
OWNCLOUDSYNC_EXPORT QVector<int> findChangedBlocks(const BlockMap &local, const BlockMap &remote);

/// Compute local FastCDC map using sliding Gear hash table.
OWNCLOUDSYNC_EXPORT FastCdcMap computeLocalFastCdcMap(const QString &filePath,
                                                     qint64 minSize = 262144,
                                                     qint64 avgSize = 1048576,
                                                     qint64 maxSize = 4194304);

/// Parse server JSON FastCDC block map response.
OWNCLOUDSYNC_EXPORT FastCdcMap parseServerFastCdcMap(const QByteArray &json);

/// Compare local CDC chunks against remote hash set, return indices of missing chunks.
OWNCLOUDSYNC_EXPORT QVector<int> findMissingCdcChunks(const FastCdcMap &local,
                                                     const QSet<QByteArray> &remoteHashes);

} // namespace DeltaSyncUtils
} // namespace OCC