/**
 * @file minimizer_fragment_mapping_lease.cppm
 * @brief Owns the PostgreSQL resource lease behind a mapped minimizer image.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "utils/memutils.h"
#include "utils/resowner.h"
}

#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

export module bioseq.postgres.storage.indexing.minimizer_fragment_mapping_lease;

import std;

export namespace bioseq::storage::indexing {

namespace minimizer_fragment_image_pg_detail {

/// PostgreSQL ResourceOwner record for one mapped file and descriptor.
struct mapping_lease {
    ResourceOwner owner = nullptr;
    int fd = -1;
    void* mapping = MAP_FAILED;
    std::size_t mapping_bytes = 0u;
    dev_t device{};
    ino_t inode{};
    bool remembered = false;
};

/// Releases the operating-system resources without forgetting PostgreSQL ownership.
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

/// ResourceOwner callback used when PostgreSQL unwinds past C++ destructors.
inline void release_mapping_lease_resource(const Datum value) noexcept {
    auto* const lease = reinterpret_cast<mapping_lease*>(DatumGetPointer(value));
    if (lease == nullptr)
        return;
    lease->remembered = false;
    release_mapping_lease_raw(*lease);
}

inline constexpr ResourceReleasePriority k_mapping_lease_release_priority = 10u;
static_assert(k_mapping_lease_release_priority >= RELEASE_PRIO_FIRST);

/// Descriptor used to remember mapped image leases in the current ResourceOwner.
inline const ResourceOwnerDesc k_mapping_lease_resource_owner_descriptor{
    .name = "bioseq minimizer fragment image mapping lease",
    .release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
    .release_priority = k_mapping_lease_release_priority,
    .ReleaseResource = release_mapping_lease_resource,
    .DebugPrint = nullptr,
};

/// Forgets and destroys one mapping lease during ordinary C++ unwinding.
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
 * @brief Forms a non-owning token for PostgreSQL-free mapped-index readers.
 * @return An aliasing token, or an empty token when the lease is incomplete.
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
 * @brief Owns one registered minimizer-fragment mmap and file-descriptor lease.
 *
 * Borrowed reader tokens do not extend the lifetime. Store this guard before
 * every reader that borrows its mapping so reverse destruction releases the
 * readers first.
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
     * @brief Adopts a registered mapping lease produced by the image opener.
     * @param[in] lease Lease transferred to the guard.
     * @return Owning guard for the supplied lease.
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
     * @brief Borrows a token that becomes invalid when this guard is released.
     * @return Non-owning aliasing token for the mapped bytes.
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
