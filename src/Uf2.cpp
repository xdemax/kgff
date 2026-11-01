#include "Uf2.h"

#include <QFile>
#include <QtEndian>

bool isValidUf2(const QString& path, QString* why) {
    auto fail = [&](const QString& m) {
        if (why) *why = m;
        return false;
    };

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail("cannot open file: " + f.errorString());

    const qint64 size = f.size();
    if (size < 512 || (size % 512) != 0) return fail("file size is not a multiple of 512 bytes");

    const QByteArray blk = f.read(512);
    if (blk.size() != 512) return fail("read error");

    const quint32 m0   = qFromLittleEndian<quint32>(blk.constData());
    const quint32 m1   = qFromLittleEndian<quint32>(blk.constData() + 4);
    const quint32 mEnd = qFromLittleEndian<quint32>(blk.constData() + 508);
    if (m0 != 0x0A324655u || m1 != 0x9E5D5157u || mEnd != 0x0AB16F30u)
        return fail("UF2 magic numbers not found");

    // Проверяем, что число блоков соответствует размеру файла
    const quint32 numBlocks = qFromLittleEndian<quint32>(blk.constData() + 24);
    if (numBlocks == 0) return fail("UF2 file declares zero blocks");
    if (static_cast<qint64>(numBlocks) * 512 != size)
        return fail("UF2 block count does not match file size");

    // Family ID RP2040 (0xE48BFF56) в поле fileSize/familyID блока
    const quint32 familyId = qFromLittleEndian<quint32>(blk.constData() + 28);
    if (familyId != 0xE48BFF56u)
        return fail("UF2 file is not for RP2040 (unexpected family ID)");

    // Проверяем magic, family ID, blockNo и payloadSize во всех блоках
    // (защита от обрезанных/битых файлов и от подмены блоков).
    f.seek(0);
    for (quint32 i = 0; i < numBlocks; ++i) {
        const QByteArray b = f.read(512);
        if (b.size() != 512) return fail("read error in block " + QString::number(i));
        const quint32 b0   = qFromLittleEndian<quint32>(b.constData());
        const quint32 b1   = qFromLittleEndian<quint32>(b.constData() + 4);
        const quint32 bEnd = qFromLittleEndian<quint32>(b.constData() + 508);
        if (b0 != 0x0A324655u || b1 != 0x9E5D5157u || bEnd != 0x0AB16F30u)
            return fail("UF2 magic numbers not found in block " + QString::number(i));

        const quint32 blockNo = qFromLittleEndian<quint32>(b.constData() + 20);
        if (blockNo != i)
            return fail("UF2 block number mismatch in block " + QString::number(i));

        const quint32 payloadSize = qFromLittleEndian<quint32>(b.constData() + 16);
        if (payloadSize == 0 || payloadSize > 476)
            return fail("UF2 payload size out of range in block " + QString::number(i));

        const quint32 bFamily = qFromLittleEndian<quint32>(b.constData() + 28);
        if (bFamily != 0xE48BFF56u)
            return fail("UF2 family ID mismatch in block " + QString::number(i));
    }
    return true;
}
