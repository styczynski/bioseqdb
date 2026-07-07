#pragma once

#include "common_tests.h"

#include <fmt/format.h>

// Place other sqlgen headers here
#include <sqlgen/write.hpp>
#include <sqlgen/read.hpp>

#define ASSERT_SQLGEN(RESULT) if (!(RESULT)) { ASSERT(false, fmt::format("Connection error: {}", result.error().what()), (RESULT)); }
