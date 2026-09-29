#pragma once

#include <QString>

#include <functional>

// Xbox 360 .iso (XDVDFS) -> folder. The SDK only accepts a directory as
// --game_data_root, so an ISO is extracted once and reused while its path and
// size stay the same. C++ port of tools/fase1_extraer.py ("extract all").
namespace nfsmw::iso {

using Progress = std::function<void(int done, int total, const QString& file)>;
using Cancelled = std::function<bool()>;

enum class Result { kOk, kCancelled, kFailed };

Result extract(const QString& isoPath, const QString& dest, const Progress& progress,
               const Cancelled& cancelled, QString* error);

bool cacheIsValid(const QString& cacheDir, const QString& isoPath);
void writeMarker(const QString& cacheDir, const QString& isoPath);
QString safeName(const QString& name);

}  // namespace nfsmw::iso
