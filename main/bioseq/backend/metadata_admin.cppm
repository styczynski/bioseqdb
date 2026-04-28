/*-------------------------------------------------------------------------
 *
 * metadata_admin.cppm
 *
 * Process-global registry of "metadata kinds" (per-row sidecars,
 * per-table indexes) plus the C++ implementation of the SQL surface
 *   - bioseq.list_metadata()        (set-returning function)
 *   - bioseq.update_index(rel, name)
 *
 * Each module that owns a metadata kind (sidecar.cppm for minimizers /
 * symbol_counts today, prefilter modules for sneaky_snake later) registers
 * its descriptor here at backend init. The two SQL entry points walk the
 * registry to enumerate or recompute kinds without hard-coding the kind
 * set.
 *
 * The age-log helper (bioseq_internal.metadata_age_log) tracks the most
 * recent (recompute, INSERT-batch) timestamp per (user_relid, kind). It is
 * read by list_metadata() and written by update_index() and (throttled to
 * once per minute via a backend-local in-memory cache) by
 * sidecar_after_insert.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

extern "C" {
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/array.h"
#include "utils/timestamp.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "catalog/indexing.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
}

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

/* IsColumnarTableAmTable is defined with extern "C++" linkage in
 * backend/table/access_method.cppm. extern "C++" declarations attach to the
 * global module fragment, not to the named module's exports, so we redeclare
 * here to make the symbol reachable from `update_index_impl`. No new symbol
 * is introduced; the linker resolves to the existing definition. */
extern "C++" bool IsColumnarTableAmTable(Oid relationId);

export module bioseq_metadata_admin;

import mlogging;
import pg_utils;

namespace bioseq::metadata_admin {

/* ================================================================== */
/* MetadataKind descriptor + registry                                 */
/* ================================================================== */

/// PER_ROW: one entry per (user_relid, sequence_rowno) row in a backing
/// table. Example: minimizers (one TargetIndex blob per sequence),
/// symbol_counts (one int64[] per sequence).
///
/// PER_TABLE: one entry per user_relid (no sequence_rowno). Example:
/// future BWA-FM index (single blob per relation).
export enum class MetadataMode : uint8_t {
    PER_ROW   = 0,
    PER_TABLE = 1,
};

/// String name of a MetadataMode for SQL output. Stable; do not change.
export [[nodiscard]] constexpr const char* metadata_mode_name(MetadataMode mode) noexcept {
    switch (mode) {
        case MetadataMode::PER_ROW:   return "PER_ROW";
        case MetadataMode::PER_TABLE: return "PER_TABLE";
    }
    return "unknown";
}

/// Type-erased "emit one user_relid" callback. Cheap, no allocations.
/// Used by enumerate_user_relids to keep the descriptor a POD.
export struct UserRelidEmitter {
    void (*invoke)(void* state, Oid user_relid) = nullptr;
    void* state = nullptr;

    [[gnu::always_inline]] void operator()(Oid user_relid) const {
        invoke(state, user_relid);
    }
};

/// Helper builder that wraps an arbitrary callable F into a
/// UserRelidEmitter without allocating. Intended for stack-local
/// lambdas; the caller must keep `fn` alive for the duration of the
/// emitter's use.
export template<typename F>
    requires std::invocable<F&, Oid>
[[nodiscard]] inline UserRelidEmitter make_user_relid_emitter(F& fn) noexcept {
    return UserRelidEmitter{
        .invoke = [](void* state, Oid relid) {
            (*static_cast<F*>(state))(relid);
        },
        .state = static_cast<void*>(std::addressof(fn)),
    };
}

/// Stable, copy-cheap descriptor for a metadata kind. Function pointers
/// are static (live for backend lifetime); string fields point at static
/// storage and need not be copied.
export struct MetadataKindDescriptor {
    /// Public name surfaced in list_metadata() and accepted by
    /// update_index(rel, name). Examples: "minimizers", "symbol_counts".
    /// MUST be unique across the registry.
    const char* name = "";

    /// PER_ROW or PER_TABLE.
    MetadataMode mode = MetadataMode::PER_ROW;

    /// Whether bioseq.update_index_on_insert applies to this kind.
    /// True for incrementally-updateable kinds; false for kinds whose
    /// recomputation cost is amortised over the whole table.
    bool insert_support = true;

    /// Returns the OID of the bioseq_internal.* relation that backs
    /// this kind, or InvalidOid when the install is broken / the kind
    /// has no physical backing.
    Oid (*backing_relation_oid)() = nullptr;

    /// Estimate the on-disk byte size of this kind's data for the given
    /// user_relid. Returns -1 when unknowable.
    int64_t (*estimate_size_bytes)(Oid user_relid) = nullptr;

    /// Enumerate distinct user_relids that have at least one row of
    /// this kind's data. Calls `emit(user_relid)` once per distinct
    /// value.
    void (*enumerate_user_relids)(UserRelidEmitter emit) = nullptr;

    /// Recompute this kind's data for `user_relid`. Returns the count
    /// of entries written.
    int64_t (*recompute_for_relid)(Oid user_relid) = nullptr;
};

/// Process-global registry. Populated once per backend by
/// `register_default_metadata_kinds()` (in sidecar.cppm) plus any future
/// per-module registration. After init returns, the registry is
/// read-only and lock-free.
///
/// We use a fixed-capacity std::array rather than std::vector so
/// register_kind can be noexcept. The expected kind count is ~10; the
/// cap of 32 gives plenty of headroom.
export class MetadataKindRegistry {
public:
    static constexpr std::size_t kMaxKinds = 32;

    [[nodiscard]] static MetadataKindRegistry& instance() noexcept {
        static MetadataKindRegistry singleton;
        return singleton;
    }

    /// Append a new descriptor. Re-registering an entry with the same
    /// name AND same function pointers is a silent no-op so a forced
    /// re-init does not double-register.
    void register_kind(const MetadataKindDescriptor& descriptor) noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            if (kinds_[i].name == nullptr || descriptor.name == nullptr) {
                continue;
            }
            if (std::strcmp(kinds_[i].name, descriptor.name) != 0) {
                continue;
            }
            const bool same_pointers =
                kinds_[i].backing_relation_oid  == descriptor.backing_relation_oid  &&
                kinds_[i].estimate_size_bytes   == descriptor.estimate_size_bytes   &&
                kinds_[i].enumerate_user_relids == descriptor.enumerate_user_relids &&
                kinds_[i].recompute_for_relid   == descriptor.recompute_for_relid;
            if (same_pointers) {
                return;
            }
            DEBUG_ASSERT(false, "metadata_admin: register_kind called with conflicting "
                                "descriptor for an existing kind name");
            return;
        }
        if (count_ >= kMaxKinds) {
            DEBUG_ASSERT(false, "metadata_admin: MetadataKindRegistry capacity exceeded "
                                "(bump kMaxKinds)");
            return;
        }
        kinds_[count_] = descriptor;
        ++count_;
        JustLog("[bioseq] metadata_admin: registered kind '{}' "
                "(mode={}, insert_support={}, count_now={})",
                descriptor.name, metadata_mode_name(descriptor.mode),
                descriptor.insert_support, count_);
    }

    /// Read-only view; safe for concurrent reads after init.
    [[nodiscard]] std::span<const MetadataKindDescriptor> kinds() const noexcept {
        return std::span<const MetadataKindDescriptor>(kinds_.data(), count_);
    }

    /// Find by name; returns nullptr if not found.
    [[nodiscard]] const MetadataKindDescriptor* find(std::string_view name) const noexcept {
        for (std::size_t i = 0; i < count_; ++i) {
            if (kinds_[i].name != nullptr && name == kinds_[i].name) {
                return &kinds_[i];
            }
        }
        return nullptr;
    }

private:
    MetadataKindRegistry() = default;
    std::array<MetadataKindDescriptor, kMaxKinds> kinds_{};
    std::size_t count_ = 0;
};

/// Idempotent populate-once hook called from `bioseq_init()`. The body
/// is intentionally tiny: actual registrations live in the modules that
/// own the kinds (sidecar.cppm, future prefilter modules) and are
/// driven from `bioseq_init()` directly. This entry point exists so the
/// registry can be reset for testing or future pg_reload_conf
/// integration without touching call sites.
export inline void init_registry() noexcept {
    static bool initialised = false;
    if (initialised) {
        return;
    }
    initialised = true;
    JustLog("[bioseq] metadata_admin: registry initialised (capacity={})",
            MetadataKindRegistry::kMaxKinds);
}

/* ================================================================== */
/* metadata_age_log resolution + writer                               */
/* ================================================================== */

namespace age_log {

/// Throttle window for note_per_insert(). One DB write per (rel, kind)
/// pair per kThrottleSeconds.
export inline constexpr int64_t kThrottleSeconds = 60;

namespace detail {

/// Resolve and cache the OID of bioseq_internal.metadata_age_log. The
/// helper goes through regclassin to share the syscache fast path.
[[nodiscard]] inline Oid resolve_age_log_relid() {
    static Oid cached_relid = InvalidOid;
    if (OidIsValid(cached_relid)) {
        return cached_relid;
    }
    Oid resolved = InvalidOid;
    PG_TRY();
    {
        resolved = DatumGetObjectId(DirectFunctionCall1(
            regclassin,
            CStringGetDatum("bioseq_internal.metadata_age_log")));
    }
    PG_CATCH();
    {
        FlushErrorState();
        resolved = InvalidOid;
    }
    PG_END_TRY();
    cached_relid = resolved;
    return cached_relid;
}

/// Heap-level UPSERT into metadata_age_log. Mirrors the
/// systable_beginscan + heap_form_tuple / heap_modify_tuple pattern
/// already used by sidecar.cppm. noexcept: PG palloc / heap_* calls
/// raise via ereport(ERROR) which longjmps out of the surrounding fmgr
/// frame; std::bad_alloc cannot escape this function.
inline void write_age_log_row(Oid user_relid, std::string_view kind,
                              TimestampTz when) noexcept {
    const Oid age_log_relid = resolve_age_log_relid();
    if (!OidIsValid(age_log_relid)) {
        return;
    }
    Relation rel = try_safe_relation_open(age_log_relid, RowExclusiveLock);
    if (rel == nullptr) {
        return;
    }

    const Oid pk_index_oid = find_primary_key_oid(rel);

    text* kind_text = cstring_to_text_with_len(kind.data(),
                                               static_cast<int>(kind.size()));

    ScanKeyData keys[2];
    ScanKeyInit(&keys[0], 1 /*user_relid*/,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(user_relid));
    ScanKeyInit(&keys[1], 2 /*kind*/,
                BTEqualStrategyNumber, F_TEXTEQ,
                PointerGetDatum(kind_text));

    SysScanDesc scan = systable_beginscan(rel, pk_index_oid,
                                          /*indexOK=*/OidIsValid(pk_index_oid),
                                          /*snapshot=*/nullptr, 2, keys);
    HeapTuple existing = systable_getnext(scan);

    Datum values[3];
    bool  nulls[3]    = {false, false, false};
    bool  replaces[3] = {false, false, true};
    values[0] = ObjectIdGetDatum(user_relid);
    values[1] = PointerGetDatum(kind_text);
    values[2] = TimestampTzGetDatum(when);

    if (HeapTupleIsValid(existing)) {
        HeapTuple updated = heap_modify_tuple(
            existing, RelationGetDescr(rel), values, nulls, replaces);
        CatalogTupleUpdate(rel, &existing->t_self, updated);
        heap_freetuple(updated);
    } else {
        HeapTuple fresh = heap_form_tuple(
            RelationGetDescr(rel), values, nulls);
        CatalogTupleInsert(rel, fresh);
        heap_freetuple(fresh);
    }

    systable_endscan(scan);
    relation_close(rel, RowExclusiveLock);
    pfree(kind_text);
}

/// Backend-local last-seen map for the throttle. AgeKey owns a
/// std::string copy of the kind name so the map entry survives the
/// caller's std::string_view going out of scope.
struct AgeKey {
    Oid           user_relid;
    std::string   kind;
};

struct AgeKeyHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(const AgeKey& key) const noexcept {
        return mix(key.user_relid, std::string_view{key.kind});
    }
    [[nodiscard]] std::size_t operator()(
        const std::pair<Oid, std::string_view>& key) const noexcept {
        return mix(key.first, key.second);
    }
    [[nodiscard]] static std::size_t mix(Oid relid,
                                         std::string_view kind) noexcept {
        // boost::hash_combine-style mixer; avoids the weak XOR pattern.
        std::size_t h = std::hash<Oid>{}(relid);
        h ^= std::hash<std::string_view>{}(kind)
             + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

struct AgeKeyEq {
    using is_transparent = void;
    [[nodiscard]] bool operator()(const AgeKey& lhs,
                                  const AgeKey& rhs) const noexcept {
        return lhs.user_relid == rhs.user_relid && lhs.kind == rhs.kind;
    }
    [[nodiscard]] bool operator()(
        const AgeKey& lhs,
        const std::pair<Oid, std::string_view>& rhs) const noexcept {
        return lhs.user_relid == rhs.first
            && std::string_view{lhs.kind} == rhs.second;
    }
    [[nodiscard]] bool operator()(
        const std::pair<Oid, std::string_view>& lhs,
        const AgeKey& rhs) const noexcept {
        return lhs.first == rhs.user_relid
            && lhs.second == std::string_view{rhs.kind};
    }
};

/// Backend-local "last DB write timestamp" cache, keyed by (rel, kind).
/// thread_local because each Postgres backend is single-threaded.
inline thread_local std::unordered_map<
    AgeKey, TimestampTz, AgeKeyHash, AgeKeyEq> last_seen_cache;

}  // namespace detail

/// Fetch last_updated for (user_relid, kind), or std::nullopt when no
/// row exists. Reads through a sequential scan with key filter; for the
/// expected size of metadata_age_log (O(N_tables × N_kinds)) this is
/// fast enough and keeps the call out of the SPI memory dance.
export [[nodiscard]] inline std::optional<TimestampTz> read(
    Oid user_relid, std::string_view kind) noexcept {
    const Oid age_log_relid = detail::resolve_age_log_relid();
    if (!OidIsValid(age_log_relid)) {
        return std::nullopt;
    }
    Relation rel = try_safe_relation_open(age_log_relid, AccessShareLock);
    if (rel == nullptr) {
        return std::nullopt;
    }

    const Oid pk_index_oid = find_primary_key_oid(rel);

    text* kind_text = cstring_to_text_with_len(kind.data(),
                                               static_cast<int>(kind.size()));

    ScanKeyData keys[2];
    ScanKeyInit(&keys[0], 1 /*user_relid*/,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(user_relid));
    ScanKeyInit(&keys[1], 2 /*kind*/,
                BTEqualStrategyNumber, F_TEXTEQ,
                PointerGetDatum(kind_text));

    SysScanDesc scan = systable_beginscan(rel, pk_index_oid,
                                          /*indexOK=*/OidIsValid(pk_index_oid),
                                          /*snapshot=*/nullptr, 2, keys);

    std::optional<TimestampTz> result;
    HeapTuple tup = systable_getnext(scan);
    if (HeapTupleIsValid(tup)) {
        bool isnull = false;
        Datum dat = heap_getattr(tup, 3 /*last_updated*/,
                                 RelationGetDescr(rel), &isnull);
        if (!isnull) {
            result = DatumGetTimestampTz(dat);
        }
    }

    systable_endscan(scan);
    relation_close(rel, AccessShareLock);
    pfree(kind_text);
    return result;
}

/// UPSERT (user_relid, kind, last_updated = now()). Always writes; used
/// by bioseq.update_index() after the recompute completes.
export inline void write(Oid user_relid, std::string_view kind) noexcept {
    if (!OidIsValid(user_relid) || kind.empty()) {
        return;
    }
    const TimestampTz now = GetCurrentTimestamp();
    detail::write_age_log_row(user_relid, kind, now);
    detail::last_seen_cache[detail::AgeKey{user_relid, std::string(kind)}] = now;
}

/// Throttled UPSERT used by sidecar_after_insert. Steady-state cost is
/// one std::unordered_map lookup; the actual DB write only fires once
/// the throttle window has expired for the (user_relid, kind) pair.
export inline void note_per_insert(Oid user_relid, std::string_view kind) noexcept {
    if (!OidIsValid(user_relid) || kind.empty()) {
        return;
    }
    const TimestampTz now = GetCurrentTimestamp();
    auto& cache = detail::last_seen_cache;
    const std::pair<Oid, std::string_view> lookup_key{user_relid, kind};
    const auto it = cache.find(lookup_key);
    if (it != cache.end()) {
        const TimestampTz prev = it->second;
        if (now - prev < kThrottleSeconds * INT64_C(1000000)) {
            return;
        }
    }
    detail::write_age_log_row(user_relid, kind, now);
    cache.insert_or_assign(detail::AgeKey{user_relid, std::string(kind)}, now);
}

}  // namespace age_log

/* ================================================================== */
/* enumerate_distinct_user_relids: walk a backing table once, emit    */
/* every distinct user_relid value seen on attribute 1.               */
/* ================================================================== */

/// Walk `backing_relid` once and call emit(user_relid) for every
/// distinct value present in column 1. AM-agnostic: slot ops come from
/// `table_slot_callbacks(rel)` so the helper handles both heap-AM
/// sidecars and (future) bioseq-AM sidecars.
export inline void enumerate_distinct_user_relids(
    Oid backing_relid, UserRelidEmitter emit)
{
    if (!OidIsValid(backing_relid)) {
        return;
    }
    Relation rel = try_safe_relation_open(backing_relid, AccessShareLock);
    if (rel == nullptr) {
        return;
    }

    TupleDesc tup_desc = RelationGetDescr(rel);
    Snapshot snapshot = GetActiveSnapshot();

    TableScanDesc scan = table_beginscan_strat(
        rel, snapshot, /*nkeys=*/0, /*key=*/nullptr,
        /*allow_strat=*/true, /*allow_sync=*/true);

    TupleTableSlot* slot = MakeSingleTupleTableSlot(
        tup_desc, table_slot_callbacks(rel));

    std::unordered_set<Oid> seen;
    seen.reserve(16);                 // typical: ~10 user tables per backend
    Oid prev_seen = InvalidOid;
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        bool isnull = false;
        const Datum dat = slot_getattr(slot, /*attnum=*/1, &isnull);
        if (isnull) {
            continue;
        }
        const Oid relid = DatumGetObjectId(dat);
        if (relid == prev_seen) {
            continue;                 // cheap monotonic skip for runs
        }
        prev_seen = relid;
        if (seen.insert(relid).second) {
            emit(relid);
        }
    }

    ExecDropSingleTupleTableSlot(slot);
    table_endscan(scan);
    relation_close(rel, AccessShareLock);
}

/* ================================================================== */
/* estimate_blob_size_for_relid: SUM(pg_column_size(blob)) by user_relid */
/* ================================================================== */

/// Returns the estimated total byte size of `blob_attno` summed over
/// every row in `backing_relid` whose user_relid matches. Returns -1 on
/// error or when the backing relation is missing.
export inline int64_t estimate_blob_size_for_relid(
    Oid backing_relid, AttrNumber blob_attno, Oid user_relid)
{
    if (!OidIsValid(backing_relid)) {
        return -1;
    }
    Relation rel = try_safe_relation_open(backing_relid, AccessShareLock);
    if (rel == nullptr) {
        return -1;
    }
    TupleDesc tup_desc = RelationGetDescr(rel);
    if (blob_attno < 1 || blob_attno > tup_desc->natts) {
        relation_close(rel, AccessShareLock);
        return -1;
    }

    Form_pg_attribute attr = TupleDescAttr(tup_desc, blob_attno - 1);
    char* column_name = pstrdup(NameStr(attr->attname));
    char* relname     = pstrdup(RelationGetRelationName(rel));
    const Oid relnamespace = RelationGetNamespace(rel);
    relation_close(rel, AccessShareLock);
    char* nspname = get_namespace_name(relnamespace);

    StringInfoData sql_buf;
    initStringInfo(&sql_buf);
    appendStringInfo(&sql_buf,
        "SELECT COALESCE(SUM(pg_column_size(%s))::bigint, 0) "
        "FROM %s.%s WHERE user_relid = $1",
        quote_identifier(column_name),
        quote_identifier(nspname),
        quote_identifier(relname));

    int64_t result = -1;
    if (SPI_connect() != SPI_OK_CONNECT) {
        pfree(sql_buf.data);
        return -1;
    }
    Oid arg_types[1]  = { OIDOID };
    Datum arg_values[1] = { ObjectIdGetDatum(user_relid) };
    const int rc = SPI_execute_with_args(sql_buf.data, 1, arg_types, arg_values,
                                         /*Nulls=*/nullptr,
                                         /*read_only=*/true, /*tcount=*/1);
    if (rc == SPI_OK_SELECT && SPI_processed == 1) {
        bool isnull = false;
        Datum dat = SPI_getbinval(SPI_tuptable->vals[0],
                                  SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull) {
            result = DatumGetInt64(dat);
        }
    }
    SPI_finish();
    pfree(sql_buf.data);
    return result;
}

/* ================================================================== */
/* SQL entry points                                                   */
/* ================================================================== */

/// bioseq.list_metadata() set-returning function. Walks every
/// registered kind, enumerates the distinct user_relids each kind
/// covers, and emits one tuple per (user_relid, kind) pair.
export [[nodiscard]] Datum list_metadata_impl(FunctionCallInfo fcinfo);

/// bioseq.update_index(rel, name) bulk recompute. Validates the
/// arguments + permissions, dispatches into the kind's
/// recompute_for_relid, and stamps metadata_age_log on success.
export [[nodiscard]] Datum update_index_impl(FunctionCallInfo fcinfo);

inline Datum list_metadata_impl(FunctionCallInfo fcinfo) {
    ReturnSetInfo* rsinfo = reinterpret_cast<ReturnSetInfo*>(fcinfo->resultinfo);
    if (rsinfo == nullptr || !IsA(rsinfo, ReturnSetInfo)) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bioseq.list_metadata() must be called in a "
                        "set-returning context")));
    }
    if ((rsinfo->allowedModes & SFRM_Materialize) == 0) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required for "
                        "bioseq.list_metadata()")));
    }

    TupleDesc tup_desc = nullptr;
    if (get_call_result_type(fcinfo, /*resultTypeId=*/nullptr, &tup_desc)
        != TYPEFUNC_COMPOSITE) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bioseq.list_metadata() return type is not "
                        "composite")));
    }

    MemoryContext per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    Tuplestorestate* tup_store = nullptr;
    {
        MemoryContextScope ctx_guard{per_query_ctx};
        tup_store = tuplestore_begin_heap(
            /*randomAccess=*/true, /*interXact=*/false, work_mem);
        rsinfo->returnMode = SFRM_Materialize;
        rsinfo->setResult  = tup_store;
        rsinfo->setDesc    = tup_desc;
    }

    auto& registry = MetadataKindRegistry::instance();
    for (const MetadataKindDescriptor& kind : registry.kinds()) {
        if (kind.enumerate_user_relids == nullptr) {
            continue;
        }
        auto emit_one = [&](Oid user_relid) {
            const int64_t bytes = (kind.estimate_size_bytes != nullptr)
                ? kind.estimate_size_bytes(user_relid) : INT64_C(-1);
            text* size_text = nullptr;
            if (bytes >= 0) {
                Datum pretty = DirectFunctionCall1(pg_size_pretty,
                                                   Int64GetDatum(bytes));
                size_text = DatumGetTextPP(pretty);
            }

            const std::optional<TimestampTz> last_updated =
                age_log::read(user_relid, kind.name);

            Datum values[6];
            bool  nulls[6] = { false, false, false, false, false, false };
            values[0] = ObjectIdGetDatum(user_relid);
            values[1] = CStringGetTextDatum(kind.name);
            values[2] = CStringGetTextDatum(metadata_mode_name(kind.mode));
            values[3] = BoolGetDatum(kind.insert_support);
            if (size_text != nullptr) {
                values[4] = PointerGetDatum(size_text);
            } else {
                nulls[4] = true;
            }
            if (last_updated.has_value()) {
                // pg_proc oid 1188 (timestamptz_mi) is implemented by
                // the C function `timestamp_mi`; timestamptz and
                // timestamp share the int64-microseconds layout, so the
                // C function applies unchanged.
                Datum age = DirectFunctionCall2(
                    timestamp_mi,
                    TimestampTzGetDatum(GetCurrentTimestamp()),
                    TimestampTzGetDatum(*last_updated));
                values[5] = age;
            } else {
                nulls[5] = true;
            }

            tuplestore_putvalues(tup_store, tup_desc, values, nulls);
        };
        kind.enumerate_user_relids(make_user_relid_emitter(emit_one));
    }

    return PointerGetDatum(nullptr);
}

inline Datum update_index_impl(FunctionCallInfo fcinfo) {
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) {
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("bioseq.update_index: rel and kind must be non-NULL")));
    }
    const Oid user_relid = PG_GETARG_OID(0);
    text* kind_text      = PG_GETARG_TEXT_PP(1);
    const std::string_view kind_name(VARDATA_ANY(kind_text),
                                     VARSIZE_ANY_EXHDR(kind_text));

    const auto& registry = MetadataKindRegistry::instance();
    const MetadataKindDescriptor* kind = registry.find(kind_name);
    if (kind == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq.update_index: unknown metadata kind '%.*s'",
                        static_cast<int>(kind_name.size()), kind_name.data()),
                 errhint("call bioseq.list_metadata() to see the registered set")));
    }

    AclResult acl = pg_class_aclcheck(user_relid, GetUserId(), ACL_SELECT);
    if (acl != ACLCHECK_OK) {
        aclcheck_error(acl, OBJECT_TABLE, get_rel_name(user_relid));
    }
    if (kind->backing_relation_oid != nullptr) {
        const Oid backing = kind->backing_relation_oid();
        if (OidIsValid(backing)) {
            AclResult bacl = pg_class_aclcheck(
                backing, GetUserId(), ACL_INSERT | ACL_DELETE);
            if (bacl != ACLCHECK_OK) {
                aclcheck_error(bacl, OBJECT_TABLE, get_rel_name(backing));
            }
        }
    }

    if (kind->recompute_for_relid == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bioseq.update_index: kind '%.*s' does not "
                        "support recompute",
                        static_cast<int>(kind_name.size()), kind_name.data())));
    }

    if (!IsColumnarTableAmTable(user_relid)) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bioseq.update_index: relation %s does not use "
                        "the bioseq access method",
                        get_rel_name(user_relid)),
                 errhint("only USING bioseq tables have a sidecar; "
                         "heap-table support is out of scope.")));
    }

    JustLog("[bioseq] metadata_admin: update_index rel={} kind={}",
            static_cast<unsigned>(user_relid),
            std::string(kind_name));

    const int64_t written = kind->recompute_for_relid(user_relid);
    age_log::write(user_relid, kind->name);

    PG_RETURN_INT64(written);
}

}  // namespace bioseq::metadata_admin
