#ifndef SQLGEN_DYNAMIC_TABLE_TYPES_HPP_
#define SQLGEN_DYNAMIC_TABLE_TYPES_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sqlgen::dynamic::types {

struct ForeignKeyReference {
  std::string table;
  std::string column;
};

struct Properties {
  bool auto_incr = false;
  bool primary = false;
  bool nullable = false;
  bool unique = false;
  std::optional<ForeignKeyReference> foreign_key_reference = std::nullopt;
};

// To be used as the default value.
struct Unknown {
  Properties properties;
};

struct Boolean {
  Properties properties;
};

struct Dynamic {
  std::string type_name;
  Properties properties;
};

struct Float32 {
  Properties properties;
};

struct Float64 {
  Properties properties;
};

struct Int8 {
  Properties properties;
};

struct Int16 {
  Properties properties;
};

struct Int32 {
  Properties properties;
};

struct Int64 {
  Properties properties;
};

struct JSON {
  Properties properties;
};

struct UInt8 {
  Properties properties;
};

struct UInt16 {
  Properties properties;
};

struct UInt32 {
  Properties properties;
};

struct UInt64 {
  Properties properties;
};

struct Enum {
  std::string name;
  std::vector<std::string> values;
  Properties properties;
};

struct Text {
  Properties properties;
};

struct Date {
  std::string tz;
  Properties properties;
};

struct Timestamp {
  std::string tz;
  Properties properties;
};

struct TimestampWithTZ {
  std::string tz;
  Properties properties;
};

struct VarChar {
  uint16_t length;
  Properties properties;
};

}  // namespace sqlgen::dynamic::types

#endif
