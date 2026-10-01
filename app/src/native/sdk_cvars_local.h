// Forced include for the SDK sources compiled into the game for the native
// renderer (the Xenos shader translator and helpers). The GPU plugin defines
// the same cvars; registering them a second time from the game would make the
// plugin's ones unreachable (the registry keeps the first and logs an error).
// Here the cvars are plain variables with their default values.
#pragma once

#include <rex/cvar.h>

namespace native_renderer_cvars {
struct NullRegistrar {
  NullRegistrar&& range(double, double) && { return std::move(*this); }
  NullRegistrar&& allowed(std::initializer_list<std::string>) && { return std::move(*this); }
  NullRegistrar&& lifecycle(::rex::cvar::Lifecycle) && { return std::move(*this); }
  NullRegistrar&& debug_only() && { return std::move(*this); }
  template <typename F>
  NullRegistrar&& validator(F&&) && {
    return std::move(*this);
  }
};
}  // namespace native_renderer_cvars

#define NATIVE_LOCAL_CVAR(type, name, default_val) \
  type& FLAGS_##name##_storage_() {                \
    static type storage = (default_val);           \
    return storage;                                \
  }                                                \
  [[maybe_unused]] static auto _cvar_reg_##name = ::native_renderer_cvars::NullRegistrar {}

#undef REXCVAR_DEFINE_BOOL
#undef REXCVAR_DEFINE_INT32
#undef REXCVAR_DEFINE_INT64
#undef REXCVAR_DEFINE_UINT32
#undef REXCVAR_DEFINE_UINT64
#undef REXCVAR_DEFINE_DOUBLE
#undef REXCVAR_DEFINE_STRING
#define REXCVAR_DEFINE_BOOL(name, d, category, desc) NATIVE_LOCAL_CVAR(bool, name, d)
#define REXCVAR_DEFINE_INT32(name, d, category, desc) NATIVE_LOCAL_CVAR(int32_t, name, d)
#define REXCVAR_DEFINE_INT64(name, d, category, desc) NATIVE_LOCAL_CVAR(int64_t, name, d)
#define REXCVAR_DEFINE_UINT32(name, d, category, desc) NATIVE_LOCAL_CVAR(uint32_t, name, d)
#define REXCVAR_DEFINE_UINT64(name, d, category, desc) NATIVE_LOCAL_CVAR(uint64_t, name, d)
#define REXCVAR_DEFINE_DOUBLE(name, d, category, desc) NATIVE_LOCAL_CVAR(double, name, d)
#define REXCVAR_DEFINE_STRING(name, d, category, desc) NATIVE_LOCAL_CVAR(std::string, name, d)
