#include "ui/CommandPalette.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "ui/Theme.h"

namespace ds {
namespace {

constexpr int kPayloadRole = Qt::UserRole + 1;
constexpr int kKindRole = Qt::UserRole + 2;
constexpr int kIndexRole = Qt::UserRole + 3;

// Same shape as the editor's ranking: subsequence match, rewarding hits at
// word boundaries and penalising long candidates.
int fuzzyScore(const QString& needle, const QString& hay) {
  if (needle.isEmpty()) return 1;
  if (needle.length() > hay.length()) return -1;

  int hi = 0, score = 0, streak = 0;
  for (int ni = 0; ni < needle.length(); ++ni) {
    const QChar nc = needle.at(ni).toLower();
    bool found = false;
    while (hi < hay.length()) {
      const QChar hc = hay.at(hi).toLower();
      const bool boundary = hi == 0 || hay.at(hi - 1) == QLatin1Char('_') ||
                            hay.at(hi - 1) == QLatin1Char('.') ||
                            hay.at(hi - 1) == QLatin1Char(' ');
      ++hi;
      if (hc == nc) {
        score += 10;
        if (boundary) score += 15;
        streak = streak ? streak + 1 : 1;
        score += streak * 3;
        found = true;
        break;
      }
      streak = 0;
      score -= 1;
    }
    if (!found) return -1;
  }
  return score - hay.length() / 4;
}

}  // namespace

CommandPalette::CommandPalette(QWidget* parent)
    : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint) {
  setModal(true);
  setMinimumWidth(560);

  input_ = new QLineEdit(this);
  input_->setPlaceholderText(
      QStringLiteral("Jump to a table or column, or run a command…"));
  input_->installEventFilter(this);

  list_ = new QListWidget(this);
  list_->setUniformItemSizes(true);
  list_->setMaximumHeight(340);
  list_->setFocusPolicy(Qt::NoFocus);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 10, 10, 10);
  layout->setSpacing(8);
  layout->addWidget(input_);
  layout->addWidget(list_);

  setStyleSheet(QStringLiteral(
                    "QDialog { background: %1; border: 1px solid %2; "
                    "border-radius: 10px; }"
                    "QLineEdit { font-size: 15px; padding: 8px 10px; }")
                    .arg(theme::surface().name(), theme::border().name()));

  connect(input_, &QLineEdit::textChanged, this, &CommandPalette::refilter);
  // A launcher should act on a single click; waiting for a double-click is
  // the wrong idiom here.
  connect(list_, &QListWidget::itemClicked, this,
          [this](QListWidgetItem* item) {
            list_->setCurrentItem(item);
            accept();
          });
}

void CommandPalette::setSchema(const Schema& schema) {
  entries_.clear();
  for (const auto& t : schema.tables) {
    const QString name = QString::fromStdString(t.name);

    PaletteEntry table;
    table.kind = PaletteEntry::Kind::Table;
    table.title = name;
    table.detail = QStringLiteral("%1 · %2 columns")
                       .arg(t.isView ? QStringLiteral("view")
                                     : QStringLiteral("table"))
                       .arg(t.columns.size());
    table.payload = name;
    entries_.append(table);

    for (const auto& c : t.columns) {
      PaletteEntry column;
      column.kind = PaletteEntry::Kind::Column;
      column.title = name + QLatin1Char('.') + QString::fromStdString(c.name);
      column.detail = QString::fromStdString(c.type);
      column.payload = column.title;
      entries_.append(column);
    }
  }
}

void CommandPalette::addAction(const QString& title, const QString& detail,
                               std::function<void()> run) {
  PaletteEntry entry;
  entry.kind = PaletteEntry::Kind::Action;
  entry.title = title;
  entry.detail = detail;
  entry.run = std::move(run);
  actions_.append(entry);
}

void CommandPalette::reset() {
  input_->clear();
  refilter(QString());
  input_->setFocus();
}

void CommandPalette::refilter(const QString& query) {
  list_->clear();

  struct Scored {
    int score;
    const PaletteEntry* entry;
    int actionIndex;   // -1 unless this came from the action pool
  };
  QVector<Scored> hits;

  auto consider = [&](const QVector<PaletteEntry>& pool, int bias,
                      bool isActionPool) {
    for (int i = 0; i < pool.size(); ++i) {
      const int score = fuzzyScore(query, pool[i].title);
      if (score < 0) continue;
      hits.append({score + bias, &pool[i], isActionPool ? i : -1});
    }
  };
  // Commands rank above raw schema names for the same match quality: if you
  // typed something that is both, you probably meant the verb.
  consider(actions_, 30, true);
  consider(entries_, 0, false);

  std::stable_sort(hits.begin(), hits.end(),
                   [](const Scored& a, const Scored& b) {
                     return a.score > b.score;
                   });
  if (hits.size() > 60) hits.resize(60);

  for (const auto& hit : hits) {
    const PaletteEntry& e = *hit.entry;
    auto* item = new QListWidgetItem(list_);
    item->setText(e.title);
    item->setData(kPayloadRole, e.payload);
    item->setData(kKindRole, static_cast<int>(e.kind));
    // Index back into whichever pool this came from, for actions.
    item->setData(kIndexRole, hit.actionIndex);
    switch (e.kind) {
      case PaletteEntry::Kind::Table:
        item->setIcon(theme::icon(QStringLiteral("table"), theme::tableColour()));
        break;
      case PaletteEntry::Kind::Column:
        item->setIcon(theme::icon(QStringLiteral("column"), theme::textMuted()));
        break;
      case PaletteEntry::Kind::Action:
        item->setIcon(theme::icon(QStringLiteral("run"), theme::accent()));
        break;
    }
    item->setToolTip(e.detail);
  }
  if (list_->count() > 0) list_->setCurrentRow(0);
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event) {
  if (watched == input_ && event->type() == QEvent::KeyPress) {
    auto* key = static_cast<QKeyEvent*>(event);
    switch (key->key()) {
      case Qt::Key_Down:
        list_->setCurrentRow(qMin(list_->currentRow() + 1, list_->count() - 1));
        return true;
      case Qt::Key_Up:
        list_->setCurrentRow(qMax(list_->currentRow() - 1, 0));
        return true;
      case Qt::Key_Return:
      case Qt::Key_Enter:
        accept();
        return true;
      default:
        break;
    }
  }
  return QDialog::eventFilter(watched, event);
}

void CommandPalette::accept() {
  QListWidgetItem* item = list_->currentItem();
  if (!item) {
    QDialog::reject();
    return;
  }

  const auto kind = static_cast<PaletteEntry::Kind>(
      item->data(kKindRole).toInt());
  const QString payload = item->data(kPayloadRole).toString();
  const int actionIndex = item->data(kIndexRole).toInt();

  // Close before running, so anything the action opens lands on top.
  QDialog::accept();

  switch (kind) {
    case PaletteEntry::Kind::Table:
      emit tableChosen(payload);
      break;
    case PaletteEntry::Kind::Column: {
      const int dot = payload.lastIndexOf(QLatin1Char('.'));
      if (dot > 0) {
        emit columnChosen(payload.left(dot), payload.mid(dot + 1));
      }
      break;
    }
    case PaletteEntry::Kind::Action:
      if (actionIndex >= 0 && actionIndex < actions_.size() &&
          actions_[actionIndex].run) {
        actions_[actionIndex].run();
      }
      break;
  }
}

}  // namespace ds
