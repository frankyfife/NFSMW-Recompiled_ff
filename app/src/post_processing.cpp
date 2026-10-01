// nfsmw - switch for the game's post-processing ("visual treatment")
//
// The frame's last passes (sub_82442478, with a flag block in r3): with the
// byte at +9 set, the picture goes through the effect "visualtreatment.fx"
// (sub_822246F0: technique "visualtreatment" or "visualtreatment_branching";
// colour curves, tint, bloom, vignette, the motion blur's blend; the bloom is
// prepared before by sub_82224C00), with +8 a second pass of it. Without it,
// the game copies the picture with the technique "screen_passthru" of its
// screen effect. With post_processing off the bytes are cleared for the call
// and put back afterwards.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(post_processing, true, "NFSMW",
                    "The game's post-processing (visual treatment: colour grading, tint, bloom, "
                    "vignette, motion blur). Off: the plain picture");

extern "C" REX_FUNC(__imp__sub_82442478);

extern "C" REX_FUNC(sub_82442478) {
  const uint32_t flags = ctx.r3.u32;
  if (REXCVAR_GET(post_processing) || !flags) {
    __imp__sub_82442478(ctx, base);
    return;
  }
  const uint8_t saved8 = base[flags + 8], saved9 = base[flags + 9];
  base[flags + 8] = 0;
  base[flags + 9] = 0;
  __imp__sub_82442478(ctx, base);
  base[flags + 8] = saved8;
  base[flags + 9] = saved9;
}
