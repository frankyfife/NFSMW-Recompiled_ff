#include "iso_extractor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStack>
#include <QVector>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace nfsmw::iso {
namespace {

constexpr qint64 kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr int kMagicLen = sizeof(kMagic) - 1;

// Raw partition / trimmed image, XGD2, XGD3, XGD1.
constexpr qint64 kKnownBases[] = {0x00000000LL, 0x0FD90000LL, 0x02080000LL, 0x18300000LL};

struct Entry {
  QString path;
  quint32 sector = 0;
  quint32 size = 0;
  bool dir = false;
};

struct Cancel {};

quint16 u16(const QByteArray& b, int off) {
  return quint16(quint8(b[off])) | quint16(quint8(b[off + 1])) << 8;
}

quint32 u32(const QByteArray& b, int off) {
  return quint32(quint8(b[off])) | quint32(quint8(b[off + 1])) << 8 |
         quint32(quint8(b[off + 2])) << 16 | quint32(quint8(b[off + 3])) << 24;
}

bool magicAt(QFile& f, qint64 base) {
  if (!f.seek(base + 32 * kSector)) {
    return false;
  }
  const QByteArray b = f.read(kMagicLen);
  return b.size() == kMagicLen && std::memcmp(b.constData(), kMagic, kMagicLen) == 0;
}

qint64 detectBase(QFile& f) {
  for (qint64 b : kKnownBases) {
    if (magicAt(f, b)) {
      return b;
    }
  }
  // Brute-force scan of the first GB in 16 MB chunks, overlapping so a magic
  // split across two chunks is not missed.
  const qint64 top = std::min<qint64>(f.size(), qint64(1) << 30);
  const qint64 chunk = 16 << 20;
  for (qint64 pos = 0; pos < top; pos += chunk) {
    f.seek(pos);
    const QByteArray buf = f.read(chunk + kMagicLen);
    int idx = buf.indexOf(kMagic);
    while (idx >= 0) {
      const qint64 abs = pos + idx;
      if (abs % kSector == 0 && abs >= 32 * kSector && magicAt(f, abs - 32 * kSector)) {
        return abs - 32 * kSector;
      }
      idx = buf.indexOf(kMagic, idx + 1);
    }
  }
  throw std::runtime_error(
      "No XDVDFS file system found in the image.\n"
      "Make sure it is a plain Xbox 360 ISO and not a compressed CCI/GOD/ZAR.");
}

void walk(QFile& f, qint64 base, quint32 sector, quint32 size, const QString& prefix,
          QVector<Entry>& out) {
  if (size == 0 || size > (256u << 20)) {
    return;
  }
  f.seek(base + qint64(sector) * kSector);
  const QByteArray table = f.read(size);

  // Each directory is a flat binary tree; child offsets are in 4-byte units.
  QVector<Entry> children;
  QStack<int> stack;
  QSet<int> seen;
  stack.push(0);
  while (!stack.isEmpty()) {
    const int off = stack.pop();
    if (seen.contains(off) || off + 14 > table.size()) {
      continue;
    }
    seen.insert(off);
    const quint16 left = u16(table, off);
    const quint16 right = u16(table, off + 2);
    if (left != 0 && left != 0xFFFF) stack.push(left * 4);
    if (right != 0 && right != 0xFFFF) stack.push(right * 4);

    const int len = quint8(table[off + 13]);
    if (len == 0 || off + 14 + len > table.size()) {
      continue;
    }
    Entry e;
    e.path = QString::fromLatin1(table.constData() + off + 14, len);
    e.sector = u32(table, off + 4);
    e.size = u32(table, off + 8);
    e.dir = (quint8(table[off + 12]) & 0x10) != 0;
    children.push_back(e);
  }
  std::sort(children.begin(), children.end(), [](const Entry& a, const Entry& b) {
    return a.path.compare(b.path, Qt::CaseInsensitive) < 0;
  });
  for (Entry& e : children) {
    e.path = prefix.isEmpty() ? e.path : prefix + QLatin1Char('/') + e.path;
    out.push_back(e);
    if (e.dir) {
      walk(f, base, e.sector, e.size, e.path, out);
    }
  }
}

void extractFile(QFile& f, qint64 base, const Entry& e, const QString& dest,
                 const Cancelled& cancelled) {
  QDir().mkpath(QFileInfo(dest).absolutePath());
  QFile out(dest);
  if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    throw std::runtime_error(("Cannot write " + dest).toStdString());
  }
  f.seek(base + qint64(e.sector) * kSector);
  qint64 left = e.size;
  constexpr qint64 kBuf = 4 << 20;
  while (left > 0) {
    if (cancelled && cancelled()) {
      throw Cancel{};
    }
    const QByteArray b = f.read(std::min(kBuf, left));
    if (b.isEmpty()) {
      throw std::runtime_error(
          ("Unexpected end of file reading " + e.path + ". Incomplete image?").toStdString());
    }
    out.write(b);
    left -= b.size();
  }
}

QString markerPath(const QString& cache) { return QDir(cache).filePath(".origen_iso.txt"); }

}  // namespace

Result extract(const QString& isoPath, const QString& dest, const Progress& progress,
               const Cancelled& cancelled, QString* error) {
  try {
    QFile f(isoPath);
    if (!f.open(QIODevice::ReadOnly)) {
      throw std::runtime_error(("Cannot open " + isoPath).toStdString());
    }
    const qint64 base = detectBase(f);
    f.seek(base + 32 * kSector);
    const QByteArray vd = f.read(kSector);
    if (vd.size() < kSector || std::memcmp(vd.constData(), kMagic, kMagicLen) != 0) {
      throw std::runtime_error("Invalid volume descriptor.");
    }

    QVector<Entry> entries;
    walk(f, base, u32(vd, 0x14), u32(vd, 0x18), QString(), entries);
    if (entries.isEmpty()) {
      throw std::runtime_error("The file system is empty. Corrupt image?");
    }

    const int total = int(std::count_if(entries.begin(), entries.end(),
                                        [](const Entry& e) { return !e.dir; }));
    int done = 0;
    for (const Entry& e : entries) {
      if (e.dir) {
        continue;
      }
      extractFile(f, base, e, QDir(dest).filePath(e.path), cancelled);
      ++done;
      if (progress) {
        progress(done, total, e.path);
      }
    }
    if (!QFileInfo::exists(QDir(dest).filePath("default.xex"))) {
      throw std::runtime_error(
          "The image has no default.xex in its root. This does not look like the "
          "Xbox 360 Need for Speed: Most Wanted disc.");
    }
    return Result::kOk;
  } catch (const Cancel&) {
    return Result::kCancelled;
  } catch (const std::exception& ex) {
    if (error) {
      *error = QString::fromUtf8(ex.what());
    }
    return Result::kFailed;
  }
}

bool cacheIsValid(const QString& cache, const QString& isoPath) {
  if (!QFileInfo::exists(QDir(cache).filePath("default.xex"))) {
    return false;
  }
  QFile m(markerPath(cache));
  if (!m.open(QIODevice::ReadOnly)) {
    return false;
  }
  const QStringList parts = QString::fromUtf8(m.readAll()).split(QLatin1Char('|'));
  if (parts.size() < 2) {
    return false;
  }
  const QFileInfo iso(isoPath);
  return QDir::cleanPath(parts[0]).compare(QDir::cleanPath(iso.absoluteFilePath()),
                                           Qt::CaseInsensitive) == 0 &&
         parts[1].trimmed().toLongLong() == iso.size();
}

void writeMarker(const QString& cache, const QString& isoPath) {
  QFile m(markerPath(cache));
  if (m.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    const QFileInfo iso(isoPath);
    m.write((QDir::toNativeSeparators(iso.absoluteFilePath()) + QLatin1Char('|') +
             QString::number(iso.size()))
                .toUtf8());
  }
}

QString safeName(const QString& name) {
  QString s = name;
  for (QChar& c : s) {
    if (QStringLiteral("<>:\"/\\|?*").contains(c) || c.unicode() < 32) {
      c = QLatin1Char('_');
    }
  }
  return s.isEmpty() ? QStringLiteral("iso") : s;
}

}  // namespace nfsmw::iso
