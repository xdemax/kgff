#pragma once
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QWidget>

class QCheckBox;
class QLabel;
class QTableWidget;
class QTimer;

// Правая панель главного окна: сравнение версий и выбор ядер для копирования.
// Зелёные строки (скачанная версия новее) отмечены по умолчанию; третье
// состояние точки — ignored: ядро не копируется и запоминается в QSettings.
class CoreVersionsPanel : public QWidget {
    Q_OBJECT
public:
    explicit CoreVersionsPanel(QWidget* parent = nullptr);

    void setRows(const QVariantList& rows, const QString& boardName, bool installedKnown);
    void clear();
    bool hasRows() const;

    // Имена файлов, отмеченных для копирования на SD-карту.
    QStringList selectedFiles() const;
    // Постоянно игнорируемые ядра (третье состояние точки).
    QStringList ignoredFiles() const;
    // Статус прошивки платы относительно последней сборки на GitHub.
    void setFirmwareStatus(bool known, bool needsUpdate,
                           const QString& installed, const QString& latest);

private:
    enum class AllMode { All, None, New, Custom };

    void applyAllMode(AllMode mode);
    void updateAllState();
    void updateRowAppearance(int row);
    void loadIgnored();
    void saveIgnored();

    QLabel* m_title = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_note = nullptr;
    QTableWidget* m_table = nullptr;
    QCheckBox* m_all = nullptr;
    QLabel* m_count = nullptr;
    QTimer* m_blinkTimer = nullptr;
    bool m_blinkOn = false;
    bool m_needsUpdate = false;
    AllMode m_allMode = AllMode::New;
    int m_newTotal = 0;         // сколько ядер вообще можно считать «новыми»
    QVariantList m_rows;        // исходные данные строк для перерисовки
    QSet<QString> m_ignored;    // имена файлов в нижнем регистре
    bool m_filling = false;     // защита от itemChanged во время заполнения
};
