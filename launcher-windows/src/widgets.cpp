#include "widgets.h"

#include "theme.h"

#include <QButtonGroup>
#include <QEnterEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRandomGenerator>
#include <QVBoxLayout>

#include <algorithm>

namespace nfsmw {

// ---------------------------------------------------------------------------
//  Card
// ---------------------------------------------------------------------------
Card::Card(const QString& title, QWidget* parent) : QFrame(parent) {
  setObjectName(QStringLiteral("card"));

  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(18, 14, 18, 18);
  outer->setSpacing(14);

  auto* head = new QHBoxLayout;
  head->setSpacing(8);
  auto* tick = new QFrame;
  tick->setObjectName(QStringLiteral("tick"));
  tick->setFixedSize(3, 12);
  auto* label = new QLabel(title.toUpper());
  label->setObjectName(QStringLiteral("cardTitle"));
  QFont f = label->font();
  f.setPointSizeF(8.0);
  f.setWeight(QFont::DemiBold);
  f.setLetterSpacing(QFont::AbsoluteSpacing, 1.8);
  label->setFont(f);
  head->addWidget(tick);
  head->addWidget(label);
  head->addStretch();
  outer->addLayout(head);

  grid_ = new QGridLayout;
  grid_->setHorizontalSpacing(14);
  grid_->setVerticalSpacing(10);
  grid_->setColumnMinimumWidth(0, 112);
  grid_->setColumnStretch(1, 1);
  outer->addLayout(grid_);
}

int Card::addRow(const QString& label, QWidget* widget) {
  const int row = grid_->rowCount();
  auto* l = new QLabel(label);
  l->setObjectName(QStringLiteral("rowLabel"));
  grid_->addWidget(l, row, 0, Qt::AlignVCenter);
  grid_->addWidget(widget, row, 1);
  return row;
}

int Card::addWide(QWidget* widget) {
  const int row = grid_->rowCount();
  grid_->addWidget(widget, row, 0, 1, 2);
  return row;
}

// ---------------------------------------------------------------------------
//  Segmented
// ---------------------------------------------------------------------------
Segmented::Segmented(const QStringList& items, QWidget* parent) : QFrame(parent) {
  setObjectName(QStringLiteral("segmented"));
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(3, 3, 3, 3);
  layout->setSpacing(3);

  group_ = new QButtonGroup(this);
  group_->setExclusive(true);
  for (int i = 0; i < items.size(); ++i) {
    auto* b = new QPushButton(items[i]);
    b->setObjectName(QStringLiteral("segment"));
    b->setCheckable(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::TabFocus);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(b);
    group_->addButton(b, i);
  }
  if (!items.isEmpty()) {
    group_->button(0)->setChecked(true);
  }
  connect(group_, &QButtonGroup::idToggled, this, [this](int id, bool on) {
    if (on) {
      emit currentIndexChanged(id);
    }
  });
}

int Segmented::currentIndex() const { return group_->checkedId(); }

int Segmented::count() const { return static_cast<int>(group_->buttons().size()); }

void Segmented::setCurrentIndex(int index) {
  index = std::clamp(index, 0, count() - 1);
  if (QAbstractButton* b = group_->button(index)) {
    b->setChecked(true);
  }
}

// ---------------------------------------------------------------------------
//  ToggleSwitch
// ---------------------------------------------------------------------------
ToggleSwitch::ToggleSwitch(const QString& text, QWidget* parent) : QAbstractButton(parent) {
  setText(text);
  setCheckable(true);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::TabFocus);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize ToggleSwitch::sizeHint() const {
  const QFontMetrics fm(font());
  return QSize(40 + 12 + fm.horizontalAdvance(text()), std::max(26, fm.height() + 8));
}

void ToggleSwitch::enterEvent(QEnterEvent* event) {
  hover_ = true;
  update();
  QAbstractButton::enterEvent(event);
}

void ToggleSwitch::leaveEvent(QEvent* event) {
  hover_ = false;
  update();
  QAbstractButton::leaveEvent(event);
}

bool ToggleSwitch::hitButton(const QPoint& pos) const { return rect().contains(pos); }

void ToggleSwitch::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const bool on = isChecked();
  const qreal w = 40, h = 22;
  const qreal y = (height() - h) / 2.0;
  const QRectF pill(0.5, y + 0.5, w - 1, h - 1);

  QColor track = on ? theme::accent() : (hover_ ? theme::fieldHover() : theme::field());
  QColor border = (hasFocus() || on) ? theme::accent() : theme::fieldBorder();
  if (!isEnabled()) {
    track = on ? theme::fieldBorder() : theme::field();
    border = theme::cardBorder();
  }
  p.setPen(QPen(border, 1));
  p.setBrush(track);
  p.drawRoundedRect(pill, h / 2, h / 2);

  const qreal knob = h - 6;
  const qreal kx = on ? w - knob - 3 : 3;
  p.setPen(Qt::NoPen);
  p.setBrush(on ? theme::accentInk() : theme::textDim());
  p.drawEllipse(QRectF(kx, y + 3, knob, knob));

  p.setPen(isEnabled() ? theme::text() : theme::textFaint());
  const QRect tr(int(w) + 12, 0, width() - int(w) - 12, height());
  p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, text());
}

// ---------------------------------------------------------------------------
//  ChevronCombo
// ---------------------------------------------------------------------------
ChevronCombo::ChevronCombo(QWidget* parent) : QComboBox(parent) {
  // A QListView is required for the item rules of the style sheet to apply.
  setView(new QListView(this));
  setCursor(Qt::PointingHandCursor);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void ChevronCombo::paintEvent(QPaintEvent* event) {
  QComboBox::paintEvent(event);
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(isEnabled() ? theme::textDim() : theme::textFaint(), 1.6, Qt::SolidLine,
                Qt::RoundCap, Qt::RoundJoin));
  const QPointF c(width() - 16.0, height() / 2.0);
  const qreal a = 4.0;
  const QPointF pts[3] = {{c.x() - a, c.y() - a / 2}, {c.x(), c.y() + a / 2},
                          {c.x() + a, c.y() - a / 2}};
  p.drawPolyline(pts, 3);
}

// ---------------------------------------------------------------------------
//  Hero
// ---------------------------------------------------------------------------
Hero::Hero(QWidget* parent) : QWidget(parent) {
  setFixedHeight(136);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void Hero::setArt(const QPixmap& art) {
  art_ = art;
  update();
}

void Hero::setGameStatus(const QString& text, const QColor& dot) {
  gameStatus_ = text;
  gameDot_ = dot;
  update();
}

void Hero::setCacheStatus(const QString& text) {
  cacheStatus_ = text;
  update();
}

void Hero::drawStreaks(QPainter& p) {
  QRandomGenerator rnd(454107);  // fixed seed: same picture on every repaint
  for (int i = 0; i < 70; ++i) {
    const qreal y = rnd.generateDouble() * height();
    const qreal len = 60 + rnd.generateDouble() * width() * 0.45;
    const qreal x0 = width() * 0.35 + rnd.generateDouble() * width() * 0.75;
    const qreal thick = 1.0 + rnd.generateDouble() * 2.2;
    const int alpha = 18 + int(rnd.bounded(60));
    QColor c = rnd.generateDouble() > 0.8 ? theme::accent() : QColor(150, 160, 175);
    const QPointF a(x0, y), b(x0 - len, y + len * 0.09);
    QLinearGradient g(a, b);
    c.setAlpha(alpha);
    g.setColorAt(0, c);
    c.setAlpha(0);
    g.setColorAt(1, c);
    p.setPen(QPen(QBrush(g), thick, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(a, b);
  }
}

int Hero::drawChip(QPainter& p, int right, int y, const QString& text, const QColor& dot) {
  if (text.isEmpty()) {
    return right;
  }
  QFont f = font();
  f.setPointSizeF(8.5);
  p.setFont(f);
  const QFontMetrics fm(f);
  const int h = 26;
  const int w = fm.horizontalAdvance(text) + 36;
  const QRectF r(right - w + 0.5, y + 0.5, w - 1, h - 1);
  p.setPen(QPen(theme::cardBorder(), 1));
  p.setBrush(QColor(14, 16, 20, 210));
  p.drawRoundedRect(r, h / 2.0, h / 2.0);
  p.setPen(Qt::NoPen);
  p.setBrush(dot);
  p.drawEllipse(QRectF(r.x() + 11, y + h / 2.0 - 4, 8, 8));
  p.setPen(theme::text());
  p.drawText(QRectF(r.x() + 25, y, w - 30, h), Qt::AlignVCenter | Qt::AlignLeft, text);
  return right - w;
}

void Hero::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::SmoothPixmapTransform);

  QLinearGradient bg(0, 0, width(), 0);
  bg.setColorAt(0, QColor(24, 27, 34));
  bg.setColorAt(1, QColor(8, 9, 11));
  p.fillRect(rect(), bg);

  if (!art_.isNull()) {
    // Right half: the art scaled to cover, faded into the background.
    const int w = std::min(width() / 2, int(art_.width() * (qreal(height()) / art_.height()) * 1.6));
    const QRect dst(width() - w, 0, w, height());
    const QPixmap scaled =
        art_.scaled(dst.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const QRect src((scaled.width() - dst.width()) / 2, int((scaled.height() - dst.height()) * 0.3),
                    dst.width(), dst.height());
    p.drawPixmap(dst, scaled, src);
    QLinearGradient fade(dst.x(), 0, dst.x() + dst.width() / 2.0, 0);
    fade.setColorAt(0, theme::back());
    QColor clear = theme::back();
    clear.setAlpha(0);
    fade.setColorAt(1, clear);
    p.fillRect(QRect(dst.x() - 1, 0, dst.width() / 2 + 2, height()), fade);
  } else {
    drawStreaks(p);
    QLinearGradient shade(0, 0, width() * 2.0 / 3.0, 0);
    QColor s = theme::back();
    s.setAlpha(235);
    shade.setColorAt(0, s);
    s.setAlpha(0);
    shade.setColorAt(1, s);
    p.fillRect(QRect(0, 0, width() * 2 / 3, height()), shade);
  }

  // Accent hairline at the bottom.
  QLinearGradient line(0, 0, width(), 0);
  line.setColorAt(0, theme::accent());
  QColor clear = theme::accent();
  clear.setAlpha(0);
  line.setColorAt(1, clear);
  p.fillRect(QRect(0, height() - 2, width(), 2), line);

  const int x = 28;
  QFont small = font();
  small.setPointSizeF(8.0);
  small.setWeight(QFont::DemiBold);
  small.setLetterSpacing(QFont::AbsoluteSpacing, 3.4);
  p.setFont(small);
  p.setPen(theme::textDim());
  p.drawText(QPoint(x, 36), QStringLiteral("NEED FOR SPEED"));

  QFont big(QStringLiteral("Segoe UI Black"));
  big.setPointSizeF(30);
  big.setItalic(true);
  p.setFont(big);
  const QFontMetrics bm(big);
  const int baseline = 44 + bm.ascent();
  p.setPen(QColor(0, 0, 0, 160));
  p.drawText(QPoint(x - 2, baseline + 3), QStringLiteral("MOST WANTED"));
  p.setPen(theme::text());
  p.drawText(QPoint(x - 4, baseline), QStringLiteral("MOST WANTED"));

  QFont sub = font();
  sub.setPointSizeF(8.5);
  p.setFont(sub);
  p.setPen(theme::textDim());
  p.drawText(QPoint(x, baseline + 26),
             QStringLiteral("Native PC recompilation of the Xbox 360 version  ·  ReXGlue"));

  int right = width() - 24;
  right = drawChip(p, right, 24, gameStatus_, gameDot_);
  drawChip(p, right - 8, 24, cacheStatus_, theme::textDim());
}

}  // namespace nfsmw
