#pragma once

#include <tests/support/common.hpp>

#include <fmt/format.h>

// Place other sqlgen headers here
#include <sqlgen/write.hpp>
#include <sqlgen/read.hpp>

/**
 * @brief Asserts a sqlgen result while preserving the test call-site location.
 *
 * The scoped binding evaluates the expression once and ensures the diagnostic
 * reads the same result that the caller supplied.
 */
#define ASSERT_SQLGEN(RESULT)                                                     \
    do {                                                                          \
        const auto& bioseq_sqlgen_result = (RESULT);                               \
        if (!bioseq_sqlgen_result) {                                               \
            ASSERT(                                                               \
                false,                                                            \
                fmt::format(                                                      \
                    "Connection error: {}",                                      \
                    bioseq_sqlgen_result.error().what()),                          \
                #RESULT);                                                         \
        }                                                                         \
    } while (false)
