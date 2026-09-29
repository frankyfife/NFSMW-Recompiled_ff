#pragma once

#include <QColor>

class QApplication;

namespace nfsmw::theme {

// One place for the palette; the style sheet in theme.cpp uses the same values.
inline QColor back() { return QColor(12, 14, 17); }
inline QColor card() { return QColor(22, 25, 31); }
inline QColor cardBorder() { return QColor(37, 42, 51); }
inline QColor field() { return QColor(14, 16, 20); }
inline QColor fieldBorder() { return QColor(48, 55, 66); }
inline QColor fieldHover() { return QColor(30, 34, 42); }
inline QColor text() { return QColor(230, 233, 238); }
inline QColor textDim() { return QColor(140, 149, 163); }
inline QColor textFaint() { return QColor(92, 100, 112); }
inline QColor accent() { return QColor(255, 146, 28); }
inline QColor accentHot() { return QColor(255, 172, 72); }
inline QColor accentInk() { return QColor(24, 14, 4); }
inline QColor good() { return QColor(76, 195, 138); }
inline QColor warn() { return QColor(255, 181, 71); }
inline QColor bad() { return QColor(255, 92, 92); }

// Fusion style + dark palette + style sheet.
void apply(QApplication& app);

}  // namespace nfsmw::theme
