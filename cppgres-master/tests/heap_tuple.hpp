#pragma once

#include "tests.hpp"

extern "C" {
#include <executor/spi_priv.h>
}

namespace tests {

add_non_atomic_test(heap_tuple, ([](test_case &) {
                      bool result = true;
                      cppgres::transaction_id txid = cppgres::transaction_id::current();
                      {
                        cppgres::spi_nonatomic_executor spi;
                        spi.execute("create table heap_tuple_1 as select 1 as i");
                        spi.commit();
                      }
                      {
                        cppgres::spi_nonatomic_executor spi;
                        auto res =
                            spi.query<std::vector<cppgres::value>>("select * from heap_tuple_1");
                        auto t = res.begin();
                        cppgres::heap_tuple ht = t;
                        result = result && _assert(ht.xmin(true) == txid);
                        spi.execute("update heap_tuple_1 set i = i+1");
                        spi.commit();
                      }
                      {
                        cppgres::spi_nonatomic_executor spi;
                        auto res =
                            spi.query<std::vector<cppgres::value>>("select * from heap_tuple_1");
                        auto t = res.begin();
                        cppgres::heap_tuple ht = t;
                        result = result && _assert(ht.xmin(true) > txid);
                      }
                      return result;
                    }));

} // namespace tests
