#include "theme.h"

#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>

namespace nfsmw::theme {
namespace {

// Fusion honours style sheets consistently; the native Windows style draws
// parts of combo boxes and scroll bars itself and ignores the colors.
const char* kStyleSheet = R"(
QFrame#card {
  background: #16191f;
  border: 1px solid #252a33;
  border-radius: 10px;
}
QFrame#tick { background: #ff921c; border: none; border-radius: 1px; }
QLabel#cardTitle { color: #8c95a3; }
QLabel#rowLabel { color: #e6e9ee; font-weight: 600; }
QLabel#note { color: #8c95a3; font-size: 8.5pt; }
QLabel#note[state="warn"] { color: #ffb547; }
QLabel#note[state="bad"] { color: #ff5c5c; }
QLabel#note[state="hot"] { color: #ffac48; }
QLabel#status { color: #8c95a3; font-size: 8.5pt; }

QLineEdit, QSpinBox {
  background: #0e1014;
  border: 1px solid #303742;
  border-radius: 6px;
  padding: 5px 9px;
  color: #e6e9ee;
  selection-background-color: #ff921c;
  selection-color: #180e04;
}
QLineEdit:hover, QSpinBox:hover { border-color: #4a5363; }
QLineEdit:focus, QSpinBox:focus { border-color: #ff921c; }
QLineEdit:disabled, QSpinBox:disabled { color: #5c6470; border-color: #252a33; }

QComboBox {
  background: #0e1014;
  border: 1px solid #303742;
  border-radius: 6px;
  padding: 5px 30px 5px 9px;
  color: #e6e9ee;
}
QComboBox:hover { border-color: #4a5363; }
QComboBox:focus, QComboBox:on { border-color: #ff921c; }
QComboBox:disabled { color: #5c6470; border-color: #252a33; }
QComboBox::drop-down { border: none; width: 28px; }
QComboBox::down-arrow { image: none; width: 0; height: 0; }
QComboBox QAbstractItemView {
  background: #16191f;
  border: 1px solid #303742;
  border-radius: 6px;
  padding: 4px;
  outline: 0;
  color: #e6e9ee;
}
QComboBox QAbstractItemView::item { min-height: 26px; padding: 0 8px; border-radius: 4px; }
QComboBox QAbstractItemView::item:selected { background: #46301a; color: #ffac48; }

QPushButton {
  background: #16191f;
  border: 1px solid #303742;
  border-radius: 6px;
  padding: 7px 16px;
  color: #e6e9ee;
  font-weight: 600;
}
QPushButton:hover { background: #1e222a; border-color: #ff921c; }
QPushButton:pressed { background: #0e1014; }
QPushButton:focus { border-color: #ff921c; }
QPushButton:disabled { color: #5c6470; border-color: #252a33; }

QPushButton#play {
  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #ff921c, stop:1 #ffac48);
  color: #180e04;
  border: none;
  border-radius: 8px;
  font-size: 14pt;
  font-weight: 900;
  padding: 10px 44px;
}
QPushButton#play:hover { background: #ffac48; }
QPushButton#play:pressed { background: #ff921c; }
QPushButton#play:disabled { background: #303742; color: #8c95a3; }

QFrame#segmented {
  background: #0e1014;
  border: 1px solid #303742;
  border-radius: 7px;
}
QFrame#segmented:disabled { border-color: #252a33; }
QPushButton#segment {
  background: transparent;
  border: none;
  border-radius: 5px;
  padding: 5px 8px;
  color: #8c95a3;
  font-weight: 400;
}
QPushButton#segment:hover { background: #1e222a; color: #e6e9ee; }
QPushButton#segment:focus { color: #e6e9ee; }
QPushButton#segment:checked { background: #ff921c; color: #180e04; font-weight: 600; }
QPushButton#segment:disabled { color: #5c6470; }
QPushButton#segment:checked:disabled { background: #303742; color: #8c95a3; }

QSlider::groove:horizontal { height: 4px; background: #303742; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #ff921c; border-radius: 2px; }
QSlider::handle:horizontal {
  background: #e6e9ee; width: 16px; height: 16px; margin: -6px 0; border-radius: 8px;
}
QSlider::handle:horizontal:hover { background: #ffffff; }
QSlider::sub-page:horizontal:disabled { background: #5c6470; }
QSlider::handle:horizontal:disabled { background: #5c6470; }

QFrame#footer { background: #0c0e11; border: none; border-top: 1px solid #252a33; }

QScrollArea { background: transparent; border: none; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: #303742; border-radius: 4px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #4a5363; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }

QProgressBar {
  background: #0e1014;
  border: 1px solid #303742;
  border-radius: 6px;
  max-height: 12px;
  color: transparent;
}
QProgressBar::chunk {
  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #ff921c, stop:1 #ffac48);
  border-radius: 5px;
}

QPlainTextEdit {
  background: #0e1014;
  border: 1px solid #303742;
  border-radius: 6px;
  color: #e6e9ee;
  padding: 6px;
}

QToolTip { background: #16191f; color: #e6e9ee; border: 1px solid #303742; padding: 4px; }
QMessageBox QLabel { color: #e6e9ee; }
)";

}  // namespace

void apply(QApplication& app) {
  app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

  QPalette p;
  p.setColor(QPalette::Window, back());
  p.setColor(QPalette::WindowText, text());
  p.setColor(QPalette::Base, field());
  p.setColor(QPalette::AlternateBase, card());
  p.setColor(QPalette::Text, text());
  p.setColor(QPalette::Button, card());
  p.setColor(QPalette::ButtonText, text());
  p.setColor(QPalette::Highlight, accent());
  p.setColor(QPalette::HighlightedText, accentInk());
  p.setColor(QPalette::ToolTipBase, card());
  p.setColor(QPalette::ToolTipText, text());
  p.setColor(QPalette::PlaceholderText, textFaint());
  p.setColor(QPalette::Disabled, QPalette::WindowText, textFaint());
  p.setColor(QPalette::Disabled, QPalette::Text, textFaint());
  p.setColor(QPalette::Disabled, QPalette::ButtonText, textFaint());
  app.setPalette(p);

  QFont f(QStringLiteral("Segoe UI"));
  f.setPointSizeF(9.75);
  app.setFont(f);

  app.setStyleSheet(QString::fromLatin1(kStyleSheet));
}

}  // namespace nfsmw::theme
