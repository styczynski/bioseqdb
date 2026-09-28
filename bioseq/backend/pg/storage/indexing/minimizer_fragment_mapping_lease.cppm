/**
 * @file minimizer_fragment_mapping_lease.cppm
 * @brief Keep a mapped minimizer index registered for PostgreSQL error cleanup.
 */
module;

extern "C" {
#include "postgres.h"

#include "utils/memutils.h"
#include "utils/resowner.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

export module bioseq.postgres.storage.indexing.minimizer_fragment_mapping_lease;

export namespace bioseq::storage::indexing {

namespace minimizer_fragment_image_pg_detail {

/// A mapped file and its descriptor registered with a PostgreSQL ResourceOwner.
struct mapping_lease {
    ResourceOwner owner = nullptr;
    int fd = -1;
    void* mapping = MAP_FAILED;
    std::size_t mapping_bytes = 0u;
    dev_t device{};
    ino_t inode{};
    bool remembered = false;
};

/// Unmap the file and close its descriptor, leaving the ResourceOwner entry in place.
inline void release_mapping_lease_raw(mapping_lease& lease) noexcept {
    if (lease.mapping != MAP_FAILED) {
        (void)munmap(lease.mapping, lease.mapping_bytes);
        lease.mapping = MAP_FAILED;
        lease.mapping_bytes = 0u;
    }
    if (lease.fd >= 0) {
        (void)::close(lease.fd);
        lease.fd = -1;
    }
}

/// Release the mapping during PostgreSQL resource cleanup.
inline void release_mapping_lease_resource(const Datum value) noexcept {
    auto* const lease = reinterpret_cast<mapping_lease*>(DatumGetPointer(value));
    if (lease == nullptr)
        return;
    lease->remembered = false;
    release_mapping_lease_raw(*lease);
}

inline constexpr ResourceReleasePriority k_mapping_lease_release_priority = 10u;
static_assert(k_mapping_lease_release_priority >= RELEASE_PRIO_FIRST);

/// Tell ResourceOwner how and when  to release a mapped index file.
inline const ResourceOwnerDesc k_mapping_lease_resource_owner_descriptor{
    .name = "bioseq minimizer fragment image mapping lease",
    .release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
    .release_priority = k_mapping_lease_release_priority,
    .ReleaseResource = release_mapping_lease_resource,
    .DebugPrint = nullptr,
};

/// Remove the ResourceOwner entry, release the mapping and free its record.
inline void destroy_mapping_lease(mapping_lease*& lease) noexcept {
    if (lease == nullptr)
        return;
    if (lease->remembered) {
        ResourceOwnerForget(
            lease->owner, PointerGetDatum(lease), &k_mapping_lease_resource_owner_descriptor);
        lease->remembered = false;
    }
    release_mapping_lease_raw(*lease);
    lease->~mapping_lease();
    pfree(lease);
    lease = nullptr;
}

/**
 * @brief Borrow the mapped address for readers that use no PostgreSQL calls.
 * @return An aliasing shared_ptr, or an empty pointer if the mapping is incomplete.
 *
 * The pointer owns no memory. Use it only while the gufrd owns the mapping
 * and PostgreSQL's ResourceOwner has not released it.
 */
[[nodiscard]] inline std::shared_ptr<const void>
borrow_mapping_token(mapping_lease* const lease) noexcept {
    if (lease == nullptr || lease->owner == nullptr || !lease->remembered || lease->fd < 0 ||
        lease->mapping == MAP_FAILED || lease->mapping == nullptr || lease->mapping_bytes == 0u) {
        return {};
    }
    std::shared_ptr<const void> no_owner;
    return std::shared_ptr<const void>{no_owner, lease->mapping};
}

} // namespace minimizer_fragment_image_pg_detail

/**
 * @brief Own a mapped index file and its PostgreSQL cleanup record.
 *
 * Readers borrow the mapping without extending its lifetime. Declare this guard
 * before those readers so they are destroyed before  the guard unmaps the file
 * PostgreSQL can also release the mapping during resource cleanup.
 */
class mapped_minimizer_fragment_lease_guard {
  private:
    minimizer_fragment_image_pg_detail::mapping_lease* lease_ = nullptr;

    explicit mapped_minimizer_fragment_lease_guard(
        minimizer_fragment_image_pg_detail::mapping_lease* lease) noexcept
        : lease_(lease) {}

  public:
    mapped_minimizer_fragment_lease_guard() = default;
    mapped_minimizer_fragment_lease_guard(const mapped_minimizer_fragment_lease_guard&) = delete;
    mapped_minimizer_fragment_lease_guard&
    operator=(const mapped_minimizer_fragment_lease_guard&) = delete;

    mapped_minimizer_fragment_lease_guard(mapped_minimizer_fragment_lease_guard&& other) noexcept
        : lease_(std::exchange(other.lease_, nullptr)) {}

    mapped_minimizer_fragment_lease_guard&
    operator=(mapped_minimizer_fragment_lease_guard&& other) noexcept {
        if (this != &other) {
            minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease_);
            lease_ = std::exchange(other.lease_, nullptr);
        }
        return *this;
    }

    ~mapped_minimizer_fragment_lease_guard() {
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease_);
    }

    /**
     * @brief Take ownership of a mapping record created by the image opener.
     * @param[in] lease Registered mapping record to own.
     * @return A guard that releases the record and mapping on destruction.
     */
    [[nodiscard]] static mapped_minimizer_fragment_lease_guard
    adopt(minimizer_fragment_image_pg_detail::mapping_lease* lease) noexcept {
        return mapped_minimizer_fragment_lease_guard{lease};
    }

    /// @return True when the guard owns a complete registered mapping.
    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(borrow_mapping_token());
    }

    /**
     * @brief Borrow, the mapped address without extending its lifetime.
     * @return A shared_ptr that points to the bytes but does not own them.
     *
     * The address is valid until this guard releases the mapping or PostgreSQL
     * releases its ResourceOwner resources.
     */
    [[nodiscard]] std::shared_ptr<const void> borrow_mapping_token() const noexcept {
        return minimizer_fragment_image_pg_detail::borrow_mapping_token(lease_);
    }
};

static_assert(!std::is_copy_constructible_v<mapped_minimizer_fragment_lease_guard>);
static_assert(!std::is_copy_assignable_v<mapped_minimizer_fragment_lease_guard>);
static_assert(std::is_nothrow_move_constructible_v<mapped_minimizer_fragment_lease_guard>);
static_assert(std::is_nothrow_move_assignable_v<mapped_minimizer_fragment_lease_guard>);

} // namespace bioseq::storage::indexing
