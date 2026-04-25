#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/sqlite.hpp>
#include <vector>

namespace test_enum_cross_table {
enum class AccessRestriction { PUBLIC = 1, INTERNAL = 2, CONFIDENTIAL = 3 };
struct Employee {
  sqlgen::PrimaryKey<uint32_t, sqlgen::auto_incr> id;
  std::string first_name;
  std::string last_name;
  AccessRestriction access_level;
};

struct Document {
  sqlgen::PrimaryKey<uint32_t, sqlgen::auto_incr> id;
  AccessRestriction min_access_level;
  std::string name;
  std::string path;
};

TEST(sqlite, test_enum_cross_table) {
  using namespace sqlgen;
  using namespace sqlgen::literals;

  auto employees = std::vector<Employee>({
      Employee{.first_name = "Homer",
               .last_name = "Simpson",
               .access_level = AccessRestriction::PUBLIC},
      Employee{.first_name = "Waylon",
               .last_name = "Smithers",
               .access_level = AccessRestriction::INTERNAL},
      Employee{.first_name = "Montgomery",
               .last_name = "Burns",
               .access_level = AccessRestriction::CONFIDENTIAL},
  });
  auto documents = std::vector<Document>({
      Document{.min_access_level = AccessRestriction::PUBLIC,
               .name = "Power Plant Safety Manual",
               .path = "/documents/powerplant/safety_manual.txt"},
      Document{.min_access_level = AccessRestriction::INTERNAL,
               .name = "Staff Memo",
               .path = "/documents/powerplant/staff_memo.txt"},
      Document{.min_access_level = AccessRestriction::CONFIDENTIAL,
               .name = "Operations Report",
               .path = "/documents/powerplant/operations_report.pdf"},
      Document{.min_access_level = AccessRestriction::PUBLIC,
               .name = "Project Plan",
               .path = "/documents/powerplant/project_plan.md"},
      Document{.min_access_level = AccessRestriction::INTERNAL,
               .name = "Budget Q1",
               .path = "/documents/powerplant/budget_q1.pdf"},
      Document{.min_access_level = AccessRestriction::CONFIDENTIAL,
               .name = "HR Policies",
               .path = "/documents/powerplant/hr_policies.pdf"},
      Document{.min_access_level = AccessRestriction::PUBLIC,
               .name = "Team Photo",
               .path = "/documents/powerplant/team.jpg"},
      Document{.min_access_level = AccessRestriction::CONFIDENTIAL,
               .name = "Executive Summary",
               .path = "/documents/powerplant/executive_summary.docx"},
      Document{.min_access_level = AccessRestriction::INTERNAL,
               .name = "Release Notes",
               .path = "/documents/powerplant/release_notes.txt"},
  });

  const auto conn = sqlite::connect();
  conn.and_then(drop<Employee> | if_exists)
      .and_then(drop<Document> | if_exists);

  write(conn, employees);
  write(conn, documents);

  const auto smithers = conn.and_then(sqlgen::read<Employee> |
                                      where("last_name"_c == "Smithers" and
                                            "first_name"_c == "Waylon"))
                            .value();

  const auto smithers_level = smithers.access_level;
  const auto smithers_documents =
      conn.and_then(sqlgen::read<std::vector<Document>> |
                    where("min_access_level"_c == smithers_level ||
                          "min_access_level"_c == AccessRestriction::PUBLIC) |
                    order_by("name"_c))
          .value();

  const auto expected_ids = std::set<uint32_t>{1, 2, 4, 5, 7, 9};
  std::set<uint32_t> actual_ids;
  for (const auto &d : smithers_documents) {
    actual_ids.emplace(d.id());
  }

  EXPECT_EQ(expected_ids, actual_ids);
}

}  // namespace test_enum_cross_table

#endif
