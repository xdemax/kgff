#pragma once
#include <QDialog>
#include <QStringList>
#include <QVariantList>

class QCheckBox;
class QLabel;
class QTableWidget;

// Окно сравнения версий и выбора ядер для копирования.
// Зелёные строки (скачанная версия новее) отмечены по умолчанию.
class CoreVersionsDialog : public QDialog {
    Q_OBJECT
public:
    explicit CoreVersionsDialog(const QVariantList& rows, const QString& boardName,
                                bool installedKnown, QWidget* parent = nullptr);

    // Имена файлов, отмеченных для копирования на SD-карту.
    QStringList selectedFiles() const;

private:
    enum class AllMode { All, None, New, Custom };

    void applyAllMode(AllMode mode);
    void updateAllState();

    QTableWidget* m_table = nullptr;
    QCheckBox* m_all = nullptr;
    QLabel* m_count = nullptr;
    AllMode m_allMode = AllMode::New;
};
