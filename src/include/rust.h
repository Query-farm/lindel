#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <new>

extern "C"
{

  void hilbert_encode_u16_var(const uint16_t *ptr, size_t len, void *result);

  void hilbert_encode_u32_var(const uint32_t *ptr, size_t len, void *result);

  void hilbert_encode_u64_var(const uint64_t *ptr, size_t len, void *result);

  void hilbert_encode_u8_var(const uint8_t *ptr, size_t len, void *result);

  void morton_encode_u16_var(const uint16_t *ptr, size_t len, void *result);

  void morton_encode_u32_var(const uint32_t *ptr, size_t len, void *result);

  void morton_encode_u64_var(const uint64_t *ptr, size_t len, void *result);

  void morton_encode_u8_var(const uint8_t *ptr, size_t len, void *result);

  void perform_decode(uint8_t encoding_type,
                      uint8_t element_bit_width,
                      const void *src,
                      void *dest,
                      size_t dest_len);

} // extern "C"
