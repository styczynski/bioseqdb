module;
#include "common.h"
#include <sqlgen/postgres.hpp>
#include <sqlgen/write.hpp>

export module integration_tests.common;

export import testing;
export import mlogging;

export class connection_test_context {
private:
    sqlgen::postgres::Credentials _credentials;
    decltype(sqlgen::postgres::connect(sqlgen::postgres::Credentials{})) _connection;
public:
    connection_test_context(): _credentials(sqlgen::postgres::Credentials{
        .user = "postgres",
        .password = "postgres",
        .host = "localhost",
        .dbname = "bioseq",
        .port = 5432
    }), _connection(sqlgen::postgres::connect(_credentials)) {
        
    }

    [[always_inline]] auto connection() const {
        return _connection;
    }
};
