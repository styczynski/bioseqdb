/*
 * Test-only PostgreSQL DSO for observing the native Bioseq table-options hook.
 *
 * The production extension publishes a pointer to its backend-local hook slot
 * through find_rendezvous_variable().  This helper chains that slot and exposes
 * the last callback payload through one temporary SQL function created by the
 * integration test.  It is never linked into, installed with, or registered as
 * a member of the Bioseq extension.
 */

extern "C" {
#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"
}

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace {

constexpr const char *kHookRendezvousName = "BioseqColumnTableSetOptions_hook";

enum class compression_type : int {
  invalid = -1,
  none = 0,
  pglz = 1,
  lz4 = 2,
  zstd = 3,
};

struct table_options {
  std::uint64_t stripe_row_count;
  std::uint32_t chunk_group_row_count;
  compression_type compression;
  int compression_level;
};

static_assert(std::is_trivially_copyable_v<table_options>);
static_assert(std::is_standard_layout_v<table_options>);
static_assert(alignof(table_options) == 8);
static_assert(sizeof(table_options) == 24);
static_assert(offsetof(table_options, stripe_row_count) == 0);
static_assert(offsetof(table_options, chunk_group_row_count) == 8);
static_assert(offsetof(table_options, compression) == 12);
static_assert(offsetof(table_options, compression_level) == 16);

using set_options_hook = void (*)(Oid, table_options);

struct probe_state {
  std::uint64_t call_count = 0;
  Oid relation_id = InvalidOid;
  table_options options{};
  set_options_hook *hook_slot = nullptr;
  set_options_hook previous_hook = nullptr;
};

probe_state state{};

void observe_set_options(Oid relation_id, table_options options) {
  ++state.call_count;
  state.relation_id = relation_id;
  state.options = options;
  if (state.previous_hook != nullptr) {
    state.previous_hook(relation_id, options);
  }
}

} // namespace

extern "C" {

PG_MODULE_MAGIC;

PGDLLEXPORT void _PG_init(void);
PGDLLEXPORT void _PG_fini(void);
PGDLLEXPORT Datum doc0388_alter_hook_snapshot(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(doc0388_alter_hook_snapshot);

void _PG_init(void) {
  auto **published_slot = reinterpret_cast<set_options_hook **>(
      find_rendezvous_variable(kHookRendezvousName));
  if (published_slot == nullptr || *published_slot == nullptr) {
    ereport(ERROR,
            (errmsg("Bioseq table-options hook rendezvous is not published")));
  }

  state.hook_slot = *published_slot;
  state.previous_hook = *state.hook_slot;
  *state.hook_slot = &observe_set_options;
}

void _PG_fini(void) {
  if (state.hook_slot != nullptr && *state.hook_slot == &observe_set_options) {
    *state.hook_slot = state.previous_hook;
  }
  state.hook_slot = nullptr;
  state.previous_hook = nullptr;
}

Datum doc0388_alter_hook_snapshot(PG_FUNCTION_ARGS) {
  static_cast<void>(fcinfo);
  char *snapshot = psprintf(
      "%llu:%u:%llu:%u:%d:%d",
      static_cast<unsigned long long>(state.call_count), state.relation_id,
      static_cast<unsigned long long>(state.options.stripe_row_count),
      state.options.chunk_group_row_count,
      static_cast<int>(state.options.compression),
      state.options.compression_level);
  PG_RETURN_TEXT_P(cstring_to_text(snapshot));
}

} // extern "C"
