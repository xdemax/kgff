#include "CoreVersionsDialog.h"

#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QVariantMap>

CoreVersionsDialog::CoreVersionsDialog(const QVariantList& rows, const QString& boardName,
                                       bool installedKnown, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Downloaded cores for %1").arg(boardName));
    resize(640, 500);

    m_all = new QCheckBox(tr("All selected"));
    m_all->setTristate(true);
    m_all->setToolTip(tr("Click to cycle: all -> none -> only new."));
    m_count = new QLabel;
    auto* top = new QHBoxLayout;
    top->addWidget(m_all);
    top->addWidget(m_count);
    top->addStretch();

    const int rowCount = static_cast<int>(rows.size());
    m_table = new QTableWidget(rowCount, 5, this);
    m_table->setHorizontalHeaderLabels(QStringList()
                                       << QString()
                                       << tr("Core")
                                       << tr("Installed")
                                       << QString()
                                       << tr("Downloaded"));
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(26);
    m_table->setShowGrid(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setWordWrap(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    // Точки вместо стандартных квадратных чекбоксов: выбранная — зелёная,
    // недоступная для платы — пустая серая.
    m_table->setStyleSheet(QStringLiteral(
        "QTableWidget::indicator { width: 14px; height: 14px; border: 1px solid palette(mid);"
        " border-radius: 7px; }"
        "QTableWidget::indicator:checked { background-color: #2ea043; border-color: #2ea043; }"
        "QTableWidget::indicator:disabled { border-color: palette(midlight); }"));

    const QColor green(QStringLiteral("#2ea043"));
    const QColor yellow(QStringLiteral("#b58900"));
    const QColor red(QStringLiteral("#e04040"));

    for (int r = 0; r < rowCount; ++r) {
        const QVariantMap row = rows.at(r).toMap();
        const QString state = row.value(QStringLiteral("state")).toString();
        const QString file = row.value(QStringLiteral("file")).toString();
        const bool blocked = state == QLatin1String("notSupported") || state == QLatin1String("parseError");
        const bool checked = state == QLatin1String("newer");
        const QColor color = state == QLatin1String("newer")
            ? green
            : (state == QLatin1String("older") ? yellow : (blocked ? red : QColor()));

        auto* check = new QTableWidgetItem;
        check->setFlags(blocked ? Qt::ItemIsEnabled
                                : (Qt::ItemIsEnabled | Qt::ItemIsUserCheckable));
        check->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        m_table->setItem(r, 0, check);

        auto* core = new QTableWidgetItem(file);
        core->setFlags(Qt::ItemIsEnabled);
        core->setData(Qt::UserRole, state);
        m_table->setItem(r, 1, core);

        if (blocked) {
            const QString message = state == QLatin1String("notSupported")
                ? QStringLiteral("> ") + tr("not supported for your Karabas")
                : QStringLiteral("> ") + tr("cannot read the downloaded core header: %1")
                      .arg(row.value(QStringLiteral("detail")).toString());
            auto* info = new QTableWidgetItem(message);
            info->setFlags(Qt::ItemIsEnabled);
            m_table->setItem(r, 2, info);
            m_table->setSpan(r, 2, 1, 3);
            if (color.isValid()) {
                check->setForeground(QBrush(color));
                core->setForeground(QBrush(color));
                info->setForeground(QBrush(color));
            }
            continue;
        }

        auto* installed = new QTableWidgetItem(row.value(QStringLiteral("installed")).toString());
        installed->setFlags(Qt::ItemIsEnabled);
        auto* arrow = new QTableWidgetItem(QStringLiteral(">"));
        arrow->setFlags(Qt::ItemIsEnabled);
        auto* downloaded = new QTableWidgetItem(row.value(QStringLiteral("downloaded")).toString());
        downloaded->setFlags(Qt::ItemIsEnabled);
        m_table->setItem(r, 2, installed);
        m_table->setItem(r, 3, arrow);
        m_table->setItem(r, 4, downloaded);

        if (color.isValid()) {
            const QBrush brush(color);
            check->setForeground(brush);
            core->setForeground(brush);
            installed->setForeground(brush);
            arrow->setForeground(brush);
            downloaded->setForeground(brush);
        }
    }

    connect(m_all, &QCheckBox::clicked, this, [this]() {
        AllMode next = AllMode::All;
        if (m_allMode == AllMode::All) next = AllMode::None;
        else if (m_allMode == AllMode::None) next = AllMode::New;
        applyAllMode(next);
    });
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item && item->column() == 0) updateAllState();
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    if (!installedKnown) {
        auto* note = new QLabel(tr("Installed versions are unknown: connect the board and press the magnifier, "
                                   "or select the serial port before downloading."));
        note->setWordWrap(true);
        layout->addWidget(note);
    }
    layout->addWidget(m_table);
    layout->addWidget(buttons);

    updateAllState();
}

void CoreVersionsDialog::applyAllMode(AllMode mode) {
    if (!m_table) return;
    QSignalBlocker blocker(m_table);
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QTableWidgetItem* item = m_table->item(r, 0);
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable)) continue;
        const QTableWidgetItem* core = m_table->item(r, 1);
        const bool isNew = core && core->data(Qt::UserRole).toString() == QLatin1String("newer");
        const bool checked = mode == AllMode::All || (mode == AllMode::New && isNew);
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    updateAllState();
}

void CoreVersionsDialog::updateAllState() {
    if (!m_all || !m_count || !m_table) return;

    int total = 0;
    int checked = 0;
    int newTotal = 0;
    int newChecked = 0;
    int nonNewChecked = 0;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QTableWidgetItem* item = m_table->item(r, 0);
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable)) continue;
        ++total;
        const QTableWidgetItem* core = m_table->item(r, 1);
        const bool isNew = core && core->data(Qt::UserRole).toString() == QLatin1String("newer");
        if (isNew) ++newTotal;
        if (item->checkState() == Qt::Checked) {
            ++checked;
            if (isNew) ++newChecked;
            else ++nonNewChecked;
        }
    }

    if (checked == 0) m_allMode = AllMode::None;
    else if (checked == total) m_allMode = AllMode::All;
    else if (newChecked == newTotal && nonNewChecked == 0) m_allMode = AllMode::New;
    else m_allMode = AllMode::Custom;

    QString modeName;
    switch (m_allMode) {
    case AllMode::All: modeName = tr("all"); break;
    case AllMode::None: modeName = tr("none"); break;
    case AllMode::New: modeName = tr("only new"); break;
    default: modeName = tr("custom"); break;
    }
    m_count->setText(tr("Checked: %1 of %2 (%3)").arg(checked).arg(total).arg(modeName));

    QSignalBlocker blocker(m_all);
    if (m_allMode == AllMode::All) m_all->setCheckState(Qt::Checked);
    else if (m_allMode == AllMode::None) m_all->setCheckState(Qt::Unchecked);
    else m_all->setCheckState(Qt::PartiallyChecked);
}

QStringList CoreVersionsDialog::selectedFiles() const {
    QStringList files;
    if (!m_table) return files;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const QTableWidgetItem* check = m_table->item(r, 0);
        if (!check || !(check->flags() & Qt::ItemIsUserCheckable) || check->checkState() != Qt::Checked) {
            continue;
        }
        const QTableWidgetItem* core = m_table->item(r, 1);
        if (core) files << core->text();
    }
    return files;
}
