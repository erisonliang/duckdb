#include "duckdb/function/scalar/string_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/string_statistics.hpp"
#include "utf8proc.hpp"

#include <string.h>

using namespace std;

namespace duckdb {

static uint8_t ascii_to_upper_map[] = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,
    22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,
    44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,
    66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,
    88,  89,  90,  91,  92,  93,  94,  95,  96,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,
    78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  123, 124, 125, 126, 127, 128, 129, 130, 131,
    132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153,
    154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
    176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197,
    198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219,
    220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241,
    242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254};
static uint8_t ascii_to_lower_map[] = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,
    22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,
    44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  97,
    98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
    120, 121, 122, 91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
    110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131,
    132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153,
    154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
    176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197,
    198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219,
    220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241,
    242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254};

template <bool IS_UPPER> static string_t strcase_ascii(Vector &result, const char *input_data, idx_t input_length) {
	idx_t output_length = input_length;
	auto result_str = StringVector::EmptyString(result, output_length);
	auto result_data = result_str.GetDataWriteable();
	for (idx_t i = 0; i < input_length; i++) {
		result_data[i] =
		    IS_UPPER ? ascii_to_upper_map[uint8_t(input_data[i])] : ascii_to_lower_map[uint8_t(input_data[i])];
	}
	result_str.Finalize();
	return result_str;
}

template <bool IS_UPPER> static string_t strcase_unicode(Vector &result, const char *input_data, idx_t input_length) {
	// first figure out the output length
	// optimization: if only ascii then input_length = output_length
	idx_t output_length = 0;
	for (idx_t i = 0; i < input_length;) {
		if (input_data[i] & 0x80) {
			// unicode
			int sz = 0;
			int codepoint = utf8proc_codepoint(input_data + i, sz);
			int converted_codepoint = IS_UPPER ? utf8proc_toupper(codepoint) : utf8proc_tolower(codepoint);
			int new_sz = utf8proc_codepoint_length(converted_codepoint);
			D_ASSERT(new_sz >= 0);
			output_length += new_sz;
			i += sz;
		} else {
			// ascii
			output_length++;
			i++;
		}
	}
	auto result_str = StringVector::EmptyString(result, output_length);
	auto result_data = result_str.GetDataWriteable();

	for (idx_t i = 0; i < input_length;) {
		if (input_data[i] & 0x80) {
			// non-ascii character
			int sz = 0, new_sz = 0;
			int codepoint = utf8proc_codepoint(input_data + i, sz);
			int converted_codepoint = IS_UPPER ? utf8proc_toupper(codepoint) : utf8proc_tolower(codepoint);
			const auto success = utf8proc_codepoint_to_utf8(converted_codepoint, new_sz, result_data);
			D_ASSERT(success);
			(void)success;
			result_data += new_sz;
			i += sz;
		} else {
			// ascii
			*result_data =
			    IS_UPPER ? ascii_to_upper_map[uint8_t(input_data[i])] : ascii_to_lower_map[uint8_t(input_data[i])];
			result_data++;
			i++;
		}
	}
	result_str.Finalize();
	return result_str;
}

template <bool IS_UPPER> static void caseconvert_function(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t, true>(args.data[0], result, args.size(), [&](string_t input) {
		auto input_data = input.GetDataUnsafe();
		auto input_length = input.GetSize();
		return strcase_unicode<IS_UPPER>(result, input_data, input_length);
	});
}

template <bool IS_UPPER>
static void caseconvert_function_ascii(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t, true>(args.data[0], result, args.size(), [&](string_t input) {
		auto input_data = input.GetDataUnsafe();
		auto input_length = input.GetSize();
		return strcase_ascii<IS_UPPER>(result, input_data, input_length);
	});
}

template <bool IS_UPPER>
static unique_ptr<BaseStatistics> caseconvert_propagate_stats(ClientContext &context, BoundFunctionExpression &expr,
                                                              FunctionData *bind_data,
                                                              vector<unique_ptr<BaseStatistics>> &child_stats) {
	D_ASSERT(child_stats.size() == 1);
	// can only propagate stats if the children have stats
	if (!child_stats[0]) {
		return nullptr;
	}
	auto &sstats = (StringStatistics &)*child_stats[0];
	if (!sstats.has_unicode) {
		expr.function.function = caseconvert_function_ascii<IS_UPPER>;
	}
	return nullptr;
}

ScalarFunction LowerFun::GetFunction() {
	return ScalarFunction({LogicalType::VARCHAR}, LogicalType::VARCHAR, caseconvert_function<false>, false, nullptr,
	                      nullptr, caseconvert_propagate_stats<false>);
}

void LowerFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction({"lower", "lcase"}, LowerFun::GetFunction());
}

void UpperFun::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction({"upper", "ucase"},
	                ScalarFunction({LogicalType::VARCHAR}, LogicalType::VARCHAR, caseconvert_function<true>, false,
	                               nullptr, nullptr, caseconvert_propagate_stats<true>));
}

} // namespace duckdb
