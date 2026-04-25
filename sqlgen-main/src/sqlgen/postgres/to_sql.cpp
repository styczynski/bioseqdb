#include "sqlgen/postgres/to_sql.hpp"

#include <ranges>
#include <rfl.hpp>
#include <sstream>
#include <stdexcept>
#include <type_traits>

#include "sqlgen/dynamic/Join.hpp"
#include "sqlgen/dynamic/Operation.hpp"
#include "sqlgen/internal/collect/vector.hpp"
#include "sqlgen/internal/strings/strings.hpp"

namespace sqlgen::postgres {

std::string aggregation_to_sql(
    const dynamic::Aggregation& _aggregation) noexcept;

std::string column_or_value_to_sql(const dynamic::ColumnOrValue& _col) noexcept;

std::string condition_to_sql(const dynamic::Condition& _cond) noexcept;

template <class ConditionType>
std::string condition_to_sql_impl(const ConditionType& _condition) noexcept;

std::string column_to_sql_definition(const dynamic::Column& _col) noexcept;

std::string create_index_to_sql(const dynamic::CreateIndex& _stmt) noexcept;

std::string create_table_to_sql(const dynamic::CreateTable& _stmt) noexcept;

std::string create_as_to_sql(const dynamic::CreateAs& _stmt) noexcept;

std::string delete_from_to_sql(const dynamic::DeleteFrom& _stmt) noexcept;

std::string drop_to_sql(const dynamic::Drop& _stmt) noexcept;

std::string escape_single_quote(const std::string& _str) noexcept;

std::string field_to_str(const dynamic::SelectFrom::Field& _field) noexcept;

std::vector<std::string> get_primary_keys(
    const dynamic::CreateTable& _stmt) noexcept;

std::vector<std::pair<std::string, std::vector<std::string>>> get_enum_types(
    const dynamic::CreateTable& _stmt) noexcept;

std::string insert_to_sql(const dynamic::Insert& _stmt) noexcept;

std::string join_to_sql(const dynamic::Join& _stmt) noexcept;

std::string operation_to_sql(const dynamic::Operation& _stmt) noexcept;

std::string properties_to_sql(
    const dynamic::types::Properties& _properties) noexcept;

std::string select_from_to_sql(const dynamic::SelectFrom& _stmt) noexcept;

std::string table_or_query_to_sql(
    const dynamic::SelectFrom::TableOrQueryType& _table_or_query) noexcept;

std::string type_to_sql(const dynamic::Type& _type) noexcept;

std::string union_to_sql(const dynamic::Union& _stmt) noexcept;

std::string update_to_sql(const dynamic::Update& _stmt) noexcept;

std::string write_to_sql(const dynamic::Write& _stmt) noexcept;

// ----------------------------------------------------------------------------

inline std::string get_name(const dynamic::Column& _col) { return _col.name; }

inline std::pair<std::string, std::vector<std::string>> get_enum_mapping(
    const dynamic::Column& _col) {
  return _col.type.visit(
      [&](const auto& _t) -> std::pair<std::string, std::vector<std::string>> {
        using T = std::remove_cvref_t<decltype(_t)>;
        if constexpr (std::is_same_v<T, dynamic::types::Enum>) {
          return {type_to_sql(_t), _t.values};
        }
        return {};
      });
}

inline std::string wrap_in_quotes(const std::string& _name) noexcept {
  return "\"" + _name + "\"";
}

inline std::string wrap_in_single_quotes(const std::string& _name) noexcept {
  return "'" + _name + "'";
}

// ----------------------------------------------------------------------------

std::string aggregation_to_sql(
    const dynamic::Aggregation& _aggregation) noexcept {
  return _aggregation.val.visit([](const auto& _agg) -> std::string {
    using Type = std::remove_cvref_t<decltype(_agg)>;
    std::stringstream stream;
    if constexpr (std::is_same_v<Type, dynamic::Aggregation::Avg>) {
      stream << "AVG(" << operation_to_sql(*_agg.val) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Aggregation::Count>) {
      const auto val =
          std::string(_agg.val && _agg.distinct ? "DISTINCT " : "") +
          (_agg.val ? column_or_value_to_sql(*_agg.val) : std::string("*"));
      stream << "COUNT(" << val << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Aggregation::Max>) {
      stream << "MAX(" << operation_to_sql(*_agg.val) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Aggregation::Min>) {
      stream << "MIN(" << operation_to_sql(*_agg.val) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Aggregation::Sum>) {
      stream << "SUM(" << operation_to_sql(*_agg.val) << ")";

    } else {
      static_assert(rfl::always_false_v<Type>, "Not all cases were covered.");
    }
    return stream.str();
  });
}

std::string column_or_value_to_sql(
    const dynamic::ColumnOrValue& _col) noexcept {
  const auto handle_value = [](const auto& _v) -> std::string {
    using Type = std::remove_cvref_t<decltype(_v)>;
    if constexpr (std::is_same_v<Type, dynamic::String>) {
      return "'" + escape_single_quote(_v.val) + "'";

    } else if constexpr (std::is_same_v<Type, dynamic::Duration>) {
      return "INTERVAL '" + std::to_string(_v.val) + " " +
             rfl::enum_to_string(_v.unit) + "'";

    } else if constexpr (std::is_same_v<Type, dynamic::Null>) {
      return "NULL";

    } else if constexpr (std::is_same_v<Type, dynamic::Timestamp>) {
      return "to_timestamp(" + std::to_string(_v.seconds_since_unix) + ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Boolean>) {
      return _v.val ? "TRUE" : "FALSE";

    } else {
      return std::to_string(_v.val);
    }
  };

  return _col.visit([&](const auto& _c) -> std::string {
    using Type = std::remove_cvref_t<decltype(_c)>;
    if constexpr (std::is_same_v<Type, dynamic::Column>) {
      if (_c.alias) {
        return *_c.alias + "." + wrap_in_quotes(_c.name);
      } else {
        return wrap_in_quotes(_c.name);
      }
    } else {
      return _c.val.visit(handle_value);
    }
  });
}

std::string condition_to_sql(const dynamic::Condition& _cond) noexcept {
  return _cond.val.visit(
      [&](const auto& _c) { return condition_to_sql_impl(_c); });
}

template <class ConditionType>
std::string condition_to_sql_impl(const ConditionType& _condition) noexcept {
  using namespace std::ranges::views;

  using C = std::remove_cvref_t<ConditionType>;

  std::stringstream stream;

  if constexpr (std::is_same_v<C, dynamic::Condition::And>) {
    stream << "(" << condition_to_sql(*_condition.cond1) << ") AND ("
           << condition_to_sql(*_condition.cond2) << ")";

  } else if constexpr (std::is_same_v<
                           C, dynamic::Condition::BooleanColumnOrValue>) {
    stream << column_or_value_to_sql(_condition.col_or_val);

  } else if constexpr (std::is_same_v<C, dynamic::Condition::Equal>) {
    stream << operation_to_sql(_condition.op1) << " = "
           << operation_to_sql(_condition.op2);

  } else if constexpr (std::is_same_v<C, dynamic::Condition::GreaterEqual>) {
    stream << operation_to_sql(_condition.op1)
           << " >= " << operation_to_sql(_condition.op2);

  } else if constexpr (std::is_same_v<C, dynamic::Condition::GreaterThan>) {
    stream << operation_to_sql(_condition.op1) << " > "
           << operation_to_sql(_condition.op2);

  } else if constexpr (std::is_same_v<C, dynamic::Condition::In>) {
    stream << operation_to_sql(_condition.op) << " IN ("
           << internal::strings::join(
                  ", ",
                  internal::collect::vector(_condition.patterns |
                                            transform(column_or_value_to_sql)))
           << ")";

  } else if constexpr (std::is_same_v<C, dynamic::Condition::IsNull>) {
    stream << operation_to_sql(_condition.op) << " IS NULL";

  } else if constexpr (std::is_same_v<C, dynamic::Condition::IsNotNull>) {
    stream << operation_to_sql(_condition.op) << " IS NOT NULL";

  } else if constexpr (std::is_same_v<C, dynamic::Condition::LesserEqual>) {
    stream << operation_to_sql(_condition.op1)
           << " <= " << operation_to_sql(_condition.op2);

  } else if constexpr (std::is_same_v<C, dynamic::Condition::LesserThan>) {
    stream << operation_to_sql(_condition.op1) << " < "
           << operation_to_sql(_condition.op2);

  } else if constexpr (std::is_same_v<C, dynamic::Condition::Like>) {
    stream << operation_to_sql(_condition.op) << " LIKE "
           << column_or_value_to_sql(_condition.pattern);

  } else if constexpr (std::is_same_v<C, dynamic::Condition::Not>) {
    stream << "NOT (" << condition_to_sql(*_condition.cond) << ")";

  } else if constexpr (std::is_same_v<C, dynamic::Condition::NotEqual>) {
    stream << operation_to_sql(_condition.op1)
           << " != " << operation_to_sql(_condition.op2);

  } else if constexpr (std::is_same_v<C, dynamic::Condition::NotLike>) {
    stream << operation_to_sql(_condition.op) << " NOT LIKE "
           << column_or_value_to_sql(_condition.pattern);

  } else if constexpr (std::is_same_v<C, dynamic::Condition::NotIn>) {
    stream << operation_to_sql(_condition.op) << " NOT IN ("
           << internal::strings::join(
                  ", ",
                  internal::collect::vector(_condition.patterns |
                                            transform(column_or_value_to_sql)))
           << ")";

  } else if constexpr (std::is_same_v<C, dynamic::Condition::Or>) {
    stream << "(" << condition_to_sql(*_condition.cond1) << ") OR ("
           << condition_to_sql(*_condition.cond2) << ")";

  } else {
    static_assert(rfl::always_false_v<C>, "Not all cases were covered.");
  }
  return stream.str();
}

std::string column_to_sql_definition(const dynamic::Column& _col) noexcept {
  return wrap_in_quotes(_col.name) + " " + type_to_sql(_col.type) +
         properties_to_sql(
             _col.type.visit([](const auto& _t) { return _t.properties; }));
}

std::string create_index_to_sql(const dynamic::CreateIndex& _stmt) noexcept {
  using namespace std::ranges::views;

  std::stringstream stream;

  if (_stmt.unique) {
    stream << "CREATE UNIQUE INDEX ";
  } else {
    stream << "CREATE INDEX ";
  }

  if (_stmt.if_not_exists) {
    stream << "IF NOT EXISTS ";
  }

  stream << "\"" << _stmt.name << "\" ";

  stream << "ON ";

  if (_stmt.table.schema) {
    stream << "\"" << *_stmt.table.schema << "\".";
  }
  stream << "\"" << _stmt.table.name << "\"";

  stream << "(";
  stream << internal::strings::join(
      ", ",
      internal::collect::vector(_stmt.columns | transform(wrap_in_quotes)));
  stream << ")";

  if (_stmt.where) {
    stream << " WHERE " << condition_to_sql(*_stmt.where);
  }

  stream << ";";

  return stream.str();
}

std::string create_table_to_sql(const dynamic::CreateTable& _stmt) noexcept {
  using namespace std::ranges::views;

  const auto col_to_sql = [&](const auto& _col) {
    return column_to_sql_definition(_col);
  };

  std::stringstream stream;

  for (const auto& [enum_name, enum_values] : get_enum_types(_stmt)) {
    if (_stmt.if_not_exists) {
      stream << "DO $$ BEGIN ";
    }
    stream << "CREATE TYPE " << enum_name << " AS ENUM ("
           << internal::strings::join(
                  ", ", internal::collect::vector(
                            enum_values | transform(wrap_in_single_quotes)))
           << "); ";
    if (_stmt.if_not_exists) {
      stream << "EXCEPTION WHEN duplicate_object THEN NULL; END $$;";
    }
  }

  stream << "CREATE TABLE ";

  if (_stmt.if_not_exists) {
    stream << "IF NOT EXISTS ";
  }

  if (_stmt.table.schema) {
    stream << wrap_in_quotes(*_stmt.table.schema) << ".";
  }
  stream << wrap_in_quotes(_stmt.table.name) << " ";

  stream << "(";
  stream << internal::strings::join(
      ", ", internal::collect::vector(_stmt.columns | transform(col_to_sql)));

  const auto primary_keys = get_primary_keys(_stmt);

  if (primary_keys.size() != 0) {
    stream << ", PRIMARY KEY (" << internal::strings::join(", ", primary_keys)
           << ")";
  }

  stream << ");";

  return stream.str();
}

std::string create_as_to_sql(const dynamic::CreateAs& _stmt) noexcept {
  std::stringstream stream;

  stream << "CREATE ";

  if (_stmt.or_replace) {
    stream << "OR REPLACE ";
  }

  stream << internal::strings::replace_all(
                internal::strings::to_upper(rfl::enum_to_string(_stmt.what)),
                "_", " ")
         << " ";

  if (_stmt.if_not_exists) {
    stream << "IF NOT EXISTS ";
  }

  if (_stmt.table_or_view.schema) {
    stream << wrap_in_quotes(*_stmt.table_or_view.schema) << ".";
  }
  stream << wrap_in_quotes(_stmt.table_or_view.name) << " AS ";

  stream << select_from_to_sql(_stmt.query);

  return stream.str();
}

std::string delete_from_to_sql(const dynamic::DeleteFrom& _stmt) noexcept {
  std::stringstream stream;

  stream << "DELETE FROM ";

  if (_stmt.table.schema) {
    stream << wrap_in_quotes(*_stmt.table.schema) << ".";
  }
  stream << wrap_in_quotes(_stmt.table.name);

  if (_stmt.where) {
    stream << " WHERE " << condition_to_sql(*_stmt.where);
  }

  stream << ";";

  return stream.str();
}

std::string drop_to_sql(const dynamic::Drop& _stmt) noexcept {
  std::stringstream stream;

  stream << "DROP "
         << internal::strings::replace_all(
                internal::strings::to_upper(rfl::enum_to_string(_stmt.what)),
                "_", " ")
         << " ";

  if (_stmt.if_exists) {
    stream << "IF EXISTS ";
  }

  if (_stmt.table.schema) {
    stream << wrap_in_quotes(*_stmt.table.schema) << ".";
  }
  stream << wrap_in_quotes(_stmt.table.name);

  if (_stmt.cascade) {
    stream << " CASCADE";
  }

  stream << ";";

  return stream.str();
}

std::string escape_single_quote(const std::string& _str) noexcept {
  return internal::strings::replace_all(_str, "'", "''");
}

std::string field_to_str(const dynamic::SelectFrom::Field& _field) noexcept {
  std::stringstream stream;

  stream << operation_to_sql(_field.val);

  if (_field.as) {
    stream << " AS " << wrap_in_quotes(*_field.as);
  }

  return stream.str();
}

std::vector<std::string> get_primary_keys(
    const dynamic::CreateTable& _stmt) noexcept {
  using namespace std::ranges::views;

  const auto is_primary_key = [](const auto& _col) -> bool {
    return _col.type.visit(
        [](const auto& _t) -> bool { return _t.properties.primary; });
  };

  return internal::collect::vector(_stmt.columns | filter(is_primary_key) |
                                   transform(get_name) |
                                   transform(wrap_in_quotes));
}

std::vector<std::pair<std::string, std::vector<std::string>>> get_enum_types(
    const dynamic::CreateTable& _stmt) noexcept {
  using namespace std::ranges::views;

  const auto is_enum = [](const dynamic::Column& _col) -> bool {
    return _col.type.visit([&](const auto& _t) -> bool {
      using T = std::remove_cvref_t<decltype(_t)>;
      return std::is_same_v<T, dynamic::types::Enum>;
    });
  };
  return internal::collect::vector(_stmt.columns | filter(is_enum) |
                                   transform(get_enum_mapping));
}

std::string insert_to_sql(const dynamic::Insert& _stmt) noexcept {
  using namespace std::ranges::views;

  const auto to_placeholder = [](const size_t _i) -> std::string {
    return "$" + std::to_string(_i + 1);
  };

  const auto as_excluded = [](const std::string& _str) -> std::string {
    return _str + "=excluded." + _str;
  };

  std::stringstream stream;
  stream << "INSERT INTO ";
  if (_stmt.table.schema) {
    stream << wrap_in_quotes(*_stmt.table.schema) << ".";
  }
  stream << wrap_in_quotes(_stmt.table.name);

  stream << " (";
  stream << internal::strings::join(
      ", ",
      internal::collect::vector(_stmt.columns | transform(wrap_in_quotes)));
  stream << ")";

  stream << " VALUES (";
  stream << internal::strings::join(
      ", ", internal::collect::vector(
                iota(static_cast<size_t>(0), _stmt.columns.size()) |
                transform(to_placeholder)));
  stream << ")";

  if (_stmt.or_replace) {
    stream << " ON CONFLICT (";
    stream << internal::strings::join(
        ", ", internal::collect::vector(_stmt.constraints));
    stream << ")";

    stream << " DO UPDATE SET ";
    stream << internal::strings::join(
        ", ",
        internal::collect::vector(_stmt.columns | transform(as_excluded)));
  }

  stream << ";";
  return stream.str();
}

std::string join_to_sql(const dynamic::Join& _stmt) noexcept {
  std::stringstream stream;

  stream << internal::strings::to_upper(internal::strings::replace_all(
                rfl::enum_to_string(_stmt.how), "_", " "))
         << " " << table_or_query_to_sql(_stmt.table_or_query) << " "
         << _stmt.alias << " ";

  if (_stmt.on) {
    stream << "ON " << condition_to_sql(*_stmt.on);
  } else {
    stream << "ON 1 = 1";
  }

  return stream.str();
}

std::string operation_to_sql(const dynamic::Operation& _stmt) noexcept {
  using namespace std::ranges::views;
  return _stmt.val.visit([](const auto& _s) -> std::string {
    using Type = std::remove_cvref_t<decltype(_s)>;

    std::stringstream stream;

    if constexpr (std::is_same_v<Type, dynamic::Operation::Abs>) {
      stream << "abs(" << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Aggregation>) {
      stream << aggregation_to_sql(_s);

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Cast>) {
      stream << "cast(" << operation_to_sql(*_s.op1) << " as "
             << type_to_sql(_s.target_type) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Coalesce>) {
      stream << "coalesce("
             << internal::strings::join(
                    ", ", internal::collect::vector(
                              _s.ops | transform([](const auto& _op) {
                                return operation_to_sql(*_op);
                              })))
             << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Ceil>) {
      stream << "ceil(" << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Column>) {
      stream << column_or_value_to_sql(_s);

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Concat>) {
      stream << "("
             << internal::strings::join(
                    " || ", internal::collect::vector(
                                _s.ops | transform([](const auto& _op) {
                                  return operation_to_sql(*_op);
                                })))
             << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Cos>) {
      stream << "cos(" << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type,
                                        dynamic::Operation::DatePlusDuration>) {
      stream << operation_to_sql(*_s.date) << " + "
             << internal::strings::join(
                    " + ",
                    internal::collect::vector(
                        _s.durations | transform([](const auto& _d) {
                          return column_or_value_to_sql(dynamic::Value{_d});
                        })));

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Day>) {
      stream << "extract(DAY from " << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type,
                                        dynamic::Operation::DaysBetween>) {
      stream << "cast(" << operation_to_sql(*_s.op2) << " as DATE) - cast("
             << operation_to_sql(*_s.op1) << " as DATE)";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Divides>) {
      stream << "(" << operation_to_sql(*_s.op1) << ") / ("
             << operation_to_sql(*_s.op2) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Exp>) {
      stream << "exp(" << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Floor>) {
      stream << "floor(" << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Hour>) {
      stream << "extract(HOUR from " << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Length>) {
      stream << "length(" << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Ln>) {
      stream << "ln(" << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Log2>) {
      stream << "log(2.0, " << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Lower>) {
      stream << "lower(" << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::LTrim>) {
      stream << "ltrim(" << operation_to_sql(*_s.op1) << ", "
             << operation_to_sql(*_s.op2) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Minus>) {
      stream << "(" << operation_to_sql(*_s.op1) << ") - ("
             << operation_to_sql(*_s.op2) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Minute>) {
      stream << "extract(MINUTE from " << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Mod>) {
      stream << "mod(" << operation_to_sql(*_s.op1) << ", "
             << operation_to_sql(*_s.op2) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Month>) {
      stream << "extract(MONTH from " << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Multiplies>) {
      stream << "(" << operation_to_sql(*_s.op1) << ") * ("
             << operation_to_sql(*_s.op2) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Plus>) {
      stream << "(" << operation_to_sql(*_s.op1) << ") + ("
             << operation_to_sql(*_s.op2) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Replace>) {
      stream << "replace(" << operation_to_sql(*_s.op1) << ", "
             << operation_to_sql(*_s.op2) << ", " << operation_to_sql(*_s.op3)
             << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Round>) {
      stream << "round(" << operation_to_sql(*_s.op1) << ", "
             << operation_to_sql(*_s.op2) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::RTrim>) {
      stream << "rtrim(" << operation_to_sql(*_s.op1) << ", "
             << operation_to_sql(*_s.op2) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Second>) {
      stream << "extract(SECOND from " << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Sin>) {
      stream << "sin(" << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Sqrt>) {
      stream << "sqrt(" << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Tan>) {
      stream << "tan(" << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Trim>) {
      stream << "trim(" << operation_to_sql(*_s.op1) << ", "
             << operation_to_sql(*_s.op2) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Unixepoch>) {
      stream << "extract(EPOCH FROM " << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Upper>) {
      stream << "upper(" << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Value>) {
      stream << column_or_value_to_sql(_s);

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Weekday>) {
      stream << "extract(DOW from " << operation_to_sql(*_s.op1) << ")";

    } else if constexpr (std::is_same_v<Type, dynamic::Operation::Year>) {
      stream << "extract(YEAR from " << operation_to_sql(*_s.op1) << ")";

    } else {
      static_assert(rfl::always_false_v<Type>, "Unsupported type.");
    }
    return stream.str();
  });
}

std::string properties_to_sql(const dynamic::types::Properties& _p) noexcept {
  return [&]() -> std::string {
    return std::string(_p.auto_incr ? " GENERATED ALWAYS AS IDENTITY" : "") +
           std::string(_p.nullable ? "" : " NOT NULL") +
           std::string(_p.unique ? " UNIQUE" : "");
  }() + [&]() -> std::string {
    if (!_p.foreign_key_reference) {
      return "";
    }
    const auto& ref = *_p.foreign_key_reference;
    return " REFERENCES " + wrap_in_quotes(ref.table) + "(" +
           wrap_in_quotes(ref.column) + ")";
  }();
}

std::string select_from_to_sql(const dynamic::SelectFrom& _stmt) noexcept {
  using namespace std::ranges::views;

  const auto order_by_to_str = [](const auto& _w) -> std::string {
    return column_or_value_to_sql(_w.column) + (_w.desc ? " DESC" : "");
  };

  std::stringstream stream;

  stream << "SELECT ";
  stream << internal::strings::join(
      ", ", internal::collect::vector(_stmt.fields | transform(field_to_str)));

  stream << " FROM " << table_or_query_to_sql(_stmt.table_or_query);

  if (_stmt.alias) {
    stream << " " << *_stmt.alias;
  }

  if (_stmt.joins) {
    stream << " "
           << internal::strings::join(
                  " ", internal::collect::vector(*_stmt.joins |
                                                 transform(join_to_sql)));
  }

  if (_stmt.where) {
    stream << " WHERE " << condition_to_sql(*_stmt.where);
  }

  if (_stmt.group_by) {
    stream << " GROUP BY "
           << internal::strings::join(
                  ", ",
                  internal::collect::vector(_stmt.group_by->columns |
                                            transform(column_or_value_to_sql)));
  }

  if (_stmt.order_by) {
    stream << " ORDER BY "
           << internal::strings::join(
                  ", ", internal::collect::vector(_stmt.order_by->columns |
                                                  transform(order_by_to_str)));
  }

  if (_stmt.limit) {
    stream << " LIMIT " << _stmt.limit->val;
  }

  if (_stmt.offset) {
    stream << " OFFSET " << _stmt.offset->val;
  }

  return stream.str();
}

std::string table_or_query_to_sql(
    const dynamic::SelectFrom::TableOrQueryType& _table_or_query) noexcept {
  return _table_or_query.visit([](const auto& _t) -> std::string {
    using Type = std::remove_cvref_t<decltype(_t)>;
    if constexpr (std::is_same_v<Type, dynamic::Table>) {
      if (_t.schema) {
        return wrap_in_quotes(*_t.schema) + "." + wrap_in_quotes(_t.name);
      }
      return wrap_in_quotes(_t.name);

    } else if constexpr (std::is_same_v<Type, Ref<dynamic::Union>>) {
      return "(" + union_to_sql(*_t) + ")";

    } else {
      return "(" + select_from_to_sql(*_t) + ")";
    }
  });
}

std::string to_sql_impl(const dynamic::Statement& _stmt) noexcept {
  return _stmt.visit([&](const auto& _s) -> std::string {
    using S = std::remove_cvref_t<decltype(_s)>;

    if constexpr (std::is_same_v<S, dynamic::CreateIndex>) {
      return create_index_to_sql(_s);

    } else if constexpr (std::is_same_v<S, dynamic::CreateTable>) {
      return create_table_to_sql(_s);

    } else if constexpr (std::is_same_v<S, dynamic::CreateAs>) {
      return create_as_to_sql(_s);

    } else if constexpr (std::is_same_v<S, dynamic::DeleteFrom>) {
      return delete_from_to_sql(_s);

    } else if constexpr (std::is_same_v<S, dynamic::Drop>) {
      return drop_to_sql(_s);

    } else if constexpr (std::is_same_v<S, dynamic::Insert>) {
      return insert_to_sql(_s);

    } else if constexpr (std::is_same_v<S, dynamic::SelectFrom>) {
      return select_from_to_sql(_s);

    } else if constexpr (std::is_same_v<S, dynamic::Update>) {
      return update_to_sql(_s);

    } else if constexpr (std::is_same_v<S, dynamic::Write>) {
      return write_to_sql(_s);

    } else if constexpr (std::is_same_v<S, dynamic::Union>) {
      return union_to_sql(_s);

    } else {
      static_assert(rfl::always_false_v<S>, "Unsupported type.");
    }
  });
}

std::string union_to_sql(const dynamic::Union& _stmt) noexcept {
  using namespace std::ranges::views;

  const auto columns = internal::strings::join(
      ", ",
      internal::collect::vector(_stmt.columns | transform(wrap_in_quotes)));

  const auto to_str = [&](const auto& _select) {
    return "SELECT " + columns + " FROM (" + select_from_to_sql(_select) + ")";
  };

  const auto separator =
      _stmt.all ? std::string(" UNION ALL ") : std::string(" UNION ");

  return internal::strings::join(
      separator, internal::collect::vector(*_stmt.selects | transform(to_str)));
}

std::string type_to_sql(const dynamic::Type& _type) noexcept {
  return _type.visit([](const auto _t) -> std::string {
    using T = std::remove_cvref_t<decltype(_t)>;
    if constexpr (std::is_same_v<T, dynamic::types::Boolean>) {
      return "BOOLEAN";

    } else if constexpr (std::is_same_v<T, dynamic::types::Dynamic>) {
      return _t.type_name;

    } else if constexpr (std::is_same_v<T, dynamic::types::Int8> ||
                         std::is_same_v<T, dynamic::types::Int16> ||
                         std::is_same_v<T, dynamic::types::UInt8> ||
                         std::is_same_v<T, dynamic::types::UInt16>) {
      return "SMALLINT";

    } else if constexpr (std::is_same_v<T, dynamic::types::Int32> ||
                         std::is_same_v<T, dynamic::types::UInt32>) {
      return "INTEGER";

    } else if constexpr (std::is_same_v<T, dynamic::types::Int64> ||
                         std::is_same_v<T, dynamic::types::UInt64>) {
      return "BIGINT";
    } else if constexpr (std::is_same_v<T, dynamic::types::Enum>) {
      return _t.name;
    } else if constexpr (std::is_same_v<T, dynamic::types::Float32> ||
                         std::is_same_v<T, dynamic::types::Float64>) {
      return "NUMERIC";

    } else if constexpr (std::is_same_v<T, dynamic::types::Text>) {
      return "TEXT";

    } else if constexpr (std::is_same_v<T, dynamic::types::VarChar>) {
      return "VARCHAR(" + std::to_string(_t.length) + ")";

    } else if constexpr (std::is_same_v<T, dynamic::types::JSON>) {
      return "JSONB";

    } else if constexpr (std::is_same_v<T, dynamic::types::Date>) {
      return "DATE";

    } else if constexpr (std::is_same_v<T, dynamic::types::Timestamp>) {
      return "TIMESTAMP";

    } else if constexpr (std::is_same_v<T, dynamic::types::TimestampWithTZ>) {
      return "TIMESTAMP WITH TIME ZONE";

    } else if constexpr (std::is_same_v<T, dynamic::types::Unknown>) {
      return "TEXT";
    } else {
      static_assert(rfl::always_false_v<T>, "Not all cases were covered.");
    }
  });
}

std::string update_to_sql(const dynamic::Update& _stmt) noexcept {
  using namespace std::ranges::views;

  const auto to_str = [](const auto& _set) -> std::string {
    return wrap_in_quotes(_set.col.name) + " = " +
           column_or_value_to_sql(_set.to);
  };

  std::stringstream stream;

  stream << "UPDATE ";

  if (_stmt.table.schema) {
    stream << wrap_in_quotes(*_stmt.table.schema) << ".";
  }
  stream << wrap_in_quotes(_stmt.table.name);

  stream << " SET ";

  stream << internal::strings::join(
      ", ", internal::collect::vector(_stmt.sets | transform(to_str)));

  if (_stmt.where) {
    stream << " WHERE " << condition_to_sql(*_stmt.where);
  }

  stream << ";";

  return stream.str();
}

std::string write_to_sql(const dynamic::Write& _stmt) noexcept {
  using namespace std::ranges::views;
  const auto schema = wrap_in_quotes(_stmt.table.schema.value_or("public"));
  const auto table = wrap_in_quotes(_stmt.table.name);
  const auto colnames = internal::strings::join(
      ", ",
      internal::collect::vector(_stmt.columns | transform(wrap_in_quotes)));
  return "COPY " + schema + "." + table + "(" + colnames +
         ") FROM STDIN WITH DELIMITER '\t' NULL '\e' CSV QUOTE '\a';";
}

}  // namespace sqlgen::postgres
