#include "Util.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <cstring>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <io.h>
#endif

bool isSafeFileName(const QString& n) {
    if (n.isEmpty() || n.size() > 128 || n == QLatin1String(".") || n == QLatin1String(".."))
        return false;
    static const QString forbidden = QStringLiteral("\\/:*?\"<>|");
    for (const QChar c : n) {
        if (c.unicode() < 0x20) return false;
        if (forbidden.contains(c)) return false;
    }
    return !n.endsWith(QLatin1Char(' ')) && !n.endsWith(QLatin1Char('.'));
}

QString workDir() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    // Если CacheLocation пуст (например, HOME не задан), путь стал бы "/work",
    // а cleanupTemp() вызывает removeRecursively() - это опасно. Возвращаем пустую
    // строку, а вызывающий код должен отказаться от работы с рабочей папкой.
    if (base.isEmpty()) return QString();
    return base + QStringLiteral("/work");
}

QStringList listFilesWithExt(const QString& dir, const QString& ext) {
    QStringList out;
    const QFileInfoList list = QDir(dir).entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo& fi : list) {
        if (fi.fileName().startsWith(QLatin1String("._"))) continue;     // служебные файлы macOS, не ядра
        if (fi.fileName().endsWith(ext, Qt::CaseInsensitive)) out << fi.absoluteFilePath();
    }
    return out;
}

QProcessEnvironment cleanChildEnvironment() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString appdir = env.value(QStringLiteral("APPDIR"));
    if (!appdir.isEmpty()) {
        QStringList keep;
        const QStringList parts = env.value(QStringLiteral("LD_LIBRARY_PATH")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
        for (const QString& e : parts) {
            if (!e.startsWith(appdir)) keep << e;
        }
        if (keep.isEmpty()) env.remove(QStringLiteral("LD_LIBRARY_PATH"));
        else env.insert(QStringLiteral("LD_LIBRARY_PATH"), keep.join(QLatin1Char(':')));
    }
    return env;
}

QProcessEnvironment cleanChildEnvironmentCLocale() {
    QProcessEnvironment env = cleanChildEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    return env;
}

bool openInFileManager(const QString& path) {
#if defined(Q_OS_LINUX)
    // xdg-open запускается с чистым окружением: внутри AppImage LD_LIBRARY_PATH указывает
    // на библиотеки образа, и файловый менеджер мог бы подхватить чужие версии.
    QProcess p;
    p.setProgram(QStringLiteral("xdg-open"));
    p.setArguments(QStringList() << path);
    p.setProcessEnvironment(cleanChildEnvironment());
    if (p.startDetached()) return true;
#endif
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

static void setErr(QString* e, const QString& m) {
    if (e) *e = m;
}

bool isKarabasVolume(const QString& mountPoint) {
    if (mountPoint.isEmpty()) return false;
    const QDir dir(mountPoint);
    if (!dir.exists()) return false;

    // Ядра в корне карты: .kg (новый общий формат) и .kg1/.kg2/.kg3 (старые карты,
    // прошитые до перехода на общий формат). Старые расширения оставлены для
    // совместимости: карта с ними всё ещё считается картой Карабаса.
    const QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo& fi : files) {
        const QString n = fi.fileName();
        if (n.startsWith(QLatin1String("._"))) continue;      // служебные файлы macOS
        if (n.endsWith(QLatin1String(".kg"), Qt::CaseInsensitive) ||
            n.endsWith(QLatin1String(".kg1"), Qt::CaseInsensitive) ||
            n.endsWith(QLatin1String(".kg2"), Qt::CaseInsensitive) ||
            n.endsWith(QLatin1String(".kg3"), Qt::CaseInsensitive)) {
            return true;
        }
    }
    // Папка резервной копии, которую создаёт программа при обновлении ядер.
    const QFileInfoList dirs = dir.entryInfoList(QDir::Dirs | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo& fi : dirs) {
        if (fi.fileName().startsWith(QLatin1String("backup_"))) return true;
    }
    return false;
}

bool copyFileSynced(const QString& src, const QString& dst, QString* err) {
    QFile in(src);
    if (!in.open(QIODevice::ReadOnly)) {
        setErr(err, "cannot open source: " + in.errorString());
        return false;
    }
    QFile out(dst);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setErr(err, "cannot create destination: " + out.errorString());
        return false;
    }

    QByteArray buf;
    buf.resize(256 * 1024);
    while (!in.atEnd()) {
        const qint64 n = in.read(buf.data(), buf.size());
        if (n < 0) {
            setErr(err, "read error: " + in.errorString());
            return false;
        }
        if (n == 0) break;
        if (out.write(buf.constData(), n) != n) {
            setErr(err, "write error: " + out.errorString());
            return false;
        }
    }
    if (!out.flush()) {
        setErr(err, "flush error: " + out.errorString());
        return false;
    }
#if defined(Q_OS_UNIX)
    if (::fsync(out.handle()) != 0) {
        setErr(err, QStringLiteral("fsync failed: ") + QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
#elif defined(Q_OS_WIN)
    {
        const int fd = out.handle();
        if (fd >= 0) _commit(fd);      // ошибка не критична: том всё равно блокируется и сбрасывается при eject
    }
#endif
    out.close();
    return true;
}
