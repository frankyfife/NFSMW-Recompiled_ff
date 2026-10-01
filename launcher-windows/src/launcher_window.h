#pragma once

#include <QMainWindow>
#include <QProcess>

class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;

namespace nfsmw {

class ChevronCombo;
class Hero;
class Segmented;
class ToggleSwitch;

class LauncherWindow final : public QMainWindow {
  Q_OBJECT

 public:
  explicit LauncherWindow(QWidget* parent = nullptr);

 private:
  // Paths.
  void locate();
  QString isoCacheDir(const QString& iso) const;


  // UI.
  QWidget* buildContent();
  QWidget* buildAdvanced();
  QWidget* buildFooter();
  void resetAdvanced();
  void refresh();
  void refreshGameStatus();
  void setNote(QLabel* label, const QString& text, const char* state = "");

  // Settings.
  void loadSettings();
  void saveSettings() const;
  void autoDetectGame();

  // Command line.
  int targetFps() const;
  QString outputResolution() const;
  QStringList buildArguments(const QString& gameDir) const;

  // Actions.
  void pickIso();
  void pickFolder();
  void showCommandLine();
  QString resolveGameDir(const QString& input);
  void play();
  void onGameFinished(int exitCode, QProcess::ExitStatus status);
  QString searchLog(const QStringList& needles, bool firstOnly) const;

  QString root_;
  QString gameExe_;
  QString logDir_;
  QString settingsFile_;
  QString runLog_;
  bool loading_ = true;

  Hero* hero_ = nullptr;
  QLineEdit* gamePath_ = nullptr;
  QLabel* gameNote_ = nullptr;

  ChevronCombo* resolution_ = nullptr;
  QSpinBox* width_ = nullptr;
  QSpinBox* height_ = nullptr;
  Segmented* scale_ = nullptr;
  QLabel* scaleNote_ = nullptr;
  Segmented* mode_ = nullptr;
  ChevronCombo* monitor_ = nullptr;

  ChevronCombo* language_ = nullptr;
  ToggleSwitch* blackEdition_ = nullptr;
  ToggleSwitch* unlockAll_ = nullptr;

  Segmented* fps_ = nullptr;
  QSpinBox* customFps_ = nullptr;
  QLabel* fpsNote_ = nullptr;
  ToggleSwitch* vsync_ = nullptr;

  Segmented* aniso_ = nullptr;
  Segmented* filter_ = nullptr;
  QSlider* sharpness_ = nullptr;
  QLabel* sharpnessValue_ = nullptr;


  // Advanced tab.
  ToggleSwitch* pacingAtGuest_ = nullptr;
  ToggleSwitch* lowLatency_ = nullptr;
  ToggleSwitch* adaptivePacing_ = nullptr;
  QSpinBox* smoothMs_ = nullptr;
  Segmented* displayLock_ = nullptr;
  ToggleSwitch* presentPerFrame_ = nullptr;
  ToggleSwitch* logStats_ = nullptr;
  ToggleSwitch* logBreakdown_ = nullptr;

  QPushButton* play_ = nullptr;
  QLabel* status_ = nullptr;
  QProcess* process_ = nullptr;
};

}  // namespace nfsmw
