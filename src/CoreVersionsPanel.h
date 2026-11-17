#pragma once
#include <QStringList>
#include <QVariantList>
#include <QWidget>

class QCheckBox;
class QLabel;
class QTableWidget;

// Правая панель главного окна: сравнение версий и выбор ядер для копирования.
// Зелёные строки (скачанная версия новее) отмечены по умолчанию.
class CoreVersionsPanel : public QWidget {
    Q_OBJECT
public:
    explicit CoreVersionsPanel(QWidget* parent = nullptr);

    void setRows(const QVariantList& rows, const QString& boardName, bool installedKnown);
    void clear();
    bool hasRows() const;

    // Имена файлов, отмеченных для копирования на SD-карту.
    QStringList selectedFiles() const;

private:
    enum class AllMode { All, None, New, Custom };

    void applyAllMode(AllMode mode);
    void updateAllState();

    QLabel* m_title = nullptr;
    QLabel* m_note = nullptr;
    QTableWidget* m_table = nullptr;
    QCheckBox* m_all = nullptr;
    QLabel* m_count = nullptr;
    AllMode m_allMode = AllMode::New;
    int m_newTotal = 0;         // сколько ядер вообще можно считать «новыми»
};
