#include "ui/HistoryPanel.h"

#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "ui/Theme.h"

namespace ds {
namespace {

// Enough to be useful, small enough that the settings file stays sane.
constexpr int kMaxEntries = 300;
constexpr int kSqlRole = Qt::UserRole + 1;

QString oneLine(const QString& sql) {
  return sql.simplified();
}

}  // namespace

HistoryPanel::HistoryPanel(QWidget* parent) : QWidget(parent) {
  filter_ = new QLineEdit(this);
  filter_->setPlaceholderText(QStringLiteral("Search history…"));
  filter_->setClearButtonEnabled(true);

  tree_ = new QTreeWidget(this);
  tree_->setColumnCount(4);
  tree_->setHeaderLabels({QStringLiteral("Statement"), QStringLiteral("Rows"),
                          QStringLiteral("Time"), QStringLiteral("When")});
  tree_->setRootIsDecorated(false);
  tree_->setUniformRowHeights(true);
  tree_->setAlternatingRowColors(false);
  tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  tree_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  tree_->setContextMenuPolicy(Qt::CustomContextMenu);
  tree_->setTextElideMode(Qt::ElideRight);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(8, 6, 8, 6);
  layout->setSpacing(6);
  layout->addWidget(filter_);
  layout->addWidget(tree_, 1);

  connect(filter_, &QLineEdit::textChanged, this, [this] { rebuild(); });
  connect(tree_, &QTreeWidget::itemActivated, this,
          [this](QTreeWidgetItem* item, int) {
            if (item) emit restoreRequested(item->data(0, kSqlRole).toString());
          });
  connect(tree_, &QWidget::customContextMenuRequested, this,
          [this](const QPoint& pos) {
            QTreeWidgetItem* item = tree_->itemAt(pos);
            if (!item) return;
            const QString sql = item->data(0, kSqlRole).toString();
            QMenu menu(this);
            menu.addAction(QStringLiteral("Load into editor"), this,
                           [this, sql] { emit restoreRequested(sql); });
            menu.addAction(QStringLiteral("Run again"), this,
                           [this, sql] { emit runRequested(sql); });
            menu.addAction(QStringLiteral("Copy"), this, [sql] {
              QApplication::clipboard()->setText(sql);
            });
            menu.exec(tree_->viewport()->mapToGlobal(pos));
          });

  load();
  rebuild();
}

HistoryPanel::~HistoryPanel() { persist(); }

void HistoryPanel::record(const HistoryEntry& entry) {
  if (entry.sql.trimmed().isEmpty()) return;

  // Re-running the same statement moves it up rather than duplicating it.
  const QString key = oneLine(entry.sql);
  for (int i = 0; i < entries_.size(); ++i) {
    if (oneLine(entries_[i].sql) == key) {
      entries_.remove(i);
      break;
    }
  }
  entries_.prepend(entry);
  while (entries_.size() > kMaxEntries) entries_.removeLast();
  rebuild();
}

void HistoryPanel::rebuild() {
  tree_->clear();
  const QString needle = filter_->text().trimmed();
  const QFont mono = theme::monospaceFont(11);

  for (const auto& e : entries_) {
    if (!needle.isEmpty() &&
        !e.sql.contains(needle, Qt::CaseInsensitive) &&
        !e.connectionName.contains(needle, Qt::CaseInsensitive)) {
      continue;
    }

    auto* item = new QTreeWidgetItem(tree_);
    item->setText(0, oneLine(e.sql));
    item->setFont(0, mono);
    item->setData(0, kSqlRole, e.sql);
    item->setIcon(0, theme::statusDot(e.succeeded ? theme::success()
                                                  : theme::danger()));

    item->setText(1, e.rowCount >= 0 ? QString::number(e.rowCount) : QString());
    item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    item->setText(2, QStringLiteral("%1 ms").arg(e.elapsedMs, 0, 'f', 1));
    item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
    item->setForeground(2, theme::textMuted());

    // Recent things read better as "4 min ago" than as a timestamp.
    const qint64 secs = e.when.secsTo(QDateTime::currentDateTime());
    QString when;
    if (secs < 60) when = QStringLiteral("just now");
    else if (secs < 3600) when = QStringLiteral("%1 min ago").arg(secs / 60);
    else if (secs < 86400) when = QStringLiteral("%1 h ago").arg(secs / 3600);
    else when = e.when.toString(QStringLiteral("d MMM"));
    item->setText(3, when);
    item->setForeground(3, theme::textMuted());

    QString tip = e.sql;
    if (!e.connectionName.isEmpty()) {
      tip += QStringLiteral("\n\non %1").arg(e.connectionName);
    }
    if (!e.succeeded) tip += QStringLiteral("\n\nfailed: %1").arg(e.error);
    item->setToolTip(0, tip);
  }
}

void HistoryPanel::load() {
  QSettings settings(QStringLiteral("DataStudio"), QStringLiteral("DataStudio"));
  const int count = settings.value(QStringLiteral("history/count"), 0).toInt();
  for (int i = 0; i < count; ++i) {
    const QString k = QStringLiteral("history/%1/").arg(i);
    HistoryEntry e;
    e.sql = settings.value(k + "sql").toString();
    if (e.sql.isEmpty()) continue;
    e.connectionName = settings.value(k + "connection").toString();
    e.when = settings.value(k + "when").toDateTime();
    e.elapsedMs = settings.value(k + "elapsed").toDouble();
    e.rowCount = settings.value(k + "rows", -1).toLongLong();
    e.succeeded = settings.value(k + "ok", true).toBool();
    e.error = settings.value(k + "error").toString();
    entries_.append(e);
  }
}

void HistoryPanel::persist() const {
  QSettings settings(QStringLiteral("DataStudio"), QStringLiteral("DataStudio"));
  settings.remove(QStringLiteral("history"));
  settings.setValue(QStringLiteral("history/count"), entries_.size());
  for (int i = 0; i < entries_.size(); ++i) {
    const QString k = QStringLiteral("history/%1/").arg(i);
    const HistoryEntry& e = entries_[i];
    settings.setValue(k + "sql", e.sql);
    settings.setValue(k + "connection", e.connectionName);
    settings.setValue(k + "when", e.when);
    settings.setValue(k + "elapsed", e.elapsedMs);
    settings.setValue(k + "rows", static_cast<qlonglong>(e.rowCount));
    settings.setValue(k + "ok", e.succeeded);
    settings.setValue(k + "error", e.error);
  }
  settings.sync();
}

}  // namespace ds
