#pragma once
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <atomic>
#include <functional>

#include "Models.h"
#include "Platform.h"

class QSaveFile;

struct Params {
    int modelIndex = -1;
    bool updateFirmware = true;     // скачать/прошить прошивку RP2040
    bool updateCores = false;       // скачать/заменить ядра на SD-карте
    bool updateExtra = false;       // скачать/скопировать доп. контент на SD-карту
    QString portLocation;       // например /dev/ttyACM0; пусто = пропустить сброс
    QString firmwarePath;
    int mscMode = 1;            // режим MSC: 1 -> AT+KARABAS=msc, 2 -> AT+KARABAS=msc2
};

// Фоновый исполнитель. Живёт в отдельном QThread, управляется из GUI через
// QMetaObject::invokeMethod. Отмена - флаг m_cancel, проверяется только в безопасных точках.
class Worker : public QObject {
    Q_OBJECT
public:
    explicit Worker(QObject* parent = nullptr) : QObject(parent) {}

    void requestCancel() { m_cancel.store(true); }     // можно вызывать из GUI-потока
    void clearCancel() { m_cancel.store(false); }      // сброс флага до постановки задачи в очередь

    void runDownload(const Params& p);
    void runFlash(const Params& p);
    void runMsc(const Params& p);
    void runDetectModel(const Params& p);

signals:
    void log(const QString& message);
    void firmwareReady(const QString& path);
    void finished();                                    // операция завершена (успешно или нет)
    void portAccessDenied(const QString& portLocation); // нет прав на порт (Linux)
    void openFolder(const QString& path);               // открыть папку в файловом менеджере
    void modelDetected(int modelIndex, const QString& raw); // ответ на AT+KARABAS=version
    void portsChanged();                                // список портов мог измениться (MSC, сброс)

private:
    struct HttpResult {
        bool ok = false;
        int status = 0;
        QString error;
        QByteArray body;
    };

    struct DownloadJob {
        QUrl url;
        QString dest;
        QString name;
    };
    struct JobResult {
        bool ok = false;
        QString error;
        qint64 bytes = 0;
    };

    void guarded(const std::function<void()>& fn);
    bool pause(int ms);                                 // false, если запрошена отмена

    HttpResult httpFetch(const QUrl& url, qint64 maxBytes, QSaveFile* file);
    bool downloadToFile(const QUrl& url, const QString& dest, QString* err,
                        qint64 maxBytes = 64 * 1024 * 1024);
    // Параллельная загрузка нескольких файлов через ОДИН QNetworkAccessManager
    // (общее соединение, до maxParallel файлов одновременно).
    QList<JobResult> downloadFiles(const QList<DownloadJob>& jobs, int maxParallel);
    QStringList fetchCoresList(const ModelInfo& m);

    void doDownload(const Params& p);
    void doFlash(const Params& p);
    void doOpenSd(const Params& p);

    // Универсальная процедура: открыть порт, отправить AT-команду, дождаться ответа.
    // Возвращает true, если ответ получен (resp заполнен). Если ответ содержит "ERROR",
    // возвращает false и заполняет resp текстом ошибки.
    bool sendAtCommand(const QString& portLocation, const QByteArray& cmd,
                       QByteArray* resp, int timeoutMs);
    bool sendMscCommand(const QString& portLocation, int mscMode);
    bool queryVersion(const QString& portLocation, QString* raw, int* board);
    bool resetPortToBootloader(const QString& portLocation);
    bool isPortPresent(const QString& portLocation);

    bool waitForMountedVolume(const std::function<QList<Platform::Candidate>()>& finder,
                              int timeoutMs, Platform::Candidate* out);
    bool updateCoresOnSd(const ModelInfo& m, const QString& portLocation,
                         const QStringList& newCores, bool updateExtra, bool updateFirmware);
    void rollbackCores(const QStringList& copiedNew, const QList<QPair<QString, QString>>& movedOld);
    bool ejectBestEffort(const Platform::Candidate& sd);
    // Удаляет служебные файлы ОС (macOS: "._*", .DS_Store и т.п.) с тома перед извлечением.
    void cleanupVolumeJunk(const QString& root, const QString& backupDir);

    // Extra content: скачивание extra.zip, распаковка через minizip-ng, копирование на SD.
    bool downloadExtraArchive(const QString& destPath);
    bool extractExtraArchive(const QString& archivePath, const QString& outDir);
    bool copyExtraToSd(const QString& srcDir, const QString& sdRoot);

    std::atomic<bool> m_cancel{false};
    bool m_permissionDenied = false;                    // только рабочий поток
    bool m_portOpenFailed = false;                      // порт не открылся или запись не удалась
};
