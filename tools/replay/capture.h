// Reading a frame written by app/src/render_capture.cpp (format version 2):
// the D3D calls of the frame with the device's register shadow at draws,
// resolves and clears, the objects they use, the data each draw reads and the
// guest physical memory at the start of the frame.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace replay {

// Entry points of the game's D3D library (index in the capture = position in
// tools/renderprobe/d3d_layer.json).
enum Entry : uint32_t {
  kEntryResolve = 10,            // sub_82592538 (dev, flags, rect*, texture*, point*, ...)
  kEntryBeginVertices = 11,      // sub_825932D8 (dev, prim, count, stride) -> data
  kEntryDrawIndexedUP = 12,      // sub_82593588
  kEntryDrawVertices = 13,       // sub_82593A10 (dev, prim, start, count)
  kEntryDrawIndexed = 14,        // sub_82593C50 (dev, prim, base, start, count)
  kEntrySwap = 45,               // sub_825989D8
  kEntryClear = 53,              // sub_8259A500
};

// Xenos registers 0x0000..0x5002; the capture has the shadowed ranges.
constexpr uint32_t kRegisterCount = 0x5003;

struct MemoryUpdate {
  uint32_t address;  // physical
  uint32_t size;
  size_t file_offset;
};

struct Record {
  uint32_t magic = 0;
  uint32_t sequence = 0;
  uint32_t entry = 0;
  uint32_t args[8] = {};  // r3..r10 at the call
  uint32_t result = 0;    // r3 after it
  bool has_registers = false;
  bool is_draw = false;
  // Device objects at a draw: vertex shader, pixel shader, declaration, IB.
  uint32_t objects[4] = {};
  // Argument n -> 64 bytes it points to (resolve, clear).
  std::map<uint32_t, std::vector<uint8_t>> pointed;
  // Data to apply before this record (and for BeginVertices, the vertices
  // the caller wrote after it).
  std::vector<MemoryUpdate> memory;
};

class Capture {
 public:
  bool Load(const std::string& directory, uint32_t frame);

  const std::vector<Record>& records() const { return records_; }
  // Full register file at a record with registers (zero outside the shadow).
  void GetRegisters(const Record& record, uint32_t* registers_out) const;
  // Guest object bytes (first 1 KB) by guest address.
  const std::vector<uint8_t>* FindObject(uint32_t address) const;

  // Guest physical memory: the snapshot, with ApplyMemory applied.
  uint8_t* memory() const { return memory_.get(); }
  void ApplyMemory(const Record& record);

  static constexpr uint32_t kPhysicalSize = 0x20000000;

 private:
  std::vector<uint8_t> raw_;
  std::vector<Record> records_;
  std::vector<size_t> register_offsets_;  // per record, offset of the shadow
  std::map<uint32_t, std::vector<uint8_t>> objects_;
  std::unique_ptr<uint8_t[]> memory_;
};

// Guest virtual address -> physical (0xE0000000 views are offset by 4 KB).
inline uint32_t GuestToPhysical(uint32_t address) {
  return (address & 0x1FFFFFFF) + (address >= 0xE0000000u ? 0x1000u : 0u);
}

inline uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

}  // namespace replay
