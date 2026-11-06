// hilbert_decode(key, 'TYPE[n]') / morton_decode(key, 'TYPE[n]'): decode
// straight into a named type.
//
// The target type determines the coordinate width and the order-preserving
// transform, so this form decodes every type the encoders accept (dates,
// timestamps, decimals, enums, ...) without the num_elements / return_float /
// return_unsigned flags of the original signature.

#include "lindel_common.hpp"

#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/unified_vector_format.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {
namespace {
struct TypedDecodeTarget {
  LogicalType type;
  LogicalType element_type;
  idx_t dimensions;
  EncodeElementInfo info;
};

string DecodeFunctionName(const BoundScalarFunction &function) {
  return function.GetName() == "hilbert_decode" ? "hilbert_decode"
                                                : "morton_decode";
}

// Parse and validate the constant type argument.
template <class INPUT>
TypedDecodeTarget ParseTarget(INPUT &input, const Expression &type_argument,
                              const string &function_name) {
  if (!type_argument.IsFoldable()) {
    throw InvalidInputException(
        "%s(): the target type must be a constant string such as 'DOUBLE[2]'",
        function_name);
  }
  auto type_name = input.GetNonNullConstant(1).ToString();

  TypedDecodeTarget target;
  target.type =
      TransformStringToLogicalType(type_name, input.GetClientContext());
  if (target.type.id() == LogicalTypeId::ARRAY) {
    target.element_type = ArrayType::GetChildType(target.type);
    target.dimensions = ArrayType::GetSize(target.type);
  } else {
    target.element_type = target.type;
    target.dimensions = 1;
  }
  if (!GetEncodeElementInfo(target.element_type, target.info)) {
    throw InvalidInputException(
        "%s(): the target type must be a %s type or a fixed-size array of one, "
        "not %s",
        function_name, SUPPORTED_ELEMENT_TYPES, target.type.ToString());
  }
  const idx_t max_elements = 128 / target.info.bit_width;
  if (target.dimensions < 1 || target.dimensions > max_elements) {
    throw InvalidInputException(
        "%s(): %s keys hold 1-%llu dimensions, got %llu", function_name,
        target.element_type.ToString(), max_elements, target.dimensions);
  }
  return target;
}

struct TypedDecodeBindData : public FunctionData {
  TypedDecodeBindData(bool hilbert_p, TypedDecodeTarget target_p,
                      string function_name_p)
      : hilbert(hilbert_p), target(std::move(target_p)),
        function_name(std::move(function_name_p)) {}

  bool hilbert;
  TypedDecodeTarget target;
  string function_name;

  unique_ptr<FunctionData> Copy() const override {
    return make_uniq<TypedDecodeBindData>(hilbert, target, function_name);
  }

  bool Equals(const FunctionData &other_p) const override {
    auto &other = other_p.Cast<TypedDecodeBindData>();
    return hilbert == other.hilbert && target.type == other.target.type;
  }
};

void TypedDecodeResolveTypes(ResolveScalarFunctionTypesInput &input) {
  auto &bound_function = input.GetBoundFunction();
  const auto function_name = DecodeFunctionName(bound_function);
  auto target = ParseTarget(input, input.GetArgument(1), function_name);

  auto &arguments = bound_function.GetArguments();
  arguments[0] = LogicalType::UHUGEINT;
  arguments[1] = LogicalType::VARCHAR;
  bound_function.SetReturnType(target.type);
}

unique_ptr<FunctionData> TypedDecodeBind(BindScalarFunctionInput &input) {
  const auto function_name = DecodeFunctionName(input.GetBoundFunction());
  auto target = ParseTarget(input, *input.GetArguments()[1], function_name);
  return make_uniq<TypedDecodeBindData>(function_name == "hilbert_decode",
                                        std::move(target), function_name);
}

uint64_t RestoreCoordinate(uint64_t ordered, const EncodeElementInfo &info) {
  switch (info.bit_width) {
  case 8:
    return FromOrderedBits<uint8_t>(static_cast<uint8_t>(ordered),
                                    info.transform);
  case 16:
    return FromOrderedBits<uint16_t>(static_cast<uint16_t>(ordered),
                                     info.transform);
  case 32:
    return FromOrderedBits<uint32_t>(static_cast<uint32_t>(ordered),
                                     info.transform);
  default:
    return FromOrderedBits<uint64_t>(ordered, info.transform);
  }
}

void TypedDecodeFunction(DataChunk &args, ExpressionState &state,
                         Vector &result) {
  auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
  auto &bind_data = func_expr.BindInfo()->Cast<TypedDecodeBindData>();
  auto &target = bind_data.target;
  const idx_t n = target.dimensions;
  const idx_t width_bytes = target.info.bit_width / 8;
  const auto key_mask = LowBitMask(target.info.bit_width * n);
  const bool is_array = target.type.id() == LogicalTypeId::ARRAY;
  const auto element_id = target.element_type.id();
  const uint64_t enum_size = element_id == LogicalTypeId::ENUM
                                 ? EnumType::GetSize(target.element_type)
                                 : 0;

  UnifiedVectorFormat key_format;
  args.data[0].ToUnifiedFormat(key_format);
  auto keys = UnifiedVectorFormat::GetData<uhugeint_t>(key_format);

  // Written through the physical storage of the target type.
  auto &values = is_array ? ArrayVector::GetChildMutable(result) : result;
  auto out = FlatVector::GetDataMutable(values);
  uint64_t coordinates[16];

  for (idx_t i = 0; i < args.size(); i++) {
    auto key_idx = key_format.sel->get_index(i);
    if (!key_format.validity.RowIsValid(key_idx)) {
      FlatVector::SetNull(result, i, true);
      continue;
    }
    const auto key = keys[key_idx];
    if (key > key_mask) {
      throw InvalidInputException("%s: key %s is too large for %s",
                                  bind_data.function_name, key.ToString(),
                                  target.type.ToString());
    }
    DecodeGridPoint(bind_data.hilbert, target.info.bit_width, key, n,
                    coordinates);
    for (idx_t d = 0; d < n; d++) {
      const uint64_t raw = RestoreCoordinate(coordinates[d], target.info);
      if ((element_id == LogicalTypeId::BOOLEAN && raw > 1) ||
          (element_id == LogicalTypeId::ENUM && raw >= enum_size)) {
        throw InvalidInputException("%s: key %s does not decode to a valid %s",
                                    bind_data.function_name, key.ToString(),
                                    target.type.ToString());
      }
      // Little-endian: the low bytes of raw are the coordinate.
      memcpy(out + (i * n + d) * width_bytes, &raw, width_bytes);
    }
  }

  if (args.size() == 1) {
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
  }
}
} // namespace

void AddTypedDecodeOverloads(ScalarFunctionSet &set) {
  ScalarFunction fn({Positional("encoded_value", LogicalType::ANY),
                     Positional("target_type", LogicalType::VARCHAR)},
                    LogicalType::ANY, TypedDecodeFunction, TypedDecodeBind);
  fn.SetResolveTypesCallback(TypedDecodeResolveTypes);
  set.AddFunction(fn);
}
} // namespace duckdb
