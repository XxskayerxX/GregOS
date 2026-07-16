#ifndef GREG_TYPES_H
#define GREG_TYPES_H

/* Freestanding-safe: <stddef.h> and <stdint.h> are required
   freestanding headers per the C and C++ standards.          */
#include <stddef.h>
#include <stdint.h>

namespace Greg {

using u8   = uint8_t;
using u16  = uint16_t;
using u32  = uint32_t;
using u64  = uint64_t;
using i8   = int8_t;
using i16  = int16_t;
using i32  = int32_t;
using i64  = int64_t;
using usize = size_t;

} // namespace Greg

#endif /* GREG_TYPES_H */
