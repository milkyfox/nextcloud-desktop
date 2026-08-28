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
        // PHP: $a=1; $b=0; for each byte: $a=($a+ord)%65521; $b=($b+$a)%65521; return ($b<<16)|$a
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

    void testFindChangedBlocksFileShrunk()
    {
        // Local has 2 blocks, remote had 3.
        // Blocks 0-1 identical; block 2 absent locally → no changed blocks.
        // Server truncation is handled by ?size= in the finalize request.
        BlockMap local, remote;
        for (int i = 0; i < 3; ++i) {
            BlockSignature sig;
            sig.blockIndex = i;
            sig.weakHash = 0x1000 + i;
            sig.strongHash = QByteArray("h") + QByteArray::number(i);
            remote.signatures.append(sig);
            if (i < 2)
                local.signatures.append(sig);
        }
        local.blockCount = 2;
        remote.blockCount = 3;

        QVector<int> changed = DeltaSyncUtils::findChangedBlocks(local, remote);
        QVERIFY(changed.isEmpty());
    }

    void testFindChangedBlocksFileGrown()
    {
        // Local has 3 blocks, remote had 2. Block 2 is new and must be uploaded.
        BlockMap local, remote;
        for (int i = 0; i < 3; ++i) {
            BlockSignature sig;
            sig.blockIndex = i;
            sig.weakHash = 0x2000 + i;
            sig.strongHash = QByteArray("h") + QByteArray::number(i);
            local.signatures.append(sig);
            if (i < 2)
                remote.signatures.append(sig);
        }
        local.blockCount = 3;
        remote.blockCount = 2;

        QVector<int> changed = DeltaSyncUtils::findChangedBlocks(local, remote);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed[0], 2);
    }

    void testWeakHashDifferentDataSameLength()
    {
        QByteArray a(4 * 1024 * 1024, 'X');
        QByteArray b(4 * 1024 * 1024, 'Y');
        QVERIFY(DeltaSyncUtils::adler32(a) != DeltaSyncUtils::adler32(b));
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
};

QTEST_GUILESS_MAIN(TestDeltaSync)
#include "testdeltasync.moc"
