/*
 * SPDX-FileCopyrightText: 2026 CrispCloud Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Block-level and FastCDC delta sync upload: instead of re-uploading entire files,
 * compute block maps (Fixed 4MB or FastCDC) and upload only changed blocks/chunks
 * via the crispcloud_delta server app's REST API.
 */

#pragma once

#include "propagateupload.h"
#include "deltasyncutils.h"

#include <QJsonArray>
#include <QSet>

namespace OCC {

Q_DECLARE_LOGGING_CATEGORY(lcPropagateUploadDelta)

class PropagateUploadFileDelta : public PropagateUploadFileCommon
{
    Q_OBJECT

public:
    PropagateUploadFileDelta(OwncloudPropagator *propagator, const SyncFileItemPtr &item);

    void doStartUpload() override;

public slots:
    void abort(PropagatorJob::AbortType abortType) override;

private slots:
    void slotStatusCheckFinished();
    void slotBlockMapFetched();
    void slotBlockUploaded();
    void slotFinalizeFinished();

private:
    void fallbackToNormalUpload();
    void uploadNextBlock();

    static constexpr qint64 DefaultBlockSize = 4 * 1024 * 1024; // 4 MB
    static constexpr qint64 MinDeltaSyncSize = 10 * 1024 * 1024; // 10 MB

    QString _deltaAppBase;
    bool _useCdc = false;

    // Fixed 4MB state
    BlockMap _localBlockMap;
    BlockMap _remoteBlockMap;
    QVector<int> _changedBlocks;

    // FastCDC state
    FastCdcMap _localCdcMap;
    FastCdcMap _remoteCdcMap;
    QVector<int> _missingCdcChunkIndices;
    QJsonArray _cdcRecipe;

    int _currentBlockIndex = 0;
    bool _deltaAvailable = false;
    std::unique_ptr<PropagateUploadFileCommon> _fallbackJob;
};

} // namespace OCC