#pragma once
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

#include <atomic>
#include <functional>

#include "CoreFile.h"
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
    // true - автодетект модели при старте: "нет ответа" не считается ошибкой,
    // пользователь сам выберет модель или нажмёт лупу.
    bool autoDetect = false;
    // Выбор ядер в окне сравнения версий. Если coreSelectionSet=false, копируются
    // все скачанные ядра (старое поведение); если true — только listed.
    bool coreSelectionSet = false;
    QStringList selectedCoreFiles;
    QStringList ignoredCoreFiles;   // постоянный список игнорируемых ядер
    // Все ядра из текущего релиза (даже не скачанные): по ним понятно, какие
    // старые файлы считаются релизными и могут быть перенесены в backup.
    QStringList knownCoreFiles;
    // Двухшаговое скачивание ядер: сначала только заголовки/версии, затем
    // полные файлы лишь для выбранных пользователем ядер.
    bool prepareCores = false;
    bool downloadSelectedCores = false;
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
    // Стартовая проверка последних сборок прошивки для всех моделей.
    void runStartupFirmwareCheck();

signals:
    void log(const QString& message);
    // Подробные сообщения (диагностика, повторы, служебные шаги): пишутся только в файл
    // журнала, в окно не выводятся, чтобы не засорять его.
    void logVerbose(const QString& message);
    void firmwareReady(const QString& path);
    void finished();                                    // операция завершена (успешно или нет)
    void portAccessDenied(const QString& portLocation); // нет прав на порт (Linux)
    void openFolder(const QString& path);               // открыть папку в файловом менеджере
    void modelDetected(int modelIndex, const QString& raw); // ответ на AT+KARABAS=version
    // Строки сравнения версий: file, installed, downloaded (доступная версия),
    // state (same/newer/older/missing/unknown/notSupported/parseError), detail.
    // availableList=true: версии прочитаны из заголовков на GitHub, полные файлы
    // ещё не скачаны; false: список построен после полного скачивания.
    void coreVersionsReady(const QVariantList& rows, const QString& boardName, bool installedKnown,
                           bool availableList);
    // Проверка версий на старте закончена (успешно или нет) — можно запускать
    // обычное автоопределение модели.
    void startupFirmwareCheckFinished();
    // Статус прошивки для панели ядер: known=false — версия неизвестна.
    void firmwareStatusReady(bool known, bool needsUpdate,
                             const QString& installed, const QString& latest);
    // Доступна более новая версия самого флешера.
    void appUpdateAvailable(const QString& version, const QString& url);
    void portsChanged();                                // список портов мог измениться (MSC, сброс)
    // Запрос подтверждения записи на диск без признаков Карабаса. Отправляется из
    // рабочего потока через Qt::BlockingQueuedConnection: GUI показывает диалог и
    // записывает ответ в *accepted. Если соединение не установлено, accepted остаётся
    // false (безопасное поведение - отказ).
    void confirmUnrecognizedDrive(const QString& node, const QString& label, qint64 bytesTotal,
                                  const QStringList& sampleFiles, bool* accepted);
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

    struct RemoteCoreInfo {
        QString name;
        qint64 size = 0;
        QUrl url;
    };

    struct RangeResult {
        bool ok = false;
        QByteArray body;
        qint64 totalSize = -1;
        QString error;
    };

    void guarded(const std::function<void()>& fn);
    bool pause(int ms);                                 // false, если запрошена отмена

    HttpResult httpFetch(const QUrl& url, qint64 maxBytes, QSaveFile* file,
                         int transferTimeoutMs = 20000);
    bool downloadToFile(const QUrl& url, const QString& dest, QString* err,
                        qint64 maxBytes = 64 * 1024 * 1024);
    // Параллельная загрузка нескольких файлов через ОДИН QNetworkAccessManager
    // (общее соединение, до maxParallel файлов одновременно).
    QList<JobResult> downloadFiles(const QList<DownloadJob>& jobs, int maxParallel);
    // Параллельное чтение небольших диапазонов (заголовки .kg) без записи на диск.
    QList<RangeResult> fetchRanges(const QList<QUrl>& urls, qint64 maxBytes, int maxParallel);
    // Единый список ядер из репозитория karabas-go (общий для всех моделей).
    QList<RemoteCoreInfo> fetchCoresList();

    void doDownload(const Params& p);
    void prepareCores(const Params& p, const ModelInfo& m);
    void downloadSelectedCores(const Params& p, const ModelInfo& m);
    void doFlash(const Params& p);
    void doOpenSd(const Params& p);

    // Универсальная процедура: открыть порт, отправить AT-команду, дождаться ответа.
    // Возвращает true, если пришли байты (resp заполнен); строку ERROR разбирает вызывающий.
    // waitForOk=false: чтение до строки +KARABAS: (version); true — до OK/ERROR (list).
    bool sendAtCommand(const QString& portLocation, const QByteArray& cmd,
                       QByteArray* resp, int timeoutMs, bool waitForOk = false);
    bool sendMscCommand(const QString& portLocation, int mscMode);
    bool queryVersion(const QString& portLocation, QString* raw, int* board,
                      QString* firmware = nullptr);
    // Второй запрос после version: список ядер на SD-карте и их версии.
    bool queryCoreList(const QString& portLocation, int board, QList<CoreFileInfo>* out);
    // Актуальная сборка прошивки из истории каталога firmware на GitHub.
    bool fetchLatestFirmwareVersion(const QString& firmwareFile, QString* version, QString* error);
    // Сравнить известную версию платы с последней на GitHub и обновить панель.
    void emitFirmwareStatus(int modelIndex = -1);
    // Проверка последнего релиза самого Karabas Flasher.
    void checkFlasherUpdate();
    bool resetPortToBootloader(const QString& portLocation);
    bool isPortPresent(const QString& portLocation);
    // Реальная проба: открыть порт и сразу закрыть, ничего не записывая.
    // isPortPresent() недостаточно: после MSC-сеанса Windows может ещё показывать
    // «мёртвый» COM-порт в списке, и проверка дала бы ложное «чужая плата».
    bool portOpens(const QString& portLocation);
    // true, если порт Карабаса ещё жив (реально открывается), а диск RPI-RP2 уже виден:
    // значит, этот диск принадлежит ДРУГОЙ RP2040-плате (у живого Карабаса в нормальном
    // режиме RPI-RP2 быть не может). Прошивка ушла бы на чужую плату.
    bool foreignBootloaderPresent(const QString& portLocation);

    bool waitForMountedVolume(const std::function<QList<Platform::Candidate>()>& finder,
                              int timeoutMs, Platform::Candidate* out);

    // Проверка «тот ли это диск» перед записью. Возвращает true, если можно писать:
    //   - на диске есть признаки Карабаса (ядра .kg* или папка backup_*) - без вопросов;
    //   - диск пустой (чистая карта) - с предупреждением в лог;
    //   - файлы есть, но признаков нет - спрашивает пользователя через сигнал.
    bool confirmDrive(const Platform::Candidate& c);

    // Результат MSC-сеанса: ядра и extra обновляются в одном сеансе, но их статусы
    // независимы. abortReason непустой, если сеанс прерван до записи (сводка всё равно печатается).
    struct MscResult {
        bool coresOk = false;
        bool extraOk = false;
        bool extraAttempted = false;
        // true, если набор extra был скачан и распакован на момент шага копирования.
        // Нужно, чтобы отличить "не скачан" от "скачан, но копирование не удалось".
        bool extraDownloaded = false;
        QString abortReason;        // пусто, если сеанс дошёл до конца
    };
    // ignoredCores — имена release-ядер из GitHub, которые пользователь
    // игнорирует: их старые файлы тоже переносятся в backup, новые не копируются.
    MscResult updateCoresOnSd(const QString& portLocation, const QStringList& newCores,
                              bool updateExtra, bool updateFirmware,
                              const QSet<QString>& ignoredCores);
    // MSC-сеанс «только extra»: переключение в MSC, ожидание диска, копирование, eject.
    // Возвращает тот же MscResult, что и updateCoresOnSd, чтобы doFlash обрабатывал
    // прерывание единообразно.
    MscResult copyExtraViaMsc(const QString& portLocation, const QString& extraFilesDir);
    void rollbackCores(const QStringList& copiedNew, const QList<QPair<QString, QString>>& movedOld);
    bool ejectBestEffort(const Platform::Candidate& sd);
    // Удаляет служебные файлы ОС (macOS: "._*", .DS_Store и т.п.) с тома перед извлечением.
    void cleanupVolumeJunk(const QString& root, const QString& backupDir);

    // Extra content: скачивание extra.zip, распаковка через minizip-ng, копирование на SD.
    bool downloadExtraArchive(const QString& destPath);
    bool extractExtraArchive(const QString& archivePath, const QString& outDir);
    bool copyExtraToSd(const QString& srcDir, const QString& sdRoot);

    // Последний успешный AT+KARABAS=list. Хранится в рабочем потоке и используется
    // для сравнения с только что скачанными ядрами.
    QList<CoreFileInfo> m_deviceCores;
    int m_deviceCoresBoard = 0;
    bool m_deviceCoresValid = false;
    QHash<int, QString> m_latestFirmwareVersions;   // model index -> build
    QString m_boardFirmwareVersion;                 // build из AT+KARABAS=version
    int m_boardModelIndex = -1;                     // модель, к которой относится build

    std::atomic<bool> m_cancel{false};
    bool m_permissionDenied = false;                    // только рабочий поток
    bool m_portOpenFailed = false;                      // порт не открылся или запись не удалась
    // Порт в списке, но открыть его нельзя (DeviceNotFoundError): после MSC-сеанса Windows
    // ещё показывает «мёртвый» COM-порт. Считаем такой порт исчезнувшим.
    bool m_portDeviceNotFound = false;
};
