#include "MainWindow.h"

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
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QStandardPaths>
#include <QThread>
#include <QToolButton>
#include <QToolTip>
#include <QCursor>
#include <QVBoxLayout>

#include <algorithm>

MainWindow::MainWindow(bool fileLogging, QWidget* parent) : QWidget(parent), m_fileLogging(fileLogging) {
    setWindowTitle(QStringLiteral("Karabas Go Family Firmware Flasher v") + QString::fromLatin1(APP_VERSION));
    const QIcon appIcon(QStringLiteral(":/krb.png"));
    if (appIcon.isNull()) {
        qWarning("Icon resource ':/krb.png' could not be loaded (missing file or PNG plugin).");
    }
    setWindowIcon(appIcon);

    auto* root = new QVBoxLayout(this);

    root->addWidget(new QLabel(tr("Select Device Model:")));
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
    m_fwChk->setChecked(true);
    m_fwChk->setToolTip(tr("Download and flash the RP2040 firmware of the board."));
    m_extraChk = new QCheckBox(tr("Download extra content"));
    m_extraChk->setToolTip(tr("Download the extra content archive and copy its files to the SD card. "
                              "Files that already exist on the card are replaced; other files are left untouched."));
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

    resize(520, 600);

    // ---- Рабочий поток ----
    m_thread = new QThread(this);
    m_worker = new Worker;
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &Worker::log, this, &MainWindow::appendLog);
    connect(m_worker, &Worker::firmwareReady, this, &MainWindow::onFirmwareReady);
    connect(m_worker, &Worker::finished, this, &MainWindow::onWorkerFinished);
    connect(m_worker, &Worker::portAccessDenied, this, &MainWindow::onPortAccessDenied);
    connect(m_worker, &Worker::openFolder, this, &MainWindow::onOpenFolder);
    // BlockingQueuedConnection: рабочий поток ждёт ответа пользователя, GUI показывает диалог.
    connect(m_worker, &Worker::confirmUnrecognizedDrive, this, &MainWindow::onConfirmUnrecognizedDrive,
            Qt::BlockingQueuedConnection);
    m_thread->start();

    connect(m_downloadBtn, &QPushButton::clicked, this, &MainWindow::onDownload);
    connect(m_fwChk, &QCheckBox::toggled, this, &MainWindow::applyOptionState);
    connect(m_browseBtn, &QPushButton::clicked, this, &MainWindow::onBrowse);
    connect(m_refreshBtn, &QPushButton::clicked, this, &MainWindow::onRefreshPorts);
    connect(m_flashBtn, &QPushButton::clicked, this, &MainWindow::onFlash);
    connect(m_mscBtn, &QPushButton::clicked, this, &MainWindow::onMsc);
    connect(m_detectBtn, &QPushButton::clicked, this, &MainWindow::onDetectModel);
    connect(m_worker, &Worker::modelDetected, this, &MainWindow::onModelDetected);
    connect(m_worker, &Worker::portsChanged, this, &MainWindow::onRefreshPorts);
    connect(m_comCombo, &QComboBox::currentIndexChanged, this, [this](int) { updateActionState(); });
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
        updateActionState();
    });

    applyOptionState();
    if (m_fileLogging) openLogFile();
    appendLog(QStringLiteral("Karabas Flasher v") + QString::fromLatin1(APP_VERSION) + QStringLiteral(" (") +
              QString::fromLatin1(APP_GIT_DESCRIBE) + QLatin1Char(')'));
    onRefreshPorts();
    appendLog(QStringLiteral("Application is ready."));
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

    if (m_logFile.isOpen()) {
        m_logFile.write((line + QLatin1Char('\n')).toUtf8());
        m_logFile.flush();
    }
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
    return p;
}

void MainWindow::setBusy(bool busy) {
    m_busy = busy;
    const bool en = !busy;
    for (QWidget* w : {static_cast<QWidget*>(m_modelCombo), static_cast<QWidget*>(m_downloadBtn),
                       static_cast<QWidget*>(m_fwChk), static_cast<QWidget*>(m_extraChk),
                       static_cast<QWidget*>(m_coresChk), static_cast<QWidget*>(m_delTempChk),
                       static_cast<QWidget*>(m_browseBtn), static_cast<QWidget*>(m_comCombo),
                       static_cast<QWidget*>(m_refreshBtn), static_cast<QWidget*>(m_flashBtn),
                       static_cast<QWidget*>(m_mscBtn), static_cast<QWidget*>(m_mscMode1Btn),
                       static_cast<QWidget*>(m_mscMode2Btn), static_cast<QWidget*>(m_mscHelp),
                       static_cast<QWidget*>(m_detectBtn)}) {
        w->setEnabled(en);
    }
    if (en) m_downloadBtn->setText(tr("Download via GitHub"));
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
    }
    if (m_extraChk->isChecked()) {
        const QString marker = workDir() + QStringLiteral("/extra/extra_complete.txt");
        if (QFile::exists(marker)) canFlash = true;
    }
    m_flashBtn->setEnabled(canFlash);

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
    const Params p = collectParams();
    m_worker->clearCancel();        // сброс в GUI-потоке до постановки задачи в очередь
    QMetaObject::invokeMethod(m_worker, [w = m_worker, p]() { w->runDownload(p); }, Qt::QueuedConnection);
}

void MainWindow::onFlash() {
    if (m_busy) return;
    setBusy(true);
    const Params p = collectParams();
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
    setBusy(true);
    const Params p = collectParams();
    m_worker->clearCancel();
    QMetaObject::invokeMethod(m_worker, [w = m_worker, p]() { w->runDetectModel(p); }, Qt::QueuedConnection);
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
    m_comCombo->clear();
    m_comCombo->addItem(tr("(not selected - flash without reset)"), QString());
    appendLog(QStringLiteral("Scanning available serial ports..."));

    QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    std::sort(ports.begin(), ports.end(), [](const QSerialPortInfo& a, const QSerialPortInfo& b) {
        return a.systemLocation() < b.systemLocation();
    });
    int added = 0;
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
        if (!info.description().isEmpty()) text += " (" + info.description() + ")";
        if (info.hasVendorIdentifier() && info.hasProductIdentifier()) {
            text += QStringLiteral(" [%1:%2]")
                        .arg(info.vendorIdentifier(), 4, 16, QLatin1Char('0'))
                        .arg(info.productIdentifier(), 4, 16, QLatin1Char('0'));
        }
        m_comCombo->addItem(text, location);
        appendLog("Found port: " + text);
        ++added;
    }
    if (added == 0) {
        appendLog(QStringLiteral("Error: Active serial ports not found. Connect the Karabas by USB "
                                 "and press 'Refresh ports'."));
    }
    // Не выбираем порт автоматически, если USB-портов больше одного: можно случайно
    // отправить команду MSC или 1200 bps на чужое устройство (Arduino и т.п.).
    m_comCombo->setCurrentIndex(added == 1 ? 1 : 0);
    if (added > 1) {
        appendLog(QStringLiteral("Warning: More than one USB serial port found. Select the Karabas port "
                                 "manually."));
    }
    updateActionState();
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
           "no .kg1/.kg2/.kg3 cores and no backup_* folder were found.\n\n"
           "Files in the root: %4\n\n"
           "Writing to this drive will replace files on it. Continue?")
            .arg(node, label, sizeText, filesText),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (accepted) *accepted = (r == QMessageBox::Yes);
    appendLog(QStringLiteral("User %1 writing to unrecognized drive %2.")
                  .arg(r == QMessageBox::Yes ? QStringLiteral("confirmed") : QStringLiteral("declined"), node));
}

void MainWindow::onFirmwareReady(const QString& path) {
    m_firmwarePath = path;
    m_firmwareModelIndex = m_modelCombo->currentIndex();
    m_pathEdit->setText(path);
    updateActionState();
}

void MainWindow::onWorkerFinished() {
    setBusy(false);
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
