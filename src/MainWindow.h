#pragma once
#include <QCloseEvent>
#include <QFile>
#include <QString>
#include <QWidget>

#include "Worker.h"

class QButtonGroup;
class QCheckBox;
class CoreVersionsPanel;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QThread;
class QTimer;
class QToolButton;

class MainWindow : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(bool fileLogging, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onDownload();
    void onBrowse();
    void onRefreshPorts();
    void pollSerialPorts();
    void onFlash();
    void onMsc();
    void onDetectModel();
    void onAutoDetectModel();
    void onStartupFirmwareCheck();
    void onStartupFirmwareCheckFinished();
    void startStartupFlow();
    void onModelDetected(int modelIndex, const QString& raw);
    void appendLog(const QString& message);
    void appendLogVerbose(const QString& message);
    void onCoreVersionsReady(const QVariantList& rows, const QString& boardName, bool installedKnown,
                             bool availableList);
    void onFirmwareStatusReady(bool known, bool needsUpdate,
                               const QString& installed, const QString& latest);
    void onAppUpdateAvailable(const QString& version, const QString& url);
    void onFirmwareReady(const QString& path);
    void onWorkerFinished();
    void onPortAccessDenied(const QString& portLocation);
    void onOpenFolder(const QString& path);
    void onConfirmUnrecognizedDrive(const QString& node, const QString& label, qint64 bytesTotal,
                                    const QStringList& sampleFiles, bool* accepted);

private:
    Params collectParams() const;
    void setBusy(bool busy);
    void applyOptionState();            // доступность поля прошивки зависит от галки Update firmware
    void updateActionState();           // доступность FLASH!/MSC по наличию входных данных
    void updatePortList(bool userRequested);
    void updatePortPolling();
    void maybeAutoDetectPort();
    void openLogFile();
    void writeLogLine(const QString& line);
    void cleanupTemp();
    void stopThread();
    void showCorePanel();
    void hideCorePanel();
    void centerWindowForWidth(int outerWidth);

    QLabel* m_appUpdateLabel = nullptr;    // ссылка на новый релиз флешера
    QComboBox* m_modelCombo = nullptr;
    QPushButton* m_detectBtn = nullptr;    // кнопка-лупа: автоопределение модели
    QPushButton* m_downloadBtn = nullptr;
    QCheckBox* m_fwChk = nullptr;
    QCheckBox* m_extraChk = nullptr;      // скачать доп. контент и скопировать на карту
    QCheckBox* m_coresChk = nullptr;
    QCheckBox* m_delTempChk = nullptr;
    QLineEdit* m_pathEdit = nullptr;
    QPushButton* m_browseBtn = nullptr;
    QComboBox* m_comCombo = nullptr;
    QTimer* m_portPollTimer = nullptr;
    QPushButton* m_refreshBtn = nullptr;
    QPushButton* m_flashBtn = nullptr;
    QPushButton* m_mscBtn = nullptr;
    QPushButton* m_mscMode1Btn = nullptr;  // режим MSC 1 (AT+KARABAS=msc)
    QPushButton* m_mscMode2Btn = nullptr;  // режим MSC 2 (AT+KARABAS=msc2)
    QButtonGroup* m_mscModeGroup = nullptr; // взаимоисключающая группа кнопок режима MSC
    QToolButton* m_mscHelp = nullptr;      // значок «?» с подсказкой про режим кард-ридера
    QToolButton* m_panelToggle = nullptr;  // «>>»/«<<»: свернуть/развернуть панель ядер
    CoreVersionsPanel* m_corePanel = nullptr;
    QPlainTextEdit* m_log = nullptr;

    QProcess* m_accessProc = nullptr;     // pkexec: установка правила udev
    QThread* m_thread = nullptr;
    Worker* m_worker = nullptr;

    QString m_firmwarePath;
    int m_firmwareModelIndex = -1;        // модель, для которой скачана/выбрана прошивка
    int m_collapsedWidth = 0;             // ширина окна до раскрытия панели ядер
    QString m_portSignature;              // чтобы не перестраивать список каждый опрос
    QString m_controlPortLocation;        // текущий control-порт
    QString m_lastAutoDetectedPort;       // чтобы не повторять автоопределение
    bool m_startupCheckDone = false;
    bool m_startupDetectionPending = false;
    bool m_startupFirmwareCheckStarted = false;
    bool m_coreListPrepared = false;      // список ядер с GitHub уже получен
    bool m_flashStarted = false;
    bool m_flashCores = false;            // в запущенном FLASH обновляются ядра
    bool m_busy = false;
    bool m_closeRequested = false;
    bool m_accessRuleInstalled = false;   // правило udev уже ставилось в этом запуске

    bool m_fileLogging = false;
    QFile m_logFile;
};
