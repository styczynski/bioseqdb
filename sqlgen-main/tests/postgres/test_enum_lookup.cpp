#ifndef SQLGEN_BUILD_DRY_TESTS_ONLY

#include <gtest/gtest.h>

#include <rfl.hpp>
#include <rfl/json.hpp>
#include <sqlgen.hpp>
#include <sqlgen/postgres.hpp>
#include <vector>
#include "test_helpers.hpp"

namespace test_enum_lookup {

enum class AccessRestriction { PUBLIC = 1, INTERNAL = 2, CONFIDENTIAL = 3 };
struct Document {
  sqlgen::PrimaryKey<uint32_t, sqlgen::auto_incr> id;
  AccessRestriction min_access_level;
  std::string name;
  std::string path;
};

TEST(postgres, test_enum_lookup) {
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

  const auto credentials = sqlgen::postgres::test::make_credentials();

  using namespace sqlgen;
  using namespace sqlgen::literals;

  const auto public_documents =
      postgres::connect(credentials)
          .and_then(drop<Document> | if_exists)
          .and_then(write(std::ref(documents)))
          .and_then(sqlgen::read<std::vector<Document>> |
                    where("min_access_level"_c == AccessRestriction::PUBLIC) |
                    order_by("name"_c.desc()))
          .value();

  const auto expected = std::vector<Document>({
      Document{.id = 7,
               .min_access_level = AccessRestriction::PUBLIC,
               .name = "Team Photo",
               .path = "/documents/powerplant/team.jpg"},
      Document{.id = 4,
               .min_access_level = AccessRestriction::PUBLIC,
               .name = "Project Plan",
               .path = "/documents/powerplant/project_plan.md"},
      Document{.id = 1,
               .min_access_level = AccessRestriction::PUBLIC,
               .name = "Power Plant Safety Manual",
               .path = "/documents/powerplant/safety_manual.txt"},
  });

  const auto json1 = rfl::json::write(expected);
  const auto json2 = rfl::json::write(public_documents);
  EXPECT_EQ(json1, json2);
}

}  // namespace test_enum_lookup

#endif
