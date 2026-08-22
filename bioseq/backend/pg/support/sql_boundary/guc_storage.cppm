/**
 * @file guc_storage.cppm
 * @brief Bridges typed enum storage to PostgreSQL GUC registration.
 */
module;

#include <bioseq/pg_compat.h>

#include <concepts>
#include <type_traits>
#include <utility>

export module bioseq.postgres.support.sql_boundary:guc_storage;

export import bioseq.postgres.support.enum_storage;
import bioseq.postgres.support;

export namespace bioseq::postgres::support {

/**
 * PostgreSQL registration facts for one typed enum setting.
 *
 * Generated owner modules provide instances with static storage duration.
 */
template<class p_enum>
struct postgres_enum_registration {
    char const* name;
    char const* short_description;
    char const* long_description;
    p_enum boot_value;
    config_enum_entry const* entries;
    GucContext context;
    int flags;
    GucEnumCheckHook check_hook = nullptr;
    GucEnumAssignHook assign_hook = nullptr;
    GucShowHook show_hook = nullptr;

    /** @brief Registers this enum setting against its integer wire cell. */
    void register_variable(int* wire) const {
        auto const* registration = this;
        auto define_variable = [registration, wire] {
            DefineCustomEnumVariable(
                registration->name,
                registration->short_description,
                registration->long_description,
                wire,
                static_cast<int>(registration->boot_value),
                registration->entries,
                registration->context,
                registration->flags,
                registration->check_hook,
                registration->assign_hook,
                registration->show_hook);
        };
        static_assert(
            std::is_trivially_destructible_v<decltype(define_variable)>);
        invoke_pg_as_cpp(define_variable);
    }
};

}  // namespace bioseq::postgres::support

namespace bioseq::postgres::support::detail {

template<class p_storage, class p_registration>
concept pg_exposes_registration_init =
    requires(p_storage& storage, p_registration const& registration) {
        storage.init(registration);
    };

template<class p_storage>
concept pg_exposes_wire_cell = requires(p_storage& storage) {
    storage.wire_;
};

enum class pg_guc_storage_probe : int {
    first = -1,
    second = 7,
};

using pg_probe_storage =
    postgres_enum_storage<pg_guc_storage_probe>;
using pg_probe_registration =
    postgres_enum_registration<pg_guc_storage_probe>;
using probe_access_signature =
    void (*)(pg_probe_storage&, pg_probe_registration const&);

[[maybe_unused]] inline constexpr probe_access_signature
    probe_access_instantiation =
        &postgres_enum_registration_access::initialize<
            pg_guc_storage_probe,
            pg_probe_registration>;

static_assert(std::is_aggregate_v<pg_probe_registration>);
static_assert(std::is_invocable_r_v<
              void,
              decltype(&pg_probe_registration::register_variable),
              pg_probe_registration const&,
              int*>);
static_assert(!pg_exposes_registration_init<
              pg_probe_storage, pg_probe_registration>);
static_assert(!pg_exposes_wire_cell<pg_probe_storage>);
static_assert(probe_access_instantiation != nullptr);

}  // namespace bioseq::postgres::support::detail
