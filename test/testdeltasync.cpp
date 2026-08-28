/*
 * Copyright (C) 2026 CrispCloud Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <QTest>
#include <QTemporaryFile>
#include <QCryptographicHash>

#include "deltasyncutils.h"

using namespace OCC;

class TestDeltaSync : public QObject
{
    Q_OBJECT

private slots:
    void testAdler32Known()
    {
        // RFC 1950 example: adler32("Wikipedia") == 0x11E60398
        QByteArray data = "Wikipedia";
        QCOMPARE(DeltaSyncUtils::adler32(data), static_cast<quint32>(0x11E60398));
    }

    void testAdler32Empty()
    {
        QCOMPARE(DeltaSyncUtils::adler32(QByteArray()), static_cast<quint32>(1));
    }

    void testAdler32MatchesPhp()
    {
        // Verify C++ adler32 matches the PHP implementation in BlockMapService.
        // For "hello": expected 0x062C0215
        QByteArray data = "hello";
        QCOMPARE(DeltaSyncUtils::adler32(data), static_cast<quint32>(0x062C0215));
    }

    void testComputeBlockMapSingleBlock()
    {
        QTemporaryFile tmp;
        QVERIFY(tmp.open());
        QByteArray content(1024 * 1024, 'A'); // 1 MB — one block
        tmp.write(content);
        tmp.flush();

        BlockMap map = DeltaSyncUtils::computeLocalBlockMap(tmp.fileName(), 4 * 1024 * 1024);
        QCOMPARE(map.blockCount, 1);
        QCOMPARE(map.totalSize, static_cast<qint64>(content.size()));
        QCOMPARE(map.signatures.size(), 1);
        QCOMPARE(map.signatures[0].blockIndex, 0);
        QCOMPARE(map.signatures[0].offset, static_cast<qint64>(0));
        QCOMPARE(map.signatures[0].size, static_cast<qint64>(content.size()));
        QCOMPARE(map.signatures[0].weakHash, DeltaSyncUtils::adler32(content));

        QCryptographicHash sha(QCryptographicHash::Sha256);
        sha.addData(content);
        QCOMPARE(map.signatures[0].strongHash, sha.result().toHex());
    }

    void testComputeBlockMapMultipleBlocks()
    {
        const qint64 blockSize = 4 * 1024 * 1024;
        // 10 MB = 2 full blocks + 1 partial block
        QByteArray content(10 * 1024 * 1024, 0);
        for (int i = 0; i < content.size(); ++i)
            content[i] = static_cast<char>(i % 256);

        QTemporaryFile tmp;
        QVERIFY(tmp.open());
        tmp.write(content);
        tmp.flush();

        BlockMap map = DeltaSyncUtils::computeLocalBlockMap(tmp.fileName(), blockSize);
        QCOMPARE(map.blockCount, 3);
        QCOMPARE(map.totalSize, static_cast<qint64>(content.size()));
        QCOMPARE(map.signatures.size(), 3);

        QCOMPARE(map.signatures[0].offset, static_cast<qint64>(0));
        QCOMPARE(map.signatures[0].size, blockSize);
        QCOMPARE(map.signatures[1].offset, blockSize);
        QCOMPARE(map.signatures[1].size, blockSize);
        QCOMPARE(map.signatures[2].offset, 2 * blockSize);
        QCOMPARE(map.signatures[2].size, static_cast<qint64>(content.size() - 2 * blockSize));

        QByteArray block0 = content.left(blockSize);
        QCOMPARE(map.signatures[0].weakHash, DeltaSyncUtils::adler32(block0));
    }

    void testFindChangedBlocksNoneChanged()
    {
        BlockMap local, remote;
        local.blockCount = 3;
        remote.blockCount = 3;
        for (int i = 0; i < 3; ++i) {
            BlockSignature sig;
            sig.blockIndex = i;
            sig.weakHash = 0xABCD0000 + i;
            sig.strongHash = QByteArray("aabbccdd") + QByteArray::number(i);
            local.signatures.append(sig);
            remote.signatures.append(sig);
        }

        QVector<int> changed = DeltaSyncUtils::findChangedBlocks(local, remote);
        QVERIFY(changed.isEmpty());
    }

    void testFindChangedBlocksMiddleChanged()
    {
        BlockMap local, remote;
        for (int i = 0; i < 3; ++i) {
            BlockSignature sig;
            sig.blockIndex = i;
            sig.weakHash = 0xABCD0000 + i;
            sig.strongHash = QByteArray("hash") + QByteArray::number(i);
            local.signatures.append(sig);
            remote.signatures.append(sig);
        }
        local.signatures[1].weakHash = 0xDEADBEEF;
        local.signatures[1].strongHash = "differenthash";

        QVector<int> changed = DeltaSyncUtils::findChangedBlocks(local, remote);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed[0], 1);
    }

    void testParseServerBlockMap()
    {
        QByteArray json = R"({
            "filePath": "/test.bin",
            "totalSize": 8388608,
            "blockSize": 4194304,
            "blockCount": 2,
            "etag": "abc123",
            "signatures": [
                {"blockIndex": 0, "offset": 0, "size": 4194304, "weakHash": 12345, "strongHash": "aabb"},
                {"blockIndex": 1, "offset": 4194304, "size": 4194304, "weakHash": 67890, "strongHash": "ccdd"}
            ]
        })";

        BlockMap map = DeltaSyncUtils::parseServerBlockMap(json);
        QCOMPARE(map.filePath, QStringLiteral("/test.bin"));
        QCOMPARE(map.totalSize, static_cast<qint64>(8388608));
        QCOMPARE(map.blockSize, static_cast<qint64>(4194304));
        QCOMPARE(map.blockCount, 2);
        QCOMPARE(map.etag, QStringLiteral("abc123"));
        QCOMPARE(map.signatures.size(), 2);
        QCOMPARE(map.signatures[0].weakHash, static_cast<quint32>(12345));
        QCOMPARE(map.signatures[1].strongHash, QByteArray("ccdd"));
    }

    // === FastCDC Tests ===

    void testFastCdcSmallFile()
    {
        QTemporaryFile tmp;
        QVERIFY(tmp.open());
        QByteArray content(100 * 1024, 'X'); // 100 KB < minSize (256 KB)
        tmp.write(content);
        tmp.flush();

        FastCdcMap map = DeltaSyncUtils::computeLocalFastCdcMap(tmp.fileName());
        QCOMPARE(map.chunkCount, 1);
        QCOMPARE(map.totalSize, static_cast<qint64>(content.size()));
        QCOMPARE(map.signatures.size(), 1);
        QCOMPARE(map.signatures[0].offset, static_cast<qint64>(0));
        QCOMPARE(map.signatures[0].size, static_cast<qint64>(content.size()));
        QCOMPARE(map.signatures[0].hash, QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
    }

    void testFastCdcMidFileInsertionRecovery()
    {
        // Generate 3 MB of pseudo-random data
        QByteArray original(3 * 1024 * 1024, 0);
        for (int i = 0; i < original.size(); ++i) {
            original[i] = static_cast<char>((i * 1103515245 + 12345) & 0xFF);
        }

        QTemporaryFile tmpOrig;
        QVERIFY(tmpOrig.open());
        tmpOrig.write(original);
        tmpOrig.flush();

        FastCdcMap origMap = DeltaSyncUtils::computeLocalFastCdcMap(tmpOrig.fileName());
        QVERIFY(origMap.chunkCount >= 3);

        // Insert 1234 bytes at offset 1.2 MB
        int insertPos = 1200000;
        QByteArray modified = original.left(insertPos) + QByteArray(1234, 'Z') + original.mid(insertPos);

        QTemporaryFile tmpMod;
        QVERIFY(tmpMod.open());
        tmpMod.write(modified);
        tmpMod.flush();

        FastCdcMap modMap = DeltaSyncUtils::computeLocalFastCdcMap(tmpMod.fileName());

        QSet<QByteArray> origHashes;
        for (const auto &c : origMap.signatures) {
            origHashes.insert(c.hash);
        }

        QVector<int> missing = DeltaSyncUtils::findMissingCdcChunks(modMap, origHashes);
        // FastCDC boundary recovery should keep almost all chunks reused (only 1 or 2 missing)
        QVERIFY(missing.size() <= 2);
        int reused = modMap.chunkCount - missing.size();
        QVERIFY(reused >= modMap.chunkCount - 2);
    }

    void testParseServerFastCdcMap()
    {
        QByteArray json = R"({
            "filePath": "/fastcdc_test.bin",
            "totalSize": 5242880,
            "algorithm": "fastcdc",
            "minSize": 262144,
            "avgSize": 1048576,
            "maxSize": 4194304,
            "blockCount": 3,
            "etag": "etag789",
            "signatures": [
                {"chunkIndex": 0, "offset": 0, "size": 1048576, "hash": "hash111"},
                {"chunkIndex": 1, "offset": 1048576, "size": 2097152, "hash": "hash222"},
                {"chunkIndex": 2, "offset": 3145728, "size": 2097152, "hash": "hash333"}
            ]
        })";

        FastCdcMap map = DeltaSyncUtils::parseServerFastCdcMap(json);
        QCOMPARE(map.filePath, QStringLiteral("/fastcdc_test.bin"));
        QCOMPARE(map.totalSize, static_cast<qint64>(5242880));
        QCOMPARE(map.minSize, static_cast<qint64>(262144));
        QCOMPARE(map.avgSize, static_cast<qint64>(1048576));
        QCOMPARE(map.maxSize, static_cast<qint64>(4194304));
        QCOMPARE(map.chunkCount, 3);
        QCOMPARE(map.etag, QStringLiteral("etag789"));
        QCOMPARE(map.signatures.size(), 3);
        QCOMPARE(map.signatures[0].hash, QByteArray("hash111"));
        QCOMPARE(map.signatures[2].size, static_cast<qint64>(2097152));
    }
};

QTEST_GUILESS_MAIN(TestDeltaSync)
#include "testdeltasync.moc"