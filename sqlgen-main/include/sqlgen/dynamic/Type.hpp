#ifndef SQLGEN_DYNAMIC_TABLE_TYPE_HPP_
#define SQLGEN_DYNAMIC_TABLE_TYPE_HPP_

#include <rfl.hpp>
#include <variant>

#include "types.hpp"

namespace sqlgen::dynamic {

using Type =
    rfl::TaggedUnion<"type", types::Unknown, types::Boolean, types::Dynamic,
                     types::Float32, types::Float64, types::Int8, types::Int16,
                     types::Int32, types::Int64, types::JSON, types::UInt8,
                     types::UInt16, types::UInt32, types::UInt64, types::Text,
                     types::Date, types::Timestamp, types::TimestampWithTZ,
                     types::VarChar, types::Enum>;

}  // namespace sqlgen::dynamic

#endif
