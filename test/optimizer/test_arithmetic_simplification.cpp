#include "catch.hpp"
#include "common/helper.hpp"
#include "expression_helper.hpp"
#include "optimizer/rule/arithmetic_simplification.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("Arithmetic simplification test", "[optimizer]") {
	DuckDB db(nullptr);
	Connection con(db);

	ExpressionHelper helper(con.context);
	helper.AddRule<ArithmeticSimplificationRule>();

	string input, expected_output;

	input = "X+0";
	expected_output = "X";
	REQUIRE(helper.VerifyRewrite(input, expected_output));

	input = "0+X";
	expected_output = "X";
	REQUIRE(helper.VerifyRewrite(input, expected_output));

	input = "X-0";
	expected_output = "X";
	REQUIRE(helper.VerifyRewrite(input, expected_output));

	input = "X*1";
	expected_output = "X";
	REQUIRE(helper.VerifyRewrite(input, expected_output));

	input = "X/0";
	expected_output = "NULL";
	REQUIRE(helper.VerifyRewrite(input, expected_output));

	input = "X/1";
	expected_output = "X";
	REQUIRE(helper.VerifyRewrite(input, expected_output));
}