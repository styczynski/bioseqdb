
#include "common_tests_integration.h"
import integration_tests.common;

struct TestTable {
    uint sequence_id;
    std::string label;
};

TEST_CASE("DDDDBGDBGDG!!!!! z", "[hello-world]") {
    connection_test_context context;
    auto conn = context.connection();

    const auto test_data = std::vector<TestTable>({
        TestTable{.sequence_id = 42, .label="A"},
        TestTable{.sequence_id = 16, .label="B"}
    });
    const auto result = sqlgen::write(conn, test_data);

    ASSERT_SQLGEN(result);
    JustLog("HELLO! :D");

    const auto read_data = sqlgen::read<std::vector<TestTable>>(conn).value();
    for (const auto& row : read_data) {
        JustLog("row = {}, label: {}", row.sequence_id, row.label);
    }
}