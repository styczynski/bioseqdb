/**
 * @file supervisor_pg_persist_queue_metadata_impl.cppm
 * @brief Legacy file-payload metadata cleanup helpers.
 */
module;

#include "common.h"

extern "C" {
#include "storage/dsm.h"
#include "storage/dsm_impl.h"
}

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

module bioseq.scheduler.supervisor_pg;
import :persist_queue_publish;

namespace bioseq::parallelism::pg {

struct IndexPersistFileDsmHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t index_name_len;
    std::uint32_t path_len;
    std::uint64_t file_bytes;
    std::uint64_t chunk_count;
};

inline constexpr std::uint32_t kIndexPersistFileDsmMagic = 0xB1051D59u;
inline constexpr std::uint32_t kIndexPersistFileDsmVersion = 1;

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
                base + sizeof(IndexPersistFileDsmHeader)
                + header.index_name_len;
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

} // namespace bioseq::parallelism::pg
