#pragma once

#include <string>

namespace TOPNSPC {

inline constexpr const char *PREFIX_SUPER = "S";
inline constexpr const char *PREFIX_COLL = "C";
inline constexpr const char *PREFIX_OBJ = "O";
inline constexpr const char *PREFIX_DEFERRED = "L";
inline constexpr const char *PREFIX_ALLOC_BITMAP = "b";
inline constexpr const char *PREFIX_OMAP = "M";         // u64 + keyname -> value
inline constexpr const char *PREFIX_PGMETA_OMAP = "P";  // u64 + keyname -> value(for meta coll)

}  // namespace TOPNSPC
