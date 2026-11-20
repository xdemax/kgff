#include "CoreFile.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <QtEndian>

#include <cstring>
#include <functional>

namespace {

// kgo2: docs/kgo2.md в karabas-go-tools и lib/common/kg2_format.h в прошивке.
constexpr int kHeaderSize = 32;
constexpr int kEntrySize = 16;
constexpr int kMaxSections = 64;
constexpr int kMaxTableBytes = kMaxSections * kEntrySize;
constexpr int kCoreSize = 16;
constexpr int kCoreSizeLink = 18;

enum SectionType {
    kSecStrings = 1,
    kSecCore = 2,
    kSecBitstream = 9,
    kSecBitstreamLink2 = 10,
    kSecCount = 11,     // типы 1..10 известны, остальные пропускаются
};

quint16 le16(const char* p) {
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(p));
}

quint32 le32(const char* p) {
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(p));
}

CoreParseResult fail(QString* why, const QString& message) {
    if (why) *why = message;
    return CoreParseResult::Error;
}

struct Section {
    quint32 offset = 0;
    quint32 length = 0;
    bool present = false;
};

}  // namespace

// Общий разбор kgo2: чтение идёт через readAt(offset, length, out). Так один и
// тот же код работает и с полным файлом, и с первыми килобайтами HTTP Range.
using ReadAt = std::function<bool(quint32 offset, quint32 length, QByteArray* out)>;

static CoreParseResult parseCoreImpl(qint64 fileSize, const ReadAt& readAt, int board,
                                     CoreFileInfo* info, QString* why) {
    if (why) why->clear();
    if (!info) return fail(why, QStringLiteral("no result structure"));
    *info = CoreFileInfo();
    info->board = board;

    if (board < 1 || board > 3) return fail(why, QStringLiteral("unsupported board id"));
    if (fileSize < kHeaderSize) return fail(why, QStringLiteral("file is smaller than the kgo2 header"));

    QByteArray header;
    if (!readAt(0, kHeaderSize, &header) || header.size() != kHeaderSize) {
        return fail(why, QStringLiteral("cannot read the kgo2 header"));
    }
    if (std::memcmp(header.constData(), "kgo2", 4) != 0) {
        return fail(why, QStringLiteral("not a kgo2 file (signature mismatch)"));
    }

    const int version = static_cast<unsigned char>(header[4]);
    const quint16 headerSize = le16(header.constData() + 6);
    const quint16 sectionCount = le16(header.constData() + 8);
    const quint16 entrySize = le16(header.constData() + 10);

    if (version < 1) return fail(why, QStringLiteral("unsupported kgo2 version"));
    if (headerSize < kHeaderSize || headerSize > fileSize) {
        return fail(why, QStringLiteral("invalid section table offset"));
    }
    if (entrySize < kEntrySize) return fail(why, QStringLiteral("invalid section entry size"));
    if (sectionCount == 0 || sectionCount > kMaxSections) {
        return fail(why, QStringLiteral("invalid section count"));
    }
    const qint64 tableBytes = static_cast<qint64>(sectionCount) * entrySize;
    if (tableBytes > kMaxTableBytes || tableBytes > fileSize - headerSize) {
        return fail(why, QStringLiteral("truncated section table"));
    }
    QByteArray table;
    if (!readAt(headerSize, static_cast<quint32>(tableBytes), &table) ||
        table.size() != tableBytes) {
        return fail(why, QStringLiteral("cannot read the section table"));
    }

    Section sections[kSecCount];
    const quint16 boardBit = static_cast<quint16>(1u << board);
    for (int i = 0; i < sectionCount; ++i) {
        const char* e = table.constData() + static_cast<qint64>(i) * entrySize;
        const quint16 type = le16(e);
        const quint16 boards = le16(e + 2);
        const quint32 offset = le32(e + 4);
        const quint32 length = le32(e + 8);

        if (static_cast<quint64>(offset) > static_cast<quint64>(fileSize) ||
            static_cast<quint64>(length) > static_cast<quint64>(fileSize) - offset) {
            return fail(why, QStringLiteral("section is outside the file"));
        }
        // Секция другой платы или тип из более новой версии формата.
        if (!(boards & boardBit) || type == 0 || type >= kSecCount) continue;

        Section& s = sections[type];
        if (s.present) return fail(why, QStringLiteral("duplicate section for this board"));
        s.offset = offset;
        s.length = length;
        s.present = true;
    }

    // Ядро может быть только для другой платы: это не ошибка файла.
    if (!sections[kSecCore].present) return CoreParseResult::NotForBoard;
    if (!sections[kSecStrings].present) {
        return fail(why, QStringLiteral("CORE section without a STRINGS section"));
    }
    // Прошивка принимает ядро только с одним битстримом для своей платы.
    if (sections[kSecBitstream].present && sections[kSecBitstreamLink2].present) {
        return fail(why, QStringLiteral("two bitstream sections for this board"));
    }
    if (!sections[kSecBitstream].present && !sections[kSecBitstreamLink2].present) {
        return CoreParseResult::NotForBoard;
    }

    const Section& stringsSec = sections[kSecStrings];
    if (stringsSec.length > 1024u * 1024u) {
        return fail(why, QStringLiteral("STRINGS section is too large"));
    }
    QByteArray strings;
    if (!readAt(stringsSec.offset, stringsSec.length, &strings) ||
        strings.size() != static_cast<int>(stringsSec.length)) {
        return fail(why, QStringLiteral("cannot read STRINGS"));
    }

    const Section& coreSec = sections[kSecCore];
    if (coreSec.length < kCoreSize) return fail(why, QStringLiteral("CORE section is too short"));
    const int coreBytes = coreSec.length >= kCoreSizeLink ? kCoreSizeLink : kCoreSize;
    QByteArray core;
    if (!readAt(coreSec.offset, coreBytes, &core) || core.size() != coreBytes) {
        return fail(why, QStringLiteral("cannot read CORE"));
    }

    const quint16 recordSize = le16(core.constData());
    if (recordSize < kCoreSize || recordSize > coreSec.length) {
        return fail(why, QStringLiteral("invalid CORE record size"));
    }

    auto readString = [&](quint16 ref, QString* out) -> bool {
        if (ref >= strings.size()) return false;
        const int end = strings.indexOf('\0', ref);
        if (end < 0) return false;
        *out = QString::fromUtf8(strings.constData() + ref, end - ref);
        return true;
    };

    if (!readString(le16(core.constData() + 2), &info->id) ||
        !readString(le16(core.constData() + 4), &info->name) ||
        !readString(le16(core.constData() + 6), &info->build)) {
        return fail(why, QStringLiteral("broken string reference in CORE"));
    }
    if (info->id.isEmpty() || info->build.isEmpty()) {
        return fail(why, QStringLiteral("empty core id or build"));
    }

    info->type = static_cast<unsigned char>(core[8]);
    info->visible = static_cast<unsigned char>(core[9]) != 0;
    info->order = static_cast<unsigned char>(core[10]);
    info->link = (recordSize > kCoreSize && core.size() > 16 &&
                  static_cast<unsigned char>(core[16]) == 2)
                     ? 2
                     : 1;
    if (sections[kSecBitstreamLink2].present) info->link = 2;
    return CoreParseResult::Ok;
}

CoreParseResult parseCoreFile(const QString& path, int board, CoreFileInfo* info, QString* why) {
    if (why) why->clear();
    if (!info) return fail(why, QStringLiteral("no result structure"));
    *info = CoreFileInfo();
    info->fileName = QFileInfo(path).fileName();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(why, QStringLiteral("cannot open file: ") + file.errorString());
    }
    const qint64 fileSize = file.size();
    const ReadAt readAt = [&](quint32 offset, quint32 length, QByteArray* out) -> bool {
        if (!file.seek(offset)) return false;
        *out = file.read(length);
        return out->size() == static_cast<int>(length);
    };
    const CoreParseResult result = parseCoreImpl(fileSize, readAt, board, info, why);
    info->fileName = QFileInfo(path).fileName();
    return result;
}

CoreParseResult parseCoreHeaderData(const QByteArray& data, qint64 fileSize, int board,
                                    CoreFileInfo* info, QString* why) {
    if (why) why->clear();
    if (!info) return fail(why, QStringLiteral("no result structure"));
    *info = CoreFileInfo();
    info->board = board;

    if (fileSize <= 0) fileSize = data.size();
    const ReadAt readAt = [&](quint32 offset, quint32 length, QByteArray* out) -> bool {
        const quint64 off = offset;
        const quint64 len = length;
        if (off + len > static_cast<quint64>(data.size())) return false;
        *out = data.mid(static_cast<int>(off), static_cast<int>(len));
        return out->size() == static_cast<int>(length);
    };
    return parseCoreImpl(fileSize, readAt, board, info, why);
}

int compareCoreBuilds(const QString& installed, const QString& downloaded) {
    bool installedIsNumber = false;
    bool downloadedIsNumber = false;
    const qulonglong installedNumber = installed.trimmed().toULongLong(&installedIsNumber);
    const qulonglong downloadedNumber = downloaded.trimmed().toULongLong(&downloadedIsNumber);
    if (installedIsNumber && downloadedIsNumber) {
        if (installedNumber < downloadedNumber) return -1;
        if (installedNumber > downloadedNumber) return 1;
        return 0;
    }
    return installed.compare(downloaded, Qt::CaseInsensitive);
}

QList<CoreFileInfo> parseCoreListResponse(const QByteArray& raw, int board, QString* error) {
    if (error) error->clear();
    QList<CoreFileInfo> cores;

    const QString text = QString::fromUtf8(raw);
    static const QRegularExpression lineSep(QStringLiteral("[\\r\\n]+"));
    const QStringList lines = text.split(lineSep, Qt::SkipEmptyParts);
    bool sawOk = false;

    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) continue;
        if (line.compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0) {
            sawOk = true;
            continue;
        }
        if (line.compare(QStringLiteral("ERROR"), Qt::CaseInsensitive) == 0) {
            if (error) *error = QStringLiteral("device returned ERROR");
            return {};
        }
        if (!line.startsWith(QStringLiteral("+KARABAS:"))) continue;

        const QString payload = line.mid(9);        // длина "+KARABAS:"
        // file;id;name;build. Имя ядра теоретически может содержать ';', поэтому
        // отделяем последнее поле (build) с конца, а не просто делим на 4 части.
        const int first = payload.indexOf(QLatin1Char(';'));
        const int second = first < 0 ? -1 : payload.indexOf(QLatin1Char(';'), first + 1);
        const int last = payload.lastIndexOf(QLatin1Char(';'));
        if (first < 0 || second < 0 || last <= second) {
            if (error) *error = QStringLiteral("malformed core line: ") + line;
            return {};
        }

        CoreFileInfo ci;
        ci.fileName = payload.left(first).trimmed();
        ci.id = payload.mid(first + 1, second - first - 1).trimmed();
        ci.name = payload.mid(second + 1, last - second - 1).trimmed();
        ci.build = payload.mid(last + 1).trimmed();
        ci.board = board;

        if (ci.fileName.isEmpty() || ci.fileName.contains(QLatin1Char('/')) ||
            ci.fileName.contains(QLatin1Char('\\')) || ci.fileName == QLatin1String(".") ||
            ci.fileName == QLatin1String("..") || ci.id.isEmpty() || ci.build.isEmpty()) {
            if (error) *error = QStringLiteral("invalid core line: ") + line;
            return {};
        }
        cores << ci;
    }

    if (!sawOk) {
        if (error) *error = QStringLiteral("no OK in the device response");
        return {};
    }
    return cores;
}
