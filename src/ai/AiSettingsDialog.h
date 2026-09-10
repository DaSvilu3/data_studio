#pragma once
#include <QDialog>

#include "ai/AiSettings.h"

class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;

namespace ds {

// Chooses where natural-language SQL is generated: a local model, or Claude.
class AiSettingsDialog : public QDialog {
  Q_OBJECT
 public:
  explicit AiSettingsDialog(QWidget* parent = nullptr);

  AiSettings settings() const;
  void applyAndSave() const;

 private slots:
  void detectLocalServers();
  void testLocalModel();
  void providerChanged();

 private:
  QRadioButton* localRadio_ = nullptr;
  QRadioButton* claudeRadio_ = nullptr;
  QRadioButton* offRadio_ = nullptr;
  QStackedWidget* pages_ = nullptr;

  QComboBox* baseUrl_ = nullptr;
  QComboBox* model_ = nullptr;
  QLineEdit* localKey_ = nullptr;
  QSpinBox* maxTables_ = nullptr;
  QLabel* localStatus_ = nullptr;
  QPushButton* detectButton_ = nullptr;
  QPushButton* testButton_ = nullptr;

  QLineEdit* claudeKey_ = nullptr;
  QComboBox* claudeModel_ = nullptr;
  QLabel* claudeStatus_ = nullptr;

  AiSettings loaded_;
};

}  // namespace ds
