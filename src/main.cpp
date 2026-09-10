#include <QApplication>

#include "ui/MainWindow.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("Data Studio"));
  app.setOrganizationName(QStringLiteral("DataStudio"));
  app.setApplicationVersion(QStringLiteral("0.1.0"));

  ds::MainWindow window;
  window.show();
  return app.exec();
}
