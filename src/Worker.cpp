#include "Worker.h"

#include "Uf2.h"
#include "Util.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSerialPort>
#include <QSet>
#include <QStorageInfo>
#include <QSerialPortInfo>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <exception>
#include <memory>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

namespace {
const qint64 kMaxTextBytes = 4 * 1024 * 1024;       // JSON / HTML
const qint64 kMaxFileBytes = 64 * 1024 * 1024;      // прошивка / ядро
const qint64 kMaxExtraBytes = 256 * 1024 * 1024;    // extra.7z
const QString kCoresCompleteMarker = QStringLiteral("download_complete.txt");
const QString kExtraCompleteMarker = QStringLiteral("extra_complete.txt");
const QString kExtraUrl = QStringLiteral("https://raw.githubusercontent.com/xdemax/kgff/refs/heads/main/extra/extra.zip");

QNetworkRequest makeRequest(const QUrl& url) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("KarabasFlasher/1.0"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    req.setTransferTimeout(20000);          // таймаут простоя передачи
    return req;
}

// Склейка путей без двойных слешей ("D:/" + "/x" -> "D:/x")
QString joinPath(const QString& dir, const QString& name) {
    return QDir::cleanPath(dir + QLatin1Char('/') + name);
}

// Построчный разбор AT-ответа. Прошивка может печатать отладочные строки, в которых
// встречается слово "error" (например, "no error"), поэтому искать подстроку нельзя:
// OK, ERROR и +KARABAS: распознаются только как отдельные строки целиком.
struct AtReply {
    bool hasKarabas = false;    // есть строка, начинающаяся с "+KARABAS:"
    bool hasOk = false;         // есть строка "OK"
    bool hasError = false;      // есть строка "ERROR"
    QString karabasLine;        // первая строка "+KARABAS:..." (без \r\n)
};

AtReply parseAtLines(const QByteArray& raw) {
    AtReply r;
    const QString text = QString::fromUtf8(raw);
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                         Qt::SkipEmptyParts);
    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QStringLiteral("+KARABAS:"))) {
            if (!r.hasKarabas) r.karabasLine = line;
            r.hasKarabas = true;
        } else if (line.compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0) {
            r.hasOk = true;
        } else if (line.compare(QStringLiteral("ERROR"), Qt::CaseInsensitive) == 0) {
            r.hasError = true;
        }
    }
    return r;
}
}

// ----------------------------------------------------------------------------
// Служебное
// ----------------------------------------------------------------------------
void Worker::guarded(const std::function<void()>& fn) {
    m_permissionDenied = false;
    m_portOpenFailed = false;
    try {
        fn();
    } catch (const std::exception& e) {
        emit log("Error: Unexpected exception: " + QString::fromLocal8Bit(e.what()));
    } catch (...) {
        emit log(QStringLiteral("Error: Unexpected unknown exception."));
    }
    emit finished();
}

bool Worker::pause(int ms) {
    int left = ms;
    while (left > 0) {
        if (m_cancel.load()) return false;
        const int step = std::min(left, 50);
        QThread::msleep(static_cast<unsigned long>(step));
        left -= step;
    }
    return !m_cancel.load();
}

void Worker::runDownload(const Params& p) { guarded([&]() { doDownload(p); }); }
void Worker::runFlash(const Params& p) { guarded([&]() { doFlash(p); }); }

bool Worker::downloadExtraArchive(const QString& destPath) {
    emit log(QStringLiteral("Downloading extra content archive..."));
    QString err;
    if (!downloadToFile(QUrl(kExtraUrl), destPath, &err, kMaxExtraBytes)) {
        emit log(m_cancel.load() ? QStringLiteral("Download cancelled.") : "Download error: " + err);
        return false;
    }
    emit log("Successfully downloaded extra archive (" +
             QString::number(QFileInfo(destPath).size() / 1024) + " KB).");
    return true;
}

bool Worker::extractExtraArchive(const QString& archivePath, const QString& outDir) {
    QDir(outDir).removeRecursively();
    if (!QDir().mkpath(outDir)) {
        emit log("Error: Cannot create folder for extra content: " + outDir);
        return false;
    }

    emit log(QStringLiteral("Extracting extra content..."));

    void* reader = mz_zip_reader_create();
    if (!reader) {
        emit log(QStringLiteral("Error: Cannot create ZIP reader."));
        return false;
    }

    int err = mz_zip_reader_open_file(reader, archivePath.toUtf8().constData());
    if (err != MZ_OK) {
        emit log("Error: Cannot open extra archive (code " + QString::number(err) + ").");
        mz_zip_reader_delete(&reader);
        return false;
    }

    const qint64 hardLimit = 2LL * 1024 * 1024 * 1024;  // 2 ГБ - абсолютный предел
    const qint64 reserve = 64LL * 1024 * 1024;          // запас на служебные нужды
    qint64 limit = hardLimit;
    qint64 total = 0;                                   // заявленные размеры (для оценки)
    qint64 writtenTotal = 0;                            // фактически записанные байты
    int extracted = 0;
    bool failed = false;

    // Проверка свободного места в кэше: распаковка не должна забить диск.
    // Эффективный лимит - min(2 ГБ, свободно - запас): даже при вранье в каталоге
    // архив не сможет выйти за пределы свободного места. Если размер неизвестен
    // (bytesAvailable < 0) - оставляем только абсолютный предел.
    const qint64 cacheFree = QStorageInfo(outDir).bytesAvailable();
    if (cacheFree >= 0) {
        if (cacheFree < reserve) {
            emit log("Error: Not enough free space in the cache folder for extra content (available " +
                     QString::number(cacheFree / 1024) + " KB). Aborting extraction.");
            mz_zip_reader_delete(&reader);
            return false;
        }
        limit = std::min(hardLimit, cacheFree - reserve);
    }

    // Безопасность пути: имя не должно содержать компонент ".." (а не подстроку "..",
    // иначе отсекаются легальные имена вроде "a..b"). Абсолютные пути и обратные слеши
    // тоже запрещены.
    auto isSafeRelPath = [](const QString& rel) {
        if (rel.isEmpty() || rel.startsWith(QLatin1Char('/')) || rel.contains(QLatin1Char('\\')))
            return false;
        const QStringList parts = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString& p : parts) {
            if (p == QLatin1String("..") || p == QLatin1String(".")) return false;
        }
        return true;
    };

    err = mz_zip_reader_goto_first_entry(reader);
    while (err == MZ_OK) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(reader, &info) != MZ_OK || !info) {
            err = mz_zip_reader_goto_next_entry(reader);
            continue;
        }
        const QString rel = QString::fromUtf8(info->filename);
        if (!isSafeRelPath(rel)) {
            emit log("Warning: Skipping unsafe path in archive: " + rel);
            err = mz_zip_reader_goto_next_entry(reader);
            continue;
        }
        const QString dst = QDir::cleanPath(outDir + QLatin1Char('/') + rel);

        // Каталог: имя оканчивается на '/'
        if (rel.endsWith(QLatin1Char('/'))) {
            QDir().mkpath(dst);
            err = mz_zip_reader_goto_next_entry(reader);
            continue;
        }

        total += info->uncompressed_size;
        if (total > limit) {
            emit log("Error: The extra archive expands to more than " +
                     QString::number(limit / (1024 * 1024)) + " MB. Aborting extraction.");
            failed = true;
            break;
        }

        QDir().mkpath(QFileInfo(dst).absolutePath());
        QFile out(dst);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit log("Warning: Cannot create " + rel + ": " + out.errorString());
            failed = true;
            err = mz_zip_reader_goto_next_entry(reader);
            continue;
        }

        err = mz_zip_reader_entry_open(reader);
        if (err != MZ_OK) {
            emit log("Warning: Cannot open entry " + rel + " (code " + QString::number(err) + ").");
            failed = true;
            err = mz_zip_reader_goto_next_entry(reader);
            continue;
        }

        char buf[64 * 1024];
        int32_t n = 0;
        bool writeFailed = false;
        bool limitHit = false;
        while ((n = mz_zip_reader_entry_read(reader, buf, sizeof(buf))) > 0) {
            if (out.write(buf, n) != n) {
                emit log("Warning: Write error for " + rel + ": " + out.errorString());
                writeFailed = true;
                break;
            }
            // Считаем ФАКТИЧЕСКИ записанные байты по всем файлам: заявленный размер
            // из каталога может врать, а распаковка не должна выйти за лимит.
            writtenTotal += n;
            if (writtenTotal > limit) {
                emit log("Error: The extra archive expands to more than " +
                         QString::number(limit / (1024 * 1024)) + " MB. Aborting extraction.");
                limitHit = true;
                break;
            }
        }
        out.close();
        // mz_zip_reader_entry_close возвращает ошибку CRC/распаковки - именно здесь
        // обнаруживается битый или подменённый файл.
        const int closeErr = mz_zip_reader_entry_close(reader);
        if (limitHit) {
            // Превышение лимита - прерываем весь цикл, а не переходим к следующей записи.
            failed = true;
            break;
        }
        if (writeFailed) {
            failed = true;
        } else if (n < 0) {
            emit log("Warning: Read error for " + rel + " (code " + QString::number(n) + ").");
            failed = true;
        } else if (closeErr != MZ_OK) {
            emit log("Warning: CRC/close error for " + rel + " (code " + QString::number(closeErr) + ").");
            failed = true;
        }
        ++extracted;
        err = mz_zip_reader_goto_next_entry(reader);
    }

    // Конец списка - MZ_END_OF_LIST. Любая другая ошибка означает повреждённый каталог.
    if (err != MZ_END_OF_LIST && !failed) {
        emit log("Warning: ZIP central directory is damaged (code " + QString::number(err) + ").");
        failed = true;
    }

    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);

    if (extracted == 0) {
        emit log(QStringLiteral("Error: The extra archive is empty or corrupted."));
        return false;
    }
    if (failed) {
        emit log(QStringLiteral("Error: Some files could not be extracted from the extra archive."));
        return false;
    }
    emit log(QStringLiteral("Extra content extracted successfully."));
    return true;
}

bool Worker::copyExtraToSd(const QString& srcDir, const QString& sdRoot) {
    emit log(QStringLiteral("Copying extra content to SD card..."));
    if (!QFileInfo(srcDir).isDir()) {
        emit log("Error: Extra content folder does not exist: " + srcDir);
        return false;
    }
    int copied = 0;
    int failed = 0;

    // Если в архиве есть верхний каталог "extra/", его содержимое копируется в корень
    // SD-карты (без вложенной папки extra). Остальные элементы корня копируются как есть.
    const QString extraSubdir = srcDir + QStringLiteral("/extra");
    // Симлинк на папку не считаем папкой: иначе итератор пойдёт по его цели
    // и вытащит произвольный локальный каталог (актуально для недоверенного архива).
    const QFileInfo extraSubdirInfo(extraSubdir);
    const bool hasExtraSubdir = extraSubdirInfo.isDir() && !extraSubdirInfo.isSymLink();
    if (hasExtraSubdir) {
        emit log(QStringLiteral("Extra archive has a top-level 'extra' folder; its contents will be "
                                "copied to the SD card root."));
    }

    auto copyOne = [&](const QString& src, const QString& rel) -> bool {
        const QString dst = QDir::cleanPath(sdRoot + QLatin1Char('/') + rel);
        const QString dstDir = QFileInfo(dst).absolutePath();
        if (!QDir().mkpath(dstDir)) {
            emit log("Warning: Cannot create folder " + dstDir);
            ++failed;
            return false;
        }
        // Файлы extra копируются с заменой существующих. Бэкап не делается:
        // в папках extra у пользователя могут быть свои файлы, и складывать их
        // в backup_* рядом с ядрами не нужно. Файлы, которых нет в архиве,
        // не трогаются вообще.
        QString e;
        if (!copyFileSynced(src, dst, &e)) {
            emit log("Warning: Failed to copy " + rel + ": " + e);
            ++failed;
            return false;
        }
        ++copied;
        return true;
    };

    // Проверка свободного места: суммарный размер файлов extra + запас 1 МБ.
    // Считаем только файлы, которые будут скопированы (без папки extra/, если она есть).
    qint64 needed = 1024 * 1024;
    auto addSize = [&](const QString& dir) {
        QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            needed += it.fileInfo().size();
        }
    };
    if (hasExtraSubdir) addSize(extraSubdir);
    const QFileInfoList topForSize = QDir(srcDir).entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& fi : topForSize) {
        if (hasExtraSubdir && fi.isDir() &&
            fi.fileName().compare(QStringLiteral("extra"), Qt::CaseInsensitive) == 0) {
            continue;
        }
        // Симлинк на папку не раскрываем: иначе оценка размера пойдёт по цели ссылки
        // и может дать ложное "мало места". Такие ссылки всё равно не копируются.
        if (fi.isSymLink()) continue;
        if (fi.isDir()) addSize(fi.absoluteFilePath());
        else needed += fi.size();
    }
    const qint64 available = QStorageInfo(sdRoot).bytesAvailable();
    if (available >= 0 && available < needed) {
        emit log("Error: Not enough free space on the SD card for extra content (need " +
                 QString::number(needed / 1024) + " KB, available " +
                 QString::number(available / 1024) + " KB). Aborting extra copy.");
        return false;
    }

    // 1) Содержимое верхней папки "extra/" -> в корень SD
    // QDir::NoSymLinks: симлинки из архива не копируем - иначе можно вытащить
    // произвольный локальный файл по ссылке (актуально для недоверенного архива).
    if (hasExtraSubdir) {
        QDirIterator it(extraSubdir, QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString src = it.next();
            const QString rel = QDir(extraSubdir).relativeFilePath(src);
            copyOne(src, rel);
        }
    }

    // 2) Остальные элементы корня распаковки (кроме самой папки "extra") -> как есть
    const QFileInfoList top = QDir(srcDir).entryInfoList(
        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& fi : top) {
        if (hasExtraSubdir && fi.isDir() &&
            fi.fileName().compare(QStringLiteral("extra"), Qt::CaseInsensitive) == 0) {
            continue;       // уже обработано выше
        }
        // Симлинк на папку не раскрываем: isSymLink() проверяется ДО isDir(),
        // иначе итератор пойдёт по цели ссылки.
        if (fi.isSymLink()) {
            emit log("Warning: Skipping symbolic link in extra archive: " + fi.fileName());
            continue;
        }
        if (fi.isDir()) {
            QDirIterator it(fi.absoluteFilePath(), QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString src = it.next();
                const QString rel = QDir(srcDir).relativeFilePath(src);
                copyOne(src, rel);
            }
        } else {
            copyOne(fi.absoluteFilePath(), fi.fileName());
        }
    }
    emit log("Extra content copied: " + QString::number(copied) + " files" +
             (failed > 0 ? ", " + QString::number(failed) + " failed" : QString()) + ".");
    if (copied == 0) {
        emit log(QStringLiteral("Error: The extra content folder is empty; nothing was copied."));
        return false;
    }
    if (failed > 0) {
        emit log("Error: Some extra content files could not be copied. The SD card may be incomplete.");
    }
    return failed == 0;
}

void Worker::runMsc(const Params& p) { guarded([&]() { doOpenSd(p); }); }

void Worker::runDetectModel(const Params& p) {
    guarded([&]() {
        if (p.portLocation.isEmpty()) {
            emit log(QStringLiteral("Error: Please select an active serial port first."));
            return;
        }
        QString raw;
        int board = 0;
        if (!queryVersion(p.portLocation, &raw, &board)) return;

        emit log("Device version: " + raw);
        int modelIndex = -1;
        switch (board) {
        case 1: modelIndex = 0; break;      // Karabas Go
        case 2: modelIndex = 1; break;      // Karabas Go Mini rev A-F
        case 3: modelIndex = 2; break;      // Karabas Go Mini rev G
        default: break;
        }
        if (modelIndex < 0) {
            emit log("Warning: Unknown board id " + QString::number(board) +
                     " in the version response. Model was not changed.");
            return;
        }
        emit log("Detected model: " + models().at(modelIndex).name);
        emit modelDetected(modelIndex, raw);
    });
}

// Кнопка «Open SD card in explorer»: режим MSC, ожидание диска, открытие папки карты.
void Worker::doOpenSd(const Params& p) {
    // Если смонтирован диск с меткой Карабаса - устройство уже в MSC-режиме.
    // Проверяем ВСЕ смонтированные USB-диски, а не только новые: диск Карабаса
    // уже был в снимке на момент повторного нажатия.
    QStringList where;
    for (const Platform::Candidate& c : Platform::newDisks(QSet<QString>())) {
        if (!isKarabasVolume(c.mountPoint)) continue;
        where << (c.mountPoint.isEmpty() ? c.node : c.mountPoint) +
                     (c.label.isEmpty() ? QString() : " [" + c.label + "]");
    }
    if (!where.isEmpty()) {
        emit log(QStringLiteral("Warning: The device is already in MSC mode (drive %1 is mounted). "
                                "To leave it, safely eject the SD card drive from your system "
                                "(or unplug the USB cable).").arg(where.join(", ")));
        return;
    }

    if (p.portLocation.isEmpty()) {
        emit log(QStringLiteral("Warning: No serial port selected. The device may already be in MSC mode "
                                "(the serial port disappears in this mode). To leave it, safely eject the "
                                "SD card drive from your system (or unplug the USB cable)."));
        return;
    }

    const QSet<QString> before = Platform::readyRemovableDisks();   // снимок до команды
    if (!sendMscCommand(p.portLocation, p.mscMode)) return;

    emit log(QStringLiteral("Waiting for the SD card to appear as a drive..."));
    Platform::Candidate sd;
    if (!waitForMountedVolume([&before]() { return Platform::newDisks(before); }, 40000, &sd)) {
        if (m_cancel.load()) {
            emit log(QStringLiteral("Operation cancelled by user."));
        } else {
            emit log(QStringLiteral("Error: SD card drive not detected after switching mode."));
            emit log(Platform::diagnostics());
        }
        return;
    }

    const QString labelPart = sd.label.isEmpty() ? QString() : " [" + sd.label + "]";
    emit log("Success: The SD card is available at " + sd.mountPoint + labelPart +
             ". Opening it in the file manager...");
    emit openFolder(sd.mountPoint);
    emit log(QStringLiteral("To leave this mode, safely eject the memory card from your system."));
    emit portsChanged();        // порт исчез - обновим список в GUI
}

// ----------------------------------------------------------------------------
// Сеть (QNetworkAccessManager, синхронно внутри рабочего потока)
// ----------------------------------------------------------------------------
Worker::HttpResult Worker::httpFetch(const QUrl& url, qint64 maxBytes, QSaveFile* file) {
    HttpResult r;
    QNetworkAccessManager nam;
    const QNetworkRequest req = makeRequest(url);

    QNetworkReply* reply = nam.get(req);   // reply принадлежит nam и удалится вместе с ним

    qint64 total = 0;
    bool tooBig = false;
    bool writeFailed = false;

    auto drain = [&]() {
        if (tooBig || writeFailed) return;
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty()) return;
        total += chunk.size();
        if (total > maxBytes) {
            tooBig = true;
            reply->abort();
            return;
        }
        if (file) {
            if (file->write(chunk) != chunk.size()) {
                writeFailed = true;
                reply->abort();
            }
        } else {
            r.body.append(chunk);
        }
    };

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, drain);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    QTimer cancelPoll;
    cancelPoll.setInterval(100);
    QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&]() {
        if (m_cancel.load()) reply->abort();
    });
    cancelPoll.start();

    loop.exec();
    cancelPoll.stop();
    drain();

    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (m_cancel.load()) {
        r.error = QStringLiteral("cancelled");
    } else if (tooBig) {
        r.error = QStringLiteral("response is too large");
    } else if (writeFailed) {
        r.error = QStringLiteral("file write error");
    } else if (reply->error() != QNetworkReply::NoError) {
        r.error = reply->errorString();
        if (r.status != 0) r.error += " (HTTP " + QString::number(r.status) + ")";
    } else if (r.status != 200) {
        r.error = "HTTP status " + QString::number(r.status);
    } else {
        r.ok = true;
    }
    return r;
}

bool Worker::downloadToFile(const QUrl& url, const QString& dest, QString* err, qint64 maxBytes) {
    QSaveFile f(dest);                       // запись во временный файл + атомарная замена
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = "cannot create file: " + f.errorString();
        return false;
    }
    const HttpResult r = httpFetch(url, maxBytes, &f);
    if (!r.ok) {
        f.cancelWriting();
        if (err) *err = r.error;
        return false;
    }
    if (!f.commit()) {
        if (err) *err = "cannot save file: " + f.errorString();
        return false;
    }
    return true;
}

QList<Worker::JobResult> Worker::downloadFiles(const QList<DownloadJob>& jobs, int maxParallel) {
    QList<JobResult> results;
    for (int i = 0; i < jobs.size(); ++i) {
        JobResult r;
        r.error = QStringLiteral("cancelled");
        results.append(r);
    }

    struct Active {
        int index = -1;
        QSaveFile* file = nullptr;
        QElapsedTimer timer;
        qint64 bytes = 0;
        bool tooBig = false;
        bool writeFailed = false;
        int attempt = 0;                // номер попытки (0 - первая)
    };

    QNetworkAccessManager nam;      // один на все файлы: соединение и TLS-сессия переиспользуются
    QEventLoop loop;
    QHash<QNetworkReply*, Active> active;
    int next = 0;

    // Забрать накопленные данные ответа и записать в файл.
    // abort() вызывает finished синхронно, поэтому после него Active не трогаем.
    auto drain = [&](QNetworkReply* reply) {
        auto it = active.find(reply);
        if (it == active.end()) return;
        Active& a = it.value();
        if (a.tooBig || a.writeFailed) return;
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty()) return;
        a.bytes += chunk.size();
        if (a.bytes > kMaxFileBytes) {
            a.tooBig = true;
            reply->abort();
            return;
        }
        if (a.file->write(chunk) != chunk.size()) {
            a.writeFailed = true;
            reply->abort();
        }
    };

    auto maybeQuit = [&]() {
        if (active.isEmpty() && (next >= jobs.size() || m_cancel.load())) loop.quit();
    };

    std::function<void()> startMore;
    std::function<void(QNetworkReply*)> finish;

    finish = [&](QNetworkReply* reply) {
        drain(reply);
        auto it = active.find(reply);
        if (it == active.end()) return;
        const Active a = it.value();
        active.erase(it);

        JobResult& r = results[a.index];
        r.bytes = a.bytes;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        QString err;
        if (m_cancel.load()) {
            err = QStringLiteral("cancelled");
        } else if (a.tooBig) {
            err = QStringLiteral("file is too large");
        } else if (a.writeFailed) {
            err = QStringLiteral("file write error");
        } else if (reply->error() != QNetworkReply::NoError) {
            err = reply->errorString();
            if (status != 0) err += " (HTTP " + QString::number(status) + ")";
        } else if (status != 200) {
            err = "HTTP status " + QString::number(status);
        }

        if (!err.isEmpty()) {
            a.file->cancelWriting();
            r.error = err;
        } else if (!a.file->commit()) {
            r.error = "cannot save file: " + a.file->errorString();
        } else {
            r.ok = true;
            r.error.clear();
        }
        delete a.file;
        reply->deleteLater();

        const QString& name = jobs[a.index].name;
        if (r.ok) {
            emit log("Downloaded core: " + name + " (" + QString::number(a.bytes / 1024) + " KB, " +
                     QString::number(a.timer.elapsed() / 1000.0, 'f', 1) + " s)");
        } else if (!m_cancel.load()) {
            // Одна повторная попытка на файл: сетевые сбои обычно временные
            if (a.attempt < 1) {
                emit log("Retrying core: " + name + " (" + r.error + ")");
                results[a.index] = JobResult();     // сброс результата
                results[a.index].error = QStringLiteral("cancelled");
                // Запускаем повторную загрузку этого файла напрямую (без изменения next)
                std::unique_ptr<QSaveFile> file(new QSaveFile(jobs[a.index].dest));
                if (file->open(QIODevice::WriteOnly)) {
                    QNetworkReply* retry = nam.get(makeRequest(jobs[a.index].url));
                    Active ra;
                    ra.index = a.index;
                    ra.file = file.release();
                    ra.timer.start();
                    ra.attempt = a.attempt + 1;
                    active.insert(retry, ra);
                    QObject::connect(retry, &QNetworkReply::readyRead, &loop, [&, retry]() { drain(retry); });
                    QObject::connect(retry, &QNetworkReply::finished, &loop, [&, retry]() { finish(retry); });
                } else {
                    emit log("Failed to download core: " + name + " (cannot create file: " +
                             file->errorString() + ")");
                }
            } else {
                emit log("Failed to download core: " + name + " (" + r.error + ")");
            }
        }

        startMore();
        maybeQuit();
    };

    startMore = [&]() {
        while (!m_cancel.load() && static_cast<int>(active.size()) < maxParallel && next < jobs.size()) {
            const int i = next++;
            std::unique_ptr<QSaveFile> file(new QSaveFile(jobs[i].dest));
            if (!file->open(QIODevice::WriteOnly)) {
                results[i].error = "cannot create file: " + file->errorString();
                emit log("Failed to download core: " + jobs[i].name + " (" + results[i].error + ")");
                continue;
            }
            QNetworkReply* reply = nam.get(makeRequest(jobs[i].url));
            Active a;
            a.index = i;
            a.file = file.release();
            a.timer.start();
            active.insert(reply, a);
            QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&, reply]() { drain(reply); });
            QObject::connect(reply, &QNetworkReply::finished, &loop, [&, reply]() { finish(reply); });
        }
    };

    QTimer cancelPoll;
    cancelPoll.setInterval(100);
    QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&]() {
        if (!m_cancel.load()) return;
        const QList<QNetworkReply*> replies = active.keys();
        for (QNetworkReply* r : replies) r->abort();       // finish() вызывается для каждого
        maybeQuit();
    });

    startMore();
    if (!active.isEmpty()) {            // loop.quit() до exec() не сработал бы
        cancelPoll.start();
        loop.exec();
        cancelPoll.stop();
    }
    return results;
}

QStringList Worker::fetchCoresList(const ModelInfo& m) {
    QStringList cores;
    auto add = [&](const QString& name) {
        if (!isSafeFileName(name)) return;                       // защита от "../" и т.п.
        if (!name.endsWith(m.ext, Qt::CaseInsensitive)) return;
        if (!cores.contains(name)) cores << name;
    };

    const HttpResult api = httpFetch(
        QUrl("https://api.github.com/repos/andykarpov/" + m.repo + "/contents/cores"), kMaxTextBytes, nullptr);
    if (api.ok) {
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(api.body, &pe);
        if (pe.error == QJsonParseError::NoError && doc.isArray()) {
            const QJsonArray arr = doc.array();
            for (const QJsonValue& v : arr) add(v.toObject().value(QStringLiteral("name")).toString());
        }
    } else if (!m_cancel.load()) {
        emit log("Warning: GitHub API request failed (" + api.error + "). Trying HTML page...");
    }

    if (cores.isEmpty() && !m_cancel.load()) {
        const HttpResult html = httpFetch(
            QUrl("https://github.com/andykarpov/" + m.repo + "/tree/master/cores"), kMaxTextBytes, nullptr);
        if (html.ok) {
            // ".kg1" не должно совпадать с началом ".kg10" - отсекаем продолжение имени
            const QRegularExpression re(
                "([A-Za-z0-9_\\-.]+" + QRegularExpression::escape(m.ext) + ")(?![A-Za-z0-9_\\-.])",
                QRegularExpression::CaseInsensitiveOption);
            const QString text = QString::fromUtf8(html.body);
            QRegularExpressionMatchIterator it = re.globalMatch(text);
            while (it.hasNext()) add(it.next().captured(1));
        } else if (!m_cancel.load()) {
            emit log("Error: Failed to connect to GitHub repository '" + m.repo + "' (" + html.error + ").");
        }
    }
    return cores;
}

// ----------------------------------------------------------------------------
// Скачивание
// ----------------------------------------------------------------------------
void Worker::doDownload(const Params& p) {
    if (p.modelIndex < 0 || p.modelIndex >= models().size()) {
        emit log(QStringLiteral("Error: Unknown device model selected."));
        return;
    }
    const ModelInfo& m = models().at(p.modelIndex);

    if (!p.updateFirmware && !p.updateCores && !p.updateExtra) {
        emit log(QStringLiteral("Nothing to download: check 'Update firmware', 'Update cores' and/or "
                                "'Download extra content'."));
        return;
    }

    const QString wd = workDir();
    if (!QDir().mkpath(wd)) {
        emit log("Error: Cannot create working directory: " + wd);
        return;
    }
    const QString destFile = wd + QStringLiteral("/firmware_temp.uf2");
    const QString coresDir = wd + QStringLiteral("/cores");

    emit log("Starting download for model: " + m.name);
    if (p.updateFirmware) {
        emit log("URL: " + m.firmwareUrl);

        QString err;
        QElapsedTimer fwTimer;
        fwTimer.start();
        if (!downloadToFile(QUrl(m.firmwareUrl), destFile, &err)) {
            emit log(m_cancel.load() ? QStringLiteral("Download cancelled.") : "Download error: " + err);
            return;
        }
        QString why;
        if (!isValidUf2(destFile, &why)) {
            emit log("Error: Downloaded file is not a valid UF2 image (" + why + "). It was discarded.");
            QFile::remove(destFile);
            return;
        }
        emit firmwareReady(destFile);
        emit log("Successfully downloaded firmware (" + QString::number(QFileInfo(destFile).size() / 1024) + " KB, " +
                 QString::number(fwTimer.elapsed() / 1000.0, 'f', 1) + " s). Path: " + destFile);
    } else {
        emit log(QStringLiteral("Skipping firmware download because 'Update firmware' is unchecked."));
    }

    if (p.updateExtra) {
        const QString extraDir = wd + QStringLiteral("/extra");
        if (!QDir().mkpath(extraDir)) {
            emit log("Error: Cannot create folder for extra content: " + extraDir);
            // Не прерываем: прошивка и ядра важнее, extra можно повторить отдельно.
        } else {
            // Удаляем старый маркер ДО скачивания: если новая распаковка упадёт посередине,
            // FLASH не должен принять частично распакованный архив за готовый набор.
            QFile::remove(extraDir + "/" + kExtraCompleteMarker);
            const QString archive = extraDir + QStringLiteral("/extra.zip");
            const QString filesDir = extraDir + QStringLiteral("/files");
            // Сбой extra не должен прерывать скачивание ядер и прошивку: сообщаем
            // об ошибке и идём дальше. Итоговая сводка будет в конце.
            if (!downloadExtraArchive(archive)) {
                emit log(QStringLiteral("Warning: Extra content download failed; continuing with the rest."));
            } else if (!extractExtraArchive(archive, filesDir)) {
                emit log(QStringLiteral("Warning: Extra content extraction failed; continuing with the rest."));
            } else {
                QFile marker(extraDir + "/" + kExtraCompleteMarker);
                if (!marker.open(QIODevice::WriteOnly)) {
                    emit log("Warning: Could not create the extra completion marker (" + marker.errorString() + ").");
                }
            }
        }
    } else {
        emit log(QStringLiteral("Skipping extra content download because 'Download extra content' is unchecked."));
    }

    if (!p.updateCores) {
        emit log(QStringLiteral("Skipping cores download because 'Update cores' is unchecked."));
        // Итоговая сводка: что скачалось, а что нет.
        emit log(QStringLiteral("Download summary: firmware=%1, extra=%2, cores=skipped.")
                    .arg(p.updateFirmware ? QStringLiteral("done") : QStringLiteral("skipped"),
                         p.updateExtra
                             ? (QFile::exists(wd + QStringLiteral("/extra/") + kExtraCompleteMarker)
                                    ? QStringLiteral("done") : QStringLiteral("failed"))
                             : QStringLiteral("skipped")));
        return;
    }

    emit log(QStringLiteral("Fetching actual cores list from GitHub repository..."));
    QElapsedTimer listTimer;
    listTimer.start();
    const QStringList cores = fetchCoresList(m);
    emit log("Cores list request took " + QString::number(listTimer.elapsed() / 1000.0, 'f', 1) + " s.");
    if (m_cancel.load()) {
        emit log(QStringLiteral("Operation cancelled by user."));
        return;
    }
    if (cores.isEmpty()) {
        emit log(QStringLiteral("Warning: Could not parse cores list or directory is empty for target filter."));
        return;
    }
    emit log("Found " + QString::number(cores.size()) + " core files (" + m.ext + "). Starting download...");

    // Старые ядра в рабочей папке удаляем, чтобы не раскатать устаревшие файлы
    QDir(coresDir).removeRecursively();
    if (!QDir().mkpath(coresDir)) {
        emit log(QStringLiteral("Error: Cannot create temp folder for cores."));
        return;
    }

    const QString baseUrl = "https://raw.githubusercontent.com/andykarpov/" + m.repo + "/master/cores/";
    QList<DownloadJob> jobs;
    for (const QString& core : cores) {
        DownloadJob j;
        j.url = QUrl(baseUrl + QString::fromLatin1(QUrl::toPercentEncoding(core)));
        j.dest = coresDir + "/" + core;
        j.name = core;
        jobs.append(j);
    }

    QElapsedTimer coresTimer;
    coresTimer.start();
    const QList<JobResult> results = downloadFiles(jobs, 4);
    if (m_cancel.load()) {
        emit log(QStringLiteral("Operation cancelled by user."));
        return;
    }

    int okCount = 0;
    qint64 totalBytes = 0;
    for (const JobResult& r : results) {
        if (r.ok) {
            ++okCount;
            totalBytes += r.bytes;
        }
    }
    const double secs = coresTimer.elapsed() / 1000.0;
    const qint64 speedKb = secs > 0.0 ? static_cast<qint64>(totalBytes / 1024.0 / secs) : 0;
    emit log("Cores download completed. Successfully updated: " + QString::number(okCount) + "/" +
             QString::number(cores.size()) + " (" + QString::number(totalBytes / 1024) + " KB in " +
             QString::number(secs, 'f', 1) + " s, " + QString::number(speedKb) + " KB/s)");

    if (okCount == cores.size()) {
        // Маркер полноты набора: прошивка с обновлением ядер без него не запустится
        QFile marker(coresDir + "/" + kCoresCompleteMarker);
        if (!marker.open(QIODevice::WriteOnly)) {
            emit log("Warning: Could not create the completion marker (" + marker.errorString() +
                     "). Flashing with 'Update cores' will be refused.");
        }
    } else {
        emit log(QStringLiteral("Warning: Cores set is incomplete. Flashing with 'Update cores' will be "
                                "refused until all cores download."));
    }

    // Итоговая сводка: что скачалось, а что нет.
    emit log(QStringLiteral("Download summary: firmware=%1, extra=%2, cores=%3.")
                .arg(p.updateFirmware ? QStringLiteral("done") : QStringLiteral("skipped"),
                     p.updateExtra
                         ? (QFile::exists(wd + QStringLiteral("/extra/") + kExtraCompleteMarker)
                                ? QStringLiteral("done") : QStringLiteral("failed"))
                         : QStringLiteral("skipped"),
                     okCount == cores.size()
                         ? QStringLiteral("done")
                         : QStringLiteral("incomplete (%1/%2)").arg(okCount).arg(cores.size())));
}

// ----------------------------------------------------------------------------
// Последовательный порт
// ----------------------------------------------------------------------------
bool Worker::isPortPresent(const QString& portLocation) {
    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo& i : ports) {
        // На Windows systemLocation() возвращает "COM3", но на некоторых версиях Qt
        // может вернуть "\\.\COM3" - сравниваем оба варианта.
        if (i.systemLocation() == portLocation || i.portName() == portLocation) return true;
    }
    return false;
}

// Универсальная процедура обмена AT-командой по последовательному порту.
// Открывает порт, пишет команду, читает ответ до появления маркера "+KARABAS:"
// (для version) или до истечения таймаута (для msc). DTR/RTS поднимаются явно:
// RP2040 (TinyUSB CDC) передаёт данные только при поднятом DTR. clear() не вызывается -
// на Windows он сбивает USB-CDC устройство.
bool Worker::sendAtCommand(const QString& portLocation, const QByteArray& cmd,
                           QByteArray* resp, int timeoutMs) {
    if (resp) resp->clear();

    QSerialPort sp;
    sp.setPortName(portLocation);
    sp.setBaudRate(QSerialPort::Baud115200);
    sp.setDataBits(QSerialPort::Data8);
    sp.setParity(QSerialPort::NoParity);
    sp.setStopBits(QSerialPort::OneStop);
    sp.setFlowControl(QSerialPort::NoFlowControl);

    if (!sp.open(QIODevice::ReadWrite)) {
        QString msg = "Error: Failed to open " + portLocation + ": " + sp.errorString() + ".";
#if defined(Q_OS_LINUX)
        if (sp.error() == QSerialPort::PermissionError) {
            msg += " No permission to access the port.";
            m_permissionDenied = true;
        } else {
            msg += " Make sure it is not used by another app (e.g. ModemManager).";
        }
#else
        msg += " Make sure the port is not used by another application.";
#endif
        emit log(msg);
        m_portOpenFailed = true;
        if (m_permissionDenied) {
            emit portAccessDenied(portLocation);
        }
        return false;
    }

    // RP2040 (TinyUSB CDC) передаёт данные только при поднятом DTR - как терминал.
    // На Linux/macOS драйвер обычно поднимает линии сам, но явное выставление
    // безвредно и делает поведение одинаковым на всех платформах.
    sp.setDataTerminalReady(true);
    sp.setRequestToSend(true);
    QThread::msleep(300);       // дать прошивке заметить подключение
    emit log(QStringLiteral("Diagnostics: port opened, DTR=%1 RTS=%2.")
                .arg(sp.isDataTerminalReady() ? QStringLiteral("on") : QStringLiteral("off"))
                .arg(sp.isRequestToSend() ? QStringLiteral("on") : QStringLiteral("off")));

    if (sp.write(cmd) != cmd.size() || !sp.waitForBytesWritten(2000)) {
        emit log(QStringLiteral("Error: Failed to write data to the serial port."));
        m_portOpenFailed = true;
        return false;
    }

    QByteArray buf;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (m_cancel.load()) {
            emit log(QStringLiteral("Operation cancelled by user."));
            return false;
        }
        // waitForReadyRead() может вернуть false, хотя данные уже в буфере
        // (особенно на Windows), поэтому проверяем bytesAvailable() отдельно.
        sp.waitForReadyRead(100);
        if (sp.bytesAvailable() > 0) buf += sp.readAll();
        // Успех (+KARABAS:) проверяем РАНЬШЕ ошибки: отладочная строка прошивки
        // со словом "error" не должна ломать разбор version.
        const AtReply reply = parseAtLines(buf);
        if (reply.hasKarabas) {
            // Дочитываем остаток строки, но не дольше 500 мс и с проверкой отмены:
            // поток отладки от устройства не должен зациклить чтение.
            QElapsedTimer drain;
            drain.start();
            while (drain.elapsed() < 500 && !m_cancel.load()) {
                if (!sp.waitForReadyRead(50)) break;
                buf += sp.readAll();
            }
            break;
        }
        // Обрываем чтение только на отдельной строке ERROR или OK. Подстроку "error"
        // не ищем: отладочные строки вроде "no error" не должны прерывать обмен.
        if (reply.hasError || reply.hasOk) break;
    }
    // Диагностика: сколько байт реально пришло и что именно (для отладки проблем
    // с USB-CDC на Windows). Если buf пуст - устройство не ответило вообще.
    if (buf.isEmpty()) {
        emit log(QStringLiteral("Diagnostics: no bytes received from the device (port open, command written)."));
    } else {
        emit log(QStringLiteral("Diagnostics: received %1 bytes: %2")
                    .arg(buf.size())
                    .arg(QString::fromUtf8(buf.left(200)).trimmed()));
    }
    if (resp) *resp = buf;
    return !buf.isEmpty();
}

bool Worker::sendMscCommand(const QString& portLocation, int mscMode) {
    const QByteArray cmd = (mscMode == 2) ? QByteArray("AT+KARABAS=msc2\r\n")
                                          : QByteArray("AT+KARABAS=msc\r\n");
    emit log("Sending " + QString::fromLatin1(cmd).trimmed() + " command to " + portLocation +
             " (MSC mode " + QString::number(mscMode) + ")...");

    QByteArray resp;
    if (!sendAtCommand(portLocation, cmd, &resp, 1000)) {
        // sendAtCommand возвращает false в трёх случаях:
        //   1) порт не открылся (нет прав, занят) - это ошибка, ждать диск бессмысленно;
        //   2) запись не удалась - тоже ошибка;
        //   3) ответа нет - это нормально: устройство могло уже перезагрузиться в MSC.
        // (1) и (2) помечаются флагом m_portOpenFailed (не зависит от платформы),
        // (3) - это пустой ответ без ошибки открытия/записи.
        if (m_portOpenFailed) {
            emit log(QStringLiteral("Error: The MSC command could not be sent (port open/write failed)."));
            return false;
        }
        // Пустой ответ без ошибки открытия/записи - устройство уже перезагрузилось в MSC.
        return true;
    }
    const AtReply reply = parseAtLines(resp);
    const QString text = QString::fromUtf8(resp).trimmed();
    if (reply.hasError) {
        emit log("Error: The device rejected the MSC command: " + text +
                 ". Check that the firmware supports this command and the MSC mode number.");
        return false;
    }
    if (!text.isEmpty()) {
        emit log("Device response: " + text);
    }
    emit log(QStringLiteral("Command sent successfully. Waiting for device reboot into MSC..."));
    return true;
}

// Запрос версии: AT+KARABAS=version -> +KARABAS:<firmware>;<board>;<core id>;<core build>
bool Worker::queryVersion(const QString& portLocation, QString* raw, int* board) {
    emit log("Sending AT+KARABAS=version command to " + portLocation + "...");
    m_permissionDenied = false;

    const QByteArray cmd("AT+KARABAS=version\r\n");
    QByteArray resp;
    // До трёх попыток: первая может потеряться, если устройство ещё не готово
    // сразу после открытия порта (типично для Windows). Между попытками - пауза,
    // чтобы устройство успело "подняться" после открытия порта.
    for (int attempt = 0; attempt < 3 && !resp.contains("+KARABAS:"); ++attempt) {
        if (attempt > 0) {
            emit log(QStringLiteral("No response yet, retrying version request..."));
            QThread::msleep(500);
        }
        if (!sendAtCommand(portLocation, cmd, &resp, 4000)) {
            // sendAtCommand уже залогировал ошибку открытия/записи/отмены.
            // Если порт не открылся или запись не удалась - повторять бессмысленно
            // (и это не должно повторно показывать диалог udev).
            if (m_cancel.load() || m_portOpenFailed) return false;
        }
    }

    const AtReply reply = parseAtLines(resp);
    const QString text = QString::fromUtf8(resp).trimmed();
    if (text.isEmpty()) {
        emit log(QStringLiteral("Error: No response from the device to AT+KARABAS=version."));
        return false;
    }
    // Успех (+KARABAS:) уже найден в цикле выше, поэтому сюда попадаем только если
    // его нет. Отдельная строка ERROR означает отказ прошивки.
    if (reply.hasError) {
        emit log("Error: The device rejected the version command: " + text);
        return false;
    }

    static const QRegularExpression re(QStringLiteral("\\+KARABAS:([^;\\r\\n]*);(\\d+);([^;\\r\\n]*);([^\\r\\n]*)"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) {
        emit log("Error: Unexpected version response: " + text);
        return false;
    }
    if (raw) *raw = m.captured(0).trimmed();
    if (board) *board = m.captured(2).toInt();
    return true;
}

// Сброс 1200 bps для перехода в режим RP2040 bootloader.
// После MSC-сеанса устройство перечисляется заново, и Windows может ещё несколько
// секунд отдавать ERROR_GEN_FAILURE ("Присоединенное к системе устройство не работает")
// при открытии порта. Поэтому делаем несколько попыток с паузой.
bool Worker::resetPortToBootloader(const QString& portLocation) {
    if (portLocation.isEmpty()) return false;
    emit log("Opening port " + portLocation + " to send reboot command to bootloader...");

    // После MSC-сеанса Windows перечисляет USB-устройство заново: порт появляется
    // в списке раньше, чем становится реально открываемым (ERROR_GEN_FAILURE).
    // Повторяем только временные ошибки (OpenError/ResourceError) и только пока порт
    // виден в списке. Если порта нет вовсе (плата уже в BOOTSEL или отключена) -
    // выходим сразу, не тратя 30 секунд.
    const int kAttempts = 30;
    QString lastError;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (m_cancel.load()) {
            emit log(QStringLiteral("Operation cancelled by user."));
            return false;
        }
        if (attempt > 0) {
            if (!isPortPresent(portLocation)) {
                m_portOpenFailed = true;
                emit log("Port " + portLocation + " is no longer present; stopping the 1200 bps reset retries.");
                return false;
            }
            emit log(QStringLiteral("Port is not ready yet, retrying 1200 bps reset (%1/%2)...")
                         .arg(attempt + 1).arg(kAttempts));
            QThread::msleep(1000);
        }

        QSerialPort sp;
        sp.setPortName(portLocation);
        sp.setBaudRate(1200);
        if (!sp.open(QIODevice::ReadWrite)) {
            lastError = sp.errorString();
            const QSerialPort::SerialPortError e = sp.error();
            // Нет прав - повторять бессмысленно, сразу сообщаем пользователю.
            if (e == QSerialPort::PermissionError) {
                m_portOpenFailed = true;
                m_permissionDenied = true;
#if defined(Q_OS_LINUX)
                emit portAccessDenied(portLocation);
#endif
                emit log("Failed to open port " + portLocation + " for 1200 bps reset: " + lastError);
                return false;
            }
            // Повторяем всё, кроме заведомо безнадёжных случаев:
            //   - PermissionError: нет прав, повтор не поможет;
            //   - DeviceNotFoundError: порта нет вовсе (плата уже в BOOTSEL или отключена).
            // Остальные коды (UnknownError, ResourceError, OpenError и т.п.) считаем
            // временными: на Windows ERROR_GEN_FAILURE ("устройство не работает") приходит
            // как UnknownError, а не как OpenError/ResourceError, поэтому фильтр по
            // конкретным кодам отключал бы повторы именно в сценарии, ради которого они введены.
            if (e == QSerialPort::DeviceNotFoundError) {
                m_portOpenFailed = true;
                emit log("Failed to open port " + portLocation + " for 1200 bps reset: " + lastError);
                return false;
            }
            continue;
        }
        sp.setDataTerminalReady(true);
        QThread::msleep(50);
        sp.setDataTerminalReady(false);
        sp.close();

        emit log("Reset 1200 bps signal sent to " + portLocation + ".");
        return true;
    }

    m_portOpenFailed = true;
    emit log("Failed to open port " + portLocation + " for 1200 bps reset: " + lastError);
    return false;
}

// ----------------------------------------------------------------------------
// Ожидание тома. Если появился ровно один подходящий диск - ждём автомонтирования
// (~3 с), затем монтируем сами через udisks. Несколько подходящих дисков - ошибка.
// ----------------------------------------------------------------------------
bool Worker::waitForMountedVolume(const std::function<QList<Platform::Candidate>()>& finder,
                                  int timeoutMs, Platform::Candidate* out) {
    QElapsedTimer total;
    total.start();
    QElapsedTimer unmountedFor;
    bool seenUnmounted = false;
    bool mountTried = false;
    qint64 lastMountTry = 0;

    while (total.elapsed() < timeoutMs) {
        if (!pause(500)) return false;                 // отмена

        const QList<Platform::Candidate> found = finder();
        if (found.size() > 1) {
            QStringList ids;
            for (const Platform::Candidate& c : found) ids << c.diskId;
            emit log("Error: More than one matching disk appeared (" + ids.join(", ") +
                     "). Leave only the Karabas connected and try again.");
            return false;
        }
        if (found.isEmpty()) {
            seenUnmounted = false;
            continue;
        }

        Platform::Candidate c = found.first();
        if (!c.mountPoint.isEmpty()) {
            *out = c;
            return true;
        }
        if (!seenUnmounted) {
            seenUnmounted = true;
            unmountedFor.start();
        }
        // Начинаем монтировать почти сразу: udisks обычно готов через 200-500 мс,
        // а при неудаче повторяем каждые 2 секунды.
        if (unmountedFor.elapsed() >= 300 && (!mountTried || total.elapsed() - lastMountTry >= 2000)) {
            mountTried = true;
            lastMountTry = total.elapsed();
            QString detail;
            if (Platform::mountCandidate(c, &detail)) {
                emit log("Mounted " + c.node + " at " + c.mountPoint);
                *out = c;
                return true;
            }
            // Сразу после появления раздела udisks может ещё не знать о нём -
            // это ожидаемо, повторяем попытку через 2 секунды.
            emit log("Mount attempt failed (will retry): " + detail);
        }
    }
    return false;
}

void Worker::cleanupVolumeJunk(const QString& root, const QString& backupDir) {
    QStringList extraDirs;
    if (QFileInfo(backupDir).isDir()) extraDirs << backupDir;
    const int cleaned = Platform::cleanupVolumeJunk(root, extraDirs);
    if (cleaned > 0) {
        emit log("Removed " + QString::number(cleaned) + " system service files from the SD card.");
    }
}

bool Worker::ejectBestEffort(const Platform::Candidate& sd) {
    QString detail;
    if (Platform::ejectCandidate(sd, &detail)) {
        emit log("Success: SD Card drive safely ejected." + (detail.isEmpty() ? QString() : " (" + detail + ")"));
        return true;
    }
    emit log("Warning: Failed to safely eject drive: " + detail);
    return false;
}

void Worker::rollbackCores(const QStringList& copiedNew, const QList<QPair<QString, QString>>& movedOld) {
    for (const QString& f : copiedNew) QFile::remove(f);
    int restored = 0;
    for (const auto& pr : movedOld) {
        if (QFile::rename(pr.second, pr.first)) {
            ++restored;
        } else {
            emit log("Warning: Could not restore " + QFileInfo(pr.first).fileName() +
                     " from backup; the file remains at " + pr.second);
        }
    }
    emit log("Warning: Rollback performed. Restored " + QString::number(restored) + " of " +
             QString::number(movedOld.size()) + " original core files.");
}

// ----------------------------------------------------------------------------
// Переключение в MSC, бэкап старых ядер, копирование новых, извлечение диска.
// true - можно продолжать прошивку; false - операция прервана.
// ----------------------------------------------------------------------------
Worker::MscResult Worker::updateCoresOnSd(const ModelInfo& m, const QString& portLocation,
                                          const QStringList& newCores, bool updateExtra, bool updateFirmware) {
    MscResult result;
    emit log(QStringLiteral("Cores check active. Switching Karabas to SD Card MSC mode (card 1)..."));

    // Снимок USB-дисков с носителем ДО переключения
    const QSet<QString> before = Platform::readyRemovableDisks();
    {
        QStringList list = before.values();
        list.sort();
        emit log("USB disks with media before switch: " + (list.isEmpty() ? QStringLiteral("none") : list.join(", ")));
    }

    // Ядра всегда лежат на карте №1, поэтому режим MSC фиксирован.
    if (!sendMscCommand(portLocation, 1)) {
        emit log(QStringLiteral("Error: Could not switch the device to MSC mode. Aborting."));
        result.abortReason = QStringLiteral("MSC command failed");
        return result;
    }

    emit log(QStringLiteral("Waiting for SD Card storage drive to mount..."));
    Platform::Candidate sd;
    if (!waitForMountedVolume([&before]() { return Platform::newDisks(before); }, 40000, &sd)) {
        if (m_cancel.load()) {
            emit log(QStringLiteral("Operation cancelled by user. The device may stay in MSC mode - replug it."));
            result.abortReason = QStringLiteral("cancelled");
        } else {
            emit log(QStringLiteral("Error: SD card storage drive not detected after switching mode."));
            emit log(Platform::diagnostics());
            result.abortReason = QStringLiteral("SD card drive not detected");
        }
        return result;
    }
    emit log("Found SD Card drive: " + sd.mountPoint + " [" + sd.label + "] (" + sd.node + ")");

    // ---- Дальше отмена не проверяется: операции над SD доводятся до конца ----
    const QString root = sd.mountPoint;
    const QString backupDir =
        joinPath(root, "backup_" + QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss"));
    if (!QDir().mkpath(backupDir)) {
        emit log("Error: Failed to create backup folder: " + backupDir);
        ejectBestEffort(sd);
        result.abortReason = QStringLiteral("cannot create backup folder");
        return result;
    }
    emit log("Success: Created backup folder: " + backupDir);

    // 1) Бэкап старых ядер. Переносим только те файлы, которые входят в релизный набор
    // (совпадают по имени с newCores); пользовательские/тестовые ядра не трогаем.
    emit log("Moving existing release cores (" + m.ext + ") from SD root to backup folder...");
    // FAT регистронезависима: "ALF.KG3" на карте и "alf.kg3" в релизе - один и тот же файл.
    // Сравниваем имена в нижнем регистре, иначе старый файл не попадёт в бэкап и будет
    // перезаписан без возможности отката.
    QSet<QString> releaseNames;
    for (const QString& src : newCores) releaseNames.insert(QFileInfo(src).fileName().toLower());
    QStringList oldCores;
    for (const QString& f : listFilesWithExt(root, m.ext)) {
        if (releaseNames.contains(QFileInfo(f).fileName().toLower())) oldCores << f;
    }
    QList<QPair<QString, QString>> movedOld;
    bool moveFailed = false;
    for (const QString& src : oldCores) {
        const QString dst = joinPath(backupDir, QFileInfo(src).fileName());
        if (QFile::exists(dst)) QFile::remove(dst);
        if (QFile::rename(src, dst)) {
            movedOld.append(qMakePair(src, dst));
        } else {
            emit log("Warning: Failed to move file " + QFileInfo(src).fileName());
            moveFailed = true;
            break;
        }
    }
    emit log("Backup finished. Moved " + QString::number(movedOld.size()) + " files.");
    if (moveFailed) {
        rollbackCores(QStringList(), movedOld);
        emit log(QStringLiteral("Error: Backup step failed. Aborting before any new core is written."));
        ejectBestEffort(sd);
        result.abortReason = QStringLiteral("backup step failed");
        return result;
    }

    // 2) Проверка свободного места: суммарный размер новых ядер + запас 1 МБ
    qint64 needed = 1024 * 1024;
    for (const QString& src : newCores) needed += QFileInfo(src).size();
    const qint64 available = QStorageInfo(root).bytesAvailable();
    if (available >= 0 && available < needed) {
        emit log("Error: Not enough free space on the SD card (need " + QString::number(needed / 1024) +
                 " KB, available " + QString::number(available / 1024) + " KB). Aborting.");
        rollbackCores(QStringList(), movedOld);
        ejectBestEffort(sd);
        result.abortReason = QStringLiteral("not enough free space");
        return result;
    }

    // 3) Копирование новых ядер (копия, а не перенос: рабочая папка остаётся для повторной прошивки)
    emit log(QStringLiteral("Copying new cores to SD root..."));
    QStringList copiedNew;
    bool copyFailed = false;
    for (const QString& src : newCores) {
        const QString dst = joinPath(root, QFileInfo(src).fileName());
        QString e;
        if (!copyFileSynced(src, dst, &e)) {
            emit log("Warning: Failed to copy new core " + QFileInfo(src).fileName() + ": " + e);
            QFile::remove(dst);
            copyFailed = true;
            break;
        }
        if (QFileInfo(src).size() != QFileInfo(dst).size()) {
            emit log("Warning: Size mismatch after copying " + QFileInfo(src).fileName() + ".");
            QFile::remove(dst);
            copyFailed = true;
            break;
        }
        copiedNew << dst;
    }
    if (copyFailed) {
        rollbackCores(copiedNew, movedOld);
        emit log(QStringLiteral("Error: Core deployment failed. Original cores were restored. Aborting."));
        ejectBestEffort(sd);
        result.abortReason = QStringLiteral("core deployment failed");
        return result;
    }
    result.coresOk = true;
    emit log("Deployment finished. Copied " + QString::number(copiedNew.size()) + " new core files to SD root.");

    if (movedOld.isEmpty() && !QDir().rmdir(backupDir)) {   // пустую папку бэкапа убираем
        emit log("Warning: Could not remove the empty backup folder: " + backupDir);
    }

    // Extra content копируется в том же MSC-сеансе, после ядер, но только если
    // пользователь отметил "Download extra content". Наличие маркера от прошлого
    // запуска не должно приводить к копированию при снятой галке.
    if (updateExtra) {
        result.extraAttempted = true;
        const QString extraFilesDir = workDir() + QStringLiteral("/extra/files");
        if (QFile::exists(workDir() + QStringLiteral("/extra/") + kExtraCompleteMarker) &&
            QFileInfo(extraFilesDir).isDir()) {
            if (!copyExtraToSd(extraFilesDir, root)) {
                // Сбой копирования extra не должен прерывать прошивку: ядра уже на карте,
                // прошивка важнее. Статус extra будет failed в сводке.
                emit log(QStringLiteral("Warning: Extra content was not fully copied; continuing with the rest."));
            } else {
                result.extraOk = true;
            }
        } else {
            emit log(QStringLiteral("Warning: Extra content is not downloaded; skipping extra copy."));
        }
    }

    // Служебные файлы ОС (macOS: "._*") удаляем ДО извлечения карты
    cleanupVolumeJunk(root, backupDir);

    emit log(QStringLiteral("Ejecting SD Card drive from system..."));
    if (!ejectBestEffort(sd)) {
        // Без eject Карабас не выйдет из режима MSC, порта не будет - ждать его бессмысленно
        if (updateFirmware) {
            emit log(QStringLiteral("Error: The SD card could not be ejected, so the device cannot leave MSC mode by "
                                    "itself. The new cores are already on the card. Replug the Karabas (unplug and plug "
                                    "the USB cable), then press FLASH again with only 'Update firmware' checked to finish "
                                    "flashing the firmware."));
        } else {
            emit log(QStringLiteral("Error: The SD card could not be ejected, so the device cannot leave MSC mode by "
                                    "itself. The new cores are already on the card. Replug the Karabas (unplug and plug "
                                    "the USB cable)."));
        }
        result.abortReason = QStringLiteral("eject failed");
        return result;
    }
    return result;
}

// ----------------------------------------------------------------------------
// Прошивка
// ----------------------------------------------------------------------------
void Worker::doFlash(const Params& p) {
    // --- Предварительные проверки: ДО любых действий с SD-картой и устройством ---
    if (!p.updateFirmware && !p.updateCores && !p.updateExtra) {
        emit log(QStringLiteral("Error: Nothing to do. Check 'Update firmware', 'Update cores' and/or "
                                "'Download extra content'."));
        return;
    }
    // Итоговая сводка: собираем статусы по каждой операции, чтобы в конце вывести
    // "firmware=done, cores=done, extra=failed" вместо одного "completed successfully".
    QString fwStatus = p.updateFirmware ? QStringLiteral("pending") : QStringLiteral("skipped");
    QString coresStatus = p.updateCores ? QStringLiteral("pending") : QStringLiteral("skipped");
    QString extraStatus = p.updateExtra ? QStringLiteral("pending") : QStringLiteral("skipped");
    auto printSummary = [&]() {
        emit log(QStringLiteral("Flash summary: firmware=%1, cores=%2, extra=%3.")
                    .arg(fwStatus, coresStatus, extraStatus));
    };
    // Все ли запрошенные операции завершились успешно (или были пропущены).
    // Нужно, чтобы не печатать "Process completed successfully", когда extra=failed.
    auto allOk = [&]() {
        auto ok = [](const QString& s) {
            return s == QLatin1String("skipped") || s == QLatin1String("done") ||
                   s.startsWith(QLatin1String("done "));
        };
        return ok(fwStatus) && ok(coresStatus) && ok(extraStatus);
    };
    // Единая точка выхода: печатает сводку и возвращает управление.
    auto finish = [&]() { printSummary(); };
    if (p.updateFirmware) {
        if (p.firmwarePath.isEmpty()) {
            emit log(QStringLiteral("Error: Firmware file not selected."));
            return;
        }
        QString why;
        if (!isValidUf2(p.firmwarePath, &why)) {
            emit log("Error: Firmware file is not usable: " + why);
            return;
        }
    }

    if (p.updateCores || p.updateExtra) {
        if (p.updateCores) {
            if (p.modelIndex < 0 || p.modelIndex >= models().size()) {
                emit log(QStringLiteral("Error: Unknown device model selected."));
                return;
            }
        }
        if (p.portLocation.isEmpty()) {
            emit log(QStringLiteral("Error: Updating cores/extra requires a serial port (to send the MSC command)."));
            return;
        }
        const QString coresDir = workDir() + QStringLiteral("/cores");
        const QString extraDir = workDir() + QStringLiteral("/extra");
        const QString extraFilesDir = extraDir + QStringLiteral("/files");
        QStringList newCores;
        if (p.updateCores) {
            const ModelInfo& m = models().at(p.modelIndex);
            if (!QFile::exists(coresDir + "/" + kCoresCompleteMarker)) {
                emit log(QStringLiteral("Error: No complete set of downloaded cores found. Check 'Update cores' and "
                                        "press 'Download via GitHub' first."));
                return;
            }
            newCores = listFilesWithExt(coresDir, m.ext);
            if (newCores.isEmpty()) {
                emit log("Error: No downloaded core files (" + m.ext +
                         ") for the selected model. Press 'Download via GitHub' first.");
                return;
            }
        }
        if (p.updateExtra) {
            if (!QFile::exists(extraDir + "/" + kExtraCompleteMarker) || !QFileInfo(extraFilesDir).isDir()) {
                // Не прерываем: прошивка и ядра важнее. Extra будет отмечен как failed в сводке.
                emit log(QStringLiteral("Warning: No downloaded extra content found. Extra content will be "
                                        "skipped; check 'Download extra content' and press 'Download via GitHub' "
                                        "to fetch it."));
                extraStatus = QStringLiteral("failed (not downloaded)");
            }
        }

        // Один MSC-сеанс: сначала ядра, затем extra (обе операции на карте №1).
        if (p.updateCores) {
            const ModelInfo& m = models().at(p.modelIndex);
            const MscResult r = updateCoresOnSd(m, p.portLocation, newCores, p.updateExtra, p.updateFirmware);
            coresStatus = r.coresOk ? QStringLiteral("done") : QStringLiteral("failed");
            if (p.updateExtra) {
                if (!r.extraAttempted) {
                    // Сеанс прервался до шага extra (ядра/бэкап/eject) - extra не трогали.
                    extraStatus = QStringLiteral("failed (session aborted)");
                } else {
                    // extraAttempted == true: шаг extra выполнялся, но набор не был скачан
                    // (или копирование не удалось) - это и есть "not downloaded".
                    extraStatus = r.extraOk ? QStringLiteral("done")
                                            : QStringLiteral("failed (not downloaded)");
                }
            }
            if (!r.abortReason.isEmpty()) {
                finish();
                return;
            }
        } else {
            // Только extra: сначала проверяем, что архив скачан и распакован.
            // Иначе MSC-сеанс (две перезагрузки устройства) запускается впустую,
            // и только copyExtraToSd сообщает об ошибке.
            if (!QFile::exists(extraDir + "/" + kExtraCompleteMarker) || !QFileInfo(extraFilesDir).isDir()) {
                emit log(QStringLiteral("Error: No downloaded extra content found. Check 'Download extra content' "
                                        "and press 'Download via GitHub' to fetch it."));
                extraStatus = QStringLiteral("failed (not downloaded)");
                finish();
                return;
            }
            // Открываем MSC и копируем.
            emit log(QStringLiteral("Switching Karabas to SD Card MSC mode (card 1) for extra content..."));
            const QSet<QString> before = Platform::readyRemovableDisks();
            if (!sendMscCommand(p.portLocation, 1)) {
                emit log(QStringLiteral("Error: Could not switch the device to MSC mode. Aborting."));
                extraStatus = QStringLiteral("failed");
                finish();
                return;
            }
            emit log(QStringLiteral("Waiting for SD Card storage drive to mount..."));
            Platform::Candidate sd;
            if (!waitForMountedVolume([&before]() { return Platform::newDisks(before); }, 40000, &sd)) {
                if (m_cancel.load()) {
                    emit log(QStringLiteral("Operation cancelled by user. The device may stay in MSC mode - replug it."));
                    extraStatus = QStringLiteral("cancelled");
                } else {
                    emit log(QStringLiteral("Error: SD card storage drive not detected after switching mode."));
                    emit log(Platform::diagnostics());
                    extraStatus = QStringLiteral("failed");
                }
                finish();
                return;
            }
            emit log("Found SD Card drive: " + sd.mountPoint + " [" + sd.label + "] (" + sd.node + ")");
            const bool extraOk = copyExtraToSd(extraFilesDir, sd.mountPoint);
            extraStatus = extraOk ? QStringLiteral("done") : QStringLiteral("failed");

            // Служебные файлы ОС (macOS: "._*") удаляем ДО извлечения карты
            cleanupVolumeJunk(sd.mountPoint, QString());

            emit log(QStringLiteral("Ejecting SD Card drive from system..."));
            if (!ejectBestEffort(sd)) {
                emit log(QStringLiteral("Error: The SD card could not be ejected. Replug the Karabas."));
                finish();
                return;
            }
        }

        emit log(QStringLiteral("Waiting for device to become available as a serial port again..."));
        bool portFound = false;
        for (int i = 0; i < 40 && !portFound; ++i) {
            if (!pause(500)) {
                emit log(QStringLiteral("Operation cancelled by user."));
                if (p.updateFirmware) fwStatus = QStringLiteral("cancelled");
                finish();
                return;
            }
            portFound = isPortPresent(p.portLocation);
        }
        if (!p.updateFirmware) {
            if (!allOk()) {
                emit log(QStringLiteral("Warning: The operation finished with errors (see the summary below)."));
            } else if (portFound) {
                emit log(QStringLiteral("Process completed successfully. The device is back in normal mode."));
            } else {
                emit log(QStringLiteral("Warning: The operation is complete, but the device port was not detected again. "
                                        "Replug the device if it does not respond."));
            }
            finish();
            return;
        }
        if (portFound) {
            emit log("Port " + p.portLocation + " is back online.");
            // Порт появился в списке, но Windows может ещё несколько секунд отдавать
            // ERROR_GEN_FAILURE при открытии. Даём устройству "устояться".
            if (!pause(2000)) {
                emit log(QStringLiteral("Operation cancelled by user."));
                fwStatus = QStringLiteral("cancelled");
                finish();
                return;
            }
        } else {
            emit log("Warning: Chosen port " + p.portLocation +
                     " not detected automatically (the device name may have changed). Trying to open directly.");
        }
        // Сначала пробуем сброс: если порт Карабаса живой, он всегда должен сработать.
        // Проверка bootloaderDisks() до сброса опасна: если рядом в BOOTSEL лежит другая
        // RP2040-плата (Pico и т.п.), сброс будет пропущен, и прошивка уйдёт на чужую плату.
        // В ветке с MSC-сеансом имя порта могло измениться, поэтому продолжать без сброса
        // нельзя: рядом в BOOTSEL может оказаться чужая плата. Прерываемся с подсказкой.
        if (!resetPortToBootloader(p.portLocation) && m_portOpenFailed) {
            const bool portGone = !m_permissionDenied && !isPortPresent(p.portLocation);
            if (portGone && !Platform::bootloaderDisks().isEmpty()) {
                emit log(QStringLiteral("Error: The serial port is no longer present, but an RPI-RP2 bootloader "
                                        "drive is available. It may belong to another board (the port name can "
                                        "change after the MSC session). Replug the Karabas and press FLASH again "
                                        "with only 'Update firmware' checked."));
            } else {
                emit log(QStringLiteral("Error: Could not open the serial port for the 1200 bps reset. "
                                        "Aborting before flashing."));
            }
            fwStatus = QStringLiteral("failed (reset)");
            finish();
            return;
        }
    } else if (!p.portLocation.isEmpty()) {
        // Ветка без MSC-сеанса: имя порта не менялось, поэтому если порт реально исчез
        // (отключили кабель) и RPI-RP2 уже виден, продолжаем без сброса.
        if (!resetPortToBootloader(p.portLocation) && m_portOpenFailed) {
            const bool portGone = !m_permissionDenied && !isPortPresent(p.portLocation);
            if (portGone && !Platform::bootloaderDisks().isEmpty()) {
                emit log(QStringLiteral("The serial port is no longer present, but the RPI-RP2 bootloader drive "
                                        "is already available; continuing without the 1200 bps reset."));
            } else {
                emit log(QStringLiteral("Error: Could not open the serial port for the 1200 bps reset. "
                                        "Aborting before flashing."));
                fwStatus = QStringLiteral("failed (reset)");
                finish();
                return;
            }
        }
    } else {
        emit log(QStringLiteral("Warning: No serial port selected, so the board will not be reset automatically. "
                                "If the board is not already in bootloader mode, hold BOOTSEL and replug the USB "
                                "cable, or select a port and press FLASH again."));
    }

    emit log(QStringLiteral("Waiting for RPI-RP2 bootloader drive to appear..."));
    Platform::Candidate boot;
    if (!waitForMountedVolume([]() { return Platform::bootloaderDisks(); }, 25000, &boot)) {
        if (m_cancel.load()) {
            emit log(QStringLiteral("Operation cancelled by user."));
            fwStatus = QStringLiteral("cancelled");
        } else {
            emit log(QStringLiteral("Error: Removable drive 'RPI-RP2' not found. Please connect the device in "
                                    "bootloader mode manually."));
            emit log(Platform::diagnostics());
            fwStatus = QStringLiteral("failed (no RPI-RP2)");
        }
        finish();
        return;
    }
    emit log("Target drive found: " + boot.mountPoint + " [RPI-RP2] (" + boot.node + ")");

    // ---- Запись прошивки: не прерывается ----
    const QString target = joinPath(boot.mountPoint, QStringLiteral("firmware.uf2"));
    emit log("Copying firmware to device: " + target);

    QString err;
    if (copyFileSynced(p.firmwarePath, target, &err)) {
        fwStatus = QStringLiteral("done");
        if (allOk()) {
            emit log(QStringLiteral("Process completed successfully. Device is rebooting with new firmware."));
        } else {
            emit log(QStringLiteral("Warning: The firmware was written, but some operations failed "
                                    "(see the summary below)."));
        }
    } else {
        // RP2040 перезагружается сразу после записи последнего блока - копирование
        // может сообщить об ошибке, хотя прошивка уже принята.
        QThread::msleep(1500);
        if (Platform::bootloaderDisks().isEmpty()) {
            fwStatus = QStringLiteral("done (copy reported an error, but the device rebooted)");
            emit log("Warning: Copy reported an error (" + err +
                     "), but the bootloader drive has disappeared - the device most likely rebooted with the "
                     "new firmware. Please verify.");
        } else {
            fwStatus = QStringLiteral("failed");
            emit log("Flash write error: " + err);
        }
    }
    finish();

    // Eject после прошивки не нужен: RP2040 перезагружается сам сразу после записи
    // последнего блока, и диск RPI-RP2 исчезает. Попытка eject на уже исчезнувшем
    // диске даёт ложную ошибку (Windows: "eject request failed (error 433)").
}
