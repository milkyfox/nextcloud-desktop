/*
 * Copyright (C) 2026 CrispCloud Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Pure algorithmic utilities for block-level delta sync.
 * No QObject/MOC dependencies — safe to include in tests directly.
 */

#pragma once

#include "owncloudlib.h"

#include <QByteArray>
#include <QString>
#include <QVector>

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

namespace DeltaSyncUtils {

/// Adler-32 checksum (RFC 1950), matching the server's PHP implementation.
OWNCLOUDSYNC_EXPORT quint32 adler32(const QByteArray &data);

/// Compute the local block map for a file at the given path.
OWNCLOUDSYNC_EXPORT BlockMap computeLocalBlockMap(const QString &filePath, qint64 blockSize);

/// Parse server JSON block map response.
OWNCLOUDSYNC_EXPORT BlockMap parseServerBlockMap(const QByteArray &json);

/// Compare local vs remote block maps, return list of changed block indices.
OWNCLOUDSYNC_EXPORT QVector<int> findChangedBlocks(const BlockMap &local, const BlockMap &remote);

} // namespace DeltaSyncUtils
} // namespace OCC
