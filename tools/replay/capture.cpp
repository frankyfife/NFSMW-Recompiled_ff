#include "capture.h"

#include <cstdio>
#include <cstring>

namespace replay {

namespace {

constexpr uint32_t kMagicFile = 0x4353464E;       // 'NFSC'
constexpr uint32_t kMagicEvent = 0x544E5645;      // 'EVNT'
constexpr uint32_t kMagicDraw = 0x57415244;       // 'DRAW'
constexpr uint32_t kMagicEventRegs = 0x52545645;  // 'EVTR'
constexpr uint32_t kMagicObject = 0x204A424F;     // 'OBJ '
constexpr uint32_t kMagicMemory = 0x204D454D;     // 'MEM '

// Order of the register shadow in the capture (render_capture.cpp).
struct ShadowGroup {
  uint32_t first_register;
  uint32_t count;
};
constexpr ShadowGroup kShadowGroups[] = {
    {0x2000, 16}, {0x2100, 21},   {0x2180, 5},    {0x2200, 12},  {0x2280, 21}, {0x2300, 38},
    {0x2380, 8},  {0x4000, 1024}, {0x4400, 1024}, {0x4800, 192}, {0x4900, 40},
};

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long long size = _ftelli64(f);
  std::fseek(f, 0, SEEK_SET);
  out.resize(size_t(size));
  const size_t read = std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  return read == out.size();
}

uint32_t Load32(const std::vector<uint8_t>& raw, size_t offset) {
  uint32_t v;
  std::memcpy(&v, raw.data() + offset, 4);
  return v;
}

}  // namespace

bool Capture::Load(const std::string& directory, uint32_t frame) {
  const std::string game = directory + "/capture_game_" + std::to_string(frame) + ".bin";
  if (!ReadFile(game, raw_)) {
    std::fprintf(stderr, "cannot read %s\n", game.c_str());
    return false;
  }
  if (raw_.size() < 8 || Load32(raw_, 0) != kMagicFile || Load32(raw_, 4) != 2) {
    std::fprintf(stderr, "%s: not a version 2 capture\n", game.c_str());
    return false;
  }
  // Memory updates are listed with the sequence of the draw they belong to;
  // the BeginVertices data comes after its record.
  std::map<uint32_t, std::vector<MemoryUpdate>> updates;
  size_t off = 8;
  while (off + 4 <= raw_.size()) {
    const uint32_t magic = Load32(raw_, off);
    if (magic == kMagicObject) {
      const uint32_t kind = Load32(raw_, off + 8), address = Load32(raw_, off + 12),
                     size = Load32(raw_, off + 16);
      off += 20;
      std::vector<uint8_t> bytes(raw_.begin() + off, raw_.begin() + off + size);
      if (kind >= 0x100) {
        if (!records_.empty()) {
          records_.back().pointed[kind - 0x100] = std::move(bytes);
        }
      } else {
        objects_.emplace(address, std::move(bytes));
      }
      off += size;
      continue;
    }
    if (magic == kMagicMemory) {
      const uint32_t sequence = Load32(raw_, off + 4);
      MemoryUpdate u{Load32(raw_, off + 8), Load32(raw_, off + 12), off + 16};
      updates[sequence].push_back(u);
      off += 16 + u.size;
      continue;
    }
    if (magic != kMagicEvent && magic != kMagicDraw && magic != kMagicEventRegs) {
      std::fprintf(stderr, "bad record at %zu\n", off);
      return false;
    }
    Record r;
    r.magic = magic;
    r.sequence = Load32(raw_, off + 4);
    r.entry = Load32(raw_, off + 8);
    for (int i = 0; i < 8; ++i) {
      r.args[i] = Load32(raw_, off + 12 + 4 * i);
    }
    r.result = Load32(raw_, off + 44);
    off += 48;
    size_t register_offset = 0;
    if (magic != kMagicEvent) {
      const uint32_t n = Load32(raw_, off);
      register_offset = off + 4;
      off += 4 + 4 * size_t(n);
      r.has_registers = true;
    }
    if (magic == kMagicDraw) {
      const uint32_t m = Load32(raw_, off);
      for (uint32_t i = 0; i < m && i < 4; ++i) {
        r.objects[i] = Load32(raw_, off + 4 + 4 * i);
      }
      off += 4 + 4 * size_t(m);
      r.is_draw = true;
    }
    records_.push_back(std::move(r));
    register_offsets_.push_back(register_offset);
  }
  for (Record& r : records_) {
    auto it = updates.find(r.sequence);
    if (it != updates.end()) {
      r.memory = std::move(it->second);
    }
  }

  const std::string snap = directory + "/capture_snap_" + std::to_string(frame) + ".bin";
  memory_.reset(new uint8_t[kPhysicalSize]);
  FILE* f = std::fopen(snap.c_str(), "rb");
  if (!f || std::fread(memory_.get(), 1, kPhysicalSize, f) != kPhysicalSize) {
    std::fprintf(stderr, "cannot read %s\n", snap.c_str());
    if (f) {
      std::fclose(f);
    }
    return false;
  }
  std::fclose(f);
  return true;
}

void Capture::GetRegisters(const Record& record, uint32_t* registers_out) const {
  std::memset(registers_out, 0, sizeof(uint32_t) * kRegisterCount);
  const size_t index = size_t(&record - records_.data());
  if (!record.has_registers || index >= register_offsets_.size()) {
    return;
  }
  size_t off = register_offsets_[index];
  for (const ShadowGroup& g : kShadowGroups) {
    for (uint32_t i = 0; i < g.count; ++i) {
      registers_out[g.first_register + i] = Load32(raw_, off);
      off += 4;
    }
  }
}

const std::vector<uint8_t>* Capture::FindObject(uint32_t address) const {
  auto it = objects_.find(address);
  return it != objects_.end() ? &it->second : nullptr;
}

void Capture::ApplyMemory(const Record& record) {
  for (const MemoryUpdate& u : record.memory) {
    if (u.address < kPhysicalSize && u.size <= kPhysicalSize - u.address) {
      std::memcpy(memory_.get() + u.address, raw_.data() + u.file_offset, u.size);
    }
  }
}

}  // namespace replay
