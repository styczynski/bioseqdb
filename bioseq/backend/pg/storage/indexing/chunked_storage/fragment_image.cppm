/**
 * @file fragment_image.cppm
 * @brief Header-last mapped fragment-image persistence and publication for
 *        chunked minimizer generations.
 *
 * Index chunks are raw BYTEA tuples in heap relations (heap_form_tuple /
 * CatalogTupleInsert + systable_beginscan), bypassing columnarisation
 * (Invariant C). The per-sequence layouts co-locate with the sequence's
 * `(bucket, shard)`. The table layout shards via `index_table_shard`.
 */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "storage/fd.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "miscadmin.h"
}

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
export module bioseq.postgres.storage.indexing.chunked_storage:fragment_image;
import std;
export import bioseq.postgres.storage.indexing.minimizer_fragment_mapping_lease;
import bioseq.common.logging;
import bioseq.indexing.minimizer_fragment_image; // explicit fixed-endian minimizer fragment image image ABI
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import :catalog;
import :catalog_source_authorization;
import :catalog_storage_descriptors;
import :file_actions;
export namespace bioseq::storage::indexing {

namespace minimizer_fragment_image_pg_detail {

[[nodiscard]] inline minimizer_generation_file_identity
generation_file_identity(const minimizer_generation_administration_identity& identity,
                         const std::uint64_t query_key,
                         const std::uint64_t generation_id) {
    if (identity.index_name != k_minimizers_per_fragment_index_name ||
        identity.database_oid != MyDatabaseId || identity.column_attno <= 0) {
        minimizer_generation_storage_error_as_cpp("minimizer fragment image file identity is not "
                                                  "the current minimizer-fragment relation");
    }
    return minimizer_generation_file_identity{
        .database_oid = identity.database_oid,
        .tablespace_oid = identity.tablespace_oid,
        .main_relfilenode = identity.main_relfilenode,
        .user_relid = identity.user_relid,
        .column_attno = identity.column_attno,
        .index_identity = k_mapped_minimizer_fragment_index_identity,
        .query_key = query_key,
        .generation_id = generation_id,
    };
}

inline void
pwrite_all(const int fd, const std::span<const std::byte> bytes, const std::uint64_t offset) {
    std::size_t written = 0u;
    while (written < bytes.size()) {
        if (written > (std::numeric_limits<std::uint64_t>::max)() - offset) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image write offset overflows");
        }
        const std::uint64_t current = offset + written;
        if (current > static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)())) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image write offset exceeds off_t");
        }
        const ssize_t result = pg_pwrite(fd,
                                         bytes.data() + static_cast<std::ptrdiff_t>(written),
                                         bytes.size() - written,
                                         static_cast<off_t>(current));
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0) {
            minimizer_generation_storage_error_as_cpp(
                fmt::format("minimizer fragment image write failed: errno={}", errno));
        }
        written += static_cast<std::size_t>(result);
    }
}

inline void write_zero_padding(const int fd, const std::uint64_t begin, const std::uint64_t end) {
    static constexpr std::array<std::byte, 64u> k_zeros{};
    if (end < begin || end - begin > k_zeros.size()) {
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image padding geometry is invalid");
    }
    pwrite_all(fd,
               std::span<const std::byte>{k_zeros}.first(static_cast<std::size_t>(end - begin)),
               begin);
}

/// Extend the private temporary inode to one checked, mmap-representable
/// image length. No final name is visible while this operation can fail.
inline void resize_temporary_image(const int fd, const std::uint64_t image_bytes) {
    if (fd < 0 || image_bytes > static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)()) ||
        image_bytes > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image image length is not mmap/off_t representable");
    }
    while (::ftruncate(fd, static_cast<off_t>(image_bytes)) != 0) {
        if (errno == EINTR)
            continue;
        minimizer_generation_storage_error_as_cpp(
            fmt::format("minimizer fragment image image resize failed: errno={}", errno));
    }
}

/// Scope-bound offset-zero shared mapping. Offset zero is page-aligned on
/// every supported PostgreSQL host, while callers expose only exact sections.
class scoped_mapped_region {
  private:
    void* mapping_ = MAP_FAILED;
    std::size_t bytes_ = 0u;

  public:
    scoped_mapped_region(const int fd, const std::uint64_t bytes, const int protection)
        : bytes_(static_cast<std::size_t>(bytes)) {
        if (fd < 0 || bytes == 0u ||
            bytes > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image mapping geometry is invalid");
        }
        mapping_ = mmap(nullptr, bytes_, protection, MAP_SHARED, fd, 0);
        if (mapping_ == MAP_FAILED) {
            minimizer_generation_storage_error_as_cpp(
                fmt::format("minimizer fragment image mmap failed: errno={}", errno));
        }
    }

    scoped_mapped_region(const scoped_mapped_region&) = delete;
    scoped_mapped_region& operator=(const scoped_mapped_region&) = delete;
    scoped_mapped_region(scoped_mapped_region&&) = delete;
    scoped_mapped_region& operator=(scoped_mapped_region&&) = delete;

    ~scoped_mapped_region() {
        if (mapping_ != MAP_FAILED) {
            (void)munmap(mapping_, bytes_);
        }
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {static_cast<const std::byte*>(mapping_), bytes_};
    }

    [[nodiscard]] std::span<std::byte> writable_bytes() noexcept {
        return {static_cast<std::byte*>(mapping_), bytes_};
    }

    /// Persist every dirty page before the outer header can authorize it.
    void sync() const {
        while (msync(mapping_, bytes_, MS_SYNC) != 0) {
            if (errno == EINTR)
                continue;
            minimizer_generation_storage_error_as_cpp(
                fmt::format("minimizer fragment image mapped image sync failed: errno={}", errno));
        }
    }
};

[[nodiscard]] inline bioseq::indexing::expected_minimizer_fragment_image_identity
expected_identity(const minimizer_generation_file_identity& identity,
                  const minimizer_generation_head& head,
                  const minimizer_generation_manifest& manifest,
                  const bioseq::indexing::minimizer_fragment_image_layout& layout,
                  const std::uint64_t header_crc64) noexcept {
    return bioseq::indexing::expected_minimizer_fragment_image_identity{
        .database_oid = identity.database_oid,
        .tablespace_oid = identity.tablespace_oid,
        .main_relfilenode = identity.main_relfilenode,
        .user_relid = identity.user_relid,
        .column_attno = static_cast<std::uint16_t>(identity.column_attno),
        .index_identity = bioseq::indexing::k_minimizer_fragment_image_index_identity,
        .layout_identity = bioseq::indexing::k_minimizer_fragment_image_layout_identity,
        .query_key = identity.query_key,
        .generation_id = identity.generation_id,
        .source_identity = head.source_identity,
        .source_content_crc = head.source_content_checksum,
        .storage_contract_checksum = head.storage_contract_checksum,
        .target_length = manifest.target_length,
        .file_bytes = layout.file_bytes,
        .position_segment_count = manifest.position_segment_count,
        .position_segment_span_bp = manifest.position_segment_span_bp,
        .payload = layout.payload,
        .directory = layout.directory,
        .manifest = layout.manifest,
        .header_crc64 = header_crc64,
        .feature_bitmap = layout.feature_bitmap,
        .compact_hash32_direct = layout.compact_hash32_direct,
    };
}

inline void validate_generation_metadata(const minimizer_generation_file_identity& identity,
                                         const minimizer_generation_head& head,
                                         const minimizer_generation_manifest& manifest,
                                         const std::span<const std::byte> directory,
                                         const std::span<const std::byte> encoded_manifest) {
    if (manifest.layout !=
            bioseq::indexing::minimizer_generation_layout::grouped_soa_segment_local ||
        static_cast<std::uint16_t>(manifest.layout) !=
            bioseq::indexing::k_minimizer_fragment_image_layout_identity ||
        identity.generation_id == 0u || identity.generation_id != head.generation_id ||
        identity.generation_id != manifest.generation_id ||
        head.source_identity != manifest.source_identity ||
        head.source_content_checksum != manifest.source_content_checksum ||
        head.storage_contract_checksum !=
            bioseq::indexing::compute_minimizer_generation_storage_contract_checksum(manifest) ||
        directory.size() != manifest.directory_bytes ||
        bioseq::indexing::crc64_ecma182(directory) != manifest.directory_checksum ||
        encoded_manifest.size() != minimizer_generation_manifest::encoded_bytes ||
        bioseq::indexing::crc64_ecma182(encoded_manifest) != head.manifest_checksum) {
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image generation metadata receipt is invalid");
    }
}

[[nodiscard]] inline mapping_lease* allocate_mapping_lease() {
    if (CurrentResourceOwner == nullptr || TopTransactionContext == nullptr) {
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image mapping requires a transaction ResourceOwner");
    }
    void* const storage = MemoryContextAlloc(TopTransactionContext, sizeof(mapping_lease));
    return ::new (storage) mapping_lease{
        .owner = CurrentResourceOwner,
    };
}

struct page_aligned_mapping_range {
    std::size_t begin = 0u;
    std::size_t end = 0u;

    [[nodiscard]] constexpr bool
    operator==(const page_aligned_mapping_range&) const noexcept = default;
};

/// Translate one byte-exact authenticated section into the page interval
/// accepted by madvise-style APIs. The final interval never exceeds the
/// mapping. Its first/last page may also contain adjacent wire bytes because
/// the VM can only advise complete pages.
[[nodiscard]] constexpr std::optional<page_aligned_mapping_range>
page_aligned_advice_range(const std::size_t mapping_bytes,
                          const std::size_t raw_begin,
                          const std::size_t raw_bytes,
                          const std::size_t page_bytes) noexcept {
    if (mapping_bytes == 0u || raw_bytes == 0u || page_bytes == 0u || raw_begin >= mapping_bytes) {
        return std::nullopt;
    }
    const std::size_t available = mapping_bytes - raw_begin;
    const std::size_t raw_end = raw_begin + std::min(raw_bytes, available);
    const std::size_t aligned_begin = raw_begin - raw_begin % page_bytes;
    std::size_t aligned_end = raw_end;
    const std::size_t remainder = aligned_end % page_bytes;
    if (remainder != 0u) {
        const std::size_t growth = page_bytes - remainder;
        aligned_end = growth > mapping_bytes - aligned_end ? mapping_bytes : aligned_end + growth;
    }
    if (aligned_end <= aligned_begin)
        return std::nullopt;
    return page_aligned_mapping_range{
        .begin = aligned_begin,
        .end = aligned_end,
    };
}

static_assert(page_aligned_advice_range(16'384u, 4'352u, 4'096u, 4'096u) ==
              page_aligned_mapping_range{4'096u, 12'288u});
static_assert(page_aligned_advice_range(10'000u, 9'000u, 1'000u, 4'096u) ==
              page_aligned_mapping_range{8'192u, 10'000u});
static_assert(!page_aligned_advice_range(10'000u, 10'000u, 1u, 4'096u));

[[nodiscard]] constexpr std::size_t file_advice_chunk_bytes(const std::size_t remaining) noexcept {
    return std::min<std::size_t>(remaining,
                                 static_cast<std::size_t>((std::numeric_limits<int>::max)()));
}

static_assert(file_advice_chunk_bytes(17u) == 17u);
static_assert(file_advice_chunk_bytes(static_cast<std::size_t>((std::numeric_limits<int>::max)()) +
                                      1u) ==
              static_cast<std::size_t>((std::numeric_limits<int>::max)()));

/// Ask the host to begin fetching one already-validated file range without
/// making success part of correctness. No error is logged or propagated: a
/// filesystem/kernel which rejects the preferred async file hint receives a
/// best-effort mapped-page hint, and rejection of that hint is also harmless.
inline void
advise_mapping_range_async_best_effort(const mapping_lease& lease,
                                       const page_aligned_mapping_range range) noexcept {
    if (lease.fd < 0 || lease.mapping == MAP_FAILED || lease.mapping == nullptr ||
        range.end <= range.begin || range.end > lease.mapping_bytes) {
        return;
    }

    bool file_advice_accepted = false;
#if defined(__APPLE__)
    std::size_t cursor = range.begin;
    file_advice_accepted = true;
    while (cursor < range.end) {
        const std::size_t remaining = range.end - cursor;
        const int count = static_cast<int>(file_advice_chunk_bytes(remaining));
        struct radvisory advice{
            .ra_offset = static_cast<off_t>(cursor),
            .ra_count = count,
        };
        int advised = -1;
        do {
            advised = ::fcntl(lease.fd, F_RDADVISE, &advice);
        } while (advised == -1 && errno == EINTR);
        if (advised == -1) {
            file_advice_accepted = false;
            break;
        }
        cursor += static_cast<std::size_t>(count);
    }
#elif defined(__linux__)
    int advised = 0;
    do {
        advised = ::posix_fadvise(lease.fd,
                                  static_cast<off_t>(range.begin),
                                  static_cast<off_t>(range.end - range.begin),
                                  POSIX_FADV_WILLNEED);
    } while (advised == EINTR);
    file_advice_accepted = advised == 0;
#endif

#if defined(MADV_WILLNEED)
    if (!file_advice_accepted) {
        auto* const address = static_cast<std::byte*>(lease.mapping) + range.begin;
        int advised = -1;
        do {
            advised = ::madvise(address, range.end - range.begin, MADV_WILLNEED);
        } while (advised == -1 && errno == EINTR);
        (void)advised;
    }
#else
    (void)file_advice_accepted;
#endif
}

/// Borrowed callback used by the PostgreSQL-free selected-page coalescer.
/// Advice rejection is intentionally invisible to query correctness.  The
/// absolute interval is translated only after proving it still belongs to the
/// stable mapping_lease which supplied the callback token.
inline void advise_borrowed_mapping_page_range(void* const context,
                                               const std::uintptr_t page_begin,
                                               const std::uintptr_t page_end) noexcept {
    auto* const lease = static_cast<mapping_lease*>(context);
    if (lease == nullptr || lease->owner == nullptr || !lease->remembered || lease->fd < 0 ||
        lease->mapping == MAP_FAILED || lease->mapping == nullptr || lease->mapping_bytes == 0u ||
        page_begin >= page_end) {
        return;
    }
    const std::uintptr_t mapping_begin = reinterpret_cast<std::uintptr_t>(lease->mapping);
    if (lease->mapping_bytes > (std::numeric_limits<std::uintptr_t>::max)() - mapping_begin) {
        return;
    }
    const std::uintptr_t mapping_end = mapping_begin + lease->mapping_bytes;
    if (page_begin < mapping_begin || page_begin >= mapping_end)
        return;
    const std::uintptr_t clipped_end = std::min(page_end, mapping_end);
    if (clipped_end <= page_begin)
        return;
    advise_mapping_range_async_best_effort(
        *lease,
        page_aligned_mapping_range{
            .begin = static_cast<std::size_t>(page_begin - mapping_begin),
            .end = static_cast<std::size_t>(clipped_end - mapping_begin),
        });
}

} // namespace minimizer_fragment_image_pg_detail

/// Shared header-last writer. Relation generations feed this from their
/// authenticated heap reread through the common grouped-SoA encoder callbacks.
class minimizer_fragment_image_writer {
  private:
    minimizer_generation_file_identity identity_{};
    pending_minimizer_generation_file_publication publication_{};
    std::uint64_t payload_bytes_ = 0u;
    crc64_ecma182_accumulator payload_crc_{};
    bool sealed_ = false;

  public:
    minimizer_fragment_image_writer(const minimizer_generation_administration_identity& identity,
                                    const std::uint64_t query_key,
                                    const std::uint64_t generation_id)
        : identity_(minimizer_fragment_image_pg_detail::generation_file_identity(
              identity, query_key, generation_id)),
          publication_(begin_generation_file_publication(identity_)) {}

    minimizer_fragment_image_writer(const minimizer_fragment_image_writer&) = delete;
    minimizer_fragment_image_writer& operator=(const minimizer_fragment_image_writer&) = delete;
    minimizer_fragment_image_writer(minimizer_fragment_image_writer&&) = delete;
    minimizer_fragment_image_writer& operator=(minimizer_fragment_image_writer&&) = delete;

    void append(const std::span<const std::byte> bytes) {
        if (sealed_ || publication_.fd < 0 ||
            bytes.size() > (std::numeric_limits<std::uint64_t>::max)() - payload_bytes_) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image payload append state/size is invalid");
        }
        minimizer_fragment_image_pg_detail::pwrite_all(
            publication_.fd,
            bytes,
            bioseq::indexing::k_minimizer_fragment_image_header_bytes + payload_bytes_);
        payload_crc_.update(bytes);
        payload_bytes_ += bytes.size();
    }

    [[nodiscard]] minimizer_fragment_image_file_receipt
    publish(const minimizer_generation_head& head,
            const minimizer_generation_manifest& manifest,
            const grouped_soa_directory_view& authenticated_directory,
            const std::span<const std::byte> encoded_manifest) {
        using namespace bioseq::indexing;
        const std::span<const std::byte> directory = authenticated_directory.encoded_bytes();
        minimizer_fragment_image_pg_detail::validate_generation_metadata(
            identity_, head, manifest, directory, encoded_manifest);
        if (sealed_ || publication_.fd < 0 || payload_bytes_ != manifest.payload_bytes) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image publication payload receipt is invalid");
        }
        const std::uint64_t payload_crc64 = payload_crc_.checksum();
        if (payload_bytes_ >
            (std::numeric_limits<std::uint64_t>::max)() - k_minimizer_fragment_image_header_bytes) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image payload mapping length overflows");
        }
        const std::uint64_t payload_mapping_bytes =
            k_minimizer_fragment_image_header_bytes + payload_bytes_;
        minimizer_fragment_image_pg_detail::resize_temporary_image(publication_.fd,
                                                                   payload_mapping_bytes);
        minimizer_fragment_image_pg_detail::scoped_mapped_region payload_mapping{
            publication_.fd, payload_mapping_bytes, PROT_READ};
        const std::span<const std::byte> mapped_payload = payload_mapping.bytes().subspan(
            static_cast<std::size_t>(k_minimizer_fragment_image_header_bytes),
            static_cast<std::size_t>(payload_bytes_));
        auto opened_adapter = minimizer_fragment_compact_hash32_adapter::open(
            opened_minimizer_fragment_generation_view{
                .head = &head,
                .manifest = &manifest,
                .directory = &authenticated_directory,
                .mapped_payload = mapped_payload,
            });
        if (!opened_adapter) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image compact-H32 source adapter rejected grouped-SoA");
        }
        minimizer_fragment_compact_hash32_adapter compact_adapter = std::move(*opened_adapter);
        const auto profile = derive_grouped_soa_minimizer_profile(manifest);
        if (!profile || profile->max_occ_cap == 0u ||
            profile->max_occ_cap > (std::numeric_limits<std::uint32_t>::max)()) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image persisted grouped-SoA cap is invalid");
        }
        const std::uint32_t persisted_max_occ_cap =
            static_cast<std::uint32_t>(profile->max_occ_cap);
        const auto compact_options = compact_adapter.compact_build_options(persisted_max_occ_cap);
        if (!compact_options) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image compact-H32 build options are incompatible");
        }
        const compact_hash32_sorted_run_source compact_source = compact_adapter.sorted_run_source();
        const auto compact_plan =
            plan_compact_hash32_direct_index(compact_source, *compact_options);
        if (!compact_plan) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image compact-H32 image planning failed");
        }

        const auto provisional_layout =
            plan_minimizer_fragment_image_layout(payload_bytes_,
                                                 directory.size(),
                                                 encoded_manifest.size(),
                                                 payload_crc64,
                                                 manifest.directory_checksum,
                                                 head.manifest_checksum,
                                                 minimizer_fragment_compact_hash32_section_spec{
                                                     .bytes = compact_plan->image_bytes,
                                                     .direct_header_crc64 = 0u,
                                                 });
        if (!provisional_layout ||
            provisional_layout->feature_bitmap != k_minimizer_fragment_compact_hash32_feature ||
            provisional_layout->compact_hash32_direct.bytes != compact_plan->image_bytes) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image direct-section layout planning failed");
        }
        const std::uint64_t payload_end =
            provisional_layout->payload.offset + provisional_layout->payload.bytes;
        minimizer_fragment_image_pg_detail::write_zero_padding(
            publication_.fd, payload_end, provisional_layout->directory.offset);
        minimizer_fragment_image_pg_detail::pwrite_all(
            publication_.fd, directory, provisional_layout->directory.offset);
        const std::uint64_t directory_end =
            provisional_layout->directory.offset + provisional_layout->directory.bytes;
        minimizer_fragment_image_pg_detail::write_zero_padding(
            publication_.fd, directory_end, provisional_layout->manifest.offset);
        minimizer_fragment_image_pg_detail::pwrite_all(
            publication_.fd, encoded_manifest, provisional_layout->manifest.offset);
        const std::uint64_t manifest_end =
            provisional_layout->manifest.offset + provisional_layout->manifest.bytes;
        minimizer_fragment_image_pg_detail::write_zero_padding(
            publication_.fd, manifest_end, provisional_layout->compact_hash32_direct.offset);
        minimizer_fragment_image_pg_detail::resize_temporary_image(publication_.fd,
                                                                   provisional_layout->file_bytes);

        std::uint64_t direct_header_crc64 = 0u;
        {
            minimizer_fragment_image_pg_detail::scoped_mapped_region destination_mapping{
                publication_.fd, provisional_layout->file_bytes, PROT_READ | PROT_WRITE};
            const auto direct_offset =
                static_cast<std::size_t>(provisional_layout->compact_hash32_direct.offset);
            const auto direct_bytes =
                static_cast<std::size_t>(provisional_layout->compact_hash32_direct.bytes);
            std::span<std::byte> direct_image =
                destination_mapping.writable_bytes().subspan(direct_offset, direct_bytes);
            if (auto built = build_compact_hash32_direct_index_into(
                    compact_source, *compact_options, *compact_plan, direct_image);
                !built) {
                minimizer_generation_storage_error_as_cpp(
                    "minimizer fragment image compact-H32 image replay/build failed");
            }
            const auto validated_direct =
                validate_compact_hash32_direct_index(std::span<const std::byte>{direct_image},
                                                     compact_options->identity,
                                                     compact_options->max_occ_cap,
                                                     compact_options->geometry);
            if (!validated_direct) {
                minimizer_generation_storage_error_as_cpp(
                    "minimizer fragment image compact-H32 nested image validation failed");
            }
            const compact_hash32_image_plan& validated_direct_plan = validated_direct->header;
            if (validated_direct_plan != *compact_plan) {
                minimizer_generation_storage_error_as_cpp(
                    "minimizer fragment image compact-H32 validated plan receipt changed");
            }
            direct_header_crc64 = crc64_ecma182(std::span<const std::byte>{direct_image}.first(
                k_compact_hash32_direct_index_header_bytes));
            destination_mapping.sync();
        }

        const auto layout =
            plan_minimizer_fragment_image_layout(payload_bytes_,
                                                 directory.size(),
                                                 encoded_manifest.size(),
                                                 payload_crc64,
                                                 manifest.directory_checksum,
                                                 head.manifest_checksum,
                                                 minimizer_fragment_compact_hash32_section_spec{
                                                     .bytes = compact_plan->image_bytes,
                                                     .direct_header_crc64 = direct_header_crc64,
                                                 });
        if (!layout || layout->payload != provisional_layout->payload ||
            layout->directory != provisional_layout->directory ||
            layout->manifest != provisional_layout->manifest ||
            layout->feature_bitmap != provisional_layout->feature_bitmap ||
            layout->compact_hash32_direct.offset !=
                provisional_layout->compact_hash32_direct.offset ||
            layout->compact_hash32_direct.bytes !=
                provisional_layout->compact_hash32_direct.bytes ||
            layout->file_bytes != provisional_layout->file_bytes) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image authenticated direct layout changed after build");
        }

        minimizer_fragment_image_header header{
            .magic = k_minimizer_fragment_image_magic,
            .layout_revision = k_minimizer_fragment_image_layout_revision,
            .header_bytes = k_minimizer_fragment_image_header_bytes,
            .database_oid = identity_.database_oid,
            .tablespace_oid = identity_.tablespace_oid,
            .main_relfilenode = identity_.main_relfilenode,
            .user_relid = identity_.user_relid,
            .column_attno = static_cast<std::uint16_t>(identity_.column_attno),
            .index_identity = k_minimizer_fragment_image_index_identity,
            .storage_kind =
                bioseq::indexing::k_minimizer_fragment_image_relation_backed_storage_kind,
            .layout_identity = k_minimizer_fragment_image_layout_identity,
            .query_key = identity_.query_key,
            .generation_id = identity_.generation_id,
            .source_identity = head.source_identity,
            .source_content_crc = head.source_content_checksum,
            .storage_contract_checksum = head.storage_contract_checksum,
            .target_length = manifest.target_length,
            .file_bytes = layout->file_bytes,
            .position_segment_count = manifest.position_segment_count,
            .position_segment_span_bp = manifest.position_segment_span_bp,
            .payload = layout->payload,
            .directory = layout->directory,
            .manifest = layout->manifest,
            .feature_bitmap = layout->feature_bitmap,
            .compact_hash32_direct = layout->compact_hash32_direct,
        };
        const auto encoded_header = encode_minimizer_fragment_image_header(header);
        if (!encoded_header) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image header encoding failed");
        }
        const auto decoded_header = decode_minimizer_fragment_image_header(*encoded_header);
        if (!decoded_header) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image header self-decode failed");
        }
        minimizer_fragment_image_pg_detail::pwrite_all(publication_.fd, *encoded_header, 0u);

        struct stat observed{};
        if (fstat(publication_.fd, &observed) != 0 || observed.st_size < 0 ||
            static_cast<std::uint64_t>(observed.st_size) != layout->file_bytes ||
            observed.st_dev != publication_.expected_device ||
            observed.st_ino != publication_.expected_inode) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer fragment image completed temp image size/inode is invalid");
        }
        const minimizer_fragment_image_validation_options validation_options{
            .expected_identity = minimizer_fragment_image_pg_detail::expected_identity(
                identity_, head, manifest, *layout, decoded_header->header_crc64),
            .payload_crc_policy = minimizer_fragment_payload_crc_policy::deferred,
        };
        {
            minimizer_fragment_image_pg_detail::scoped_mapped_region validation_mapping{
                publication_.fd, layout->file_bytes, PROT_READ};
            const auto validated =
                validate_minimizer_fragment_image(validation_mapping.bytes(), validation_options);
            if (!validated || validated->payload_crc_bytes != 0u) {
                minimizer_generation_storage_error_as_cpp(
                    "minimizer fragment image bounded publication validation failed");
            }
        }

        const minimizer_fragment_image_file_receipt receipt{
            .file_bytes = layout->file_bytes,
            .payload_crc64 = payload_crc64,
            .header_crc64 = decoded_header->header_crc64,
            .device = publication_.expected_device,
            .inode = publication_.expected_inode,
        };
        publish_generation_file(publication_);
        sealed_ = true;
        return receipt;
    }
};

enum class minimizer_fragment_image_mapped_open_validation : std::uint8_t {
    /// Timed restart path: authenticate only the fixed outer header, layout,
    /// descriptor identity and section receipts. No section payload is read.
    bounded_header_only = 0u,
    /// Explicit publication/scrub validation. Payload CRC remains
    /// deferred, while directory/manifest metadata receive complete
    /// validation.
    full_metadata = 1u,
};

/// Validated mapped image with borrowed section views and an explicit lease transfer.
class mapped_minimizer_fragment_image {
  private:
    minimizer_fragment_image_pg_detail::mapping_lease* lease_ = nullptr;
    std::span<const std::byte> payload_{};
    std::span<const std::byte> compact_h32_direct_{};
    std::uint64_t payload_raw_checksum_ = 0u;
    std::optional<std::uint64_t> compact_h32_direct_header_receipt_{};

    void clear_views_() noexcept {
        payload_ = {};
        compact_h32_direct_ = {};
        payload_raw_checksum_ = 0u;
        compact_h32_direct_header_receipt_.reset();
    }

    void release_() noexcept {
        clear_views_();
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease_);
    }

  public:
    mapped_minimizer_fragment_image() = default;
    mapped_minimizer_fragment_image(const mapped_minimizer_fragment_image&) = delete;
    mapped_minimizer_fragment_image& operator=(const mapped_minimizer_fragment_image&) = delete;
    mapped_minimizer_fragment_image(mapped_minimizer_fragment_image&& other) noexcept
        : lease_(std::exchange(other.lease_, nullptr)), payload_(std::exchange(other.payload_, {})),
          compact_h32_direct_(std::exchange(other.compact_h32_direct_, {})),
          payload_raw_checksum_(std::exchange(other.payload_raw_checksum_, 0u)),
          compact_h32_direct_header_receipt_(
              std::exchange(other.compact_h32_direct_header_receipt_, std::nullopt)) {}
    mapped_minimizer_fragment_image& operator=(mapped_minimizer_fragment_image&&) = delete;
    ~mapped_minimizer_fragment_image() {
        release_();
    }

    [[nodiscard]] std::span<const std::byte> payload() const noexcept {
        return payload_;
    }

    [[nodiscard]] std::uint64_t payload_raw_checksum() const noexcept {
        return payload_raw_checksum_;
    }

    /// Authenticated lazy span for the optional nested compact-H32 direct
    /// image. The bounded outer open formed this span arithmetically without
    /// reading any nested payload byte.
    [[nodiscard]] std::span<const std::byte> compact_h32_direct() const noexcept {
        return compact_h32_direct_;
    }

    /// Raw CRC64/ECMA receipt over the finalized fixed 256-byte nested header,
    /// persisted in and authenticated by the outer minimizer fragment image header.
    [[nodiscard]] std::optional<std::uint64_t> compact_h32_direct_header_receipt() const noexcept {
        return compact_h32_direct_header_receipt_;
    }

    /// Issue fail-open asynchronous readahead for the authenticated compact-
    /// H32 metadata only. Selected posting pages are advised later, while the
    /// existing descriptor layout traversal already knows their exact spans.
    /// The unrelated outer minimizer-fragment payload and unselected posting pages are never
    /// named as byte ranges. Page granularity may include adjacent bytes on a
    /// boundary page.
    void advise_compact_h32_direct_metadata_readahead(
        const bioseq::indexing::compact_hash32_direct_index_view& authenticated_direct)
        const noexcept {
        using minimizer_fragment_image_pg_detail::page_aligned_mapping_range;
        if (lease_ == nullptr || lease_->mapping == MAP_FAILED || lease_->mapping == nullptr ||
            lease_->mapping_bytes == 0u || compact_h32_direct_.empty() ||
            authenticated_direct.image.data() != compact_h32_direct_.data() ||
            authenticated_direct.image.size() != compact_h32_direct_.size()) {
            return;
        }

        const std::uintptr_t mapping_address = reinterpret_cast<std::uintptr_t>(lease_->mapping);
        const std::uintptr_t direct_address =
            reinterpret_cast<std::uintptr_t>(compact_h32_direct_.data());
        if (direct_address < mapping_address)
            return;
        const std::uint64_t direct_file_offset =
            static_cast<std::uint64_t>(direct_address - mapping_address);
        if (direct_file_offset > lease_->mapping_bytes ||
            compact_h32_direct_.size() >
                lease_->mapping_bytes - static_cast<std::size_t>(direct_file_offset)) {
            return;
        }

        const auto& header = authenticated_direct.header;
        if (header.image_bytes != compact_h32_direct_.size())
            return;
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 3u> direct_sections{
            std::pair{header.membership_offset, header.membership_bytes},
            std::pair{header.rank_offset, header.rank_bytes},
            std::pair{header.descriptors_offset, header.descriptors_bytes},
        };
        std::array<page_aligned_mapping_range, direct_sections.size()> merged_ranges{};
        std::size_t merged_count = 0u;
        const long observed_page_bytes = ::sysconf(_SC_PAGESIZE);
        if (observed_page_bytes <= 0)
            return;
        const std::size_t page_bytes = static_cast<std::size_t>(observed_page_bytes);

        std::uint64_t previous_section_end = 0u;
        for (const auto [section_offset, section_bytes] : direct_sections) {
            if (section_offset < previous_section_end ||
                section_offset > compact_h32_direct_.size() ||
                section_bytes >
                    compact_h32_direct_.size() - static_cast<std::size_t>(section_offset)) {
                return;
            }
            const std::uint64_t section_end = section_offset + section_bytes;
            previous_section_end = section_end;
            if (section_bytes == 0u)
                continue;

            if (section_offset > (std::numeric_limits<std::uint64_t>::max)() - direct_file_offset) {
                return;
            }
            const std::uint64_t file_offset = direct_file_offset + section_offset;
            if (file_offset > lease_->mapping_bytes ||
                section_bytes > lease_->mapping_bytes - static_cast<std::size_t>(file_offset)) {
                return;
            }
            const auto aligned = minimizer_fragment_image_pg_detail::page_aligned_advice_range(
                lease_->mapping_bytes,
                static_cast<std::size_t>(file_offset),
                static_cast<std::size_t>(section_bytes),
                page_bytes);
            if (!aligned)
                return;
            if (merged_count != 0u && aligned->begin <= merged_ranges[merged_count - 1u].end) {
                merged_ranges[merged_count - 1u].end =
                    std::max(merged_ranges[merged_count - 1u].end, aligned->end);
            } else {
                merged_ranges[merged_count++] = *aligned;
            }
        }
        if (previous_section_end > header.postings_offset ||
            header.postings_offset > header.image_bytes ||
            header.postings_bytes > header.image_bytes - header.postings_offset ||
            header.postings_offset + header.postings_bytes != header.image_bytes) {
            return;
        }
        for (std::size_t ordinal = 0u; ordinal < merged_count; ++ordinal) {
            minimizer_fragment_image_pg_detail::advise_mapping_range_async_best_effort(
                *lease_, merged_ranges[ordinal]);
        }
    }

    /// Form a non-owning adviser before this image transfers its lease into
    /// the longer-lived guard. The context is the mapping_lease allocation
    /// itself, whose address remains stable across that transfer.
    [[nodiscard]] bioseq::indexing::compact_direct_selected_page_adviser
    borrow_compact_h32_selected_page_adviser(
        const bioseq::indexing::compact_hash32_direct_index_view& authenticated_direct)
        const noexcept {
        if (lease_ == nullptr || lease_->owner == nullptr || !lease_->remembered ||
            lease_->fd < 0 || lease_->mapping == MAP_FAILED || lease_->mapping == nullptr ||
            lease_->mapping_bytes == 0u || compact_h32_direct_.empty() ||
            authenticated_direct.image.data() != compact_h32_direct_.data() ||
            authenticated_direct.image.size() != compact_h32_direct_.size() ||
            authenticated_direct.header.image_bytes != compact_h32_direct_.size()) {
            return {};
        }
        const std::uintptr_t mapping_begin = reinterpret_cast<std::uintptr_t>(lease_->mapping);
        const std::uintptr_t direct_begin =
            reinterpret_cast<std::uintptr_t>(compact_h32_direct_.data());
        if (lease_->mapping_bytes > (std::numeric_limits<std::uintptr_t>::max)() - mapping_begin ||
            direct_begin < mapping_begin) {
            return {};
        }
        const std::uintptr_t mapping_end = mapping_begin + lease_->mapping_bytes;
        if (compact_h32_direct_.size() >
                (std::numeric_limits<std::uintptr_t>::max)() - direct_begin ||
            direct_begin + compact_h32_direct_.size() > mapping_end) {
            return {};
        }
        const long observed_page_bytes = ::sysconf(_SC_PAGESIZE);
        if (observed_page_bytes <= 0)
            return {};
        return bioseq::indexing::compact_direct_selected_page_adviser{
            .context = lease_,
            .page_bytes = static_cast<std::size_t>(observed_page_bytes),
            .advise_fn = &minimizer_fragment_image_pg_detail::advise_borrowed_mapping_page_range,
        };
    }

    /// Borrow the PG-free reader's shared_ptr-shaped lifetime token while
    /// retaining this image as the prompt normal-path lease authority.
    [[nodiscard]] std::shared_ptr<const void> borrow_mapping_token() const noexcept {
        return minimizer_fragment_image_pg_detail::borrow_mapping_token(lease_);
    }

    /// Transfer only the mmap/fd lifetime authority into a compact guard.  Any
    /// spans copied before this call remain valid until the returned guard is
    /// destroyed. This moved-from image no longer exposes mapped spans.
    [[nodiscard]] mapped_minimizer_fragment_lease_guard release_mapping_lease_guard() && noexcept {
        auto* const lease = std::exchange(lease_, nullptr);
        clear_views_();
        return mapped_minimizer_fragment_lease_guard::adopt(lease);
    }

    friend mapped_minimizer_fragment_image
    open_exact_minimizer_fragment_image_pg(const minimizer_generation_administration_identity&,
                                           std::uint64_t,
                                           const minimizer_generation_head&,
                                           const minimizer_generation_manifest&,
                                           const minimizer_generation_storage_descriptor&,
                                           minimizer_fragment_image_mapped_open_validation);
};

[[nodiscard]] inline mapped_minimizer_fragment_image open_exact_minimizer_fragment_image_pg(
    const minimizer_generation_administration_identity& identity,
    const std::uint64_t query_key,
    const minimizer_generation_head& head,
    const minimizer_generation_manifest& manifest,
    const minimizer_generation_storage_descriptor& descriptor,
    const minimizer_fragment_image_mapped_open_validation validation) {
    using namespace bioseq::indexing;
    const minimizer_generation_file_identity file_identity =
        minimizer_fragment_image_pg_detail::generation_file_identity(
            identity, query_key, manifest.generation_id);
    if (manifest.layout != minimizer_generation_layout::grouped_soa_segment_local ||
        head.generation_id == 0u || head.generation_id != manifest.generation_id ||
        head.source_identity != manifest.source_identity ||
        head.source_content_checksum != manifest.source_content_checksum ||
        !exact_storage_descriptor_identity(descriptor, identity, query_key) ||
        descriptor.generation_id != manifest.generation_id ||
        descriptor.state != minimizer_generation_storage_state::live ||
        descriptor.retire_full_xid || !has_exact_storage_descriptor_relation_shape(descriptor)) {
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image open binding is invalid");
    }
    const resolved_minimizer_generation_file_path resolved =
        resolve_generation_file_path(file_identity);
    auto* lease = minimizer_fragment_image_pg_detail::allocate_mapping_lease();
    ResourceOwnerEnlarge(lease->owner);
    lease->fd = ::open(resolved.final_path.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | PG_BINARY);
    if (lease->fd < 0) {
        const int saved_errno = errno;
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease);
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "minimizer fragment image mapped image open failed: errno={}", saved_errno));
    }
    struct stat observed{};
    const bool inode_valid =
        fstat(lease->fd, &observed) == 0 && S_ISREG(observed.st_mode) &&
        observed.st_uid == geteuid() && (observed.st_mode & 07777) == 0400 &&
        observed.st_nlink == 1 &&
        observed.st_size >= static_cast<off_t>(k_minimizer_fragment_image_header_bytes) &&
        static_cast<std::uint64_t>(observed.st_size) <= (std::numeric_limits<std::size_t>::max)() &&
        static_cast<std::uint64_t>(observed.st_size) == descriptor.image_bytes;
    if (!inode_valid) {
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease);
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image mapped image inode contract is invalid");
    }
    lease->device = observed.st_dev;
    lease->inode = observed.st_ino;
    // No PostgreSQL call is permitted between the raw fd/inode acquisition
    // above and this pre-enlarged ResourceOwner registration.
    ResourceOwnerRemember(
        lease->owner,
        PointerGetDatum(lease),
        &minimizer_fragment_image_pg_detail::k_mapping_lease_resource_owner_descriptor);
    lease->remembered = true;
    lease->mapping_bytes = static_cast<std::size_t>(observed.st_size);
    lease->mapping = mmap(nullptr, lease->mapping_bytes, PROT_READ, MAP_PRIVATE, lease->fd, 0);
    if (lease->mapping == MAP_FAILED) {
        const int saved_errno = errno;
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease);
        minimizer_generation_storage_error_as_cpp(
            fmt::format("minimizer fragment image image mmap failed: errno={}", saved_errno));
    }
    const std::span<const std::byte> image{static_cast<const std::byte*>(lease->mapping),
                                           lease->mapping_bytes};
    encoded_minimizer_fragment_image_header encoded_header{};
    std::memcpy(encoded_header.data(), image.data(), encoded_header.size());
    const auto decoded_header = decode_minimizer_fragment_image_header(encoded_header);
    if (!decoded_header || decoded_header->header_crc64 != descriptor.header_checksum) {
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease);
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image mapped header disagrees with its LIVE descriptor");
    }
    std::optional<minimizer_fragment_compact_hash32_section_spec> compact_h32_direct;
    if ((decoded_header->feature_bitmap & k_minimizer_fragment_compact_hash32_feature) != 0u) {
        compact_h32_direct = minimizer_fragment_compact_hash32_section_spec{
            .bytes = decoded_header->compact_hash32_direct.bytes,
            .direct_header_crc64 = decoded_header->compact_hash32_direct.direct_header_crc64,
        };
    }
    const auto layout =
        plan_minimizer_fragment_image_layout(manifest.payload_bytes,
                                             manifest.directory_bytes,
                                             minimizer_generation_manifest::encoded_bytes,
                                             decoded_header->payload.crc64,
                                             manifest.directory_checksum,
                                             head.manifest_checksum,
                                             compact_h32_direct);
    if (!layout || descriptor.image_bytes != layout->file_bytes) {
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease);
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image descriptor layout is invalid");
    }
    const minimizer_fragment_image_validation_options options{
        .expected_identity = minimizer_fragment_image_pg_detail::expected_identity(
            file_identity, head, manifest, *layout, descriptor.header_checksum),
        .payload_crc_policy = minimizer_fragment_payload_crc_policy::deferred,
    };
    const auto validated =
        validation == minimizer_fragment_image_mapped_open_validation::bounded_header_only
            ? open_minimizer_fragment_image_bounded(image, options)
        : validation == minimizer_fragment_image_mapped_open_validation::full_metadata
            ? validate_minimizer_fragment_image(image, options)
            : std::expected<validated_minimizer_fragment_image_view,
                            minimizer_fragment_image_error>{
                  std::unexpected(minimizer_fragment_image_error::identity_mismatch)};
    const bool bounded_validation_exact =
        validated && validated->header_crc_bytes == k_minimizer_fragment_image_header_bytes &&
        validated->payload_crc_bytes == 0u &&
        (validation == minimizer_fragment_image_mapped_open_validation::bounded_header_only
             ? validated->directory_crc_bytes == 0u && validated->manifest_crc_bytes == 0u
             : validation == minimizer_fragment_image_mapped_open_validation::full_metadata &&
                   validated->directory_crc_bytes == manifest.directory_bytes &&
                   validated->manifest_crc_bytes == minimizer_generation_manifest::encoded_bytes);
    if (!bounded_validation_exact) {
        minimizer_fragment_image_pg_detail::destroy_mapping_lease(lease);
        minimizer_generation_storage_error_as_cpp(
            "minimizer fragment image mapped image validation failed");
    }
    mapped_minimizer_fragment_image result;
    result.lease_ = lease;
    result.payload_ = validated->payload;
    result.compact_h32_direct_ = validated->compact_hash32_direct;
    result.payload_raw_checksum_ = validated->header.payload.crc64;
    if (!validated->compact_hash32_direct.empty()) {
        result.compact_h32_direct_header_receipt_ =
            validated->header.compact_hash32_direct.direct_header_crc64;
    }
    return result;
}

} // namespace bioseq::storage::indexing
