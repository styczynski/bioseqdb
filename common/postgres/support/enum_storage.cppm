module;

#include <concepts>
#include <type_traits>
#include <utility>

export module bioseq.postgres.support.enum_storage;

export namespace bioseq::postgres::support {

struct postgres_enum_registration_access;

/**
 * Owns the stable integer cell used by a PostgreSQL enum setting.
 *
 * This module deliberately has no PostgreSQL dependency. Ordinary consumers
 * construct and read the typed value; generated PostgreSQL support performs
 * registration through postgres_enum_registration_access.
 *
 * @tparam p_enum Enum with an `int` wire representation.
 */
template <class p_enum>
    requires std::is_enum_v<p_enum> && std::same_as<std::underlying_type_t<p_enum>, int>
class postgres_enum_storage final {
  public:
    /**
     * @brief Initializes the stable wire cell from a typed enum value.
     *
     * @param[in] initial_value Initial setting value.
     */
    constexpr explicit postgres_enum_storage(p_enum initial_value) noexcept
        : wire_value_{static_cast<int>(initial_value)} {}

    postgres_enum_storage(postgres_enum_storage const&) = delete;
    postgres_enum_storage& operator=(postgres_enum_storage const&) = delete;

    /**
     * @brief Returns the typed value currently held by PostgreSQL.
     *
     * @return Current enum setting value.
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
 * Narrow one-shot access used by generated PostgreSQL registration support.
 *
 * A registrar receives the stable integer cell. Completion is recorded only
 * after the registrar returns, so an ordinary C++ exception leaves the
 * storage eligible for a deliberate retry.
 */
struct postgres_enum_registration_access final {
    /**
     * @brief Registers a typed enum cell exactly once.
     *
     * @tparam p_enum Enum stored in the wire cell.
     * @tparam p_registrar Registration adapter accepted by the PostgreSQL
     * boundary.
     * @param[in,out] storage Stable setting storage with static lifetime.
     * @param[in] registrar Adapter that registers the integer wire cell.
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
    /** @brief Accepts the stable wire cell for the compile-time access check. */
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

/** @brief Confirms that construction preserves the typed enum value. */
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
