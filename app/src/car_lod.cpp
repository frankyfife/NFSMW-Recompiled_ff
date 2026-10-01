// nfsmw - cars at their highest level of detail at any distance
//
// The car's render step (sub_824E0648, CarRenderInfo +5688 / +5692 are the
// lowest and highest LOD index its models allow) asks the view how many
// pixels the car covers (sub_8221E848(view, position, radius), the guest's
// 1280x720) and picks the LOD from that: 120 or more LOD 0 (the full model),
// then one step more below 25, 20, 10 and 0 pixels, clamped to the car's
// range. Below 1 pixel the car is not drawn. So a car went to its second
// model at a moderate distance (at 720p: 120 pixels), visible at the higher
// resolutions the PC draws at.
//
// With car_lod_highest the pixel count the render step gets for a car on
// screen is at least 120 (1024 is written; only for its two calls, told apart by the return
// address): the lowest LOD index the car has, culling unchanged. Measured in
// free roam: about half of the cars drawn were below 120 pixels; every car
// has LOD 0..4, so LOD 0 is always loaded. In a frozen view 4663 -> 4762
// draws (the full models have more parts).

#include <cstdint>

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(car_lod_highest, true, "NFSMW",
                    "Cars always at their highest level of detail (the game lowers it from "
                    "about 120 pixels on screen down)");

extern "C" REX_FUNC(__imp__sub_8221E848);

namespace {
constexpr uint32_t kCarRenderReturn = 0x824E06CC;      // sub_824E0648: the car's view
constexpr uint32_t kCarRenderReturnAlt = 0x824E06F4;   // and for view 13
constexpr int32_t kHighestLodPixels = 120;
// Written instead: well above, since in one game state the step scales the
// count by 0.7 afterwards (0x82A2CEE4 == 3).
constexpr int32_t kWrittenPixels = 1024;
}  // namespace

extern "C" REX_FUNC(sub_8221E848) {
  const uint32_t return_address = uint32_t(ctx.lr);
  __imp__sub_8221E848(ctx, base);
  if (return_address != kCarRenderReturn && return_address != kCarRenderReturnAlt) {
    return;
  }
  const int32_t pixels = int32_t(ctx.r3.u32);
  if (REXCVAR_GET(car_lod_highest) && pixels >= 1 && pixels < kHighestLodPixels) {
    ctx.r3.u64 = uint64_t(kWrittenPixels);
  }
}
