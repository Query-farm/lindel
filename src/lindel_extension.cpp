#define DUCKDB_EXTENSION_MAIN

#include "lindel_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// Include the declarations of things from Rust.
#include "rust.h"
#include "query_farm_telemetry.hpp"
namespace duckdb
{

    // Since we have functions that can decode or encode using two different types of encoding to reduce
    // the number of functions we need to write we'll use a single function to handle both.
    // and just store the encoding type in the bind_info.
    //
    // The encoding type is 0 for Hilbert and 1 for Morton.
    //
    // This extension supports two different types of encoding, Hilbert and Morton.
    //
    // In both cases the encoding is done in a similar way, the only difference is the
    // encoding function that is called.
    //
    // Rather than writing two separate functions for each encoding type we'll write a single
    // function that can handle both and just store the encoding type in the bind_info object.
    //
    // The bind_info object is created before the functions are called but when DuckDB starts to evaluate
    // the expression.
    struct lindelEncodingBindData : public FunctionData
    {
        uint8_t encoding_type;
        lindelEncodingBindData(uint8_t encoding_type_p) : FunctionData(), encoding_type(encoding_type_p)
        {
        }

        duckdb::unique_ptr<FunctionData> Copy() const override
        {
            return make_uniq<lindelEncodingBindData>(encoding_type);
        }

        bool Equals(const FunctionData &other_p) const override
        {
            auto &other = other_p.Cast<lindelEncodingBindData>();
            return encoding_type == other.encoding_type;
        }
    };

    // This is the "bind" fucntion that is called when we are decoding an array of values.
    //
    // In SQL this will be a function of the form:
    //
    // hilbert_decode(UTINYINT|USMALLINT|UINTEGER|UBIGINT|UHUGEINT, TINYINT, BOOLEAN)
    // morton_decode(UTINYINT|USMALLINT|UINTEGER|UBIGINT|UHUGEINT, TINYINT, BOOLEAN)
    //
    // The arguments are as follows:
    //
    // 1. The value to decode.
    // 2. The number of parts to return.
    // 3. Whether or not to return the parts as floats or integers.
    // 4. Whether or not to return unsigned integers (true if unsigned)
    //
    // This binding function also needs to determine the encoding type by looking at the bound function name.
    //
    // This function also determines the actual type that will be returned by the function, it will always be an array
    // but the type of element and number of elements will depend on the input type and what the caller requests.
    //
    static unique_ptr<FunctionData> lindelDecodeToArrayBind(ClientContext &context, ScalarFunction &bound_function,
                                                            vector<unique_ptr<Expression>> &arguments)
    {
        unique_ptr<lindelEncodingBindData> bind_data = make_uniq<lindelEncodingBindData>(0);
        if (bound_function.name == "hilbert_decode")
        {
            bind_data->encoding_type = 0;
        }
        else if (bound_function.name == "morton_decode")
        {
            bind_data->encoding_type = 1;
        }
        else
        {
            throw NotImplementedException("Unknown function name in lindelDecodeToArrayBind, expected either hilbert_decode() or morton_decode()");
        }

        auto &left_type = arguments[0]->return_type;

        auto get_foldable_value = [&](size_t index, LogicalType expected_type, const string &error_msg) -> Value
        {
            if (!arguments[index]->IsFoldable())
            {
                throw NotImplementedException(error_msg);
            }
            Value val = ExpressionExecutor::EvaluateScalar(context, *arguments[index]).CastAs(context, expected_type);
            if (val.IsNull())
            {
                throw NotImplementedException(error_msg + " expected a not-null value");
            }
            return val;
        };

        auto return_number_of_parts = UTinyIntValue::Get(get_foldable_value(1, LogicalType::UTINYINT, "hilbert_decode(ANY, TINYINT, BOOLEAN, BOOLEAN)"));
        auto return_float = BooleanValue::Get(get_foldable_value(2, LogicalType::BOOLEAN, "hilbert_decode(ANY, TINYINT, BOOLEAN, BOOLEAN)"));
        auto return_unsigned = BooleanValue::Get(get_foldable_value(3, LogicalType::BOOLEAN, "hilbert_decode(ANY, TINYINT, BOOLEAN, BOOLEAN)"));

        if (return_number_of_parts == 0)
        {
            throw InvalidInputException("Number of parts to return must be greater than 0.");
        }

        auto set_return_type = [&](LogicalType base_type, size_t parts, string_t allowed_types, const vector<LogicalType> &type_options)
        {
            if (find(type_options.begin(), type_options.end(), left_type.id()) == type_options.end())
            {
                throw InvalidInputException("Expected one of the following types:" + allowed_types.GetString());
            }
            bound_function.return_type = LogicalType::ARRAY(base_type, parts);
        };

        if (return_float)
        {
            switch (left_type.id())
            {
            case LogicalType::UINTEGER:
                set_return_type(LogicalType::FLOAT, 1, "UINTEGER", {LogicalType::UINTEGER});
                break;
            case LogicalType::UBIGINT:
                if (return_number_of_parts == 1)
                {
                    set_return_type(LogicalType::DOUBLE, 1, "UBIGINT", {LogicalType::UBIGINT});
                }
                else if (return_number_of_parts == 2)
                {
                    set_return_type(LogicalType::FLOAT, 2, "UBIGINT", {LogicalType::UBIGINT});
                }
                else
                {
                    throw InvalidInputException("Expected 1 or 2 parts for UBIGINT");
                }
                break;
            case LogicalType::UHUGEINT:
                if (return_number_of_parts == 2)
                {
                    set_return_type(LogicalType::DOUBLE, 2, "UHUGEINT", {LogicalType::UHUGEINT});
                }
                else if (return_number_of_parts >= 3 && return_number_of_parts <= 4)
                {
                    set_return_type(LogicalType::FLOAT, return_number_of_parts, "UHUGEINT", {LogicalType::UHUGEINT});
                }
                else
                {
                    throw InvalidInputException("Expected 2-4 parts for UHUGEINT");
                }
                break;
            default:
                throw InvalidInputException("Expected UINTEGER, UBIGINT, or UHUGEINT");
            }
            return bind_data;
        }

        if (return_number_of_parts == 1)
        {
            set_return_type(left_type.id(), 1, "UINTEGER, USMALLINT, UTINYINT, UBIGINT, UHUGEINT", {
                                                                                                       (return_unsigned ? LogicalType::UINTEGER : LogicalType::INTEGER),
                                                                                                       (return_unsigned ? LogicalType::USMALLINT : LogicalType::SMALLINT),
                                                                                                       (return_unsigned ? LogicalType::UTINYINT : LogicalType::TINYINT),
                                                                                                       (return_unsigned ? LogicalType::UBIGINT : LogicalType::BIGINT),
                                                                                                   });
            return bind_data;
        }

        auto set_integer_return_type = [&](LogicalType base_type, size_t parts, string_t allowed_types, string_t bounds, const map<size_t, LogicalType> &type_map)
        {
            if (type_map.find(return_number_of_parts) != type_map.end())
            {
                set_return_type(type_map.at(return_number_of_parts), return_number_of_parts, allowed_types, {base_type});
            }
            else
            {
                throw InvalidInputException("Expected " + bounds.GetString() + " parts for " + base_type.ToString());
            }
        };

        // The number of parts in the output array is determined by the number of parts requested and the datatype passed
        // to decode.

        switch (left_type.id())
        {
        case LogicalType::UTINYINT:
            throw InvalidInputException("Expected 1 parts for UTINYINT");
        case LogicalType::USMALLINT:
            set_integer_return_type(LogicalType::USMALLINT, return_number_of_parts, "UTINYINT", "2", {{2, return_unsigned ? LogicalType::UTINYINT : LogicalType::TINYINT}});
            break;
        case LogicalType::UINTEGER:
            set_integer_return_type(LogicalType::UINTEGER, return_number_of_parts, "UTINYINT, USMALLINT", "2-4", {{2, (return_unsigned ? LogicalType::USMALLINT : LogicalType::SMALLINT)}, {3, (return_unsigned ? LogicalType::UTINYINT : LogicalType::TINYINT)}});
            break;
        case LogicalType::UBIGINT:
            set_integer_return_type(LogicalType::UBIGINT, return_number_of_parts, "UTINYINT, USMALLINT, UINTEGER", "2-8", {{2, (return_unsigned ? LogicalType::UINTEGER : LogicalType::INTEGER)}, {3, (return_unsigned ? LogicalType::USMALLINT : LogicalType::SMALLINT)}, {4, (return_unsigned ? LogicalType::USMALLINT : LogicalType::SMALLINT)}, {5, (return_unsigned ? LogicalType::UTINYINT : LogicalType::TINYINT)}, {6, (return_unsigned ? LogicalType::UTINYINT : LogicalType::TINYINT)}, {7, (return_unsigned ? LogicalType::UTINYINT : LogicalType::TINYINT)}, {8, (return_unsigned ? LogicalType::UTINYINT : LogicalType::TINYINT)}});
            break;
        case LogicalType::UHUGEINT:
            set_integer_return_type(LogicalType::UHUGEINT, return_number_of_parts, "UTINYINT, USMALLINT, UINTEGER, UBIGINT", "2-16", {{2, (return_unsigned ? LogicalType::UBIGINT : LogicalType::BIGINT)}, {3, (return_unsigned ? LogicalType::UINTEGER : LogicalType::INTEGER)}, {4, (return_unsigned ? LogicalType::UINTEGER : LogicalType::INTEGER)}, {5, (return_unsigned ? LogicalType::USMALLINT : LogicalType::SMALLINT)}, {6, (return_unsigned ? LogicalType::USMALLINT : LogicalType::SMALLINT)}, {7, (return_unsigned ? LogicalType::USMALLINT : LogicalType::SMALLINT)}, {8, (return_unsigned ? LogicalType::USMALLINT : LogicalType::SMALLINT)}, {9, (return_unsigned ? LogicalType::UTINYINT : LogicalType::TINYINT)}, {10, (return_unsigned ? LogicalType::UTINYINT : LogicalType::TINYINT)}, {11, (return_unsigned ? LogicalType::UTINYINT : LogicalType::TINYINT)}, {12, (return_unsigned ? LogicalType::UTINYINT : LogicalType::TINYINT)}, {13, (return_unsigned ? LogicalType::UTINYINT : LogicalType::TINYINT)}, {14, (return_unsigned ? LogicalType::UTINYINT : LogicalType::TINYINT)}, {15, (return_unsigned ? LogicalType::UTINYINT : LogicalType::TINYINT)}, {16, (return_unsigned ? LogicalType::UTINYINT : LogicalType::TINYINT)}});
            break;
        default:
            throw InvalidInputException("Expected UINTEGER, USMALLINT, UTINYINT, UBIGINT, or UHUGEINT");
        }

        return bind_data;
    }

    // Struct to hold input data information
    struct DecodeInputDataInfo
    {
        void *data_ptr;
        size_t increment;

        DecodeInputDataInfo(void *ptr, size_t inc) : data_ptr(ptr), increment(inc) {}
    };

    // Struct to hold output data information
    struct DecodeOutputDataInfo
    {
        void *data_ptr;
        size_t increment;
        uint8_t bit_width;

        DecodeOutputDataInfo(void *ptr, size_t inc, uint8_t width)
            : data_ptr(ptr), increment(inc), bit_width(width) {}
    };

    // Helper function to get input data pointer and increment size
    inline DecodeInputDataInfo getDecodeInputDataInfo(Vector &input_vector)
    {
        void *data_ptr = NULL;
        size_t increment = 0;

        switch (input_vector.GetType().id())
        {
        case LogicalTypeId::UTINYINT:
            data_ptr = FlatVector::GetData<uint8_t>(input_vector);
            increment = sizeof(uint8_t);
            break;
        case LogicalTypeId::TINYINT:
            data_ptr = FlatVector::GetData<int8_t>(input_vector);
            increment = sizeof(int8_t);
            break;
        case LogicalTypeId::USMALLINT:
            data_ptr = FlatVector::GetData<uint16_t>(input_vector);
            increment = sizeof(uint16_t);
            break;
        case LogicalTypeId::SMALLINT:
            data_ptr = FlatVector::GetData<int16_t>(input_vector);
            increment = sizeof(int16_t);
            break;
        case LogicalTypeId::UINTEGER:
            data_ptr = FlatVector::GetData<uint32_t>(input_vector);
            increment = sizeof(uint32_t);
            break;
        case LogicalTypeId::INTEGER:
            data_ptr = FlatVector::GetData<int32_t>(input_vector);
            increment = sizeof(int32_t);
            break;
        case LogicalTypeId::UBIGINT:
            data_ptr = FlatVector::GetData<uint64_t>(input_vector);
            increment = sizeof(uint64_t);
            break;
        case LogicalTypeId::BIGINT:
            data_ptr = FlatVector::GetData<int64_t>(input_vector);
            increment = sizeof(int64_t);
            break;
        case LogicalTypeId::UHUGEINT:
            data_ptr = FlatVector::GetData<uhugeint_t>(input_vector);
            increment = sizeof(uhugeint_t);
            break;
        case LogicalTypeId::HUGEINT:
            data_ptr = FlatVector::GetData<hugeint_t>(input_vector);
            increment = sizeof(hugeint_t);
            break;
        default:
            throw NotImplementedException("hilbert_decode()/morton_decode() only supports integer input types");
        }

        return DecodeInputDataInfo(data_ptr, increment);
    }

    // Helper function to get output data pointer, increment size, and bit width
    inline DecodeOutputDataInfo getDecodeOutputDataInfo(Vector &result, const LogicalType &output_child_type)
    {
        auto &result_data_children = ArrayVector::GetEntry(result);
        void *data_ptr = NULL;
        size_t increment = 0;
        uint8_t bit_width = 0;

        switch (output_child_type.id())
        {
        case LogicalTypeId::UTINYINT:
            data_ptr = FlatVector::GetData<uint8_t>(result_data_children);
            increment = sizeof(uint8_t);
            bit_width = 8;
            break;
        case LogicalTypeId::TINYINT:
            data_ptr = FlatVector::GetData<int8_t>(result_data_children);
            increment = sizeof(int8_t);
            bit_width = 8;
            break;
        case LogicalTypeId::USMALLINT:
            data_ptr = FlatVector::GetData<uint16_t>(result_data_children);
            increment = sizeof(uint16_t);
            bit_width = 16;
            break;
        case LogicalTypeId::SMALLINT:
            data_ptr = FlatVector::GetData<int16_t>(result_data_children);
            increment = sizeof(int16_t);
            bit_width = 16;
            break;
        case LogicalTypeId::UINTEGER:
            data_ptr = FlatVector::GetData<uint32_t>(result_data_children);
            increment = sizeof(uint32_t);
            bit_width = 32;
            break;
        case LogicalTypeId::INTEGER:
            data_ptr = FlatVector::GetData<int32_t>(result_data_children);
            increment = sizeof(int32_t);
            bit_width = 32;
            break;
        case LogicalTypeId::FLOAT:
            data_ptr = FlatVector::GetData<float>(result_data_children);
            increment = sizeof(float);
            bit_width = 32;
            break;
        case LogicalTypeId::UBIGINT:
            data_ptr = FlatVector::GetData<uint64_t>(result_data_children);
            increment = sizeof(uint64_t);
            bit_width = 64;
            break;
        case LogicalTypeId::BIGINT:
            data_ptr = FlatVector::GetData<int64_t>(result_data_children);
            increment = sizeof(int64_t);
            bit_width = 64;
            break;
        case LogicalTypeId::DOUBLE:
            data_ptr = FlatVector::GetData<double>(result_data_children); // Fixed: was int64_t
            increment = sizeof(double);
            bit_width = 64;
            break;
        case LogicalTypeId::UHUGEINT:
            data_ptr = FlatVector::GetData<uhugeint_t>(result_data_children);
            increment = sizeof(uhugeint_t);
            bit_width = 128;
            break;
        case LogicalTypeId::HUGEINT:
            data_ptr = FlatVector::GetData<hugeint_t>(result_data_children);
            increment = sizeof(hugeint_t);
            bit_width = 128;
            break;
        default:
            throw NotImplementedException("hilbert_decode()/morton_decode() only supports specified output types");
        }

        return DecodeOutputDataInfo(data_ptr, increment, bit_width);
    }

    // Helper function to process all rows
    inline void decodeRows(const DataChunk &args, const UnifiedVectorFormat &left_format,
                           const lindelEncodingBindData &bind_info, const DecodeInputDataInfo &input_info,
                           const DecodeOutputDataInfo &output_info, idx_t output_number_of_elements, Vector &result)
    {
        for (idx_t i = 0; i < args.size(); i++)
        {
            auto left_idx = left_format.sel->get_index(i);

            // If the input value is NULL then the output value should be NULL.
            if (!left_format.validity.RowIsValid(left_idx))
            {
                FlatVector::SetNull(result, i, true);
                continue;
            }

            // Calculate memory locations for this row
            auto result_offset = i * output_number_of_elements;
            void *output_location = (char *)output_info.data_ptr + result_offset * output_info.increment;
            void *source_location = (char *)input_info.data_ptr + left_idx * input_info.increment;

            // Perform the actual decoding
            perform_decode(bind_info.encoding_type, output_info.bit_width, source_location,
                           output_location, output_number_of_elements);
        }
    }

    // This function performs the actual decoding of values as a DuckDB scalar function.
    //
    inline void lindelDecodeArrayFun(DataChunk &args, ExpressionState &state, Vector &result)
    {
        // This is the number of elements in the output array, not the number of rows being processed.
        auto output_number_of_elements = ArrayType::GetSize(result.GetType());

        // The type of the elements in the output array this will either be an integer type or a float type.
        auto output_child_type = ArrayType::GetChildType(result.GetType());

        // Get a reference to the bind data that was already created that will determine the type
        // of encoding to use.
        auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
        auto &bind_info = func_expr.bind_info->Cast<lindelEncodingBindData>();

        // Reference the source data.
        auto &left = args.data[0];

        // Standardize the vectors to a unified format, so it can be iterated.
        UnifiedVectorFormat left_format;
        left.ToUnifiedFormat(args.size(), left_format);

        // Get typed pointers and metadata for input and output
        DecodeInputDataInfo input_info = getDecodeInputDataInfo(left);
        DecodeOutputDataInfo output_info = getDecodeOutputDataInfo(result, output_child_type);

        // Process each row
        decodeRows(args, left_format, bind_info, input_info, output_info,
                   output_number_of_elements, result);

        // Optimize for single-element case
        if (args.size() == 1)
        {
            result.SetVectorType(VectorType::CONSTANT_VECTOR);
        }
    }

    // This is the "bind" function that is called for encoding an array of values.
    //
    // It doesn't have to do anything with the return type right now but it may in the future.
    static unique_ptr<FunctionData> lindelEncodeArrayBind(ClientContext &context, ScalarFunction &bound_function,
                                                          vector<unique_ptr<Expression>> &arguments)
    {
        unique_ptr<lindelEncodingBindData> bind_data = make_uniq<lindelEncodingBindData>(0);
        if (bound_function.name == "hilbert_encode")
        {
            bind_data->encoding_type = 0;
        }
        else if (bound_function.name == "morton_encode")
        {
            bind_data->encoding_type = 1;
        }
        else
        {
            throw NotImplementedException("Unknown function name in lindelEncodeBind");
        }

        // Now deal with validating the input type
        auto &left_type = arguments[0]->return_type;

        // This is the number of elements in the output array, not the number of rows being procssed.
        auto input_number_of_elements = ArrayType::GetSize(left_type);

        // The type of the elements in the output array this will either be an integer type or a float type.
        auto input_child_type = ArrayType::GetChildType(left_type);

        switch (input_child_type.id())
        {
        case LogicalTypeId::DOUBLE:
        {
            switch (input_number_of_elements)
            {
            case 1:
                bound_function.return_type = LogicalType::UBIGINT;
                break;
            case 2:
                bound_function.return_type = LogicalType::UHUGEINT;
                break;
            default:
                throw InvalidInputException("hilbert_encode()/morton_encode() only supports arrays of lengths of 1 or 2 for DOUBLE.");
            }
        }
        break;
        case LogicalTypeId::FLOAT:
        {
            switch (input_number_of_elements)
            {
            case 1:
                bound_function.return_type = LogicalType::UINTEGER;
                break;
            case 2:
                bound_function.return_type = LogicalType::UBIGINT;
                break;
            case 3:
            case 4:
                bound_function.return_type = LogicalType::UHUGEINT;
                break;
            default:
                throw InvalidInputException("hilbert_encode()/morton_encode() only supports arrays of lengths 1-4 for FLOAT.");
            }
        }
        break;
        case LogicalTypeId::UBIGINT:
        case LogicalTypeId::BIGINT:
        {
            switch (input_number_of_elements)
            {
            case 1:
                bound_function.return_type = LogicalType::UBIGINT;
                break;
            case 2:
                bound_function.return_type = LogicalType::UHUGEINT;
                break;
            default:
                throw InvalidInputException("hilbert_encode()/morton_encode() only supports arrays of lengths of 1 or 2 for BIGINT/UBIGINT.");
            }
        }
        break;
        case LogicalTypeId::UINTEGER:
        case LogicalTypeId::INTEGER:
        {
            switch (input_number_of_elements)
            {
            case 1:
                bound_function.return_type = LogicalType::UINTEGER;
                break;
            case 2:
                bound_function.return_type = LogicalType::UBIGINT;
                break;
            case 3:
            case 4:
                bound_function.return_type = LogicalType::UHUGEINT;
                break;
            default:
                throw InvalidInputException("hilbert_encode()/morton_encode() only supports arrays of lengths 1-4 for UINTEGER/INTEGER.");
            }
        }
        break;
        case LogicalTypeId::USMALLINT:
        case LogicalTypeId::SMALLINT:
        {
            switch (input_number_of_elements)
            {
            case 1: // 16
                bound_function.return_type = LogicalType::USMALLINT;
                break;
            case 2: // 32
                bound_function.return_type = LogicalType::UINTEGER;
                break;
            case 3:
            case 4:
                bound_function.return_type = LogicalType::UBIGINT;
                break;
            case 5:
            case 6:
            case 7:
            case 8:
                bound_function.return_type = LogicalType::UHUGEINT;
                break;
            default:
                throw InvalidInputException("hilbert_encode()/morton_encode() only supports arrays of lengths 1-8 for USMALLINT/SMALLINT.");
            }
        }
        break;
        case LogicalTypeId::UTINYINT:
        case LogicalTypeId::TINYINT:
        {
            switch (input_number_of_elements)
            {
            case 1:
                bound_function.return_type = LogicalType::UTINYINT;
                break;
            case 2:
                bound_function.return_type = LogicalType::USMALLINT;
                break;
            case 3:
            case 4:
                bound_function.return_type = LogicalType::UINTEGER;
                break;
            case 5:
            case 6:
            case 7:
            case 8:
                bound_function.return_type = LogicalType::UBIGINT;
                break;
            case 9:
            case 10:
            case 11:
            case 12:
            case 13:
            case 14:
            case 15:
            case 16:
                bound_function.return_type = LogicalType::UHUGEINT;
                break;
            default:
                throw InvalidInputException("hilbert_encode()/morton_encode() only supports arrays of lengths 1-16 for UTINYINT/TINYINT.");
            }
        }
        break;
        default:
            throw InvalidInputException("hilbert_encode()/morton_encode() only supports arrays of types DOUBLE, FLOAT, UBIGINT, BIGINT, UINTEGER, INTEGER, USMALLINT, SMALLINT, UTINYINT, TINYINT");
        }

        return bind_data;
    }

    // Perform encoding for an array of values.
    inline void lindelEncodeArrayFunc(DataChunk &args, ExpressionState &state, Vector &result)
    {
        // Get a reference to the bind data.
        auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
        auto &bind_info = func_expr.bind_info->Cast<lindelEncodingBindData>();

        // This is the size of the array
        auto array_number_of_elements = ArrayType::GetSize(args.data[0].GetType());
        auto child_type = ArrayType::GetChildType(args.data[0].GetType());

        // Get a pointer to the input data.
        auto left = args.data[0];
        auto &left_child = ArrayVector::GetEntry(left);
        auto &left_child_validity = FlatVector::Validity(left_child);
        UnifiedVectorFormat left_format;

        left.ToUnifiedFormat(args.size(), left_format);

        for (idx_t i = 0; i < args.size(); i++)
        {
            auto left_idx = left_format.sel->get_index(i);
            if (!left_format.validity.RowIsValid(left_idx))
            {
                FlatVector::SetNull(result, i, true);
                continue;
            }

            auto left_offset = left_idx * array_number_of_elements;
            if (!left_child_validity.CheckAllValid(left_offset + array_number_of_elements, left_offset))
            {
                throw InvalidInputException(StringUtil::Format("%s: array can not contain NULL values", "hilbert_encode"));
            }

            switch (child_type.id())
            {
            case LogicalTypeId::DOUBLE:
            {
                auto encoder = bind_info.encoding_type == 0 ? hilbert_encode_u64_var : morton_encode_u64_var;
                switch (array_number_of_elements)
                {
                case 1:
                {
                    auto left_data_double = FlatVector::GetData<double_t>(left_child);
                    auto result_data_u64 = FlatVector::GetData<uint64_t>(result);

                    encoder((uint64_t *)(left_data_double + left_offset), array_number_of_elements, result_data_u64 + i);
                    break;
                }
                case 2:
                {
                    auto left_data_double = FlatVector::GetData<double_t>(left_child);
                    auto result_data_u128 = FlatVector::GetData<uhugeint_t>(result);

                    encoder((uint64_t *)(left_data_double + left_offset), array_number_of_elements, result_data_u128 + i);
                    break;
                }
                default:
                    throw NotImplementedException("hilbert_encode()/morton_encode() only supports arrays of lengths of 1 or 2 for DOUBLE.");
                }
            }
            break;
            case LogicalTypeId::FLOAT:
            {
                // The number of elements in the array dictates the output type.
                auto encoder = bind_info.encoding_type == 0 ? hilbert_encode_u32_var : morton_encode_u32_var;
                switch (array_number_of_elements)
                {
                case 1:
                {
                    auto left_data_float = FlatVector::GetData<float_t>(left_child);
                    auto result_data_u32 = FlatVector::GetData<uint32_t>(result);

                    encoder((uint32_t *)(left_data_float + left_offset), array_number_of_elements, result_data_u32 + i);
                    break;
                }
                case 2:
                case 3:
                {
                    auto left_data_float = FlatVector::GetData<float_t>(left_child);
                    auto result_data_u64 = FlatVector::GetData<uint64_t>(result);

                    encoder((uint32_t *)(left_data_float + left_offset), array_number_of_elements, result_data_u64 + i);
                    break;
                }
                case 4:
                {
                    auto left_data_float = FlatVector::GetData<float_t>(left_child);
                    auto result_data_u128 = FlatVector::GetData<uhugeint_t>(result);

                    hilbert_encode_u32_var((uint32_t *)(left_data_float + left_offset), array_number_of_elements, result_data_u128 + i);
                    break;
                }
                default:
                    throw NotImplementedException("hilbert_encode()/morton_encode() only supports arrays of lengths 1-4 for FLOAT.");
                }
            }
            break;
            case LogicalTypeId::UBIGINT:
            case LogicalTypeId::BIGINT:
            {
                auto encoder = bind_info.encoding_type == 0 ? hilbert_encode_u64_var : morton_encode_u64_var;
                switch (array_number_of_elements)
                {
                case 1:
                {
                    auto left_data_64 = FlatVector::GetData<int64_t>(left_child);
                    auto result_data_u64 = FlatVector::GetData<uint64_t>(result);

                    encoder((uint64_t *)(left_data_64 + left_offset), array_number_of_elements, result_data_u64 + i);
                    break;
                }
                case 2:
                {
                    auto left_data_64 = FlatVector::GetData<int64_t>(left_child);
                    auto result_data_u128 = FlatVector::GetData<uhugeint_t>(result);

                    encoder((uint64_t *)(left_data_64 + left_offset), array_number_of_elements, result_data_u128 + i);
                    break;
                }
                default:
                    throw NotImplementedException("hilbert_encode()/morton_encode() only supports arrays of lengths of 1 or 2 for BIGINT/UBIGINT.");
                }
            }
            break;

            case LogicalTypeId::UINTEGER:
            case LogicalTypeId::INTEGER:
            {
                auto encoder = bind_info.encoding_type == 0 ? hilbert_encode_u32_var : morton_encode_u32_var;
                // The number of elements in the array dictates the output type.
                switch (array_number_of_elements)
                {
                case 1:
                {
                    auto left_data_32 = FlatVector::GetData<int32_t>(left_child);
                    auto result_data_u32 = FlatVector::GetData<uint32_t>(result);

                    encoder((uint32_t *)(left_data_32 + left_offset), array_number_of_elements, result_data_u32 + i);
                    break;
                }
                case 2:
                case 3:
                {
                    auto left_data_32 = FlatVector::GetData<int32_t>(left_child);
                    auto result_data_u64 = FlatVector::GetData<uint64_t>(result);

                    encoder((uint32_t *)(left_data_32 + left_offset), array_number_of_elements, result_data_u64 + i);
                    break;
                }
                case 4:
                {
                    auto left_data_32 = FlatVector::GetData<int32_t>(left_child);
                    auto result_data_u128 = FlatVector::GetData<uhugeint_t>(result);

                    encoder((uint32_t *)(left_data_32 + left_offset), array_number_of_elements, result_data_u128 + i);
                    break;
                }
                default:
                    throw NotImplementedException("hilbert_encode()/morton_encode() only supports arrays of lengths 1-4 for UINTEGER/INTEGER.");
                }
            }
            break;
            case LogicalTypeId::SMALLINT:
            case LogicalTypeId::USMALLINT:
            {
                // The number of elements in the array dictates the output type.
                auto encoder = bind_info.encoding_type == 0 ? hilbert_encode_u16_var : morton_encode_u16_var;
                switch (array_number_of_elements)
                {
                case 1:
                {
                    auto left_data_16 = FlatVector::GetData<int16_t>(left_child);
                    auto result_data_u16 = FlatVector::GetData<uint16_t>(result);

                    encoder((uint16_t *)(left_data_16 + left_offset), array_number_of_elements, result_data_u16 + i);
                    break;
                }
                case 2:
                {
                    auto left_data_16 = FlatVector::GetData<int16_t>(left_child);
                    auto result_data_u32 = FlatVector::GetData<uint32_t>(result);

                    encoder((uint16_t *)(left_data_16 + left_offset), array_number_of_elements, result_data_u32 + i);
                    break;
                }
                case 3:
                case 4:
                {
                    auto left_data_16 = FlatVector::GetData<int16_t>(left_child);
                    auto result_data_u64 = FlatVector::GetData<uint64_t>(result);

                    encoder((uint16_t *)(left_data_16 + left_offset), array_number_of_elements, result_data_u64 + i);
                    break;
                }
                case 5:
                case 6:
                case 7:
                case 8:
                {
                    auto left_data_16 = FlatVector::GetData<int16_t>(left_child);
                    auto result_data_u128 = FlatVector::GetData<uhugeint_t>(result);

                    encoder((uint16_t *)(left_data_16 + left_offset), array_number_of_elements, result_data_u128 + i);
                    break;
                }
                default:
                    throw NotImplementedException("hilbert_encode()/morton_encode() only supports arrays of length 1-8 for SMALLINT/USMALLINT.");
                }
            }
            break;
            case LogicalTypeId::TINYINT:
            case LogicalTypeId::UTINYINT:
            {
                // The number of elements in the array dictates the output type.
                auto encoder = bind_info.encoding_type == 0 ? hilbert_encode_u8_var : morton_encode_u8_var;
                switch (array_number_of_elements)
                {
                case 1:
                {
                    auto left_data_8 = FlatVector::GetData<int8_t>(left_child);
                    auto result_data_u8 = FlatVector::GetData<uint8_t>(result);

                    encoder((uint8_t *)(left_data_8 + left_offset), array_number_of_elements, result_data_u8 + i);
                    break;
                }
                case 2:
                {
                    auto left_data_8 = FlatVector::GetData<int8_t>(left_child);
                    auto result_data_u16 = FlatVector::GetData<uint16_t>(result);

                    encoder((uint8_t *)(left_data_8 + left_offset), array_number_of_elements, result_data_u16 + i);
                    break;
                }
                case 3:
                case 4:
                {
                    auto left_data_8 = FlatVector::GetData<int8_t>(left_child);
                    auto result_data_u32 = FlatVector::GetData<uint32_t>(result);

                    encoder((uint8_t *)(left_data_8 + left_offset), array_number_of_elements, result_data_u32 + i);
                    break;
                }
                case 5:
                case 6:
                case 7:
                case 8:
                {
                    auto left_data_8 = FlatVector::GetData<int8_t>(left_child);
                    auto result_data_u64 = FlatVector::GetData<uint64_t>(result);

                    encoder((uint8_t *)(left_data_8 + left_offset), array_number_of_elements, result_data_u64 + i);
                    break;
                }
                case 9:
                case 10:
                case 11:
                case 12:
                case 13:
                case 14:
                case 15:
                case 16:
                {
                    auto left_data_8 = FlatVector::GetData<int8_t>(left_child);
                    auto result_data_u128 = FlatVector::GetData<uhugeint_t>(result);

                    encoder((uint8_t *)(left_data_8 + left_offset), array_number_of_elements, result_data_u128 + i);
                    break;
                }
                default:
                    throw NotImplementedException("hilbert_encode()/morton_encode() only supports arrays of length 1-16 for UTINYINT/TINYINT.");
                }
            }
            break;
            default:
                throw NotImplementedException("hilbert_encode()/morton_encode() only supports arrays of FLOAT, DOUBLE, BIGINT, UBIGINT, INTEGER, UINTEGER, SMALLINT, USMALLINT, TINYINT, UTINYINT types");
            }
        }

        if (args.size() == 1)
        {
            result.SetVectorType(VectorType::CONSTANT_VECTOR);
        }
    }

    // Extension initalization.
    static void LoadInternal(ExtensionLoader &loader)
    {
        ScalarFunctionSet hilbert_encode("hilbert_encode");
        ScalarFunctionSet morton_encode("morton_encode");

        using SF = ScalarFunction; // Alias for ScalarFunction

        hilbert_encode.AddFunction(SF({LogicalType::ARRAY(LogicalType::ANY, optional_idx::Invalid())}, LogicalType::ANY, lindelEncodeArrayFunc, lindelEncodeArrayBind));
        morton_encode.AddFunction(SF({LogicalType::ARRAY(LogicalType::ANY, optional_idx::Invalid())}, LogicalType::ANY, lindelEncodeArrayFunc, lindelEncodeArrayBind));

        loader.RegisterFunction(hilbert_encode);
        loader.RegisterFunction(morton_encode);

        ScalarFunctionSet hilbert_decode = ScalarFunctionSet("hilbert_decode");
        ScalarFunctionSet morton_decode = ScalarFunctionSet("morton_decode");

        std::vector<LogicalType> types_that_can_be_decoded = {
            LogicalType::UTINYINT,
            LogicalType::USMALLINT,
            LogicalType::UINTEGER,
            LogicalType::UBIGINT,
            LogicalType::UHUGEINT};

        for (const auto &decodable_type : types_that_can_be_decoded)
        {
            hilbert_decode.AddFunction(
                ScalarFunction({decodable_type, LogicalType::UTINYINT, LogicalType::BOOLEAN, LogicalType::BOOLEAN}, LogicalType::ARRAY(LogicalType::ANY, optional_idx::Invalid()),
                               lindelDecodeArrayFun,
                               lindelDecodeToArrayBind));

            morton_decode.AddFunction(
                ScalarFunction({decodable_type, LogicalType::UTINYINT, LogicalType::BOOLEAN, LogicalType::BOOLEAN}, LogicalType::ARRAY(LogicalType::ANY, optional_idx::Invalid()),
                               lindelDecodeArrayFun,
                               lindelDecodeToArrayBind));
        }

        loader.RegisterFunction(hilbert_decode);
        loader.RegisterFunction(morton_decode);

        QueryFarmSendTelemetry(loader, loader.GetDatabaseInstance().shared_from_this(), "shellfs", "202509231");
    }

    void LindelExtension::Load(ExtensionLoader &loader)
    {
        LoadInternal(loader);
    }
    std::string LindelExtension::Name()
    {
        return "lindel";
    }

    std::string LindelExtension::Version() const
    {
        return "202509301";
    }

} // namespace duckdb

extern "C"
{

    DUCKDB_CPP_EXTENSION_ENTRY(lindel, loader)
    {
        duckdb::LoadInternal(loader);
    }
}
