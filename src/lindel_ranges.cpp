// Range decomposition: turn a box query into the key intervals of a
// space-filling curve.
//
// Every aligned sub-cube of the grid occupies one contiguous run of keys on
// both the Hilbert and Morton curves. Recursively splitting the grid, emitting
// cubes that fall inside the query box and splitting those that straddle its
// edge, yields a set of [lo, hi] key ranges whose union is exactly the box.
// Splitting stops after a fixed amount of work, emitting any cubes that still
// straddle the edge whole, and the ranges are then coarsened to at most
// max_ranges by closing the narrowest gaps. Either step makes the ranges a
// superset of the box (so the caller re-checks the exact predicate), but a
// point inside the box is never missed.

#include "lindel_common.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include <duckdb/parser/parsed_data/create_table_function_info.hpp>

#include <algorithm>

namespace duckdb {
namespace {
constexpr int64_t DEFAULT_MAX_RANGES = 64;
// Upper bound on the number of grid cells examined. The box is refined as far
// as this allows and the result is then coarsened to max_ranges by closing the
// narrowest gaps, which keeps the tightest cover for the budget while bounding
// the cost of wide or high-dimensional boxes.
constexpr idx_t CELL_WORK_LIMIT = idx_t(1) << 18;

struct KeyRange {
  uhugeint_t lo;
  uhugeint_t hi;
};

struct GridBox {
  bool hilbert;
  idx_t dimensions;
  // Bits per coordinate in the grid; the grid has 2^levels cells per dimension.
  idx_t levels;
  // Width of the coordinates passed to the encoder (>= levels).
  idx_t element_width;
  uint64_t lo[16];
  uint64_t hi[16];
};

struct Cell {
  uint64_t origin[16];
};

// The key run covered by the aligned cube at `origin` whose side is
// 2^side_bits.
KeyRange CubeRange(const GridBox &box, const uint64_t *origin,
                   idx_t side_bits) {
  const idx_t total_bits = box.dimensions * box.levels;
  const idx_t shift = box.dimensions * side_bits;
  if (shift >= total_bits) {
    return {uhugeint_t(uint64_t(0)), LowBitMask(total_bits)};
  }
  auto key =
      EncodeGridPoint(box.hilbert, box.element_width, origin, box.dimensions);
  auto mask = LowBitMask(shift);
  auto lo = key & ~mask;
  return {lo, lo | mask};
}

vector<KeyRange> MergeRanges(vector<KeyRange> ranges, idx_t max_ranges) {
  if (ranges.empty()) {
    return ranges;
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const KeyRange &a, const KeyRange &b) { return a.lo < b.lo; });

  // Merge overlapping and adjacent runs.
  vector<KeyRange> merged;
  merged.push_back(ranges[0]);
  const auto max_key = NumericLimits<uhugeint_t>::Maximum();
  for (idx_t i = 1; i < ranges.size(); i++) {
    auto &last = merged.back();
    if (last.hi == max_key ||
        ranges[i].lo <= last.hi + uhugeint_t(uint64_t(1))) {
      if (ranges[i].hi > last.hi) {
        last.hi = ranges[i].hi;
      }
    } else {
      merged.push_back(ranges[i]);
    }
  }
  if (merged.size() <= max_ranges) {
    return merged;
  }

  // Still over budget: keep the (max_ranges - 1) widest gaps and close the
  // rest, which adds the fewest extra keys.
  vector<idx_t> gap_order(merged.size() - 1);
  for (idx_t i = 0; i < gap_order.size(); i++) {
    gap_order[i] = i;
  }
  std::sort(gap_order.begin(), gap_order.end(), [&](idx_t a, idx_t b) {
    return (merged[a + 1].lo - merged[a].hi) >
           (merged[b + 1].lo - merged[b].hi);
  });
  vector<bool> keep_gap(merged.size() - 1, false);
  for (idx_t i = 0; i + 1 < max_ranges; i++) {
    keep_gap[gap_order[i]] = true;
  }
  vector<KeyRange> coarse;
  coarse.push_back(merged[0]);
  for (idx_t i = 1; i < merged.size(); i++) {
    if (keep_gap[i - 1]) {
      coarse.push_back(merged[i]);
    } else {
      coarse.back().hi = merged[i].hi;
    }
  }
  return coarse;
}

vector<KeyRange> ComputeRanges(const GridBox &box, idx_t max_ranges) {
  const idx_t n = box.dimensions;
  const uint64_t last_cell = box.levels >= 64
                                 ? NumericLimits<uint64_t>::Maximum()
                                 : ((uint64_t(1) << box.levels) - 1);

  vector<KeyRange> out;
  Cell root;
  bool root_inside = true;
  for (idx_t d = 0; d < n; d++) {
    root.origin[d] = 0;
    root_inside = root_inside && box.lo[d] == 0 && box.hi[d] == last_cell;
  }
  if (root_inside) {
    out.push_back(CubeRange(box, root.origin, box.levels));
    return out;
  }

  // Cells in the frontier straddle the box edge; they all have side
  // 2^side_bits.
  vector<Cell> frontier = {root};
  idx_t side_bits = box.levels;
  idx_t work = 1;

  while (!frontier.empty()) {
    // Each straddling cell has at most 2^n children.
    const idx_t children_bound = frontier.size() * (idx_t(1) << n);
    if (work + children_bound > CELL_WORK_LIMIT || side_bits == 0) {
      break;
    }
    const idx_t child_bits = side_bits - 1;
    const uint64_t half = uint64_t(1) << child_bits;

    vector<Cell> next;
    for (auto &cell : frontier) {
      // Per dimension, the child origins whose half-cell intersects the box.
      uint64_t options[16][2];
      idx_t option_count[16];
      for (idx_t d = 0; d < n; d++) {
        option_count[d] = 0;
        for (uint64_t start : {cell.origin[d], cell.origin[d] + half}) {
          const uint64_t end = start + (half - 1);
          if (start <= box.hi[d] && end >= box.lo[d]) {
            options[d][option_count[d]++] = start;
          }
        }
      }

      // Walk the cartesian product of the per-dimension options.
      idx_t choice[16] = {0};
      while (true) {
        Cell child;
        bool inside = true;
        for (idx_t d = 0; d < n; d++) {
          child.origin[d] = options[d][choice[d]];
          const uint64_t end = child.origin[d] + (half - 1);
          inside = inside && child.origin[d] >= box.lo[d] && end <= box.hi[d];
        }
        work++;
        if (inside) {
          out.push_back(CubeRange(box, child.origin, child_bits));
        } else {
          next.push_back(child);
        }

        idx_t d = 0;
        while (d < n && ++choice[d] == option_count[d]) {
          choice[d] = 0;
          d++;
        }
        if (d == n) {
          break;
        }
      }
    }
    frontier = std::move(next);
    side_bits = child_bits;
  }

  // Out of budget: cover the remaining straddling cells whole.
  for (auto &cell : frontier) {
    out.push_back(CubeRange(box, cell.origin, side_bits));
  }
  return MergeRanges(std::move(out), max_ranges);
}

//===--------------------------------------------------------------------===//
// Argument helpers
//===--------------------------------------------------------------------===//
const vector<Value> &
PointValues(const Value &value, const string &function_name, const char *name) {
  if (value.IsNull()) {
    throw InvalidInputException("%s(): %s can not be NULL", function_name,
                                name);
  }
  switch (value.type().id()) {
  case LogicalTypeId::ARRAY:
    return ArrayValue::GetChildren(value);
  case LogicalTypeId::LIST:
    return ListValue::GetChildren(value);
  default:
    throw InvalidInputException("%s(): %s must be an array, got %s",
                                function_name, name, value.type().ToString());
  }
}

LogicalType PointChildType(const Value &value) {
  return value.type().id() == LogicalTypeId::ARRAY
             ? ArrayType::GetChildType(value.type())
             : ListType::GetChildType(value.type());
}

int64_t MaxRangesArgument(const vector<Value> &inputs, idx_t index,
                          const string &function_name) {
  if (inputs.size() <= index) {
    return DEFAULT_MAX_RANGES;
  }
  if (inputs[index].IsNull()) {
    throw InvalidInputException("%s(): max_ranges can not be NULL",
                                function_name);
  }
  auto max_ranges = inputs[index].GetValue<int64_t>();
  if (max_ranges < 1) {
    throw InvalidInputException("%s(): max_ranges must be at least 1, got %lld",
                                function_name, max_ranges);
  }
  return max_ranges;
}

// The order-preserving unsigned grid coordinate of one element of a typed
// point.
uint64_t OrderedCoordinate(const Value &value, const LogicalType &type,
                           const EncodeElementInfo &info,
                           const string &function_name) {
  if (value.IsNull()) {
    throw InvalidInputException("%s(): points can not contain NULL values",
                                function_name);
  }
  auto cast = value.DefaultCastAs(type);
  uint64_t bits;
  switch (type.InternalType()) {
  case PhysicalType::BOOL:
    bits = cast.GetValueUnsafe<bool>() ? 1 : 0;
    break;
  case PhysicalType::UINT8:
    bits = cast.GetValueUnsafe<uint8_t>();
    break;
  case PhysicalType::INT8:
    bits = static_cast<uint8_t>(cast.GetValueUnsafe<int8_t>());
    break;
  case PhysicalType::UINT16:
    bits = cast.GetValueUnsafe<uint16_t>();
    break;
  case PhysicalType::INT16:
    bits = static_cast<uint16_t>(cast.GetValueUnsafe<int16_t>());
    break;
  case PhysicalType::UINT32:
    bits = cast.GetValueUnsafe<uint32_t>();
    break;
  case PhysicalType::INT32:
    bits = static_cast<uint32_t>(cast.GetValueUnsafe<int32_t>());
    break;
  case PhysicalType::UINT64:
    bits = cast.GetValueUnsafe<uint64_t>();
    break;
  case PhysicalType::INT64:
    bits = static_cast<uint64_t>(cast.GetValueUnsafe<int64_t>());
    break;
  case PhysicalType::FLOAT: {
    auto f = cast.GetValueUnsafe<float>();
    if (std::isnan(f)) {
      throw InvalidInputException("%s(): points can not contain NaN",
                                  function_name);
    }
    uint32_t raw;
    memcpy(&raw, &f, sizeof(raw));
    bits = raw;
    break;
  }
  case PhysicalType::DOUBLE: {
    auto f = cast.GetValueUnsafe<double>();
    if (std::isnan(f)) {
      throw InvalidInputException("%s(): points can not contain NaN",
                                  function_name);
    }
    memcpy(&bits, &f, sizeof(bits));
    break;
  }
  default:
    throw InternalException("%s(): unexpected coordinate type", function_name);
  }
  switch (info.bit_width) {
  case 8:
    return ToOrderedBits<uint8_t>(static_cast<uint8_t>(bits), info.transform);
  case 16:
    return ToOrderedBits<uint16_t>(static_cast<uint16_t>(bits), info.transform);
  case 32:
    return ToOrderedBits<uint32_t>(static_cast<uint32_t>(bits), info.transform);
  default:
    return ToOrderedBits<uint64_t>(bits, info.transform);
  }
}

void CheckBoxOrder(const GridBox &box, const string &function_name) {
  for (idx_t d = 0; d < box.dimensions; d++) {
    if (box.lo[d] > box.hi[d]) {
      throw InvalidInputException(
          "%s(): min_point must not exceed max_point (dimension %llu)",
          function_name, d + 1);
    }
  }
}

struct RangesBindData : public TableFunctionData {
  vector<KeyRange> ranges;
};

struct RangesState : public GlobalTableFunctionState {
  idx_t offset = 0;
};

bool IsHilbertTableFunction(const TableFunctionBindInput &input) {
  const auto &name = input.table_function.GetName();
  return name == "hilbert_ranges" || name == "hilbert_ranges_bounded";
}

//===--------------------------------------------------------------------===//
// {hilbert,morton}_ranges(min_point, max_point [, max_ranges])
//===--------------------------------------------------------------------===//
unique_ptr<FunctionData> RangesBind(ClientContext &context,
                                    TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types,
                                    vector<Identifier> &names) {
  const bool hilbert = IsHilbertTableFunction(input);
  const string function_name = hilbert ? "hilbert_ranges" : "morton_ranges";
  auto &inputs = input.inputs;

  auto &min_values = PointValues(inputs[0], function_name, "min_point");
  auto &max_values = PointValues(inputs[1], function_name, "max_point");
  auto min_type = PointChildType(inputs[0]);
  auto max_type = PointChildType(inputs[1]);
  if (min_type != max_type || min_values.size() != max_values.size()) {
    throw InvalidInputException(
        "%s(): min_point and max_point must have the same type, got %s and %s",
        function_name, inputs[0].type().ToString(),
        inputs[1].type().ToString());
  }

  EncodeElementInfo info;
  if (!GetEncodeElementInfo(min_type, info)) {
    throw InvalidInputException(
        "%s(): points must be arrays of %s values, not %s", function_name,
        SUPPORTED_ELEMENT_TYPES, min_type.ToString());
  }
  const idx_t n = min_values.size();
  const idx_t max_elements = 128 / info.bit_width;
  if (n < 1 || n > max_elements) {
    throw InvalidInputException(
        "%s(): points of %s support 1-%llu dimensions, got %llu", function_name,
        min_type.ToString(), max_elements, n);
  }

  GridBox box;
  box.hilbert = hilbert;
  box.dimensions = n;
  box.levels = info.bit_width;
  box.element_width = info.bit_width;
  for (idx_t d = 0; d < n; d++) {
    box.lo[d] = OrderedCoordinate(min_values[d], min_type, info, function_name);
    box.hi[d] = OrderedCoordinate(max_values[d], min_type, info, function_name);
  }
  CheckBoxOrder(box, function_name);

  auto result = make_uniq<RangesBindData>();
  result->ranges = ComputeRanges(
      box, static_cast<idx_t>(MaxRangesArgument(inputs, 2, function_name)));

  auto key_type = KeyTypeForBits(info.bit_width * n);
  return_types = {key_type, key_type};
  names = {Identifier("lo"), Identifier("hi")};
  return std::move(result);
}

//===--------------------------------------------------------------------===//
// {hilbert,morton}_ranges_bounded(min_point, max_point, min_values, max_values,
// bits [, max_ranges])
//===--------------------------------------------------------------------===//
vector<double> DoublePoint(const Value &value, const string &function_name,
                           const char *name) {
  vector<double> result;
  for (auto &child : PointValues(value, function_name, name)) {
    if (child.IsNull()) {
      throw InvalidInputException("%s(): %s can not contain NULL values",
                                  function_name, name);
    }
    result.push_back(
        child.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>());
  }
  return result;
}

unique_ptr<FunctionData> RangesBoundedBind(ClientContext &context,
                                           TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types,
                                           vector<Identifier> &names) {
  const bool hilbert = IsHilbertTableFunction(input);
  const string function_name =
      hilbert ? "hilbert_ranges_bounded" : "morton_ranges_bounded";
  auto &inputs = input.inputs;

  auto min_point = DoublePoint(inputs[0], function_name, "min_point");
  auto max_point = DoublePoint(inputs[1], function_name, "max_point");
  auto min_values = DoublePoint(inputs[2], function_name, "min_values");
  auto max_values = DoublePoint(inputs[3], function_name, "max_values");
  const idx_t n = min_point.size();
  if (max_point.size() != n || min_values.size() != n ||
      max_values.size() != n) {
    throw InvalidInputException(
        "%s(): all four arrays must have the same number of dimensions",
        function_name);
  }
  if (inputs[4].IsNull()) {
    throw InvalidInputException("%s(): bits can not be NULL", function_name);
  }
  auto bits = inputs[4].GetValue<int64_t>();
  auto element_width = ValidateBoundedGrid(function_name, n, bits);

  GridBox box;
  box.hilbert = hilbert;
  box.dimensions = n;
  box.levels = static_cast<idx_t>(bits);
  box.element_width = element_width;
  for (idx_t d = 0; d < n; d++) {
    ValidateBounds(min_values[d], max_values[d], d, function_name);
    box.lo[d] = QuantizeToGrid(min_point[d], min_values[d], max_values[d],
                               box.levels, function_name);
    box.hi[d] = QuantizeToGrid(max_point[d], min_values[d], max_values[d],
                               box.levels, function_name);
  }
  CheckBoxOrder(box, function_name);

  auto result = make_uniq<RangesBindData>();
  result->ranges = ComputeRanges(
      box, static_cast<idx_t>(MaxRangesArgument(inputs, 5, function_name)));

  auto key_type = KeyTypeForBits(n * box.levels);
  return_types = {key_type, key_type};
  names = {Identifier("lo"), Identifier("hi")};
  return std::move(result);
}

unique_ptr<GlobalTableFunctionState> RangesInit(ClientContext &context,
                                                TableFunctionInitInput &input) {
  return make_uniq<RangesState>();
}

void RangesFunction(ClientContext &context, TableFunctionInput &data,
                    DataChunk &output) {
  auto &bind_data = data.bind_data->Cast<RangesBindData>();
  auto &state = data.global_state->Cast<RangesState>();
  idx_t count = 0;
  while (state.offset < bind_data.ranges.size() &&
         count < STANDARD_VECTOR_SIZE) {
    auto &range = bind_data.ranges[state.offset++];
    StoreKey(output.data[0], count, range.lo);
    StoreKey(output.data[1], count, range.hi);
    count++;
  }
  output.SetChildCardinality(count);
}

void RegisterRangesSet(ExtensionLoader &loader, TableFunctionSet set,
                       const string &description, vector<string> examples,
                       vector<string> parameter_names) {
  CreateTableFunctionInfo info(std::move(set));
  FunctionDescription desc;
  desc.description = description;
  desc.examples = std::move(examples);
  desc.categories = {"spatial"};
  desc.parameter_names = std::move(parameter_names);
  info.descriptions.push_back(std::move(desc));
  loader.RegisterFunction(std::move(info));
}
} // namespace

void RegisterRangeFunctions(ExtensionLoader &loader) {
  for (auto curve : {"hilbert", "morton"}) {
    const string prefix = curve;
    const string encode = prefix + "_encode";

    {
      TableFunctionSet set(Identifier(prefix + "_ranges"));
      auto signature = FunctionSignature()
                           .AddPositionalOnly("min_point", LogicalType::ANY)
                           .AddPositionalOnly("max_point", LogicalType::ANY);
      set.AddFunction(TableFunction(Identifier(prefix + "_ranges"), signature,
                                    RangesFunction, RangesBind, RangesInit));
      set.AddFunction(TableFunction(
          Identifier(prefix + "_ranges"),
          FunctionSignature()
              .AddPositionalOnly("min_point", LogicalType::ANY)
              .AddPositionalOnly("max_point", LogicalType::ANY)
              .AddPositionalOnly("max_ranges", LogicalType::BIGINT),
          RangesFunction, RangesBind, RangesInit));
      RegisterRangesSet(
          loader, std::move(set),
          "Returns the key ranges (lo, hi) of " + encode +
              "() that cover the box between min_point and max_point "
              "(inclusive, same array type as the "
              "encoded values). With enough ranges the cover is exact; when "
              "max_ranges (default 64) is "
              "reached the ranges become a superset, so keep the exact "
              "coordinate predicate in the query. Intended for "
              "systems that store or index only the key (key-value stores, "
              "other engines, files partitioned by key); "
              "within DuckDB, filtering the coordinates of data sorted by the "
              "key is usually faster than joining "
              "against the ranges.",
          {"SELECT * FROM " + prefix +
               "_ranges([2, 3]::UTINYINT[2], [10, 12]::UTINYINT[2])",
           "SELECT p.* FROM points p, " + prefix +
               "_ranges([2, 3]::UTINYINT[2], [10, 12]::UTINYINT[2], 16) r "
               "WHERE p.key BETWEEN r.lo AND r.hi AND p.x BETWEEN 2 AND 10 AND "
               "p.y BETWEEN 3 AND 12"},
          {"min_point", "max_point", "max_ranges"});
    }

    {
      TableFunctionSet set(Identifier(prefix + "_ranges_bounded"));
      auto base = FunctionSignature()
                      .AddPositionalOnly("min_point", LogicalType::ANY)
                      .AddPositionalOnly("max_point", LogicalType::ANY)
                      .AddPositionalOnly("min_values", LogicalType::ANY)
                      .AddPositionalOnly("max_values", LogicalType::ANY)
                      .AddPositionalOnly("bits", LogicalType::BIGINT);
      set.AddFunction(TableFunction(Identifier(prefix + "_ranges_bounded"),
                                    base, RangesFunction, RangesBoundedBind,
                                    RangesInit));
      set.AddFunction(TableFunction(
          Identifier(prefix + "_ranges_bounded"),
          FunctionSignature()
              .AddPositionalOnly("min_point", LogicalType::ANY)
              .AddPositionalOnly("max_point", LogicalType::ANY)
              .AddPositionalOnly("min_values", LogicalType::ANY)
              .AddPositionalOnly("max_values", LogicalType::ANY)
              .AddPositionalOnly("bits", LogicalType::BIGINT)
              .AddPositionalOnly("max_ranges", LogicalType::BIGINT),
          RangesFunction, RangesBoundedBind, RangesInit));
      RegisterRangesSet(
          loader, std::move(set),
          "Returns the key ranges (lo, hi) of " + encode +
              "_bounded() with the given bounds and bits that cover the box "
              "between min_point and "
              "max_point. Ranges are exact at grid-cell resolution until "
              "max_ranges (default 64) is "
              "reached, then a superset; keep the exact coordinate predicate "
              "in the query. Intended for systems that "
              "store or index only the key; within DuckDB, filtering the "
              "coordinates of key-sorted data is usually faster.",
          {"SELECT * FROM " + prefix +
           "_ranges_bounded([10, 10], [20, 30], [0, 0], [100, 100], 16)"},
          {"min_point", "max_point", "min_values", "max_values", "bits",
           "max_ranges"});
    }
  }
}
} // namespace duckdb
