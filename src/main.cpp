#include <QApplication>

#include "ui/MainWindow.h"
#include "ui/Theme.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("Data Studio"));
  app.setOrganizationName(QStringLiteral("DataStudio"));
  app.setApplicationVersion(QStringLiteral("0.1.0"));

  // The stylesheet reads the palette, so it must be built after QApplication.
  app.setStyleSheet(ds::theme::appStyleSheet());

  ds::MainWindow window;
  window.show();
  return app.exec();
}
