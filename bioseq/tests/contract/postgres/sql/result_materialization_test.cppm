/**
 * @file result_materialization_test.cppm
 * @brief Checks which C++ results can be converted to SQL rows.
 */
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

export module bioseq.tests.contract.postgres.sql.result_materialization;

import bioseq.postgres.support.sql_boundary;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.table.sql_rows;

namespace {

using bioseq::postgres::support::named_output;
using bioseq::postgres::support::named_output_set;
using bioseq::postgres::support::table_rows;
using bioseq::storage::reaper_event;
using bioseq::table::teardown_diagnostics_row;

/** @brief Encodes a storage reaper event into two named SQL fields. */
struct reaper_event_encoder final {
    inline static constexpr std::array<std::string_view, 2>
        column_names{"action", "target"};

    [[nodiscard]] auto operator()(
            const reaper_event& row) const {
        return bioseq::postgres::support::encode_result_row(
            row.action,
            row.target);
    }
};

/** @brief Encodes teardown diagnostics into two named SQL fields. */
struct teardown_diagnostics_encoder final {
    inline static constexpr std::array<std::string_view, 2>
        column_names{
            "lock_fallback_classifications",
            "missing_relation_observations"};

    [[nodiscard]] auto operator()(
            const teardown_diagnostics_row& row) const noexcept {
        return bioseq::postgres::support::encode_result_row(
            row.lock_fallback_classifications,
            row.missing_relation_observations);
    }
};

/** @brief Supplies an intentionally invalid encoder result for rejection checks. */
struct invalid_result_encoder final {
    inline static constexpr std::array<std::string_view, 2>
        column_names{"action", "target"};

    [[nodiscard]] int operator()(
            const reaper_event&) const noexcept {
        return 0;
    }
};

/** @brief Omits column names to exercise named-result rejection. */
struct unnamed_result_encoder final {
    [[nodiscard]] auto operator()(
            const reaper_event& row) const {
        return bioseq::postgres::support::encode_result_row(
            row.action,
            row.target);
    }
};

/** @brief Matches fields accepted by the SQL result row encoder. */
template<class p_value>
concept encodable_result_field =
    requires(p_value&& value) {
        bioseq::postgres::support::encode_result_row(
            std::forward<p_value>(value));
    };

/** @brief Checks whether encode_result accepts an owned result and encoder. */
template<class p_result, class p_encoder>
concept materializable_owned_result =
    requires(
        FunctionCallInfo call,
        p_result&& result,
        p_encoder&& encoder) {
        {
            bioseq::postgres::support::encode_result(
                call,
                std::move(result),
                std::move(encoder))
        } -> std::same_as<Datum>;
    };

/** @brief Checks whether encode_result accepts a borrowed result. */
template<class p_result, class p_encoder>
concept materializable_borrowed_result =
    requires(
        FunctionCallInfo call,
        p_result& result,
        p_encoder&& encoder) {
        bioseq::postgres::support::encode_result(
            call,
            result,
            std::move(encoder));
    };

using encoded_reaper_event = decltype(
    bioseq::postgres::support::encode_result_row(
        std::declval<const std::string&>(),
        std::declval<const std::string&>()));
using encoded_teardown_diagnostics = decltype(
    bioseq::postgres::support::encode_result_row(
        std::declval<std::int64_t>(),
        std::declval<std::int64_t>()));
using encoded_nullable_fields = decltype(
    bioseq::postgres::support::encode_result_row(
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

static_assert(encoded_reaper_event::field_count == 2);
static_assert(
    encoded_reaper_event::expected_type_oids()[0] == TEXTOID);
static_assert(
    encoded_reaper_event::expected_type_oids()[1] == TEXTOID);
static_assert(encoded_teardown_diagnostics::field_count == 2);
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
              table_rows<reaper_event>,
              reaper_event_encoder>);
static_assert(materializable_owned_result<
              named_output<teardown_diagnostics_row>,
              teardown_diagnostics_encoder>);
static_assert(materializable_owned_result<
              named_output_set<teardown_diagnostics_row>,
              teardown_diagnostics_encoder>);
static_assert(!materializable_owned_result<
              table_rows<reaper_event>,
              invalid_result_encoder>);
static_assert(!materializable_owned_result<
              table_rows<reaper_event>,
              unnamed_result_encoder>);
static_assert(!materializable_borrowed_result<
              table_rows<reaper_event>,
              reaper_event_encoder>);
static_assert(!materializable_borrowed_result<
              named_output<teardown_diagnostics_row>,
              teardown_diagnostics_encoder>);

}  // namespace
