// Bounded encoding: scale real-valued coordinates into a fixed-size integer
// grid before encoding.
//
// Encoding raw floating point bits gives poor locality (the exponent dominates
// the key). Quantizing each coordinate into 2^bits cells over known bounds
// gives compact keys whose curve order follows the coordinates' geometry, which
// is what spatial indexing wants.

#include "lindel_common.hpp"

#include "duckdb/common/string_util.hpp"

#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/unified_vector_format.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

namespace duckdb {
namespace {
constexpr double LATITUDE_MIN = -90.0;
constexpr double LATITUDE_MAX = 90.0;
constexpr double LONGITUDE_MIN = -180.0;
constexpr double LONGITUDE_MAX = 180.0;
constexpr int64_t LATLON_DEFAULT_BITS = 32;

struct BoundedBindData : public FunctionData {
  BoundedBindData(bool hilbert_p, idx_t dimensions_p, idx_t bits_p,
                  string function_name_p, bool geometry_p = false)
      : hilbert(hilbert_p), dimensions(dimensions_p), bits(bits_p),
        element_width(ElementWidthForBits(bits_p)),
        function_name(std::move(function_name_p)), geometry(geometry_p) {}

  bool hilbert;
  idx_t dimensions;
  idx_t bits;
  idx_t element_width;
  string function_name;
  //! The coordinates come from the bounding-box centre of a GEOMETRY argument
  bool geometry;

  unique_ptr<FunctionData> Copy() const override {
    return make_uniq<BoundedBindData>(hilbert, dimensions, bits, function_name,
                                      geometry);
  }

  bool Equals(const FunctionData &other_p) const override {
    auto &other = other_p.Cast<BoundedBindData>();
    return hilbert == other.hilbert && dimensions == other.dimensions &&
           bits == other.bits && function_name == other.function_name &&
           geometry == other.geometry;
  }
};

bool IsHilbert(const string &function_name) {
  return StringUtil::StartsWith(function_name, "hilbert_");
}

string FunctionName(const BoundScalarFunction &function) {
  // GetName() is an Identifier in DuckDB 2.0; ToString() would quote it.
  const auto &name = function.GetName();
  for (auto candidate : {"hilbert_encode_bounded", "morton_encode_bounded",
                         "hilbert_decode_bounded", "morton_decode_bounded",
                         "hilbert_encode_latlon", "morton_encode_latlon",
                         "hilbert_decode_latlon", "morton_decode_latlon"}) {
    if (name == candidate) {
      return candidate;
    }
  }
  throw InternalException("lindel: unexpected bounded function");
}

// The `bits` argument must be a constant because it decides the key type.
int64_t GetBitsArgument(BindFunctionInput &input, idx_t index,
                        const string &function_name) {
  if (!input.GetArguments()[index]->IsFoldable()) {
    throw InvalidInputException("%s(): bits must be a constant", function_name);
  }
  return input.GetNonNullConstant(index)
      .DefaultCastAs(LogicalType::BIGINT)
      .GetValue<int64_t>();
}

int64_t GetBitsArgument(ResolveScalarFunctionTypesInput &input, idx_t index,
                        const string &function_name) {
  if (!input.GetArgument(index).IsFoldable()) {
    throw InvalidInputException("%s(): bits must be a constant", function_name);
  }
  return input.GetNonNullConstant(index)
      .DefaultCastAs(LogicalType::BIGINT)
      .GetValue<int64_t>();
}

// The number of dimensions comes from whichever array argument has a fixed
// size, so the other arrays may be written as plain list literals such as [0,
// 0].
bool FindDimensions(ResolveScalarFunctionTypesInput &input,
                    const vector<idx_t> &array_arguments,
                    const string &function_name, idx_t &dimensions) {
  for (auto index : array_arguments) {
    auto &type = input.GetArgumentType(index);
    if (type.id() == LogicalTypeId::UNKNOWN) {
      return false;
    }
    if (type.id() == LogicalTypeId::ARRAY) {
      dimensions = ArrayType::GetSize(type);
      return true;
    }
  }
  for (auto index : array_arguments) {
    auto &type = input.GetArgumentType(index);
    if (type.id() == LogicalTypeId::LIST &&
        input.GetArgument(index).IsFoldable()) {
      auto value = input.GetConstant(index);
      if (!value.IsNull()) {
        dimensions = ListValue::GetChildren(value).size();
        return true;
      }
    }
  }
  throw InvalidInputException(
      "%s(): the coordinates must be a fixed-size array such as DOUBLE[2]",
      function_name);
}

//===--------------------------------------------------------------------===//
// {hilbert,morton}_encode_bounded(values, min_values, max_values, bits)
//===--------------------------------------------------------------------===//
void EncodeBoundedResolveTypes(ResolveScalarFunctionTypesInput &input) {
  auto &bound_function = input.GetBoundFunction();
  const auto function_name = FunctionName(bound_function);
  const bool geometry =
      input.GetArgumentType(0).id() == LogicalTypeId::GEOMETRY;
  idx_t dimensions = 2;
  if (!geometry &&
      !FindDimensions(input, {0, 1, 2}, function_name, dimensions)) {
    bound_function.SetReturnType(LogicalType::UNKNOWN);
    return;
  }
  auto bits = GetBitsArgument(input, 3, function_name);
  ValidateBoundedGrid(function_name, dimensions, bits);

  auto array_type = LogicalType::ARRAY(LogicalType::DOUBLE, dimensions);
  auto &arguments = bound_function.GetArguments();
  arguments[0] = geometry ? input.GetArgumentType(0) : array_type;
  arguments[1] = array_type;
  arguments[2] = array_type;
  arguments[3] = LogicalType::BIGINT;
  bound_function.SetReturnType(
      KeyTypeForBits(dimensions * static_cast<idx_t>(bits)));
}

unique_ptr<FunctionData> EncodeBoundedBind(BindScalarFunctionInput &input) {
  const auto function_name = FunctionName(input.GetBoundFunction());
  const bool geometry =
      input.GetArguments()[0]->GetReturnType().id() == LogicalTypeId::GEOMETRY;
  auto dimensions =
      ArrayType::GetSize(input.GetArguments()[1]->GetReturnType());
  auto bits = GetBitsArgument(input, 3, function_name);
  ValidateBoundedGrid(function_name, dimensions, bits);
  return make_uniq<BoundedBindData>(IsHilbert(function_name), dimensions,
                                    static_cast<idx_t>(bits), function_name,
                                    geometry);
}

// Readers for the three DOUBLE[n] arguments of a bounded function.
struct DoubleArrayArgument {
  explicit DoubleArrayArgument(const Vector &vector) {
    vector.ToUnifiedFormat(format);
    auto &child = ArrayVector::GetChild(vector);
    data = FlatVector::GetData<double>(child);
    child_validity = &FlatVector::Validity(child);
  }

  UnifiedVectorFormat format;
  const double *data;
  const ValidityMask *child_validity;
};

void EncodeBoundedFunction(DataChunk &args, ExpressionState &state,
                           Vector &result) {
  auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
  auto &bind_data = func_expr.BindInfo()->Cast<BoundedBindData>();
  const idx_t n = bind_data.dimensions;
  const idx_t total_bits = n * bind_data.bits;
  const auto key_mask = LowBitMask(total_bits);

  // The coordinates are either a DOUBLE[n] array or (for GEOMETRY) a WKB value.
  UnifiedVectorFormat value_format;
  const double *value_data = nullptr;
  const ValidityMask *value_child_validity = nullptr;
  const string_t *geometries = nullptr;
  args.data[0].ToUnifiedFormat(value_format);
  if (bind_data.geometry) {
    geometries = UnifiedVectorFormat::GetDataUnsafe<string_t>(value_format);
  } else {
    auto &child = ArrayVector::GetChild(args.data[0]);
    value_data = FlatVector::GetData<double>(child);
    value_child_validity = &FlatVector::Validity(child);
  }
  DoubleArrayArgument mins(args.data[1]);
  DoubleArrayArgument maxs(args.data[2]);
  uint64_t cells[16];
  double coordinates[16];

  for (idx_t i = 0; i < args.size(); i++) {
    auto value_idx = value_format.sel->get_index(i);
    auto min_idx = mins.format.sel->get_index(i);
    auto max_idx = maxs.format.sel->get_index(i);
    if (!value_format.validity.RowIsValid(value_idx) ||
        !mins.format.validity.RowIsValid(min_idx) ||
        !maxs.format.validity.RowIsValid(max_idx)) {
      FlatVector::SetNull(result, i, true);
      continue;
    }
    if (!mins.child_validity->CheckAllValid((min_idx + 1) * n, min_idx * n) ||
        !maxs.child_validity->CheckAllValid((max_idx + 1) * n, max_idx * n) ||
        (value_child_validity && !value_child_validity->CheckAllValid(
                                     (value_idx + 1) * n, value_idx * n))) {
      throw InvalidInputException("%s: arrays can not contain NULL values",
                                  bind_data.function_name);
    }

    if (bind_data.geometry) {
      auto extent = GeometryExtent::Empty();
      Geometry::GetExtent(geometries[value_idx], extent);
      if (!extent.HasXY()) {
        // Empty geometries have no position to encode.
        FlatVector::SetNull(result, i, true);
        continue;
      }
      coordinates[0] = (extent.x_min + extent.x_max) / 2;
      coordinates[1] = (extent.y_min + extent.y_max) / 2;
    } else {
      for (idx_t d = 0; d < n; d++) {
        coordinates[d] = value_data[value_idx * n + d];
      }
    }

    for (idx_t d = 0; d < n; d++) {
      const double lo = mins.data[min_idx * n + d];
      const double hi = maxs.data[max_idx * n + d];
      ValidateBounds(lo, hi, d, bind_data.function_name);
      cells[d] = QuantizeToGrid(coordinates[d], lo, hi, bind_data.bits,
                                bind_data.function_name);
    }

    auto key =
        EncodeGridPoint(bind_data.hilbert, bind_data.element_width, cells, n);
    if (key > key_mask) {
      // Coordinates below 2^bits stay in the first 2^(n*bits) keys of both
      // curves.
      throw InternalException("%s: key exceeds %llu bits",
                              bind_data.function_name, total_bits);
    }
    StoreKey(result, i, key);
  }

  if (args.size() == 1) {
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
  }
}

//===--------------------------------------------------------------------===//
// {hilbert,morton}_decode_bounded(key, min_values, max_values, bits)
//===--------------------------------------------------------------------===//
void DecodeBoundedResolveTypes(ResolveScalarFunctionTypesInput &input) {
  auto &bound_function = input.GetBoundFunction();
  const auto function_name = FunctionName(bound_function);
  idx_t dimensions;
  if (!FindDimensions(input, {1, 2}, function_name, dimensions)) {
    bound_function.SetReturnType(LogicalType::UNKNOWN);
    return;
  }
  auto bits = GetBitsArgument(input, 3, function_name);
  ValidateBoundedGrid(function_name, dimensions, bits);

  auto array_type = LogicalType::ARRAY(LogicalType::DOUBLE, dimensions);
  auto &arguments = bound_function.GetArguments();
  arguments[0] = LogicalType::UHUGEINT;
  arguments[1] = array_type;
  arguments[2] = array_type;
  arguments[3] = LogicalType::BIGINT;
  bound_function.SetReturnType(array_type);
}

unique_ptr<FunctionData> DecodeBoundedBind(BindScalarFunctionInput &input) {
  const auto function_name = FunctionName(input.GetBoundFunction());
  auto dimensions =
      ArrayType::GetSize(input.GetArguments()[1]->GetReturnType());
  auto bits = GetBitsArgument(input, 3, function_name);
  ValidateBoundedGrid(function_name, dimensions, bits);
  return make_uniq<BoundedBindData>(IsHilbert(function_name), dimensions,
                                    static_cast<idx_t>(bits), function_name);
}

void CheckKeyRange(uhugeint_t key, const uhugeint_t &key_mask,
                   const BoundedBindData &bind_data) {
  if (key > key_mask) {
    throw InvalidInputException(
        "%s: key %s is too large for %llu dimensions of %llu bits",
        bind_data.function_name, key.ToString(), bind_data.dimensions,
        bind_data.bits);
  }
}

void DecodeBoundedFunction(DataChunk &args, ExpressionState &state,
                           Vector &result) {
  auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
  auto &bind_data = func_expr.BindInfo()->Cast<BoundedBindData>();
  const idx_t n = bind_data.dimensions;
  const auto key_mask = LowBitMask(n * bind_data.bits);

  UnifiedVectorFormat key_format;
  args.data[0].ToUnifiedFormat(key_format);
  auto keys = UnifiedVectorFormat::GetData<uhugeint_t>(key_format);
  DoubleArrayArgument mins(args.data[1]);
  DoubleArrayArgument maxs(args.data[2]);

  auto result_data =
      FlatVector::GetDataMutable<double>(ArrayVector::GetChildMutable(result));
  uint64_t cells[16];

  for (idx_t i = 0; i < args.size(); i++) {
    auto key_idx = key_format.sel->get_index(i);
    auto min_idx = mins.format.sel->get_index(i);
    auto max_idx = maxs.format.sel->get_index(i);
    if (!key_format.validity.RowIsValid(key_idx) ||
        !mins.format.validity.RowIsValid(min_idx) ||
        !maxs.format.validity.RowIsValid(max_idx)) {
      FlatVector::SetNull(result, i, true);
      continue;
    }
    if (!mins.child_validity->CheckAllValid((min_idx + 1) * n, min_idx * n) ||
        !maxs.child_validity->CheckAllValid((max_idx + 1) * n, max_idx * n)) {
      throw InvalidInputException("%s: arrays can not contain NULL values",
                                  bind_data.function_name);
    }

    CheckKeyRange(keys[key_idx], key_mask, bind_data);
    DecodeGridPoint(bind_data.hilbert, bind_data.element_width, keys[key_idx],
                    n, cells);
    for (idx_t d = 0; d < n; d++) {
      const double lo = mins.data[min_idx * n + d];
      const double hi = maxs.data[max_idx * n + d];
      ValidateBounds(lo, hi, d, bind_data.function_name);
      result_data[i * n + d] = GridCellCenter(cells[d], lo, hi, bind_data.bits);
    }
  }

  if (args.size() == 1) {
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
  }
}

//===--------------------------------------------------------------------===//
// {hilbert,morton}_encode_latlon(lat, lon [, bits])
//===--------------------------------------------------------------------===//
unique_ptr<FunctionData> EncodeLatLonBind(BindScalarFunctionInput &input) {
  auto &bound_function = input.GetBoundFunction();
  const auto function_name = FunctionName(bound_function);
  auto bits = input.GetArguments().size() > 2
                  ? GetBitsArgument(input, 2, function_name)
                  : LATLON_DEFAULT_BITS;
  ValidateBoundedGrid(function_name, 2, bits);
  bound_function.SetReturnType(KeyTypeForBits(2 * static_cast<idx_t>(bits)));
  return make_uniq<BoundedBindData>(IsHilbert(function_name), 2,
                                    static_cast<idx_t>(bits), function_name);
}

void CheckCoordinate(double value, double lo, double hi, const char *name,
                     const string &function_name) {
  if (!(value >= lo && value <= hi)) {
    throw InvalidInputException("%s: %s %s is outside [%s, %s]", function_name,
                                name, std::to_string(value), std::to_string(lo),
                                std::to_string(hi));
  }
}

void EncodeLatLonFunction(DataChunk &args, ExpressionState &state,
                          Vector &result) {
  auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
  auto &bind_data = func_expr.BindInfo()->Cast<BoundedBindData>();

  UnifiedVectorFormat lat_format;
  UnifiedVectorFormat lon_format;
  args.data[0].ToUnifiedFormat(lat_format);
  args.data[1].ToUnifiedFormat(lon_format);
  auto lats = UnifiedVectorFormat::GetData<double>(lat_format);
  auto lons = UnifiedVectorFormat::GetData<double>(lon_format);

  for (idx_t i = 0; i < args.size(); i++) {
    auto lat_idx = lat_format.sel->get_index(i);
    auto lon_idx = lon_format.sel->get_index(i);
    if (!lat_format.validity.RowIsValid(lat_idx) ||
        !lon_format.validity.RowIsValid(lon_idx)) {
      FlatVector::SetNull(result, i, true);
      continue;
    }
    const double lat = lats[lat_idx];
    const double lon = lons[lon_idx];
    CheckCoordinate(lat, LATITUDE_MIN, LATITUDE_MAX, "latitude",
                    bind_data.function_name);
    CheckCoordinate(lon, LONGITUDE_MIN, LONGITUDE_MAX, "longitude",
                    bind_data.function_name);

    uint64_t cells[2] = {
        QuantizeToGrid(lat, LATITUDE_MIN, LATITUDE_MAX, bind_data.bits,
                       bind_data.function_name),
        QuantizeToGrid(lon, LONGITUDE_MIN, LONGITUDE_MAX, bind_data.bits,
                       bind_data.function_name)};
    StoreKey(
        result, i,
        EncodeGridPoint(bind_data.hilbert, bind_data.element_width, cells, 2));
  }

  if (args.size() == 1) {
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
  }
}

//===--------------------------------------------------------------------===//
// {hilbert,morton}_decode_latlon(key [, bits])
//===--------------------------------------------------------------------===//
LogicalType LatLonStructType() {
  child_list_t<LogicalType> children;
  children.emplace_back("lat", LogicalType::DOUBLE);
  children.emplace_back("lon", LogicalType::DOUBLE);
  return LogicalType::STRUCT(std::move(children));
}

unique_ptr<FunctionData> DecodeLatLonBind(BindScalarFunctionInput &input) {
  const auto function_name = FunctionName(input.GetBoundFunction());
  auto bits = input.GetArguments().size() > 1
                  ? GetBitsArgument(input, 1, function_name)
                  : LATLON_DEFAULT_BITS;
  ValidateBoundedGrid(function_name, 2, bits);
  return make_uniq<BoundedBindData>(IsHilbert(function_name), 2,
                                    static_cast<idx_t>(bits), function_name);
}

void DecodeLatLonFunction(DataChunk &args, ExpressionState &state,
                          Vector &result) {
  auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
  auto &bind_data = func_expr.BindInfo()->Cast<BoundedBindData>();
  const auto key_mask = LowBitMask(2 * bind_data.bits);

  UnifiedVectorFormat key_format;
  args.data[0].ToUnifiedFormat(key_format);
  auto keys = UnifiedVectorFormat::GetData<uhugeint_t>(key_format);

  auto &entries = StructVector::GetEntries(result);
  auto lat_data = FlatVector::GetDataMutable<double>(entries[0]);
  auto lon_data = FlatVector::GetDataMutable<double>(entries[1]);
  uint64_t cells[2];

  for (idx_t i = 0; i < args.size(); i++) {
    auto key_idx = key_format.sel->get_index(i);
    if (!key_format.validity.RowIsValid(key_idx)) {
      FlatVector::SetNull(result, i, true);
      continue;
    }
    CheckKeyRange(keys[key_idx], key_mask, bind_data);
    DecodeGridPoint(bind_data.hilbert, bind_data.element_width, keys[key_idx],
                    2, cells);
    lat_data[i] =
        GridCellCenter(cells[0], LATITUDE_MIN, LATITUDE_MAX, bind_data.bits);
    lon_data[i] =
        GridCellCenter(cells[1], LONGITUDE_MIN, LONGITUDE_MAX, bind_data.bits);
  }

  if (args.size() == 1) {
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
  }
}

void RegisterDocumented(ExtensionLoader &loader, ScalarFunctionSet set,
                        const string &description, vector<string> examples,
                        vector<string> parameter_names) {
  set.SetFallible();
  CreateScalarFunctionInfo info(std::move(set));
  FunctionDescription desc;
  desc.description = description;
  desc.examples = std::move(examples);
  desc.categories = {"spatial"};
  desc.parameter_names = std::move(parameter_names);
  info.descriptions.push_back(std::move(desc));
  loader.RegisterFunction(std::move(info));
}
} // namespace

void RegisterBoundedFunctions(ExtensionLoader &loader) {
  for (auto curve : {"hilbert", "morton"}) {
    const string prefix = curve;
    const string curve_name =
        prefix == "hilbert" ? "Hilbert" : "Morton (Z-order)";

    // encode_bounded
    {
      ScalarFunctionSet set(Identifier(prefix + "_encode_bounded"));
      ScalarFunction fn({Positional("values", LogicalType::ANY),
                         Positional("min_values", LogicalType::ANY),
                         Positional("max_values", LogicalType::ANY),
                         Positional("bits", LogicalType::BIGINT)},
                        LogicalType::ANY, EncodeBoundedFunction,
                        EncodeBoundedBind);
      fn.SetResolveTypesCallback(EncodeBoundedResolveTypes);
      set.AddFunction(fn);
      RegisterDocumented(
          loader, std::move(set),
          "Scales each coordinate from [min, max] onto a grid of 2^bits cells "
          "and encodes the cell with a " +
              curve_name +
              " curve. Returns the narrowest unsigned integer that holds "
              "dimensions * bits bits. Values "
              "outside the bounds are clamped to the edge cells; NaN is an "
              "error. Supports up to 64 bits "
              "for 1-2 dimensions, 32 for 3-4, 16 for 5-8 and 8 for 9-16. A "
              "GEOMETRY value is encoded by "
              "the centre of its bounding box (empty geometries give NULL), "
              "with 2-D bounds.",
          {prefix + "_encode_bounded([12.5, 40.25]::DOUBLE[2], [0, 0], [100, "
                    "100], 16)",
           prefix + "_encode_bounded(geom, [-180, -90], [180, 90], 16)"},
          {"values", "min_values", "max_values", "bits"});
    }

    // decode_bounded
    {
      ScalarFunctionSet set(Identifier(prefix + "_decode_bounded"));
      ScalarFunction fn({Positional("encoded_value", LogicalType::ANY),
                         Positional("min_values", LogicalType::ANY),
                         Positional("max_values", LogicalType::ANY),
                         Positional("bits", LogicalType::BIGINT)},
                        LogicalType::ANY, DecodeBoundedFunction,
                        DecodeBoundedBind);
      fn.SetResolveTypesCallback(DecodeBoundedResolveTypes);
      set.AddFunction(fn);
      RegisterDocumented(loader, std::move(set),
                         "Decodes a key produced by " + prefix +
                             "_encode_bounded() with the same bounds and bits, "
                             "returning the centre of the grid cell "
                             "as a DOUBLE array.",
                         {prefix + "_decode_bounded(" + prefix +
                          "_encode_bounded([12.5, 40.25]::DOUBLE[2], [0, 0], "
                          "[100, 100], 16), [0, 0]::DOUBLE[2], "
                          "[100, 100]::DOUBLE[2], 16)"},
                         {"encoded_value", "min_values", "max_values", "bits"});
    }

    // encode_latlon
    {
      ScalarFunctionSet set(Identifier(prefix + "_encode_latlon"));
      set.AddFunction(ScalarFunction(
          {Positional("latitude", LogicalType::DOUBLE),
           Positional("longitude", LogicalType::DOUBLE)},
          LogicalType::ANY, EncodeLatLonFunction, EncodeLatLonBind));
      set.AddFunction(ScalarFunction(
          {Positional("latitude", LogicalType::DOUBLE),
           Positional("longitude", LogicalType::DOUBLE),
           Positional("bits", LogicalType::BIGINT)},
          LogicalType::ANY, EncodeLatLonFunction, EncodeLatLonBind));
      RegisterDocumented(
          loader, std::move(set),
          "Encodes a latitude/longitude pair with a " + curve_name +
              " curve over a grid of 2^bits cells per axis (default 32, giving "
              "a UBIGINT key). Latitude "
              "must be within [-90, 90] and longitude within [-180, 180].",
          {prefix + "_encode_latlon(52.52, 13.405)",
           prefix + "_encode_latlon(52.52, 13.405, 16)"},
          {"latitude", "longitude", "bits"});
    }

    // decode_latlon
    {
      ScalarFunctionSet set(Identifier(prefix + "_decode_latlon"));
      set.AddFunction(ScalarFunction(
          {Positional("encoded_value", LogicalType::UHUGEINT)},
          LatLonStructType(), DecodeLatLonFunction, DecodeLatLonBind));
      set.AddFunction(ScalarFunction(
          {Positional("encoded_value", LogicalType::UHUGEINT),
           Positional("bits", LogicalType::BIGINT)},
          LatLonStructType(), DecodeLatLonFunction, DecodeLatLonBind));
      RegisterDocumented(loader, std::move(set),
                         "Decodes a key produced by " + prefix +
                             "_encode_latlon() with the same bits (default 32) "
                             "into the centre of its grid cell as "
                             "STRUCT(lat DOUBLE, lon DOUBLE).",
                         {prefix + "_decode_latlon(" + prefix +
                          "_encode_latlon(52.52, 13.405))"},
                         {"encoded_value", "bits"});
    }
  }
}
} // namespace duckdb
