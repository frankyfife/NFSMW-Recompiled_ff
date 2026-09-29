#pragma once

#include <QAbstractButton>
#include <QComboBox>
#include <QFrame>
#include <QPixmap>
#include <QWidget>

class QButtonGroup;
class QGridLayout;
class QLabel;

namespace nfsmw {

// Rounded panel with a letter-spaced title and an accent tick. Rows go into
// grid(): column 0 is the label, column 1 the control.
class Card final : public QFrame {
  Q_OBJECT

 public:
  explicit Card(const QString& title, QWidget* parent = nullptr);

  QGridLayout* grid() const { return grid_; }
  // Adds "label | widget" and returns the row index used.
  int addRow(const QString& label, QWidget* widget);
  int addWide(QWidget* widget);

 private:
  QGridLayout* grid_ = nullptr;
};

// Row of exclusive choices, drawn as one pill with the selection filled.
class Segmented final : public QFrame {
  Q_OBJECT

 public:
  explicit Segmented(const QStringList& items, QWidget* parent = nullptr);

  int currentIndex() const;
  void setCurrentIndex(int index);
  int count() const;

 signals:
  void currentIndexChanged(int index);

 private:
  QButtonGroup* group_ = nullptr;
};

// On/off switch with its caption on the right.
class ToggleSwitch final : public QAbstractButton {
  Q_OBJECT

 public:
  explicit ToggleSwitch(const QString& text, QWidget* parent = nullptr);

  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  bool hitButton(const QPoint& pos) const override;

 private:
  bool hover_ = false;
};

// Combo box with the chevron painted by hand (the style sheet hides the
// native arrow, which Fusion would draw in the wrong color).
class ChevronCombo final : public QComboBox {
  Q_OBJECT

 public:
  explicit ChevronCombo(QWidget* parent = nullptr);

 protected:
  void paintEvent(QPaintEvent* event) override;
};

// Header banner: title, subtitle, status chips, and either the user's cover
// art (portada.jpg next to the exe) or a procedural speed-streak pattern.
class Hero final : public QWidget {
  Q_OBJECT

 public:
  explicit Hero(QWidget* parent = nullptr);

  void setArt(const QPixmap& art);
  void setGameStatus(const QString& text, const QColor& dot);
  void setCacheStatus(const QString& text);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  int drawChip(QPainter& p, int right, int y, const QString& text, const QColor& dot);
  void drawStreaks(QPainter& p);

  QPixmap art_;
  QString gameStatus_;
  QColor gameDot_;
  QString cacheStatus_;
};

}  // namespace nfsmw
