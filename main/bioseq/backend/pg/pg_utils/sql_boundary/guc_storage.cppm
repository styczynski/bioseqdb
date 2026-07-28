module;

#include "common.h"

#include <concepts>
#include <type_traits>
#include <utility>

export module bioseq.pg_utils.sql_boundary:guc_storage;

export import bioseq.config.guc_storage;
import bioseq.pg_utils.error_bridge;

export namespace bioseq::pg_utils {

/**
 * PostgreSQL registration facts for one typed enum setting.
 *
 * Generated owner modules provide instances with static storage duration.
 */
template<class E>
struct postgres_enum_registration {
    char const* name;
    char const* short_description;
    char const* long_description;
    E boot_value;
    config_enum_entry const* entries;
    GucContext context;
    int flags;
    GucEnumCheckHook check_hook = nullptr;
    GucEnumAssignHook assign_hook = nullptr;
    GucShowHook show_hook = nullptr;

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

}  // namespace bioseq::pg_utils

namespace bioseq::pg_utils::detail {

template<class Storage, class Registration>
concept pg_exposes_registration_init =
    requires(Storage& storage, Registration const& registration) {
        storage.init(registration);
    };

template<class Storage>
concept pg_exposes_wire_cell = requires(Storage& storage) {
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
        &guc_registration_access::init<
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

}  // namespace bioseq::pg_utils::detail
