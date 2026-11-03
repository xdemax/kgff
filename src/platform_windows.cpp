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

// Буква системного диска (там, где стоит Windows), например "C:/". Пусто, если
// определить не удалось. Нужна, чтобы не принять системный диск за SD-карту Карабаса.
QString systemDriveRoot() {
    wchar_t win[MAX_PATH + 1] = { 0 };
    const UINT n = GetWindowsDirectoryW(win, MAX_PATH);
    if (n == 0 || n > MAX_PATH) return QString();
    const QString path = QDir::fromNativeSeparators(QString::fromWCharArray(win));
    if (path.size() < 2 || path.at(1) != QLatin1Char(':')) return QString();
    return path.left(2) + QLatin1Char('/');
}

// Буква диска, на котором лежит сама программа. Пусто, если определить не удалось.
QString appDriveRoot() {
    wchar_t exe[MAX_PATH + 1] = { 0 };
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n > MAX_PATH) return QString();
    const QString path = QDir::fromNativeSeparators(QString::fromWCharArray(exe));
    if (path.size() < 2 || path.at(1) != QLatin1Char(':')) return QString();
    return path.left(2) + QLatin1Char('/');
}

// Диски-кандидаты для SD-карты Карабаса: FIXED и REMOVABLE, кроме системного диска
// и диска, на котором лежит сама программа. RP2040 в режиме MSC на Windows иногда
// определяется как DRIVE_FIXED, поэтому фильтр только по DRIVE_REMOVABLE терял карту.
// Сетевые (DRIVE_REMOTE), CD-ROM и диски без корневого каталога пропускаем: чтение
// метки на отключённой шаре может блокироваться на десятки секунд.
// Запись на чужой диск дополнительно страхует Worker::confirmDrive().
QStringList candidateRoots() {
    QStringList roots;
    const QString sys = systemDriveRoot();
    const QString app = appDriveRoot();
    wchar_t buf[512] = { 0 };
    const DWORD n = GetLogicalDriveStringsW(511, buf);
    if (n == 0 || n > 511) return roots;
    for (const wchar_t* d = buf; *d; d += std::wcslen(d) + 1) {
        const UINT type = GetDriveTypeW(d);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        const QString r = QDir::fromNativeSeparators(QString::fromWCharArray(d));
        if (!sys.isEmpty() && r.compare(sys, Qt::CaseInsensitive) == 0) continue;
        if (!app.isEmpty() && r.compare(app, Qt::CaseInsensitive) == 0) continue;
        roots << r;
    }
    return roots;
}

// true, если в диске есть готовый носитель (том читается).
// Одна попытка без ретраев: функция вызывается в циклах опроса (каждые 500 мс),
// и повторные попытки на пустом слоте кардридера или приводе без диска
// добавляли бы около секунды на каждый вызов. Готовность носителя проверяется
// на следующей итерации цикла опроса.
bool volumeLabel(const QString& root, QString* label) {
    wchar_t buf[MAX_PATH + 1] = { 0 };
    const std::wstring w = nativeRoot(root).toStdWString();
    if (GetVolumeInformationW(w.c_str(), buf, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0)) {
        if (label) *label = QString::fromWCharArray(buf);
        return true;
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
    for (const QString& r : candidateRoots()) {
        if (volumeLabel(r, nullptr)) ready.insert(r);   // буква без носителя в снимок не входит
    }
    return ready;
}

QList<Candidate> newDisks(const QSet<QString>& before) {
    QList<Candidate> out;
    for (const QString& r : candidateRoots()) {
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
    // Ищем по метке среди логических дисков типов FIXED и REMOVABLE: RP2040 в BOOTSEL
    // на Windows иногда определяется как DRIVE_FIXED, и фильтр только по DRIVE_REMOVABLE
    // его терял. Сетевые (DRIVE_REMOTE), CD-ROM и диски без корневого каталога
    // пропускаем: GetVolumeInformationW на отключённой сетевой шаре может блокироваться
    // на десятки секунд, а bootloaderDisks() вызывается перед каждой прошивкой.
    for (const QString& r : allDriveRoots()) {
        const std::wstring w = nativeRoot(r).toStdWString();
        const UINT type = GetDriveTypeW(w.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
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

    // SD-карта Карабаса в режиме MSC может определяться как DRIVE_FIXED (RP2040 иногда
    // так перечисляется), поэтому FIXED попадает в кандидаты. Но извлекать внутренний
    // жёсткий диск нельзя: LOCK+DISMOUNT на простаивающем системном томе - опасная
    // операция. Отказываемся явно, а не пытаемся извлечь.
    const std::wstring root = nativeRoot(c.mountPoint).toStdWString();
    if (GetDriveTypeW(root.c_str()) == DRIVE_FIXED) {
        setDetail(QStringLiteral("refusing to eject a fixed drive (%1); it is not a removable SD card")
                      .arg(c.mountPoint));
        return false;
    }

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
        const QString typeName = driveTypeName(r);
        // Метку читаем только у FIXED/REMOVABLE: GetVolumeInformationW на отключённой
        // сетевой шаре или пустом CD-ROM может блокироваться на десятки секунд,
        // а диагностика вызывается в том числе при ошибках поиска диска.
        const bool canRead = typeName == QLatin1String("fixed") || typeName == QLatin1String("removable");
        QString label;
        const bool ready = canRead && volumeLabel(r, &label);
        lines << QStringLiteral("Diagnostics: %1 type=%2 %3")
                     .arg(r, typeName,
                          ready ? QStringLiteral("label [") + label + QStringLiteral("]")
                                : QStringLiteral("no media / not ready"));
    }
    return lines.join(QLatin1Char('\n'));
}

}  // namespace Platform
