/**
 * @file postgres.hpp
 * @brief Provides PostgreSQL integration-test includes and the SQL result assertion macro.
 */

#pragma once

#include <fmt/format.h>
#include <tests/support/common.hpp>

// Integration tests consume both sqlgen text interfaces at this boundary.
#include <sqlgen/read.hpp>
#include <sqlgen/write.hpp>

/**
 * @brief Asserts a sqlgen result while preserving the test call-site location.
 *
 * The scoped binding evaluates the expression once and ensures the diagnostic
 * reads the same result that the caller supplied.
 */
#define ASSERT_SQLGEN(RESULT)                                                                      \
    do {                                                                                           \
        const auto& bioseq_sqlgen_result = (RESULT);                                               \
        if (!bioseq_sqlgen_result) {                                                               \
            ASSERT(false,                                                                          \
                   fmt::format("Connection error: {}", bioseq_sqlgen_result.error().what()),       \
                   #RESULT);                                                                       \
        }                                                                                          \
    } while (false)
