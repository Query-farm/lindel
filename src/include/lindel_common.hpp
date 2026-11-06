// Shared helpers for the lindel extension's encode, decode, bounded and range
// functions.
#pragma once

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/limits.hpp"

#include <cmath>
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

#include <cstring>

#include "rust.h"

namespace duckdb {
//! A positional-only parameter with a real name (bare types would be reported
//! as col0, col1, ...)
static inline FunctionParameter Positional(const char *name, LogicalType type) {
  return FunctionParameter(name, std::move(type), {},
                           FunctionParameterKind::POSITIONAL_ONLY);
}

// Store a 128-bit key into row `row` of a flat vector of any unsigned key type.
// The caller guarantees the key fits the vector's type.
static inline void StoreKey(Vector &vector, idx_t row, uhugeint_t key) {
  switch (vector.GetType().id()) {
  case LogicalTypeId::UTINYINT:
    FlatVector::GetDataMutable<uint8_t>(vector)[row] =
        static_cast<uint8_t>(key.lower);
    break;
  case LogicalTypeId::USMALLINT:
    FlatVector::GetDataMutable<uint16_t>(vector)[row] =
        static_cast<uint16_t>(key.lower);
    break;
  case LogicalTypeId::UINTEGER:
    FlatVector::GetDataMutable<uint32_t>(vector)[row] =
        static_cast<uint32_t>(key.lower);
    break;
  case LogicalTypeId::UBIGINT:
    FlatVector::GetDataMutable<uint64_t>(vector)[row] = key.lower;
    break;
  case LogicalTypeId::UHUGEINT:
    FlatVector::GetDataMutable<uhugeint_t>(vector)[row] = key;
    break;
  default:
    throw InternalException("lindel: unexpected key type %s",
                            vector.GetType().ToString());
  }
}
// Space-filling curves compare coordinates as unsigned integers, so signed and
// floating point coordinates are mapped to unsigned bit patterns whose unsigned
// order matches the value order before encoding, and mapped back after
// decoding:
//
//   SIGNED: flip the sign bit, so INT_MIN..-1,0..INT_MAX maps to 0..UINT_MAX in
//   order. FLOAT:  for non-negative values set the sign bit; for negative
//   values invert every bit.
//           This orders -inf < -1.0 < -0.0 < +0.0 < 1.0 < +inf (NaNs sort
//           beyond the infinities).
//
// Both mappings are bijections, so decode recovers the exact original bits.
enum class OrderTransform : uint8_t { NONE, SIGNED, FLOAT };

template <class U>
static inline U ToOrderedBits(U bits, OrderTransform transform) {
  constexpr U sign_bit = static_cast<U>(U(1) << (sizeof(U) * 8 - 1));
  switch (transform) {
  case OrderTransform::SIGNED:
    return static_cast<U>(bits ^ sign_bit);
  case OrderTransform::FLOAT:
    return (bits & sign_bit) ? static_cast<U>(~bits)
                             : static_cast<U>(bits | sign_bit);
  default:
    return bits;
  }
}

template <class U>
static inline U FromOrderedBits(U bits, OrderTransform transform) {
  constexpr U sign_bit = static_cast<U>(U(1) << (sizeof(U) * 8 - 1));
  switch (transform) {
  case OrderTransform::SIGNED:
    return static_cast<U>(bits ^ sign_bit);
  case OrderTransform::FLOAT:
    return (bits & sign_bit) ? static_cast<U>(bits & ~sign_bit)
                             : static_cast<U>(~bits);
  default:
    return bits;
  }
}

// The width (in bits) of each coordinate an encoder accepts, and how the
// coordinate's bits have to be adjusted so that unsigned comparison of the
// adjusted bits matches the natural ordering of the type.
struct EncodeElementInfo {
  idx_t bit_width;
  OrderTransform transform;
};

//! Human-readable list of the coordinate types the encoders accept, for error
//! messages.
static constexpr const char *SUPPORTED_ELEMENT_TYPES =
    "integer, FLOAT, DOUBLE, BOOLEAN, DATE, TIME, TIMESTAMP, DECIMAL (up to 18 "
    "digits) or ENUM";

// Coordinates are encoded from their physical representation, so any type whose
// physical value orders like the type itself can be encoded: dates and times
// are signed day/microsecond counts, DECIMALs are scaled signed integers, ENUMs
// are ordinal indexes.
static inline bool GetEncodeElementInfo(const LogicalType &type,
                                        EncodeElementInfo &info) {
  switch (type.id()) {
  case LogicalTypeId::BOOLEAN:
  case LogicalTypeId::UTINYINT:
  case LogicalTypeId::TINYINT:
  case LogicalTypeId::USMALLINT:
  case LogicalTypeId::SMALLINT:
  case LogicalTypeId::UINTEGER:
  case LogicalTypeId::INTEGER:
  case LogicalTypeId::UBIGINT:
  case LogicalTypeId::BIGINT:
  case LogicalTypeId::FLOAT:
  case LogicalTypeId::DOUBLE:
  case LogicalTypeId::DATE:
  case LogicalTypeId::TIME:
  case LogicalTypeId::TIME_NS:
  case LogicalTypeId::TIMESTAMP:
  case LogicalTypeId::TIMESTAMP_SEC:
  case LogicalTypeId::TIMESTAMP_MS:
  case LogicalTypeId::TIMESTAMP_NS:
  case LogicalTypeId::TIMESTAMP_TZ:
  case LogicalTypeId::TIMESTAMP_TZ_NS:
  case LogicalTypeId::DECIMAL:
  case LogicalTypeId::ENUM:
    break;
  default:
    return false;
  }
  switch (type.InternalType()) {
  case PhysicalType::BOOL:
  case PhysicalType::UINT8:
    info = {8, OrderTransform::NONE};
    return true;
  case PhysicalType::INT8:
    info = {8, OrderTransform::SIGNED};
    return true;
  case PhysicalType::UINT16:
    info = {16, OrderTransform::NONE};
    return true;
  case PhysicalType::INT16:
    info = {16, OrderTransform::SIGNED};
    return true;
  case PhysicalType::UINT32:
    info = {32, OrderTransform::NONE};
    return true;
  case PhysicalType::INT32:
    info = {32, OrderTransform::SIGNED};
    return true;
  case PhysicalType::FLOAT:
    info = {32, OrderTransform::FLOAT};
    return true;
  case PhysicalType::UINT64:
    info = {64, OrderTransform::NONE};
    return true;
  case PhysicalType::INT64:
    info = {64, OrderTransform::SIGNED};
    return true;
  case PhysicalType::DOUBLE:
    info = {64, OrderTransform::FLOAT};
    return true;
  default:
    // e.g. DECIMAL with more than 18 digits (INT128)
    return false;
  }
}

// The narrowest unsigned integer type that can hold a key of total_bits bits.
static inline LogicalType KeyTypeForBits(idx_t total_bits) {
  if (total_bits <= 8) {
    return LogicalType(LogicalTypeId::UTINYINT);
  }
  if (total_bits <= 16) {
    return LogicalType(LogicalTypeId::USMALLINT);
  }
  if (total_bits <= 32) {
    return LogicalType(LogicalTypeId::UINTEGER);
  }
  if (total_bits <= 64) {
    return LogicalType(LogicalTypeId::UBIGINT);
  }
  return LogicalType(LogicalTypeId::UHUGEINT);
}

// The narrowest coordinate width (8, 16, 32 or 64) the Rust encoders accept
// that holds bits bits.
static inline idx_t ElementWidthForBits(idx_t bits) {
  if (bits <= 8) {
    return 8;
  }
  if (bits <= 16) {
    return 16;
  }
  if (bits <= 32) {
    return 32;
  }
  return 64;
}

// 2^bits - 1 as a 128-bit value (bits may be 0..128).
static inline uhugeint_t LowBitMask(idx_t bits) {
  if (bits >= 128) {
    return NumericLimits<uhugeint_t>::Maximum();
  }
  if (bits == 0) {
    return uhugeint_t(uint64_t(0));
  }
  return (uhugeint_t(uint64_t(1)) << uhugeint_t(uint64_t(bits))) -
         uhugeint_t(uint64_t(1));
}

// Encode n grid coordinates (each < 2^element_width) on the Hilbert (hilbert ==
// true) or Morton curve. The coordinates are passed to the encoder as
// element_width-bit unsigned integers and the key is returned widened to 128
// bits.
static inline uhugeint_t EncodeGridPoint(bool hilbert, idx_t element_width,
                                         const uint64_t *coordinates, idx_t n) {
  // The Rust encoders write a little-endian key of up to 16 bytes; start from
  // zero so narrower keys leave the upper bytes clear.
  uhugeint_t key(uint64_t(0));
  bool ok = false;
  switch (element_width) {
  case 8: {
    uint8_t values[16];
    for (idx_t i = 0; i < n; i++) {
      values[i] = static_cast<uint8_t>(coordinates[i]);
    }
    ok = hilbert ? hilbert_encode_u8_var(values, n, &key)
                 : morton_encode_u8_var(values, n, &key);
    break;
  }
  case 16: {
    uint16_t values[8];
    for (idx_t i = 0; i < n; i++) {
      values[i] = static_cast<uint16_t>(coordinates[i]);
    }
    ok = hilbert ? hilbert_encode_u16_var(values, n, &key)
                 : morton_encode_u16_var(values, n, &key);
    break;
  }
  case 32: {
    uint32_t values[4];
    for (idx_t i = 0; i < n; i++) {
      values[i] = static_cast<uint32_t>(coordinates[i]);
    }
    ok = hilbert ? hilbert_encode_u32_var(values, n, &key)
                 : morton_encode_u32_var(values, n, &key);
    break;
  }
  case 64:
    ok = hilbert ? hilbert_encode_u64_var(coordinates, n, &key)
                 : morton_encode_u64_var(coordinates, n, &key);
    break;
  default:
    break;
  }
  if (!ok) {
    throw InternalException(
        "lindel: unsupported grid of %llu coordinates of %llu bits", n,
        element_width);
  }
  return key;
}

// The inverse of EncodeGridPoint.
static inline void DecodeGridPoint(bool hilbert, idx_t element_width,
                                   uhugeint_t key, idx_t n,
                                   uint64_t *coordinates) {
  // perform_decode reads a little-endian key of the width the encoder produced
  // from the start of the buffer; the uhugeint_t holds the key zero-extended to
  // 16 bytes.
  uint8_t buffer[16 * 8];
  if (!perform_decode(hilbert ? 0 : 1, static_cast<uint8_t>(element_width),
                      &key, buffer, n)) {
    throw InternalException(
        "lindel: unsupported grid of %llu coordinates of %llu bits", n,
        element_width);
  }
  for (idx_t i = 0; i < n; i++) {
    switch (element_width) {
    case 8:
      coordinates[i] = buffer[i];
      break;
    case 16: {
      uint16_t v;
      memcpy(&v, buffer + i * sizeof(v), sizeof(v));
      coordinates[i] = v;
      break;
    }
    case 32: {
      uint32_t v;
      memcpy(&v, buffer + i * sizeof(v), sizeof(v));
      coordinates[i] = v;
      break;
    }
    default: {
      uint64_t v;
      memcpy(&v, buffer + i * sizeof(v), sizeof(v));
      coordinates[i] = v;
      break;
    }
    }
  }
}

// Validate the grid parameters shared by the bounded functions and return the
// coordinate width used to call the encoders.
static inline idx_t ValidateBoundedGrid(const string &function_name,
                                        idx_t dimensions, int64_t bits) {
  if (dimensions < 1 || dimensions > 16) {
    throw InvalidInputException("%s() supports 1-16 dimensions, got %llu",
                                function_name, dimensions);
  }
  if (bits < 1 || bits > 64) {
    throw InvalidInputException("%s() bits must be between 1 and 64, got %lld",
                                function_name, bits);
  }
  auto element_width = ElementWidthForBits(static_cast<idx_t>(bits));
  if (element_width * dimensions > 128) {
    // The encoders take whole 8/16/32/64-bit coordinates and produce at most a
    // 128-bit key.
    idx_t max_bits =
        128 / dimensions >= 64
            ? 64
            : (128 / dimensions >= 32 ? 32 : (128 / dimensions >= 16 ? 16 : 8));
    throw InvalidInputException("%s() supports at most %llu bits per dimension "
                                "with %llu dimensions, got %lld",
                                function_name, max_bits, dimensions, bits);
  }
  return element_width;
}

// Map v from [lo, hi] onto the cells 0..2^bits-1. Values outside the bounds are
// clamped to the first or last cell.
static inline uint64_t QuantizeToGrid(double v, double lo, double hi,
                                      idx_t bits, const string &function_name) {
  if (std::isnan(v)) {
    throw InvalidInputException("%s: cannot encode NaN", function_name);
  }
  const double cells = std::ldexp(1.0, static_cast<int>(bits));
  const uint64_t last_cell = bits >= 64 ? NumericLimits<uint64_t>::Maximum()
                                        : ((uint64_t(1) << bits) - 1);
  const double t = (v - lo) / (hi - lo) * cells;
  if (!(t > 0)) {
    return 0;
  }
  if (t >= cells) {
    return last_cell;
  }
  auto cell = static_cast<uint64_t>(t);
  return cell > last_cell ? last_cell : cell;
}

// The centre of grid cell `cell` of 2^bits cells spanning [lo, hi].
static inline double GridCellCenter(uint64_t cell, double lo, double hi,
                                    idx_t bits) {
  const double cells = std::ldexp(1.0, static_cast<int>(bits));
  return lo + (static_cast<double>(cell) + 0.5) * ((hi - lo) / cells);
}

// Check a pair of bounds for one dimension.
static inline void ValidateBounds(double lo, double hi, idx_t dimension,
                                  const string &function_name) {
  if (!std::isfinite(lo) || !std::isfinite(hi) || !std::isfinite(hi - lo)) {
    throw InvalidInputException("%s: bounds for dimension %llu must be finite",
                                function_name, dimension + 1);
  }
  if (!(hi > lo)) {
    throw InvalidInputException(
        "%s: max must be greater than min for dimension %llu (min %s, max %s)",
        function_name, dimension + 1, std::to_string(lo), std::to_string(hi));
  }
}

void RegisterBoundedFunctions(ExtensionLoader &loader);
void RegisterRangeFunctions(ExtensionLoader &loader);
void AddTypedDecodeOverloads(ScalarFunctionSet &set);

} // namespace duckdb
