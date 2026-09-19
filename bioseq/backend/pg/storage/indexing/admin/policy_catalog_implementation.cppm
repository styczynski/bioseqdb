/**
 * @file admin/policy_catalog_implementation.cppm
 * @brief Implements policy operations that instantiate chunked storage handles.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/relation.h"
#include "catalog/namespace.h"
#include "utils/errcodes.h"
#include "utils/lsyscache.h"
}

// Clang 22 must see these textual libc++ declarations before deserializing
// header-backed dependency BMIs; importing std first makes aligned operator new
// appear as two unrelated candidates during container allocation.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>

module bioseq.postgres.storage.indexing.admin.policy_catalog;

import bioseq.indexing.type;
import bioseq.pipeline.runtime_contract;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.fragment_handle;
import bioseq.postgres.storage.indexing.index_relation_identity;
import bioseq.postgres.storage.indexing.supported_indexes;
import bioseq.storage.lazy_sequence;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences;

namespace bioseq::storage::indexing {

void note_successful_write(Oid user_relid, Oid relfn, std::int16_t attno, std::string_view name) {
    static_cast<void>(upsert_metadata_age_log_pg(user_relid, relfn, attno, name));
}

/// Record successful INSERT maintenance for every enabled per-insert index.
void note_policy_on_insert_writes(Oid user_relid,
                                  Oid relfn,
                                  std::int16_t attno,
                                  const pg_supported_indexes& idx) {
    constexpr auto k_index_types = pg_supported_indexes::list_types();
    for (std::size_t i = 0; i < k_index_types.size(); ++i) {
        if (idx.enabled[i]
            && idx.policy[i]
                == bioseq::indexing::index_update_policy::update_on_insert) {
            note_successful_write(user_relid, relfn, attno, k_index_types[i].first);
        }
    }
}

void index_build_on_insert(Oid user_relid,
                           std::int16_t attno,
                           std::uint64_t rowno,
                           const bioseq::pg_types::seq* hdr,
                           const std::optional<sequence_locator>& loc_opt,
                           MemoryContext mctx) {
    if (!bioseq::settings::index::insert_maintenance::enabled)
        return;
    if (!loc_opt.has_value())
        return;
    const sequence_locator& loc = *loc_opt;
    const relation_identity rid = index_relation_identity(user_relid);
    auto policy_rows = load_table_policy(user_relid, rid.relfilenode, attno);
    pg_supported_indexes idx;
    idx.apply_table_defaults();
    idx.merge_overrides(policy_rows);
    reset_last_minimizer_sequence_build_stats();
    if (bioseq::aligner::settings::grouped_soa_generation::enabled) {
        constexpr auto k_index_types = pg_supported_indexes::list_types();
        for (std::size_t i = 0; i < k_index_types.size(); ++i) {
            const bool minimizer = k_index_types[i].first == k_minimizers_per_fragment_index_name ||
                                   k_index_types[i].first == k_minimizers_per_sequence_index_name;
            if (minimizer && idx.enabled[i] &&
                idx.policy[i]
                    == bioseq::indexing::index_update_policy::update_on_insert) {
                bioseq::postgres::support::invoke_pg_as_cpp([index_name = k_index_types[i].first] {
                    ereport(ERROR,
                            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                             errmsg("grouped-SoA persistence forbids INSERT-time "
                                    "minimizer maintenance"),
                             errdetail("index %.*s would write the scheme-untagged layout "
                                       "before its PostgreSQL locator can be "
                                       "reopened by the grouped-SoA coordinator",
                                       static_cast<int>(index_name.size()),
                                       index_name.data()),
                             errhint("Set bioseq.index.insert_maintenance.enabled=off and "
                                     "publish the index with bioseq.update_index "
                                     "after the source row commits.")));
                });
                pg_unreachable();
            }
        }
    }
    const auto requested_algorithm =
        static_cast<bioseq::simd::minimizer_algorithm>(
            static_cast<int>(bioseq::aligner::settings::minimizer::algorithm.get_value()));
    if (requested_algorithm ==
        bioseq::simd::minimizer_algorithm::simd_minimap2_reference) {
        constexpr auto k_index_types = pg_supported_indexes::list_types();
        for (std::size_t i = 0; i < k_index_types.size(); ++i) {
            const bool minimizer =
                k_index_types[i].first == k_minimizers_per_fragment_index_name ||
                k_index_types[i].first == k_minimizers_per_sequence_index_name;
            if (minimizer && idx.enabled[i] &&
                idx.policy[i] == bioseq::indexing::index_update_policy::update_on_insert) {
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [index_name = k_index_types[i].first] {
                        ereport(
                            ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("minimap2-reference INSERT index generation is not READY"),
                             errdetail("index %.*s has no algorithm-identity-bearing INSERT "
                                       "storage route",
                                       static_cast<int>(index_name.size()),
                                       index_name.data()),
                             errhint("Disable INSERT maintenance and publish this index with "
                                     "bioseq.update_index using grouped-SoA generation.")));
                    });
                pg_unreachable();
            }
        }
    }
    const auto backend = resolve_index_sketch_backend(requested_algorithm);
    const bioseq::minimizers::minimizer_kernels* const minimizer_kernels =
        selected_minimizer_kernels_for_operation(backend, "bioseq INSERT index build");
    if (backend == bioseq::minimizers::minimizer_sketch_backend::bounded_nthash_simd) {
        constexpr auto k_index_types = pg_supported_indexes::list_types();
        for (std::size_t i = 0; i < k_index_types.size(); ++i) {
            if (k_index_types[i].second == sequence_index_layout::fragment_chunks &&
                idx.enabled[i]
                && idx.policy[i]
                    == bioseq::indexing::index_update_policy::update_on_insert) {
                reject_bounded_nthash_for_scheme_untagged_fragment_layout(
                    backend, "bioseq INSERT index build");
            }
        }
    }
    ensure_vulkan_index_build_runtime_ready(backend);
    bioseq::pg_types::seq::dispatch_instance(
        hdr->alphabet_code,
        hdr->storage_unit_byte_width,
        [hdr, rid, attno, user_relid, loc, rowno, mctx, &idx, backend, minimizer_kernels]<
            class p_sequence_type>() {
            compact_sequence_range<p_sequence_type> rng{*hdr};
            auto lazy = bioseq::storage::make_resident_lazy_sequence(rng);
            const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
            std::map<std::string_view, fragment_index_storage> fcache;
            std::map<std::string_view, sequence_index_storage> qcache;
            auto open_frag = [&fcache, rid, attno, user_relid, loc, rowno, mctx](
                                 std::string_view name) -> fragment_index_storage& {
                if (auto it = fcache.find(name); it != fcache.end())
                    return it->second;
                acquire_index_advisory_xact_lock(
                    rid.db_oid, rid.tablespace_oid, rid.relfilenode, attno, name);
                return fcache
                    .emplace(name,
                             open_fragment_storage(user_relid,
                                                   rid.db_oid,
                                                   rid.tablespace_oid,
                                                   rid.relfilenode,
                                                   attno,
                                                   name,
                                                   loc,
                                                   rowno,
                                                   nullptr,
                                                   mctx))
                    .first->second;
            };
            auto open_inl = [&qcache, rid, attno, user_relid, loc, rowno, mctx](
                                std::string_view name) -> sequence_index_storage& {
                if (auto it = qcache.find(name); it != qcache.end())
                    return it->second;
                acquire_index_advisory_xact_lock(
                    rid.db_oid, rid.tablespace_oid, rid.relfilenode, attno, name);
                return qcache
                    .emplace(name,
                             open_sequence_storage_handle(user_relid,
                                                          rid.db_oid,
                                                          rid.tablespace_oid,
                                                          rid.relfilenode,
                                                          attno,
                                                          name,
                                                          loc,
                                                          rowno,
                                                          nullptr,
                                                          mctx))
                    .first->second;
            };
            try {
                idx.build_fragmented<true>(
                    open_frag, lazy_view, backend, minimizer_kernels, hdr->alphabet_code);
                idx.build_sequence<true>(open_inl, lazy_view);
                for (auto& [name, storage] : fcache) {
                    (void)name;
                    storage.close_checked();
                }
                for (auto& [name, storage] : qcache) {
                    (void)name;
                    storage.close_checked();
                }
            } catch (...) {
                for (auto& [name, storage] : fcache) {
                    (void)name;
                    storage.abandon_after_pg_error_noexcept();
                }
                for (auto& [name, storage] : qcache) {
                    (void)name;
                    storage.abandon_after_pg_error_noexcept();
                }
                throw;
            }
        });
    note_policy_on_insert_writes(user_relid, rid.relfilenode, attno, idx);
}

[[nodiscard]] bool index_present(Oid user_relid,
                                 Oid db,
                                 Oid spc,
                                 Oid relfn,
                                 std::int16_t attno,
                                 std::string_view index_name,
                                 const sequence_locator& loc,
                                 std::int64_t row_key,
                                 seq_caches* caches,
                                 MemoryContext mctx) {
    auto s = open_fragment_storage(user_relid,
                                   db,
                                   spc,
                                   relfn,
                                   attno,
                                   index_name,
                                   loc,
                                   static_cast<std::uint64_t>(row_key),
                                   caches,
                                   mctx);
    // both layouts gate on the k_directory_sub_key witness written LAST.
    try {
        const bool present = s.number_of_chunks(k_directory_sub_key) >= 1;
        s.close_checked();
        return present;
    } catch (...) {
        s.abandon_after_pg_error_noexcept();
        throw;
    }
}

[[nodiscard]] float
estimate_index_presence(Oid user_relid, double estimated_total_rows, std::string_view index_name) {
    if (estimated_total_rows <= 0)
        return 0.0f;
    const relation_identity rid = index_relation_identity(user_relid);
    // Probe the bucket-0 shard-0 shard's reltuples (free, no scan).
    const std::string nm =
        format_seqidx_name(index_name, rid.db_oid, rid.tablespace_oid, rid.relfilenode, 0, 0, 0);
    const Oid ns = get_namespace_oid("bioseq_internal", true);
    if (!OidIsValid(ns))
        return 0.0f;
    const Oid rel = get_relname_relid(nm.c_str(), ns);
    if (!OidIsValid(rel))
        return 0.0f;
    double shard_reltuples = 0.0;
    {
        Relation r = bioseq::postgres::support::try_open_postgres_relation(rel, AccessShareLock);
        if (r != nullptr) {
            shard_reltuples = r->rd_rel->reltuples;
            relation_close(r, AccessShareLock);
        }
    }
    constexpr double k_fragment_count_estimate = 64.0; // coarse nominal
    double est_rows = shard_reltuples < 0 ? (double)0 : shard_reltuples;
    double distinct_seq_est = est_rows / k_fragment_count_estimate;
    float cov = (float)std::min(1.0, distinct_seq_est / estimated_total_rows);
    return cov < 0.0f ? 0.0f : cov;
}

} // namespace bioseq::storage::indexing
