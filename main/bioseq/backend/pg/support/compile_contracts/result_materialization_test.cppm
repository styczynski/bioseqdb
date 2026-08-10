module;

#include <bioseq/pg_compat.h>

#include <array>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

export module bioseq.postgres.support.compile_contracts.result_materialization_test;

import bioseq.postgres.support.sql_boundary;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.table.sql_rows;

namespace {

using bioseq::postgres::support::named_out;
using bioseq::postgres::support::returns_table;
using bioseq::postgres::support::setof_named_out;
using bioseq::storage::reaper_event;
using bioseq::table::teardown_diagnostics_row;

struct reaper_event_encoder final {
    inline static constexpr std::array<std::string_view, 2>
        column_names{"action", "target"};

    [[nodiscard]] auto operator()(
            const reaper_event& row) const {
        return bioseq::postgres::support::sql_result_row(
            row.action,
            row.target);
    }
};

struct teardown_diagnostics_encoder final {
    inline static constexpr std::array<std::string_view, 2>
        column_names{
            "lock_fallback_classifications",
            "missing_relation_observations"};

    [[nodiscard]] auto operator()(
            const teardown_diagnostics_row& row) const noexcept {
        return bioseq::postgres::support::sql_result_row(
            row.lock_fallback_classifications,
            row.missing_relation_observations);
    }
};

struct invalid_result_encoder final {
    inline static constexpr std::array<std::string_view, 2>
        column_names{"action", "target"};

    [[nodiscard]] int operator()(
            const reaper_event&) const noexcept {
        return 0;
    }
};

struct unnamed_result_encoder final {
    [[nodiscard]] auto operator()(
            const reaper_event& row) const {
        return bioseq::postgres::support::sql_result_row(
            row.action,
            row.target);
    }
};

template<class T>
concept encodable_result_field =
    requires(T&& value) {
        bioseq::postgres::support::sql_result_row(
            std::forward<T>(value));
    };

template<class Result, class Encoder>
concept materializable_owned_result =
    requires(
        FunctionCallInfo call,
        Result&& result,
        Encoder&& encoder) {
        {
            bioseq::postgres::support::sql_result_to_datum(
                call,
                std::move(result),
                std::move(encoder))
        } -> std::same_as<Datum>;
    };

template<class Result, class Encoder>
concept materializable_borrowed_result =
    requires(
        FunctionCallInfo call,
        Result& result,
        Encoder&& encoder) {
        bioseq::postgres::support::sql_result_to_datum(
            call,
            result,
            std::move(encoder));
    };

using encoded_reaper_event = decltype(
    bioseq::postgres::support::sql_result_row(
        std::declval<const std::string&>(),
        std::declval<const std::string&>()));
using encoded_teardown_diagnostics = decltype(
    bioseq::postgres::support::sql_result_row(
        std::declval<std::int64_t>(),
        std::declval<std::int64_t>()));
using encoded_nullable_fields = decltype(
    bioseq::postgres::support::sql_result_row(
        std::declval<const std::optional<std::string>&>(),
        std::declval<const std::optional<std::int64_t>&>()));

static_assert(std::is_aggregate_v<reaper_event>);
static_assert(std::is_aggregate_v<teardown_diagnostics_row>);
static_assert(std::same_as<
              decltype(reaper_event::action),
              std::string>);
static_assert(std::same_as<
              decltype(reaper_event::target),
              std::string>);
static_assert(std::same_as<
              decltype(
                  teardown_diagnostics_row::
                      lock_fallback_classifications),
              std::int64_t>);
static_assert(std::same_as<
              decltype(
                  teardown_diagnostics_row::
                      missing_relation_observations),
              std::int64_t>);

static_assert(encoded_reaper_event::fields_count == 2);
static_assert(
    encoded_reaper_event::expected_type_oids()[0] == TEXTOID);
static_assert(
    encoded_reaper_event::expected_type_oids()[1] == TEXTOID);
static_assert(encoded_teardown_diagnostics::fields_count == 2);
static_assert(
    encoded_teardown_diagnostics::expected_type_oids()[0]
    == INT8OID);
static_assert(
    encoded_teardown_diagnostics::expected_type_oids()[1]
    == INT8OID);
static_assert(
    encoded_nullable_fields::expected_type_oids()[0]
    == TEXTOID);
static_assert(
    encoded_nullable_fields::expected_type_oids()[1]
    == INT8OID);
static_assert(!encodable_result_field<Datum>);
static_assert(std::same_as<
              decltype(
                  std::declval<encoded_reaper_event&>()
                      .values_data()),
              Datum*>);
static_assert(std::same_as<
              decltype(
                  std::declval<const encoded_reaper_event&>()
                      .nulls_data()),
              const bool*>);

static_assert(materializable_owned_result<
              returns_table<reaper_event>,
              reaper_event_encoder>);
static_assert(materializable_owned_result<
              named_out<teardown_diagnostics_row>,
              teardown_diagnostics_encoder>);
static_assert(materializable_owned_result<
              setof_named_out<teardown_diagnostics_row>,
              teardown_diagnostics_encoder>);
static_assert(!materializable_owned_result<
              returns_table<reaper_event>,
              invalid_result_encoder>);
static_assert(!materializable_owned_result<
              returns_table<reaper_event>,
              unnamed_result_encoder>);
static_assert(!materializable_borrowed_result<
              returns_table<reaper_event>,
              reaper_event_encoder>);
static_assert(!materializable_borrowed_result<
              named_out<teardown_diagnostics_row>,
              teardown_diagnostics_encoder>);

}  // namespace
