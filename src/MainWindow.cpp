#include "MainWindow.h"

#include "CoreVersionsPanel.h"
#include "Models.h"
#include "Uf2.h"
#include "Util.h"
#include "Version.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QMetaType>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QSerialPortInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QCursor>
#include <QVBoxLayout>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#endif

#include <algorithm>

namespace {

enum class KarabasPortRole {
    Unknown,
    Control,        // interface 0: log, DIAG, AT+KARABAS
    CoreSerial,     // interface 2: UART of the core
};

#if defined(Q_OS_WIN)
// На Windows описание COM-порта локализовано и часто одинаково у обоих
// интерфейсов ("USB Serial Device"). Роль определяем по USB composite
// interface: MI_00 — control, MI_02 — core serial.
//
// Основной путь — SetupAPI: он даёт device instance id и связанное значение
// PortName даже при локализованном описании. Реестр ниже оставлен запасным.
QHash<QString, KarabasPortRole> windowsPortRolesSetupApi() {
    QHash<QString, KarabasPortRole> roles;
    HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) return roles;

    SP_DEVINFO_DATA data;
    data.cbSize = sizeof(data);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &data); ++index) {
        wchar_t instanceId[512];
        if (!SetupDiGetDeviceInstanceIdW(devices, &data, instanceId,
                                         static_cast<DWORD>(sizeof(instanceId) / sizeof(instanceId[0])),
                                         nullptr)) {
            continue;
        }
        const QString id = QString::fromWCharArray(instanceId);
        if (!id.contains(QStringLiteral("VID_2E8A&PID_000A"), Qt::CaseInsensitive)) continue;

        const bool control = id.contains(QStringLiteral("&MI_00"), Qt::CaseInsensitive);
        const bool coreSerial = id.contains(QStringLiteral("&MI_02"), Qt::CaseInsensitive);
        if (!control && !coreSerial) continue;

        HKEY key = SetupDiOpenDevRegKey(devices, &data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        wchar_t portName[64] = {};
        DWORD size = sizeof(portName);
        DWORD type = 0;
        const LONG result = RegQueryValueExW(key, L"PortName", nullptr, &type,
                                             reinterpret_cast<LPBYTE>(portName), &size);
        RegCloseKey(key);
        if (result != ERROR_SUCCESS || type != REG_SZ) continue;

        const QString port = QString::fromWCharArray(portName).trimmed();
        if (port.isEmpty()) continue;
        roles.insert(port, control ? KarabasPortRole::Control : KarabasPortRole::CoreSerial);
    }
    SetupDiDestroyDeviceInfoList(devices);
    return roles;
}

KarabasPortRole windowsPortRole(const QString& portName) {
    const QHash<QString, KarabasPortRole> setupRoles = windowsPortRolesSetupApi();
    const auto setupIt = setupRoles.constFind(portName);
    if (setupIt != setupRoles.constEnd()) return setupIt.value();

    QSettings usb(QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Enum\\USB"),
                  QSettings::Registry64Format);
    const QStringList devices = usb.childGroups();
    for (const QString& device : devices) {
        if (!device.contains(QStringLiteral("VID_2E8A&PID_000A"), Qt::CaseInsensitive)) continue;
        const bool control = device.contains(QStringLiteral("MI_00"), Qt::CaseInsensitive);
        const bool coreSerial = device.contains(QStringLiteral("MI_02"), Qt::CaseInsensitive);
        if (!control && !coreSerial) continue;

        usb.beginGroup(device);
        const QStringList instances = usb.childGroups();
        for (const QString& instance : instances) {
            usb.beginGroup(instance);
            const QString name =
                usb.value(QStringLiteral("Device Parameters/PortName")).toString();
            usb.endGroup();
            if (name.compare(portName, Qt::CaseInsensitive) == 0) {
                usb.endGroup();
                return control ? KarabasPortRole::Control : KarabasPortRole::CoreSerial;
            }
        }
        usb.endGroup();
    }
    return KarabasPortRole::Unknown;
}
#endif

// С прошивки 2026-11-19 у платы два CDC-порта. Для флешера нужен control
// (interface 0), core serial (interface 2) — это чистый UART ядра.
KarabasPortRole karabasPortRole(const QSerialPortInfo& info) {
    if (!info.hasVendorIdentifier() || !info.hasProductIdentifier() ||
        info.vendorIdentifier() != 0x2e8a || info.productIdentifier() != 0x000a) {
        return KarabasPortRole::Unknown;
    }

#if defined(Q_OS_WIN)
    const KarabasPortRole windowsRole = windowsPortRole(info.portName());
    if (windowsRole != KarabasPortRole::Unknown) return windowsRole;
#endif

#if defined(Q_OS_LINUX)
    // Самый надёжный признак: номер интерфейса в sysfs.
    QFile number(QStringLiteral("/sys/class/tty/%1/device/bInterfaceNumber").arg(info.portName()));
    if (number.open(QIODevice::ReadOnly)) {
        const QString value = QString::fromLatin1(number.readAll()).trimmed();
        if (value == QLatin1String("00")) return KarabasPortRole::Control;
        if (value == QLatin1String("02")) return KarabasPortRole::CoreSerial;
    }
    // Запасной вариант — стабильные имена /dev/serial/by-id/*-if00 / *-if02.
    const QString target = QFileInfo(info.systemLocation()).canonicalFilePath();
    if (!target.isEmpty()) {
        const QFileInfoList links = QDir(QStringLiteral("/dev/serial/by-id"))
            .entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System);
        for (const QFileInfo& link : links) {
            if (link.canonicalFilePath() != target) continue;
            if (link.fileName().endsWith(QLatin1String("-if00"))) return KarabasPortRole::Control;
            if (link.fileName().endsWith(QLatin1String("-if02"))) return KarabasPortRole::CoreSerial;
        }
    }
#endif

    const QString description = info.description();
    if (description.contains(QStringLiteral("core serial"), Qt::CaseInsensitive)) {
        return KarabasPortRole::CoreSerial;
    }
    if (description.contains(QStringLiteral("control"), Qt::CaseInsensitive)) {
        return KarabasPortRole::Control;
    }

#if defined(Q_OS_MACOS)
    // На macOS control — /dev/cu.usbmodem<serial>1, core — ...3.
    const QString location = info.systemLocation();
    if (location.endsWith(QLatin1Char('1'))) return KarabasPortRole::Control;
    if (location.endsWith(QLatin1Char('3'))) return KarabasPortRole::CoreSerial;
#endif

    // Роль не определена: лучше не выбирать порт автоматически, чем угадать.
    return KarabasPortRole::Unknown;
}

}  // namespace

MainWindow::MainWindow(bool fileLogging, QWidget* parent) : QWidget(parent), m_fileLogging(fileLogging) {
    setWindowTitle(QStringLiteral("Karabas Go Family Firmware Flasher v") + QString::fromLatin1(APP_VERSION));
    const QIcon appIcon(QStringLiteral(":/krb.png"));
    if (appIcon.isNull()) {
        qWarning("Icon resource ':/krb.png' could not be loaded (missing file or PNG plugin).");
    }
    setWindowIcon(appIcon);

    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(9, 9, 9, 9);
    auto* left = new QWidget(this);
    auto* root = new QVBoxLayout(left);
    root->setContentsMargins(0, 0, 0, 0);

    m_appUpdateLabel = new QLabel;
    m_appUpdateLabel->setTextFormat(Qt::RichText);
    m_appUpdateLabel->setOpenExternalLinks(true);
    m_appUpdateLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_appUpdateLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_appUpdateLabel->setVisible(false);

    // Уведомление об обновлении — на одной строке с "Select Device Model:",
    // справа, над кнопкой Download via GitHub.
    auto* modelHeader = new QHBoxLayout;
    modelHeader->addWidget(new QLabel(tr("Select Device Model:")));
    modelHeader->addStretch();
    modelHeader->addWidget(m_appUpdateLabel, 0, Qt::AlignRight | Qt::AlignVCenter);
    root->addLayout(modelHeader);
    auto* row1 = new QHBoxLayout;
    m_modelCombo = new QComboBox;
    m_modelCombo->setMinimumHeight(28);
    m_modelCombo->setMaximumHeight(28);
    for (const ModelInfo& m : models()) m_modelCombo->addItem(m.name);
    m_detectBtn = new QPushButton;
    m_detectBtn->setFixedSize(32, 28);
    m_detectBtn->setToolTip(tr("Automatic model detection"));
    // Иконка лупы рисуется вручную: эмодзи 🔍 есть не во всех шрифтах (на Linux вместо него
    // появляются «кракозябры»), а QStyle-иконки на разных платформах выглядят по-разному.
    {
        QPixmap pm(16, 16);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(palette().color(QPalette::ButtonText));
        pen.setWidthF(1.8);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(2.0, 2.0, 9.0, 9.0));
        p.drawLine(QPointF(10.0, 10.0), QPointF(14.0, 14.0));
        p.end();
        m_detectBtn->setIcon(QIcon(pm));
        m_detectBtn->setIconSize(QSize(16, 16));
    }
    m_downloadBtn = new QPushButton(tr("Download via GitHub"));
    // Ширина правых кнопок (Browse..., Refresh ports) подгоняется под "Download via GitHub",
    // чтобы правый край всех строк выглядел ровно.
    const int rightBtnWidth = m_downloadBtn->sizeHint().width();
    row1->addWidget(m_modelCombo, 1);
    row1->addWidget(m_detectBtn, 0);
    row1->addWidget(m_downloadBtn);
    root->addLayout(row1);

    m_fwChk = new QCheckBox(tr("Update firmware"));
    m_fwChk->setChecked(false);
    m_fwChk->setToolTip(tr("Download and flash the RP2040 firmware of the board."));
    m_extraChk = new QCheckBox(tr("Download extra content (disabled)"));
    m_extraChk->setChecked(false);
    m_extraChk->setEnabled(false);
    m_extraChk->setToolTip(tr("Extra content is temporarily disabled."));
    m_coresChk = new QCheckBox(tr("Update cores"));
    m_coresChk->setToolTip(tr("Download the FPGA cores and replace them on the SD card. "
                              "The old cores are saved to a backup folder on the card."));
    m_delTempChk = new QCheckBox(tr("Delete temp files on exit"));
    m_delTempChk->setChecked(true);
    // Чекбоксы в две колонки: 2x2
    auto* chkGrid = new QGridLayout;
    chkGrid->setContentsMargins(0, 0, 0, 0);
    chkGrid->setHorizontalSpacing(16);
    chkGrid->setVerticalSpacing(4);
    chkGrid->addWidget(m_fwChk, 0, 0, Qt::AlignLeft);
    chkGrid->addWidget(m_extraChk, 0, 1, Qt::AlignRight);
    chkGrid->addWidget(m_coresChk, 1, 0, Qt::AlignLeft);
    chkGrid->addWidget(m_delTempChk, 1, 1, Qt::AlignRight);
    chkGrid->setColumnStretch(0, 1);
    chkGrid->setColumnStretch(1, 1);
    root->addLayout(chkGrid);

    root->addWidget(new QLabel(tr("Firmware File Path (.uf2):")));
    auto* row2 = new QHBoxLayout;
    m_pathEdit = new QLineEdit;
    m_pathEdit->setReadOnly(true);
    m_browseBtn = new QPushButton(tr("Browse..."));
    m_browseBtn->setMinimumWidth(rightBtnWidth);
    row2->addWidget(m_pathEdit, 1);
    row2->addWidget(m_browseBtn);
    root->addLayout(row2);

    root->addWidget(new QLabel(tr("Serial port (choose '(none)' to skip reset):")));
    auto* row3 = new QHBoxLayout;
    m_comCombo = new QComboBox;
    m_refreshBtn = new QPushButton(tr("Refresh ports"));
    m_refreshBtn->setMinimumWidth(rightBtnWidth);
    row3->addWidget(m_comCombo, 1);
    row3->addWidget(m_refreshBtn);
    root->addLayout(row3);

    auto* row4 = new QHBoxLayout;
    m_flashBtn = new QPushButton(tr("FLASH!"));
    m_flashBtn->setMinimumHeight(40);
    m_mscBtn = new QPushButton(tr("Open SD card >"));
    m_mscBtn->setToolTip(tr("Open the SD card in the file manager (USB card reader mode)."));

    // Переключатель режима MSC: 1 -> AT+KARABAS=msc, 2 -> AT+KARABAS=msc2.
    // QButtonGroup с exclusive=true гарантирует, что активна ровно одна кнопка.
    m_mscMode1Btn = new QPushButton(QStringLiteral("1"));
    m_mscMode1Btn->setCheckable(true);
    m_mscMode1Btn->setFixedWidth(32);
    m_mscMode1Btn->setMinimumHeight(40);
    m_mscMode1Btn->setToolTip(tr("MSC mode 1 (AT+KARABAS=msc): main SD card with FPGA cores."));
    m_mscMode2Btn = new QPushButton(QStringLiteral("2"));
    m_mscMode2Btn->setCheckable(true);
    m_mscMode2Btn->setFixedWidth(32);
    m_mscMode2Btn->setMinimumHeight(40);
    m_mscMode2Btn->setToolTip(tr("MSC mode 2 (AT+KARABAS=msc2): additional SD card for extra files."));
    m_mscModeGroup = new QButtonGroup(this);
    m_mscModeGroup->setExclusive(true);
    m_mscModeGroup->addButton(m_mscMode1Btn, 1);
    m_mscModeGroup->addButton(m_mscMode2Btn, 2);
    m_mscMode1Btn->setChecked(true);

    // Значок «?»: подсказка при наведении (и по щелчку - для тачпадов и macOS)
    m_mscHelp = new QToolButton;
    m_mscHelp->setText(QStringLiteral("?"));
    m_mscHelp->setFixedSize(24, 24);
    m_mscHelp->setCursor(Qt::WhatsThisCursor);
    m_mscHelp->setFocusPolicy(Qt::NoFocus);
    m_mscHelp->setStyleSheet(QStringLiteral(
        "QToolButton { border: 1px solid palette(mid); border-radius: 12px; font-weight: bold; }"
        "QToolButton:hover { background: palette(midlight); }"));
    m_mscHelp->setToolTip(QStringLiteral(
        "<p>This button switches the Karabas into USB card reader mode and lets you access the SD card "
        "through your system's standard file manager. You can work with the files directly. "
        "To leave this mode, you need to safely eject the memory card from your system.</p>"
        "<p><b>Card 1</b> (MSC mode 1) is the main SD card where the FPGA cores are stored. "
        "<b>Card 2</b> (MSC mode 2) is an additional card for extra files.</p>"
        "<p>The 1/2 switch only affects which card is opened by this button. "
        "Core updates always use card 1.</p>"));
    connect(m_mscHelp, &QToolButton::clicked, this, [this]() {
        QToolTip::showText(QCursor::pos(), m_mscHelp->toolTip(), m_mscHelp);
    });

    row4->addWidget(m_flashBtn, 2);
    row4->addWidget(m_mscBtn, 1);
    row4->addWidget(m_mscMode1Btn, 0);
    row4->addWidget(m_mscMode2Btn, 0);
    row4->addWidget(m_mscHelp, 0, Qt::AlignVCenter);
    root->addLayout(row4);

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(5000);          // старые строки отбрасываются автоматически
    root->addWidget(m_log, 1);

    // Правая половина главного окна: панель ядер. По умолчанию свернута;
    // «>>» разворачивает её, «<<» сворачивает.
    m_corePanel = new CoreVersionsPanel(this);
    m_corePanel->hide();
    m_panelToggle = new QToolButton(this);
    m_panelToggle->setText(QStringLiteral(">>"));
    m_panelToggle->setAutoRaise(true);
    m_panelToggle->setFixedWidth(18);
    m_panelToggle->setToolTip(tr("Show core versions"));
    m_panelToggle->setCursor(Qt::PointingHandCursor);
    m_panelToggle->setFocusPolicy(Qt::NoFocus);
    connect(m_panelToggle, &QToolButton::clicked, this, [this]() {
        if (m_corePanel->isVisible()) hideCorePanel();
        else showCorePanel();
    });

    outer->addWidget(left, 1);
    outer->addWidget(m_panelToggle, 0, Qt::AlignVCenter);
    outer->addWidget(m_corePanel, 1);

    resize(538, 600);       // 520 px контента + 18 px на кнопку панели

    // ---- Рабочий поток ----
    m_thread = new QThread(this);
    m_worker = new Worker;
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &Worker::log, this, &MainWindow::appendLog);
    connect(m_worker, &Worker::logVerbose, this, &MainWindow::appendLogVerbose);
    connect(m_worker, &Worker::firmwareReady, this, &MainWindow::onFirmwareReady);
    connect(m_worker, &Worker::coreVersionsReady, this, &MainWindow::onCoreVersionsReady);
    connect(m_worker, &Worker::firmwareStatusReady, this, &MainWindow::onFirmwareStatusReady);
    connect(m_worker, &Worker::appUpdateAvailable, this, &MainWindow::onAppUpdateAvailable);
    connect(m_worker, &Worker::startupFirmwareCheckFinished, this,
            &MainWindow::onStartupFirmwareCheckFinished);
    connect(m_worker, &Worker::finished, this, &MainWindow::onWorkerFinished);
    connect(m_worker, &Worker::portAccessDenied, this, &MainWindow::onPortAccessDenied);
    connect(m_worker, &Worker::openFolder, this, &MainWindow::onOpenFolder);
    // BlockingQueuedConnection: рабочий поток ждёт ответа пользователя, GUI показывает диалог.
    connect(m_worker, &Worker::confirmUnrecognizedDrive, this, &MainWindow::onConfirmUnrecognizedDrive,
            Qt::BlockingQueuedConnection);
    m_thread->start();

    qRegisterMetaType<RollbackInfo>("RollbackInfo");
    connect(m_worker, &Worker::rollbackPrepared, this, &MainWindow::onRollbackPrepared);
    connect(m_downloadBtn, &QPushButton::clicked, this, &MainWindow::onDownload);
    connect(m_fwChk, &QCheckBox::toggled, this, &MainWindow::applyOptionState);
    connect(m_browseBtn, &QPushButton::clicked, this, &MainWindow::onBrowse);
    connect(m_refreshBtn, &QPushButton::clicked, this, &MainWindow::onRefreshPorts);
    connect(m_flashBtn, &QPushButton::clicked, this, &MainWindow::onFlash);
    connect(m_mscBtn, &QPushButton::clicked, this, &MainWindow::onMsc);
    connect(m_detectBtn, &QPushButton::clicked, this, &MainWindow::onDetectModel);
    connect(m_worker, &Worker::modelDetected, this, &MainWindow::onModelDetected);
    connect(m_worker, &Worker::portsChanged, this, &MainWindow::onRefreshPorts);
    connect(m_corePanel, &CoreVersionsPanel::selectionChanged,
            this, &MainWindow::updateActionState);
    connect(m_comCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        updateActionState();
        updatePortPolling();
    });
    connect(m_modelCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        // Прошивка привязана к модели: при смене модели сбрасываем путь, чтобы
        // случайно не прошить плату прошивкой другой модели.
        if (m_firmwareModelIndex >= 0 && m_firmwareModelIndex != idx && !m_firmwarePath.isEmpty()) {
            appendLog(QStringLiteral("Warning: Device model changed; the previously selected firmware was "
                                     "discarded. Download or select the firmware for the new model."));
            m_firmwarePath.clear();
            m_pathEdit->clear();
            m_firmwareModelIndex = -1;
        }
        // Выбор ядер зависит от модели: для другой платы часть ядер может быть
        // недоступна, поэтому старый выбор сбрасываем.
        if (m_corePanel && m_corePanel->hasRows()) {
            m_corePanel->clear();
            m_coreListPrepared = false;
            // Старый набор ядер относится к прежней модели: без нового
            // скачивания FLASH с обновлением ядер запускать нельзя.
            QFile::remove(workDir() + QStringLiteral("/cores/download_complete.txt"));
            hideCorePanel();
            appendLog(QStringLiteral("Device model changed; the core selection was reset. Download the cores "
                                     "again to choose what to update."));
        }
        updateActionState();
    });

    applyOptionState();
    if (QCoreApplication::instance()) QCoreApplication::instance()->installEventFilter(this);
    if (m_fileLogging) openLogFile();
    appendLog(QStringLiteral("Karabas Flasher v") + QString::fromLatin1(APP_VERSION) + QStringLiteral(" (") +
              QString::fromLatin1(APP_GIT_DESCRIBE) + QLatin1Char(')'));
    // Опрос нужен только пока порт не выбран: плату можно подключить после
    // запуска, а уже выбранный порт не сканируем каждую секунду.
    m_portPollTimer = new QTimer(this);
    m_portPollTimer->setInterval(1000);
    connect(m_portPollTimer, &QTimer::timeout, this, &MainWindow::pollSerialPorts);
    onRefreshPorts();
    appendLog(QStringLiteral("Application is ready."));

    // Порядок запуска: сначала порт и AT+KARABAS=version, затем проверка
    // актуальных версий прошивки на GitHub. Если порта нет — проверку
    // запускаем сразу, а автоопределение сработает при подключении платы.
    QMetaObject::invokeMethod(this, &MainWindow::startStartupFlow, Qt::QueuedConnection);
}

MainWindow::~MainWindow() {
    stopThread();
}

void MainWindow::stopThread() {
    if (m_thread && m_thread->isRunning()) {
        m_worker->requestCancel();
        m_thread->quit();
        m_thread->wait();
    }
}

void MainWindow::openLogFile() {
    // Основной путь - рядом с исполняемым файлом (как просили пользователи Windows:
    // лог должен лежать в каталоге с программой, а не в AppData).
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString primary = appDir + QStringLiteral("/flasher_log.txt");

    m_logFile.setFileName(primary);
    if (m_logFile.open(QIODevice::Append)) {
        m_logFile.write("--- Application started with -log switch ---\n");
        m_logFile.flush();
        appendLog("Logging to file: " + m_logFile.fileName());
        return;
    }

    // Каталог с программой недоступен для записи (Program Files, /usr/bin, .app в /Applications).
    // Откатываемся на пользовательский каталог данных.
    const QString fallbackDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(fallbackDir);
    m_logFile.setFileName(fallbackDir + QStringLiteral("/flasher_log.txt"));
    if (m_logFile.open(QIODevice::Append)) {
        m_logFile.write("--- Application started with -log switch ---\n");
        m_logFile.flush();
        appendLog("Warning: Cannot write the log next to the program (" + primary +
                  "); logging to " + m_logFile.fileName() + " instead.");
    } else {
        appendLog("Warning: Could not open the log file " + m_logFile.fileName() + "; file logging is disabled.");
    }
}

void MainWindow::writeLogLine(const QString& line) {
    if (m_logFile.isOpen()) {
        m_logFile.write((line + QLatin1Char('\n')).toUtf8());
        m_logFile.flush();
    }
}

void MainWindow::appendLog(const QString& message) {
    const QString line = "[" + QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") + "] " + message;

    QString color;
    if (message.contains("Error", Qt::CaseInsensitive) || message.contains("Failed", Qt::CaseInsensitive) ||
        message.contains("Warning", Qt::CaseInsensitive)) {
        color = QStringLiteral("#e04040");
    } else if (message.contains("Success", Qt::CaseInsensitive) || message.contains("completed", Qt::CaseInsensitive) ||
               message.contains("Found", Qt::CaseInsensitive) || message.contains("Ready", Qt::CaseInsensitive) ||
               message.contains("Downloaded", Qt::CaseInsensitive)) {
        color = QStringLiteral("#2ea043");
    }
    const QString style = color.isEmpty() ? QStringLiteral("white-space:pre-wrap;")
                                          : "white-space:pre-wrap;color:" + color + ";";
    m_log->appendHtml("<span style=\"" + style + "\">" + line.toHtmlEscaped() + "</span>");
    writeLogLine(line);
}

// Подробные сообщения идут только в файл журнала: в окне они не нужны и мешают
// читать ход операции. Если файл не открыт (запуск без --log), они теряются.
void MainWindow::appendLogVerbose(const QString& message) {
    const QString line = "[" + QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") + "] " + message;
    writeLogLine(line);
}

Params MainWindow::collectParams() const {
    Params p;
    p.modelIndex = m_modelCombo->currentIndex();
    p.updateFirmware = m_fwChk->isChecked();
    p.updateCores = m_coresChk->isChecked();
    p.updateExtra = m_extraChk->isChecked();
    p.portLocation = m_comCombo->currentData().toString();
    p.firmwarePath = m_firmwarePath;
    p.mscMode = (m_mscModeGroup->checkedId() == 2) ? 2 : 1;
    const bool hasCoreRows = m_corePanel && m_corePanel->hasRows();
    p.coreSelectionSet = hasCoreRows;
    p.selectedCoreFiles = hasCoreRows ? m_corePanel->selectedFiles() : QStringList();
    p.ignoredCoreFiles = m_corePanel ? m_corePanel->ignoredFiles() : QStringList();
    p.knownCoreFiles = m_corePanel ? m_corePanel->allFiles() : QStringList();
    return p;
}

void MainWindow::setBusy(bool busy) {
    m_busy = busy;
    const bool en = !busy;
    for (QWidget* w : {static_cast<QWidget*>(m_modelCombo), static_cast<QWidget*>(m_downloadBtn),
                       static_cast<QWidget*>(m_fwChk),
                       static_cast<QWidget*>(m_coresChk), static_cast<QWidget*>(m_delTempChk),
                       static_cast<QWidget*>(m_browseBtn), static_cast<QWidget*>(m_comCombo),
                       static_cast<QWidget*>(m_refreshBtn), static_cast<QWidget*>(m_flashBtn),
                       static_cast<QWidget*>(m_mscBtn), static_cast<QWidget*>(m_mscMode1Btn),
                       static_cast<QWidget*>(m_mscMode2Btn), static_cast<QWidget*>(m_mscHelp),
                       static_cast<QWidget*>(m_detectBtn), static_cast<QWidget*>(m_panelToggle),
                       static_cast<QWidget*>(m_corePanel)}) {
        w->setEnabled(en);
    }
    if (en) m_downloadBtn->setText(tr("Download via GitHub"));
    updateFlashButtonText();
    applyOptionState();
    if (en) updateActionState();
}

void MainWindow::applyOptionState() {
    const bool fw = m_fwChk->isChecked();
    m_pathEdit->setEnabled(fw);
    m_browseBtn->setEnabled(!m_busy && fw);
    updateActionState();
}

void MainWindow::updateActionState() {
    if (m_busy) return;                 // setBusy сам управляет доступностью

    // FLASH!: нужна прошивка (если включено обновление прошивки) и/или готовый набор ядер
    bool canFlash = false;
    if (m_fwChk->isChecked() && !m_firmwarePath.isEmpty()) canFlash = true;
    if (m_coresChk->isChecked()) {
        const QString marker = workDir() + QStringLiteral("/cores/download_complete.txt");
        if (QFile::exists(marker)) canFlash = true;
        // Список с GitHub подготовлен, ядра отмечены: FLASH сам скачает
        // недостающие выбранные файлы перед записью на карту.
        if (m_coreListPrepared && m_corePanel && !m_corePanel->selectedFiles().isEmpty()) {
            canFlash = true;
        }
    }
    if (m_extraChk->isChecked()) {
        const QString marker = workDir() + QStringLiteral("/extra/extra_complete.txt");
        if (QFile::exists(marker)) canFlash = true;
    }
    // BACKUP доступен по Shift+клику, даже если ни одна операция не отмечена:
    // для него нужен только выбранный порт.
    const bool hasPort = !m_comCombo->currentData().toString().isEmpty();
    m_flashBtn->setEnabled(canFlash || hasPort);

    // MSC: кнопка активна всегда. Если порт не выбран/отсутствует, при нажатии
    // покажем подсказку про безопасное извлечение (устройство может быть уже в MSC).
    m_mscBtn->setEnabled(true);
}

// ----------------------------------------------------------------------------
// Действия
// ----------------------------------------------------------------------------
void MainWindow::onDownload() {
    if (m_busy) return;
    setBusy(true);
    m_downloadBtn->setText(tr("Downloading..."));
    Params p = collectParams();
    // Ядра качаются в два шага: сначала с GitHub читаются только заголовки
    // для списка версий, затем FLASH докачивает полные файлы выбранных ядер.
    if (p.updateCores) p.prepareCores = true;
    m_worker->clearCancel();        // сброс в GUI-потоке до постановки задачи в очередь
    QMetaObject::invokeMethod(m_worker, [w = m_worker, p]() { w->runDownload(p); }, Qt::QueuedConnection);
}

void MainWindow::onFlash() {
    if (m_busy) return;

    const bool backup = QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
    if (backup) {
        if (m_comCombo->currentData().toString().isEmpty()) {
            appendLog(QStringLiteral("Error: Select the Karabas control port before BACKUP."));
            return;
        }
        m_rollbackPreparing = true;
        m_rollbackReady = false;
        hideCorePanel();
        setBusy(true);
        m_flashBtn->setText(tr("Preparing..."));
        Params p = collectParams();
        m_worker->clearCancel();
        QMetaObject::invokeMethod(m_worker, [w = m_worker, p]() { w->runPrepareRollback(p); },
                                  Qt::QueuedConnection);
        return;
    }

    Params p = collectParams();
    if (!p.updateFirmware && !p.updateCores && !p.updateExtra) {
        appendLog(QStringLiteral("Error: Nothing to do. Check 'Update firmware', 'Update cores' "
                                 "or 'Download extra content'."));
        return;
    }

    m_flashStarted = true;
    m_flashCores = m_coresChk->isChecked();
    // Правая панель во время FLASH не нужна; после обновления ядер её
    // содержимое будет сброшено, чтобы не показывать старые Installed.
    hideCorePanel();
    setBusy(true);
    m_worker->clearCancel();        // сброс в GUI-потоке до постановки задачи в очередь
    QMetaObject::invokeMethod(m_worker, [w = m_worker, p]() { w->runFlash(p); }, Qt::QueuedConnection);
}

void MainWindow::onMsc() {
    if (m_busy) return;
    const Params p = collectParams();

    // Проверка "уже в MSC" и подсказка про пустой порт живут в Worker::doOpenSd:
    // там они выполняются в рабочем потоке и не блокируют GUI (на macOS Platform::newDisks
    // вызывает diskutil на каждый том, до 10 с на том).
    setBusy(true);
    m_worker->clearCancel();        // сброс в GUI-потоке до постановки задачи в очередь
    QMetaObject::invokeMethod(m_worker, [w = m_worker, p]() { w->runMsc(p); }, Qt::QueuedConnection);
}

void MainWindow::onDetectModel() {
    if (m_busy) return;
    m_lastAutoDetectedPort = m_comCombo->currentData().toString();
    setBusy(true);
    const Params p = collectParams();
    m_worker->clearCancel();
    QMetaObject::invokeMethod(m_worker, [w = m_worker, p]() { w->runDetectModel(p); }, Qt::QueuedConnection);
}

// Автодетект модели при старте: не блокирует GUI (без setBusy), не мешает
// пользователю. Если ответа нет - просто пишем в лог, что модель не определена.
void MainWindow::onAutoDetectModel() {
    if (m_busy) return;
    m_lastAutoDetectedPort = m_comCombo->currentData().toString();
    Params p = collectParams();
    p.autoDetect = true;
    m_worker->clearCancel();
    QMetaObject::invokeMethod(m_worker, [w = m_worker, p]() { w->runDetectModel(p); }, Qt::QueuedConnection);
}

void MainWindow::startStartupFlow() {
    if (m_closeRequested) return;
    if (m_busy) {
        // Пользователь успел начать операцию — не влезаем, попробуем позже.
        QTimer::singleShot(200, this, &MainWindow::startStartupFlow);
        return;
    }
    if (!m_comCombo->currentData().toString().isEmpty()) {
        m_startupDetectionPending = true;
        onAutoDetectModel();
        return;
    }
    // Порта ещё нет: узнать версию платы сейчас нельзя. Проверяем прошивки,
    // а модель определится при появлении control-порта.
    onStartupFirmwareCheck();
}

void MainWindow::onStartupFirmwareCheck() {
    if (m_startupFirmwareCheckStarted) return;
    m_startupFirmwareCheckStarted = true;
    m_worker->clearCancel();
    QMetaObject::invokeMethod(m_worker, [w = m_worker]() { w->runStartupFirmwareCheck(); },
                              Qt::QueuedConnection);
}

void MainWindow::onStartupFirmwareCheckFinished() {
    m_startupCheckDone = true;
    // Если плату подключили позже и порт появился во время проверки,
    // автоопределение запустится только сейчас.
    if (!m_busy && !m_closeRequested) maybeAutoDetectPort();
}

void MainWindow::onRollbackPrepared(const RollbackInfo& info) {
    m_rollbackInfo = info;
    m_rollbackReady = true;
}

bool MainWindow::showRollbackDialog() {
    const RollbackInfo info = m_rollbackInfo;
    if (!info.firmwareAvailable && !info.coresAvailable) {
        QStringList reasons;
        if (!info.firmwareError.isEmpty()) reasons << info.firmwareError;
        if (!info.coresError.isEmpty()) reasons << info.coresError;
        QMessageBox::warning(this, tr("Backup"),
                             tr("Nothing can be restored:\n%1").arg(reasons.join(QLatin1Char('\n'))));
        return false;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Backup"));
    auto* layout = new QVBoxLayout(&dialog);

    auto* intro = new QLabel(tr("Restore the previous state?"), &dialog);
    QFont introFont = intro->font();
    introFont.setBold(true);
    intro->setFont(introFont);
    layout->addWidget(intro);

    auto* fw = new QCheckBox(
        info.firmwareAvailable
            ? tr("Firmware: %1 → %2").arg(info.installedBuild, info.targetBuild)
            : tr("Firmware: not available"),
        &dialog);
    fw->setChecked(info.firmwareAvailable);
    fw->setEnabled(info.firmwareAvailable);
    if (!info.firmwareAvailable)
        fw->setToolTip(info.firmwareError);
    layout->addWidget(fw);
    if (!info.firmwareAvailable && !info.firmwareError.isEmpty()) {
        auto* fwError = new QLabel(info.firmwareError, &dialog);
        fwError->setWordWrap(true);
        fwError->setStyleSheet(QStringLiteral("color:#e04040;"));
        layout->addWidget(fwError);
    }

    auto* cores = new QCheckBox(
        info.coresAvailable
            ? tr("Cores: %1 (%2 .kg files)").arg(info.backupName).arg(info.coreCount)
            : tr("Cores: not available"),
        &dialog);
    cores->setChecked(info.coresAvailable);
    cores->setEnabled(info.coresAvailable);
    if (!info.coresAvailable)
        cores->setToolTip(info.coresError);
    layout->addWidget(cores);
    if (!info.coresAvailable && !info.coresError.isEmpty()) {
        auto* coresError = new QLabel(info.coresError, &dialog);
        coresError->setWordWrap(true);
        coresError->setStyleSheet(QStringLiteral("color:#e04040;"));
        layout->addWidget(coresError);
    }

    auto* warning = new QLabel(
        tr("Current .kg files will be overwritten without an additional safety backup."), &dialog);
    warning->setWordWrap(true);
    warning->setStyleSheet(QStringLiteral("color:#b58900;"));
    warning->setVisible(info.coresAvailable);
    layout->addWidget(warning);
    connect(cores, &QCheckBox::toggled, warning, &QWidget::setVisible);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    QPushButton* restore = buttons->addButton(tr("Restore"), QDialogButtonBox::AcceptRole);
    restore->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) return false;

    const bool doFirmware = fw->isChecked();
    const bool doCores = cores->isChecked();
    if (!doFirmware && !doCores) {
        QMessageBox::warning(this, tr("Backup"), tr("Select firmware, cores, or both."));
        return false;
    }

    Params p = collectParams();
    if (info.modelIndex >= 0 && info.modelIndex < m_modelCombo->count()) {
        p.modelIndex = info.modelIndex;
    }
    p.updateFirmware = false;
    p.updateCores = false;
    p.updateExtra = false;
    p.rollbackFirmware = doFirmware;
    p.rollbackCores = doCores;
    p.rollbackFirmwareUrl = info.targetUrl;
    p.rollbackTargetBuild = info.targetBuild;
    p.rollbackBackupName = info.backupName;
    p.coreSelectionSet = false;
    p.selectedCoreFiles.clear();
    p.ignoredCoreFiles.clear();
    p.knownCoreFiles.clear();

    m_flashStarted = true;
    m_flashCores = doCores;
    hideCorePanel();
    setBusy(true);
    m_worker->clearCancel();
    QMetaObject::invokeMethod(m_worker, [w = m_worker, p]() { w->runRollback(p); },
                              Qt::QueuedConnection);
    return true;
}

void MainWindow::onModelDetected(int modelIndex, const QString& raw) {
    Q_UNUSED(raw);      // ответ уже выведен в журнал рабочим потоком
    if (modelIndex >= 0 && modelIndex < m_modelCombo->count()) {
        m_modelCombo->setCurrentIndex(modelIndex);
    }
}

void MainWindow::onBrowse() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Select firmware"), QString(),
                                                      tr("UF2 Files (*.uf2);;All Files (*)"));
    if (path.isEmpty()) return;
    QString why;
    if (!isValidUf2(path, &why)) {
        appendLog("Error: Selected file is not a valid UF2 image (" + why + "): " + path);
        return;
    }
    m_firmwarePath = path;
    m_firmwareModelIndex = m_modelCombo->currentIndex();
    m_pathEdit->setText(path);
    appendLog("User manually selected file: " + path);
    updateActionState();
}

void MainWindow::onRefreshPorts() {
    updatePortList(true);
    maybeAutoDetectPort();
}

void MainWindow::pollSerialPorts() {
    if (m_busy) return;
    // Если порт уже выбран (автоматически или вручную), опрос не нужен.
    if (m_comCombo && !m_comCombo->currentData().toString().isEmpty()) {
        updatePortPolling();
        return;
    }
    updatePortList(false);
    maybeAutoDetectPort();
    updatePortPolling();
}

void MainWindow::updatePortList(bool userRequested) {
    struct PortEntry {
        QString text;
        QString location;
        KarabasPortRole role = KarabasPortRole::Unknown;
    };

    if (userRequested) appendLog(QStringLiteral("Scanning available serial ports..."));

    QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    std::sort(ports.begin(), ports.end(), [](const QSerialPortInfo& a, const QSerialPortInfo& b) {
        return a.systemLocation() < b.systemLocation();
    });

    QList<PortEntry> entries;
    int controlCount = 0;
    int coreSerialCount = 0;
    QString oldControl = m_controlPortLocation;
    QString newControl;
    QString newControlText;

    for (const QSerialPortInfo& info : ports) {
#if defined(Q_OS_MACOS)
        if (info.systemLocation().startsWith(QStringLiteral("/dev/tty."))) continue;   // оставляем /dev/cu.*
#endif
        // Только USB-устройства: у встроенных COM-портов, Bluetooth-портов и консоли отладки
        // нет USB-идентификатора, а Карабас всегда его имеет.
        if (!info.hasVendorIdentifier()) continue;
#if defined(Q_OS_WIN)
        QString text = info.portName();                  // COM3
        // Qt6 на Windows ожидает "COM3", а не "\\.\COM3": с префиксом порт
        // открывается, но waitForReadyRead() не видит входящих данных.
        const QString location = info.portName();
#else
        QString text = info.systemLocation();
        const QString location = info.systemLocation();
#endif
        const KarabasPortRole role = karabasPortRole(info);
        if (!info.description().isEmpty()) text += " (" + info.description() + ")";
        if (role == KarabasPortRole::Control) text += QStringLiteral(" [control]");
        else if (role == KarabasPortRole::CoreSerial) text += QStringLiteral(" [core serial]");
        if (info.hasVendorIdentifier() && info.hasProductIdentifier()) {
            text += QStringLiteral(" [%1:%2]")
                        .arg(info.vendorIdentifier(), 4, 16, QLatin1Char('0'))
                        .arg(info.productIdentifier(), 4, 16, QLatin1Char('0'));
        }
        entries.append(PortEntry{text, location, role});
        if (role == KarabasPortRole::Control) {
            ++controlCount;
            newControl = location;
            newControlText = text;
        } else if (role == KarabasPortRole::CoreSerial) {
            ++coreSerialCount;
        }
    }

    // Уникальный control — основной кандидат. Если порт один и это не core
    // serial (старая прошивка или роль не определилась), тоже считаем его
    // control: именно так работали версии до появления второго порта.
    if (controlCount != 1) {
        newControl.clear();
        newControlText.clear();
        if (controlCount == 0 && entries.size() == 1 && coreSerialCount == 0) {
            newControl = entries.first().location;
            newControlText = entries.first().text;
        }
    }
    m_controlPortLocation = newControl;
    if (oldControl != newControl) {
        if (!newControl.isEmpty()) {
            if (!userRequested && controlCount == 1) {
                appendLog("Karabas control port detected: " + newControlText);
            }
        } else if (!oldControl.isEmpty()) {
            appendLog(QStringLiteral("Karabas control port disconnected."));
            m_lastAutoDetectedPort.clear();
        }
    }

    QStringList signatureParts;
    for (const PortEntry& entry : entries) {
        signatureParts << entry.location + QLatin1Char('|') +
                              QString::number(static_cast<int>(entry.role)) + QLatin1Char('|') +
                              entry.text;
    }
    const QString signature = signatureParts.join(QLatin1Char('\n'));
    if (!userRequested && signature == m_portSignature) {
        updatePortPolling();
        return;
    }
    m_portSignature = signature;

    const int added = entries.size();
    if (userRequested) {
        for (const PortEntry& entry : entries) appendLog("Found port: " + entry.text);
        if (added == 0) {
            appendLog(QStringLiteral("Error: Active serial ports not found. Connect the Karabas by USB "
                                     "and press 'Refresh ports'."));
        }
    }

    const QString previous = m_comCombo->currentData().toString();
    m_comCombo->clear();
    m_comCombo->addItem(tr("(not selected - flash without reset)"), QString());

    int controlIndex = -1;
    int previousIndex = -1;
    for (const PortEntry& entry : entries) {
        const int itemIndex = m_comCombo->count();
        m_comCombo->addItem(entry.text, entry.location);
        if (entry.role == KarabasPortRole::Control) controlIndex = itemIndex;
        if (!previous.isEmpty() && entry.location == previous) previousIndex = itemIndex;
    }

    // При изменении набора портов уникальный control важнее прежнего выбора:
    // именно к нему нужно отправлять AT-команды. Пока набор не меняется,
    // ручной выбор пользователя (например, core serial) не трогаем.
    int selectedIndex = controlCount == 1 ? controlIndex : previousIndex;
    if (selectedIndex < 0) {
        if (added == 1 && coreSerialCount == 0) {
            selectedIndex = 1;      // единственный USB-порт (старая прошивка или другое устройство)
        } else {
            selectedIndex = 0;
        }
    }

    if (controlCount > 1) {
        appendLog(QStringLiteral("Warning: More than one Karabas control port found. Select the required "
                                 "board manually."));
    } else if (controlCount == 0 && coreSerialCount > 0) {
        appendLog(QStringLiteral("Warning: Could not identify the Karabas control port automatically. "
                                 "Select the [control] port manually; [core serial] is not used for flashing."));
    } else if (controlCount == 0 && added > 1) {
        appendLog(QStringLiteral("Warning: More than one USB serial port found. Select the Karabas control "
                                 "port manually."));
    }
    if (coreSerialCount > 0 && controlCount > 0) {
        appendLog(QStringLiteral("Karabas core serial port is for the core UART only and is not used for "
                                 "flashing."));
    }

    m_comCombo->setCurrentIndex(selectedIndex);
    if (userRequested && controlCount == 1 && selectedIndex == controlIndex) {
        appendLog("Selected Karabas control port: " + m_comCombo->itemText(controlIndex));
    }
    updateActionState();
    updatePortPolling();
}

void MainWindow::updatePortPolling() {
    if (!m_portPollTimer) return;
    // Опрашиваем только пока control-порт не найден и порт не выбран.
    // Если пользователь выбрал конкретный порт (или «not selected» при
    // найденном control), не трогаем систему каждую секунду.
    const bool needPolling = m_comCombo && m_comCombo->currentData().toString().isEmpty() &&
                             m_controlPortLocation.isEmpty();
    if (needPolling && !m_portPollTimer->isActive()) {
        m_portPollTimer->start();
    } else if (!needPolling && m_portPollTimer->isActive()) {
        m_portPollTimer->stop();
    }
}

void MainWindow::maybeAutoDetectPort() {
    if (!m_startupCheckDone || m_busy || !m_comCombo) return;
    const QString selected = m_comCombo->currentData().toString();
    if (selected.isEmpty() || selected != m_controlPortLocation) return;
    if (selected == m_lastAutoDetectedPort) return;
    m_lastAutoDetectedPort = selected;
    onAutoDetectModel();
}

void MainWindow::updateFlashButtonText() {
    if (!m_flashBtn || m_busy) return;
    const bool backup = QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
    m_flashBtn->setText(backup ? tr("BACKUP") : tr("FLASH!"));
    m_flashBtn->setToolTip(backup
        ? tr("Restore the previous firmware and/or the latest core backup.")
        : tr("Download missing selected cores and flash the board."));
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Shift && !key->isAutoRepeat() && !m_busy) {
            m_flashBtn->setText(event->type() == QEvent::KeyPress ? tr("BACKUP") : tr("FLASH!"));
        }
    } else if (event->type() == QEvent::ApplicationDeactivate && !m_busy) {
        m_flashBtn->setText(tr("FLASH!"));
    }
    return QWidget::eventFilter(watched, event);
}

void MainWindow::onOpenFolder(const QString& path) {
    if (!openInFileManager(path)) {
        appendLog("Warning: Could not open the file manager automatically. Open the folder manually: " + path);
    }
}

void MainWindow::onConfirmUnrecognizedDrive(const QString& node, const QString& label, qint64 bytesTotal,
                                            const QStringList& sampleFiles, bool* accepted) {
    if (accepted) *accepted = false;        // безопасное поведение по умолчанию

    QStringList files = sampleFiles;
    if (files.size() > 10) files = files.mid(0, 10);
    const QString filesText = files.isEmpty() ? tr("(no files)") : files.join(QStringLiteral(", "));
    const QString sizeText = QString::number(bytesTotal / (1024 * 1024)) + QStringLiteral(" MB");

    const auto r = QMessageBox::warning(
        this, QStringLiteral("Karabas Flasher"),
        tr("The drive %1 [%2] (%3) does not look like the Karabas SD card:\n"
           "no .kg cores and no backup_* folder were found.\n\n"
           "Files in the root: %4\n\n"
           "Writing to this drive will replace files on it. Continue?")
            .arg(node, label, sizeText, filesText),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (accepted) *accepted = (r == QMessageBox::Yes);
    appendLog(QStringLiteral("User %1 writing to unrecognized drive %2.")
                  .arg(r == QMessageBox::Yes ? QStringLiteral("confirmed") : QStringLiteral("declined"), node));
}

void MainWindow::onCoreVersionsReady(const QVariantList& rows, const QString& boardName,
                                     bool installedKnown, bool availableList) {
    m_coreListPrepared = availableList;
    m_corePanel->setRows(rows, boardName, installedKnown, availableList);
    showCorePanel();        // после подготовки/парсинга панель раскрывается сама
}

void MainWindow::onFirmwareStatusReady(bool known, bool needsUpdate,
                                       const QString& installed, const QString& latest) {
    m_corePanel->setFirmwareStatus(known, needsUpdate, installed, latest);
    if (known) {
        appendLogVerbose(QStringLiteral("Firmware status: installed %1, latest %2, %3.")
                             .arg(installed, latest,
                                  needsUpdate ? QStringLiteral("update needed")
                                              : QStringLiteral("up to date")));
    }
}

void MainWindow::onAppUpdateAvailable(const QString& version, const QString& url) {
    if (version.isEmpty() || url.isEmpty()) {
        m_appUpdateLabel->hide();
        return;
    }
    const QString text = tr("New flasher version available");
    m_appUpdateLabel->setText(
        QStringLiteral("<a href=\"%1\" style=\"color:#e04040; font-weight:bold;\">%2</a>")
            .arg(url.toHtmlEscaped(), text.toHtmlEscaped()));
    m_appUpdateLabel->setToolTip(tr("Latest version: %1").arg(version));
    m_appUpdateLabel->show();
    appendLogVerbose(QStringLiteral("New Karabas Flasher version available: %1 (%2)").arg(version, url));
}

void MainWindow::showCorePanel() {
    if (!m_corePanel || m_corePanel->isVisible()) return;

    // Запоминаем внешнюю ширину свёрнутого окна, а не клиентскую.
    m_collapsedWidth = frameGeometry().width();
    m_corePanel->show();
    m_panelToggle->setText(QStringLiteral("<<"));
    m_panelToggle->setToolTip(tr("Hide core versions"));

    // Вторая половина: окно расширяется, но не до краёв экрана. На Windows
    // с масштабированием прежнее «удвоение» упиралось в available.width() и
    // выглядело как разворот на весь экран.
    QScreen* screen = QGuiApplication::screenAt(frameGeometry().center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect();
    int target = qMax(m_collapsedWidth * 2, m_collapsedWidth + 360);
    if (!available.isEmpty()) {
        const int maxWidth = qMax(m_collapsedWidth, static_cast<int>(available.width() * 0.85));
        target = qMin(target, maxWidth);
    }
    centerWindowForWidth(target);
}

void MainWindow::hideCorePanel() {
    if (!m_corePanel || !m_corePanel->isVisible()) return;

    m_corePanel->hide();
    m_panelToggle->setText(QStringLiteral(">>"));
    m_panelToggle->setToolTip(tr("Show core versions"));
    if (m_collapsedWidth > 0) centerWindowForWidth(m_collapsedWidth);
}

void MainWindow::centerWindowForWidth(int outerWidth) {
    QScreen* screen = QGuiApplication::screenAt(frameGeometry().center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect();
    if (available.isEmpty()) {
        resize(qMax(1, outerWidth), height());
        return;
    }

    // frame* — толщина рамки оконного менеджера; geometry() задаёт клиентскую
    // область, поэтому центрируем внешний прямоугольник и переводим его обратно.
    const QRect frame = frameGeometry();
    const int frameWidth = frame.width() - width();
    const int frameHeight = frame.height() - height();
    const int frameLeft = frame.left() - x();
    const int frameTop = frame.top() - y();

    // Не прижимаем окно к краям: на Windows ширина, равная available.width(),
    // визуально воспринимается как разворот на весь экран.
    const int margin = 32;
    const int maxOuterWidth = qMax(1, available.width() - margin * 2);
    const int targetOuterWidth = qMin(outerWidth, maxOuterWidth);
    const int targetOuterHeight = qMin(frame.height(), available.height());
    const int clientWidth = qMax(1, targetOuterWidth - frameWidth);
    const int clientHeight = qMax(1, targetOuterHeight - frameHeight);
    const int clientX = available.left() + (available.width() - targetOuterWidth) / 2 + frameLeft;
    const int clientY = available.top() + (available.height() - targetOuterHeight) / 2 + frameTop;
    setGeometry(clientX, clientY, clientWidth, clientHeight);
}

void MainWindow::onFirmwareReady(const QString& path) {
    m_firmwarePath = path;
    m_firmwareModelIndex = m_modelCombo->currentIndex();
    m_pathEdit->setText(path);
    updateActionState();
}

void MainWindow::onWorkerFinished() {
    const bool startFirmwareCheck = m_startupDetectionPending;
    m_startupDetectionPending = false;
    const bool showRollback = m_rollbackReady;
    const bool rollbackPrepareFailed = m_rollbackPreparing && !showRollback;
    m_rollbackReady = false;
    m_rollbackPreparing = false;
    if (m_flashStarted) {
        if (m_flashCores) {
            if (m_corePanel) m_corePanel->clear();
            m_coreListPrepared = false;
            // После FLASH старый набор/маркер больше не соответствуют
            // состоянию карты: следующий список готовится заново.
            QFile::remove(workDir() + QStringLiteral("/cores/download_complete.txt"));
        }
        m_flashStarted = false;
        m_flashCores = false;
    }
    setBusy(false);
    if (showRollback && !m_closeRequested) {
        showRollbackDialog();
        return;
    }
    if (rollbackPrepareFailed && !m_closeRequested) {
        appendLog(QStringLiteral("Warning: Could not prepare BACKUP."));
    }
    if (startFirmwareCheck && !m_closeRequested) {
        QMetaObject::invokeMethod(this, &MainWindow::onStartupFirmwareCheck, Qt::QueuedConnection);
    }
    if (m_closeRequested) close();
}

// Нет прав на порт (Linux): предлагаем поставить правило udev с TAG+="uaccess".
// Пароль администратора запрашивает polkit (pkexec), один раз.
void MainWindow::onPortAccessDenied(const QString& portLocation) {
#if defined(Q_OS_LINUX)
    if (m_accessProc) return;                       // установка уже идёт
    if (m_accessRuleInstalled) {
        appendLog("Warning: The access rule is already installed, but " + portLocation +
                  " is still not accessible. Replug the device (or log out and in again). "
                  "You can check the permissions with: getfacl " + portLocation);
        return;
    }

    quint16 vid = 0, pid = 0;
    bool found = false;
    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo& info : ports) {
        if (info.systemLocation() == portLocation && info.hasVendorIdentifier() && info.hasProductIdentifier()) {
            vid = info.vendorIdentifier();
            pid = info.productIdentifier();
            found = true;
            break;
        }
    }
    if (!found) {
        appendLog("Warning: Cannot identify the USB device behind " + portLocation +
                  ". Add your user to the 'dialout' group ('uucp' on Arch/Manjaro) and log in again.");
        return;
    }

    const QString vidS = QStringLiteral("%1").arg(vid, 4, 16, QLatin1Char('0'));
    const QString pidS = QStringLiteral("%1").arg(pid, 4, 16, QLatin1Char('0'));

    const auto answer = QMessageBox::question(
        this, QStringLiteral("Karabas Flasher"),
        tr("The program has no permission to open %1.\n\nInstall a udev rule that gives the current user access "
           "to this device (USB %2:%3)?\n\nYou will be asked for the administrator password once.")
            .arg(portLocation, vidS, pidS),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes) {
        appendLog(QStringLiteral("Access rule was not installed. The serial port stays unavailable."));
        return;
    }

    // vid/pid - только шестнадцатеричные цифры, подстановка в команду безопасна
    const QString rule = QStringLiteral("SUBSYSTEM==\"tty\", ATTRS{idVendor}==\"%1\", ATTRS{idProduct}==\"%2\", "
                                        "TAG+=\"uaccess\", ENV{ID_MM_DEVICE_IGNORE}=\"1\"")
                             .arg(vidS, pidS);
    // Номер файла должен быть МЕНЬШЕ 73: тег uaccess обрабатывает 73-seat-late.rules.
    const QString file = QStringLiteral("/etc/udev/rules.d/70-karabas-%1-%2.rules").arg(vidS, pidS);
    const QString oldFile = QStringLiteral("/etc/udev/rules.d/99-karabas-%1-%2.rules").arg(vidS, pidS);
    const QString script =
        QStringLiteral("rm -f %3; printf '%s\\n' '%1' > %2 && udevadm control --reload && "
                       "udevadm trigger --subsystem-match=tty --action=change")
            .arg(rule, file, oldFile);

    m_accessProc = new QProcess(this);
    m_accessProc->setProcessEnvironment(cleanChildEnvironment());
    connect(m_accessProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        if (!m_accessProc) return;                  // уже обработано в finished()
        appendLog(QStringLiteral("Error: 'pkexec' was not found (install 'polkit'). Add your user to the "
                                 "'dialout' group ('uucp' on Arch/Manjaro) and log in again."));
        m_accessProc->deleteLater();
        m_accessProc = nullptr;
    });
    connect(m_accessProc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!m_accessProc) return;                  // уже обработано в errorOccurred()
        const QString err = QString::fromUtf8(m_accessProc->readAllStandardError()).trimmed();
        if (status == QProcess::NormalExit && code == 0) {
            m_accessRuleInstalled = true;
            appendLog(QStringLiteral("Success: Access rule installed. If the port is still not accessible, "
                                     "replug the device. Then repeat the operation."));
        } else if (code == 126 || code == 127) {
            appendLog(QStringLiteral("Warning: Administrator authorization was cancelled or denied. "
                                     "The access rule was not installed."));
        } else {
            appendLog("Error: Failed to install the access rule (code " + QString::number(code) + "): " + err);
        }
        m_accessProc->deleteLater();
        m_accessProc = nullptr;
    });

    appendLog(QStringLiteral("Requesting administrator rights to install the access rule..."));
    m_accessProc->start(QStringLiteral("pkexec"), QStringList() << QStringLiteral("sh") << QStringLiteral("-c") << script);
#else
    Q_UNUSED(portLocation);
#endif
}

void MainWindow::cleanupTemp() {
    if (!m_delTempChk->isChecked()) return;
    const QString wd = workDir();
    // Пустой путь (CacheLocation недоступен) нельзя передавать в QDir: QDir("")
    // указывает на текущую папку процесса, и removeRecursively() снёс бы её.
    if (wd.isEmpty()) return;
    QDir(wd).removeRecursively();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (m_busy) {
        const auto r = QMessageBox::warning(
            this, QStringLiteral("Karabas Flasher"),
            tr("An operation is still in progress.\n\nStop it and exit? The operation will be stopped at the "
               "nearest safe point; steps that modify the SD card or write the firmware are completed first."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        m_closeRequested = true;
        m_worker->requestCancel();
        appendLog(QStringLiteral("Stopping the current operation; the window will close when it is safe..."));
        event->ignore();            // окно закроется из onWorkerFinished()
        return;
    }
    appendLog(QStringLiteral("Application closing."));
    cleanupTemp();
    stopThread();
    event->accept();
}
