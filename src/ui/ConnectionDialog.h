#pragma once
#include <QDialog>

#include "core/Driver.h"

class QLineEdit;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QStackedWidget;
class QLabel;
class QPushButton;

namespace ds {

// Create or edit one connection. Tests the connection on a worker thread so
// an unreachable host doesn't freeze the dialog.
class ConnectionDialog : public QDialog {
  Q_OBJECT
 public:
  explicit ConnectionDialog(QWidget* parent = nullptr);

  void setConfig(const ConnectionConfig& cfg);
  ConnectionConfig config() const;
  bool shouldSavePassword() const;

 private slots:
  void browseForFile();
  void testConnection();
  void dialectChanged(int index);

 private:
  void updateOkState();

  QLineEdit* name_ = nullptr;
  QComboBox* dialect_ = nullptr;
  QStackedWidget* pages_ = nullptr;

  QLineEdit* filePath_ = nullptr;
  QCheckBox* readOnly_ = nullptr;

  QLineEdit* host_ = nullptr;
  QSpinBox* port_ = nullptr;
  QLineEdit* user_ = nullptr;
  QLineEdit* password_ = nullptr;
  QLineEdit* database_ = nullptr;
  QCheckBox* useSsl_ = nullptr;
  QCheckBox* savePassword_ = nullptr;

  QLabel* testResult_ = nullptr;
  QPushButton* testButton_ = nullptr;
  std::string id_;
};

}  // namespace ds
