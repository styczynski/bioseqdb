/**
 * @file supervisor_pg_persist_queue_impl.cppm
 * @brief Shared asynchronous persistence publication and cleanup implementation.
 */
module;

#include "common.h"

extern "C" {
#include "port/atomics.h"
#include "storage/dsm.h"
#include "storage/dsm_impl.h"
}

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

module bioseq.scheduler.supervisor_pg;

import bioseq.common.mlogging;
import bioseq.indexing.type;

namespace bioseq::parallelism::pg {

struct IndexPersistDsmHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t chunk_count;
    std::uint32_t index_name_len;
    std::uint64_t payload_bytes;
};

struct IndexPersistDsmChunk {
    std::int64_t sub_key;
    std::uint32_t chunk_no;
    std::uint32_t reserved;
    std::uint64_t offset;
    std::uint64_t size;
};

struct IndexPersistFileDsmHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t index_name_len;
    std::uint32_t path_len;
    std::uint64_t file_bytes;
    std::uint64_t chunk_count;
};

struct PersistFileChunkHeader {
    std::int64_t sub_key;
    std::uint32_t chunk_no;
    std::uint32_t reserved;
    std::uint64_t size;
};

inline constexpr std::uint32_t kIndexPersistDsmMagic = 0xB1051D58u;
inline constexpr std::uint32_t kIndexPersistDsmVersion = 1;
inline constexpr std::uint32_t kIndexPersistFileDsmMagic = 0xB1051D59u;
inline constexpr std::uint32_t kIndexPersistFileDsmVersion = 1;
inline std::atomic<std::uint64_t> g_prohibited_legacy_persist_rejections{0};

void record_prohibited_legacy_persist_rejection(
        std::string_view route) noexcept {
    g_prohibited_legacy_persist_rejections.fetch_add(
        1, std::memory_order_relaxed);
    bioseq_log<DBG>(
        "[bioseq] ERROR prohibited legacy async persistence rejected route={} total_rejections={}",
        route,
        static_cast<unsigned long long>(
            g_prohibited_legacy_persist_rejections.load(
                std::memory_order_relaxed)));
}

[[nodiscard]] std::uint64_t align_up_u64(std::uint64_t value, std::uint64_t alignment) noexcept {
    return alignment == 0
        ? value
        : ((value + alignment - 1) / alignment) * alignment;
}

void remove_temp_file_noexcept(std::string_view path) noexcept {
    if (path.empty()) {
        return;
    }
    std::string owned{path};
    std::remove(owned.c_str());
}

[[nodiscard]] std::string file_path_from_metadata_dsm(dsm_handle handle) {
    if (handle == DSM_HANDLE_INVALID) {
        return {};
    }
    dsm_segment* seg = dsm_attach(handle);
    if (seg == nullptr) {
        return {};
    }
    const Size mapped = dsm_segment_map_length(seg);
    std::string path;
    if (mapped >= sizeof(IndexPersistFileDsmHeader)) {
        char* base = static_cast<char*>(dsm_segment_address(seg));
        IndexPersistFileDsmHeader header{};
        std::memcpy(&header, base, sizeof(header));
        if (header.magic == kIndexPersistFileDsmMagic
            && header.version == kIndexPersistFileDsmVersion
            && header.index_name_len > 0
            && header.path_len > 0
            && sizeof(IndexPersistFileDsmHeader) + header.index_name_len
                    <= static_cast<std::uint64_t>(mapped)
            && sizeof(IndexPersistFileDsmHeader) + header.index_name_len
                    + header.path_len
                    <= static_cast<std::uint64_t>(mapped)) {
            const char* path_begin =
                base + sizeof(IndexPersistFileDsmHeader) + header.index_name_len;
            path.assign(path_begin, path_begin + header.path_len);
        }
    }
    dsm_detach(seg);
    return path;
}

void remove_file_payload_if_present_noexcept(dsm_handle handle) noexcept {
    try {
        remove_temp_file_noexcept(file_path_from_metadata_dsm(handle));
    } catch (...) {
    }
}

struct PersistFileMetadata {
    std::string index_name{};
    std::string path{};
    std::uint64_t file_bytes = 0;
    std::uint64_t chunk_count = 0;
};

[[nodiscard]] std::optional<PersistFileMetadata>
parse_persist_file_metadata(const char* base, Size mapped) {
    if (mapped < sizeof(IndexPersistFileDsmHeader)) {
        return std::nullopt;
    }
    IndexPersistFileDsmHeader header{};
    std::memcpy(&header, base, sizeof(header));
    if (header.magic != kIndexPersistFileDsmMagic
        || header.version != kIndexPersistFileDsmVersion
        || header.index_name_len == 0
        || header.path_len == 0) {
        return std::nullopt;
    }
    const std::uint64_t names_offset = sizeof(IndexPersistFileDsmHeader);
    const std::uint64_t path_offset = names_offset + header.index_name_len;
    const std::uint64_t path_end = path_offset + header.path_len;
    if (path_offset < names_offset
        || path_end < path_offset
        || path_end > static_cast<std::uint64_t>(mapped)) {
        return std::nullopt;
    }
    PersistFileMetadata meta{};
    meta.index_name.assign(base + names_offset, base + path_offset);
    meta.path.assign(base + path_offset, base + path_end);
    meta.file_bytes = header.file_bytes;
    meta.chunk_count = header.chunk_count;
    return meta;
}

[[nodiscard]] bool stage_async_index_persist(
        const PersistTarget& target,
        std::span<const PersistChunk> chunks) {
    (void)target;
    (void)chunks;
    record_prohibited_legacy_persist_rejection("stage_dsm_chunks");
    return false;
#if 0  // Deleted with the identity-queue replacement; retained only for archaeology.
    const std::uint64_t stage_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    if (!bioseq::settings::index_async_persist || chunks.empty() || target.index_name.empty()) {
        return false;
    }
    if (!async_persist_queue_available()) {
        bioseq_log<DBG>("[bioseq] index: async persist skipped row={} index={} because shared supervisor queue is unavailable",
                static_cast<unsigned long long>(target.sequence_rowno),
                target.index_name);
        return false;
    }
    ensure_persist_xact_callback_registered();
    std::uint64_t bytes_total = sizeof(IndexPersistDsmHeader);
    bytes_total += target.index_name.size();
    bytes_total = align_up_u64(bytes_total, alignof(IndexPersistDsmChunk));
    const std::uint64_t directory_offset = bytes_total;
    bytes_total += static_cast<std::uint64_t>(chunks.size()) * sizeof(IndexPersistDsmChunk);
    bytes_total = align_up_u64(bytes_total, alignof(std::max_align_t));
    const std::uint64_t payload_offset = bytes_total;
    for (const PersistChunk& chunk : chunks) {
        bytes_total = align_up_u64(bytes_total, alignof(std::max_align_t));
        bytes_total += chunk.bytes.size();
    }
    if (bytes_total == 0 || bytes_total > static_cast<std::uint64_t>((std::numeric_limits<Size>::max)())) {
        return false;
    }

    auto permit =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(bytes_total);
    if (!permit) {
        bioseq_log<DBG>("[bioseq] index: async persist staging skipped by memory admission (bytes={})",
                static_cast<unsigned long long>(bytes_total));
        return false;
    }

    dsm_segment* seg = dsm_create(static_cast<Size>(bytes_total), DSM_CREATE_NULL_IF_MAXSEGMENTS);
    if (seg == nullptr) {
        return false;
    }
    char* base = static_cast<char*>(dsm_segment_address(seg));
    IndexPersistDsmHeader header{
        kIndexPersistDsmMagic,
        kIndexPersistDsmVersion,
        static_cast<std::uint32_t>(chunks.size()),
        static_cast<std::uint32_t>(target.index_name.size()),
        bytes_total,
    };
    std::memcpy(base, &header, sizeof(header));
    std::memcpy(base + sizeof(header), target.index_name.data(), target.index_name.size());
    auto* directory = reinterpret_cast<IndexPersistDsmChunk*>(base + directory_offset);
    std::uint64_t cursor = payload_offset;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const PersistChunk& chunk = chunks[i];
        cursor = align_up_u64(cursor, alignof(std::max_align_t));
        directory[i] = IndexPersistDsmChunk{
            .sub_key = static_cast<std::int64_t>(chunk.sub_key),
            .chunk_no = chunk.chunk_no,
            .reserved = 0,
            .offset = cursor,
            .size = static_cast<std::uint64_t>(chunk.bytes.size()),
        };
        if (!chunk.bytes.empty()) {
            std::memcpy(base + cursor, chunk.bytes.data(), chunk.bytes.size());
        }
        cursor += chunk.bytes.size();
    }
    dsm_pin_segment(seg);
    const dsm_handle handle = dsm_segment_handle(seg);
    dsm_detach(seg);
    const std::uint64_t dsm_ready_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());

    PendingPersistJob job{};
    job.target = target;
    job.index_name.assign(target.index_name.begin(), target.index_name.end());
    job.target.index_name = job.index_name;
    job.dsm = handle;
    job.payload_bytes = bytes_total;
    job.payload_kind = PersistPayloadKind::prohibited_legacy_dsm;
    job.permit = std::move(*permit);
    pending_persist_jobs().push_back(std::move(job));
    const std::uint64_t queued_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    bioseq_log<DBG>("[bioseq] index: staged async persist job row={} index={} chunks={} bytes={} dsm_copy_ns={} total_stage_ns={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            pending_persist_jobs().back().index_name,
            chunks.size(),
            static_cast<unsigned long long>(bytes_total),
            static_cast<unsigned long long>(dsm_ready_ns - stage_start_ns),
            static_cast<unsigned long long>(queued_ns - stage_start_ns));
    return true;
#endif
}

[[nodiscard]] bool stage_async_index_persist_file(
        const PersistTarget& target,
        std::string temp_file_path,
        std::uint64_t file_bytes,
        std::uint64_t chunk_count) {
    (void)target;
    (void)file_bytes;
    (void)chunk_count;
    remove_temp_file_noexcept(temp_file_path);
    record_prohibited_legacy_persist_rejection("stage_external_file");
    return false;
#if 0  // Deleted with the identity-queue replacement; retained only for archaeology.
    const std::uint64_t stage_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    if (!bioseq::settings::index_async_persist
        || target.index_name.empty()
        || temp_file_path.empty()
        || chunk_count == 0) {
        remove_temp_file_noexcept(temp_file_path);
        return false;
    }
    if (!async_persist_queue_available()) {
        bioseq_log<DBG>("[bioseq] index: file async persist skipped row={} index={} chunks={} file_bytes={} because shared supervisor queue is unavailable",
                static_cast<unsigned long long>(target.sequence_rowno),
                target.index_name,
                static_cast<unsigned long long>(chunk_count),
                static_cast<unsigned long long>(file_bytes));
        remove_temp_file_noexcept(temp_file_path);
        return false;
    }
    ensure_persist_xact_callback_registered();

    std::uint64_t bytes_total = sizeof(IndexPersistFileDsmHeader);
    bytes_total += target.index_name.size();
    bytes_total += temp_file_path.size();
    if (bytes_total == 0
        || bytes_total
            > static_cast<std::uint64_t>((std::numeric_limits<Size>::max)())) {
        remove_temp_file_noexcept(temp_file_path);
        return false;
    }

    dsm_segment* seg = dsm_create(
        static_cast<Size>(bytes_total),
        DSM_CREATE_NULL_IF_MAXSEGMENTS);
    if (seg == nullptr) {
        remove_temp_file_noexcept(temp_file_path);
        return false;
    }
    char* base = static_cast<char*>(dsm_segment_address(seg));
    IndexPersistFileDsmHeader header{
        kIndexPersistFileDsmMagic,
        kIndexPersistFileDsmVersion,
        static_cast<std::uint32_t>(target.index_name.size()),
        static_cast<std::uint32_t>(temp_file_path.size()),
        file_bytes,
        chunk_count,
    };
    std::memcpy(base, &header, sizeof(header));
    std::memcpy(
        base + sizeof(header),
        target.index_name.data(),
        target.index_name.size());
    std::memcpy(
        base + sizeof(header) + target.index_name.size(),
        temp_file_path.data(),
        temp_file_path.size());
    dsm_pin_segment(seg);
    const dsm_handle handle = dsm_segment_handle(seg);
    dsm_detach(seg);

    PendingPersistJob job{};
    job.target = target;
    job.index_name.assign(target.index_name.begin(), target.index_name.end());
    job.target.index_name = job.index_name;
    job.temp_file_path = std::move(temp_file_path);
    job.dsm = handle;
    job.payload_bytes = file_bytes;
    job.file_backed = true;
    pending_persist_jobs().push_back(std::move(job));
    const std::uint64_t queued_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    bioseq_log<DBG>("[bioseq] index: staged file async persist job row={} index={} chunks={} file_bytes={} meta_bytes={} total_stage_ns={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            pending_persist_jobs().back().index_name,
            static_cast<unsigned long long>(chunk_count),
            static_cast<unsigned long long>(file_bytes),
            static_cast<unsigned long long>(bytes_total),
            static_cast<unsigned long long>(queued_ns - stage_start_ns));
    return true;
#endif
}

} // namespace bioseq::parallelism::pg
