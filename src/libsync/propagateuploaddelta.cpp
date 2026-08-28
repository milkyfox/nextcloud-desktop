/*
 * SPDX-FileCopyrightText: 2026 CrispCloud Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Block-level delta sync upload implementation.
 */

#include "propagateuploaddelta.h"
#include "networkjobs.h"
#include "account.h"
#include "owncloudpropagator.h"
#include "common/syncjournaldb.h"
#include "common/utility.h"
#include "filesystem.h"
#include "propagateupload.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkReply>
#include <QBuffer>

namespace OCC {

Q_LOGGING_CATEGORY(lcPropagateUploadDelta, "nextcloud.sync.propagator.upload.delta", QtInfoMsg)

PropagateUploadFileDelta::PropagateUploadFileDelta(OwncloudPropagator *propagator, const SyncFileItemPtr &item)
    : PropagateUploadFileCommon(propagator, item)
{
}

void PropagateUploadFileDelta::doStartUpload()
{
    // Only attempt delta sync for files above threshold
    if (_fileToUpload._size < MinDeltaSyncSize) {
        qCInfo(lcPropagateUploadDelta) << "File too small for delta sync, falling back:"
                                       << _fileToUpload._size << "bytes";
        fallbackToNormalUpload();
        return;
    }

    // Probe the server for the crispcloud_delta app
    _deltaAppBase = QStringLiteral("/index.php/apps/crispcloud_delta");

    auto *job = new SimpleNetworkJob(propagator()->account(), this);
    auto url = propagator()->account()->url();
    url.setPath(url.path() + _deltaAppBase + QStringLiteral("/api/status"));

    QNetworkRequest req;
    req.setUrl(url);
    job->startRequest("GET", url, req);
    connect(job, &SimpleNetworkJob::finishedSignal, this, &PropagateUploadFileDelta::slotStatusCheckFinished);
}

void PropagateUploadFileDelta::slotStatusCheckFinished()
{
    auto *job = qobject_cast<SimpleNetworkJob *>(sender());
    if (!job) {
        fallbackToNormalUpload();
        return;
    }

    auto reply = job->reply();
    if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        qCInfo(lcPropagateUploadDelta) << "Delta sync app not available, falling back to normal upload";
        fallbackToNormalUpload();
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (doc.isNull() || doc.object()[QStringLiteral("app")].toString() != QStringLiteral("crispcloud_delta")) {
        qCInfo(lcPropagateUploadDelta) << "Delta sync app response invalid, falling back";
        fallbackToNormalUpload();
        return;
    }

    _deltaAvailable = true;
    qCInfo(lcPropagateUploadDelta) << "Delta sync app detected, fetching remote block map for"
                                   << _fileToUpload._file;

    // Fetch remote block map
    auto url = propagator()->account()->url();
    // The remote path relative to user root
    QString remotePath = _item->_file;
    url.setPath(url.path() + _deltaAppBase + QStringLiteral("/api/blockmap/") + remotePath);

    auto *bmJob = new SimpleNetworkJob(propagator()->account(), this);
    QNetworkRequest req;
    req.setUrl(url);
    bmJob->startRequest("GET", url, req);
    connect(bmJob, &SimpleNetworkJob::finishedSignal, this, &PropagateUploadFileDelta::slotBlockMapFetched);
}

void PropagateUploadFileDelta::slotBlockMapFetched()
{
    auto *job = qobject_cast<SimpleNetworkJob *>(sender());
    if (!job) {
        fallbackToNormalUpload();
        return;
    }

    auto reply = job->reply();
    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (httpCode == 404) {
        // File doesn't exist on server yet — can't do delta, do normal upload
        qCInfo(lcPropagateUploadDelta) << "Remote file not found (new file), falling back to normal upload";
        fallbackToNormalUpload();
        return;
    }

    if (reply->error() != QNetworkReply::NoError || httpCode != 200) {
        qCWarning(lcPropagateUploadDelta) << "Failed to fetch remote block map, HTTP" << httpCode;
        fallbackToNormalUpload();
        return;
    }

    QByteArray body = reply->readAll();
    _remoteBlockMap = DeltaSyncUtils::parseServerBlockMap(body);
    if (_remoteBlockMap.blockCount == 0) {
        qCInfo(lcPropagateUploadDelta) << "Empty remote block map, falling back";
        fallbackToNormalUpload();
        return;
    }

    // Compute local block map using the server's block size
    qint64 blockSize = _remoteBlockMap.blockSize > 0 ? _remoteBlockMap.blockSize : DefaultBlockSize;
    _localBlockMap = DeltaSyncUtils::computeLocalBlockMap(_fileToUpload._path, blockSize);

    if (_localBlockMap.blockCount == 0) {
        qCWarning(lcPropagateUploadDelta) << "Failed to compute local block map";
        fallbackToNormalUpload();
        return;
    }

    // Compare
    _changedBlocks = DeltaSyncUtils::findChangedBlocks(_localBlockMap, _remoteBlockMap);

    if (_changedBlocks.isEmpty()) {
        qCInfo(lcPropagateUploadDelta) << "File is identical, no upload needed:" << _item->_file;
        // File is identical — skip upload, mark as done
        finalize();
        return;
    }

    qint64 changedBytes = 0;
    for (int idx : _changedBlocks) {
        if (idx < _localBlockMap.signatures.size()) {
            changedBytes += _localBlockMap.signatures[idx].size;
        }
    }
    double savingsPercent = _localBlockMap.totalSize > 0
        ? (1.0 - static_cast<double>(changedBytes) / _localBlockMap.totalSize) * 100.0
        : 0.0;

    qCInfo(lcPropagateUploadDelta) << "Delta sync:" << _changedBlocks.size()
                                   << "of" << _localBlockMap.blockCount << "blocks changed,"
                                   << changedBytes << "bytes to transfer ("
                                   << QString::number(savingsPercent, 'f', 1) << "% savings)";

    // Start uploading changed blocks
    _currentBlockIndex = 0;
    uploadNextBlock();
}

void PropagateUploadFileDelta::uploadNextBlock()
{
    if (_currentBlockIndex >= _changedBlocks.size()) {
        // All blocks uploaded — finalize
        auto url = propagator()->account()->url();
        QString remotePath = _item->_file;
        url.setPath(url.path() + _deltaAppBase + QStringLiteral("/api/finalize/") + remotePath);

        QUrlQuery finalizeQuery;
        finalizeQuery.addQueryItem(QStringLiteral("size"), QString::number(_localBlockMap.totalSize));
        url.setQuery(finalizeQuery);

        auto *finalizeJob = new SimpleNetworkJob(propagator()->account(), this);
        QNetworkRequest req;
        req.setUrl(url);
        req.setRawHeader("OCS-APIREQUEST", "true");
        finalizeJob->startRequest("POST", url, req);
        connect(finalizeJob, &SimpleNetworkJob::finishedSignal, this, &PropagateUploadFileDelta::slotFinalizeFinished);
        return;
    }

    int blockIdx = _changedBlocks[_currentBlockIndex];
    if (blockIdx >= _localBlockMap.signatures.size()) {
        qCWarning(lcPropagateUploadDelta) << "Block index out of range:" << blockIdx;
        fallbackToNormalUpload();
        return;
    }

    const BlockSignature &sig = _localBlockMap.signatures[blockIdx];

    // Read block data from local file
    QFile file(_fileToUpload._path);
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(lcPropagateUploadDelta) << "Cannot open local file for block read";
        fallbackToNormalUpload();
        return;
    }
    file.seek(sig.offset);
    QByteArray blockData = file.read(sig.size);
    file.close();

    if (blockData.size() != sig.size) {
        qCWarning(lcPropagateUploadDelta) << "Short read for block" << blockIdx;
        fallbackToNormalUpload();
        return;
    }

    qCDebug(lcPropagateUploadDelta) << "Uploading block" << blockIdx
                                    << "offset=" << sig.offset
                                    << "size=" << sig.size;

    auto url = propagator()->account()->url();
    QString remotePath = _item->_file;
    url.setPath(url.path() + _deltaAppBase + QStringLiteral("/api/blocks/") + remotePath);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("offset"), QString::number(sig.offset));
    query.addQueryItem(QStringLiteral("size"), QString::number(sig.size));
    url.setQuery(query);

    auto *putJob = new SimpleNetworkJob(propagator()->account(), this);
    QNetworkRequest req;
    req.setUrl(url);
    req.setRawHeader("Content-Type", "application/octet-stream");
    req.setRawHeader("OCS-APIREQUEST", "true");

    auto *buffer = new QBuffer(this);
    buffer->setData(blockData);
    buffer->open(QIODevice::ReadOnly);

    putJob->startRequest("POST", url, req, buffer);
    connect(putJob, &SimpleNetworkJob::finishedSignal, this, &PropagateUploadFileDelta::slotBlockUploaded);
}

void PropagateUploadFileDelta::slotBlockUploaded()
{
    auto *job = qobject_cast<SimpleNetworkJob *>(sender());
    if (!job) {
        fallbackToNormalUpload();
        return;
    }

    auto reply = job->reply();
    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError || httpCode != 200) {
        qCWarning(lcPropagateUploadDelta) << "Block upload failed, HTTP" << httpCode;
        fallbackToNormalUpload();
        return;
    }

    _currentBlockIndex++;
    uploadNextBlock();
}

void PropagateUploadFileDelta::slotFinalizeFinished()
{
    auto *job = qobject_cast<SimpleNetworkJob *>(sender());
    if (!job) {
        fallbackToNormalUpload();
        return;
    }

    auto reply = job->reply();
    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError || httpCode != 200) {
        qCWarning(lcPropagateUploadDelta) << "Finalize failed, HTTP" << httpCode;
        fallbackToNormalUpload();
        return;
    }

    qint64 totalBytes = _localBlockMap.totalSize;
    qint64 transferredBytes = 0;
    for (int idx : _changedBlocks) {
        if (idx < _localBlockMap.signatures.size())
            transferredBytes += _localBlockMap.signatures[idx].size;
    }
    double savings = totalBytes > 0
        ? (1.0 - static_cast<double>(transferredBytes) / totalBytes) * 100.0
        : 0.0;

    _item->_deltaSyncInfo = QStringLiteral("%1/%2 blocks, %3 of %4 transferred (%5% saved)")
        .arg(_changedBlocks.size())
        .arg(_localBlockMap.blockCount)
        .arg(Utility::octetsToString(transferredBytes))
        .arg(Utility::octetsToString(totalBytes))
        .arg(QString::number(savings, 'f', 1));

    qCInfo(lcPropagateUploadDelta) << "Delta sync completed for" << _item->_file
        << "—" << _item->_deltaSyncInfo;
    finalize();
}

void PropagateUploadFileDelta::fallbackToNormalUpload()
{
    qCInfo(lcPropagateUploadDelta) << "Falling back to normal upload for" << _item->_file;

    // Create a non-delta upload job directly (avoid recursion via createUploadJob)
    std::unique_ptr<PropagateUploadFileCommon> job;
    if (_item->_size > propagator()->syncOptions()._initialChunkSize
        && propagator()->account()->capabilities().chunkingNg()) {
        job = std::make_unique<PropagateUploadFileNG>(propagator(), _item);
    } else {
        job = std::make_unique<PropagateUploadFileV1>(propagator(), _item);
    }
    job->setDeleteExisting(false);
    job->start();
}

void PropagateUploadFileDelta::abort(PropagatorJob::AbortType abortType)
{
    abortNetworkJobs(abortType,
        [](AbstractNetworkJob *) { return true; });
}

} // namespace OCC
