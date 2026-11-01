#include "Platform.h"
#include "Util.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QStringList>
#include <QThread>

#include <unistd.h>

namespace Platform {
namespace {

// Запуск внешней программы. Возврат: код завершения; -1 не удалось запустить;
// -2 таймаут; -3 аварийное завершение.
int runProcess(const QString& program, const QStringList& args, int timeoutMs,
               QString* out = nullptr, QString* err = nullptr) {
    QProcess p;
    // udisksctl/gdbus/busctl/eject: их текстовый вывод разбирается регулярками,
    // поэтому запускаем с C-локалью. xdg-open и 7za идут через cleanChildEnvironment().
    p.setProcessEnvironment(cleanChildEnvironmentCLocale());
    p.start(program, args);
    if (!p.waitForStarted(3000)) return -1;
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        return -2;
    }
    if (out) *out = QString::fromUtf8(p.readAllStandardOutput());
    if (err) *err = QString::fromUtf8(p.readAllStandardError());
    return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -3;
}

// /sys/block/sdb -> .../usb1/1-2/... : диск находится на шине USB
bool isUsbDisk(const QString& id) {
    const QString real = QFileInfo(QStringLiteral("/sys/block/") + id).canonicalFilePath();
    return real.contains(QStringLiteral("/usb"));
}

qint64 diskSizeSectors(const QString& id) {
    QFile f(QStringLiteral("/sys/block/") + id + QStringLiteral("/size"));
    if (!f.open(QIODevice::ReadOnly)) return 0;
    return f.readAll().trimmed().toLongLong();
}

QStringList allUsbDiskIds() {
    QStringList ids;
    const QStringList entries = QDir(QStringLiteral("/sys/block"))
        .entryList(QStringList() << QStringLiteral("sd*"), QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& id : entries) {
        if (isUsbDisk(id)) ids << id;
    }
    return ids;
}

// Первый раздел диска (sdb1) или сам диск, если разделов нет.
QString primaryNodeForDisk(const QString& id) {
    const QStringList parts = QDir(QStringLiteral("/sys/block/") + id)
        .entryList(QStringList() << (id + QLatin1Char('*')), QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    if (!parts.isEmpty()) return QStringLiteral("/dev/") + parts.first();
    return QStringLiteral("/dev/") + id;
}

// udev кодирует спецсимволы в именах by-label как \xNN
QString decodeUdevLabel(const QString& s) {
    const QByteArray in = s.toUtf8();
    QByteArray out;
    for (int i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 3 < in.size() && in[i + 1] == 'x') {
            bool ok = false;
            const int v = in.mid(i + 2, 2).toInt(&ok, 16);
            if (ok) {
                out.append(static_cast<char>(v));
                i += 3;
                continue;
            }
        }
        out.append(in[i]);
    }
    return QString::fromUtf8(out);
}

QString labelForNode(const QString& node) {
    const QString target = QFileInfo(node).canonicalFilePath();
    if (target.isEmpty()) return QString();
    const QFileInfoList links = QDir(QStringLiteral("/dev/disk/by-label"))
        .entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System);
    for (const QFileInfo& fi : links) {
        if (fi.canonicalFilePath() == target) return decodeUdevLabel(fi.fileName());
    }
    return QString();
}

QString mountPointForNode(const QString& node) {
    const QString target = QFileInfo(node).canonicalFilePath();
    if (target.isEmpty()) return QString();
    const QList<QStorageInfo> vols = QStorageInfo::mountedVolumes();
    for (const QStorageInfo& v : vols) {
        if (QFileInfo(QString::fromUtf8(v.device())).canonicalFilePath() == target) return v.rootPath();
    }
    return QString();
}

// Извлечение носителя через udisks (D-Bus Drive.Eject): SCSI-eject выполняется демоном udisks
// с его правами, поэтому доступ к /dev/sdX обычному пользователю не нужен.
// Питание USB-порта при этом НЕ отключается (в отличие от power-off), и Карабас
// после eject сам возвращается в обычный режим.
bool udisksEject(const QString& diskNode, QString* detail) {
    QString out, err;
    int rc = runProcess(QStringLiteral("udisksctl"),
                        QStringList() << QStringLiteral("info") << QStringLiteral("-b") << diskNode, 10000, &out, &err);
    if (rc != 0) {
        if (detail) *detail = (rc == -1) ? QStringLiteral("udisksctl not found") : err.trimmed();
        return false;
    }
    static const QRegularExpression driveRe(QStringLiteral("'(/org/freedesktop/UDisks2/drives/[^']+)'"));
    const QRegularExpressionMatch m = driveRe.match(out);
    if (!m.hasMatch()) {
        if (detail) *detail = QStringLiteral("udisks drive object not found for ") + diskNode;
        return false;
    }
    const QString drive = m.captured(1);

    rc = runProcess(QStringLiteral("gdbus"),
                    QStringList() << QStringLiteral("call") << QStringLiteral("--system")
                                  << QStringLiteral("--dest") << QStringLiteral("org.freedesktop.UDisks2")
                                  << QStringLiteral("--object-path") << drive
                                  << QStringLiteral("--method") << QStringLiteral("org.freedesktop.UDisks2.Drive.Eject")
                                  << QStringLiteral("{}"),
                    15000, &out, &err);
    if (rc == 0) return true;
    if (rc == -1) {                                        // gdbus нет - пробуем busctl
        rc = runProcess(QStringLiteral("busctl"),
                        QStringList() << QStringLiteral("call") << QStringLiteral("org.freedesktop.UDisks2") << drive
                                      << QStringLiteral("org.freedesktop.UDisks2.Drive") << QStringLiteral("Eject")
                                      << QStringLiteral("a{sv}") << QStringLiteral("0"),
                        15000, &out, &err);
        if (rc == 0) return true;
    }
    if (detail) *detail = err.trimmed().isEmpty() ? out.trimmed() : err.trimmed();
    return false;
}

}  // namespace

QSet<QString> readyRemovableDisks() {
    QSet<QString> ready;
    for (const QString& id : allUsbDiskIds()) {
        if (diskSizeSectors(id) > 0) ready.insert(id);
    }
    return ready;
}

QList<Candidate> newDisks(const QSet<QString>& before) {
    QList<Candidate> out;
    for (const QString& id : allUsbDiskIds()) {
        if (before.contains(id)) continue;
        if (diskSizeSectors(id) <= 0) continue;          // носителя ещё нет

        Candidate c;
        c.diskId = id;
        c.diskNode = QStringLiteral("/dev/") + id;
        c.node = primaryNodeForDisk(id);
        if (!QFileInfo::exists(c.node)) continue;        // udev ещё не создал узел
        c.label = labelForNode(c.node);
        if (c.label.compare(QStringLiteral("RPI-RP2"), Qt::CaseInsensitive) == 0) continue;
        c.mountPoint = mountPointForNode(c.node);
        out << c;
    }
    return out;
}

QList<Candidate> bootloaderDisks() {
    QList<Candidate> out;
    const QFileInfo link(QStringLiteral("/dev/disk/by-label/RPI-RP2"));
    if (!link.exists()) return out;

    Candidate c;
    c.node = link.canonicalFilePath();
    c.label = QStringLiteral("RPI-RP2");
    static const QRegularExpression re(QStringLiteral("^/dev/(sd[a-z]+)\\d*$"));
    const QRegularExpressionMatch m = re.match(c.node);
    if (m.hasMatch()) {
        c.diskId = m.captured(1);
        c.diskNode = QStringLiteral("/dev/") + c.diskId;
    } else {
        c.diskNode = c.node;
    }
    c.mountPoint = mountPointForNode(c.node);
    out << c;
    return out;
}

bool mountCandidate(Candidate& c, QString* detail) {
    auto setDetail = [&](const QString& s) { if (detail) *detail = s; };

    QString out, err;
    const int rc = runProcess(QStringLiteral("udisksctl"),
                              QStringList() << QStringLiteral("mount") << QStringLiteral("-b") << c.node
                                            << QStringLiteral("--no-user-interaction"),
                              15000, &out, &err);
    if (rc == -1) {
        setDetail(QStringLiteral("udisksctl not found (install the 'udisks2' package)"));
        return false;
    }
    if (rc == 0) {
        // "Mounted /dev/sdb1 at /run/media/user/CORES."
        static const QRegularExpression okRe(QStringLiteral("^Mounted\\s+\\S+\\s+at\\s+(.+?)\\.?$"));
        const QRegularExpressionMatch m = okRe.match(out.trimmed());
        c.mountPoint = m.hasMatch() ? m.captured(1) : mountPointForNode(c.node);
        return !c.mountPoint.isEmpty();
    }
    // Уже смонтирован (например, автомонтированием рабочего стола)
    static const QRegularExpression alreadyRe(QStringLiteral("already mounted at `(.+)'"));
    const QRegularExpressionMatch m = alreadyRe.match(err);
    if (m.hasMatch()) {
        c.mountPoint = m.captured(1);
        return true;
    }
    c.mountPoint = mountPointForNode(c.node);
    if (!c.mountPoint.isEmpty()) return true;
    setDetail(err.trimmed().isEmpty() ? QStringLiteral("udisksctl failed (code %1)").arg(rc) : err.trimmed());
    return false;
}

bool ejectCandidate(const Candidate& c, QString* detail) {
    auto setDetail = [&](const QString& s) { if (detail) *detail = s; };
    setDetail(QString());

    ::sync();

    // 1) размонтирование (пока том занят - повторяем)
    if (!mountPointForNode(c.node).isEmpty()) {
        bool unmounted = false;
        QString lastErr;
        for (int i = 0; i < 10 && !unmounted; ++i) {
            QString out, err;
            const int rc = runProcess(QStringLiteral("udisksctl"),
                                      QStringList() << QStringLiteral("unmount") << QStringLiteral("-b") << c.node
                                                    << QStringLiteral("--no-user-interaction"),
                                      15000, &out, &err);
            if (rc == 0) {
                unmounted = true;
            } else if (rc == -1) {
                lastErr = QStringLiteral("udisksctl not found (install the 'udisks2' package)");
                break;
            } else {
                lastErr = err.trimmed();
                QThread::msleep(500);
            }
        }
        if (!unmounted) {
            setDetail(QStringLiteral("cannot unmount ") + c.node + QStringLiteral(": ") + lastErr);
            return false;
        }
    }

    // 2) извлечение носителя: сначала через udisks (с правами системы)
    QString udErr;
    if (udisksEject(c.diskNode, &udErr)) return true;

    // 3) запасной вариант: прямой SCSI-eject (нужен доступ к /dev/sdX, например группа disk)
    QString out, err;
    const int rc = runProcess(QStringLiteral("eject"), QStringList() << c.diskNode, 15000, &out, &err);
    if (rc == 0) return true;
    const QString ejectErr = (rc == -1) ? QStringLiteral("'eject' not found") : err.trimmed();

    // power-off сознательно НЕ используется: он отключает питание USB-порта, и Карабас
    // не возвращается в обычный режим до переподключения.
    setDetail(QStringLiteral("eject failed (udisks: ") + udErr + QStringLiteral("; eject: ") + ejectErr + QStringLiteral(")"));
    return false;
}

int cleanupVolumeJunk(const QString&, const QStringList&) {
    return 0;       // Linux не создаёт служебных файлов на vfat
}

QString diagnostics() {
    QStringList lines;
    const QStringList ids = allUsbDiskIds();
    if (ids.isEmpty()) lines << QStringLiteral("Diagnostics: no USB disks (/sys/block/sd*) are visible.");
    for (const QString& id : ids) {
        const QString node = primaryNodeForDisk(id);
        const QString mp = mountPointForNode(node);
        lines << QStringLiteral("Diagnostics: %1 size=%2 sectors, node=%3, label=[%4], %5")
                     .arg(id)
                     .arg(diskSizeSectors(id))
                     .arg(node, labelForNode(node), mp.isEmpty() ? QStringLiteral("not mounted") : QStringLiteral("mounted at ") + mp);
    }
    return lines.join(QLatin1Char('\n'));
}

}  // namespace Platform
