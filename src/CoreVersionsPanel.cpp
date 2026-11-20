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
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
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

        const int checkState = index.data(Qt::CheckStateRole).toInt();
        const bool checked = checkState == Qt::Checked;
        const bool ignored = checkState == Qt::PartiallyChecked;
        const bool notSupported =
            index.data(Qt::UserRole).toString() == QLatin1String("notSupported");
        QColor color;
        if (ignored) color = QColor(QStringLiteral("#57606a"));
        else if (checked) color = QColor(QStringLiteral("#2ea043"));
        else if (notSupported) color = QColor(QStringLiteral("#e04040"));
        else color = opt.palette.color(QPalette::Mid);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        QPen pen(color);
        pen.setWidthF(1.5);
        painter->setPen(pen);
        painter->setBrush((checked || ignored) ? QBrush(color) : QBrush(Qt::NoBrush));
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
                const Qt::CheckState current = static_cast<Qt::CheckState>(
                    index.data(Qt::CheckStateRole).toInt());
                const Qt::CheckState next = current == Qt::Checked
                    ? Qt::PartiallyChecked
                    : (current == Qt::PartiallyChecked ? Qt::Unchecked : Qt::Checked);
                model->setData(index, next, Qt::CheckStateRole);
            }
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Space || key->key() == Qt::Key_Select) {
                const Qt::CheckState current = static_cast<Qt::CheckState>(
                    index.data(Qt::CheckStateRole).toInt());
                const Qt::CheckState next = current == Qt::Checked
                    ? Qt::PartiallyChecked
                    : (current == Qt::PartiallyChecked ? Qt::Unchecked : Qt::Checked);
                model->setData(index, next, Qt::CheckStateRole);
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

    m_status = new QLabel;
    m_status->setVisible(false);
    m_status->setAlignment(Qt::AlignRight | Qt::AlignTop);
    m_status->setStyleSheet(QStringLiteral("font-weight: bold;"));

    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(600);
    connect(m_blinkTimer, &QTimer::timeout, this, [this]() {
        if (!m_needsUpdate || !m_status->isVisible()) return;
        m_blinkOn = !m_blinkOn;
        m_status->setStyleSheet(m_blinkOn
                                    ? QStringLiteral("color: #e04040; font-weight: bold;")
                                    : QStringLiteral("color: transparent; font-weight: bold;"));
    });

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
        if (m_filling || !item || item->column() != 0) return;
        const int row = item->row();
        updateRowAppearance(row);
        const QTableWidgetItem* core = m_table->item(row, 1);
        if (!core) return;
        const QString key = core->text().toLower();
        if (item->checkState() == Qt::PartiallyChecked) m_ignored.insert(key);
        else m_ignored.remove(key);
        saveIgnored();
        updateAllState();
    });

    auto* header = new QHBoxLayout;
    header->addWidget(m_title, 1);
    header->addWidget(m_status, 0, Qt::AlignTop | Qt::AlignRight);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(header);
    layout->addWidget(m_note);
    layout->addLayout(top);
    layout->addWidget(m_table, 1);

    loadIgnored();
    clear();
}

void CoreVersionsPanel::setRows(const QVariantList& rows, const QString& boardName, bool installedKnown,
                                bool availableList) {
    m_rows = rows;
    m_availableMode = availableList;
    m_title->setText(availableList ? tr("Available cores for %1").arg(boardName)
                                   : tr("Downloaded cores for %1").arg(boardName));
    m_table->setHorizontalHeaderLabels(QStringList()
                                       << QString()
                                       << tr("Core")
                                       << tr("Installed")
                                       << QString()
                                       << (availableList ? tr("Available") : tr("Downloaded")));
    m_note->setVisible(!installedKnown);

    m_filling = true;
    QSignalBlocker blocker(m_table);
    m_table->clearSpans();
    m_table->clearContents();
    const int rowCount = static_cast<int>(rows.size());
    m_table->setRowCount(rowCount);

    for (int r = 0; r < rowCount; ++r) {
        const QVariantMap row = rows.at(r).toMap();
        const QString state = row.value(QStringLiteral("state")).toString();
        const QString file = row.value(QStringLiteral("file")).toString();
        const bool notSupported = state == QLatin1String("notSupported");
        const bool parseError = state == QLatin1String("parseError");
        const bool blocked = notSupported || parseError;
        const bool checked = state == QLatin1String("newer");
        const bool ignored = !parseError && m_ignored.contains(file.toLower());

        auto* check = new QTableWidgetItem;
        check->setFlags(parseError ? Qt::ItemIsEnabled
                                   : (Qt::ItemIsEnabled | Qt::ItemIsUserCheckable));
        if (notSupported) {
            check->setToolTip(tr("Not supported by this board. Click: copy -> ignored -> skipped."));
        } else if (parseError) {
            check->setToolTip(tr("Cannot read the header; this core cannot be selected."));
        } else {
            check->setToolTip(tr("Click: update -> ignored -> skipped."));
        }
        check->setCheckState(ignored ? Qt::PartiallyChecked
                                     : (checked ? Qt::Checked : Qt::Unchecked));
        check->setData(Qt::UserRole, state);
        m_table->setItem(r, 0, check);

        auto* core = new QTableWidgetItem(file);
        core->setFlags(Qt::ItemIsEnabled);
        core->setData(Qt::UserRole, state);
        m_table->setItem(r, 1, core);

        if (blocked) {
            auto* info = new QTableWidgetItem;
            info->setFlags(Qt::ItemIsEnabled);
            m_table->setItem(r, 2, info);
            m_table->setSpan(r, 2, 1, 3);
            continue;
        }

        auto* installed = new QTableWidgetItem;
        installed->setFlags(Qt::ItemIsEnabled);
        auto* arrow = new QTableWidgetItem(QStringLiteral(">"));
        arrow->setFlags(Qt::ItemIsEnabled);
        auto* downloaded = new QTableWidgetItem;
        downloaded->setFlags(Qt::ItemIsEnabled);
        m_table->setItem(r, 2, installed);
        m_table->setItem(r, 3, arrow);
        m_table->setItem(r, 4, downloaded);
    }

    m_filling = false;
    for (int r = 0; r < rowCount; ++r) updateRowAppearance(r);
    updateAllState();
}

void CoreVersionsPanel::clear() {
    m_filling = true;
    m_rows.clear();
    m_blinkTimer->stop();
    m_needsUpdate = false;
    m_status->hide();
    m_title->setText(tr("Downloaded cores"));
    m_note->hide();
    m_table->clearSpans();
    m_table->clearContents();
    m_table->setRowCount(0);
    m_filling = false;
    m_all->setEnabled(false);
    QSignalBlocker blocker(m_all);
    m_all->setCheckState(Qt::Unchecked);
    m_count->clear();
    m_allMode = AllMode::None;
    m_newTotal = 0;
    m_availableMode = false;
}

bool CoreVersionsPanel::hasRows() const {
    return m_table && m_table->rowCount() > 0;
}

QStringList CoreVersionsPanel::ignoredFiles() const {
    QStringList files;
    for (const QString& file : m_ignored) files << file;
    files.sort();
    return files;
}

QStringList CoreVersionsPanel::allFiles() const {
    QStringList files;
    if (!m_table) return files;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const QTableWidgetItem* core = m_table->item(r, 1);
        if (core && !core->text().isEmpty()) files << core->text();
    }
    return files;
}

void CoreVersionsPanel::setFirmwareStatus(bool known, bool needsUpdate,
                                          const QString& installed, const QString& latest) {
    m_blinkTimer->stop();
    m_needsUpdate = false;
    if (!known) {
        m_status->hide();
        return;
    }

    m_status->show();
    m_status->setToolTip(tr("Installed firmware: %1\nLatest firmware: %2").arg(installed, latest));
    if (needsUpdate) {
        m_needsUpdate = true;
        m_blinkOn = true;
        m_status->setText(tr("Firmware needs an update!"));
        m_status->setStyleSheet(QStringLiteral("color: #e04040; font-weight: bold;"));
        m_blinkTimer->start();
    } else {
        m_status->setText(tr("Firmware is up to date."));
        m_status->setStyleSheet(QStringLiteral("color: #2ea043; font-weight: bold;"));
    }
}

void CoreVersionsPanel::loadIgnored() {
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("KarabasFlasher"), QStringLiteral("KarabasFlasher"));
    const QStringList files = settings.value(QStringLiteral("ignoredCores")).toStringList();
    for (const QString& file : files) {
        const QString key = file.trimmed().toLower();
        if (!key.isEmpty()) m_ignored.insert(key);
    }
}

void CoreVersionsPanel::saveIgnored() {
    QStringList files;
    for (const QString& file : m_ignored) files << file;
    files.sort();
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("KarabasFlasher"), QStringLiteral("KarabasFlasher"));
    settings.setValue(QStringLiteral("ignoredCores"), files);
}

void CoreVersionsPanel::updateRowAppearance(int row) {
    if (!m_table || row < 0 || row >= static_cast<int>(m_rows.size())) return;
    const QVariantMap data = m_rows.at(row).toMap();
    const QString state = data.value(QStringLiteral("state")).toString();
    const bool notSupported = state == QLatin1String("notSupported");
    const bool parseError = state == QLatin1String("parseError");
    const bool blocked = notSupported || parseError;

    QTableWidgetItem* check = m_table->item(row, 0);
    QTableWidgetItem* core = m_table->item(row, 1);
    QTableWidgetItem* second = m_table->item(row, 2);
    QTableWidgetItem* arrow = m_table->item(row, 3);
    QTableWidgetItem* downloaded = m_table->item(row, 4);
    if (!check || !core || !second) return;

    const bool ignored = check->checkState() == Qt::PartiallyChecked;
    QColor color;
    if (ignored) color = QColor(QStringLiteral("#57606a"));
    else if (state == QLatin1String("newer")) color = QColor(QStringLiteral("#2ea043"));
    else if (state == QLatin1String("older")) color = QColor(QStringLiteral("#b58900"));
    else if (blocked) color = QColor(QStringLiteral("#e04040"));

    QSignalBlocker blocker(m_table);
    if (blocked) {
        const QString original = notSupported
            ? QStringLiteral("> ") + tr("not supported for your Karabas")
            : QStringLiteral("> ") + tr("cannot read the downloaded core header: %1")
                  .arg(data.value(QStringLiteral("detail")).toString());
        second->setText(ignored ? QStringLiteral("> ") + tr("ignored") : original);
        second->setToolTip(original);
        m_table->setSpan(row, 2, 1, 3);
    } else {
        second->setText(ignored ? tr("ignored") : data.value(QStringLiteral("installed")).toString());
        if (arrow) arrow->setText(QStringLiteral(">"));
        if (downloaded) downloaded->setText(data.value(QStringLiteral("downloaded")).toString());
    }

    auto applyColor = [&](QTableWidgetItem* item) {
        if (!item) return;
        if (color.isValid()) item->setForeground(QBrush(color));
        else item->setData(Qt::ForegroundRole, QVariant());
    };
    applyColor(check);
    applyColor(core);
    applyColor(second);
    applyColor(arrow);
    applyColor(downloaded);
}

void CoreVersionsPanel::applyAllMode(AllMode mode) {
    if (!m_table) return;
    QSignalBlocker blocker(m_table);
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QTableWidgetItem* item = m_table->item(r, 0);
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable)) continue;
        if (item->checkState() == Qt::PartiallyChecked) continue;   // ignored не трогаем
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
    int ignored = 0;
    int newTotal = 0;
    int newChecked = 0;
    int nonNewChecked = 0;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        QTableWidgetItem* item = m_table->item(r, 0);
        if (!item || !(item->flags() & Qt::ItemIsUserCheckable)) continue;
        if (item->checkState() == Qt::PartiallyChecked) {
            ++ignored;
            continue;
        }
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
    m_all->setEnabled(total > 0);
    if (total == 0) {
        m_allMode = AllMode::None;
        QSignalBlocker blocker(m_all);
        m_all->setCheckState(Qt::Unchecked);
        m_count->setText(ignored > 0 ? tr("All cores are ignored") : tr("No cores"));
        emit selectionChanged();
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
    QString countText = tr("Checked: %1 of %2 (%3)").arg(checked).arg(total).arg(modeName);
    if (ignored > 0) countText += tr(", ignored: %1").arg(ignored);
    m_count->setText(countText);

    QSignalBlocker blocker(m_all);
    if (m_allMode == AllMode::All) m_all->setCheckState(Qt::Checked);
    else if (m_allMode == AllMode::None) m_all->setCheckState(Qt::Unchecked);
    else m_all->setCheckState(Qt::PartiallyChecked);
    emit selectionChanged();
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
