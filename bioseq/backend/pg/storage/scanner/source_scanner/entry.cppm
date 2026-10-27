/**
 * @file scanner/source_scanner/entry.cppm
 * @brief Cache scanners in a read session and read ranks or packed sequence data.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "utils/errcodes.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <span>
#include <algorithm>
#include <array>
#include <expected>

export module bioseq.postgres.storage.scanner:source_scanner_entry;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.common.seqan_adapter;
import bioseq.postgres.storage.manager;
import bioseq.storage.scanner;          // Sequence source callbacks and read  sessions.
import bioseq.storage.lazy_sequence;    // Source-backed sequence types.
import bioseq.storage.coordinates;
import bioseq.storage.layout;
import bioseq.postgres.storage.physical;
import bioseq.storage.size;
import bioseq.postgres.storage.metadata;
import bioseq.sequences;                 // Packed sequence types and alphabet traits.
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.table.io;
import bioseq.parallelism.pool;
import bioseq.postgres.storage.scanner.fragment_scanner;
import :source_registry;

export namespace bioseq::storage {

namespace sequence_source_detail {

/// @return Unused letters in the locator's last packed unit for this sequence type.
/// @tparam p_sequence_instance Alphabet and packed storage type.
/// @param locator Locator whose length is measured in letters.
template <class p_sequence_instance>
[[nodiscard]] constexpr std::uint16_t
locator_padding_letters(const sequence_locator& locator) noexcept {
    using range = compact_sequence_range<p_sequence_instance>;
    if constexpr (range::traits::is_run_encoded) {
        return 0u;
    } else {
        constexpr std::uint64_t letters_per_unit = range::traits::letters_per_unit;
        const std::uint64_t remainder = locator.logical_length % letters_per_unit;
        return static_cast<std::uint16_t>(remainder == 0u ? 0u : letters_per_unit - remainder);
    }
}

/// Check the locator version, reserved fields, sourve epoch and fragment count.
/// @param stored_type Alphabet and packed-unit width used to interpret the length.
/// @param last_padding_letters Padding to check when `validate_padding` is true.
/// @return Whether the type is supported and all requested checks pass.
/// @param locator Locator, to check.
/// @param validate_padding Whether  to compare the supplied padding.
[[nodiscard]] inline bool locator_geometry_is_valid(const sequence_locator& locator,
                                                    compact_sequence_stdc_type_params stored_type,
                                                    std::uint16_t last_padding_letters,
                                                    bool validate_padding = true) {
    if (!is_supported_alphabet_code(stored_type.alphabet_code) ||
        !pgseq_supported_storage_unit_byte_widths::contains_runtime(stored_type.storage_unit_byte_width) ||
        (pgseq_is_cigar_alphabet(stored_type.alphabet_code) &&
            stored_type.storage_unit_byte_width != sizeof(bioseq::cigar_run))) return false;
    bool dispatched = false;
    bool valid = false;
    bioseq::pg_types::seq::dispatch_instance(
        stored_type.alphabet_code,
        stored_type.storage_unit_byte_width,
        [&dispatched,
         &valid,
         locator,
         last_padding_letters,
         validate_padding,
         stored_type]<class p_sequence_instance>() {
            using range = compact_sequence_range<p_sequence_instance>;
            constexpr bool is_rle = range::traits::is_run_encoded;
            constexpr std::uint64_t letters_per_fragment = [] {
                if constexpr (is_rle) {
                    return static_cast<std::uint64_t>(
                        postgres_sequence_scanner<p_sequence_instance>::fragment_storage_units);
                } else {
                    return static_cast<std::uint64_t>(
                        postgres_sequence_scanner<p_sequence_instance>::fragment_storage_units)
                            * range::traits::letters_per_unit;
                }
            }();
            const std::uint64_t extent = is_rle ? locator.encoded_payload_bytes : locator.logical_length;
            const std::uint64_t expected_fragments = extent / letters_per_fragment +
                (extent % letters_per_fragment != 0u ? 1u : 0u);
            const bool codec_valid = is_rle
                ? stored_type.encoding == bioseq::sequence_encoding::cigar_rle &&
                    bioseq::valid_cigar_space(stored_type.space) &&
                    ((locator.logical_length == 0u) == (locator.encoded_payload_bytes == 0u)) &&
                    locator.logical_length <= std::numeric_limits<std::uint32_t>::max()
                : stored_type.encoding == bioseq::sequence_encoding::rank_packed &&
                    stored_type.space == bioseq::cigar_space::compatibility &&
                    locator.encoded_payload_bytes == 0u;
            dispatched = true;
            valid = validate_sequence_locator(locator) == sequence_locator_validation_error::none &&
                    codec_valid && locator.fragment_count == expected_fragments &&
                    (!validate_padding ||
                     last_padding_letters == locator_padding_letters<p_sequence_instance>(locator));
        });
    return dispatched && valid;
}

/** Copy only requested canonical bytes, crossing cached physical fragments as needed. */
template<class p_sequence_instance>
inline void typed_scanner_read_encoded_bytes(source_scanner_entry& entry,
        std::uint64_t byte_offset, std::span<std::uint8_t> output) {
    using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
    auto& scanner = *static_cast<scanner_type*>(entry.concrete_scanner);
    const std::uint64_t available = scanner.bp_size() == 0u
        ? sizeof(bioseq::cigar_payload_header) : scanner.storage_byte_count();
    if (byte_offset > available || output.size() > available - byte_offset) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                errmsg("bioseq encoded source read exceeds its verified locator")));
        });
    }
    if (scanner.bp_size() == 0u) {
        std::fill(output.begin(), output.end(), std::uint8_t{0});
        return;
    }
    std::size_t copied = 0u;
    while (copied < output.size()) {
        const std::uint64_t physical = byte_offset + copied;
        const std::size_t fragment_no = static_cast<std::size_t>(physical / scanner_type::fragment_storage_units);
        const std::size_t local = static_cast<std::size_t>(physical % scanner_type::fragment_storage_units);
        const auto bytes = scanner.load_fragment(fragment_no);
        if (local >= bytes.size()) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                    errmsg("bioseq encoded source fragment is truncated")));
            });
        }
        const std::size_t count = std::min(output.size() - copied, bytes.size() - local);
        std::memcpy(output.data() + copied, bytes.data() + local, count);
        copied += count;
    }
}

/** Adapt the source scanner to the no-Postgres indexed codec without retaining scratch. */
template<class p_sequence_instance>
struct source_cigar_byte_reader {
    source_scanner_entry& entry;
    /** Read one exact byte interval into caller-owned codec scratch. */
    [[nodiscard]] std::expected<void, bioseq::cigar_error> read_bytes(
            std::size_t offset, std::span<std::byte> destination) const {
        typed_scanner_read_encoded_bytes<p_sequence_instance>(entry, offset,
            {reinterpret_cast<std::uint8_t*>(destination.data()), destination.size()});
        return {};
    }
};

/// Read an interval as decoded alphabet ranks, applying the requested transformation.
/// @param entry Entry holding the matching concrete scanner.
/// @param transform Forward, reverse, complement or reverse-complement order
/// @param source_from First letter of the source interval, counted from zero.
/// @param output Buffer that receives every rank on success.
/// @throws C++ exception for failed reads, truncated fragments, invalid ranks
/// or a complement operation  unsupported by the alphabet.
/// @tparam p_sequence_instance Alphabet and packed storage type.
template <class p_sequence_instance>
inline void typed_scanner_read_ranks(source_scanner_entry& entry,
                                     sequence_transform transform,
                                     std::uint64_t source_from,
                                     std::span<std::uint8_t> output) {
    using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
    using range = compact_sequence_range<p_sequence_instance>;
    using traits = typename range::traits;
    if constexpr (traits::is_run_encoded) {
        if (bioseq::storage::is_complemented(transform)) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("bioseq CIGAR alphabet has no complement operation")));
            });
        }
        using alphabet = typename p_sequence_instance::alphabet_type;
        const source_cigar_byte_reader<p_sequence_instance> bytes{entry};
        auto& scanner = *static_cast<scanner_type*>(entry.concrete_scanner);
        const auto space = entry.domain->stored_type.space;
        const bioseq::cigar_rle_reader<alphabet, source_cigar_byte_reader<p_sequence_instance>>
            reader{bytes, static_cast<std::uint32_t>(scanner.bp_size()), space};
        const auto header = reader.read_header();
        const auto seek = reader.seek(static_cast<std::uint32_t>(source_from));
        if (!header || !seek) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                    errmsg("bioseq CIGAR source directory is invalid")));
            });
        }
        std::uint32_t run_index = seek->run_index;
        std::uint32_t offset = seek->offset;
        std::size_t copied = 0u;
        while (copied < output.size()) {
            const auto run = reader.run_at(run_index++, header->run_count);
            if (!run || offset >= run->length) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                        errmsg("bioseq CIGAR source run is invalid")));
                });
            }
            const std::size_t count = std::min(output.size() - copied,
                static_cast<std::size_t>(run->length - offset));
            std::fill_n(output.data() + copied, count,
                static_cast<std::uint8_t>(seqan3::to_rank(run->operation)));
            copied += count;
            offset = 0u;
        }
        if (bioseq::storage::is_reversed(transform)) std::reverse(output.begin(), output.end());
    } else {
    using unit = typename traits::storage_unit;
    auto& scanner = *static_cast<scanner_type*>(entry.concrete_scanner);
    constexpr std::uint64_t letters_per_fragment =
        static_cast<std::uint64_t>(scanner_type::fragment_storage_units) * traits::letters_per_unit;
    const bool reverse = bioseq::storage::is_reversed(transform);
    const bool complement = bioseq::storage::is_complemented(transform);

    for (std::size_t output_index = 0u; output_index < output.size(); ++output_index) {
        const std::uint64_t physical =
            source_from + (reverse ? output.size() - 1u - output_index : output_index);
        const std::size_t fragment_no = static_cast<std::size_t>(physical / letters_per_fragment);
        const auto packed = scanner.load_fragment(fragment_no);
        const std::uint64_t local_symbol = physical % letters_per_fragment;
        const std::size_t unit_index =
            static_cast<std::size_t>(local_symbol / traits::letters_per_unit);
        if (unit_index >= packed.size()) {
            const auto packed_unit_count = packed.size();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [fragment_no, unit_index, packed_unit_count] {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("bioseq source scanner returned a truncated fragment"),
                             errdetail("fragment=%zu unit=%zu available_units=%zu",
                                       fragment_no,
                                       unit_index,
                                       packed_unit_count)));
                });
        }
        const unit word = packed[unit_index];
        const std::size_t shift =
            static_cast<std::size_t>(local_symbol % traits::letters_per_unit) *
            traits::bits_per_char;
        std::uint8_t rank = static_cast<std::uint8_t>((word >> shift) & traits::char_mask);
        if (rank >= seqan3::alphabet_size<typename p_sequence_instance::alphabet_type>) {
            bioseq::postgres::support::invoke_pg_as_cpp([fragment_no, local_symbol, rank] {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq source fragment contains an invalid rank"),
                         errdetail("fragment=%zu local_symbol=%llu rank=%u",
                                   fragment_no,
                                   static_cast<unsigned long long>(local_symbol),
                                   static_cast<unsigned>(rank))));
            });
        }
        if (complement) {
            if constexpr (requires(unit value) { traits::letter_complement(value); }) {
                rank =
                    static_cast<std::uint8_t>(traits::letter_complement(static_cast<unit>(rank)));
            } else {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("bioseq source alphabet has no complement operation")));
                });
            }
        }
        output[output_index] = rank;
    }
    }
}

/// Copy packed units across fragment boundaries without decoding letters.
/// @param entry Entry holding the matching concrete scanner.
/// @param first_unit First storage unit to read, counted from zero.
/// @param output Destination bytes. Its size must be a multiple of the unt width.
/// @throws C++ exception for an invalid range, truncated fragment or read failure.
/// @tparam p_sequence_instance Alphabet and packed storage type.
template <class p_sequence_instance>
inline void typed_scanner_read_packed_units(source_scanner_entry& entry,
                                            std::uint64_t first_unit,
                                            std::span<std::uint8_t> output) {
    using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
    using unit = typename scanner_type::storage_unit;
    auto& scanner = *static_cast<scanner_type*>(entry.concrete_scanner);
    if (output.size() % sizeof(unit) != 0u) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq packed source read has invalid byte geometry")));
        });
    }
    const std::uint64_t requested_units = output.size() / sizeof(unit);
    const std::uint64_t available_units = scanner.storage_unit_count();
    if (first_unit > available_units || requested_units > available_units - first_unit) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [first_unit, requested_units, available_units] {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq packed source read exceeds its verified locator"),
                         errdetail("first_unit=%llu requested_units=%llu available_units=%llu",
                                   static_cast<unsigned long long>(first_unit),
                                   static_cast<unsigned long long>(requested_units),
                                   static_cast<unsigned long long>(available_units))));
            });
    }

    std::uint64_t copied_units = 0u;
    while (copied_units < requested_units) {
        const std::uint64_t physical_unit = first_unit + copied_units;
        const std::size_t fragment_no = static_cast<std::size_t>(
            physical_unit / scanner_type::fragment_storage_units);
        const std::size_t local_unit = static_cast<std::size_t>(
            physical_unit % scanner_type::fragment_storage_units);
        const auto packed = scanner.load_fragment(fragment_no);
        if (local_unit >= packed.size()) {
            const std::size_t packed_unit_count = packed.size();
            bioseq::postgres::support::invoke_pg_as_cpp(
                [fragment_no, local_unit, packed_unit_count] {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("bioseq packed source read found a truncated fragment"),
                             errdetail("fragment=%zu unit=%zu available_units=%zu",
                                       fragment_no,
                                       local_unit,
                                       packed_unit_count)));
                });
        }
        const std::uint64_t available_here = packed.size() - local_unit;
        const std::uint64_t remaining = requested_units - copied_units;
        const std::size_t copy_units = static_cast<std::size_t>(
            available_here < remaining ? available_here : remaining);
        std::memcpy(output.data() + copied_units * sizeof(unit),
                    packed.data() + local_unit,
                    copy_units * sizeof(unit));
        copied_units += copy_units;
    }
}

/// Release PostgreSQL resources and destroy a successfully used  scanner.
/// @throws C++ exception if resource cleanup fails.
/// @tparam p_sequence_instance Alphabet and packed storage type.
/// @param entry Entry whose scanner is cleaned up and destroyed.
template <class p_sequence_instance>
inline void finish_typed_scanner_entry(source_scanner_entry& entry) {
    using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
    auto* scanner = static_cast<scanner_type*>(entry.concrete_scanner);
    if (scanner == nullptr || !entry.constructed) {
        return;
    }
    scanner->finish_pg_resources_as_cpp();
    scanner->~scanner_type();
    entry.concrete_scanner = nullptr;
    entry.constructed = false;
}

/// Destroy a failed scanner, leaving its PostgreSQL resources to parent cleanup.
/// @tparam p_sequence_instance Alphabet and packed storage type.
/// @param entry Entry whose scanner is abandoned and destroyed.
template <class p_sequence_instance>
inline void abort_typed_scanner_entry_noexcept(source_scanner_entry& entry) noexcept {
    using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
    auto* scanner = static_cast<scanner_type*>(entry.concrete_scanner);
    if (scanner == nullptr || !entry.constructed) {
        return;
    }
    scanner->abandon_pg_resources_noexcept();
    scanner->~scanner_type();
    entry.concrete_scanner = nullptr;
    entry.constructed = false;
}

/// Abort an entry and release its reference to the read domain.
/// A null pointer does nothing.
/// @param opaque Scanner entry pointer, or null.
inline void abort_source_scanner_entry_noexcept(void* opaque) noexcept {
    auto* entry = static_cast<source_scanner_entry*>(opaque);
    if (entry == nullptr) {
        return;
    }
    if (entry->abort_noexcept != nullptr) {
        entry->abort_noexcept(*entry);
    }
    if (entry->domain_retained) {
        entry->domain_retained = false;
        release_domain(entry->domain);
    }
    entry->domain = nullptr;
}

/// Finish a  scanner entry, then release its read-domain reference.
/// A null pointer does nothing
/// @throws PostgreSQL ERROR if scanner cleanup fails. C++ exceptions are
/// caught before reporting the failure to PostgreSQL.
/// @param opaque Scanner entry pointer, or null.
inline void finish_source_scanner_entry(void* opaque, bool) {
    auto* entry = static_cast<source_scanner_entry*>(opaque);
    if (entry == nullptr) {
        return;
    }
    ErrorData* pg_error_data = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    if (entry->finish != nullptr) {
        try {
            entry->finish(*entry);
        } catch (bioseq::postgres::support::postgres_error& error) {
            pg_error_data = error.release_error_data();
        } catch (const std::exception& error) {
            strlcpy(detail,
                    error.what() == nullptr ? "std::exception without detail" : error.what(),
                    sizeof(detail));
            cpp_error = true;
        } catch (...) {
            unknown_cpp_error = true;
        }
    }
    if (pg_error_data != nullptr) {
        ReThrowError(pg_error_data);
    }
    if (cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq source scanner finalization failed"),
                 errdetail_internal("%s", detail)));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq source scanner finalization failed"),
                 errdetail_internal("unknown C++ exception")));
    }
    if (entry->domain_retained) {
        entry->domain_retained = false;
        release_domain(entry->domain);
    }
    entry->domain = nullptr;
}

/// Create a, scanner for the stored alphabet and packed-unit width.
/// The entry holds a counted read-domain referene and lives in the session context.
/// @param opaque Borrowed `source_scanner_factory_argument`.
/// @param payload_context Context owning the entry and scanner.
/// @return Scanner entry allocated in `payload_context`.
/// @throws PostgreSQL ERROR if dispatch, reference counting or construction fails.
[[nodiscard]] inline void* create_source_scanner_entry(void* opaque,
                                                       MemoryContext payload_context) {
    auto const& factory = *static_cast<const source_scanner_factory_argument*>(opaque);
    auto* entry = static_cast<source_scanner_entry*>(
        MemoryContextAllocZero(payload_context, sizeof(source_scanner_entry)));
    entry->domain = factory.domain;
    bool dispatched = false;
    ErrorData* pg_error_data = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    try {
        bioseq::pg_types::seq::dispatch_instance(
            factory.stored_type.alphabet_code,
            factory.stored_type.storage_unit_byte_width,
            [entry,
             payload_context,
             locator = factory.locator,
             stored_type = factory.stored_type,
             &dispatched]<class p_sequence_instance>() {
                using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
                void* storage = bioseq::postgres::support::invoke_pg_as_cpp([payload_context] {
                    return MemoryContextAllocZero(payload_context, sizeof(scanner_type));
                });
                entry->concrete_scanner = storage;
                entry->read_ranks = &typed_scanner_read_ranks<p_sequence_instance>;
                if constexpr (compact_sequence_range<p_sequence_instance>::traits::is_run_encoded) {
                    entry->read_encoded_bytes = &typed_scanner_read_encoded_bytes<p_sequence_instance>;
                } else {
                    entry->read_packed_units = &typed_scanner_read_packed_units<p_sequence_instance>;
                }
                entry->finish = &finish_typed_scanner_entry<p_sequence_instance>;
                entry->abort_noexcept = &abort_typed_scanner_entry_noexcept<p_sequence_instance>;
                retain_domain(entry->domain);
                entry->domain_retained = true;
                if (!domain_is_readable(*entry->domain)) {
                    bioseq::postgres::support::invoke_pg_as_cpp([] {
                        ereport(ERROR,
                                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                                 errmsg("bioseq source domain became invalid while "
                                        "creating a scanner")));
                    });
                }
                auto* registry = entry->domain->registry;
                ::new (storage) scanner_type(registry->database_oid,
                                             registry->tablespace_oid,
                                             registry->main_relfilenode,
                                             entry->domain->column_attno,
                                             locator,
                                             locator_padding_letters<p_sequence_instance>(locator),
                                             entry->domain->registered_snapshot,
                                             entry->domain->read_caches,
                                             payload_context, stored_type.space);
                entry->constructed = true;
                dispatched = true;
            });
        if (!dispatched) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq source binding uses an unsupported sequence representation")));
            });
        }
    } catch (bioseq::postgres::support::postgres_error& error) {
        pg_error_data = error.release_error_data();
    } catch (const std::exception& error) {
        strlcpy(detail,
                error.what() == nullptr ? "std::exception without detail" : error.what(),
                sizeof(detail));
        cpp_error = true;
    } catch (...) {
        unknown_cpp_error = true;
    }
    if (pg_error_data != nullptr || cpp_error || unknown_cpp_error) {
        abort_source_scanner_entry_noexcept(entry);
    }
    if (pg_error_data != nullptr) {
        ReThrowError(pg_error_data);
    }
    if (cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq source scanner construction failed"),
                 errdetail_internal("%s", detail)));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq source scanner construction failed"),
                 errdetail_internal("unknown C++ exception")));
    }
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.scanner_entries_created;
#endif
    return entry;
}

/// Read decoded ranks through  a scanner cached by this session.
/// The caller must be on the domain's backend thread. Check that the domain
/// is still readable and the requested interval fits the locator before reading.
/// @param source_from First source letter, counted from zero.
/// @param output Buffer receiving the requested ranks.
/// @return `ok` once the buffer is filled.
/// @throws PostgreSQL ERROR for an invalid source, range or failed read.
/// @param domain Live domain owned by the current backend thread.
/// @param session Session that caches scanner entries.
/// @param locator Checked source locator.
/// @param stored_type Stored alphabet and packed-unit width.
/// @param transform Requested sequence transformation.
[[nodiscard]] inline pg_sequence_source_read_status
read_domain_ranks_on_backend(pg_sequence_source_domain* domain,
                             pg_sequence_read_session& session,
                             const sequence_locator& locator,
                             compact_sequence_stdc_type_params stored_type,
                             sequence_transform transform,
                             std::uint64_t source_from,
                             std::span<std::uint8_t> output) {
    if (!domain_is_readable(*domain)) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source transaction has ended"),
             errhint("materialize locator-backed values while their owning transaction is live")));
    }
    if (domain->stored_type != stored_type ||
        !locator_geometry_is_valid(locator, stored_type, 0u, /*validate_padding=*/false) ||
        locator.logical_length >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        source_from > locator.logical_length ||
        output.size() > locator.logical_length - source_from) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq source read request does not match its verified binding")));
    }
    if (output.empty()) {
        return pg_sequence_source_read_status::ok;
    }

    const pg_sequence_read_entry_key key{
        .source = domain_identity(domain),
        .locator = locator,
        .stored_type_params = stored_type,
    };
#ifdef BIOSEQ_TEST_INDEXES
    for (auto* candidate = session.entries; candidate != nullptr; candidate = candidate->next) {
        if (candidate->key.source == key.source &&
            same_locator(candidate->key.locator, key.locator) &&
            candidate->key.stored_type_params == key.stored_type_params) {
            ++g_pg_sequence_source_stats.scanner_entry_hits;
            break;
        }
    }
#endif
    source_scanner_factory_argument factory{
        .domain = domain,
        .locator = locator,
        .stored_type = stored_type,
    };
    auto* reader = static_cast<source_scanner_entry*>(
        find_or_create_pg_sequence_read_entry(session,
                                              key,
                                              &factory,
                                              create_source_scanner_entry,
                                              finish_source_scanner_entry,
                                              abort_source_scanner_entry_noexcept));
    if (reader == nullptr || reader->read_ranks == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq source read session is not accepting scanner entries")));
    }

#ifdef BIOSEQ_TEST_INDEXES
    /* Install  the entry and its abort callback before raising this test error.
     * The outer read wrapper converts it to a C++ exception so the lazy view
     * can abort its session before passing the error back to PostgreSQL. */
    inject_pg_sequence_source_failure(sequence_source_failure::read_before_scanner);
#endif

    const ResourceOwner saved_owner = CurrentResourceOwner;
    ErrorData* pg_error_data = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    try {
        CurrentResourceOwner = domain->resource_owner;
        reader->read_ranks(*reader, transform, source_from, output);
        CurrentResourceOwner = saved_owner;
    } catch (bioseq::postgres::support::postgres_error& error) {
        CurrentResourceOwner = saved_owner;
        pg_error_data = error.release_error_data();
    } catch (const std::exception& error) {
        CurrentResourceOwner = saved_owner;
        strlcpy(detail,
                error.what() == nullptr ? "std::exception without detail" : error.what(),
                sizeof(detail));
        cpp_error = true;
    } catch (...) {
        CurrentResourceOwner = saved_owner;
        unknown_cpp_error = true;
    }
    if (pg_error_data != nullptr) {
        ReThrowError(pg_error_data);
    }
    if (cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq source scanner read failed"),
                 errdetail_internal("%s", detail)));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq source scanner read failed"),
                 errdetail_internal("unknown C++ exception")));
    }
    return pg_sequence_source_read_status::ok;
}

/// Copy packed units through a scanner cached by this session.
/// The caller must be on the domain's backend thread Validate the sxurce
/// and requested range before reading.
/// @param first_unit First packed unit, counted from zero.
/// @param output Destination bytes, sized to a multiple of the stored unit width.
/// @return `ok` once the requested units are copied.
/// @throws PostgreSQL ERROR for an invalid  source, range or failed read.
/// @param domain Live domain owned by the current backend thread.
/// @param session Session that caches scanner entries.
/// @param locator Checked source locator.
/// @param stored_type Stored alphabet and packed-unit width.
enum class source_storage_read_kind : std::uint8_t { packed_units, encoded_bytes };

template<source_storage_read_kind Kind>
[[nodiscard]] inline pg_sequence_source_read_status
read_domain_storage_bytes_on_backend(pg_sequence_source_domain* domain,
                                    pg_sequence_read_session& session,
                                    const sequence_locator& locator,
                                    compact_sequence_stdc_type_params stored_type,
                                    std::uint64_t first_unit,
                                    std::span<std::uint8_t> output) {
    if (!domain_is_readable(*domain)) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source transaction has ended"),
             errhint("consume locator-backed values while their owning transaction is live")));
    }
    if (domain->stored_type != stored_type ||
        !locator_geometry_is_valid(locator, stored_type, 0u, /*validate_padding=*/false)) {
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
            errmsg("bioseq source bytes do not match their verified representation")));
    }
    bool type_dispatched = false;
    std::uint64_t available_units = 0u;
    bioseq::pg_types::seq::dispatch_instance(
        stored_type.alphabet_code,
        stored_type.storage_unit_byte_width,
        [&type_dispatched, &available_units, locator]<class p_sequence_instance>() {
            using range = compact_sequence_range<p_sequence_instance>;
            if constexpr (Kind == source_storage_read_kind::encoded_bytes) {
                if constexpr (range::traits::is_run_encoded) {
                    available_units = locator.logical_length == 0u
                        ? sizeof(bioseq::cigar_payload_header) : locator.encoded_payload_bytes;
                    type_dispatched = true;
                }
            } else if constexpr (!range::traits::is_run_encoded) {
                constexpr std::uint64_t letters_per_unit = range::traits::letters_per_unit;
                available_units = locator.logical_length / letters_per_unit +
                                  (locator.logical_length % letters_per_unit != 0u ? 1u : 0u);
                type_dispatched = true;
            }
        });
    const std::size_t unit_width = Kind == source_storage_read_kind::encoded_bytes
        ? 1u : stored_type.storage_unit_byte_width;
    const bool byte_geometry_valid = unit_width != 0u && output.size() % unit_width == 0u;
    const std::uint64_t requested_units = byte_geometry_valid ? output.size() / unit_width : 0u;
    if (!type_dispatched ||
        !byte_geometry_valid ||
        first_unit > available_units || requested_units > available_units - first_unit) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq packed source read does not match its verified binding")));
    }
    if (output.empty()) {
        return pg_sequence_source_read_status::ok;
    }

    const pg_sequence_read_entry_key key{
        .source = domain_identity(domain),
        .locator = locator,
        .stored_type_params = stored_type,
    };
#ifdef BIOSEQ_TEST_INDEXES
    for (auto* candidate = session.entries; candidate != nullptr; candidate = candidate->next) {
        if (candidate->key.source == key.source &&
            same_locator(candidate->key.locator, key.locator) &&
            candidate->key.stored_type_params == key.stored_type_params) {
            ++g_pg_sequence_source_stats.scanner_entry_hits;
            break;
        }
    }
#endif
    source_scanner_factory_argument factory{
        .domain = domain,
        .locator = locator,
        .stored_type = stored_type,
    };
    auto* reader = static_cast<source_scanner_entry*>(
        find_or_create_pg_sequence_read_entry(session,
                                              key,
                                              &factory,
                                              create_source_scanner_entry,
                                              finish_source_scanner_entry,
                                              abort_source_scanner_entry_noexcept));
    if (reader == nullptr ||
        (Kind == source_storage_read_kind::encoded_bytes
            ? reader->read_encoded_bytes == nullptr : reader->read_packed_units == nullptr)) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq source read session has no packed scanner capability")));
    }

#ifdef BIOSEQ_TEST_INDEXES
    inject_pg_sequence_source_failure(sequence_source_failure::read_before_scanner);
#endif

    const ResourceOwner saved_owner = CurrentResourceOwner;
    ErrorData* pg_error_data = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    try {
        CurrentResourceOwner = domain->resource_owner;
        if constexpr (Kind == source_storage_read_kind::encoded_bytes) {
            reader->read_encoded_bytes(*reader, first_unit, output);
        } else {
            reader->read_packed_units(*reader, first_unit, output);
        }
        CurrentResourceOwner = saved_owner;
    } catch (bioseq::postgres::support::postgres_error& error) {
        CurrentResourceOwner = saved_owner;
        pg_error_data = error.release_error_data();
    } catch (const std::exception& error) {
        CurrentResourceOwner = saved_owner;
        strlcpy(detail,
                error.what() == nullptr ? "std::exception without detail" : error.what(),
                sizeof(detail));
        cpp_error = true;
    } catch (...) {
        CurrentResourceOwner = saved_owner;
        unknown_cpp_error = true;
    }
    if (pg_error_data != nullptr) {
        ReThrowError(pg_error_data);
    }
    if (cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq packed source scanner read failed"),
                 errdetail_internal("%s", detail)));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq packed source scanner read failed"),
                 errdetail_internal("unknown C++ exception")));
    }
    return pg_sequence_source_read_status::ok;
}

/// Read ranks after checking that the domain and session belong to this thread.
/// @return `wrong_thread` for a missing domain or mismatched thread.
/// @throws C++ exception, if the PostgreSQL read fails.
/// @param opaque Source-domain pointer.
/// @param session Session whose backend thread must match the domain.
[[nodiscard]] inline pg_sequence_source_read_status
read_domain_ranks(const void* opaque,
                  pg_sequence_read_session& session,
                  const sequence_locator& locator,
                  compact_sequence_stdc_type_params stored_type,
                  sequence_transform transform,
                  std::uint64_t source_from,
                  std::span<std::uint8_t> output) {
    auto* domain = const_cast<pg_sequence_source_domain*>(
        static_cast<const pg_sequence_source_domain*>(opaque));
    /* Check the thread before calling invoke_pg_as_cpp. PostgreSQL's error
     * handlers and ErrorData can only be used on the backend thread. */
    if (domain == nullptr || !domain->backend_thread.is_current() ||
        !session.backend_thread.is_current() ||
        domain->backend_thread.owner != session.backend_thread.owner) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.wrong_thread_rejections;
#endif
        return pg_sequence_source_read_status::wrong_thread;
    }
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [domain, &session, locator, stored_type, transform, source_from, output] {
            return read_domain_ranks_on_backend(
                domain, session, locator, stored_type, transform, source_from, output);
        });
}

/// Copy packed units after checking the  domain and session belong to this thread.
/// @return `wrong_thread` for a missing domain or mismatched thread.
/// @throws C++ exception if the PostgreSQL read fails.
template<source_storage_read_kind Kind>
[[nodiscard]] inline pg_sequence_source_read_status
read_domain_storage_bytes(const void* opaque,
                         pg_sequence_read_session& session,
                         const sequence_locator& locator,
                         compact_sequence_stdc_type_params stored_type,
                         std::uint64_t first_unit,
                         std::span<std::uint8_t> output) {
    auto* domain = const_cast<pg_sequence_source_domain*>(
        static_cast<const pg_sequence_source_domain*>(opaque));
    if (domain == nullptr || !domain->backend_thread.is_current() ||
        !session.backend_thread.is_current() ||
        domain->backend_thread.owner != session.backend_thread.owner) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.wrong_thread_rejections;
#endif
        return pg_sequence_source_read_status::wrong_thread;
    }
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [domain, &session, locator, stored_type, first_unit, output] {
            return read_domain_storage_bytes_on_backend<Kind>(
                domain, session, locator, stored_type, first_unit, output);
        });
}

/// @return Shared callbacks for counting references and identifying and reading a domain.
[[nodiscard]] inline const pg_sequence_source_backend_ops& source_backend_ops() noexcept {
    static const pg_sequence_source_backend_ops ops{
        .retain = retain_domain,
        .release = release_domain,
        .identity = domain_identity,
        .worker_shareable = domain_worker_shareable,
        .read_ranks = read_domain_ranks,
        .read_packed_units = read_domain_storage_bytes<source_storage_read_kind::packed_units>,
        .read_encoded_bytes = read_domain_storage_bytes<source_storage_read_kind::encoded_bytes>,
    };
    return ops;
}

} // namespace sequence_source_detail

} // namespace bioseq::storage
