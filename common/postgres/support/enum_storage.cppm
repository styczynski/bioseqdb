module;

#include <concepts>
#include <type_traits>
#include <utility>

export module bioseq.postgres.support.enum_storage;

export namespace bioseq::postgres::support {

struct postgres_enum_registration_access;

/**
 * Keep an enum setting in an integer cell whose address stays fied.
 * Ordinary C++ code reads the enum value. Generated PostgreSQL code registers
 * the integer cell through postgres_enum_registration_access.
 *
 * This module has no PostgreSQL dependency.
 * @tparam p_enum Enum whose underlying type is int.
 */
template <class p_enum>
    requires std::is_enum_v<p_enum> && std::same_as<std::underlying_type_t<p_enum>, int>
class postgres_enum_storage final {
  public:
    /**
     * @brief Store the initial enum value.
     * @param[in] initial_value Initial setting  value.
     */
    constexpr explicit postgres_enum_storage(p_enum initial_value) noexcept
        : wire_value_{static_cast<int>(initial_value)} {}

    postgres_enum_storage(postgres_enum_storage const&) = delete;
    postgres_enum_storage& operator=(postgres_enum_storage const&) = delete;

    /**
     * @brief Read the current setting as its enum type.
     * @return Current enum value.
     */
    [[nodiscard]] constexpr p_enum get_value() const noexcept {
        return static_cast<p_enum>(wire_value_);
    }

  private:
    friend struct postgres_enum_registration_access;

    int wire_value_;
    bool is_registered_ = false;
};

/**
 * Give generated registration code access to the setting's integer cell.
 * Mark it registered after the registrar returns. If registration throws a
 * C++ exception, a later call can retry.
 */
struct postgres_enum_registration_access final {
    /**
     * Register the setting unless it has already been registered.
     *
     * @tparam p_enum Enum stored in the integer cell.
     * @tparam p_registrar Type with register_variable(int*).
     * @param[in,out] storage Setting storage that must remain alive after  registration
     * @param[in] registrar Registers the cell with PostgreSQL.
     */
    template <class p_enum, class p_registrar>
        requires requires(p_registrar const& registrar, int* wire_value) {
            { registrar.register_variable(wire_value) } -> std::same_as<void>;
        }
    static inline void initialize(postgres_enum_storage<p_enum>& storage,
                                  p_registrar const& registrar) {
        if (storage.is_registered_) {
            return;
        }
        registrar.register_variable(&storage.wire_value_);
        storage.is_registered_ = true;
    }
};

} // namespace bioseq::postgres::support

namespace bioseq::postgres::support::detail {

enum class valid_enum_probe : int {
    first = -1,
    second = 7,
};

enum class unsupported_underlying_type_probe : unsigned char {
    value = 0,
};

struct enum_registrar_probe {
    /** Accept an integer cell so the registration interface can be checked at compile time. */
    void register_variable(int*) const {}
};

template <class p_enum>
concept postgres_enum_storable = requires { typename postgres_enum_storage<p_enum>; };

template <class p_storage, class p_registrar>
concept has_public_registration_initialization =
    requires(p_storage& storage, p_registrar const& registrar) { storage.initialize(registrar); };

template <class p_storage>
concept has_public_wire_value = requires(p_storage& storage) { storage.wire_value_; };

using valid_probe_storage = postgres_enum_storage<valid_enum_probe>;

/** Check at compile time that the initial enum value can be read back. */
consteval bool initial_value_round_trips() {
    valid_probe_storage storage{valid_enum_probe::second};
    return storage.get_value() == valid_enum_probe::second;
}

static_assert(postgres_enum_storable<valid_enum_probe>);
static_assert(!postgres_enum_storable<unsupported_underlying_type_probe>);
static_assert(!std::is_default_constructible_v<valid_probe_storage>);
static_assert(!std::is_copy_constructible_v<valid_probe_storage>);
static_assert(!std::is_copy_assignable_v<valid_probe_storage>);
static_assert(!std::is_move_constructible_v<valid_probe_storage>);
static_assert(!std::is_move_assignable_v<valid_probe_storage>);
static_assert(!std::is_convertible_v<valid_probe_storage, int>);
static_assert(std::is_nothrow_constructible_v<valid_probe_storage, valid_enum_probe>);
static_assert(noexcept(std::declval<valid_probe_storage const&>().get_value()));
static_assert(!has_public_registration_initialization<valid_probe_storage, enum_registrar_probe>);
static_assert(!has_public_wire_value<valid_probe_storage>);
static_assert(initial_value_round_trips());

} // namespace bioseq::postgres::support::detail
