#pragma once

#include "tests.hpp"

namespace tests {

add_test(current_role, ([](test_case &) {
           bool result = true;
           cppgres::role role;
           result = result && _assert(role.is_valid());
           return result;
         }));

add_test(find_a_role, ([](test_case &) {
           bool result = true;

           {
             cppgres::spi_executor spi;
             spi.execute("create role findthisrole");
             cppgres::role role("findthisrole");
             result = result && _assert(role.name() == "findthisrole");
             result = result && _assert(role.is_valid());
           }

           {
             bool exception_raised = false;
             cppgres::internal_subtransaction tx;
             try {
               cppgres::role role("thisroledoesnotexist");
             } catch (std::exception) {
               exception_raised = true;
             }
             result = result && _assert(exception_raised);
           }
           return result;
         }));

add_test(membership, ([](test_case &) {
           bool result = true;

           cppgres::spi_executor spi;
           spi.execute("create role role1");
           spi.execute("create role role2");
           spi.execute("grant role2 to role1");
           cppgres::role role1("role1");
           cppgres::role role2("role2");

           result = result && _assert(role1.is_member(role2));
           result = result && _assert(!role2.is_member(role1));

           return result;
         }));

add_test(security_context, ([](test_case &) {
           bool result = true;

           cppgres::spi_executor spi;
           spi.execute("create role sectx1");
           cppgres::role role("sectx1");
           {
             cppgres::security_context ctx(role);
             result = result && _assert(cppgres::role() == role);
           }
           result = result && _assert(cppgres::role() != role);

           return result;
         }));

add_test(security_context_escape, ([](test_case &) {
           bool result = true;

           cppgres::spi_executor spi;
           spi.execute("create role sectx2_other");
           spi.execute("create role sectx2");
           cppgres::role role("sectx2");
           {
             cppgres::security_context ctx(role, cppgres::security_local_user_id_change);
             {
               bool exception_raised = false;
               try {
                 cppgres::internal_subtransaction tx;
                 spi.execute("set role sectx2_other");
               } catch (std::exception) {
                 exception_raised = true;
               }
               result = result && _assert(exception_raised);
             }
             {
               bool exception_raised = false;
               try {
                 cppgres::internal_subtransaction tx;
                 spi.execute("reset role");
               } catch (std::exception) {
                 exception_raised = true;
               }
               result = result && _assert(exception_raised);
             }
           }
           result = result && _assert(cppgres::role() != role);

           return result;
         }));

} // namespace tests
