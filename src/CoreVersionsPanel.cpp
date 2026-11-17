#include "CoreVersionsPanel.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QVariantMap>

namespace {

// Круглая точка выбора в первой колонке. Для notSupported она красная, пока
// ядро не выбрано, и зелёная после выбора. Обычные ядра — серая/зелёная.
class CheckDotDelegate : public QStyledItemDelegate {
public:
    explicit CheckDotDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setWidth(qMax(size.width(), 26));
        return size;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.icon = QIcon();
        opt.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        const QWidget* widget = opt.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        const bool checked = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
        const bool notSupported =
            index.data(Qt::UserRole).toString() == QLatin1String("notSupported");
        QColor color;
        if (checked) color = QColor(QStringLiteral("#2ea043"));
        else if (notSupported) color = QColor(QStringLiteral("#e04040"));
        else color = opt.palette.color(QPalette::Mid);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        QPen pen(color);
        pen.setWidthF(1.5);
        painter->setPen(pen);
        painter->setBrush(checked ? QBrush(color) : QBrush(Qt::NoBrush));
        painter->drawEllipse(dotRect(option.rect));
        painter->restore();
    }

    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option, const QModelIndex& index) override {
        if (!(model->flags(index) & Qt::ItemIsUserCheckable)) return false;
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (!dotRect(option.rect).contains(mouse->position().toPoint())) return false;
            if (event->type() == QEvent::MouseButtonRelease) {
                const Qt::CheckState state = index.data(Qt::CheckStateRole).toInt() == Qt::Checked
                    ? Qt::Unchecked
                    : Qt::Checked;
                model->setData(index, state, Qt::CheckStateRole);
            }
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Space || key->key() == Qt::Key_Select) {
                const Qt::CheckState state = index.data(Qt::CheckStateRole).toInt() == Qt::Checked
                    ? Qt::Unchecked
                    : Qt::Checked;
                model->setData(index, state, Qt::CheckStateRole);
                return true;
            }
        }
        return false;
    }

private:
    static QRect dotRect(const QRect& cell) {
        constexpr int diameter = 14;
        return QRect(cell.left() + 6, cell.center().y() - diameter / 2, diameter, diameter);
    }
};

}  // namespace

CoreVersionsPanel::CoreVersionsPanel(QWidget* parent) : QWidget(parent) {
    m_title = new QLabel(tr("Downloaded cores"));
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    m_title->setWordWrap(true);

    m_note = new QLabel(tr("Installed versions are unknown: connect the board and press the magnifier, "
                           "or select the serial port before downloading."));
    m_note->setWordWrap(true);
    m_note->hide();

    m_all = new QCheckBox(tr("All selected"));
    m_all->setTristate(true);
    m_all->setToolTip(tr("Click to cycle: all -> none -> only new."));
    m_all->setEnabled(false);
    m_count = new QLabel;
    auto* top = new QHBoxLayout;
    top->addWidget(m_all);
    top->addWidget(m_count);
    top->addStretch();

    m_table = new QTableWidget(0, 5, this);
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
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->setItemDelegateForColumn(0, new CheckDotDelegate(m_table));

    connect(m_all, &QCheckBox::clicked, this, [this]() {
        AllMode next = AllMode::All;
        if (m_allMode == AllMode::All) {
            next = AllMode::None;
        } else if (m_allMode == AllMode::None) {
            // Если новых ядер нет, состояние «только новые» совпадает с «ни одного»:
            // пропускаем его, иначе клик выглядит как будто ничего не произошло.
            next = m_newTotal > 0 ? AllMode::New : AllMode::All;
        }
        applyAllMode(next);
    });
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item && item->column() == 0) updateAllState();
    });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_title);
    layout->addWidget(m_note);
    layout->addLayout(top);
    layout->addWidget(m_table, 1);

    clear();
}

void CoreVersionsPanel::setRows(const QVariantList& rows, const QString& boardName, bool installedKnown) {
    m_title->setText(tr("Downloaded cores for %1").arg(boardName));
    m_note->setVisible(!installedKnown);

    m_table->clearSpans();
    m_table->clearContents();
    const int rowCount = static_cast<int>(rows.size());
    m_table->setRowCount(rowCount);
    m_all->setEnabled(rowCount > 0);

    const QColor green(QStringLiteral("#2ea043"));
    const QColor yellow(QStringLiteral("#b58900"));
    const QColor red(QStringLiteral("#e04040"));

    for (int r = 0; r < rowCount; ++r) {
        const QVariantMap row = rows.at(r).toMap();
        const QString state = row.value(QStringLiteral("state")).toString();
        const QString file = row.value(QStringLiteral("file")).toString();
        const bool notSupported = state == QLatin1String("notSupported");
        const bool parseError = state == QLatin1String("parseError");
        const bool blocked = notSupported || parseError;
        const bool checked = state == QLatin1String("newer");
        const QColor color = state == QLatin1String("newer")
            ? green
            : (state == QLatin1String("older") ? yellow : (blocked ? red : QColor()));

        auto* check = new QTableWidgetItem;
        // notSupported можно включить вручную: по умолчанию ядро не трогаем,
        // но пользователь может принудительно скопировать его на карту.
        check->setFlags(parseError ? Qt::ItemIsEnabled
                                   : (Qt::ItemIsEnabled | Qt::ItemIsUserCheckable));
        if (notSupported) {
            check->setToolTip(tr("Not supported by this board. Check to copy it anyway."));
        } else if (parseError) {
            check->setToolTip(tr("Cannot read the header; this core cannot be selected."));
        }
        check->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        check->setData(Qt::UserRole, state);
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
            info->setToolTip(message);
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

    updateAllState();
}

void CoreVersionsPanel::clear() {
    m_title->setText(tr("Downloaded cores"));
    m_note->hide();
    m_table->clearSpans();
    m_table->clearContents();
    m_table->setRowCount(0);
    m_all->setEnabled(false);
    QSignalBlocker blocker(m_all);
    m_all->setCheckState(Qt::Unchecked);
    m_count->clear();
    m_allMode = AllMode::None;
    m_newTotal = 0;
}

bool CoreVersionsPanel::hasRows() const {
    return m_table && m_table->rowCount() > 0;
}

void CoreVersionsPanel::applyAllMode(AllMode mode) {
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

void CoreVersionsPanel::updateAllState() {
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

    m_newTotal = newTotal;
    if (total == 0) {
        m_allMode = AllMode::None;
        QSignalBlocker blocker(m_all);
        m_all->setCheckState(Qt::Unchecked);
        m_count->setText(tr("No cores"));
        return;
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

QStringList CoreVersionsPanel::selectedFiles() const {
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
