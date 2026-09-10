#pragma once
#include <QColor>
#include <QIcon>
#include <QString>

namespace ds {

// One place for the colours and metrics the widgets share, so light and dark
// stay consistent and nothing hard-codes a hex value at the call site.
namespace theme {

bool isDark();

QColor accent();          // primary action, selection
QColor accentMuted();
QColor surface();         // panel background
QColor surfaceRaised();   // toolbar, headers
QColor border();
QColor textPrimary();
QColor textMuted();
QColor danger();
QColor warning();
QColor success();
QColor info();

// Type-specific colours reused by the schema tree, completer and highlighter.
QColor tableColour();
QColor viewColour();
QColor keyColour();
QColor indexColour();
QColor relationColour();

// Icons are painted rather than loaded, so the app ships no image assets and
// every glyph follows the current palette.
//  run, runAll, explain, cancel, refresh, database, table, view, column,
//  primaryKey, foreignKey, index, sparkle, plug, warning, error, info
QIcon icon(const QString& name, const QColor& tint = QColor());

// Small round status dot, for connection state.
QIcon statusDot(const QColor& colour);

QString appStyleSheet();

// Monospace face used by the editor, result grid and inspector.
QFont monospaceFont(int pointSize = 13);

}  // namespace theme
}  // namespace ds
