/**
 * @file
 * @brief Owns the saved PostgreSQL planner-hook chain state.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_planner_hook_state_contracts;

export {
    /* saved hook values in case of unload */
    set_rel_pathlist_hook_type previous_set_rel_pathlist_hook = nullptr;
    set_join_pathlist_hook_type previous_set_join_pathlist_hook = nullptr;
#if PG_VERSION_NUM >= 190000
    build_simple_rel_hook_type previous_build_simple_rel_hook = nullptr;
#else
    get_relation_info_hook_type previous_get_relation_info_hook = nullptr;
#endif
}
