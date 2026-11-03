// Windows: диски определяются по буквам. Логика перенесена из проверенной на железе
// версии на чистом WinAPI (refactor.cpp).
#include "Platform.h"

#include <QDir>
#include <QStringList>
#include <QThread>

#include <windows.h>
#include <winioctl.h>

#include <cwchar>
#include <string>

namespace Platform {
namespace {

QString nativeRoot(const QString& root) { return QDir::toNativeSeparators(root); }

// Все съёмные диски (буквы), в том числе без носителя. Формат: "D:/"
QStringList allRemovableRoots() {
    QStringList roots;
    wchar_t buf[512] = { 0 };
    const DWORD n = GetLogicalDriveStringsW(511, buf);
    if (n == 0 || n > 511) return roots;
    for (const wchar_t* d = buf; *d; d += std::wcslen(d) + 1) {
        if (GetDriveTypeW(d) == DRIVE_REMOVABLE) {
            roots << QDir::fromNativeSeparators(QString::fromWCharArray(d));
        }
    }
    return roots;
}

// true, если в диске есть готовый носитель (том читается).
// Сразу после появления буквы Windows может ещё не дочитать метку тома
// (GetVolumeInformationW возвращает ошибку) - поэтому несколько попыток.
bool volumeLabel(const QString& root, QString* label) {
    wchar_t buf[MAX_PATH + 1] = { 0 };
    const std::wstring w = nativeRoot(root).toStdWString();
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (GetVolumeInformationW(w.c_str(), buf, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0)) {
            if (label) *label = QString::fromWCharArray(buf);
            return true;
        }
        QThread::msleep(200);
    }
    return false;
}

// Все логические диски (буквы), независимо от типа. Нужно для поиска RPI-RP2:
// RP2040 в BOOTSEL на Windows иногда определяется как DRIVE_FIXED, а не DRIVE_REMOVABLE.
QStringList allDriveRoots() {
    QStringList roots;
    wchar_t buf[512] = { 0 };
    const DWORD n = GetLogicalDriveStringsW(511, buf);
    if (n == 0 || n > 511) return roots;
    for (const wchar_t* d = buf; *d; d += std::wcslen(d) + 1) {
        roots << QDir::fromNativeSeparators(QString::fromWCharArray(d));
    }
    return roots;
}

QString driveTypeName(const QString& root) {
    const std::wstring w = nativeRoot(root).toStdWString();
    switch (GetDriveTypeW(w.c_str())) {
    case DRIVE_REMOVABLE: return QStringLiteral("removable");
    case DRIVE_FIXED:     return QStringLiteral("fixed");
    case DRIVE_REMOTE:    return QStringLiteral("remote");
    case DRIVE_CDROM:     return QStringLiteral("cdrom");
    case DRIVE_RAMDISK:   return QStringLiteral("ramdisk");
    case DRIVE_NO_ROOT_DIR: return QStringLiteral("no-root");
    default:              return QStringLiteral("unknown");
    }
}

Candidate makeCandidate(const QString& root, const QString& label) {
    Candidate c;
    c.diskId = root;                         // "D:/"
    c.diskNode = root.left(2);               // "D:"
    c.node = QStringLiteral("\\\\.\\") + root.left(2);
    c.label = label;
    c.mountPoint = root;                     // Windows монтирует сама
    return c;
}

}  // namespace

QSet<QString> readyRemovableDisks() {
    QSet<QString> ready;
    for (const QString& r : allRemovableRoots()) {
        if (volumeLabel(r, nullptr)) ready.insert(r);   // буква без носителя в снимок не входит
    }
    return ready;
}

QList<Candidate> newDisks(const QSet<QString>& before) {
    QList<Candidate> out;
    for (const QString& r : allRemovableRoots()) {
        if (before.contains(r)) continue;
        QString label;
        if (!volumeLabel(r, &label)) continue;                                   // носитель ещё не готов
        if (label.compare(QStringLiteral("RPI-RP2"), Qt::CaseInsensitive) == 0) continue;
        if (!QDir(r).exists()) continue;
        out << makeCandidate(r, label);
    }
    return out;
}

QList<Candidate> bootloaderDisks() {
    QList<Candidate> out;
    // Ищем по метке среди ВСЕХ логических дисков: RP2040 в BOOTSEL на Windows
    // иногда определяется как DRIVE_FIXED, и фильтр по DRIVE_REMOVABLE его терял.
    for (const QString& r : allDriveRoots()) {
        QString label;
        if (volumeLabel(r, &label) && label.compare(QStringLiteral("RPI-RP2"), Qt::CaseInsensitive) == 0) {
            out << makeCandidate(r, label);
        }
    }
    return out;
}

bool mountCandidate(Candidate& c, QString* detail) {
    Q_UNUSED(detail);
    return !c.mountPoint.isEmpty();          // на Windows том уже смонтирован
}

bool ejectCandidate(const Candidate& c, QString* detail) {
    auto setDetail = [&](const QString& s) { if (detail) *detail = s; };
    setDetail(QString());

    const std::wstring vol = c.node.toStdWString();
    HANDLE h = CreateFileW(vol.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        setDetail(QStringLiteral("cannot open volume (error %1)").arg(GetLastError()));
        return false;
    }

    DWORD br = 0;
    bool locked = false;
    for (int i = 0; i < 10 && !locked; ++i) {
        if (DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &br, nullptr)) locked = true;
        else QThread::msleep(500);
    }
    if (!locked) {                           // без lock том не размонтируется
        CloseHandle(h);
        setDetail(QStringLiteral("volume is busy and cannot be locked"));
        return false;
    }

    FlushFileBuffers(h);
    DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &br, nullptr);

    PREVENT_MEDIA_REMOVAL pmr;
    pmr.PreventMediaRemoval = FALSE;
    DeviceIoControl(h, IOCTL_STORAGE_MEDIA_REMOVAL, &pmr, sizeof(pmr), nullptr, 0, &br, nullptr);

    const BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &br, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok) {
        setDetail(QStringLiteral("eject request failed (error %1)").arg(err));
        return false;
    }
    return true;
}

int cleanupVolumeJunk(const QString&, const QStringList&) {
    return 0;       // Windows ничего не создаёт в корне тома при записи ядер
}

QString diagnostics() {
    QStringList lines;
    const QStringList roots = allDriveRoots();
    if (roots.isEmpty()) lines << QStringLiteral("Diagnostics: no logical drives are visible.");
    for (const QString& r : roots) {
        QString label;
        const bool ready = volumeLabel(r, &label);
        lines << QStringLiteral("Diagnostics: %1 type=%2 %3")
                     .arg(r, driveTypeName(r),
                          ready ? QStringLiteral("label [") + label + QStringLiteral("]")
                                : QStringLiteral("no media / not ready"));
    }
    return lines.join(QLatin1Char('\n'));
}

}  // namespace Platform
