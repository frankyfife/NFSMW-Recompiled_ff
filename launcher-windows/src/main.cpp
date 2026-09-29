#include "launcher_window.h"
#include "theme.h"

#include <QApplication>

int main(int argc, char** argv) {
  QApplication::setHighDpiScaleFactorRoundingPolicy(
      Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("NFS Most Wanted Launcher"));
  QApplication::setApplicationVersion(QStringLiteral(NFSMW_LAUNCHER_VERSION));
  nfsmw::theme::apply(app);

  nfsmw::LauncherWindow window;
  window.show();
  return app.exec();
}
