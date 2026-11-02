#pragma once
#include <QCloseEvent>
#include <QFile>
#include <QString>
#include <QWidget>

#include "Worker.h"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QThread;
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
    void onFlash();
    void onMsc();
    void onDetectModel();
    void onModelDetected(int modelIndex, const QString& raw);
    void appendLog(const QString& message);
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
    void openLogFile();
    void cleanupTemp();
    void stopThread();

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
    QPushButton* m_refreshBtn = nullptr;
    QPushButton* m_flashBtn = nullptr;
    QPushButton* m_mscBtn = nullptr;
    QPushButton* m_mscMode1Btn = nullptr;  // режим MSC 1 (AT+KARABAS=msc)
    QPushButton* m_mscMode2Btn = nullptr;  // режим MSC 2 (AT+KARABAS=msc2)
    QButtonGroup* m_mscModeGroup = nullptr; // взаимоисключающая группа кнопок режима MSC
    QToolButton* m_mscHelp = nullptr;      // значок «?» с подсказкой про режим кард-ридера
    QPlainTextEdit* m_log = nullptr;

    QProcess* m_accessProc = nullptr;     // pkexec: установка правила udev
    QThread* m_thread = nullptr;
    Worker* m_worker = nullptr;

    QString m_firmwarePath;
    int m_firmwareModelIndex = -1;        // модель, для которой скачана/выбрана прошивка
    bool m_busy = false;
    bool m_closeRequested = false;
    bool m_accessRuleInstalled = false;   // правило udev уже ставилось в этом запуске

    bool m_fileLogging = false;
    QFile m_logFile;
};
