// macOS: тома монтируются системой в /Volumes. Свойства тома берутся из `diskutil info -plist`.
#include "Platform.h"

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

int runProcess(const QString& program, const QStringList& args, int timeoutMs,
               QString* out = nullptr, QString* err = nullptr) {
    QProcess p;
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

QString xmlUnescape(QString s) {
    s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    s.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    s.replace(QStringLiteral("&apos;"), QStringLiteral("'"));
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    return s;
}

QString plistString(const QString& xml, const QString& key) {
    const QRegularExpression re(QStringLiteral("<key>") + QRegularExpression::escape(key) +
                                QStringLiteral("</key>\\s*<string>(.*?)</string>"),
                                QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = re.match(xml);
    return m.hasMatch() ? xmlUnescape(m.captured(1)) : QString();
}

bool plistBool(const QString& xml, const QString& key, bool def) {
    const QRegularExpression re(QStringLiteral("<key>") + QRegularExpression::escape(key) +
                                QStringLiteral("</key>\\s*<(true|false)/>"));
    const QRegularExpressionMatch m = re.match(xml);
    return m.hasMatch() ? (m.captured(1) == QLatin1String("true")) : def;
}

// Внешние USB-тома, смонтированные в /Volumes
QList<Candidate> usbVolumes() {
    QList<Candidate> out;
    const QList<QStorageInfo> vols = QStorageInfo::mountedVolumes();
    for (const QStorageInfo& s : vols) {
        const QString root = s.rootPath();
        if (!root.startsWith(QStringLiteral("/Volumes/")) || !s.isReady()) continue;

        QString xml;
        if (runProcess(QStringLiteral("diskutil"),
                       QStringList() << QStringLiteral("info") << QStringLiteral("-plist") << root, 10000, &xml) != 0)
            continue;
        const QString node = plistString(xml, QStringLiteral("DeviceNode"));
        const QString whole = plistString(xml, QStringLiteral("ParentWholeDisk"));
        const QString bus = plistString(xml, QStringLiteral("BusProtocol"));
        if (node.isEmpty() || whole.isEmpty()) continue;
        if (plistBool(xml, QStringLiteral("Internal"), true)) continue;
        if (bus.compare(QStringLiteral("USB"), Qt::CaseInsensitive) != 0) continue;

        Candidate c;
        c.diskId = whole;                                   // "disk4"
        c.diskNode = QStringLiteral("/dev/") + whole;
        c.node = node;                                      // "/dev/disk4s1"
        c.label = plistString(xml, QStringLiteral("VolumeName"));
        c.mountPoint = root;
        out << c;
    }
    return out;
}

bool isBootloaderLabel(const QString& l) { return l.compare(QStringLiteral("RPI-RP2"), Qt::CaseInsensitive) == 0; }

}  // namespace

QSet<QString> readyRemovableDisks() {
    QSet<QString> ids;
    for (const Candidate& c : usbVolumes()) ids.insert(c.diskId);
    return ids;
}

QList<Candidate> newDisks(const QSet<QString>& before) {
    QList<Candidate> out;
    for (const Candidate& c : usbVolumes()) {
        if (before.contains(c.diskId)) continue;
        if (isBootloaderLabel(c.label)) continue;
        out << c;
    }
    return out;
}

QList<Candidate> bootloaderDisks() {
    QList<Candidate> out;
    for (const Candidate& c : usbVolumes()) {
        if (isBootloaderLabel(c.label)) out << c;
    }
    return out;
}

bool mountCandidate(Candidate& c, QString* detail) {
    if (!c.mountPoint.isEmpty()) return true;
    QString out, err;
    const int rc = runProcess(QStringLiteral("diskutil"),
                              QStringList() << QStringLiteral("mount") << c.node, 15000, &out, &err);
    if (rc != 0) {
        if (detail) *detail = err.trimmed().isEmpty() ? QStringLiteral("diskutil mount failed") : err.trimmed();
        return false;
    }
    QString xml;
    runProcess(QStringLiteral("diskutil"), QStringList() << QStringLiteral("info") << QStringLiteral("-plist") << c.node,
               10000, &xml);
    c.mountPoint = plistString(xml, QStringLiteral("MountPoint"));
    return !c.mountPoint.isEmpty();
}

bool ejectCandidate(const Candidate& c, QString* detail) {
    auto setDetail = [&](const QString& s) { if (detail) *detail = s; };
    setDetail(QString());

    ::sync();

    // diskutil eject сам размонтирует том и извлечёт носитель. Отдельный unmount перед eject
    // приводит к системному уведомлению "disk not ejected properly", поэтому не используем его.
    // Spotlight может ненадолго занять том - повторяем попытку.
    QString lastErr;
    for (int i = 0; i < 10; ++i) {
        QString out, err;
        const int rc = runProcess(QStringLiteral("diskutil"),
                                  QStringList() << QStringLiteral("eject") << c.diskNode, 15000, &out, &err);
        if (rc == 0) return true;
        lastErr = (err.trimmed().isEmpty() ? out.trimmed() : err.trimmed());
        QThread::msleep(500);
    }

    // Запасной вариант: сначала размонтировать, затем извлечь
    QString out, err;
    if (runProcess(QStringLiteral("diskutil"),
                   QStringList() << QStringLiteral("unmount") << c.mountPoint, 15000, &out, &err) == 0) {
        if (runProcess(QStringLiteral("diskutil"),
                       QStringList() << QStringLiteral("eject") << c.diskNode, 15000, &out, &err) == 0) {
            return true;
        }
    }
    setDetail(QStringLiteral("eject failed: ") + lastErr);
    return false;
}

int cleanupVolumeJunk(const QString& mountPoint, const QStringList& extraDirs) {
    int removed = 0;

    // Файлы AppleDouble "._*" и .DS_Store - в корне и в указанных папках
    auto cleanFiles = [&](const QString& dirPath) {
        const QFileInfoList entries =
            QDir(dirPath).entryInfoList(QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const QFileInfo& fi : entries) {
            const QString n = fi.fileName();
            if (n.startsWith(QStringLiteral("._")) || n == QLatin1String(".DS_Store")) {
                if (QFile::remove(fi.absoluteFilePath())) ++removed;
            }
        }
    };
    cleanFiles(mountPoint);
    for (const QString& d : extraDirs) cleanFiles(d);

    // Служебные папки, которые macOS создаёт на съёмных томах (не трогаем .metadata_never_index:
    // он запрещает индексацию Spotlight и тем самым не даёт появиться новому мусору)
    static const QStringList junkDirs = { QStringLiteral(".Trashes"), QStringLiteral(".Spotlight-V100"),
                                          QStringLiteral(".TemporaryItems"), QStringLiteral(".fseventsd") };
    for (const QString& name : junkDirs) {
        const QString path = mountPoint + QLatin1Char('/') + name;
        if (QFileInfo(path).isDir() && QDir(path).removeRecursively()) ++removed;
    }
    return removed;
}

QString diagnostics() {
    QStringList lines;
    const QList<Candidate> vols = usbVolumes();
    if (vols.isEmpty()) lines << QStringLiteral("Diagnostics: no external USB volumes are mounted in /Volumes.");
    for (const Candidate& c : vols) {
        lines << QStringLiteral("Diagnostics: %1 node=%2 label=[%3] mounted at %4")
                     .arg(c.diskId, c.node, c.label, c.mountPoint);
    }
    return lines.join(QLatin1Char('\n'));
}

}  // namespace Platform
