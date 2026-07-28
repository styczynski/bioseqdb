module;

#include <concepts>
#include <type_traits>
#include <utility>

export module bioseq.config.guc_storage;

export namespace bioseq::pg_utils {

struct guc_registration_access;

/**
 * Owns the stable integer cell used by a PostgreSQL enum setting.
 *
 * This module deliberately has no PostgreSQL dependency. Ordinary consumers
 * construct and read the typed value; generated PostgreSQL support performs
 * registration through guc_registration_access.
 */
template<class E>
    requires std::is_enum_v<E>
          && std::same_as<std::underlying_type_t<E>, int>
class postgres_enum_storage final {
public:
    constexpr explicit postgres_enum_storage(E initial) noexcept
        : wire_{static_cast<int>(initial)} {
    }

    postgres_enum_storage(postgres_enum_storage const&) = delete;
    postgres_enum_storage&
    operator=(postgres_enum_storage const&) = delete;

    [[nodiscard]] constexpr E value() const noexcept {
        return static_cast<E>(wire_);
    }

private:
    friend struct guc_registration_access;

    int wire_;
    bool registered_ = false;
};

/**
 * Narrow one-shot access used by generated PostgreSQL registration support.
 *
 * A registrar receives the stable integer cell. Completion is recorded only
 * after the registrar returns, so an ordinary C++ exception leaves the
 * storage eligible for a deliberate retry.
 */
struct guc_registration_access final {
    template<class E, class Registrar>
        requires requires(
            Registrar const& registrar,
            int* wire) {
            { registrar.register_variable(wire) } -> std::same_as<void>;
        }
    static inline void init(
        postgres_enum_storage<E>& storage,
        Registrar const& registrar) {
        if (storage.registered_) {
            return;
        }
        registrar.register_variable(&storage.wire_);
        storage.registered_ = true;
    }
};

}  // namespace bioseq::pg_utils

namespace bioseq::pg_utils::detail {

enum class guc_storage_probe : int {
    first = -1,
    second = 7,
};

enum class rejected_guc_storage_probe : unsigned char {
    value = 0,
};

struct guc_registrar_probe {
    void register_variable(int*) const {
    }
};

template<class E>
concept has_postgres_enum_storage = requires {
    typename postgres_enum_storage<E>;
};

template<class Storage, class Registrar>
concept exposes_registration_init =
    requires(Storage& storage, Registrar const& registrar) {
        storage.init(registrar);
    };

template<class Storage>
concept exposes_wire_cell = requires(Storage& storage) {
    storage.wire_;
};

using probe_storage = postgres_enum_storage<guc_storage_probe>;

consteval bool probe_initial_value_round_trip() {
    probe_storage storage{guc_storage_probe::second};
    return storage.value() == guc_storage_probe::second;
}

static_assert(has_postgres_enum_storage<guc_storage_probe>);
static_assert(!has_postgres_enum_storage<rejected_guc_storage_probe>);
static_assert(!std::is_default_constructible_v<probe_storage>);
static_assert(!std::is_copy_constructible_v<probe_storage>);
static_assert(!std::is_copy_assignable_v<probe_storage>);
static_assert(!std::is_move_constructible_v<probe_storage>);
static_assert(!std::is_move_assignable_v<probe_storage>);
static_assert(!std::is_convertible_v<probe_storage, int>);
static_assert(std::is_nothrow_constructible_v<
              probe_storage, guc_storage_probe>);
static_assert(noexcept(std::declval<probe_storage const&>().value()));
static_assert(!exposes_registration_init<
              probe_storage, guc_registrar_probe>);
static_assert(!exposes_wire_cell<probe_storage>);
static_assert(probe_initial_value_round_trip());

}  // namespace bioseq::pg_utils::detail
