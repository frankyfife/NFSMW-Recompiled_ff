// Builds without the native renderer (not Windows, or no rexglue-sdk source
// tree next to the project): the D3D hooks see it disabled.
#include <cstdint>

bool NativeRendererEnabled() { return false; }
void NativeRendererRecord(int, const uint32_t*, uint32_t, uint8_t*) {}
void NativeRendererBefore(int, const uint32_t*, uint8_t*) {}
void NativeRendererSwapDone() {}
