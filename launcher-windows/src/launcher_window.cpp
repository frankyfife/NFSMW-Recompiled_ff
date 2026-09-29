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

const QStringList kAaValues = {"none", "fxaa", "fxaa_extreme"};
const QList<int> kAnisoValues = {0, 2, 3, 4, 5};  // off, 2x, 4x, 8x, 16x
const QStringList kFilterValues = {"bilinear", "cas", "fsr"};
const QStringList kEdramValues = {"auto", "rtv", "rov"};

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

  auto* scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setWidget(buildContent());
  v->addWidget(scroll, 1);
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

// The SDK keeps Xenia-style shader storage in Documents\nfsmw\cache.
QString LauncherWindow::shaderCacheDir() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
      .filePath(QStringLiteral("nfsmw/cache/shaders/shareable"));
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
  gameOpts->grid()->addWidget(
      note(QStringLiteral("Can also be switched live in the in-game ESC menu.")),
      gameOpts->grid()->rowCount(), 0, 1, 2);
  connect(language_, &QComboBox::currentIndexChanged, this, onChange);
  connect(blackEdition_, &ToggleSwitch::toggled, this, onChange);
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
  vsync_ = new ToggleSwitch(QStringLiteral("V-Sync (sync to the display, no tearing)"));
  frame->addWide(vsync_);
  connect(fps_, &Segmented::currentIndexChanged, this, onChange);
  connect(customFps_, &QSpinBox::valueChanged, this, onChange);
  connect(vsync_, &ToggleSwitch::toggled, this, onChange);
  right->addWidget(frame);

  // ---- Image quality ----
  auto* image = new Card(QStringLiteral("Image quality"));
  aa_ = new Segmented({QStringLiteral("Off"), QStringLiteral("FXAA"), QStringLiteral("FXAA Extreme")});
  image->addRow(QStringLiteral("Anti-aliasing"), aa_);
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
  connect(aa_, &Segmented::currentIndexChanged, this, onChange);
  connect(aniso_, &Segmented::currentIndexChanged, this, onChange);
  connect(filter_, &Segmented::currentIndexChanged, this, onChange);
  connect(sharpness_, &QSlider::valueChanged, this, onChange);
  right->addWidget(image);

  // ---- Renderer & shaders ----
  auto* renderer = new Card(QStringLiteral("Renderer & shaders"));
  api_ = new Segmented({QStringLiteral("Direct3D 12"), QStringLiteral("Vulkan (exp.)")});
  renderer->addRow(QStringLiteral("Graphics API"), api_);
  edram_ = new Segmented({QStringLiteral("Auto"), QStringLiteral("Fast (RTV)"),
                          QStringLiteral("Accurate (ROV)")});
  renderer->addRow(QStringLiteral("EDRAM path"), edram_);
  asyncShaders_ = new ToggleSwitch(QStringLiteral("Compile shaders in the background (less stutter)"));
  renderer->addWide(asyncShaders_);
  auto* cacheRow = new QWidget;
  auto* cc = new QHBoxLayout(cacheRow);
  cc->setContentsMargins(0, 0, 0, 0);
  cc->setSpacing(8);
  cacheNote_ = note();
  auto* openCache = new QPushButton(QStringLiteral("Open"));
  auto* clearCache = new QPushButton(QStringLiteral("Clear"));
  cc->addWidget(cacheNote_, 1);
  cc->addWidget(openCache, 0, Qt::AlignTop);
  cc->addWidget(clearCache, 0, Qt::AlignTop);
  renderer->addWide(cacheRow);
  connect(api_, &Segmented::currentIndexChanged, this, onChange);
  connect(edram_, &Segmented::currentIndexChanged, this, onChange);
  connect(asyncShaders_, &ToggleSwitch::toggled, this, onChange);
  connect(openCache, &QPushButton::clicked, this, &LauncherWindow::openShaderCache);
  connect(clearCache, &QPushButton::clicked, this, &LauncherWindow::clearShaderCache);
  right->addWidget(renderer);
  right->addStretch();

  return content;
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

  const QString fps = s.value("frame/mode", "30").toString();
  fps_->setCurrentIndex(fps == "60" ? 1 : fps == "unlimited" ? 2 : fps == "custom" ? 3 : 0);
  customFps_->setValue(s.value("frame/fps", 60).toInt());
  vsync_->setChecked(s.value("frame/vsync", true).toBool());

  aa_->setCurrentIndex(std::max<qsizetype>(0, kAaValues.indexOf(s.value("image/aa", "none").toString())));
  const qsizetype an = kAnisoValues.indexOf(s.value("image/anisotropic", 4).toInt());
  aniso_->setCurrentIndex(an >= 0 ? int(an) : 3);
  filter_->setCurrentIndex(
      std::max<qsizetype>(0, kFilterValues.indexOf(s.value("image/filter", "bilinear").toString())));
  sharpness_->setValue(s.value("image/sharpness", 50).toInt());

  api_->setCurrentIndex(s.value("renderer/api", "d3d12").toString() == "vulkan" ? 1 : 0);
  edram_->setCurrentIndex(
      std::max<qsizetype>(0, kEdramValues.indexOf(s.value("renderer/edram", "auto").toString())));
  asyncShaders_->setChecked(s.value("renderer/async_shaders", true).toBool());
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
  static const char* const kFpsModes[] = {"30", "60", "unlimited", "custom"};
  s.setValue("frame/mode", kFpsModes[std::clamp(fps_->currentIndex(), 0, 3)]);
  s.setValue("frame/fps", customFps_->value());
  s.setValue("frame/vsync", vsync_->isChecked());
  s.setValue("image/aa", kAaValues[aa_->currentIndex()]);
  s.setValue("image/anisotropic", kAnisoValues[aniso_->currentIndex()]);
  s.setValue("image/filter", kFilterValues[filter_->currentIndex()]);
  s.setValue("image/sharpness", sharpness_->value());
  s.setValue("renderer/api", api_->currentIndex() == 1 ? "vulkan" : "d3d12");
  s.setValue("renderer/edram", kEdramValues[edram_->currentIndex()]);
  s.setValue("renderer/async_shaders", asyncShaders_->isChecked());
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

QStringList LauncherWindow::buildArguments(const QString& gameDir) const {
  QStringList a;
  a << "--log_level" << "info";
  a << "--log_file" << QDir::toNativeSeparators(runLog_);
  a << "--game_data_root" << QDir::toNativeSeparators(gameDir);
  a << "--gpu_plugin" << "xenos";
  a << "--mnk_mode";
  a << "--readback_resolve=fast";  // without it the image comes out washed out

  // Always passed: the command line wins over nfsmw.toml, so an API picked in
  // the F4 menu that shows a black screen can never lock the game out.
  a << QStringLiteral("--gpu_backend=%1").arg(api_->currentIndex() == 1 ? "vulkan" : "d3d12");

  a << "--resolution" << outputResolution();
  a << "--resolution_scale" << QString::number(scale_->currentIndex() + 1);
  a << QStringLiteral("--fullscreen=%1").arg(mode_->currentIndex() == 0 ? "true" : "false");
  a << "--monitor" << QString::number(std::max(0, monitor_->currentIndex()));

  // The game presents every second vblank, so the vblank rate is twice the
  // target; "unlimited" gives it a vblank every millisecond. V-Sync only
  // decides whether presentation waits for the display.
  const int fps = targetFps();
  a << QStringLiteral("--vsync=%1").arg(vsync_->isChecked() ? "true" : "false");
  a << "--guest_vblank_rate" << QString::number(fps == 0 ? 1000 : fps * 2);
  a << "--max_fps" << "0";

  a << QStringLiteral("--swap_post_effect=%1").arg(kAaValues[aa_->currentIndex()]);
  a << "--anisotropic_override" << QString::number(kAnisoValues[aniso_->currentIndex()]);
  if (filter_->currentIndex() != 0) {
    a << QStringLiteral("--present_effect=%1").arg(kFilterValues[filter_->currentIndex()]);
  }
  a << "--present_cas_additional_sharpness" << QString::number(sharpness_->value() / 100.0, 'f', 2);
  if (edram_->currentIndex() == 1) a << "--render_target_path_d3d12=rtv";
  if (edram_->currentIndex() == 2) a << "--render_target_path_d3d12=rov";

  const QString lang = QString::fromLatin1(kLanguages[language_->currentIndex()].value);
  if (lang != QLatin1String("0")) {
    a << "--user_language" << lang;
  }
  a << QStringLiteral("--black_edition=%1").arg(blackEdition_->isChecked() ? "true" : "false");
  a << QStringLiteral("--async_shader_compilation=%1")
           .arg(asyncShaders_->isChecked() ? "true" : "false");
  // One line every 10 s with the frames the game really presents.
  a << "--log_guest_fps=true";
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
    setNote(scaleNote_, QStringLiteral("Native Xbox 360 rendering resolution."));
  } else {
    setNote(scaleNote_,
            QStringLiteral("%1× width and height · %2× the pixels and GPU work.")
                .arg(scale)
                .arg(scale * scale),
            "hot");
  }

  customFps_->setEnabled(fps_->currentIndex() == 3);
  const int fps = targetFps();
  if (fps == 30) {
    setNote(fpsNote_, QStringLiteral("Console timing, as shipped. Smoothest."));
  } else {
    setNote(fpsNote_,
            QStringLiteral("Experimental: the game reaches what the emulation allows; menus "
                           "stay at 30. Physics run on real time, so speed is unaffected."),
            "warn");
  }

  const bool sharpen = filter_->currentIndex() != 0;
  sharpness_->setEnabled(sharpen);
  sharpnessValue_->setEnabled(sharpen);
  sharpnessValue_->setText(QStringLiteral("%1%").arg(sharpness_->value()));

  refreshGameStatus();
  refreshCache();

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

void LauncherWindow::refreshCache() {
  const QDir dir(shaderCacheDir());
  const QString xsh = dir.filePath(QStringLiteral("454107D9.xsh"));
  if (!QFileInfo::exists(xsh)) {
    setNote(cacheNote_, QStringLiteral("Shader cache is empty. It fills while you play; the first "
                                       "run stutters whenever something new is drawn."));
    hero_->setCacheStatus(QStringLiteral("Shader cache empty"));
    return;
  }
  qint64 bytes = 0;
  for (const QFileInfo& f : dir.entryInfoList({QStringLiteral("454107D9.*")}, QDir::Files)) {
    bytes += f.size();
  }
  const int n = countShaders(xsh);
  setNote(cacheNote_, QStringLiteral("%1 shaders cached (%2 KB). They are precompiled at every "
                                     "start; only new ones stutter once.")
                          .arg(n)
                          .arg(bytes / 1024));
  hero_->setCacheStatus(QStringLiteral("%1 shaders cached").arg(n));
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

void LauncherWindow::openShaderCache() {
  const QString dir = shaderCacheDir();
  QDir().mkpath(dir);
  QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void LauncherWindow::clearShaderCache() {
  if (QMessageBox::question(this, QStringLiteral("Clear shader cache"),
                            QStringLiteral("Delete the NFS Most Wanted shader cache?\n\nThe next "
                                           "run will stutter again until playing rebuilds it.")) !=
      QMessageBox::Yes) {
    return;
  }
  const QDir dir(shaderCacheDir());
  for (const QString& f : dir.entryList({QStringLiteral("454107D9.*")}, QDir::Files)) {
    QFile::remove(dir.filePath(f));
  }
  refresh();
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
