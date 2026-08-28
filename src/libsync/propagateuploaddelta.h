/*
 * SPDX-FileCopyrightText: 2026 CrispCloud Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Block-level delta sync upload: instead of re-uploading entire files,
 * compute Adler-32 + SHA-256 block maps and upload only changed blocks
 * via the crispcloud_delta server app's REST API.
 *
 * Requires the crispcloud_delta Nextcloud/ownCloud app to be installed.
 * Falls back to normal upload if the app is not detected.
 */

#pragma once

#include "propagateupload.h"
#include "deltasyncutils.h"

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
    BlockMap _localBlockMap;
    BlockMap _remoteBlockMap;
    QVector<int> _changedBlocks;
    int _currentBlockIndex = 0;
    bool _deltaAvailable = false;
};

} // namespace OCC
