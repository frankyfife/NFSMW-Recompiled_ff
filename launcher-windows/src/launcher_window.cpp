#include "launcher_window.h"

#include "iso_extractor.h"
#include "theme.h"
#include "widgets.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <thread>

#include <windows.h>

namespace nfsmw {
namespace {

struct Choice {
  const char* label;
  const char* value;
};

constexpr Choice kResolutions[] = {
    {"480p  ·  640 × 480", "480p"},     {"540p  ·  960 × 540", "540p"},
    {"720p  ·  1280 × 720", "720p"},    {"900p  ·  1600 × 900", "900p"},
    {"1080p  ·  1920 × 1080", "1080p"}, {"1440p  ·  2560 × 1440", "1440p"},
    {"1800p  ·  3200 × 1800", "1800p"}, {"2160p  ·  3840 × 2160", "2160p"},
    {"Custom", "custom"},
};

// Xbox XLanguage ids. Only languages present on the disc work; the German PAL
// disc was verified with 3.
constexpr Choice kLanguages[] = {
    {"Auto (from nfsmw.toml)", "0"}, {"English", "1"}, {"German", "3"},
    {"French", "4"},                 {"Spanish", "5"}, {"Italian", "6"},
};

const QList<int> kAnisoValues = {0, 2, 3, 4, 5};  // off, 2x, 4x, 8x, 16x
const QStringList kFilterValues = {"bilinear", "cas", "fsr"};

QLabel* note(const QString& text = QString()) {
  auto* l = new QLabel(text);
  l->setObjectName(QStringLiteral("note"));
  l->setWordWrap(true);
  return l;
}

QSpinBox* spin(int lo, int hi) {
  auto* s = new QSpinBox;
  s->setRange(lo, hi);
  s->setButtonSymbols(QAbstractSpinBox::NoButtons);
  s->setAlignment(Qt::AlignCenter);
  s->setFixedWidth(72);
  return s;
}

QString quoted(const QString& s) {
  return s.contains(QLatin1Char(' ')) ? QLatin1Char('"') + s + QLatin1Char('"') : s;
}

// Counts guest shaders in a .xsh: 8-byte header, then records of
// {u64 hash, u32 dword_count:31 | type:1} followed by the microcode.
int countShaders(const QString& xsh) {
  QFile f(xsh);
  if (!f.open(QIODevice::ReadOnly) || f.size() < 8) {
    return 0;
  }
  const QByteArray b = f.readAll();
  qint64 pos = 8;
  int n = 0;
  while (pos + 12 <= b.size()) {
    quint32 dwords = 0;
    std::memcpy(&dwords, b.constData() + pos + 8, 4);
    dwords &= 0x7FFFFFFFu;
    const qint64 next = pos + 12 + qint64(dwords) * 4;
    if (next > b.size()) {
      break;
    }
    pos = next;
    ++n;
  }
  return n;
}

// ---------------------------------------------------------------------------
//  Extraction progress dialog: the copy runs on its own thread so the window
//  keeps painting while ~7 GB are written.
// ---------------------------------------------------------------------------
class ExtractDialog final : public QDialog {
 public:
  ExtractDialog(const QString& iso, const QString& dest, QWidget* parent)
      : QDialog(parent), iso_(iso), dest_(dest) {
    setWindowTitle(QStringLiteral("Extracting the ISO"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint &
                   ~Qt::WindowCloseButtonHint);
    setModal(true);
    setMinimumWidth(520);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(10);
    auto* title = new QLabel(QStringLiteral("Preparing the game"));
    QFont f = title->font();
    f.setPointSizeF(12);
    f.setWeight(QFont::DemiBold);
    title->setFont(f);
    layout->addWidget(title);
    label_ = note(QStringLiteral("Extracting %1. Only needed once per ISO; this can take a few "
                                 "minutes.")
                      .arg(QFileInfo(iso).fileName()));
    layout->addWidget(label_);
    bar_ = new QProgressBar;
    bar_->setRange(0, 1000);
    bar_->setTextVisible(false);
    layout->addWidget(bar_);
    auto* row = new QHBoxLayout;
    row->addStretch();
    cancel_ = new QPushButton(QStringLiteral("Cancel"));
    row->addWidget(cancel_);
    layout->addLayout(row);
    connect(cancel_, &QPushButton::clicked, this, [this] {
      cancelled_ = true;
      cancel_->setEnabled(false);
      label_->setText(QStringLiteral("Cancelling…"));
    });
  }

  ~ExtractDialog() override {
    cancelled_ = true;
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  int exec() override {
    worker_ = std::thread([this] {
      QDir(dest_).removeRecursively();
      QDir().mkpath(dest_);
      QString error;
      const iso::Result r = iso::extract(
          iso_, dest_,
          [this](int done, int total, const QString& file) {
            QMetaObject::invokeMethod(this, [this, done, total, file] {
              bar_->setValue(total > 0 ? done * 1000 / total : 0);
              if (!cancelled_) {
                label_->setText(QStringLiteral("%1 / %2  ·  %3").arg(done).arg(total).arg(file));
              }
            });
          },
          [this] { return cancelled_.load(); }, &error);
      if (r == iso::Result::kOk) {
        iso::writeMarker(dest_, iso_);
      }
      QMetaObject::invokeMethod(this, [this, r, error] {
        error_ = error;
        if (r == iso::Result::kOk) {
          accept();
        } else {
          reject();
        }
      });
    });
    return QDialog::exec();
  }

  QString error() const { return error_; }

 private:
  QString iso_, dest_, error_;
  QLabel* label_ = nullptr;
  QProgressBar* bar_ = nullptr;
  QPushButton* cancel_ = nullptr;
  std::atomic<bool> cancelled_{false};
  std::thread worker_;
};

}  // namespace

// ===========================================================================
//  Construction
// ===========================================================================
LauncherWindow::LauncherWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("Need for Speed: Most Wanted — Recompiled"));
  locate();

  auto* central = new QWidget;
  auto* v = new QVBoxLayout(central);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);

  hero_ = new Hero;
  QPixmap art(QDir(root_).filePath(QStringLiteral("portada.jpg")));
  if (!art.isNull()) {
    hero_->setArt(art);
  }
  v->addWidget(hero_);

  // Two pages: the everyday settings and the advanced ones (latency, texture
  // cache, diagnostics), switched with the same pill control as the options.
  auto* tabBar = new QWidget;
  auto* tb = new QHBoxLayout(tabBar);
  tb->setContentsMargins(20, 14, 20, 0);
  auto* tabs = new Segmented({QStringLiteral("General"), QStringLiteral("Advanced")});
  tabs->setCurrentIndex(0);
  tb->addWidget(tabs);
  tb->addStretch();
  v->addWidget(tabBar);

  const auto page = [](QWidget* content) {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    return scroll;
  };
  auto* pages = new QStackedWidget;
  pages->addWidget(page(buildContent()));
  pages->addWidget(page(buildAdvanced()));
  connect(tabs, &Segmented::currentIndexChanged, pages, &QStackedWidget::setCurrentIndex);
  v->addWidget(pages, 1);
  v->addWidget(buildFooter());
  setCentralWidget(central);

  loadSettings();
  loading_ = false;
  autoDetectGame();
  refresh();

  // Fit the screen: content scrolls when the window has to be shorter.
  const QSize want(1040, 960);
  const QRect avail = screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 720);
  resize(std::min(want.width(), avail.width() - 40), std::min(want.height(), avail.height() - 60));
}

// The launcher sits either in the portable folder next to nfsmw.exe, or in
// the source tree (launcher-windows/out), where the game is looked up in
// build\ or app\out\build\win-amd64-release\.
void LauncherWindow::locate() {
  const QString me = QCoreApplication::applicationDirPath();
  QDir d(me);
  root_ = me;
  gameExe_ = d.filePath(QStringLiteral("nfsmw.exe"));
  if (!QFileInfo::exists(gameExe_)) {
    for (int up = 0; up < 5 && d.cdUp(); ++up) {
      const QString portable = d.filePath(QStringLiteral("build/nfsmw.exe"));
      const QString dev = d.filePath(QStringLiteral("app/out/build/win-amd64-release/nfsmw.exe"));
      if (QFileInfo::exists(portable)) {
        root_ = d.filePath(QStringLiteral("build"));
        gameExe_ = portable;
        break;
      }
      if (QFileInfo::exists(dev)) {
        root_ = d.absolutePath();
        gameExe_ = dev;
        break;
      }
    }
  }
  root_ = QDir::cleanPath(root_);
  logDir_ = QDir(root_).filePath(QStringLiteral("logs"));
  settingsFile_ = QDir(root_).filePath(QStringLiteral("launcher.ini"));
  runLog_ = QDir(logDir_).filePath(QStringLiteral("launcher.log"));
}

QString LauncherWindow::isoCacheDir(const QString& iso) const {
  return QDir(root_).filePath(QStringLiteral("game_root_cache/") +
                              iso::safeName(QFileInfo(iso).completeBaseName()));
}

QWidget* LauncherWindow::buildContent() {
  auto* content = new QWidget;
  auto* cols = new QHBoxLayout(content);
  cols->setContentsMargins(20, 18, 20, 18);
  cols->setSpacing(16);
  auto* left = new QVBoxLayout;
  auto* right = new QVBoxLayout;
  left->setSpacing(16);
  right->setSpacing(16);
  cols->addLayout(left, 1);
  cols->addLayout(right, 1);

  const auto onChange = [this] { refresh(); };

  // ---- Game data ----
  auto* game = new Card(QStringLiteral("Game data"));
  gamePath_ = new QLineEdit;
  gamePath_->setPlaceholderText(QStringLiteral("ISO file or extracted folder"));
  game->addWide(gamePath_);
  auto* gameRow = new QWidget;
  auto* gr = new QHBoxLayout(gameRow);
  gr->setContentsMargins(0, 0, 0, 0);
  gr->setSpacing(8);
  gameNote_ = note();
  gr->addWidget(gameNote_, 1);
  auto* pickIsoButton = new QPushButton(QStringLiteral("ISO…"));
  auto* pickDirButton = new QPushButton(QStringLiteral("Folder…"));
  gr->addWidget(pickIsoButton);
  gr->addWidget(pickDirButton);
  game->addWide(gameRow);
  connect(gamePath_, &QLineEdit::textChanged, this, onChange);
  connect(pickIsoButton, &QPushButton::clicked, this, &LauncherWindow::pickIso);
  connect(pickDirButton, &QPushButton::clicked, this, &LauncherWindow::pickFolder);
  left->addWidget(game);

  // ---- Display ----
  auto* display = new Card(QStringLiteral("Display"));
  auto* resRow = new QWidget;
  auto* rr = new QHBoxLayout(resRow);
  rr->setContentsMargins(0, 0, 0, 0);
  rr->setSpacing(8);
  resolution_ = new ChevronCombo;
  for (const Choice& c : kResolutions) resolution_->addItem(QString::fromUtf8(c.label));
  width_ = spin(320, 7680);
  height_ = spin(240, 4320);
  auto* times = new QLabel(QStringLiteral("×"));
  rr->addWidget(resolution_, 1);
  rr->addWidget(width_);
  rr->addWidget(times);
  rr->addWidget(height_);
  display->addRow(QStringLiteral("Resolution"), resRow);
  scale_ = new Segmented({QStringLiteral("1×"), QStringLiteral("2×"),
                          QStringLiteral("3×"), QStringLiteral("4×")});
  display->addRow(QStringLiteral("Internal scale"), scale_);
  scaleNote_ = note();
  display->grid()->addWidget(scaleNote_, display->grid()->rowCount(), 1);
  mode_ = new Segmented({QStringLiteral("Fullscreen"), QStringLiteral("Windowed")});
  display->addRow(QStringLiteral("Mode"), mode_);
  monitor_ = new ChevronCombo;
  monitor_->addItem(QStringLiteral("Automatic"));
  const QList<QScreen*> screens = QGuiApplication::screens();
  for (int i = 0; i < screens.size(); ++i) {
    const QSize px = screens[i]->size() * screens[i]->devicePixelRatio();
    monitor_->addItem(QStringLiteral("Monitor %1  ·  %2 × %3%4")
                          .arg(i + 1)
                          .arg(px.width())
                          .arg(px.height())
                          .arg(screens[i] == QGuiApplication::primaryScreen()
                                   ? QStringLiteral("  (primary)")
                                   : QString()));
  }
  display->addRow(QStringLiteral("Monitor"), monitor_);
  connect(resolution_, &QComboBox::currentIndexChanged, this, onChange);
  connect(width_, &QSpinBox::valueChanged, this, onChange);
  connect(height_, &QSpinBox::valueChanged, this, onChange);
  connect(scale_, &Segmented::currentIndexChanged, this, onChange);
  connect(mode_, &Segmented::currentIndexChanged, this, onChange);
  connect(monitor_, &QComboBox::currentIndexChanged, this, onChange);
  left->addWidget(display);

  // ---- Game ----
  auto* gameOpts = new Card(QStringLiteral("Game"));
  language_ = new ChevronCombo;
  for (const Choice& c : kLanguages) language_->addItem(QString::fromUtf8(c.label));
  gameOpts->addRow(QStringLiteral("Language"), language_);
  blackEdition_ = new ToggleSwitch(QStringLiteral("Black Edition cars in the car lot"));
  gameOpts->addWide(blackEdition_);
  unlockAll_ = new ToggleSwitch(QStringLiteral("Unlock everything (cars, parts, events)"));
  gameOpts->addWide(unlockAll_);
  gameOpts->grid()->addWidget(
      note(QStringLiteral("Both can also be switched live in the in-game ESC menu. "
                          "Unlocking does not touch your save: switch it off and "
                          "your normal progress is back.")),
      gameOpts->grid()->rowCount(), 0, 1, 2);
  connect(language_, &QComboBox::currentIndexChanged, this, onChange);
  connect(blackEdition_, &ToggleSwitch::toggled, this, onChange);
  connect(unlockAll_, &ToggleSwitch::toggled, this, onChange);
  left->addWidget(gameOpts);
  left->addStretch();

  // ---- Frame rate ----
  auto* frame = new Card(QStringLiteral("Frame rate"));
  fps_ = new Segmented({QStringLiteral("30 · Original"), QStringLiteral("60"),
                        QStringLiteral("Unlimited"), QStringLiteral("Custom")});
  frame->addRow(QStringLiteral("Target"), fps_);
  auto* customRow = new QWidget;
  auto* cr = new QHBoxLayout(customRow);
  cr->setContentsMargins(0, 0, 0, 0);
  cr->setSpacing(10);
  customFps_ = spin(10, 240);
  fpsNote_ = note();
  cr->addWidget(customFps_);
  cr->addWidget(fpsNote_, 1);
  frame->addRow(QStringLiteral("Custom fps"), customRow);
  vsync_ = new ToggleSwitch(
      QStringLiteral("V-Sync (leave off with G-Sync/FreeSync)"));
  frame->addWide(vsync_);
  connect(fps_, &Segmented::currentIndexChanged, this, onChange);
  connect(customFps_, &QSpinBox::valueChanged, this, onChange);
  connect(vsync_, &ToggleSwitch::toggled, this, onChange);
  right->addWidget(frame);

  // ---- Image quality ----
  auto* image = new Card(QStringLiteral("Image quality"));
  aniso_ = new Segmented({QStringLiteral("Off"), QStringLiteral("2×"), QStringLiteral("4×"),
                          QStringLiteral("8×"), QStringLiteral("16×")});
  image->addRow(QStringLiteral("Anisotropic"), aniso_);
  filter_ = new Segmented({QStringLiteral("Bilinear"), QStringLiteral("CAS"), QStringLiteral("FSR")});
  image->addRow(QStringLiteral("Output filter"), filter_);
  auto* sharpRow = new QWidget;
  auto* sr = new QHBoxLayout(sharpRow);
  sr->setContentsMargins(0, 0, 0, 0);
  sr->setSpacing(10);
  sharpness_ = new QSlider(Qt::Horizontal);
  sharpness_->setRange(0, 100);
  sharpness_->setCursor(Qt::PointingHandCursor);
  sharpnessValue_ = new QLabel;
  sharpnessValue_->setFixedWidth(40);
  sharpnessValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  sr->addWidget(sharpness_, 1);
  sr->addWidget(sharpnessValue_);
  image->addRow(QStringLiteral("Sharpness"), sharpRow);
  connect(aniso_, &Segmented::currentIndexChanged, this, onChange);
  connect(filter_, &Segmented::currentIndexChanged, this, onChange);
  connect(sharpness_, &QSlider::valueChanged, this, onChange);
  right->addWidget(image);

  right->addStretch();

  return content;
}

// Advanced tab: the switches added while hunting latency and stutter. The
// defaults are the measured best; "Reset" puts them back.
QWidget* LauncherWindow::buildAdvanced() {
  auto* content = new QWidget;
  auto* cols = new QHBoxLayout(content);
  cols->setContentsMargins(20, 18, 20, 18);
  cols->setSpacing(16);
  auto* left = new QVBoxLayout;
  auto* right = new QVBoxLayout;
  left->setSpacing(16);
  right->setSpacing(16);
  cols->addLayout(left, 1);
  cols->addLayout(right, 1);

  const auto onChange = [this] { refresh(); };
  const auto addNote = [](Card* card, const QString& text) {
    card->grid()->addWidget(note(text), card->grid()->rowCount(), 0, 1, 2);
  };

  // ---- Latency & frame pacing ----
  auto* latency = new Card(QStringLiteral("Latency & frame pacing"));
  pacingAtGuest_ = new ToggleSwitch(QStringLiteral("Pace frames in the game thread"));
  latency->addWide(pacingAtGuest_);
  addNote(latency, QStringLiteral(
                       "The game waits for its turn when it hands over a frame, as on the "
                       "console, so it cannot queue frames ahead. Measured 50 → 19 ms from "
                       "frame to screen at 60 fps."));
  lowLatency_ = new ToggleSwitch(QStringLiteral("Low-latency mode (like NVIDIA Reflex)"));
  latency->addWide(lowLatency_);
  addNote(latency, QStringLiteral(
                       "The game starts the next frame, and reads the controller, only once "
                       "the previous one is out. Helps most at high frame rates in busy "
                       "scenes."));
  adaptivePacing_ = new ToggleSwitch(QStringLiteral("Adaptive pacing"));
  latency->addWide(adaptivePacing_);
  addNote(latency, QStringLiteral(
                       "When a scene cannot hold the target (measured: 75-117 fps at a "
                       "120 fps target in the city), pace at what it holds evenly instead of "
                       "alternating fast and slow frames."));
  connect(adaptivePacing_, &ToggleSwitch::toggled, this, onChange);
  smoothMs_ = spin(0, 16);
  auto* smoothRow = new QWidget;
  auto* smh = new QHBoxLayout(smoothRow);
  smh->setContentsMargins(0, 0, 0, 0);
  smh->addWidget(smoothMs_);
  smh->addStretch(1);
  latency->addRow(QStringLiteral("Smoothing (ms)"), smoothRow);
  addNote(latency, QStringLiteral(
                       "Every frame is shown the same time after the game releases it, for "
                       "even frame times. Uses the slowest recent frame, up to this limit. "
                       "0 = show as soon as possible."));
  displayLock_ = new Segmented(
      {QStringLiteral("Never"), QStringLiteral("With V-Sync"), QStringLiteral("Always")});
  latency->addRow(QStringLiteral("Lock to display"), displayLock_);
  addNote(latency, QStringLiteral(
                       "Aligns the frame clock with the monitor's real refresh. Needed with "
                       "V-Sync; with G-Sync/FreeSync it causes jitter."));
  presentPerFrame_ = new ToggleSwitch(QStringLiteral("One present per game frame"));
  latency->addWide(presentPerFrame_);
  addNote(latency, QStringLiteral(
                       "Without it the overlay repaints on every monitor refresh: up to 180 "
                       "presents for 60 frames, which knocks G-Sync/FreeSync out of range."));
  connect(pacingAtGuest_, &ToggleSwitch::toggled, this, onChange);
  connect(lowLatency_, &ToggleSwitch::toggled, this, onChange);
  connect(smoothMs_, &QSpinBox::valueChanged, this, onChange);
  connect(displayLock_, &Segmented::currentIndexChanged, this, onChange);
  connect(presentPerFrame_, &ToggleSwitch::toggled, this, onChange);
  left->addWidget(latency);
  left->addStretch();


  // ---- Diagnostics ----
  auto* diagnostics = new Card(QStringLiteral("Diagnostics"));
  logStats_ = new ToggleSwitch(QStringLiteral("Performance statistics in the log"));
  diagnostics->addWide(logStats_);
  addNote(diagnostics, QStringLiteral(
                           "Every 10 s: fps, frame times, latency, texture cache and a line "
                           "for every late frame. Costs next to nothing."));
  logBreakdown_ = new ToggleSwitch(QStringLiteral("Break slow frames down by stage"));
  diagnostics->addWide(logBreakdown_);
  addNote(diagnostics, QStringLiteral(
                           "Shaders, textures, render targets, GPU waits… Costs 2-3 ms per "
                           "frame in busy scenes, so only for hunting a problem."));
  auto* reset = new QPushButton(QStringLiteral("Reset advanced settings"));
  diagnostics->addWide(reset);
  connect(logStats_, &ToggleSwitch::toggled, this, onChange);
  connect(logBreakdown_, &ToggleSwitch::toggled, this, onChange);
  connect(reset, &QPushButton::clicked, this, &LauncherWindow::resetAdvanced);
  right->addWidget(diagnostics);
  right->addStretch();

  return content;
}

void LauncherWindow::resetAdvanced() {
  pacingAtGuest_->setChecked(true);
  lowLatency_->setChecked(true);
  adaptivePacing_->setChecked(true);
  smoothMs_->setValue(6);
  displayLock_->setCurrentIndex(1);
  presentPerFrame_->setChecked(true);
  logStats_->setChecked(true);
  logBreakdown_->setChecked(false);
  refresh();
}

QWidget* LauncherWindow::buildFooter() {
  auto* footer = new QFrame;
  footer->setObjectName(QStringLiteral("footer"));
  auto* h = new QHBoxLayout(footer);
  h->setContentsMargins(20, 14, 20, 14);
  h->setSpacing(12);

  auto* cmd = new QPushButton(QStringLiteral("Command line…"));
  h->addWidget(cmd);
  status_ = new QLabel;
  status_->setObjectName(QStringLiteral("status"));
  status_->setWordWrap(true);
  h->addWidget(status_, 1);
  auto* quit = new QPushButton(QStringLiteral("Quit"));
  quit->setMinimumHeight(46);
  quit->setMinimumWidth(96);
  h->addWidget(quit);
  play_ = new QPushButton(QStringLiteral("PLAY"));
  play_->setObjectName(QStringLiteral("play"));
  play_->setCursor(Qt::PointingHandCursor);
  play_->setMinimumHeight(46);
  play_->setMinimumWidth(190);
  play_->setDefault(true);
  h->addWidget(play_);

  connect(cmd, &QPushButton::clicked, this, &LauncherWindow::showCommandLine);
  connect(quit, &QPushButton::clicked, this, &QWidget::close);
  connect(play_, &QPushButton::clicked, this, &LauncherWindow::play);
  return footer;
}

// ===========================================================================
//  Settings (launcher.ini next to the game)
// ===========================================================================
void LauncherWindow::loadSettings() {
  QSettings s(settingsFile_, QSettings::IniFormat);
  gamePath_->setText(s.value("game/path").toString());

  const QString res = s.value("display/resolution", "1080p").toString();
  resolution_->setCurrentIndex(4);
  for (int i = 0; i < int(std::size(kResolutions)); ++i) {
    if (res == QLatin1String(kResolutions[i].value)) resolution_->setCurrentIndex(i);
  }
  width_->setValue(s.value("display/width", 1920).toInt());
  height_->setValue(s.value("display/height", 1080).toInt());
  scale_->setCurrentIndex(s.value("display/scale", 2).toInt() - 1);
  mode_->setCurrentIndex(s.value("display/fullscreen", true).toBool() ? 0 : 1);
  const int mon = s.value("display/monitor", 0).toInt();
  monitor_->setCurrentIndex(mon >= 0 && mon < monitor_->count() ? mon : 0);

  const QString lang = s.value("game/language", "0").toString();
  language_->setCurrentIndex(0);
  for (int i = 0; i < int(std::size(kLanguages)); ++i) {
    if (lang == QLatin1String(kLanguages[i].value)) language_->setCurrentIndex(i);
  }
  blackEdition_->setChecked(s.value("game/black_edition", true).toBool());
  unlockAll_->setChecked(s.value("game/unlock_all", false).toBool());

  const QString fps = s.value("frame/mode", "60").toString();
  fps_->setCurrentIndex(fps == "60" ? 1 : fps == "unlimited" ? 2 : fps == "custom" ? 3 : 0);
  customFps_->setValue(s.value("frame/fps", 60).toInt());
  // v2: V-Sync off by default -it measured worse- and the old key is ignored.
  vsync_->setChecked(s.value("frame/vsync2", false).toBool());

  const qsizetype an = kAnisoValues.indexOf(s.value("image/anisotropic", 4).toInt());
  aniso_->setCurrentIndex(an >= 0 ? int(an) : 3);
  filter_->setCurrentIndex(
      std::max<qsizetype>(0, kFilterValues.indexOf(s.value("image/filter", "bilinear").toString())));
  sharpness_->setValue(s.value("image/sharpness", 50).toInt());


  pacingAtGuest_->setChecked(s.value("advanced/pacing_at_guest", true).toBool());
  lowLatency_->setChecked(s.value("advanced/low_latency", true).toBool());
  adaptivePacing_->setChecked(s.value("advanced/adaptive_pacing", true).toBool());
  smoothMs_->setValue(s.value("advanced/smooth_ms", 6).toInt());
  displayLock_->setCurrentIndex(std::clamp(s.value("advanced/display_lock", 1).toInt(), 0, 2));
  presentPerFrame_->setChecked(s.value("advanced/present_per_frame", true).toBool());
  logStats_->setChecked(s.value("advanced/log_stats", true).toBool());
  logBreakdown_->setChecked(s.value("advanced/log_breakdown", false).toBool());
}

void LauncherWindow::saveSettings() const {
  QSettings s(settingsFile_, QSettings::IniFormat);
  s.setValue("game/path", gamePath_->text().trimmed());
  s.setValue("display/resolution", QString::fromLatin1(kResolutions[resolution_->currentIndex()].value));
  s.setValue("display/width", width_->value());
  s.setValue("display/height", height_->value());
  s.setValue("display/scale", scale_->currentIndex() + 1);
  s.setValue("display/fullscreen", mode_->currentIndex() == 0);
  s.setValue("display/monitor", monitor_->currentIndex());
  s.setValue("game/language", QString::fromLatin1(kLanguages[language_->currentIndex()].value));
  s.setValue("game/black_edition", blackEdition_->isChecked());
  s.setValue("game/unlock_all", unlockAll_->isChecked());
  static const char* const kFpsModes[] = {"30", "60", "unlimited", "custom"};
  s.setValue("frame/mode", kFpsModes[std::clamp(fps_->currentIndex(), 0, 3)]);
  s.setValue("frame/fps", customFps_->value());
  s.setValue("frame/vsync2", vsync_->isChecked());
  s.setValue("image/anisotropic", kAnisoValues[aniso_->currentIndex()]);
  s.setValue("image/filter", kFilterValues[filter_->currentIndex()]);
  s.setValue("image/sharpness", sharpness_->value());
  s.setValue("advanced/pacing_at_guest", pacingAtGuest_->isChecked());
  s.setValue("advanced/low_latency", lowLatency_->isChecked());
  s.setValue("advanced/adaptive_pacing", adaptivePacing_->isChecked());
  s.setValue("advanced/smooth_ms", smoothMs_->value());
  s.setValue("advanced/display_lock", displayLock_->currentIndex());
  s.setValue("advanced/present_per_frame", presentPerFrame_->isChecked());
  s.setValue("advanced/log_stats", logStats_->isChecked());
  s.setValue("advanced/log_breakdown", logBreakdown_->isChecked());
  s.sync();
}

void LauncherWindow::autoDetectGame() {
  if (!gamePath_->text().trimmed().isEmpty()) {
    return;
  }
  const QDir d(root_);
  if (QFileInfo::exists(d.filePath("game_root/default.xex"))) {
    gamePath_->setText(QDir::toNativeSeparators(d.filePath("game_root")));
    return;
  }
  const QStringList isos = d.entryList({QStringLiteral("*.iso")}, QDir::Files, QDir::Name);
  if (!isos.isEmpty()) {
    gamePath_->setText(QDir::toNativeSeparators(d.filePath(isos.first())));
  }
}

// ===========================================================================
//  Command line
// ===========================================================================

// The game presents every second vblank (30 fps on a 60 Hz console). The
// guest vblank rate -guest_vblank_rate, the same idea as Xenia Canary's
// framerate_limit- sets the pace: 120 Hz gives 60 fps, throttled exactly like
// on the console, while vsync keeps presentation in step with the monitor.
// 0 = unlimited.
int LauncherWindow::targetFps() const {
  switch (fps_->currentIndex()) {
    case 1: return 60;
    case 2: return 0;
    case 3: return customFps_->value();
    default: return 30;
  }
}

QString LauncherWindow::outputResolution() const {
  const int i = std::max(0, resolution_->currentIndex());
  if (QLatin1String(kResolutions[i].value) == QLatin1String("custom")) {
    return QStringLiteral("%1x%2").arg(width_->value()).arg(height_->value());
  }
  return QString::fromLatin1(kResolutions[i].value);
}

// Every value goes as --name=value. MEASURED: with "--name value" the cvars
// that live in the GPU plugin DLL (frame_pacing_fps, guest_vblank_rate) were
// silently left at their defaults.
QStringList LauncherWindow::buildArguments(const QString& gameDir) const {
  const auto opt = [](const char* name, const QString& value) {
    return QStringLiteral("--%1=%2").arg(QLatin1String(name), value);
  };
  const auto flag = [](const char* name, bool on) {
    return QStringLiteral("--%1=%2").arg(QLatin1String(name), on ? "true" : "false");
  };

  QStringList a;
  a << opt("log_level", "info");
  a << opt("log_file", QDir::toNativeSeparators(runLog_));
  a << opt("game_data_root", QDir::toNativeSeparators(gameDir));
  a << opt("gpu_plugin", "xenos");
  a << flag("mnk_mode", true);
  a << opt("readback_resolve", "fast");  // without it the image comes out washed out

  // The game's frames are drawn by its own Direct3D 12 renderer
  // (docs/NATIVE_RENDERER.md); the emulated draws are skipped and its picture
  // reaches the window through the D3D12 swap. Always passed: the command line
  // wins over nfsmw.toml (an API picked in the F4 menu cannot lock it out).
  a << opt("gpu_backend", "d3d12");
  a << flag("native_renderer", true);

  a << opt("resolution", outputResolution());
  // The internal scale is the native renderer's: it draws at that multiple of
  // the game's 1280x720 (supersampling). The emulation draws nothing, so its
  // own render targets stay at 1x.
  a << opt("resolution_scale", "1");
  a << opt("native_renderer_scale", QString::number(scale_->currentIndex() + 1));
  a << flag("fullscreen", mode_->currentIndex() == 0);
  a << opt("monitor", QString::number(std::max(0, monitor_->currentIndex())));

  // Frame pacing. At the console's 60 Hz vblank the game schedules each flip
  // into a vblank slot and a frame that is a hair late loses the whole slot
  // (flips every 16/33/50 ms instead of 33). So the game gets a vblank every
  // millisecond -it never misses a slot- and the pace is set by
  // frame_pacing_fps, a precise clock in the GPU thread: measured 33.1-33.5 ms
  // per frame at 30 and 16.4-16.9 ms at 60. V-Sync only decides whether the
  // host waits for the display; it adds a second clock and some judder, so
  // it is off by default (G-Sync/FreeSync removes the tearing).
  a << flag("vsync", vsync_->isChecked());
  a << opt("guest_vblank_rate", "1000");
  a << opt("frame_pacing_fps", QString::number(targetFps()));
  a << opt("max_fps", "0");

  a << opt("swap_post_effect", "none");
  a << opt("anisotropic_override", QString::number(kAnisoValues[aniso_->currentIndex()]));
  a << opt("native_renderer_anisotropic", QString::number(kAnisoValues[aniso_->currentIndex()]));
  // Advanced tab (see buildAdvanced for what each one does).
  a << flag("frame_pacing_at_guest", pacingAtGuest_->isChecked());
  a << flag("frame_pacing_low_latency", lowLatency_->isChecked());
  a << flag("frame_pacing_adaptive", adaptivePacing_->isChecked());
  a << opt("frame_pacing_smooth_max_ms", QString::number(smoothMs_->value()));
  a << opt("frame_pacing_display_lock", QString::number(displayLock_->currentIndex()));
  a << flag("present_ui_with_guest_frames", presentPerFrame_->isChecked());
  if (filter_->currentIndex() != 0) {
    a << opt("present_effect", kFilterValues[filter_->currentIndex()]);
  }
  a << opt("present_cas_additional_sharpness", QString::number(sharpness_->value() / 100.0, 'f', 2));

  const QString lang = QString::fromLatin1(kLanguages[language_->currentIndex()].value);
  if (lang != QLatin1String("0")) {
    a << opt("user_language", lang);
  }
  a << QStringLiteral("--black_edition=%1").arg(blackEdition_->isChecked() ? "true" : "false");
  a << QStringLiteral("--unlock_all=%1").arg(unlockAll_->isChecked() ? "true" : "false");
  // Every 10 s: frames the game really presents, latency, texture cache.
  a << flag("log_guest_fps", logStats_->isChecked());
  a << flag("log_frame_breakdown", logBreakdown_->isChecked());
  // F10 in the game records frame times into this folder (one CSV each).
  a << opt("frame_times_dir", QDir::toNativeSeparators(QDir(logDir_).filePath("frametimes")));
  return a;
}

// ===========================================================================
//  State
// ===========================================================================
void LauncherWindow::setNote(QLabel* label, const QString& text, const char* state) {
  label->setText(text);
  if (label->property("state").toString() != QLatin1String(state)) {
    label->setProperty("state", QString::fromLatin1(state));
    label->style()->unpolish(label);
    label->style()->polish(label);
  }
}

void LauncherWindow::refresh() {
  if (loading_) {
    return;
  }
  const bool custom = QLatin1String(kResolutions[resolution_->currentIndex()].value) ==
                      QLatin1String("custom");
  width_->setEnabled(custom);
  height_->setEnabled(custom);

  const int scale = scale_->currentIndex() + 1;
  if (scale == 1) {
    setNote(scaleNote_, QStringLiteral("Xbox 360 rendering resolution, 1280 × 720."));
  } else {
    setNote(scaleNote_,
            QStringLiteral("Drawn at %1 × %2 (%3× the pixels): sharper, much smoother edges "
                           "(supersampling on top of the game's 4× MSAA).")
                .arg(1280 * scale)
                .arg(720 * scale)
                .arg(scale * scale),
            "hot");
  }

  customFps_->setEnabled(fps_->currentIndex() == 3);
  const int fps = targetFps();
  if (fps == 0) {
    setNote(fpsNote_, QStringLiteral("As fast as the PC allows. Physics run on real time, so "
                                     "game speed is unaffected."),
            "warn");
  } else {
    setNote(fpsNote_, QStringLiteral("Evenly paced at %1 fps. Physics run on real time, so game "
                                     "speed is the same at any frame rate.")
                          .arg(fps));
  }

  const bool sharpen = filter_->currentIndex() != 0;
  sharpness_->setEnabled(sharpen);
  sharpnessValue_->setEnabled(sharpen);
  sharpnessValue_->setText(QStringLiteral("%1%").arg(sharpness_->value()));

  refreshGameStatus();

  if (!QFileInfo::exists(gameExe_)) {
    status_->setText(QStringLiteral("nfsmw.exe was not found next to the launcher."));
    play_->setEnabled(false);
  } else if (!process_) {
    play_->setEnabled(true);
  }
}

void LauncherWindow::refreshGameStatus() {
  const QString g = gamePath_->text().trimmed();
  const QFileInfo fi(g);
  if (g.isEmpty()) {
    setNote(gameNote_, QStringLiteral("Pick your ISO or an extracted folder."), "warn");
    hero_->setGameStatus(QStringLiteral("No game selected"), theme::warn());
  } else if (fi.isDir() && QFileInfo::exists(QDir(g).filePath("default.xex"))) {
    setNote(gameNote_, QStringLiteral("Extracted folder · ready."));
    hero_->setGameStatus(QStringLiteral("Game ready"), theme::good());
  } else if (fi.isFile() && g.endsWith(QLatin1String(".iso"), Qt::CaseInsensitive)) {
    const bool ready = iso::cacheIsValid(isoCacheDir(g), g);
    setNote(gameNote_, ready ? QStringLiteral("ISO · already extracted, starts right away.")
                             : QStringLiteral("ISO · extracted once on the first PLAY (~7 GB)."));
    hero_->setGameStatus(ready ? QStringLiteral("Game ready") : QStringLiteral("ISO found"),
                         ready ? theme::good() : theme::warn());
  } else {
    setNote(gameNote_, QStringLiteral("Not found, or not an ISO / folder with default.xex."), "bad");
    hero_->setGameStatus(QStringLiteral("Game not found"), theme::bad());
  }
}

// ===========================================================================
//  Actions
// ===========================================================================
void LauncherWindow::pickIso() {
  const QString cur = gamePath_->text().trimmed();
  const QString start = QFileInfo(cur).isFile() ? QFileInfo(cur).absolutePath() : root_;
  const QString f = QFileDialog::getOpenFileName(
      this, QStringLiteral("Select your Need for Speed: Most Wanted ISO"), start,
      QStringLiteral("Xbox 360 disc image (*.iso);;All files (*.*)"));
  if (!f.isEmpty()) {
    gamePath_->setText(QDir::toNativeSeparators(f));
  }
}

void LauncherWindow::pickFolder() {
  const QString cur = gamePath_->text().trimmed();
  const QString d = QFileDialog::getExistingDirectory(
      this, QStringLiteral("Select the extracted game folder (it must contain default.xex)"),
      QFileInfo(cur).isDir() ? cur : root_);
  if (!d.isEmpty()) {
    gamePath_->setText(QDir::toNativeSeparators(d));
  }
}

void LauncherWindow::showCommandLine() {
  const QString g = gamePath_->text().trimmed();
  const QString dir = g.endsWith(QLatin1String(".iso"), Qt::CaseInsensitive) ? isoCacheDir(g) : g;
  QStringList parts{quoted(QDir::toNativeSeparators(gameExe_))};
  for (const QString& a : buildArguments(dir.isEmpty() ? QStringLiteral("(no game selected)") : dir)) {
    parts << quoted(a);
  }
  const QString cmd = parts.join(QLatin1Char(' '));

  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("Command line"));
  dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
  dlg.resize(760, 300);
  auto* v = new QVBoxLayout(&dlg);
  v->setContentsMargins(20, 20, 20, 20);
  auto* text = new QPlainTextEdit(cmd);
  text->setReadOnly(true);
  QFont mono(QStringLiteral("Consolas"));
  mono.setPointSizeF(9.5);
  text->setFont(mono);
  v->addWidget(text, 1);
  auto* row = new QHBoxLayout;
  row->addStretch();
  auto* copy = new QPushButton(QStringLiteral("Copy"));
  auto* close = new QPushButton(QStringLiteral("Close"));
  row->addWidget(copy);
  row->addWidget(close);
  v->addLayout(row);
  connect(copy, &QPushButton::clicked, &dlg, [copy, cmd] {
    QGuiApplication::clipboard()->setText(cmd);
    copy->setText(QStringLiteral("Copied"));
  });
  connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
  dlg.exec();
}

QString LauncherWindow::resolveGameDir(const QString& input) {
  if (QFileInfo(input).isDir()) {
    return input;
  }
  const QString cache = isoCacheDir(input);
  if (iso::cacheIsValid(cache, input)) {
    return cache;
  }
  ExtractDialog dlg(input, cache, this);
  if (dlg.exec() != QDialog::Accepted) {
    if (!dlg.error().isEmpty()) {
      QMessageBox::critical(this, QStringLiteral("Extraction failed"),
                            QStringLiteral("The ISO could not be extracted:\n\n") + dlg.error());
    }
    return QString();
  }
  return cache;
}

void LauncherWindow::play() {
  if (process_) {
    return;
  }
  const QString input = gamePath_->text().trimmed();
  const QFileInfo fi(input);
  const bool isIso = fi.isFile() && input.endsWith(QLatin1String(".iso"), Qt::CaseInsensitive);
  if (!isIso && !fi.isDir()) {
    QMessageBox::warning(this, QStringLiteral("No game selected"),
                         QStringLiteral("Pick an existing ISO, or a folder with the extracted game "
                                        "(default.xex)."));
    return;
  }
  saveSettings();

  const QString gameDir = resolveGameDir(input);
  if (gameDir.isEmpty()) {
    return;
  }
  refresh();
  QDir().mkpath(logDir_);

  process_ = new QProcess(this);
  process_->setProgram(gameExe_);
  process_->setArguments(buildArguments(gameDir));
  process_->setWorkingDirectory(QFileInfo(gameExe_).absolutePath());
  // Above-normal scheduling helps the audio and GPU threads when the CPU is
  // busy. High, never RealTime.
  process_->setCreateProcessArgumentsModifier(
      [](QProcess::CreateProcessArguments* args) { args->flags |= HIGH_PRIORITY_CLASS; });
  connect(process_, &QProcess::finished, this, &LauncherWindow::onGameFinished);
  connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
    if (e == QProcess::FailedToStart) {
      QMessageBox::critical(this, QStringLiteral("Error"),
                            QStringLiteral("Could not start the game:\n\n") + process_->errorString());
      process_->deleteLater();
      process_ = nullptr;
      play_->setEnabled(true);
      play_->setText(QStringLiteral("PLAY"));
      status_->clear();
    }
  });

  play_->setEnabled(false);
  play_->setText(QStringLiteral("RUNNING"));
  status_->setText(QStringLiteral("Running · ESC in game for settings, F3 for the fps counter."));
  process_->start();
  if (process_ && process_->waitForStarted(5000)) {
    showMinimized();
  }
}

void LauncherWindow::onGameFinished(int exitCode, QProcess::ExitStatus status) {
  if (process_) {
    process_->deleteLater();
    process_ = nullptr;
  }
  play_->setText(QStringLiteral("PLAY"));
  status_->clear();
  showNormal();
  raise();
  activateWindow();
  refresh();

  const QString lowered = searchLog({QStringLiteral("draw resolution scale is not supported")}, true);
  if (!lowered.isEmpty()) {
    QMessageBox::information(this, QStringLiteral("Internal scale reduced"),
                             QStringLiteral("Your GPU does not support the internal scale you "
                                            "picked, so it was lowered automatically:\n\n") +
                                 lowered);
  }
  if (status == QProcess::CrashExit || exitCode != 0) {
    const QString hints = searchLog({QStringLiteral("[critical]"), QStringLiteral("FATAL"),
                                     QStringLiteral("unregistered"),
                                     QStringLiteral("Unhandled guest")},
                                    false);
    QMessageBox::warning(
        this, QStringLiteral("Game exited with an error"),
        QStringLiteral("The game exited with code 0x%1.%2\n\nLog: %3")
            .arg(quint32(exitCode), 8, 16, QLatin1Char('0'))
            .arg(hints.isEmpty() ? QString() : QStringLiteral("\n\n") + hints)
            .arg(QDir::toNativeSeparators(runLog_)));
  }
}

// First matching line, or the last eight matches when firstOnly is false.
QString LauncherWindow::searchLog(const QStringList& needles, bool firstOnly) const {
  QFile f(runLog_);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return QString();
  }
  QStringList found;
  QTextStream in(&f);
  while (!in.atEnd()) {
    const QString line = in.readLine();
    for (const QString& n : needles) {
      if (line.contains(n)) {
        if (firstOnly) {
          return line;
        }
        found << line;
        break;
      }
    }
  }
  if (found.size() > 8) {
    found = found.mid(found.size() - 8);
  }
  return found.join(QLatin1Char('\n'));
}

}  // namespace nfsmw
