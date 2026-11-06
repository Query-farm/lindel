#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <new>

extern "C" {

/// Encode `len` coordinates read from `ptr` and write the key to `result`.
///
/// Returns `false` (and writes nothing) for an unsupported `len` or a
/// null pointer.
///
/// # Safety
///
/// `ptr` must be valid for `len` reads and `result` must have room for
/// the key type that `len` maps to.
bool hilbert_encode_u16_var(const uint16_t *ptr, size_t len, void *result);

/// Encode `len` coordinates read from `ptr` and write the key to `result`.
///
/// Returns `false` (and writes nothing) for an unsupported `len` or a
/// null pointer.
///
/// # Safety
///
/// `ptr` must be valid for `len` reads and `result` must have room for
/// the key type that `len` maps to.
bool hilbert_encode_u32_var(const uint32_t *ptr, size_t len, void *result);

/// Encode `len` coordinates read from `ptr` and write the key to `result`.
///
/// Returns `false` (and writes nothing) for an unsupported `len` or a
/// null pointer.
///
/// # Safety
///
/// `ptr` must be valid for `len` reads and `result` must have room for
/// the key type that `len` maps to.
bool hilbert_encode_u64_var(const uint64_t *ptr, size_t len, void *result);

/// Encode `len` coordinates read from `ptr` and write the key to `result`.
///
/// Returns `false` (and writes nothing) for an unsupported `len` or a
/// null pointer.
///
/// # Safety
///
/// `ptr` must be valid for `len` reads and `result` must have room for
/// the key type that `len` maps to.
bool hilbert_encode_u8_var(const uint8_t *ptr, size_t len, void *result);

/// Encode `len` coordinates read from `ptr` and write the key to `result`.
///
/// Returns `false` (and writes nothing) for an unsupported `len` or a
/// null pointer.
///
/// # Safety
///
/// `ptr` must be valid for `len` reads and `result` must have room for
/// the key type that `len` maps to.
bool morton_encode_u16_var(const uint16_t *ptr, size_t len, void *result);

/// Encode `len` coordinates read from `ptr` and write the key to `result`.
///
/// Returns `false` (and writes nothing) for an unsupported `len` or a
/// null pointer.
///
/// # Safety
///
/// `ptr` must be valid for `len` reads and `result` must have room for
/// the key type that `len` maps to.
bool morton_encode_u32_var(const uint32_t *ptr, size_t len, void *result);

/// Encode `len` coordinates read from `ptr` and write the key to `result`.
///
/// Returns `false` (and writes nothing) for an unsupported `len` or a
/// null pointer.
///
/// # Safety
///
/// `ptr` must be valid for `len` reads and `result` must have room for
/// the key type that `len` maps to.
bool morton_encode_u64_var(const uint64_t *ptr, size_t len, void *result);

/// Encode `len` coordinates read from `ptr` and write the key to `result`.
///
/// Returns `false` (and writes nothing) for an unsupported `len` or a
/// null pointer.
///
/// # Safety
///
/// `ptr` must be valid for `len` reads and `result` must have room for
/// the key type that `len` maps to.
bool morton_encode_u8_var(const uint8_t *ptr, size_t len, void *result);

/// Decode one encoded key into `dest_len` coordinates of `element_bit_width`
/// bits each.
///
/// `encoding_type` is 0 for Hilbert and 1 for Morton. `src` must point at an
/// unsigned integer of the width the encoder produced for that combination and
/// `dest` at space for `dest_len` unsigned integers of `element_bit_width`.
///
/// # Safety
///
/// `src` and `dest` must be valid for the reads and writes described above.
bool perform_decode(uint8_t encoding_type, uint8_t element_bit_width,
                    const void *src, void *dest, size_t dest_len);

} // extern "C"
