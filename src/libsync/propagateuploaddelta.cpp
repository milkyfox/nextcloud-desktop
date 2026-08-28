/*
 * SPDX-FileCopyrightText: 2026 CrispCloud Contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Block-level and FastCDC delta sync upload implementation.
 */

#include "propagateuploaddelta.h"
#include "networkjobs.h"
#include "account.h"
#include "owncloudpropagator.h"
#include "common/syncjournaldb.h"
#include "common/utility.h"
#include "filesystem.h"
#include "propagateupload.h"
#include "configfile.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkReply>
#include <QBuffer>
#include <QUrlQuery>

namespace OCC {

Q_LOGGING_CATEGORY(lcPropagateUploadDelta, "nextcloud.sync.propagator.upload.delta", QtInfoMsg)

static QString getDeltaRemotePath(const OwncloudPropagator *propagator, const QString &relFile)
{
    QString full = propagator->fullRemotePath(relFile);
    while (full.startsWith(QLatin1Char('/'))) {
        full.remove(0, 1);
    }
    return full;
}

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
    _jobs.append(job);
    connect(job, &QObject::destroyed, this, &PropagateUploadFileCommon::slotJobDestroyed);
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
    slotJobDestroyed(job);

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
    QJsonObject statusObj = doc.object();

    // Check if FastCDC is requested and supported
    bool cdcConfigured = ConfigFile().deltaSyncCdcEnabled();
    bool cdcSupported = false;
    if (statusObj.contains(QStringLiteral("supportedAlgorithms"))) {
        QJsonArray algos = statusObj[QStringLiteral("supportedAlgorithms")].toArray();
        for (const auto &a : algos) {
            if (a.toString() == QStringLiteral("fastcdc")) {
                cdcSupported = true;
                break;
            }
        }
    } else if (statusObj[QStringLiteral("defaultAlgorithm")].toString() == QStringLiteral("fastcdc")) {
        cdcSupported = true;
    }

    _useCdc = cdcConfigured && cdcSupported;

    qCInfo(lcPropagateUploadDelta) << "Delta sync app detected (CDC configured:" << cdcConfigured
                                   << ", CDC supported:" << cdcSupported << ", using:"
                                   << (_useCdc ? "FastCDC" : "Fixed") << "), fetching block map for"
                                   << _fileToUpload._file;

    // Fetch remote block map
    auto url = propagator()->account()->url();
    QString remotePath = getDeltaRemotePath(propagator(), _item->_file);
    url.setPath(url.path() + _deltaAppBase + QStringLiteral("/api/blockmap/") + remotePath);

    if (_useCdc) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("algo"), QStringLiteral("fastcdc"));
        url.setQuery(query);
    }

    auto *bmJob = new SimpleNetworkJob(propagator()->account(), this);
    _jobs.append(bmJob);
    connect(bmJob, &QObject::destroyed, this, &PropagateUploadFileCommon::slotJobDestroyed);
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
    slotJobDestroyed(job);

    auto reply = job->reply();
    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (httpCode == 404) {
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

    if (_useCdc) {
        // === FastCDC Mode ===
        _remoteCdcMap = DeltaSyncUtils::parseServerFastCdcMap(body);
        if (_remoteCdcMap.chunkCount == 0) {
            qCInfo(lcPropagateUploadDelta) << "Empty remote FastCDC map, falling back";
            fallbackToNormalUpload();
            return;
        }

        // Compute local FastCDC map
        _localCdcMap = DeltaSyncUtils::computeLocalFastCdcMap(
            _fileToUpload._path,
            _remoteCdcMap.minSize > 0 ? _remoteCdcMap.minSize : 262144,
            _remoteCdcMap.avgSize > 0 ? _remoteCdcMap.avgSize : 1048576,
            _remoteCdcMap.maxSize > 0 ? _remoteCdcMap.maxSize : 4194304
        );

        if (_localCdcMap.chunkCount == 0) {
            qCWarning(lcPropagateUploadDelta) << "Failed to compute local FastCDC map";
            fallbackToNormalUpload();
            return;
        }

        // Compare
        QSet<QByteArray> remoteHashes;
        for (const auto &sig : _remoteCdcMap.signatures) {
            remoteHashes.insert(sig.hash);
        }

        _missingCdcChunkIndices = DeltaSyncUtils::findMissingCdcChunks(_localCdcMap, remoteHashes);

        // Build Recipe
        _cdcRecipe = QJsonArray();
        for (const auto &chunk : _localCdcMap.signatures) {
            QJsonObject item;
            item[QStringLiteral("hash")] = QString::fromLatin1(chunk.hash);
            _cdcRecipe.append(item);
        }

        if (_missingCdcChunkIndices.isEmpty() && _localCdcMap.totalSize == _remoteCdcMap.totalSize) {
            qCInfo(lcPropagateUploadDelta) << "File is identical (FastCDC), no upload needed:" << _item->_file;
            finalize();
            return;
        }

        qint64 uploadBytes = 0;
        for (int idx : _missingCdcChunkIndices) {
            if (idx < _localCdcMap.signatures.size()) {
                uploadBytes += _localCdcMap.signatures[idx].size;
            }
        }
        double savings = _localCdcMap.totalSize > 0
            ? (1.0 - static_cast<double>(uploadBytes) / _localCdcMap.totalSize) * 100.0
            : 0.0;

        int reused = _localCdcMap.chunkCount - _missingCdcChunkIndices.size();
        qCInfo(lcPropagateUploadDelta) << "FastCDC Delta sync:" << reused << "of" << _localCdcMap.chunkCount
                                       << "chunks reused," << uploadBytes << "bytes to transfer ("
                                       << QString::number(savings, 'f', 1) << "% savings)";

        _currentBlockIndex = 0;
        _deltaBytesTransferred = 0;
        propagator()->reportProgress(*_item, 0);
        uploadNextBlock();
    } else {
        // === Legacy Fixed 4MB Mode ===
        _remoteBlockMap = DeltaSyncUtils::parseServerBlockMap(body);
        if (_remoteBlockMap.blockCount == 0) {
            qCInfo(lcPropagateUploadDelta) << "Empty remote block map, falling back";
            fallbackToNormalUpload();
            return;
        }

        qint64 blockSize = _remoteBlockMap.blockSize > 0 ? _remoteBlockMap.blockSize : DefaultBlockSize;
        _localBlockMap = DeltaSyncUtils::computeLocalBlockMap(_fileToUpload._path, blockSize);

        if (_localBlockMap.blockCount == 0) {
            qCWarning(lcPropagateUploadDelta) << "Failed to compute local block map";
            fallbackToNormalUpload();
            return;
        }

        _changedBlocks = DeltaSyncUtils::findChangedBlocks(_localBlockMap, _remoteBlockMap);

        if (_changedBlocks.isEmpty() && _localBlockMap.totalSize == _remoteBlockMap.totalSize) {
            qCInfo(lcPropagateUploadDelta) << "File is identical, no upload needed:" << _item->_file;
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

        _currentBlockIndex = 0;
        _deltaBytesTransferred = 0;
        propagator()->reportProgress(*_item, 0);
        uploadNextBlock();
    }
}

void PropagateUploadFileDelta::uploadNextBlock()
{
    // Ensure the local file hasn't been changed on disk by another app while delta sync is running
    if (!FileSystem::verifyFileUnchanged(_fileToUpload._path, _item->_size, _item->_modtime)) {
        propagator()->_anotherSyncNeeded = true;
        abortWithError(SyncFileItem::SoftError, tr("Local file changed during sync."));
        return;
    }

    if (_useCdc) {
        // === FastCDC Upload Flow ===
        if (_currentBlockIndex >= _missingCdcChunkIndices.size()) {
            // All CDC chunks uploaded — finalize with Recipe
            auto url = propagator()->account()->url();
            QString remotePath = getDeltaRemotePath(propagator(), _item->_file);
            url.setPath(url.path() + _deltaAppBase + QStringLiteral("/api/finalize/") + remotePath);

            QJsonObject finalizePayload;
            finalizePayload[QStringLiteral("recipe")] = _cdcRecipe;
            finalizePayload[QStringLiteral("totalSize")] = _localCdcMap.totalSize;
            QByteArray jsonBody = QJsonDocument(finalizePayload).toJson(QJsonDocument::Compact);

            auto *finalizeJob = new SimpleNetworkJob(propagator()->account(), this);
            finalizeJob->setTimeout(qMax(300 * 1000, finalizeJob->timeoutMsec()));
            adjustLastJobTimeout(finalizeJob, _localCdcMap.totalSize);
            _jobs.append(finalizeJob);
            connect(finalizeJob, &QObject::destroyed, this, &PropagateUploadFileCommon::slotJobDestroyed);
            QNetworkRequest req;
            req.setUrl(url);
            req.setRawHeader("Content-Type", "application/json");
            req.setRawHeader("OCS-APIREQUEST", "true");
            req.setRawHeader("X-OC-Mtime", QByteArray::number(qint64(_item->_modtime)));
            if (!_remoteCdcMap.etag.isEmpty()) {
                req.setRawHeader("If-Match", '"' + _remoteCdcMap.etag.toUtf8() + '"');
            }

            auto *buffer = new QBuffer(this);
            buffer->setData(jsonBody);
            buffer->open(QIODevice::ReadOnly);

            finalizeJob->startRequest("POST", url, req, buffer);
            connect(finalizeJob, &SimpleNetworkJob::finishedSignal, this, &PropagateUploadFileDelta::slotFinalizeFinished);
            return;
        }

        int chunkIdx = _missingCdcChunkIndices[_currentBlockIndex];
        if (chunkIdx >= _localCdcMap.signatures.size()) {
            qCWarning(lcPropagateUploadDelta) << "CDC chunk index out of range:" << chunkIdx;
            fallbackToNormalUpload();
            return;
        }

        const FastCdcChunk &chunk = _localCdcMap.signatures[chunkIdx];

        QFile file(_fileToUpload._path);
        if (!file.open(QIODevice::ReadOnly)) {
            qCWarning(lcPropagateUploadDelta) << "Cannot open local file for CDC chunk read";
            fallbackToNormalUpload();
            return;
        }
        file.seek(chunk.offset);
        QByteArray chunkData = file.read(chunk.size);
        file.close();

        if (chunkData.size() != chunk.size) {
            qCWarning(lcPropagateUploadDelta) << "Short read for CDC chunk" << chunkIdx;
            fallbackToNormalUpload();
            return;
        }

        qCDebug(lcPropagateUploadDelta) << "Uploading CDC chunk" << chunkIdx
                                        << "hash=" << chunk.hash.left(8)
                                        << "size=" << chunk.size;

        auto url = propagator()->account()->url();
        QString remotePath = getDeltaRemotePath(propagator(), _item->_file);
        url.setPath(url.path() + _deltaAppBase + QStringLiteral("/api/blocks/") + remotePath);
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("hash"), QString::fromLatin1(chunk.hash));
        query.addQueryItem(QStringLiteral("size"), QString::number(chunk.size));
        url.setQuery(query);

        auto *putJob = new SimpleNetworkJob(propagator()->account(), this);
        putJob->setTimeout(120 * 1000);
        _jobs.append(putJob);
        connect(putJob, &QObject::destroyed, this, &PropagateUploadFileCommon::slotJobDestroyed);
        QNetworkRequest req;
        req.setUrl(url);
        req.setRawHeader("Content-Type", "application/octet-stream");
        req.setRawHeader("OCS-APIREQUEST", "true");

        auto *buffer = new QBuffer(this);
        buffer->setData(chunkData);
        buffer->open(QIODevice::ReadOnly);

        putJob->startRequest("POST", url, req, buffer);
        connect(putJob, &SimpleNetworkJob::finishedSignal, this, &PropagateUploadFileDelta::slotBlockUploaded);
    } else {
        // === Legacy Fixed 4MB Flow ===
        if (_currentBlockIndex >= _changedBlocks.size()) {
            auto url = propagator()->account()->url();
            QString remotePath = getDeltaRemotePath(propagator(), _item->_file);
            url.setPath(url.path() + _deltaAppBase + QStringLiteral("/api/finalize/") + remotePath);

            QUrlQuery finalizeQuery;
            finalizeQuery.addQueryItem(QStringLiteral("size"), QString::number(_localBlockMap.totalSize));
            url.setQuery(finalizeQuery);

            auto *finalizeJob = new SimpleNetworkJob(propagator()->account(), this);
            finalizeJob->setTimeout(qMax(300 * 1000, finalizeJob->timeoutMsec()));
            adjustLastJobTimeout(finalizeJob, _localBlockMap.totalSize);
            _jobs.append(finalizeJob);
            connect(finalizeJob, &QObject::destroyed, this, &PropagateUploadFileCommon::slotJobDestroyed);
            QNetworkRequest req;
            req.setUrl(url);
            req.setRawHeader("OCS-APIREQUEST", "true");
            req.setRawHeader("X-OC-Mtime", QByteArray::number(qint64(_item->_modtime)));
            if (!_remoteBlockMap.etag.isEmpty()) {
                req.setRawHeader("If-Match", '"' + _remoteBlockMap.etag.toUtf8() + '"');
            }
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
        QString remotePath = getDeltaRemotePath(propagator(), _item->_file);
        url.setPath(url.path() + _deltaAppBase + QStringLiteral("/api/blocks/") + remotePath);
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("offset"), QString::number(sig.offset));
        query.addQueryItem(QStringLiteral("size"), QString::number(sig.size));
        url.setQuery(query);

        auto *putJob = new SimpleNetworkJob(propagator()->account(), this);
        putJob->setTimeout(120 * 1000);
        _jobs.append(putJob);
        connect(putJob, &QObject::destroyed, this, &PropagateUploadFileCommon::slotJobDestroyed);
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
}

void PropagateUploadFileDelta::slotBlockUploaded()
{
    auto *job = qobject_cast<SimpleNetworkJob *>(sender());
    if (!job) {
        fallbackToNormalUpload();
        return;
    }
    slotJobDestroyed(job);

    auto reply = job->reply();
    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError || httpCode != 200) {
        qCWarning(lcPropagateUploadDelta) << "Block/chunk upload failed, HTTP" << httpCode;
        fallbackToNormalUpload();
        return;
    }

    if (_useCdc) {
        if (_currentBlockIndex < _missingCdcChunkIndices.size()) {
            int chunkIdx = _missingCdcChunkIndices[_currentBlockIndex];
            if (chunkIdx < _localCdcMap.signatures.size()) {
                _deltaBytesTransferred += _localCdcMap.signatures[chunkIdx].size;
            }
        }
    } else {
        if (_currentBlockIndex < _changedBlocks.size()) {
            int blockIdx = _changedBlocks[_currentBlockIndex];
            if (blockIdx < _localBlockMap.signatures.size()) {
                _deltaBytesTransferred += _localBlockMap.signatures[blockIdx].size;
            }
        }
    }
    propagator()->reportProgress(*_item, _deltaBytesTransferred);

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
    slotJobDestroyed(job);

    auto reply = job->reply();
    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError || httpCode != 200) {
        qCWarning(lcPropagateUploadDelta) << "Finalize failed, HTTP" << httpCode;
        fallbackToNormalUpload();
        return;
    }

    // Extract new ETag and FileId from server response to ensure DB consistency
    QByteArray body = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        QString etagStr = doc.object().value(QStringLiteral("etag")).toString();
        if (!etagStr.isEmpty()) {
            _item->_etag = parseEtag(etagStr.toUtf8());
        }
        QString fid = doc.object().value(QStringLiteral("fileId")).toString();
        if (!fid.isEmpty()) {
            _item->_fileId = fid.toUtf8();
        }
    }
    if (_item->_etag.isEmpty()) {
        _item->_etag = parseEtag(getEtagFromReply(reply));
    }
    if (_item->_fileId.isEmpty()) {
        _item->_fileId = reply->rawHeader("OC-FileId");
    }

    if (_useCdc) {
        qint64 totalBytes = _localCdcMap.totalSize;
        qint64 transferredBytes = 0;
        for (int idx : _missingCdcChunkIndices) {
            if (idx < _localCdcMap.signatures.size()) {
                transferredBytes += _localCdcMap.signatures[idx].size;
            }
        }
        double savings = totalBytes > 0
            ? (1.0 - static_cast<double>(transferredBytes) / totalBytes) * 100.0
            : 0.0;

        int reused = _localCdcMap.chunkCount - _missingCdcChunkIndices.size();
        _item->_deltaSyncInfo = QStringLiteral("%1/%2 FastCDC chunks reused, %3 transferred (%4% saved)")
            .arg(reused)
            .arg(_localCdcMap.chunkCount)
            .arg(Utility::octetsToString(transferredBytes))
            .arg(QString::number(savings, 'f', 1));
    } else {
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
    }

    qCInfo(lcPropagateUploadDelta) << "Delta sync completed for" << _item->_file
        << "—" << _item->_deltaSyncInfo;
    propagator()->reportProgress(*_item, _fileToUpload._size);
    finalize();
}

void PropagateUploadFileDelta::fallbackToNormalUpload()
{
    qCInfo(lcPropagateUploadDelta) << "Falling back to normal upload for" << _item->_file;

    if (_item->_size > propagator()->syncOptions()._initialChunkSize
        && propagator()->account()->capabilities().chunkingNg()) {
        _fallbackJob = std::make_unique<PropagateUploadFileNG>(propagator(), _item);
    } else {
        _fallbackJob = std::make_unique<PropagateUploadFileV1>(propagator(), _item);
    }
    _fallbackJob->setDeleteExisting(_deleteExisting);

    connect(_fallbackJob.get(), &PropagatorJob::finished, this, [this](SyncFileItem::Status status) {
        done(status, _item->_errorString);
    });

    _fallbackJob->start();
}

void PropagateUploadFileDelta::abort(PropagatorJob::AbortType abortType)
{
    if (_fallbackJob) {
        _fallbackJob->abort(abortType);
    }
    abortNetworkJobs(abortType,
        [](AbstractNetworkJob *) { return true; });
}

} // namespace OCC