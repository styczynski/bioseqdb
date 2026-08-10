#!/usr/bin/env python3
"""Validate PostgreSQL ERROR-safety ledgers, runtime coverage, and DSO closure."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from typing import Any, Mapping


PG17_TAG = "REL_17_10"
PG17_REVISION = "25c49f3a4a742ba283f5cc43cc7f1d361552e917"
PG17_SERVER_VERSION_NUM = 170010
MAX_CALLBACK_CLOSURE_DEPTH = 512
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
REVISION_PATTERN = re.compile(r"[0-9a-f]{40}")
RUNTIME_EVENT_PATTERN = re.compile(r"[a-z0-9][a-z0-9_.]{0,63}")
FAULT_KEY_PATTERN = re.compile(r"[a-z0-9_.]{1,128}")
PROBE_SITE_ID_PATTERN = re.compile(r"probe:[0-9a-f]{64}")
CASE_ID_PATTERN = re.compile(r"[a-z0-9][a-z0-9_.-]{0,127}")
SQLSTATE_PATTERN = re.compile(r"[0-9A-Z]{5}")
POSTGRESQL_TOOLS = (
    "postgres",
    "initdb",
    "pg_ctl",
    "createdb",
    "dropdb",
    "psql",
)
AUTHORIZATION_MARKER = (
    b"bioseq.postgres_error_safety.authorization_marker\n"
    b"revision=2\n"
)
AUTHORIZATION_MARKER_NAME = "bioseq-safety-runtime.authorization"
PRIVATE_DATABASE = "bioseq_safety_runtime"
PRIVATE_PASSWORD = "unused-private-trust-auth"
CONNECTION_FIELDS = frozenset(
    {"host", "port", "database", "user", "password"}
)
PROVENANCE_FIELDS = frozenset(
    {
        "schema_revision",
        "postgresql_major",
        "postgresql_tag",
        "postgresql_revision",
        "source_base_url",
        "pg_no_longjmp_allowlist",
        "records",
    }
)
LEDGER_FIELDS = frozenset(
    {
        "schema_revision",
        "profile",
        "postgresql_major",
        "postgresql_revision",
        "provenance_bundle_sha256",
        "source_list_sha256",
        "compile_database_sha256",
        "producer_executable_sha256",
        "compile_graph_sha256",
        "pcm_manifest_sha256",
        "sql_manifest_sha256",
        "compiler_facts_sha256",
        "clang_version",
        "analyzer_version",
        "generator_revision",
        "callback_root_count",
        "callback_closure_sha256",
        "parents",
        "symbol_groups",
    }
)

PROFILES = frozenset({"production", "test_indexes"})
EXPECTED_TEMPLATE_COUNT = 25
EXPECTED_PROFILE_CASE_COUNT = 25
EXPECTED_MATRIX_CASE_COUNT = 50
SEVERITIES = frozenset({"error", "fatal", "panic", "unresolved"})
TERMINAL_SEVERITIES = frozenset({"fatal", "panic"})
RESOLVED_TARGETS = frozenset({"direct", "resolved_indirect"})
CLASSIFICATIONS = frozenset(
    {
        "bridged",
        "specialized_bridge",
        "lifetime_free_direct",
        "terminal_after_unwind",
    }
)
BRIDGES = frozenset(
    {
        "none",
        "invoke_pg_as_cpp",
        "sql_subtransaction",
        "sql_call_context",
        "generated_boundary_tail",
    }
)
CROSSING_ORIGINS = frozenset(
    {
        "automatic_object",
        "temporary",
        "lambda_capture",
        "catch_parameter",
        "coroutine_state",
    }
)
COVERAGE_DIMENSIONS = frozenset(
    {
        "classification",
        "bridge",
        "converter_family",
        "owner_family",
        "cleanup_phase",
    }
)
RUNTIME_POLICY_FIELDS = frozenset(
    {"schema_revision", "required_coverage"}
)
COVERAGE_KEY_FIELDS = frozenset({"profile", "dimension", "value"})
RUNTIME_MATRIX_FIELDS = frozenset(
    {
        "schema_revision",
        "production_ledger_sha256",
        "test_indexes_ledger_sha256",
        "production_sql_manifest_sha256",
        "test_indexes_sql_manifest_sha256",
        "production_case_plan_sha256",
        "test_indexes_case_plan_sha256",
        "production_runtime_inventory_sha256",
        "test_indexes_runtime_inventory_sha256",
        "runtime_policy_sha256",
        "production_stage_manifest_sha256",
        "test_indexes_stage_manifest_sha256",
        "production_tool_signature_suite_sha256",
        "test_indexes_tool_signature_suite_sha256",
        "production_cluster_authority_path",
        "production_cluster_authority_sha256",
        "test_indexes_cluster_authority_path",
        "test_indexes_cluster_authority_sha256",
        "production_dso_sha256",
        "test_indexes_dso_sha256",
        "runtime_control_dso_sha256",
        "runtime_producer_executable_sha256",
        "production_test_executable_sha256",
        "test_indexes_test_executable_sha256",
        "cases",
    }
)
RUNTIME_MATRIX_CASE_FIELDS = frozenset(
    {
        "case_id",
        "probe_abi_revision",
        "profile",
        "sql_identity",
        "site_id",
        "ledger_site_sha256",
        "invocation_key",
        "fault_key",
        "relation",
        "probe_contract",
        "case_plan_sha256",
        "ledger_sha256",
        "sql_manifest_sha256",
        "active_dso_sha256",
        "producer_executable_sha256",
        "test_executable_sha256",
        "expected_backend_outcome",
        "observed_backend_outcome",
        "probe_observation_mode",
        "initial_backend_pid",
        "recovery_backend_pid",
        "connection_replaced",
        "initial_connection_status",
        "post_invocation_connection_status",
        "invocation_result_status",
        "terminal_guard_observed",
        "terminal_causality",
        "site_visit_count",
        "fault_trigger_count",
        "site_visit_sequence",
        "fault_trigger_sequence",
        "expected_destructor_sentinels",
        "observed_destructor_sentinels",
        "expected_cleanup_events",
        "observed_cleanup_events",
        "expected_postgres_cleanup_events",
        "observed_postgres_cleanup_events",
        "expected_fmgr_abort_count",
        "observed_fmgr_abort_count",
        "expected_sqlstate",
        "observed_sqlstate",
        "expected_message",
        "observed_message",
        "expected_recovery_postcondition",
        "observed_recovery_postcondition",
        "exercised_terminal_severity",
        "immutable_test_log_path",
        "immutable_test_log_sha256",
        "immutable_result_path",
        "immutable_result_sha256",
    }
)
IMMUTABLE_RUNTIME_RESULT_FIELDS = frozenset(
    (
        RUNTIME_MATRIX_CASE_FIELDS
        - {
            "immutable_test_log_path",
            "immutable_test_log_sha256",
            "immutable_result_path",
            "immutable_result_sha256",
        }
    )
    | {"schema_revision", "attestation"}
)
RUNTIME_ATTESTATION_FIELDS = frozenset(
    {
        "schema_revision",
        "request_path",
        "request_sha256",
        "catalog_path",
        "catalog_sha256",
        "catalog_entry_sha256",
        "activation_receipt_path",
        "activation_receipt_sha256",
        "cluster_config_path",
        "cluster_config_sha256",
        "stage_manifest_sha256",
        "tool_signature_suite_sha256",
        "active_dso_path",
        "active_dso_sha256",
        "cluster_system_identifier",
        "cluster_data_directory",
        "postmaster_start_time",
        "activation_nonce",
        "server_version_num",
        "backend_pid",
        "expected_backend_outcome",
        "observed_backend_outcome",
        "probe_observation_mode",
        "initial_backend_pid",
        "recovery_backend_pid",
        "connection_replaced",
        "initial_connection_status",
        "post_invocation_connection_status",
        "invocation_result_status",
        "terminal_guard_observed",
        "terminal_causality",
        "probe_abi_revision",
        "site_id",
        "ledger_site_sha256",
        "fault_key",
        "relation",
        "probe_contract",
        "fault_invocation_count",
        "fmgr_start_count",
        "fmgr_end_count",
        "fmgr_abort_count",
        "probe_invalid_count",
        "probe_overflow_count",
        "probe_inside_target",
        "site_visit_count",
        "fault_trigger_count",
        "site_visit_sequence",
        "fault_trigger_sequence",
        "mapped_target_path",
        "mapped_target_sha256",
        "control_dso_path",
        "control_dso_sha256",
        "recovery_result_row_count",
        "recovery_rows_sha256",
        "recovery_artifact_path",
        "recovery_artifact_sha256",
        "raw_result_path",
        "raw_result_sha256",
    }
)
CASE_PLAN_FIELDS = frozenset(
    {
        "schema_revision",
        "profile",
        "probe_abi_revision",
        "template_sha256",
        "ledger_sha256",
        "runtime_inventory_sha256",
        "sql_manifest_sha256",
        "runtime_policy_sha256",
        "cases",
    }
)
CASE_PLAN_CASE_FIELDS = frozenset({"catalog", "execution"})
CATALOG_FIELDS = frozenset(
    {
        "schema_revision",
        "profile",
        "case_plan_sha256",
        "test_executable_sha256",
        "cases",
    }
)
CATALOG_ENTRY_FIELDS = frozenset(
    {
        "case_id",
        "sql_identity",
        "site_id",
        "ledger_site_sha256",
        "invocation_key",
        "fault_key",
        "relation",
        "probe_contract",
        "postgres_severity",
        "expected",
    }
)
EXPECTED_RESULT_FIELDS = frozenset(
    {
        "backend_outcome",
        "destructor_sentinels",
        "cleanup_events",
        "postgres_cleanup_events",
        "sqlstate",
        "message",
        "recovery_postcondition",
    }
)
EXECUTION_FIELDS = frozenset(
    {
        "target_regprocedure",
        "setup_sql",
        "invocation_sql",
        "post_error_sql",
        "recovery_sql",
        "recovery_expected_rows",
        "cleanup_sql",
    }
)
PRE_FAULT_CONTROL_EXECUTION_FIELDS = frozenset(
    {
        "pre_fault_control_sql",
        "pre_fault_control_recovery_sql",
        "pre_fault_control_recovery_expected_rows",
    }
)
REQUEST_FIELDS = frozenset(
    {
        "schema_revision",
        "profile",
        "case_id",
        "sql_identity",
        "site_id",
        "ledger_site_sha256",
        "invocation_key",
        "fault_key",
        "relation",
        "probe_contract",
        "case_plan_path",
        "case_plan_sha256",
        "ledger_path",
        "ledger_sha256",
        "sql_manifest_path",
        "sql_manifest_sha256",
        "active_dso_path",
        "active_dso_sha256",
        "test_executable_sha256",
        "producer_executable_path",
        "producer_executable_sha256",
        "cluster_config_path",
        "cluster_config_sha256",
        "stage_manifest_path",
        "stage_manifest_sha256",
        "tool_signature_suite_sha256",
        "runtime_control_dso_sha256",
        "activation_receipt_path",
        "activation_receipt_sha256",
        "catalog_path",
        "catalog_sha256",
        "catalog_entry_sha256",
    }
)
RAW_RESULT_FIELDS = frozenset(
    {
        "schema_revision",
        "probe_abi_revision",
        "profile",
        "case_id",
        "sql_identity",
        "site_id",
        "ledger_site_sha256",
        "invocation_key",
        "fault_key",
        "relation",
        "probe_contract",
        "case_plan_sha256",
        "ledger_sha256",
        "sql_manifest_sha256",
        "active_dso_sha256",
        "test_executable_sha256",
        "producer_executable_sha256",
        "activation_receipt_sha256",
        "bioseq_guc_properties",
        "bioseq_guc_properties_sha256",
        "cluster_config_sha256",
        "request_sha256",
        "catalog_sha256",
        "catalog_entry_sha256",
        "execution_backend",
        "exercised_terminal_severity",
        "backend_outcome",
        "terminal_causality",
        "destructor_events",
        "cleanup_events",
        "postgres_cleanup_events",
        "postgres_error",
        "fault_evidence",
        "recovery_probe",
        "cluster_evidence",
    }
)
RUNTIME_EVENT_FIELDS = frozenset({"name", "count"})
POSTGRES_ERROR_FIELDS = frozenset({"severity", "sqlstate", "message"})
BACKEND_OUTCOME_FIELDS = frozenset(
    {
        "expected",
        "observed",
        "initial_backend_pid",
        "recovery_backend_pid",
        "initial_connection_status",
        "post_invocation_connection_status",
        "invocation_result_status",
        "terminal_guard_observed",
    }
)
TERMINAL_CAUSALITY_FIELDS = frozenset(
    {
        "marker",
        "server_log",
        "marker_directory",
        "server_log_path",
        "postmaster_identity_unchanged",
        "fatal_panic_signal_core_rejected",
    }
)
TERMINAL_MARKER_FIELDS = frozenset(
    {
        "path",
        "sha256",
        "payload",
        "schema",
        "schema_revision",
        "token",
        "site_id",
        "fault_key",
        "guard_kind",
        "backend_pid",
        "mode",
        "uid",
        "link_count",
    }
)
TERMINAL_SERVER_LOG_FIELDS = frozenset(
    {
        "path",
        "device",
        "inode",
        "offset",
        "interval_size",
        "interval_sha256",
        "interval_text",
        "exit_record",
        "fatal_present",
        "panic_present",
        "signal_present",
        "core_dump_present",
    }
)
FAULT_EVIDENCE_FIELDS = frozenset(
    {
        "observation_mode",
        "probe_abi_revision",
        "site_id",
        "fault_key",
        "relation",
        "probe_contract",
        "invocation_count",
        "fmgr_start_count",
        "fmgr_end_count",
        "fmgr_abort_count",
        "site_visit_count",
        "fault_trigger_count",
        "site_visit_sequence",
        "fault_trigger_sequence",
        "invalid_count",
        "overflow_count",
        "inside_target",
    }
)
RECOVERY_PROBE_FIELDS = frozenset(
    {
        "postcondition",
        "passed",
        "transaction_status",
        "nonempty_result",
        "expected_rows",
        "observed_rows",
        "result_row_count",
    }
)
CLUSTER_EVIDENCE_FIELDS = frozenset(
    {
        "cluster_system_identifier",
        "cluster_data_directory",
        "postmaster_start_time",
        "activation_nonce",
        "server_version_num",
        "backend_pid",
        "initial_backend_pid",
        "recovery_backend_pid",
        "connection_replaced",
        "active_dso_path",
        "mapped_target_path",
        "control_dso_path",
        "control_dso_sha256",
    }
)
ACTIVATION_RECEIPT_FIELDS = frozenset(
    {
        "schema_revision",
        "profile",
        "case_plan_sha256",
        "test_executable_sha256",
        "requested_dso_sha256",
        "cluster_config_sha256",
        "active_dso_path",
        "active_dso_sha256",
        "cluster_system_identifier",
        "cluster_data_directory",
        "postmaster_start_time",
        "activation_nonce",
        "server_version_num",
        "extension_oid",
        "bioseq_guc_count",
        "bioseq_test_only_guc_count",
        "bioseq_guc_properties",
        "bioseq_guc_properties_sha256",
        "installed_control_sha256",
        "installed_sql_sha256",
        "sql_manifest_sha256",
        "sql_script_sha256",
        "control_dso_path",
        "control_dso_sha256",
    }
)
GUC_GENERATION_FIELDS = frozenset(
    {
        "description_authority",
        "owner_facets",
        "profile",
        "profile_delta_validation",
        "registration_count",
        "registrations",
        "registry_content_sha256",
        "registry_module",
    }
)
GUC_REGISTRATION_FIELDS = frozenset(
    {
        "registration_ordinal",
        "name",
        "owner",
        "member",
        "kind",
        "cpp_storage",
        "storage_kind",
        "source_module",
        "source_path",
        "source_sha256",
        "default_expression",
        "minimum_expression",
        "maximum_expression",
        "context",
        "flags",
        "unit",
        "hooks",
        "feature_predicate",
        "enum_default",
        "enum_rows_name_wire_condition_alias_hidden_compiled",
        "generated_live_authority",
    }
)
GUC_GENERATED_LIVE_FIELDS = frozenset(
    {
        "vartype",
        "context",
        "unit",
        "boot_value",
        "reset_value",
        "current_value",
        "minimum",
        "maximum",
        "visible_enum_values",
        "short_description",
        "extra_description",
        "raw_flags",
        "visible_in_show_all",
        "included_in_sample",
        "hooks",
    }
)
GUC_GENERATED_HOOK_FIELDS = frozenset(
    {"symbol", "present", "behavior"}
)
GUC_OWNER_FACET_FIELDS = frozenset(
    {
        "owner",
        "source_module",
        "generated_module",
        "content_sha256",
        "registration_count",
    }
)
GUC_PROPERTY_FIELDS = frozenset(
    {
        "registration_ordinal",
        "name",
        "manifest_registration_sha256",
        "registration",
        "profile_projection",
        "pg_settings",
    }
)
GUC_PROFILE_PROJECTION_FIELDS = frozenset(
    {
        "profile",
        "registry_module",
        "registry_content_sha256",
        "profile_delta_validation_sha256",
        "owner_facet",
    }
)
GUC_PG_SETTINGS_FIELDS = GUC_GENERATED_LIVE_FIELDS
GUC_OWNER_ORDER = ("bioseq", "scheduler", "storage", "aligner")
GUC_SOURCE_MODULES = {
    "bioseq": "bioseq.postgres.configuration.core",
    "scheduler": "bioseq.postgres.configuration.scheduler",
    "storage": "bioseq.postgres.configuration.storage",
    "aligner": "bioseq.postgres.configuration.aligner",
}
GUC_SOURCE_PATHS = {
    "bioseq": "bioseq/backend/pg/configuration/core.cppm",
    "scheduler": "bioseq/backend/pg/configuration/scheduler.cppm",
    "storage": "bioseq/backend/pg/configuration/storage.cppm",
    "aligner": "bioseq/backend/pg/configuration/aligner.cppm",
}
GUC_KIND_STORAGE_KINDS = {
    "bool": "bool",
    "integer": "int32",
    "real": "double",
    "string": "string",
    "enum": "int32_enum",
}
GUC_PRODUCTION_OWNER_COUNTS = {
    "bioseq": 26,
    "scheduler": 3,
    "storage": 1,
    "aligner": 43,
}
GUC_TEST_ONLY_NAMES = (
    "bioseq.testing.chunked_scanner.enabled",
    "bioseq.testing.in_memory_scanner.enabled",
    "bioseq.testing.persisted_index.failure_mode",
)
GUC_PROFILE_TOTALS = {"production": 73, "test_indexes": 76}
CLUSTER_CONFIG_FIELDS = frozenset(
    {
        "schema_revision",
        "profile",
        "dedicated_safety_cluster",
        "connection",
        "private_stage_root",
        "expected_data_directory",
        "terminal_marker_directory",
        "server_log_path",
        "expected_system_identifier",
        "authorization_file",
        "installed_dso_path",
        "installed_control_dso_path",
        "installed_control_path",
        "installed_sql_path",
        "restart_command",
        "stage_manifest_path",
        "stage_manifest_sha256",
        "tool_signature_suite_sha256",
    }
)
CLUSTER_AUTHORITY_FIELDS = frozenset(
    {
        "schema_revision",
        "profile",
        "cluster_system_identifier",
        "data_directory",
        "socket_directory",
        "connection_port",
        "connection_user",
        "original_authorization_path",
        "authorization_mode",
        "authorization_uid",
        "authorization_sha256",
        "authorization_artifact_path",
        "authorization_artifact_sha256",
    }
)
STAGE_MANIFEST_SCHEMA = "bioseq.private_postgres.stage_manifest"
STAGE_MANIFEST_SCHEMA_REVISION = 3
GENERATED_SQL_MANIFEST_FIELDS = frozenset(
    {
        "schema_revision",
        "authority_mode",
        "compiler_profile",
        "clang_version",
        "compilation_signature",
        "extension_name",
        "extension_schema",
        "generated_module_prefix",
        "fmgr_boundary_module",
        "script_sha256",
        "statement_count",
        "statements",
        "entity_count",
        "public_entity_count",
        "public_stable_identities",
        "public_installed_identities",
        "routine_entity_count",
        "c_routine_identity_count",
        "c_link_symbol_inventory_count",
        "c_finfo_symbol_inventory_count",
        "c_wrapper_entity_count",
        "generated_wrapper_symbol_count",
        "handwritten_fmgr_export_symbol_count",
        "handwritten_fmgr_export_external_symbol_count",
        "handwritten_fmgr_export_owner_count",
        "c_symbol_sharing_group_count",
        "generated_cpp_profile_invariant",
        "entity_handle_count",
        "entity_handle_module_count",
        "entity_handle_modules",
        "row_codec_count",
        "row_codec_module_count",
        "row_codec_modules",
        "routine_boundary_contract_count",
        "routine_boundary_contracts",
        "result_shape_contract_count",
        "result_shape_contracts",
        "row_codec_contract_count",
        "row_codec_contracts",
        "c_routine_identities",
        "c_link_symbols",
        "c_finfo_symbols",
        "generated_wrapper_symbols",
        "handwritten_fmgr_export_symbols",
        "handwritten_fmgr_export_external_symbols",
        "fmgr_entrypoint_module_count",
        "fmgr_entrypoint_modules",
        "handwritten_fmgr_export_owners",
        "c_symbol_owners",
        "entity_counts_by_kind",
        "entities",
        "guc_generation",
    }
)
RUNTIME_CLOSURE_ALGORITHM = "postgresql.materialized_package"
RUNTIME_CLOSURE_SCHEMA_REVISION = 1
STAGE_MANIFEST_FIELDS = frozenset(
    {
        "schema",
        "schema_revision",
        "postgresql_version",
        "postgresql_configure",
        "source",
        "staged",
        "runtime_closure",
        "extension_artifacts",
        "expected_tool_paths",
        "tool_signatures",
    }
)
STAGE_LAYOUT_FIELDS = frozenset({"bindir", "sharedir", "pkglibdir"})
STAGE_RUNTIME_CLOSURE_FIELDS = frozenset(
    {
        "algorithm",
        "schema_revision",
        "entry_count",
        "aggregate_sha256",
        "roots",
        "required_sentinels",
        "loader_dependencies",
        "entries",
    }
)
STAGE_RUNTIME_ROOT_FIELDS = frozenset(
    {"reported_source", "canonical_source", "staged"}
)
STAGE_RUNTIME_ENTRY_FIELDS = frozenset(
    {
        "root",
        "path",
        "kind",
        "source_kind",
        "source_path",
        "source_link_target",
        "resolved_source_path",
        "size",
        "sha256",
    }
)
STAGE_RUNTIME_SENTINEL_FIELDS = frozenset(
    {"root", "path", "kind", "sha256"}
)
STAGE_LOADER_CLOSURE_FIELDS = frozenset(
    {"entry_count", "aggregate_sha256", "entries"}
)
STAGE_LOADER_ENTRY_FIELDS = frozenset(
    {"source", "staged", "size", "sha256"}
)
STAGE_EXTENSION_CLOSURE_FIELDS = frozenset(
    {"aggregate_sha256", "artifacts"}
)
STAGE_EXTENSION_ARTIFACT_FIELDS = frozenset(
    {
        "name",
        "module_pathname_override",
        "effective_module_name",
        "control",
        "install_sql",
        "library",
    }
)
STAGE_CONTROL_ARTIFACT_FIELDS = frozenset(
    {"source", "filename", "staged", "source_sha256", "sha256"}
)
STAGE_INSTALL_SQL_ARTIFACT_FIELDS = frozenset(
    {"source", "filename", "staged", "version", "sha256"}
)
STAGE_LIBRARY_ARTIFACT_FIELDS = frozenset(
    {"source", "filename", "staged", "sha256"}
)
STAGE_TOOL_SIGNATURE_FIELDS = frozenset(
    {"tool", "staged_path", "version", "sha256"}
)
CASE_LOG_FIELDS = frozenset(
    {
        "schema_revision",
        "context",
        "command",
        "returncode",
        "stdout",
        "stderr",
        "bindings",
    }
)
CASE_LOG_BINDING_FIELDS = frozenset(
    {
        "request_sha256",
        "raw_result_sha256",
        "immutable_result_sha256",
        "activation_receipt_sha256",
        "catalog_sha256",
        "case_plan_sha256",
        "runtime_inventory_sha256",
        "ledger_sha256",
        "sql_manifest_sha256",
        "active_dso_sha256",
        "control_dso_sha256",
        "test_executable_sha256",
        "producer_executable_sha256",
        "cluster_config_sha256",
        "stage_manifest_sha256",
        "tool_signature_suite_sha256",
    }
)
SOURCE_POSITION_FIELDS = frozenset(
    {"byte_offset", "line", "column"}
)
SOURCE_RANGE_FIELDS = frozenset({"begin", "end"})
COMPILER_ANALYSIS_FACT_FIELDS = frozenset(
    {
        "callback_roots",
        "clang_version",
        "compile_database_sha256",
        "compile_graph_sha256",
        "compiler_profile",
        "functions",
        "pcm_manifest_sha256",
        "producer_executable_sha256",
        "profile",
        "root_usrs",
        "runtime_probes",
        "schema_revision",
        "source_list_sha256",
        "sql_manifest_sha256",
    }
)
POSTGRES_CALLBACK_ROOT_FIELDS = frozenset(
    {
        "family",
        "owner_qualified_name",
        "owner_usr",
        "registration",
        "slot",
        "stable_callback_id",
        "target_definition",
        "target_qualified_name",
        "target_usr",
    }
)
LOCATED_SOURCE_RANGE_FIELDS = frozenset({"source_file", "span"})
FUNCTION_FLOW_FIELDS = frozenset(
    {
        "ast_callsite_count",
        "body_available",
        "body_sha256",
        "body_span",
        "calls",
        "cfg_block_count",
        "cfg_complete",
        "cfg_element_count",
        "cfg_implicit_callsite_count",
        "cfg_sha256",
        "control_flow_assembly_sites",
        "declaration_span",
        "emitted_callsite_count",
        "implicit_call_edges_complete",
        "lifetime_analysis_complete",
        "lifetime_inventory_sha256",
        "lifetime_object_ids",
        "macro_expansion_complete",
        "module_name",
        "qualified_name",
        "source_file",
        "stable_usr",
        "template_instantiation_complete",
        "unavailable_bodies",
        "unresolved_indirect_targets",
    }
)
FLOW_CALL_FIELDS = frozenset(
    {
        "analyzer_entered_bridge_callable",
        "bridge_primitive_evidence",
        "bridge_transition",
        "callable_capture_objects",
        "caller_usr",
        "constant_integer_arguments",
        "expansion",
        "generated_boundary_tail",
        "live_objects",
        "macro_chain",
        "resolution",
        "spelling",
        "stable_site_id",
        "target_set_complete",
        "targets",
    }
)
FLOW_CALL_TARGET_FIELDS = frozenset(
    {
        "domain",
        "external_mode",
        "external_provenance",
        "pg_no_longjmp_allowlist_evidence",
        "target_qualified_name",
        "target_usr",
    }
)
FLOW_OBJECT_FIELDS = frozenset(
    {
        "canonical_type",
        "declaration",
        "declaration_usr",
        "identity_kind",
        "origin",
        "stable_object_id",
        "trivially_destructible",
    }
)
EXTERNAL_DECLARATION_PROVENANCE_FIELDS = frozenset(
    {
        "declaration_sha256",
        "declaration_span",
        "library_version",
        "record_id",
        "source_file",
        "user_code_callback_free",
    }
)
PROVENANCE_REFERENCE_FIELDS = frozenset(
    {
        "fragment_sha256",
        "postgresql_revision",
        "rationale",
        "record_id",
        "upstream_file",
        "upstream_span",
    }
)
BRIDGE_TRANSITION_EVIDENCE_FIELDS = frozenset(
    {
        "implementation_body_sha256",
        "implementation_contract_id",
        "implementation_contract_sha256",
        "implementation_definition",
        "implementation_module",
        "implementation_qualified_name",
        "implementation_usr",
        "primitive",
        "protected_region_id",
        "target_qualified_name",
        "target_usr",
        "transition_callsite",
    }
)
MACRO_EXPANSION_FIELDS = frozenset(
    {"definition", "definition_sha256", "invocation", "name"}
)
CONSTANT_INTEGER_ARGUMENT_FIELDS = frozenset({"ordinal", "value"})
POSTGRES_CALLBACK_FAMILIES = frozenset(
    {
        "background_worker",
        "custom_exec",
        "custom_path",
        "custom_scan",
        "error_context",
        "guc_callback",
        "memory_context",
        "postgres_hook",
        "process_exit_callback",
        "relcache_callback",
        "subxact_callback",
        "syscache_callback",
        "table_am",
        "tuple_table_slot",
        "xact_callback",
    }
)
FLOW_RESOLUTIONS = frozenset(
    {
        "direct",
        "resolved_indirect",
        "unresolved_indirect",
        "unavailable_body",
        "control_flow_assembly",
        "cycle_without_fixed_point",
    }
)
CALLEE_DOMAINS = frozenset(
    {
        "project",
        "postgresql_server",
        "standard_library",
        "system_library",
        "compiler_builtin",
        "unknown",
    }
)
EXTERNAL_CALL_MODES = frozenset(
    {
        "not_external",
        "audited_no_user_code",
        "analyze_body",
        "unknown",
    }
)
OBJECT_IDENTITY_KINDS = frozenset(
    {
        "declaration_usr",
        "synthetic_temporary",
        "synthetic_capture",
        "synthetic_coroutine_state",
    }
)
FLOW_OBJECT_ORIGINS = frozenset(
    {
        "automatic_object",
        "temporary",
        "lambda_capture",
        "catch_parameter",
        "coroutine_state",
    }
)
CALL_PATH_FIELDS = frozenset(
    {"usr", "qualified_declaration", "source_file", "source_span"}
)
RUNTIME_INVENTORY_FIELDS = frozenset(
    {
        "schema_revision",
        "profile",
        "probe_abi_revision",
        "ledger_sha256",
        "compiler_facts_sha256",
        "sql_manifest_sha256",
        "source_list_sha256",
        "compile_database_sha256",
        "compile_graph_sha256",
        "pcm_manifest_sha256",
        "inventory_producer_executable_sha256",
        "entries",
    }
)
RUNTIME_INVENTORY_ENTRY_FIELDS = frozenset(
    {
        "sql_identity",
        "fault_key",
        "site_id",
        "site_revision",
        "relation",
        "ledger_site_sha256",
        "site_anchor",
        "visit_anchor",
        "trigger_anchor",
        "transitive_call_path",
        "probe_contract",
    }
)
RUNTIME_ANCHOR_FIELDS = frozenset(
    {
        "kind",
        "compiler_stable_id",
        "enclosing_function_usr",
        "source_file",
        "expanded_source_span",
        "source_fragment_sha256",
    }
)
PROBE_CONTRACT_FIELDS = frozenset(
    {
        "abi_revision",
        "expected_visit_count",
        "expected_trigger_count",
        "ordering",
    }
)
RUNTIME_RELATION_CONTRACTS: dict[str, tuple[int, int, str]] = {
    "natural_at_site": (1, 0, "visit_only"),
    "injected_routes_to_site": (1, 1, "trigger_before_visit"),
    "injected_after_site_success": (1, 1, "visit_before_trigger"),
}
GENERATED_BOUNDARY_FAULT_KEY = "generated_boundary_tail.std_exception"
BACKEND_SCOPED_VACUUM_CLEANUP_FAULT_KEY = (
    "invoke_pg_as_cpp.vacuum_cleanup_restore"
)
MANDATORY_RUNTIME_POLICY_VALUES: dict[str, frozenset[str]] = {
    "classification": frozenset(
        {
            "bridged",
            "specialized_bridge",
            "lifetime_free_direct",
            "terminal_after_unwind",
        }
    ),
    "bridge": frozenset(
        {
            "invoke_pg_as_cpp",
            "sql_subtransaction",
            "sql_call_context",
            "generated_boundary_tail",
        }
    ),
    "converter_family": frozenset(
        {
            "aggregate_state",
            "base_type",
            "polymorphic",
            "scalar",
            "set_returning_function",
            "vector",
        }
    ),
    "owner_family": frozenset(
        {
            "bioseq.reap_orphan_seq_sub_tables",
            "bioseq.vacuum_seq_storage",
            "pg_catalog.alter_bioseq_table_reset",
            "pg_catalog.alter_bioseq_table_set",
        }
    ),
    "cleanup_phase": frozenset(
        {
            "memory_context_restore",
            "resource_owner_restore",
            "spi_connect",
            "spi_finish",
            "subtransaction_abort",
            "subtransaction_begin",
            "subtransaction_commit",
        }
    ),
}
PROVENANCE_KINDS = frozenset(
    {
        "recoverable_error_seed",
        "terminal_severity_seed",
        "bridge_primitive",
        "pg_no_longjmp_leaf",
        "standard_library_declaration",
        "system_library_declaration",
        "compiler_builtin_declaration",
    }
)
EXTERNAL_SYMBOLS = frozenset(
    {
        "bioseq_external_handler",
    }
)
DSO_CLOSURE_CONTRACTS = frozenset({"bioseq", "manifest"})
FMGR_OWNER_MACROS = frozenset(
    {
        "CXX_PG_FUNCTION_INFO_V1",
        "PG_FUNCTION_INFO_V1",
        "POSTGRES_FN",
        "E_POSTGRES_FN",
    }
)
ABSENT_VARCHAR_SYMBOLS = frozenset(
    {"vchareq", "vcharne", "vchargt", "vcharlt", "vcharle", "vcharge"}
)
TARGET_BPCHAR_SYMBOLS = frozenset(
    {
        "vbpchareq",
        "vbpcharne",
        "vbpchargt",
        "vbpcharlt",
        "vbpcharle",
        "vbpcharge",
    }
)

ELOG_H_SHA256 = (
    "e6b0864bf3fae574d5f6676ca3797dd7bccd70b69b5ebf1a0cdb86f635853e0d"
)
ELOG_C_SHA256 = (
    "c0fa4ad0198a13bd121080688b81d1faf8a352fb0852b01675b090f50c4db55c"
)
PALLOC_H_SHA256 = (
    "f6a9f047b0f343811a07f997c34aebd1b8b07e5fd60b1db82f7cae5ed2594f5a"
)
MCXT_C_SHA256 = (
    "4f4a96524812c55ff3b63a0cb22b69af258f8e69c7ddc20b2250a8c1769edc37"
)

PG17_REQUIRED_SEEDS: dict[str, dict[str, Any]] = {
    "pg17.severity_levels": {
        "kind": "terminal_severity_seed",
        "severities": {"error", "fatal", "panic"},
        "file": "src/include/utils/elog.h",
        "begin": 593,
        "end": 2155,
        "file_sha256": ELOG_H_SHA256,
        "fragment_sha256": (
            "b1c2030376e5858d484b65928856171de3b9f62957f2d26566795150027b5f6d"
        ),
    },
    "pg17.ereport_macro": {
        "kind": "recoverable_error_seed",
        "severities": {"error"},
        "file": "src/include/utils/elog.h",
        "begin": 6037,
        "end": 6985,
        "file_sha256": ELOG_H_SHA256,
        "fragment_sha256": (
            "790c1855de5610cfc507aa3221efcb40b241e59fb659d00463e9e25366330743"
        ),
    },
    "pg17.elog_macro": {
        "kind": "recoverable_error_seed",
        "severities": {"error"},
        "file": "src/include/utils/elog.h",
        "begin": 9575,
        "end": 9819,
        "file_sha256": ELOG_H_SHA256,
        "fragment_sha256": (
            "288f9d80a16a31057b6004c07b060a9cb7565bd197d31d0b20e2727d6317d848"
        ),
    },
    "pg17.pg_try_rethrow_macros": {
        "kind": "bridge_primitive",
        "severities": set(),
        "file": "src/include/utils/elog.h",
        "begin": 15244,
        "end": 16531,
        "file_sha256": ELOG_H_SHA256,
        "fragment_sha256": (
            "d96fc354e6c3e8eb0f0b531e32d0b97de0e2865f656f1557152ead276f8cdb49"
        ),
    },
    "pg17.rethrow_declarations": {
        "kind": "bridge_primitive",
        "severities": set(),
        "file": "src/include/utils/elog.h",
        "begin": 18571,
        "end": 18740,
        "file_sha256": ELOG_H_SHA256,
        "fragment_sha256": (
            "e32a0526deb30bfa66316389c51c4ce1e338e92b30e54a086fe0a92968fbf03a"
        ),
    },
    "pg17.errstart": {
        "kind": "recoverable_error_seed",
        "severities": {"error", "fatal", "panic"},
        "file": "src/backend/utils/error/elog.c",
        "begin": 11150,
        "end": 14888,
        "file_sha256": ELOG_C_SHA256,
        "fragment_sha256": (
            "a20f4d33c3d85bd4ffd35d9ff943e814e06d9c0825fa0ebce8696594858083af"
        ),
    },
    "pg17.errfinish": {
        "kind": "recoverable_error_seed",
        "severities": {"error", "fatal", "panic"},
        "file": "src/backend/utils/error/elog.c",
        "begin": 15040,
        "end": 19122,
        "file_sha256": ELOG_C_SHA256,
        "fragment_sha256": (
            "f04234bff935be4007825804dccd11adf5fb78c3685e8c6f13aadefc1c7bcac5"
        ),
    },
    "pg17.rethrow_error": {
        "kind": "recoverable_error_seed",
        "severities": {"error"},
        "file": "src/backend/utils/error/elog.c",
        "begin": 57136,
        "end": 58661,
        "file_sha256": ELOG_C_SHA256,
        "fragment_sha256": (
            "13286a15f4aa4d8f6b063d7b0a9ffb67cc75306886278ddbdf2d94189c2cbff5"
        ),
    },
    "pg17.pg_re_throw": {
        "kind": "recoverable_error_seed",
        "severities": {"error", "fatal"},
        "file": "src/backend/utils/error/elog.c",
        "begin": 58665,
        "end": 60663,
        "file_sha256": ELOG_C_SHA256,
        "fragment_sha256": (
            "c3c4b2cd89e2e9190ec7fd82b861f16b856cb7dda3b93ed5d9e8d8c42a659893"
        ),
    },
}

PG17_PINNED_NO_LONGJMP_LEAVES: dict[str, dict[str, Any]] = {
    "pg17.pfree": {
        "kind": "pg_no_longjmp_leaf",
        "severities": set(),
        "file": "src/include/utils/palloc.h",
        "begin": 3488,
        "end": 3520,
        "file_sha256": PALLOC_H_SHA256,
        "fragment_sha256": (
            "894b9cdaa6d1b9d5bbab45ca5c41c8c03f05ba078578bbd475a744049c585496"
        ),
        "definition_file": "src/backend/utils/mmgr/mcxt.c",
        "definition_begin": 47020,
        "definition_end": 47352,
        "definition_file_sha256": MCXT_C_SHA256,
        "definition_fragment_sha256": (
            "bc4585116ec9c6c32bc607184b57831d1b48a2918f3430a07808456f6e322a1a"
        ),
    },
}


class EvidenceError(ValueError):
    """Raised when generated evidence does not close the reviewed contract."""


@dataclass(frozen=True)
class LedgerPolicy:
    expected_parent_count: int = 272
    expected_symbol_count: int = 270
    shared_symbols: frozenset[str] = field(
        default_factory=lambda: frozenset({"from_array", "mmsearch"})
    )


@dataclass(frozen=True)
class DynamicSymbol:
    name: str
    binding: str
    symbol_type: str
    source_tool: str
    defined: bool = True
    dynamic_visible: bool = True


@dataclass(frozen=True)
class StageManifestBinding:
    manifest_path: Path
    manifest_sha256: str
    tool_signature_suite_sha256: str
    stage_root: Path
    bindir: Path
    sharedir: Path
    pkglibdir: Path
    pg_ctl_path: Path
    control_path: Path
    control_sha256: str
    install_sql_path: Path
    install_sql_sha256: str
    library_path: Path
    library_sha256: str


@dataclass(frozen=True)
class RetainedClusterAuthority:
    profile: str
    record_path: Path
    record_sha256: str
    cluster_system_identifier: str
    data_directory: Path
    socket_directory: Path
    connection_port: str
    connection_user: str
    original_authorization_path: Path
    authorization_artifact_path: Path
    authorization_sha256: str
    authorization_mode: int
    authorization_uid: int


@dataclass(frozen=True)
class ClusterConfigAuthority:
    socket_directory: Path
    data_directory: Path
    terminal_marker_directory: Path
    server_log_path: Path
    system_identifier: str
    active_dso_path: Path
    control_dso_path: Path
    installed_control_path: Path
    installed_sql_path: Path


def _fail(context: str, message: str) -> None:
    raise EvidenceError(f"PostgreSQL safety evidence rejected {context}: {message}")


def _object(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        _fail(context, "expected a JSON object")
    return value


def _array(value: Any, context: str) -> list[Any]:
    if not isinstance(value, list):
        _fail(context, "expected a JSON array")
    return value


def _field(value: dict[str, Any], name: str, context: str) -> Any:
    if name not in value:
        _fail(context, f"missing required field {name!r}")
    return value[name]


def _require_exact_fields(
    value: Mapping[str, Any],
    expected: set[str] | frozenset[str],
    context: str,
) -> None:
    actual = set(value)
    missing = sorted(expected - actual)
    unknown = sorted(actual - expected)
    if not missing and not unknown:
        return
    diagnostics: list[str] = []
    if missing:
        diagnostics.append("missing fields: " + ", ".join(missing))
    if unknown:
        diagnostics.append("unknown fields: " + ", ".join(unknown))
    _fail(context, "; ".join(diagnostics))


def _require_current_generated_sql_manifest(
    manifest: Mapping[str, Any], context: str
) -> None:
    unknown = sorted(set(manifest) - GENERATED_SQL_MANIFEST_FIELDS)
    if unknown:
        _fail(context, "unknown fields: " + ", ".join(unknown))
    if (
        _integer(
            _field(manifest, "schema_revision", context),
            f"{context}.schema_revision",
        )
        != 1
    ):
        _fail(context, "unsupported schema revision")


def _string(value: Any, context: str) -> str:
    if not isinstance(value, str) or not value:
        _fail(context, "expected a non-empty string")
    return value


def _fault_key(value: Any, context: str) -> str:
    result = _string(value, context)
    if FAULT_KEY_PATTERN.fullmatch(result) is None:
        _fail(
            context,
            "expected 1-128 bytes containing only [a-z0-9_.]",
        )
    return result


def _boolean(value: Any, context: str) -> bool:
    if not isinstance(value, bool):
        _fail(context, "expected a boolean")
    return value


def _integer(value: Any, context: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        _fail(context, "expected a non-negative integer")
    return value


def _sha256(value: Any, context: str) -> str:
    text = _string(value, context)
    if SHA256_PATTERN.fullmatch(text) is None:
        _fail(context, "expected a lowercase SHA-256 value")
    return text


def _string_list(
    value: Any,
    context: str,
    *,
    allow_empty: bool = True,
) -> list[str]:
    result = [_string(item, f"{context}[{index}]") for index, item in enumerate(
        _array(value, context)
    )]
    if not allow_empty and not result:
        _fail(context, "array must not be empty")
    if len(set(result)) != len(result):
        _fail(context, "array contains duplicates")
    return result


def _runtime_event_list(
    value: Any,
    context: str,
    *,
    allow_empty: bool,
) -> list[str]:
    result = _string_list(
        value,
        context,
        allow_empty=allow_empty,
    )
    for event in result:
        if RUNTIME_EVENT_PATTERN.fullmatch(event) is None:
            _fail(context, "contains an invalid runtime event name")
    return result


def _source_span(value: Any, context: str) -> tuple[int, int]:
    span = _object(value, context)
    begin = _object(_field(span, "begin", context), f"{context}.begin")
    end = _object(_field(span, "end", context), f"{context}.end")
    begin_offset = _integer(
        _field(begin, "byte_offset", f"{context}.begin"),
        f"{context}.begin.byte_offset",
    )
    end_offset = _integer(
        _field(end, "byte_offset", f"{context}.end"),
        f"{context}.end.byte_offset",
    )
    begin_line = _integer(
        _field(begin, "line", f"{context}.begin"), f"{context}.begin.line"
    )
    begin_column = _integer(
        _field(begin, "column", f"{context}.begin"), f"{context}.begin.column"
    )
    end_line = _integer(
        _field(end, "line", f"{context}.end"), f"{context}.end.line"
    )
    end_column = _integer(
        _field(end, "column", f"{context}.end"), f"{context}.end.column"
    )
    if (
        begin_line == 0
        or begin_column == 0
        or end_line == 0
        or end_column == 0
        or end_offset <= begin_offset
        or (end_line, end_column) < (begin_line, begin_column)
    ):
        _fail(context, "source span is empty or ordered incorrectly")
    return begin_offset, end_offset


def _reject_duplicate_json_keys(
    pairs: list[tuple[str, Any]],
) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise EvidenceError(
                f"JSON object contains duplicate key {key!r}"
            )
        result[key] = value
    return result


def load_json(
    path: Path,
    *,
    require_single_link: bool = False,
) -> tuple[dict[str, Any], bytes]:
    try:
        _, raw, _ = _authenticated_file_bytes(
            path,
            str(path),
            require_single_link=require_single_link,
        )
        value = json.loads(
            raw,
            object_pairs_hook=_reject_duplicate_json_keys,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise EvidenceError(f"cannot read {path}: {error}") from error
    return _object(value, str(path)), raw


def _postgresql_source_path(value: str) -> bool:
    path = Path(value)
    return (
        not path.is_absolute()
        and path.parts
        and path.parts[0] == "src"
        and ".." not in path.parts
        and str(path) == value
    )


def _absolute_normalized_path(value: str) -> bool:
    path = Path(value)
    return (
        path.is_absolute()
        and ".." not in path.parts
        and str(path) == value
    )


def _absolute_path(path: Path) -> Path:
    return Path(os.path.abspath(os.fspath(path)))


def _open_directory(path: Path, context: str) -> int:
    absolute = _absolute_path(path)
    descriptor = -1
    try:
        descriptor = os.open(
            absolute.anchor,
            os.O_RDONLY | os.O_DIRECTORY,
        )
        for component in absolute.parts[1:]:
            next_descriptor = os.open(
                component,
                os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                dir_fd=descriptor,
            )
            metadata = os.fstat(next_descriptor)
            if not stat.S_ISDIR(metadata.st_mode):
                os.close(next_descriptor)
                _fail(context, f"path component is not a directory: {component}")
            os.close(descriptor)
            descriptor = next_descriptor
        return descriptor
    except OSError as error:
        if descriptor >= 0:
            os.close(descriptor)
        raise EvidenceError(
            "PostgreSQL safety evidence rejected "
            f"{context}: path contains a symbolic link or invalid directory "
            f"component in {absolute}: {error}"
        ) from error


def _descriptor_bytes(
    descriptor: int,
    context: str,
    *,
    require_single_link: bool,
) -> tuple[bytes, os.stat_result]:
    before = os.fstat(descriptor)
    if not stat.S_ISREG(before.st_mode):
        _fail(context, "artifact is not a regular non-symlink file")
    if require_single_link and before.st_nlink != 1:
        _fail(context, "artifact must have exactly one hard link")
    os.lseek(descriptor, 0, os.SEEK_SET)
    chunks: list[bytes] = []
    while True:
        chunk = os.read(descriptor, 1024 * 1024)
        if not chunk:
            break
        chunks.append(chunk)
    after = os.fstat(descriptor)
    before_identity = (
        before.st_dev,
        before.st_ino,
        before.st_mode,
        before.st_nlink,
        before.st_size,
        before.st_mtime_ns,
        before.st_ctime_ns,
    )
    after_identity = (
        after.st_dev,
        after.st_ino,
        after.st_mode,
        after.st_nlink,
        after.st_size,
        after.st_mtime_ns,
        after.st_ctime_ns,
    )
    if before_identity != after_identity:
        _fail(context, "artifact changed while it was being read")
    return b"".join(chunks), after


def _authenticated_file_bytes(
    path: Path,
    context: str,
    *,
    require_single_link: bool,
) -> tuple[Path, bytes, os.stat_result]:
    absolute = _absolute_path(path)
    parent_descriptor = _open_directory(absolute.parent, f"{context}.parent")
    try:
        try:
            descriptor = os.open(
                absolute.name,
                os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                dir_fd=parent_descriptor,
            )
        except OSError as error:
            raise EvidenceError(
                "PostgreSQL safety evidence rejected "
                f"{context}: path contains a symbolic link or cannot be "
                f"opened as a file: {absolute}: {error}"
            ) from error
        try:
            raw, metadata = _descriptor_bytes(
                descriptor,
                context,
                require_single_link=require_single_link,
            )
        finally:
            os.close(descriptor)
    finally:
        os.close(parent_descriptor)
    return absolute, raw, metadata


def _authenticated_file_metadata(
    path: Path,
    context: str,
    *,
    require_single_link: bool,
) -> tuple[Path, os.stat_result]:
    absolute = _absolute_path(path)
    parent_descriptor = _open_directory(absolute.parent, f"{context}.parent")
    try:
        try:
            descriptor = os.open(
                absolute.name,
                os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                dir_fd=parent_descriptor,
            )
        except OSError as error:
            raise EvidenceError(
                "PostgreSQL safety evidence rejected "
                f"{context}: path contains a symbolic link or cannot be "
                f"opened as a file: {absolute}: {error}"
            ) from error
        try:
            metadata = os.fstat(descriptor)
            if not stat.S_ISREG(metadata.st_mode):
                _fail(context, "artifact is not a regular non-symlink file")
            if require_single_link and metadata.st_nlink != 1:
                _fail(context, "artifact must have exactly one hard link")
        finally:
            os.close(descriptor)
    finally:
        os.close(parent_descriptor)
    return absolute, metadata


def _canonical_regular_file(
    path: Path,
    context: str,
    *,
    require_single_link: bool = False,
) -> Path:
    canonical, _ = _authenticated_file_metadata(
        path,
        context,
        require_single_link=require_single_link,
    )
    return canonical


def _canonical_directory(path: Path, context: str) -> Path:
    absolute = _absolute_path(path)
    descriptor = _open_directory(absolute, context)
    try:
        if not stat.S_ISDIR(os.fstat(descriptor).st_mode):
            _fail(context, f"not a real directory: {absolute}")
    finally:
        os.close(descriptor)
    return absolute


def _candidate_root(runtime_matrix_path: Path) -> Path:
    matrix_path = _canonical_regular_file(
        runtime_matrix_path,
        "runtime-matrix",
        require_single_link=True,
    )
    return _canonical_directory(
        matrix_path.parent,
        "runtime-matrix-root",
    )


def _load_candidate_artifact(
    *,
    candidate_root: Path,
    path_text: Any,
    category: str,
    profile: str,
    stem: str,
    suffix: str,
    expected_sha256: Any,
    context: str,
    artifact_identities: dict[tuple[int, int], tuple[Any, ...]],
    semantic_identity: tuple[Any, ...],
    object_payload: bool = True,
) -> tuple[Any | None, bytes, Path]:
    relative_text = _string(path_text, f"{context}.path")
    relative = Path(relative_text)
    expected_parent = Path("evidence") / category / profile
    if (
        relative.is_absolute()
        or ".." in relative.parts
        or str(relative) != relative_text
        or relative.parent != expected_parent
    ):
        _fail(
            context,
            "artifact path must be a normalized relative direct child of "
            f"{expected_parent}",
        )
    artifact_path = candidate_root / relative
    canonical, raw, metadata = _authenticated_file_bytes(
        artifact_path,
        context,
        require_single_link=True,
    )
    try:
        canonical.relative_to(candidate_root)
    except ValueError:
        _fail(context, "artifact escapes the candidate root")
    expected_hash = _sha256(expected_sha256, f"{context}.sha256")
    if _artifact_sha256(raw) != expected_hash:
        _fail(context, "artifact hash is stale")
    if canonical.name != f"{stem}.{expected_hash}.{suffix}":
        _fail(context, "content-addressed artifact name is stale")
    file_identity = (metadata.st_dev, metadata.st_ino)
    registered_identity = (semantic_identity, canonical)
    previous_identity = artifact_identities.setdefault(
        file_identity,
        registered_identity,
    )
    if previous_identity != registered_identity:
        _fail(
            context,
            "artifact aliases a different immutable path or hardlink",
        )
    payload: Any | None = None
    if suffix.endswith("json"):
        try:
            payload = json.loads(
                raw,
                object_pairs_hook=_reject_duplicate_json_keys,
            )
            if object_payload:
                payload = _object(payload, context)
        except json.JSONDecodeError as error:
            raise EvidenceError(
                f"{context}: immutable artifact is not JSON: {error}"
            ) from error
    return payload, raw, canonical


def validate_provenance(bundle: dict[str, Any]) -> dict[str, dict[str, Any]]:
    _require_exact_fields(bundle, PROVENANCE_FIELDS, "provenance")
    if _integer(
        _field(bundle, "schema_revision", "provenance"),
        "provenance.schema_revision",
    ) != 2:
        _fail("provenance", "unsupported schema revision")
    if _integer(
        _field(bundle, "postgresql_major", "provenance"),
        "provenance.postgresql_major",
    ) != 17:
        _fail("provenance", "PostgreSQL major version is not 17")
    if _field(bundle, "postgresql_tag", "provenance") != PG17_TAG:
        _fail("provenance", f"tag must be exactly {PG17_TAG}")
    revision = _field(bundle, "postgresql_revision", "provenance")
    if revision != PG17_REVISION or REVISION_PATTERN.fullmatch(str(revision)) is None:
        _fail("provenance", "PostgreSQL source revision is stale")

    records: dict[str, dict[str, Any]] = {}
    seed_classes: set[str] = set()
    whole_file_hashes: dict[str, str] = {}
    target_authorities: set[tuple[str, str, str]] = set()
    target_locations: set[tuple[str, int, int]] = set()
    for index, raw_record in enumerate(
        _array(_field(bundle, "records", "provenance"), "provenance.records")
    ):
        context = f"provenance.records[{index}]"
        record = _object(raw_record, context)
        record_id = _string(
            _field(record, "record_id", context),
            f"{context}.record_id",
        )
        if record_id in records:
            _fail(record_id, "duplicate provenance record")
        kind = _field(record, "kind", record_id)
        if kind not in PROVENANCE_KINDS:
            _fail(record_id, "unknown provenance kind")
        seed_classes.add(kind)
        _string(_field(record, "spelling", record_id), f"{record_id}.spelling")
        upstream_file = _string(
            _field(record, "upstream_file", record_id),
            f"{record_id}.upstream_file",
        )
        upstream_span = _source_span(
            _field(record, "upstream_span", record_id),
            f"{record_id}.upstream_span",
        )
        upstream_file_sha256 = _sha256(
            _field(record, "upstream_file_sha256", record_id),
            f"{record_id}.upstream_file_sha256",
        )
        _sha256(
            _field(record, "fragment_sha256", record_id),
            f"{record_id}.fragment_sha256",
        )
        _string(_field(record, "rationale", record_id), f"{record_id}.rationale")
        severities = _string_list(
            _field(record, "severities", record_id), f"{record_id}.severities"
        )
        if (
            not set(severities) <= SEVERITIES
            or len(set(severities)) != len(severities)
            or "unresolved" in severities
        ):
            _fail(record_id, "unknown severity")
        previous_file_sha256 = whole_file_hashes.setdefault(
            upstream_file,
            upstream_file_sha256,
        )
        if previous_file_sha256 != upstream_file_sha256:
            _fail(
                record_id,
                "one provenance file has conflicting whole-file hashes",
            )
        if kind == "terminal_severity_seed" and not (
            TERMINAL_SEVERITIES <= set(severities)
        ):
            _fail(record_id, "terminal seed must contain FATAL and PANIC")
        if kind == "pg_no_longjmp_leaf" and severities:
            _fail(record_id, "no-longjmp allowlist evidence must be severity-free")
        library_declaration = kind in {
            "standard_library_declaration",
            "system_library_declaration",
            "compiler_builtin_declaration",
        }
        library_version = record.get("library_version", "")
        if not isinstance(library_version, str):
            _fail(
                record_id,
                "library_version must be a string",
            )
        if library_declaration:
            library_version = _string(
                _field(record, "library_version", record_id),
                f"{record_id}.library_version",
            )
            record["user_code_callback_free"] = _boolean(
                _field(
                    record,
                    "user_code_callback_free",
                    record_id,
                ),
                f"{record_id}.user_code_callback_free",
            )
            authority_id = record.get("authority_id", "")
            if (
                not isinstance(authority_id, str)
                or authority_id != library_version
                or not authority_id.startswith("clang:")
            ):
                _fail(
                    record_id,
                    "library provenance has no exact compiler/library authority",
                )
        else:
            callback_free = record.get(
                "user_code_callback_free", False
            )
            if "user_code_callback_free" in record:
                callback_free = _boolean(
                    callback_free,
                    f"{record_id}.user_code_callback_free",
                )
            if library_version or callback_free:
                _fail(
                    record_id,
                    "non-library provenance carries a library "
                    "version or callback contract",
                )
        target_record = kind in {
            "pg_no_longjmp_leaf",
            "standard_library_declaration",
            "system_library_declaration",
            "compiler_builtin_declaration",
        }
        for field_name in (
            "target_usr",
            "target_qualified_name",
            "authority_id",
        ):
            value = record.get(field_name, "")
            if target_record:
                _string(value, f"{record_id}.{field_name}")
            elif value != "":
                _fail(
                    record_id,
                    "non-target provenance carries target authority keys",
                )
        if target_record:
            target_key = (
                record["target_usr"],
                record["target_qualified_name"],
                record["authority_id"],
            )
            if target_key in target_authorities:
                _fail(
                    record_id,
                    "multiple provenance records claim one target and authority",
                )
            target_authorities.add(target_key)
            source_key = (
                upstream_file,
                upstream_span[0],
                upstream_span[1],
            )
            if source_key in target_locations:
                _fail(
                    record_id,
                    "multiple target records claim one declaration source range",
                )
            target_locations.add(source_key)

        definition = record.get("definition")
        if kind == "pg_no_longjmp_leaf":
            if (
                record["authority_id"] != PG17_REVISION
                or not _postgresql_source_path(upstream_file)
            ):
                _fail(
                    record_id,
                    "PostgreSQL leaf is not bound to the pinned source revision",
                )
            definition_context = f"{record_id}.definition"
            definition = _object(
                _field(record, "definition", record_id),
                definition_context,
            )
            _require_exact_fields(
                definition,
                {
                    "upstream_file",
                    "upstream_span",
                    "upstream_file_sha256",
                    "fragment_sha256",
                },
                definition_context,
            )
            definition_file = _string(
                definition["upstream_file"],
                f"{definition_context}.upstream_file",
            )
            if not _postgresql_source_path(definition_file):
                _fail(
                    definition_context,
                    "definition path is not a normalized PostgreSQL source path",
                )
            _source_span(
                definition["upstream_span"],
                f"{definition_context}.upstream_span",
            )
            definition_file_sha256 = _sha256(
                definition["upstream_file_sha256"],
                f"{definition_context}.upstream_file_sha256",
            )
            _sha256(
                definition["fragment_sha256"],
                f"{definition_context}.fragment_sha256",
            )
            previous_definition_sha256 = whole_file_hashes.setdefault(
                definition_file,
                definition_file_sha256,
            )
            if previous_definition_sha256 != definition_file_sha256:
                _fail(
                    record_id,
                    "one provenance file has conflicting whole-file hashes",
                )
        elif definition is not None:
            _fail(
                record_id,
                "non-PostgreSQL-leaf provenance carries a definition chain",
            )
        if kind in {
            "standard_library_declaration",
            "system_library_declaration",
        } and not _absolute_normalized_path(upstream_file):
            _fail(
                record_id,
                "standard/system declaration path is not absolute and normalized",
            )
        if (
            kind == "compiler_builtin_declaration"
            and upstream_file
            != f"clang-builtin:{record['authority_id']}"
        ):
            _fail(
                record_id,
                "compiler builtin provenance has a foreign authority path",
            )
        records[record_id] = record

    for record_id, expected in PG17_REQUIRED_SEEDS.items():
        record = records.get(record_id)
        if record is None:
            _fail("provenance", f"missing required record {record_id}")
        span = _source_span(record["upstream_span"], f"{record_id}.upstream_span")
        actual = {
            "kind": record["kind"],
            "severities": set(record["severities"]),
            "file": record["upstream_file"],
            "begin": span[0],
            "end": span[1],
            "file_sha256": record["upstream_file_sha256"],
            "fragment_sha256": record["fragment_sha256"],
        }
        if actual != expected:
            _fail(record_id, "PostgreSQL file, byte span, or hash is stale")

    for record_id, expected in PG17_PINNED_NO_LONGJMP_LEAVES.items():
        record = records.get(record_id)
        if record is None:
            continue
        span = _source_span(
            record["upstream_span"],
            f"{record_id}.upstream_span",
        )
        definition = _object(
            record["definition"],
            f"{record_id}.definition",
        )
        definition_span = _source_span(
            definition["upstream_span"],
            f"{record_id}.definition.upstream_span",
        )
        actual = {
            "kind": record["kind"],
            "severities": set(record["severities"]),
            "file": record["upstream_file"],
            "begin": span[0],
            "end": span[1],
            "file_sha256": record["upstream_file_sha256"],
            "fragment_sha256": record["fragment_sha256"],
            "definition_file": definition["upstream_file"],
            "definition_begin": definition_span[0],
            "definition_end": definition_span[1],
            "definition_file_sha256": definition[
                "upstream_file_sha256"
            ],
            "definition_fragment_sha256": definition[
                "fragment_sha256"
            ],
        }
        if (
            actual != expected
            or actual["definition_file"] == actual["file"]
        ):
            _fail(
                record_id,
                "pinned PostgreSQL leaf declaration/definition provenance is stale",
            )

    if not {
        "recoverable_error_seed",
        "terminal_severity_seed",
        "bridge_primitive",
    } <= seed_classes:
        _fail("provenance", "recoverable, terminal, and bridge seeds are required")

    allowlist = set(
        _string_list(
            _field(bundle, "pg_no_longjmp_allowlist", "provenance"),
            "provenance.pg_no_longjmp_allowlist",
        )
    )
    record_allowlist = {
        record_id
        for record_id, record in records.items()
        if record["kind"] == "pg_no_longjmp_leaf"
    }
    if allowlist != record_allowlist:
        _fail("provenance", "allowlist IDs do not match reviewed leaf records")
    return records


def _validate_crossing(value: Any, context: str) -> dict[str, Any]:
    crossing = _object(value, context)
    _string(_field(crossing, "canonical_type", context), f"{context}.canonical_type")
    origin = _field(crossing, "origin", context)
    if origin not in CROSSING_ORIGINS:
        _fail(context, "unknown crossing origin")
    _string(_field(crossing, "source_file", context), f"{context}.source_file")
    _source_span(
        _field(crossing, "declaration_span", context),
        f"{context}.declaration_span",
    )
    _boolean(
        _field(crossing, "trivially_destructible", context),
        f"{context}.trivially_destructible",
    )
    _boolean(
        _field(crossing, "crosses_postgres_longjmp", context),
        f"{context}.crosses_postgres_longjmp",
    )
    if crossing["crosses_postgres_longjmp"] and not crossing["trivially_destructible"]:
        _fail(context, "nontrivially destructible type crosses PostgreSQL longjmp")
    return crossing


def _validate_allowlist_reference(
    value: Any,
    context: str,
    provenance: dict[str, dict[str, Any]],
) -> None:
    reference = _object(value, context)
    record_id = _string(_field(reference, "record_id", context), f"{context}.record_id")
    record = provenance.get(record_id)
    if record is None or record["kind"] != "pg_no_longjmp_leaf":
        _fail(context, "reference does not name a reviewed PostgreSQL leaf")
    if (
        _field(reference, "postgresql_revision", context) != PG17_REVISION
        or _field(reference, "upstream_file", context) != record["upstream_file"]
        or _source_span(
            _field(reference, "upstream_span", context),
            f"{context}.upstream_span",
        )
        != _source_span(record["upstream_span"], f"{record_id}.upstream_span")
        or _sha256(
            _field(reference, "fragment_sha256", context),
            f"{context}.fragment_sha256",
        )
        != record["fragment_sha256"]
    ):
        _fail(context, "no-longjmp evidence is stale")
    if _string(
        _field(reference, "rationale", context),
        f"{context}.rationale",
    ) != record["rationale"]:
        _fail(context, "no-longjmp rationale is stale")
    if set(record["severities"]) & TERMINAL_SEVERITIES:
        _fail(context, "terminal severity cannot be allowlisted")


def _declaration_leaf(value: str) -> str:
    name = value.removeprefix("::").split("(", 1)[0]
    return name.rsplit("::", 1)[-1]


def _validate_audited_stop(
    value: Any,
    context: str,
    provenance: dict[str, dict[str, Any]],
) -> tuple[str, dict[str, Any]]:
    stop = _object(value, context)
    stop_id = _string(
        _field(stop, "stop_id", context),
        f"{context}.stop_id",
    )
    _string(
        _field(stop, "source_file", stop_id),
        f"{stop_id}.source_file",
    )
    _source_span(
        _field(stop, "expanded_source_span", stop_id),
        f"{stop_id}.expanded_source_span",
    )
    _string(
        _field(stop, "target_usr", stop_id),
        f"{stop_id}.target_usr",
    )
    target_name = _string(
        _field(stop, "target_qualified_name", stop_id),
        f"{stop_id}.target_qualified_name",
    )
    if _field(stop, "resolution", stop_id) not in RESOLVED_TARGETS:
        _fail(stop_id, "audited stop target is unresolved")
    domain = _field(stop, "domain", stop_id)
    required_kind = {
        "postgresql_server": "pg_no_longjmp_leaf",
        "standard_library": "standard_library_declaration",
        "system_library": "system_library_declaration",
        "compiler_builtin": "compiler_builtin_declaration",
    }.get(domain)
    if required_kind is None:
        _fail(stop_id, "audited stop has an unknown domain")
    path = _array(
        _field(stop, "transitive_call_path", stop_id),
        f"{stop_id}.transitive_call_path",
    )
    if not path:
        _fail(stop_id, "audited stop call path is empty")
    for index, raw_step in enumerate(path):
        step_context = f"{stop_id}.transitive_call_path[{index}]"
        step = _object(raw_step, step_context)
        for field_name in (
            "usr",
            "qualified_declaration",
            "source_file",
        ):
            _string(
                _field(step, field_name, step_context),
                f"{step_context}.{field_name}",
            )
        _source_span(
            _field(step, "source_span", step_context),
            f"{step_context}.source_span",
        )
    evidence_context = f"{stop_id}.evidence"
    evidence = _object(
        _field(stop, "evidence", stop_id),
        evidence_context,
    )
    record_id = _string(
        _field(evidence, "record_id", evidence_context),
        f"{evidence_context}.record_id",
    )
    record = provenance.get(record_id)
    if record is None or record["kind"] != required_kind:
        _fail(stop_id, "audited stop names the wrong provenance class")
    if (
        _field(evidence, "postgresql_revision", evidence_context)
            != PG17_REVISION
        or _field(evidence, "upstream_file", evidence_context)
            != record["upstream_file"]
        or _source_span(
            _field(evidence, "upstream_span", evidence_context),
            f"{evidence_context}.upstream_span",
        )
            != _source_span(
                record["upstream_span"],
                f"{record_id}.upstream_span",
            )
        or _sha256(
            _field(evidence, "fragment_sha256", evidence_context),
            f"{evidence_context}.fragment_sha256",
        )
            != record["fragment_sha256"]
        or _string(
            _field(evidence, "rationale", evidence_context),
            f"{evidence_context}.rationale",
        )
            != record["rationale"]
        or _declaration_leaf(record["spelling"])
            != _declaration_leaf(target_name)
        or record.get("target_usr") != stop["target_usr"]
        or record.get("target_qualified_name") != target_name
        or not record.get("authority_id")
        or record["severities"]
    ):
        _fail(stop_id, "audited stop provenance is stale or incomplete")
    library = domain != "postgresql_server"
    library_version = _field(stop, "library_version", stop_id)
    callback_free = _boolean(
        _field(stop, "user_code_callback_free", stop_id),
        f"{stop_id}.user_code_callback_free",
    )
    if (
        library
        and (
            _string(library_version, f"{stop_id}.library_version")
                != record["library_version"]
            or not callback_free
            or not record["user_code_callback_free"]
        )
    ) or (
        not library
        and (library_version != "" or callback_free)
    ):
        _fail(stop_id, "audited stop callback/library contract is stale")
    return stop_id, stop


def _validate_site(
    value: Any,
    context: str,
    provenance: dict[str, dict[str, Any]],
) -> tuple[str, dict[str, Any]]:
    site = _object(value, context)
    site_id = _string(_field(site, "site_id", context), f"{context}.site_id")
    _string(_field(site, "source_file", site_id), f"{site_id}.source_file")
    _source_span(
        _field(site, "expanded_source_span", site_id),
        f"{site_id}.expanded_source_span",
    )
    callee = _string(
        _field(site, "callee_or_macro", site_id), f"{site_id}.callee_or_macro"
    )
    path = _array(
        _field(site, "transitive_call_path", site_id),
        f"{site_id}.transitive_call_path",
    )
    if not path:
        _fail(site_id, "transitive call path is empty")
    for index, raw_step in enumerate(path):
        step_context = f"{site_id}.transitive_call_path[{index}]"
        step = _object(raw_step, step_context)
        _string(_field(step, "usr", step_context), f"{step_context}.usr")
        _string(
            _field(step, "qualified_declaration", step_context),
            f"{step_context}.qualified_declaration",
        )
        _string(
            _field(step, "source_file", step_context), f"{step_context}.source_file"
        )
        _source_span(
            _field(step, "source_span", step_context),
            f"{step_context}.source_span",
        )

    possible_severities = _string_list(
        _field(site, "possible_severities", site_id),
        f"{site_id}.possible_severities",
        allow_empty=False,
    )
    if set(possible_severities) & TERMINAL_SEVERITIES:
        _fail(site_id, "FATAL/PANIC is reachable")
    if possible_severities != ["error"] or not _boolean(
        _field(site, "severity_constant_propagated", site_id),
        f"{site_id}.severity_constant_propagated",
    ):
        _fail(site_id, "severity is unresolved or not exactly ERROR")

    resolution = _field(site, "resolution", site_id)
    if resolution not in RESOLVED_TARGETS:
        _fail(site_id, f"call target resolution is {resolution!r}")
    classification = _field(site, "classification", site_id)
    if classification not in CLASSIFICATIONS:
        _fail(site_id, "recoverable ERROR is unclassified")
    bridge = _field(site, "bridge", site_id)
    if bridge not in BRIDGES:
        _fail(site_id, "unknown bridge kind")
    bridge_evidence = _field(site, "bridge_evidence", site_id)
    if bridge == "none":
        if bridge_evidence is not None:
            _fail(site_id, "direct site carries bridge evidence")
    else:
        evidence_context = f"{site_id}.bridge_evidence"
        evidence = _object(bridge_evidence, evidence_context)
        primitive_context = f"{evidence_context}.primitive"
        primitive = _object(
            _field(evidence, "primitive", evidence_context),
            primitive_context,
        )
        record_id = _string(
            _field(primitive, "record_id", primitive_context),
            f"{primitive_context}.record_id",
        )
        expected_record = (
            "pg17.rethrow_declarations"
            if bridge == "generated_boundary_tail"
            else "pg17.pg_try_rethrow_macros"
        )
        record = provenance.get(record_id)
        if (
            record_id != expected_record
            or record is None
            or record["kind"] != "bridge_primitive"
            or _field(
                primitive,
                "postgresql_revision",
                primitive_context,
            )
                != PG17_REVISION
            or _field(
                primitive,
                "upstream_file",
                primitive_context,
            )
                != record["upstream_file"]
            or _source_span(
                _field(
                    primitive,
                    "upstream_span",
                    primitive_context,
                ),
                f"{primitive_context}.upstream_span",
            )
                != _source_span(
                    record["upstream_span"],
                    f"{record_id}.upstream_span",
                )
            or _sha256(
                _field(
                    primitive,
                    "fragment_sha256",
                    primitive_context,
                ),
                f"{primitive_context}.fragment_sha256",
            )
                != record["fragment_sha256"]
            or _string(
                _field(
                    primitive,
                    "rationale",
                    primitive_context,
                ),
                f"{primitive_context}.rationale",
            )
                != record["rationale"]
            or record["severities"]
        ):
            _fail(
                site_id,
                "bridge evidence does not name the exact pinned primitive",
            )
        for field_name in (
            "target_usr",
            "target_qualified_name",
            "transition_source_file",
            "implementation_usr",
            "implementation_qualified_name",
            "implementation_module",
            "implementation_source_file",
            "protected_region_id",
        ):
            _string(
                _field(evidence, field_name, evidence_context),
                f"{evidence_context}.{field_name}",
            )
        _source_span(
            _field(
                evidence,
                "transition_source_span",
                evidence_context,
            ),
            f"{evidence_context}.transition_source_span",
        )
        _source_span(
            _field(
                evidence,
                "implementation_source_span",
                evidence_context,
            ),
            f"{evidence_context}.implementation_source_span",
        )
        _sha256(
            _field(
                evidence,
                "implementation_body_sha256",
                evidence_context,
            ),
            f"{evidence_context}.implementation_body_sha256",
        )
        implementation_name = _field(
            evidence,
            "implementation_qualified_name",
            evidence_context,
        )
        implementation_module = _field(
            evidence,
            "implementation_module",
            evidence_context,
        )
        implementation_source = _field(
            evidence,
            "implementation_source_file",
            evidence_context,
        )
        protected_region = _field(
            evidence,
            "protected_region_id",
            evidence_context,
        )
        implementation_contract_id = _field(
            evidence,
            "implementation_contract_id",
            evidence_context,
        )
        implementation_contract_sha256 = _field(
            evidence,
            "implementation_contract_sha256",
            evidence_context,
        )
        if not isinstance(implementation_contract_id, str) or not isinstance(
            implementation_contract_sha256, str
        ):
            _fail(
                evidence_context,
                "bridge implementation contract fields must be strings",
            )
        target_name = _field(
            evidence,
            "target_qualified_name",
            evidence_context,
        )
        if bridge == "invoke_pg_as_cpp":
            expected_implementation = (
                "bioseq::postgres::support::invoke_pg_as_cpp"
            )
            expected_module = (
                "bioseq.postgres.support:error_boundary"
            )
            expected_source_suffix = (
                "/common/postgres/support/error_boundary.cppm"
            )
            expected_region = (
                "bioseq.postgres.support.error_boundary/"
                "invoke_pg_as_cpp"
            )
            expected_target = ""
            expected_contract_id = (
                "bioseq.postgres.support.error_boundary/error_capture"
            )
            expected_contract_sha256 = (
                "f217b3f824a03a5d3cf977fb25c8ace"
                "4a8765904e13e46dcfcc0fbd8c67f3099"
            )
        elif bridge == "sql_call_context":
            expected_implementation = (
                "bioseq::postgres::support::"
                "invoke_pg_across_transactions_as_cpp"
            )
            expected_module = (
                "bioseq.postgres.support:error_boundary"
            )
            expected_source_suffix = (
                "/common/postgres/support/error_boundary.cppm"
            )
            expected_region = (
                "bioseq.postgres.support.error_boundary/"
                "invoke_pg_across_transactions_as_cpp"
            )
            expected_target = ""
            expected_contract_id = (
                "bioseq.postgres.support.error_boundary/error_capture"
            )
            expected_contract_sha256 = (
                "f217b3f824a03a5d3cf977fb25c8ace"
                "4a8765904e13e46dcfcc0fbd8c67f3099"
            )
        elif bridge == "sql_subtransaction":
            exact_regions = {
                "bioseq::postgres::support::subtransaction::begin": (
                    "BeginInternalSubTransaction",
                    "begin",
                ),
                "bioseq::postgres::support::subtransaction::commit": (
                    "ReleaseCurrentSubTransaction",
                    "commit",
                ),
                "bioseq::postgres::support::subtransaction::rollback": (
                    "RollbackAndReleaseCurrentSubTransaction",
                    "rollback",
                ),
                (
                    "bioseq::postgres::support::subtransaction::"
                    "~subtransaction"
                ): (
                    "RollbackAndReleaseCurrentSubTransaction",
                    "~subtransaction",
                ),
            }
            region = exact_regions.get(implementation_name)
            if region is None:
                _fail(
                    site_id,
                    "SQL subtransaction bridge is not an exact "
                    "reviewed implementation",
                )
            expected_implementation = implementation_name
            expected_module = "bioseq.postgres.sql_boundary"
            expected_source_suffix = (
                "/common/postgres/sql_boundary/"
                "call_context_implementation.cppm"
            )
            expected_region = (
                "bioseq.postgres.support.sql_boundary.context/"
                + region[1]
            )
            expected_target = region[0]
            expected_contract_id = ""
            expected_contract_sha256 = ""
        elif bridge == "generated_boundary_tail":
            expected_implementation = (
                "bioseq::postgres::support::invoke_postgres_callback"
            )
            expected_module = "bioseq.postgres.sql_boundary"
            expected_source_suffix = (
                "/common/postgres/sql_boundary/index.cppm"
            )
            expected_region = (
                "bioseq.postgres.support.sql_boundary/rethrow_tail"
            )
            expected_target = ""
            expected_contract_id = ""
            expected_contract_sha256 = ""
        else:
            _fail(
                site_id,
                "bridge kind has no exact protected implementation",
            )
        target_matches = (
            not expected_target
            or target_name == expected_target
            or (
                "::" not in expected_target
                and target_name.endswith("::" + expected_target)
            )
        )
        if (
            implementation_name != expected_implementation
            or implementation_module != expected_module
            or not implementation_source.endswith(
                expected_source_suffix
            )
            or protected_region != expected_region
            or implementation_contract_id
                != expected_contract_id
            or implementation_contract_sha256
                != expected_contract_sha256
            or not target_matches
        ):
            _fail(
                site_id,
                "bridge evidence does not match the exact protected "
                "implementation",
            )

    crossing_types = [
        _validate_crossing(item, f"{site_id}.crossing_types[{index}]")
        for index, item in enumerate(
            _array(_field(site, "crossing_types", site_id), f"{site_id}.crossing_types")
        )
    ]
    callable_crossings = [
        _validate_crossing(
            item, f"{site_id}.bridge_callable_crossing_types[{index}]"
        )
        for index, item in enumerate(
            _array(
                _field(site, "bridge_callable_crossing_types", site_id),
                f"{site_id}.bridge_callable_crossing_types",
            )
        )
    ]
    if any(not item["trivially_destructible"] for item in callable_crossings):
        _fail(site_id, "bridge callable contains a nontrivial crossing type")

    entered_callable = _boolean(
        _field(site, "analyzer_entered_bridge_callable", site_id),
        f"{site_id}.analyzer_entered_bridge_callable",
    )
    generated_tail = _boolean(
        _field(site, "generated_boundary_tail", site_id),
        f"{site_id}.generated_boundary_tail",
    )
    if classification == "bridged":
        if bridge != "invoke_pg_as_cpp" or not entered_callable or generated_tail:
            _fail(site_id, "bridged proof does not enter invoke_pg_as_cpp")
    elif classification == "specialized_bridge":
        if (
            bridge not in {"sql_subtransaction", "sql_call_context"}
            or (
                bridge == "sql_subtransaction"
                and entered_callable
            )
            or (
                bridge == "sql_call_context"
                and not entered_callable
            )
            or generated_tail
        ):
            _fail(site_id, "specialized bridge is not an audited transaction bridge")
    elif classification == "lifetime_free_direct":
        if (
            bridge != "none"
            or entered_callable
            or generated_tail
            or callable_crossings
            or any(not item["trivially_destructible"] for item in crossing_types)
        ):
            _fail(site_id, "direct leaf is not lifetime-free")
    elif classification == "terminal_after_unwind":
        if (
            bridge != "generated_boundary_tail"
            or entered_callable
            or not generated_tail
            or callable_crossings
            or any(not item["trivially_destructible"] for item in crossing_types)
            or callee not in {"ReThrowError", "ereport"}
        ):
            _fail(site_id, "tail is not the generated post-unwind boundary")

    allowlist_evidence = _field(site, "no_longjmp_allowlist_evidence", site_id)
    if allowlist_evidence is not None:
        _validate_allowlist_reference(
            allowlist_evidence,
            f"{site_id}.no_longjmp_allowlist_evidence",
            provenance,
        )
    for field_name in ("converter_families", "owner_families", "cleanup_phases"):
        _string_list(
            _field(site, field_name, site_id), f"{site_id}.{field_name}"
        )
    return site_id, site


def _validate_analysis(value: Any, context: str) -> None:
    analysis = _object(value, context)
    for field_name in (
        "template_instantiation_complete",
        "macro_expansion_complete",
        "cfg_complete",
        "lifetime_analysis_complete",
        "call_graph_fixed_point_complete",
    ):
        if not _boolean(
            _field(analysis, field_name, context), f"{context}.{field_name}"
        ):
            _fail(context, "analysis closure is incomplete")
    for field_name in (
        "unresolved_indirect_targets",
        "unavailable_bodies",
        "control_flow_assembly_sites",
    ):
        if _string_list(
            _field(analysis, field_name, context), f"{context}.{field_name}"
        ):
            _fail(
                context,
                "analysis contains unresolved targets, bodies, or control flow",
            )


def _compiler_text(
    value: Any,
    context: str,
    *,
    allow_empty: bool = False,
) -> str:
    if not isinstance(value, str) or (not allow_empty and not value):
        qualifier = "" if allow_empty else " non-empty"
        _fail(context, f"expected a{qualifier} string")
    return value


def _compiler_string_array(value: Any, context: str) -> list[str]:
    return [
        _compiler_text(item, f"{context}[{index}]")
        for index, item in enumerate(_array(value, context))
    ]


def _cxx_string_key(value: str) -> bytes:
    """Match std::string's bytewise ordering for UTF-8 JSON strings."""

    return value.encode("utf-8")


def _compiler_range_fact(
    value: Any,
    context: str,
    *,
    require_valid: bool = True,
) -> str:
    span = _object(value, context)
    _require_exact_fields(span, SOURCE_RANGE_FIELDS, context)

    positions: list[tuple[int, int, int]] = []
    for name in ("begin", "end"):
        position_context = f"{context}.{name}"
        position = _object(
            _field(span, name, context),
            position_context,
        )
        _require_exact_fields(
            position,
            SOURCE_POSITION_FIELDS,
            position_context,
        )
        positions.append(
            (
                _integer(
                    _field(position, "byte_offset", position_context),
                    f"{position_context}.byte_offset",
                ),
                _integer(
                    _field(position, "line", position_context),
                    f"{position_context}.line",
                ),
                _integer(
                    _field(position, "column", position_context),
                    f"{position_context}.column",
                ),
            )
        )

    begin, end = positions
    if require_valid and (
        begin[1] == 0
        or begin[2] == 0
        or end[1] == 0
        or end[2] == 0
        or end[0] < begin[0]
        or (end[1], end[2]) < (begin[1], begin[2])
    ):
        _fail(context, "compiler source range is invalid")
    return (
        f"{begin[0]}:{begin[1]}:{begin[2]}-"
        f"{end[0]}:{end[1]}:{end[2]}"
    )


def _compiler_located_range(
    value: Any,
    context: str,
    *,
    require_valid: bool = True,
) -> tuple[str, str]:
    located = _object(value, context)
    _require_exact_fields(
        located,
        LOCATED_SOURCE_RANGE_FIELDS,
        context,
    )
    source_file = _compiler_text(
        _field(located, "source_file", context),
        f"{context}.source_file",
    )
    return (
        source_file,
        _compiler_range_fact(
            _field(located, "span", context),
            f"{context}.span",
            require_valid=require_valid,
        ),
    )


def _canonical_compiler_reference(value: Any, context: str) -> str:
    reference = _object(value, context)
    _require_exact_fields(
        reference,
        PROVENANCE_REFERENCE_FIELDS,
        context,
    )
    return "\t".join(
        (
            _compiler_text(
                _field(reference, "record_id", context),
                f"{context}.record_id",
            ),
            _compiler_text(
                _field(reference, "postgresql_revision", context),
                f"{context}.postgresql_revision",
            ),
            _compiler_text(
                _field(reference, "upstream_file", context),
                f"{context}.upstream_file",
            ),
            _compiler_range_fact(
                _field(reference, "upstream_span", context),
                f"{context}.upstream_span",
            ),
            _sha256(
                _field(reference, "fragment_sha256", context),
                f"{context}.fragment_sha256",
            ),
            _compiler_text(
                _field(reference, "rationale", context),
                f"{context}.rationale",
            ),
        )
    )


def _canonical_compiler_external_provenance(
    value: Any,
    context: str,
) -> str:
    provenance = _object(value, context)
    _require_exact_fields(
        provenance,
        EXTERNAL_DECLARATION_PROVENANCE_FIELDS,
        context,
    )
    return "\t".join(
        (
            _compiler_text(
                _field(provenance, "record_id", context),
                f"{context}.record_id",
            ),
            _compiler_text(
                _field(provenance, "source_file", context),
                f"{context}.source_file",
            ),
            _compiler_range_fact(
                _field(provenance, "declaration_span", context),
                f"{context}.declaration_span",
            ),
            _sha256(
                _field(provenance, "declaration_sha256", context),
                f"{context}.declaration_sha256",
            ),
            _compiler_text(
                _field(provenance, "library_version", context),
                f"{context}.library_version",
            ),
            (
                "callback-free"
                if _boolean(
                    _field(
                        provenance,
                        "user_code_callback_free",
                        context,
                    ),
                    f"{context}.user_code_callback_free",
                )
                else "may-callback"
            ),
        )
    )


def _canonical_compiler_flow_object(
    value: Any,
    context: str,
) -> str:
    flow_object = _object(value, context)
    _require_exact_fields(flow_object, FLOW_OBJECT_FIELDS, context)
    stable_object_id = _compiler_text(
        _field(flow_object, "stable_object_id", context),
        f"{context}.stable_object_id",
    )
    identity_kind = _field(flow_object, "identity_kind", context)
    if identity_kind not in OBJECT_IDENTITY_KINDS:
        _fail(context, "unknown compiler object identity kind")
    declaration_usr_value = _field(flow_object, "declaration_usr", context)
    if declaration_usr_value is None:
        declaration_usr = "-"
    else:
        declaration_usr = _compiler_text(
            declaration_usr_value,
            f"{context}.declaration_usr",
        )
    if (identity_kind == "declaration_usr") != (
        declaration_usr_value is not None
    ):
        _fail(
            context,
            "compiler object identity/declaration USR is inconsistent",
        )
    origin = _field(flow_object, "origin", context)
    if origin not in FLOW_OBJECT_ORIGINS:
        _fail(context, "unknown compiler object origin")
    canonical_origin = (
        "automatic" if origin == "automatic_object" else origin
    )
    declaration_source, declaration_range = _compiler_located_range(
        _field(flow_object, "declaration", context),
        f"{context}.declaration",
    )
    return "\t".join(
        (
            stable_object_id,
            identity_kind,
            declaration_usr,
            _compiler_text(
                _field(flow_object, "canonical_type", context),
                f"{context}.canonical_type",
            ),
            canonical_origin,
            declaration_source,
            declaration_range,
            (
                "trivial"
                if _boolean(
                    _field(
                        flow_object,
                        "trivially_destructible",
                        context,
                    ),
                    f"{context}.trivially_destructible",
                )
                else "nontrivial"
            ),
        )
    )


def _canonical_compiler_target(value: Any, context: str) -> str:
    target = _object(value, context)
    _require_exact_fields(target, FLOW_CALL_TARGET_FIELDS, context)
    target_usr = _compiler_text(
        _field(target, "target_usr", context),
        f"{context}.target_usr",
        allow_empty=True,
    )
    target_name = _compiler_text(
        _field(target, "target_qualified_name", context),
        f"{context}.target_qualified_name",
    )
    domain = _field(target, "domain", context)
    if domain not in CALLEE_DOMAINS:
        _fail(context, "unknown compiler callee domain")
    external_mode = _field(target, "external_mode", context)
    if external_mode not in EXTERNAL_CALL_MODES:
        _fail(context, "unknown compiler external-call mode")

    external_provenance = _field(
        target, "external_provenance", context
    )
    allowlist = _field(
        target,
        "pg_no_longjmp_allowlist_evidence",
        context,
    )
    external_domain = domain in {
        "standard_library",
        "system_library",
        "compiler_builtin",
    }
    if external_domain:
        if (
            external_mode == "not_external"
            or external_provenance is None
            or allowlist is not None
        ):
            _fail(context, "external target contract is inconsistent")
    elif external_mode != "not_external" or external_provenance is not None:
        _fail(context, "non-external target carries external provenance")
    if allowlist is not None and domain != "postgresql_server":
        _fail(context, "PostgreSQL allowlist is attached to another domain")

    fact = "\t".join(
        (target_usr, target_name, domain, external_mode)
    )
    if external_provenance is not None:
        fact += (
            "\tX\t"
            + _canonical_compiler_external_provenance(
                external_provenance,
                f"{context}.external_provenance",
            )
        )
    if allowlist is not None:
        fact += (
            "\tA\t"
            + _canonical_compiler_reference(
                allowlist,
                f"{context}.pg_no_longjmp_allowlist_evidence",
            )
        )
    return fact


def _canonical_compiler_bridge_evidence(
    value: Any,
    context: str,
) -> str:
    evidence = _object(value, context)
    _require_exact_fields(
        evidence,
        BRIDGE_TRANSITION_EVIDENCE_FIELDS,
        context,
    )
    transition_source, transition_range = _compiler_located_range(
        _field(evidence, "transition_callsite", context),
        f"{context}.transition_callsite",
    )
    implementation_source, implementation_range = (
        _compiler_located_range(
            _field(evidence, "implementation_definition", context),
            f"{context}.implementation_definition",
        )
    )
    return "\t".join(
        (
            _canonical_compiler_reference(
                _field(evidence, "primitive", context),
                f"{context}.primitive",
            ),
            _compiler_text(
                _field(evidence, "target_usr", context),
                f"{context}.target_usr",
            ),
            _compiler_text(
                _field(evidence, "target_qualified_name", context),
                f"{context}.target_qualified_name",
            ),
            transition_source,
            transition_range,
            _compiler_text(
                _field(evidence, "implementation_usr", context),
                f"{context}.implementation_usr",
            ),
            _compiler_text(
                _field(
                    evidence,
                    "implementation_qualified_name",
                    context,
                ),
                f"{context}.implementation_qualified_name",
            ),
            _compiler_text(
                _field(evidence, "implementation_module", context),
                f"{context}.implementation_module",
            ),
            implementation_source,
            implementation_range,
            _sha256(
                _field(
                    evidence,
                    "implementation_body_sha256",
                    context,
                ),
                f"{context}.implementation_body_sha256",
            ),
            _compiler_text(
                _field(
                    evidence,
                    "implementation_contract_id",
                    context,
                ),
                f"{context}.implementation_contract_id",
                allow_empty=True,
            ),
            _compiler_text(
                _field(
                    evidence,
                    "implementation_contract_sha256",
                    context,
                ),
                f"{context}.implementation_contract_sha256",
                allow_empty=True,
            ),
            _compiler_text(
                _field(evidence, "protected_region_id", context),
                f"{context}.protected_region_id",
            ),
        )
    )


def _canonical_compiler_call(value: Any, context: str) -> str:
    call = _object(value, context)
    _require_exact_fields(call, FLOW_CALL_FIELDS, context)
    stable_site_id = _compiler_text(
        _field(call, "stable_site_id", context),
        f"{context}.stable_site_id",
    )
    caller_usr = _compiler_text(
        _field(call, "caller_usr", context),
        f"{context}.caller_usr",
    )
    spelling_source, spelling_range = _compiler_located_range(
        _field(call, "spelling", context),
        f"{context}.spelling",
    )
    expansion_source, expansion_range = _compiler_located_range(
        _field(call, "expansion", context),
        f"{context}.expansion",
    )
    resolution = _field(call, "resolution", context)
    if resolution not in FLOW_RESOLUTIONS:
        _fail(context, "unknown compiler call resolution")
    target_set_complete = _boolean(
        _field(call, "target_set_complete", context),
        f"{context}.target_set_complete",
    )
    bridge = _field(call, "bridge_transition", context)
    if bridge not in BRIDGES:
        _fail(context, "unknown compiler bridge transition")
    entered = _boolean(
        _field(call, "analyzer_entered_bridge_callable", context),
        f"{context}.analyzer_entered_bridge_callable",
    )
    generated_tail = _boolean(
        _field(call, "generated_boundary_tail", context),
        f"{context}.generated_boundary_tail",
    )
    targets = _array(_field(call, "targets", context), f"{context}.targets")
    if (
        resolution in {"direct", "resolved_indirect"}
        and (not target_set_complete or not targets)
    ):
        _fail(context, "resolved compiler call lacks a complete target set")
    if resolution == "direct" and len(targets) != 1:
        _fail(context, "direct compiler call does not have one target")
    if (
        resolution not in {"direct", "resolved_indirect"}
        and target_set_complete
    ):
        _fail(context, "unresolved compiler call claims complete targets")

    bridge_evidence = _field(
        call,
        "bridge_primitive_evidence",
        context,
    )
    if (
        generated_tail != (bridge == "generated_boundary_tail")
        or (bridge == "none" and entered)
        or ((bridge == "none") != (bridge_evidence is None))
        or (bridge == "generated_boundary_tail" and entered)
    ):
        _fail(context, "compiler bridge transition flags are inconsistent")

    result = "\t".join(
        (
            stable_site_id,
            caller_usr,
            spelling_source,
            spelling_range,
            expansion_source,
            expansion_range,
            resolution,
            (
                "targets-complete"
                if target_set_complete
                else "targets-incomplete"
            ),
            bridge,
            "entered" if entered else "outside",
            "tail" if generated_tail else "ordinary",
        )
    )
    if bridge_evidence is not None:
        result += (
            "\nB\t"
            + _canonical_compiler_bridge_evidence(
                bridge_evidence,
                f"{context}.bridge_primitive_evidence",
            )
        )

    target_facts: list[str] = []
    target_identities: set[tuple[str, str]] = set()
    for index, raw_target in enumerate(targets):
        target_context = f"{context}.targets[{index}]"
        target = _object(raw_target, target_context)
        target_identity = (
            _compiler_text(
                _field(target, "target_usr", target_context),
                f"{target_context}.target_usr",
                allow_empty=True,
            ),
            _compiler_text(
                _field(
                    target,
                    "target_qualified_name",
                    target_context,
                ),
                f"{target_context}.target_qualified_name",
            ),
        )
        if target_identity in target_identities:
            _fail(context, "compiler call contains duplicate targets")
        target_identities.add(target_identity)
        target_facts.append(
            _canonical_compiler_target(target, target_context)
        )
    for target_fact in sorted(target_facts, key=_cxx_string_key):
        result += "\nT\t" + target_fact

    macros = _array(
        _field(call, "macro_chain", context),
        f"{context}.macro_chain",
    )
    for index, raw_macro in enumerate(macros):
        macro_context = f"{context}.macro_chain[{index}]"
        macro = _object(raw_macro, macro_context)
        _require_exact_fields(
            macro,
            MACRO_EXPANSION_FIELDS,
            macro_context,
        )
        invocation_source, invocation_range = _compiler_located_range(
            _field(macro, "invocation", macro_context),
            f"{macro_context}.invocation",
        )
        definition_source, definition_range = _compiler_located_range(
            _field(macro, "definition", macro_context),
            f"{macro_context}.definition",
        )
        result += "\nM\t" + "\t".join(
            (
                _compiler_text(
                    _field(macro, "name", macro_context),
                    f"{macro_context}.name",
                ),
                invocation_source,
                invocation_range,
                definition_source,
                definition_range,
                _sha256(
                    _field(macro, "definition_sha256", macro_context),
                    f"{macro_context}.definition_sha256",
                ),
            )
        )

    arguments: list[str] = []
    argument_ordinals: set[int] = set()
    for index, raw_argument in enumerate(
        _array(
            _field(call, "constant_integer_arguments", context),
            f"{context}.constant_integer_arguments",
        )
    ):
        argument_context = (
            f"{context}.constant_integer_arguments[{index}]"
        )
        argument = _object(raw_argument, argument_context)
        _require_exact_fields(
            argument,
            CONSTANT_INTEGER_ARGUMENT_FIELDS,
            argument_context,
        )
        ordinal = _integer(
            _field(argument, "ordinal", argument_context),
            f"{argument_context}.ordinal",
        )
        if ordinal in argument_ordinals:
            _fail(context, "compiler call duplicates a constant ordinal")
        argument_ordinals.add(ordinal)
        raw_value = _field(argument, "value", argument_context)
        if raw_value is None:
            value_text = "unresolved"
        else:
            if isinstance(raw_value, bool) or not isinstance(raw_value, int):
                _fail(
                    f"{argument_context}.value",
                    "expected a signed integer or null",
                )
            value_text = str(raw_value)
        arguments.append(f"{ordinal}\t{value_text}")
    for argument in sorted(arguments, key=_cxx_string_key):
        result += "\nI\t" + argument

    for field_name, prefix in (
        ("live_objects", "L"),
        ("callable_capture_objects", "C"),
    ):
        object_facts = [
            _canonical_compiler_flow_object(
                raw_object,
                f"{context}.{field_name}[{index}]",
            )
            for index, raw_object in enumerate(
                _array(
                    _field(call, field_name, context),
                    f"{context}.{field_name}",
                )
            )
        ]
        for object_fact in sorted(object_facts, key=_cxx_string_key):
            result += f"\n{prefix}\t{object_fact}"
    return result


def _canonical_compiler_function(
    function: dict[str, Any],
    context: str,
) -> str:
    _require_exact_fields(function, FUNCTION_FLOW_FIELDS, context)
    body_available = _boolean(
        _field(function, "body_available", context),
        f"{context}.body_available",
    )
    declaration_range = _compiler_range_fact(
        _field(function, "declaration_span", context),
        f"{context}.declaration_span",
    )
    body_range = _compiler_range_fact(
        _field(function, "body_span", context),
        f"{context}.body_span",
        require_valid=body_available,
    )
    lifetime_ids = _compiler_string_array(
        _field(function, "lifetime_object_ids", context),
        f"{context}.lifetime_object_ids",
    )
    result = "\nF\t" + "\t".join(
        (
            _compiler_text(
                _field(function, "stable_usr", context),
                f"{context}.stable_usr",
            ),
            _compiler_text(
                _field(function, "qualified_name", context),
                f"{context}.qualified_name",
            ),
            _compiler_text(
                _field(function, "source_file", context),
                f"{context}.source_file",
            ),
            _compiler_text(
                _field(function, "module_name", context),
                f"{context}.module_name",
                allow_empty=True,
            ),
            declaration_range,
            body_range,
            "body" if body_available else "declaration",
            (
                "templates-complete"
                if _boolean(
                    _field(
                        function,
                        "template_instantiation_complete",
                        context,
                    ),
                    f"{context}.template_instantiation_complete",
                )
                else "templates-incomplete"
            ),
            (
                "macros-complete"
                if _boolean(
                    _field(
                        function,
                        "macro_expansion_complete",
                        context,
                    ),
                    f"{context}.macro_expansion_complete",
                )
                else "macros-incomplete"
            ),
            (
                "cfg-complete"
                if _boolean(
                    _field(function, "cfg_complete", context),
                    f"{context}.cfg_complete",
                )
                else "cfg-incomplete"
            ),
            (
                "lifetimes-complete"
                if _boolean(
                    _field(
                        function,
                        "lifetime_analysis_complete",
                        context,
                    ),
                    f"{context}.lifetime_analysis_complete",
                )
                else "lifetimes-incomplete"
            ),
            (
                "implicit-edges-complete"
                if _boolean(
                    _field(
                        function,
                        "implicit_call_edges_complete",
                        context,
                    ),
                    f"{context}.implicit_call_edges_complete",
                )
                else "implicit-edges-incomplete"
            ),
            _compiler_text(
                _field(function, "body_sha256", context),
                f"{context}.body_sha256",
            ),
            _compiler_text(
                _field(function, "cfg_sha256", context),
                f"{context}.cfg_sha256",
            ),
            str(
                _integer(
                    _field(function, "cfg_block_count", context),
                    f"{context}.cfg_block_count",
                )
            ),
            str(
                _integer(
                    _field(function, "cfg_element_count", context),
                    f"{context}.cfg_element_count",
                )
            ),
            str(
                _integer(
                    _field(function, "ast_callsite_count", context),
                    f"{context}.ast_callsite_count",
                )
            ),
            str(
                _integer(
                    _field(
                        function,
                        "cfg_implicit_callsite_count",
                        context,
                    ),
                    f"{context}.cfg_implicit_callsite_count",
                )
            ),
            str(
                _integer(
                    _field(function, "emitted_callsite_count", context),
                    f"{context}.emitted_callsite_count",
                )
            ),
            _compiler_text(
                _field(function, "lifetime_inventory_sha256", context),
                f"{context}.lifetime_inventory_sha256",
            ),
        )
    )
    stable_usr = _field(function, "stable_usr", context)
    for object_id in lifetime_ids:
        result += f"\nO\t{stable_usr}\t{object_id}"
    return result


def _compiler_callback_root_header(
    value: Any,
    context: str,
) -> tuple[str, str, str, str, str]:
    root = _object(value, context)
    _require_exact_fields(root, POSTGRES_CALLBACK_ROOT_FIELDS, context)
    family = _field(root, "family", context)
    if family not in POSTGRES_CALLBACK_FAMILIES:
        _fail(context, "unknown PostgreSQL callback family")
    stable_callback_id = _compiler_text(
        _field(root, "stable_callback_id", context),
        f"{context}.stable_callback_id",
    )
    slot = _compiler_text(
        _field(root, "slot", context),
        f"{context}.slot",
    )
    owner_usr = _compiler_text(
        _field(root, "owner_usr", context),
        f"{context}.owner_usr",
    )
    owner_name = _compiler_text(
        _field(root, "owner_qualified_name", context),
        f"{context}.owner_qualified_name",
    )
    registration_source, registration_range = (
        _compiler_located_range(
            _field(root, "registration", context),
            f"{context}.registration",
        )
    )
    registration_span = _object(
        _field(
            _object(
                _field(root, "registration", context),
                f"{context}.registration",
            ),
            "span",
            f"{context}.registration",
        ),
        f"{context}.registration.span",
    )
    registration_begin = _integer(
        _field(
            _object(
                _field(
                    registration_span,
                    "begin",
                    f"{context}.registration.span",
                ),
                f"{context}.registration.span.begin",
            ),
            "byte_offset",
            f"{context}.registration.span.begin",
        ),
        f"{context}.registration.span.begin.byte_offset",
    )
    registration_end = _integer(
        _field(
            _object(
                _field(
                    registration_span,
                    "end",
                    f"{context}.registration.span",
                ),
                f"{context}.registration.span.end",
            ),
            "byte_offset",
            f"{context}.registration.span.end",
        ),
        f"{context}.registration.span.end.byte_offset",
    )
    target_usr = _compiler_text(
        _field(root, "target_usr", context),
        f"{context}.target_usr",
    )
    target_name = _compiler_text(
        _field(root, "target_qualified_name", context),
        f"{context}.target_qualified_name",
    )
    target_source, target_range = _compiler_located_range(
        _field(root, "target_definition", context),
        f"{context}.target_definition",
    )
    stable_material = "\n".join(
        (
            family,
            slot,
            owner_usr,
            target_usr,
            registration_source,
            str(registration_begin),
            str(registration_end),
        )
    )
    expected_callback_id = (
        "pg-callback:"
        + hashlib.sha256(stable_material.encode("utf-8")).hexdigest()[:32]
    )
    if stable_callback_id != expected_callback_id:
        _fail(context, "PostgreSQL callback stable identity is stale")
    return (
        "\nR\t"
        + "\t".join(
            (
                stable_callback_id,
                family,
                slot,
                owner_usr,
                owner_name,
                registration_source,
                registration_range,
                target_usr,
                target_name,
                target_source,
                target_range,
            )
        ),
        target_usr,
        target_name,
        target_source,
        target_range,
    )


def _compiler_callback_closure_material(
    root_function: dict[str, Any],
    functions_by_usr: dict[str, dict[str, Any]],
    context: str,
) -> str:
    canonicalized_functions: set[str] = set()
    canonicalized_calls: set[tuple[str, str]] = set()
    callsite_owner_by_identity: dict[str, str] = {}
    objects_by_identity: dict[str, bytes] = {}
    seen_contexts: dict[
        tuple[str, str, str],
        tuple[set[str], set[str]],
    ] = {}
    material: list[str] = []

    def register_object(raw_object: Any, object_context: str) -> str:
        flow_object = _object(raw_object, object_context)
        _canonical_compiler_flow_object(
            flow_object,
            object_context,
        )
        stable_object_id = _field(
            flow_object,
            "stable_object_id",
            object_context,
        )
        exact_object = _canonical_json_bytes(flow_object)
        previous = objects_by_identity.setdefault(
            stable_object_id,
            exact_object,
        )
        if previous != exact_object:
            _fail(
                object_context,
                "stable compiler object identity has conflicting facts",
            )
        return stable_object_id

    def append_unique_objects(
        destination: list[dict[str, Any]],
        incoming: list[Any],
        append_context: str,
    ) -> list[dict[str, Any]]:
        result = list(destination)
        by_id: dict[str, dict[str, Any]] = {}
        for index, raw_object in enumerate(result):
            object_context = f"{append_context}.existing[{index}]"
            parsed = _object(raw_object, object_context)
            object_id = register_object(parsed, object_context)
            by_id[object_id] = parsed
        for index, raw_object in enumerate(incoming):
            object_context = f"{append_context}.incoming[{index}]"
            parsed = _object(raw_object, object_context)
            object_id = register_object(parsed, object_context)
            if object_id not in by_id:
                by_id[object_id] = parsed
        return [
            by_id[object_id]
            for object_id in sorted(by_id, key=_cxx_string_key)
        ]

    def visit(
        function: dict[str, Any],
        active_bridge: str,
        active_transition_id: str,
        crossing_objects: list[dict[str, Any]],
        capture_objects: list[dict[str, Any]],
        depth: int,
    ) -> None:
        if depth > MAX_CALLBACK_CLOSURE_DEPTH:
            _fail(
                context,
                "callback closure exceeds the reviewed traversal "
                f"depth of {MAX_CALLBACK_CLOSURE_DEPTH}",
            )
        function_usr = _compiler_text(
            _field(function, "stable_usr", context),
            f"{context}.function.stable_usr",
        )
        context_key = (
            function_usr,
            active_bridge,
            active_transition_id,
        )
        crossing_ids = {
            register_object(
                raw_object,
                f"{context}.{function_usr}.crossing[{index}]",
            )
            for index, raw_object in enumerate(crossing_objects)
        }
        capture_ids = {
            register_object(
                raw_object,
                f"{context}.{function_usr}.capture[{index}]",
            )
            for index, raw_object in enumerate(capture_objects)
        }
        previous = seen_contexts.get(context_key)
        if previous is None:
            seen_contexts[context_key] = (
                set(crossing_ids),
                set(capture_ids),
            )
        else:
            previous_crossings, previous_captures = previous
            adds_information = bool(
                crossing_ids - previous_crossings
                or capture_ids - previous_captures
            )
            previous_crossings.update(crossing_ids)
            previous_captures.update(capture_ids)
            if not adds_information:
                return

        function_context = f"{context}.functions[{function_usr}]"
        if function_usr not in canonicalized_functions:
            canonicalized_functions.add(function_usr)
            material.append(
                _canonical_compiler_function(
                    function,
                    function_context,
                )
            )

        calls = _array(
            _field(function, "calls", function_context),
            f"{function_context}.calls",
        )
        callsite_ids: set[str] = set()
        ordered_calls: list[tuple[int, bytes, bytes, dict[str, Any]]] = []
        for index, raw_call in enumerate(calls):
            call_context = f"{function_context}.calls[{index}]"
            call = _object(raw_call, call_context)
            _require_exact_fields(call, FLOW_CALL_FIELDS, call_context)
            caller_usr = _compiler_text(
                _field(call, "caller_usr", call_context),
                f"{call_context}.caller_usr",
            )
            if caller_usr != function_usr:
                _fail(call_context, "compiler call has another owner")
            stable_site_id = _compiler_text(
                _field(call, "stable_site_id", call_context),
                f"{call_context}.stable_site_id",
            )
            if stable_site_id in callsite_ids:
                _fail(function_context, "duplicate compiler call-site id")
            callsite_ids.add(stable_site_id)
            previous_owner = callsite_owner_by_identity.setdefault(
                stable_site_id,
                function_usr,
            )
            if previous_owner != function_usr:
                _fail(
                    call_context,
                    "compiler call-site identity has multiple owners",
                )
            expansion = _object(
                _field(call, "expansion", call_context),
                f"{call_context}.expansion",
            )
            expansion_source, _ = _compiler_located_range(
                expansion,
                f"{call_context}.expansion",
            )
            expansion_span = _object(
                _field(
                    expansion,
                    "span",
                    f"{call_context}.expansion",
                ),
                f"{call_context}.expansion.span",
            )
            expansion_begin = _object(
                _field(
                    expansion_span,
                    "begin",
                    f"{call_context}.expansion.span",
                ),
                f"{call_context}.expansion.span.begin",
            )
            begin_offset = _integer(
                _field(
                    expansion_begin,
                    "byte_offset",
                    f"{call_context}.expansion.span.begin",
                ),
                f"{call_context}.expansion.span.begin.byte_offset",
            )
            for field_name in (
                "live_objects",
                "callable_capture_objects",
            ):
                for object_index, raw_object in enumerate(
                    _array(
                        _field(call, field_name, call_context),
                        f"{call_context}.{field_name}",
                    )
                ):
                    register_object(
                        raw_object,
                        (
                            f"{call_context}.{field_name}"
                            f"[{object_index}]"
                        ),
                    )
            ordered_calls.append(
                (
                    begin_offset,
                    _cxx_string_key(stable_site_id),
                    _cxx_string_key(expansion_source),
                    call,
                )
            )
        ordered_calls.sort(key=lambda item: item[:3])

        for call_index, (_, _, _, call) in enumerate(ordered_calls):
            stable_site_id = _field(
                call,
                "stable_site_id",
                function_context,
            )
            call_context = (
                f"{function_context}.ordered_calls[{call_index}]"
            )
            call_key = (function_usr, stable_site_id)
            if call_key not in canonicalized_calls:
                canonicalized_calls.add(call_key)
                material.append(
                    "\n"
                    + _canonical_compiler_call(
                        call,
                        call_context,
                    )
                )
            resolution = _field(call, "resolution", call_context)
            if resolution not in {"direct", "resolved_indirect"}:
                continue

            next_bridge = active_bridge
            next_transition_id = active_transition_id
            next_crossings = list(crossing_objects)
            next_captures = list(capture_objects)
            bridge = _field(call, "bridge_transition", call_context)
            live_objects = _array(
                _field(call, "live_objects", call_context),
                f"{call_context}.live_objects",
            )
            callable_captures = _array(
                _field(
                    call,
                    "callable_capture_objects",
                    call_context,
                ),
                f"{call_context}.callable_capture_objects",
            )
            if bridge != "none":
                next_bridge = bridge
                next_transition_id = stable_site_id
                if bridge == "generated_boundary_tail":
                    next_crossings = append_unique_objects(
                        next_crossings,
                        live_objects,
                        call_context,
                    )
                elif bridge == "sql_subtransaction":
                    next_crossings = [
                        _object(
                            value,
                            f"{call_context}.live_objects[{index}]",
                        )
                        for index, value in enumerate(live_objects)
                    ]
                    next_captures = []
                else:
                    next_crossings = []
                    next_captures = [
                        _object(
                            value,
                            (
                                f"{call_context}."
                                "callable_capture_objects"
                                f"[{index}]"
                            ),
                        )
                        for index, value in enumerate(
                            callable_captures
                        )
                    ]
            else:
                next_crossings = append_unique_objects(
                    next_crossings,
                    live_objects,
                    call_context,
                )

            targets = [
                _object(
                    raw_target,
                    f"{call_context}.targets[{index}]",
                )
                for index, raw_target in enumerate(
                    _array(
                        _field(call, "targets", call_context),
                        f"{call_context}.targets",
                    )
                )
            ]
            targets.sort(
                key=lambda target: (
                    _cxx_string_key(
                        _compiler_text(
                            _field(
                                target,
                                "target_usr",
                                call_context,
                            ),
                            f"{call_context}.target_usr",
                            allow_empty=True,
                        )
                    ),
                    _cxx_string_key(
                        _compiler_text(
                            _field(
                                target,
                                "target_qualified_name",
                                call_context,
                            ),
                            f"{call_context}.target_qualified_name",
                        )
                    ),
                )
            )
            for target in targets:
                target_domain = _field(
                    target,
                    "domain",
                    call_context,
                )
                allowlist = _field(
                    target,
                    "pg_no_longjmp_allowlist_evidence",
                    call_context,
                )
                if (
                    target_domain == "postgresql_server"
                    or allowlist is not None
                    or target_domain == "unknown"
                ):
                    continue
                if target_domain in {
                    "standard_library",
                    "system_library",
                    "compiler_builtin",
                } and _field(
                    target,
                    "external_mode",
                    call_context,
                ) != "analyze_body":
                    continue
                target_usr = _field(target, "target_usr", call_context)
                target_function = functions_by_usr.get(target_usr)
                if target_function is None or not _boolean(
                    _field(
                        target_function,
                        "body_available",
                        context,
                    ),
                    f"{context}.{target_usr}.body_available",
                ):
                    continue
                target_name = _field(
                    target,
                    "target_qualified_name",
                    call_context,
                )
                if _field(
                    target_function,
                    "qualified_name",
                    context,
                ) != target_name:
                    _fail(
                        call_context,
                        "resolved compiler target USR names another function",
                    )
                visit(
                    target_function,
                    next_bridge,
                    next_transition_id,
                    next_crossings,
                    next_captures,
                    depth + 1,
                )

    try:
        visit(root_function, "none", "", [], [], 0)
    except RecursionError as error:
        raise EvidenceError(
            "PostgreSQL safety evidence rejected "
            f"{context}: callback closure exhausted the bounded "
            "Python traversal stack"
        ) from error
    return "".join(material)


def _compiler_callback_closure_binding(
    facts_value: Any,
    context: str,
) -> tuple[int, str]:
    facts = _object(facts_value, context)
    _require_exact_fields(
        facts,
        COMPILER_ANALYSIS_FACT_FIELDS,
        context,
    )
    if _integer(
        _field(facts, "schema_revision", context),
        f"{context}.schema_revision",
    ) != 2:
        _fail(context, "compiler facts require schema revision 2")
    for field_name in ("profile", "compiler_profile", "clang_version"):
        _compiler_text(
            _field(facts, field_name, context),
            f"{context}.{field_name}",
        )
    for field_name in (
        "source_list_sha256",
        "compile_database_sha256",
        "producer_executable_sha256",
        "compile_graph_sha256",
        "pcm_manifest_sha256",
        "sql_manifest_sha256",
    ):
        _sha256(
            _field(facts, field_name, context),
            f"{context}.{field_name}",
        )
    _compiler_string_array(
        _field(facts, "root_usrs", context),
        f"{context}.root_usrs",
    )
    _array(
        _field(facts, "runtime_probes", context),
        f"{context}.runtime_probes",
    )

    functions_by_usr: dict[str, dict[str, Any]] = {}
    for index, raw_function in enumerate(
        _array(_field(facts, "functions", context), f"{context}.functions")
    ):
        function_context = f"{context}.functions[{index}]"
        function = _object(raw_function, function_context)
        _require_exact_fields(
            function,
            FUNCTION_FLOW_FIELDS,
            function_context,
        )
        stable_usr = _compiler_text(
            _field(function, "stable_usr", function_context),
            f"{function_context}.stable_usr",
        )
        _compiler_text(
            _field(function, "qualified_name", function_context),
            f"{function_context}.qualified_name",
        )
        _compiler_text(
            _field(function, "source_file", function_context),
            f"{function_context}.source_file",
        )
        _compiler_range_fact(
            _field(function, "declaration_span", function_context),
            f"{function_context}.declaration_span",
        )
        body_available = _boolean(
            _field(function, "body_available", function_context),
            f"{function_context}.body_available",
        )
        _compiler_range_fact(
            _field(function, "body_span", function_context),
            f"{function_context}.body_span",
            require_valid=body_available,
        )
        if stable_usr in functions_by_usr:
            _fail(context, "compiler facts duplicate a function USR")
        functions_by_usr[stable_usr] = function

    roots = [
        _object(
            raw_root,
            f"{context}.callback_roots[{index}]",
        )
        for index, raw_root in enumerate(
            _array(
                _field(facts, "callback_roots", context),
                f"{context}.callback_roots",
            )
        )
    ]
    roots.sort(
        key=lambda root: _cxx_string_key(
            _compiler_text(
                _field(root, "stable_callback_id", context),
                f"{context}.callback_root.stable_callback_id",
            )
        )
    )
    callback_ids: set[str] = set()
    material: list[str] = []
    for index, root in enumerate(roots):
        root_context = f"{context}.ordered_callback_roots[{index}]"
        (
            root_header,
            target_usr,
            target_name,
            target_source,
            target_range,
        ) = _compiler_callback_root_header(root, root_context)
        callback_id = _field(root, "stable_callback_id", root_context)
        if callback_id in callback_ids:
            _fail(context, "compiler facts duplicate a callback root")
        callback_ids.add(callback_id)
        target_function = functions_by_usr.get(target_usr)
        if target_function is None:
            _fail(root_context, "callback target function is absent")
        target_function_name = _field(
            target_function,
            "qualified_name",
            root_context,
        )
        target_function_source = _field(
            target_function,
            "source_file",
            root_context,
        )
        target_function_range = _compiler_range_fact(
            _field(
                target_function,
                "declaration_span",
                root_context,
            ),
            f"{root_context}.target_function.declaration_span",
        )
        if (
            target_function_name != target_name
            or target_function_source != target_source
            or target_function_range != target_range
        ):
            _fail(
                root_context,
                "callback target differs from its exact function definition",
            )
        material.append(root_header)
        material.append(
            _compiler_callback_closure_material(
                target_function,
                functions_by_usr,
                root_context,
            )
        )
    if not roots:
        return 0, ""
    return (
        len(roots),
        hashlib.sha256(
            "".join(material).encode("utf-8")
        ).hexdigest(),
    )


def validate_compiler_callback_closure(
    ledger: dict[str, Any],
    compiler_facts: dict[str, Any],
    profile: str,
) -> None:
    context = f"{profile}.compiler_facts"
    facts_profile = _field(compiler_facts, "profile", context)
    compiler_profile = _field(
        compiler_facts,
        "compiler_profile",
        context,
    )
    if facts_profile != profile or compiler_profile != profile:
        _fail(profile, "compiler facts carry another profile")
    for field_name in (
        "source_list_sha256",
        "compile_database_sha256",
        "producer_executable_sha256",
        "compile_graph_sha256",
        "pcm_manifest_sha256",
        "sql_manifest_sha256",
        "clang_version",
    ):
        if _field(compiler_facts, field_name, context) != _field(
            ledger,
            field_name,
            profile,
        ):
            _fail(
                profile,
                f"compiler facts {field_name} differs from the ledger",
            )
    callback_count, callback_digest = (
        _compiler_callback_closure_binding(
            compiler_facts,
            context,
        )
    )
    ledger_count = _integer(
        _field(ledger, "callback_root_count", profile),
        f"{profile}.callback_root_count",
    )
    ledger_digest = _field(
        ledger,
        "callback_closure_sha256",
        profile,
    )
    if callback_count != ledger_count or callback_digest != ledger_digest:
        _fail(
            profile,
            "callback root count or closure digest differs from "
            "sealed compiler facts",
        )


def _routine_leaf(identity: str) -> str:
    name = identity.split("(", 1)[0]
    return name.rsplit(".", 1)[-1]


def validate_ledger(
    ledger: dict[str, Any],
    provenance: dict[str, dict[str, Any]],
    provenance_sha256: str,
    policy: LedgerPolicy = LedgerPolicy(),
) -> dict[str, Any]:
    _require_exact_fields(ledger, LEDGER_FIELDS, "ledger")
    profile = _field(ledger, "profile", "ledger")
    if profile not in PROFILES:
        _fail("ledger", "profile is not production or test_indexes")
    if _integer(
        _field(ledger, "schema_revision", profile),
        f"{profile}.schema_revision",
    ) != 2:
        _fail(profile, "unsupported schema revision")
    if (
        _integer(
            _field(ledger, "postgresql_major", profile),
            f"{profile}.postgresql_major",
        )
        != 17
        or _field(ledger, "postgresql_revision", profile) != PG17_REVISION
        or _sha256(
            _field(ledger, "provenance_bundle_sha256", profile),
            f"{profile}.provenance_bundle_sha256",
        )
        != provenance_sha256
    ):
        _fail(profile, "ledger provenance is stale or not PostgreSQL 17")
    for field_name in (
        "source_list_sha256",
        "compile_database_sha256",
        "producer_executable_sha256",
        "compile_graph_sha256",
        "pcm_manifest_sha256",
        "sql_manifest_sha256",
        "compiler_facts_sha256",
    ):
        _sha256(
            _field(ledger, field_name, profile),
            f"{profile}.{field_name}",
        )
    _string(_field(ledger, "clang_version", profile), f"{profile}.clang_version")
    _string(_field(ledger, "analyzer_version", profile), f"{profile}.analyzer_version")
    _string(
        _field(ledger, "generator_revision", profile),
        f"{profile}.generator_revision",
    )
    callback_root_count = _integer(
        _field(ledger, "callback_root_count", profile),
        f"{profile}.callback_root_count",
    )
    callback_closure_sha256 = _field(
        ledger, "callback_closure_sha256", profile
    )
    if callback_root_count == 0:
        if callback_closure_sha256 != "":
            _fail(
                profile,
                "empty callback closure carries a digest",
            )
    else:
        _sha256(
            callback_closure_sha256,
            f"{profile}.callback_closure_sha256",
        )

    parents = _array(_field(ledger, "parents", profile), f"{profile}.parents")
    if len(parents) != policy.expected_parent_count:
        _fail(
            profile,
            "expected "
            f"{policy.expected_parent_count} parent rows, "
            f"found {len(parents)}",
        )
    parent_by_identity: dict[str, dict[str, Any]] = {}
    site_ids: set[str] = set()
    stop_ids: set[str] = set()
    for index, raw_parent in enumerate(parents):
        context = f"{profile}.parents[{index}]"
        parent = _object(raw_parent, context)
        identity = _string(
            _field(parent, "sql_identity", context), f"{context}.sql_identity"
        )
        if identity in parent_by_identity:
            _fail(identity, "duplicate SQL identity")
        if _field(parent, "profile", identity) != profile:
            _fail(identity, "parent profile differs from ledger profile")
        for field_name in (
            "owner_usr",
            "qualified_declaration",
            "owner_source_file",
            "generated_wrapper",
            "public_symbol",
        ):
            _string(_field(parent, field_name, identity), f"{identity}.{field_name}")
        _source_span(
            _field(parent, "owner_source_span", identity),
            f"{identity}.owner_source_span",
        )
        _sha256(
            _field(parent, "transitive_closure_sha256", identity),
            f"{identity}.transitive_closure_sha256",
        )
        if _integer(
            _field(parent, "terminal_site_count", identity),
            f"{identity}.terminal_site_count",
        ) != 0:
            _fail(identity, "terminal-severity site count is not zero")
        raw_sites = _array(_field(parent, "sites", identity), f"{identity}.sites")
        if _integer(
            _field(parent, "recoverable_site_count", identity),
            f"{identity}.recoverable_site_count",
        ) != len(raw_sites):
            _fail(identity, "recoverable site count does not match site rows")
        _validate_analysis(
            _field(parent, "analysis", identity), f"{identity}.analysis"
        )
        sites: list[dict[str, Any]] = []
        for site_index, raw_site in enumerate(raw_sites):
            site_id, site = _validate_site(
                raw_site, f"{identity}.sites[{site_index}]", provenance
            )
            if site_id in site_ids or site_id in stop_ids:
                _fail(identity, f"duplicate site_id {site_id!r}")
            site_ids.add(site_id)
            sites.append(site)
        raw_stops = _array(
            _field(parent, "audited_stops", identity),
            f"{identity}.audited_stops",
        )
        if _integer(
            _field(parent, "audited_stop_count", identity),
            f"{identity}.audited_stop_count",
        ) != len(raw_stops):
            _fail(identity, "audited stop count does not match stop rows")
        stops: list[dict[str, Any]] = []
        for stop_index, raw_stop in enumerate(raw_stops):
            stop_id, stop = _validate_audited_stop(
                raw_stop,
                f"{identity}.audited_stops[{stop_index}]",
                provenance,
            )
            if stop_id in stop_ids or stop_id in site_ids:
                _fail(identity, f"duplicate audited stop_id {stop_id!r}")
            stop_ids.add(stop_id)
            stops.append(stop)
        parent["sites"] = sites
        parent["audited_stops"] = stops
        parent_by_identity[identity] = parent

    groups = _array(
        _field(ledger, "symbol_groups", profile), f"{profile}.symbol_groups"
    )
    if len(groups) != policy.expected_symbol_count:
        _fail(
            profile,
            "expected "
            f"{policy.expected_symbol_count} symbol groups, "
            f"found {len(groups)}",
        )
    grouped_identities: set[str] = set()
    public_symbols: set[str] = set()
    observed_shared_symbols: set[str] = set()
    for index, raw_group in enumerate(groups):
        context = f"{profile}.symbol_groups[{index}]"
        group = _object(raw_group, context)
        symbol = _string(
            _field(group, "public_symbol", context),
            f"{context}.public_symbol",
        )
        if symbol in public_symbols:
            _fail(symbol, "duplicate public symbol group")
        public_symbols.add(symbol)
        if _field(group, "finfo_symbol", symbol) != f"pg_finfo_{symbol}":
            _fail(symbol, "finfo symbol does not match public symbol")
        wrapper = _string(
            _field(group, "generated_wrapper", symbol),
            f"{symbol}.generated_wrapper",
        )
        identities = _string_list(
            _field(group, "sql_identities", symbol),
            f"{symbol}.sql_identities",
            allow_empty=False,
        )
        if len(identities) > 1:
            observed_shared_symbols.add(symbol)
        for identity in identities:
            parent = parent_by_identity.get(identity)
            if (
                parent is None
                or identity in grouped_identities
                or _routine_leaf(identity) != symbol
                or parent["public_symbol"] != symbol
                or parent["generated_wrapper"] != wrapper
            ):
                _fail(symbol, "symbol sharing crosses SQL names or misses a parent")
            grouped_identities.add(identity)
    if grouped_identities != set(parent_by_identity):
        _fail(profile, "symbol groups do not cover every parent identity exactly")
    if observed_shared_symbols != set(policy.shared_symbols):
        _fail(profile, "same-name overload sharing is not exactly from_array/mmsearch")
    return {
        "profile": profile,
        "sql_manifest_sha256": ledger["sql_manifest_sha256"],
        "parents": parent_by_identity,
        "symbols": public_symbols,
        "shared_symbols": observed_shared_symbols,
    }


def validate_current_ledger_inputs(
    ledger: dict[str, Any],
    profile: str,
    *,
    source_list: Path,
    compile_database: Path,
    producer_executable: Path,
    compile_graph: Path,
    pcm_manifest: Path,
    compiler_facts: Path,
) -> None:
    artifacts = {
        "source_list_sha256": source_list,
        "compile_database_sha256": compile_database,
        "producer_executable_sha256": producer_executable,
        "compile_graph_sha256": compile_graph,
        "pcm_manifest_sha256": pcm_manifest,
    }
    for field_name, path in artifacts.items():
        declared = _sha256(
            _field(ledger, field_name, profile),
            f"{profile}.{field_name}",
        )
        actual = _path_artifact_sha256(
            path,
            f"{profile}.{field_name}",
        )
        if declared != actual:
            _fail(
                profile,
                f"{field_name} differs from the current "
                f"artifact bytes at {path}",
            )
    _, compiler_facts_raw, _ = _authenticated_file_bytes(
        compiler_facts,
        f"{profile}.compiler_facts_sha256",
        require_single_link=False,
    )
    declared_compiler_facts_sha256 = _sha256(
        _field(ledger, "compiler_facts_sha256", profile),
        f"{profile}.compiler_facts_sha256",
    )
    if (
        declared_compiler_facts_sha256
        != _artifact_sha256(compiler_facts_raw)
    ):
        _fail(
            profile,
            "compiler_facts_sha256 differs from the current "
            f"artifact bytes at {compiler_facts}",
        )
    try:
        compiler_facts_json = _object(
            json.loads(
                compiler_facts_raw,
                object_pairs_hook=_reject_duplicate_json_keys,
            ),
            str(compiler_facts),
        )
    except json.JSONDecodeError as error:
        raise EvidenceError(
            f"cannot read {compiler_facts}: {error}"
        ) from error
    validate_compiler_callback_closure(
        ledger,
        compiler_facts_json,
        profile,
    )


def validate_profile_pair(
    production: dict[str, Any], test_indexes: dict[str, Any]
) -> None:
    if (
        production["profile"] != "production"
        or test_indexes["profile"] != "test_indexes"
    ):
        _fail("profile-pair", "both compilation profiles are required")
    production_closure = {
        identity: parent["public_symbol"]
        for identity, parent in production["parents"].items()
    }
    test_closure = {
        identity: parent["public_symbol"]
        for identity, parent in test_indexes["parents"].items()
    }
    if production_closure != test_closure:
        _fail("profile-pair", "profile identity or public-symbol closure differs")


def validate_ledger_manifest_closure(
    manifest: dict[str, Any],
    production: dict[str, Any],
    test_indexes: dict[str, Any],
    *,
    policy: LedgerPolicy = LedgerPolicy(),
    require_complete_codegen: bool = True,
) -> dict[str, Any]:
    """Join both compiler-profile ledgers to the generated SQL owner manifest."""

    _require_current_generated_sql_manifest(
        manifest, "ledger-manifest"
    )

    identities = _string_list(
        _field(manifest, "c_routine_identities", "ledger-manifest"),
        "ledger-manifest.c_routine_identities",
        allow_empty=False,
    )
    if (
        _integer(
            _field(
                manifest,
                "c_routine_identity_count",
                "ledger-manifest",
            ),
            "ledger-manifest.c_routine_identity_count",
        )
        != policy.expected_parent_count
        or len(identities) != policy.expected_parent_count
    ):
        _fail(
            "ledger-manifest",
            "generated manifest does not contain the exact parent inventory",
        )

    symbols = _string_list(
        _field(manifest, "c_link_symbols", "ledger-manifest"),
        "ledger-manifest.c_link_symbols",
        allow_empty=False,
    )
    if (
        _integer(
            _field(
                manifest,
                "c_link_symbol_inventory_count",
                "ledger-manifest",
            ),
            "ledger-manifest.c_link_symbol_inventory_count",
        )
        != policy.expected_symbol_count
        or len(symbols) != policy.expected_symbol_count
    ):
        _fail(
            "ledger-manifest",
            "generated manifest does not contain the exact symbol inventory",
        )

    expected_finfo = {f"pg_finfo_{symbol}" for symbol in symbols}
    finfo_symbols = set(
        _string_list(
            _field(manifest, "c_finfo_symbols", "ledger-manifest"),
            "ledger-manifest.c_finfo_symbols",
            allow_empty=False,
        )
    )
    if (
        _integer(
            _field(
                manifest,
                "c_finfo_symbol_inventory_count",
                "ledger-manifest",
            ),
            "ledger-manifest.c_finfo_symbol_inventory_count",
        )
        != policy.expected_symbol_count
        or finfo_symbols != expected_finfo
    ):
        _fail("ledger-manifest", "generated finfo inventory is stale")

    owners = _array(
        _field(manifest, "c_symbol_owners", "ledger-manifest"),
        "ledger-manifest.c_symbol_owners",
    )
    if len(owners) != policy.expected_symbol_count:
        _fail("ledger-manifest", "generated symbol-owner inventory is incomplete")

    identity_to_symbol: dict[str, str] = {}
    owner_symbols: set[str] = set()
    generated_symbols: set[str] = set()
    observed_shared_symbols: set[str] = set()
    for index, raw_owner in enumerate(owners):
        context = f"ledger-manifest.c_symbol_owners[{index}]"
        owner = _object(raw_owner, context)
        symbol = _string(_field(owner, "symbol", context), f"{context}.symbol")
        if symbol not in symbols or symbol in owner_symbols:
            _fail(context, "symbol owner is missing, extra, or duplicated")
        owner_symbols.add(symbol)
        if _field(owner, "finfo_symbol", context) != f"pg_finfo_{symbol}":
            _fail(context, "finfo owner differs from its public symbol")
        if _boolean(
            _field(owner, "generated_wrapper", context),
            f"{context}.generated_wrapper",
        ):
            generated_symbols.add(symbol)
        owner_identities = _string_list(
            _field(owner, "routine_identities", context),
            f"{context}.routine_identities",
            allow_empty=False,
        )
        _string_list(
            _field(owner, "implementation_declarations", context),
            f"{context}.implementation_declarations",
            allow_empty=False,
        )
        if len(owner_identities) > 1:
            observed_shared_symbols.add(symbol)
        for identity in owner_identities:
            if (
                identity not in identities
                or identity in identity_to_symbol
                or _routine_leaf(identity) != symbol
            ):
                _fail(
                    context,
                    "owner identities are missing, duplicated, or cross-name",
                )
            identity_to_symbol[identity] = symbol

    if (
        owner_symbols != set(symbols)
        or set(identity_to_symbol) != set(identities)
        or observed_shared_symbols != set(policy.shared_symbols)
    ):
        _fail(
            "ledger-manifest",
            "generated identities do not close over the reviewed owner groups",
        )
    if _integer(
        _field(
            manifest,
            "c_symbol_sharing_group_count",
            "ledger-manifest",
        ),
        "ledger-manifest.c_symbol_sharing_group_count",
    ) != len(policy.shared_symbols):
        _fail("ledger-manifest", "generated sharing-group count is stale")

    declared_generated_symbols = set(
        _string_list(
            _field(
                manifest,
                "generated_wrapper_symbols",
                "ledger-manifest",
            ),
            "ledger-manifest.generated_wrapper_symbols",
        )
    )
    if (
        declared_generated_symbols != generated_symbols
        or _integer(
            _field(
                manifest,
                "generated_wrapper_symbol_count",
                "ledger-manifest",
            ),
            "ledger-manifest.generated_wrapper_symbol_count",
        )
        != len(generated_symbols)
    ):
        _fail(
            "ledger-manifest",
            "generated-wrapper inventory differs from symbol owners",
        )
    if _integer(
        _field(manifest, "c_wrapper_entity_count", "ledger-manifest"),
        "ledger-manifest.c_wrapper_entity_count",
    ) != len(generated_symbols):
        _fail(
            "ledger-manifest",
            "generated wrapper entity count differs from symbol owners",
        )
    handwritten_fmgr_export_symbols = set(
        _string_list(
            _field(
                manifest,
                "handwritten_fmgr_export_symbols",
                "ledger-manifest",
            ),
            "ledger-manifest.handwritten_fmgr_export_symbols",
        )
    )
    if (
        _integer(
            _field(
                manifest,
                "handwritten_fmgr_export_symbol_count",
                "ledger-manifest",
            ),
            "ledger-manifest.handwritten_fmgr_export_symbol_count",
        )
        != len(handwritten_fmgr_export_symbols)
        or generated_symbols & handwritten_fmgr_export_symbols
        or generated_symbols | handwritten_fmgr_export_symbols
        != set(symbols)
    ):
        _fail(
            "ledger-manifest",
            "generated and handwritten ownership partitions are "
            "inconsistent",
        )
    if require_complete_codegen and generated_symbols != set(symbols):
        _fail(
            "ledger-manifest",
            "final evidence contains a non-generated fmgr owner",
        )

    for ledger in (production, test_indexes):
        ledger_identity_to_symbol = {
            identity: parent["public_symbol"]
            for identity, parent in ledger["parents"].items()
        }
        if ledger_identity_to_symbol != identity_to_symbol:
            _fail(
                f"{ledger['profile']}-manifest",
                "ledger parents differ from generated SQL ownership",
            )
        if ledger["symbols"] != set(symbols):
            _fail(
                f"{ledger['profile']}-manifest",
                "ledger symbol groups differ from generated SQL ownership",
            )

    return {
        "manifest_parent_count": len(identities),
        "manifest_symbol_count": len(symbols),
        "manifest_generated_wrapper_count": len(generated_symbols),
    }


def _coverage_key(value: Any, context: str) -> tuple[str, str, str]:
    key = _object(value, context)
    _require_exact_fields(key, COVERAGE_KEY_FIELDS, context)
    profile = _string(
        _field(key, "profile", context),
        f"{context}.profile",
    )
    dimension = _string(
        _field(key, "dimension", context),
        f"{context}.dimension",
    )
    if profile not in PROFILES:
        _fail(context, "unknown coverage profile")
    if dimension not in COVERAGE_DIMENSIONS:
        _fail(context, "unknown coverage dimension")
    item = _string(_field(key, "value", context), f"{context}.value")
    return profile, dimension, item


def validate_runtime_policy(policy: dict[str, Any]) -> set[tuple[str, str, str]]:
    _require_exact_fields(
        policy,
        RUNTIME_POLICY_FIELDS,
        "runtime-policy",
    )
    if _integer(
        _field(policy, "schema_revision", "runtime-policy"),
        "runtime-policy.schema_revision",
    ) != 2:
        _fail("runtime-policy", "unsupported schema revision")
    result: set[tuple[str, str, str]] = set()
    for index, raw_key in enumerate(
        _array(
            _field(policy, "required_coverage", "runtime-policy"),
            "runtime-policy.required_coverage",
        )
    ):
        key = _coverage_key(raw_key, f"runtime-policy.required_coverage[{index}]")
        if key in result:
            _fail("runtime-policy", "duplicate required coverage key")
        result.add(key)
    mandatory = {
        (profile, dimension, value)
        for profile in PROFILES
        for dimension, values in MANDATORY_RUNTIME_POLICY_VALUES.items()
        for value in values
    }
    if not mandatory <= result:
        missing = sorted(mandatory - result)
        _fail(
            "runtime-policy",
            "missing mandatory two-profile coverage keys: "
            + ", ".join(
                f"{profile}/{dimension}/{value}"
                for profile, dimension, value in missing
            ),
        )
    return result


def derive_site_coverage(
    profile: str,
    site: dict[str, Any],
) -> set[tuple[str, str, str]]:
    result: set[tuple[str, str, str]] = set()
    result.add((profile, "classification", site["classification"]))
    if site["bridge"] != "none":
        result.add((profile, "bridge", site["bridge"]))
    for field_name, dimension in (
        ("converter_families", "converter_family"),
        ("owner_families", "owner_family"),
        ("cleanup_phases", "cleanup_phase"),
    ):
        result.update(
            (profile, dimension, item) for item in site[field_name]
        )
    return result


def derive_ledger_coverage(
    *ledgers: dict[str, Any],
) -> set[tuple[str, str, str]]:
    result: set[tuple[str, str, str]] = set()
    for ledger in ledgers:
        profile = ledger["profile"]
        for parent in ledger["parents"].values():
            for site in parent["sites"]:
                result.update(derive_site_coverage(profile, site))
    return result


def _canonical_json_bytes(value: Any) -> bytes:
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        + "\n"
    ).encode("utf-8")


def _guc_owner_key(owner: str, context: str) -> str:
    if owner in GUC_SOURCE_MODULES:
        return owner
    _fail(context, f"unsupported GUC owner {owner!r}")


def _validate_generated_live_authority(
    authority: Mapping[str, Any],
    value: Any,
    context: str,
) -> dict[str, Any]:
    live = _object(value, context)
    _require_exact_fields(live, GUC_GENERATED_LIVE_FIELDS, context)
    kind = _string(_field(authority, "kind", context), f"{context}.kind")
    source_context = _string(
        _field(authority, "context", context),
        f"{context}.source_context",
    )
    expected_context = {
        "sighup": "sighup",
        "superuser_set": "superuser",
        "user_set": "user",
    }.get(source_context)
    unit = _field(authority, "unit", context)
    expected_unit = "MB" if unit == "mb" else unit
    boot = _field(live, "boot_value", context)
    short_description = _field(live, "short_description", context)
    extra_description = _field(live, "extra_description", context)
    if (
        _field(live, "vartype", context) != kind
        or _field(live, "context", context) != expected_context
        or _field(live, "unit", context) != expected_unit
        or not isinstance(boot, str)
        or _field(live, "reset_value", context) != boot
        or _field(live, "current_value", context) != boot
        or not isinstance(short_description, str)
        or not short_description
        or (
            extra_description is not None
            and not isinstance(extra_description, str)
        )
    ):
        _fail(context, "generated live scalar authority differs")
    minimum = _field(live, "minimum", context)
    maximum = _field(live, "maximum", context)
    bounded = kind in {"integer", "real"}
    if (
        bounded
        and (
            not isinstance(minimum, str)
            or not isinstance(maximum, str)
        )
    ) or (
        not bounded and (minimum is not None or maximum is not None)
    ):
        _fail(context, "generated live bound authority differs")
    enum_values = _array(
        _field(live, "visible_enum_values", context),
        f"{context}.visible_enum_values",
    )
    if (
        any(not isinstance(item, str) or not item for item in enum_values)
        or len(set(enum_values)) != len(enum_values)
        or (kind == "enum" and (not enum_values or boot not in enum_values))
        or (kind != "enum" and enum_values)
        or (kind == "bool" and boot not in {"on", "off"})
    ):
        _fail(context, "generated live enum/default authority differs")
    flags = _string_list(
        _field(authority, "flags", context),
        f"{context}.authority_flags",
        allow_empty=True,
    )
    raw_flags = 0
    if "no_show_all" in flags:
        raw_flags |= 0x000004
    if "not_in_sample" in flags:
        raw_flags |= 0x000080
    unit_flag = {
        None: 0,
        "kb": 0x01000000,
        "blocks": 0x02000000,
        "mb": 0x04000000,
        "ms": 0x10000000,
        "s": 0x20000000,
        "min": 0x30000000,
    }.get(unit)
    if unit_flag is None:
        _fail(context, "generated live unit flag is unsupported")
    raw_flags |= unit_flag
    if (
        _integer(
            _field(live, "raw_flags", context),
            f"{context}.raw_flags",
        )
        != raw_flags
        or _boolean(
            _field(live, "visible_in_show_all", context),
            f"{context}.visible_in_show_all",
        )
        != ("no_show_all" not in flags)
        or _boolean(
            _field(live, "included_in_sample", context),
            f"{context}.included_in_sample",
        )
        != ("not_in_sample" not in flags)
    ):
        _fail(context, "generated live flag behavior differs")
    authority_hooks = _array(
        _field(authority, "hooks", context),
        f"{context}.authority_hooks",
    )
    live_hooks = _object(
        _field(live, "hooks", context), f"{context}.hooks"
    )
    _require_exact_fields(
        live_hooks,
        frozenset({"check", "assign", "show"}),
        f"{context}.hooks",
    )
    for index, name in enumerate(("check", "assign", "show")):
        hook_context = f"{context}.hooks.{name}"
        hook = _object(_field(live_hooks, name, context), hook_context)
        _require_exact_fields(
            hook, GUC_GENERATED_HOOK_FIELDS, hook_context
        )
        symbol = authority_hooks[index]
        if (
            _field(hook, "symbol", hook_context) != symbol
            or _field(hook, "present", hook_context)
            is not (symbol is not None)
            or _field(hook, "behavior", hook_context)
            != (f"custom_{name}" if symbol is not None else "none")
        ):
            _fail(hook_context, "generated hook behavior differs")
    return live


def _validate_guc_manifest_contract(
    manifest: Mapping[str, Any],
    *,
    profile: str,
    context: str,
) -> dict[str, Any]:
    generation_context = f"{context}.guc_generation"
    generation = _object(
        _field(manifest, "guc_generation", context),
        generation_context,
    )
    _require_exact_fields(
        generation,
        GUC_GENERATION_FIELDS,
        generation_context,
    )
    expected_total = GUC_PROFILE_TOTALS.get(profile, -1)
    expected_test_only = 0 if profile == "production" else 3
    if (
        profile not in PROFILES
        or _field(generation, "profile", generation_context) != profile
        or _field(
            generation, "description_authority", generation_context
        )
        != "cleaned_doxygen_only"
        or _integer(
            _field(generation, "registration_count", generation_context),
            f"{generation_context}.registration_count",
        )
        != expected_total
        or _field(generation, "registry_module", generation_context)
        != f"bioseq.postgres.sql.generated.{profile}.guc_registry"
    ):
        _fail(generation_context, "GUC profile authority differs")
    registry_sha256 = _sha256(
        _field(
            generation, "registry_content_sha256", generation_context
        ),
        f"{generation_context}.registry_content_sha256",
    )
    owner_order = GUC_OWNER_ORDER
    base_counts = GUC_PRODUCTION_OWNER_COUNTS
    facets: dict[str, dict[str, Any]] = {}
    owner_facets = _array(
        _field(generation, "owner_facets", generation_context),
        f"{generation_context}.owner_facets",
    )
    if len(owner_facets) != len(owner_order):
        _fail(generation_context, "owner-facet count differs")
    for index, raw_facet in enumerate(owner_facets):
        facet_context = f"{generation_context}.owner_facets[{index}]"
        facet = _object(raw_facet, facet_context)
        _require_exact_fields(
            facet,
            GUC_OWNER_FACET_FIELDS,
            facet_context,
        )
        owner = _string(
            _field(facet, "owner", facet_context),
            f"{facet_context}.owner",
        )
        expected_source = GUC_SOURCE_MODULES[owner]
        if (
            owner != owner_order[index]
            or _field(facet, "source_module", facet_context)
            != expected_source
            or _field(facet, "generated_module", facet_context)
            != f"bioseq.postgres.sql.generated.{profile}.{owner}.gucs"
            or _integer(
                _field(facet, "registration_count", facet_context),
                f"{facet_context}.registration_count",
            )
            != base_counts[owner]
            + (expected_test_only if owner == "bioseq" else 0)
        ):
            _fail(facet_context, "owner-facet authority differs")
        _sha256(
            _field(facet, "content_sha256", facet_context),
            f"{facet_context}.content_sha256",
        )
        facets[owner] = facet

    delta_context = f"{generation_context}.profile_delta_validation"
    delta = _object(
        _field(generation, "profile_delta_validation", generation_context),
        delta_context,
    )
    _require_exact_fields(
        delta,
        frozenset(
            {
                "production_registry_content_sha256",
                "test_indexes_registry_content_sha256",
                "added_in_test_indexes",
                "removed_from_test_indexes",
                "changed_in_test_indexes",
                "owner_descriptor_profile_invariant",
                "owner_artifacts",
            }
        ),
        delta_context,
    )
    if (
        _field(delta, "added_in_test_indexes", delta_context)
        != list(GUC_TEST_ONLY_NAMES)
        or _field(delta, "removed_from_test_indexes", delta_context) != []
        or _field(delta, "changed_in_test_indexes", delta_context) != []
    ):
        _fail(delta_context, "sole profile delta differs")
    production_registry_sha256 = _sha256(
        _field(
            delta,
            "production_registry_content_sha256",
            delta_context,
        ),
        delta_context,
    )
    test_registry_sha256 = _sha256(
        _field(
            delta,
            "test_indexes_registry_content_sha256",
            delta_context,
        ),
        delta_context,
    )
    if registry_sha256 != (
        production_registry_sha256
        if profile == "production"
        else test_registry_sha256
    ):
        _fail(delta_context, "registry hash is outside the profile proof")
    invariants = _object(
        _field(
            delta,
            "owner_descriptor_profile_invariant",
            delta_context,
        ),
        f"{delta_context}.owner_descriptor_profile_invariant",
    )
    artifacts = _object(
        _field(delta, "owner_artifacts", delta_context),
        f"{delta_context}.owner_artifacts",
    )
    _require_exact_fields(invariants, frozenset(owner_order), delta_context)
    _require_exact_fields(artifacts, frozenset(owner_order), delta_context)
    for owner in owner_order:
        if _boolean(
            _field(invariants, owner, delta_context),
            f"{delta_context}.owner_invariant.{owner}",
        ) != (owner != "bioseq"):
            _fail(delta_context, "owner invariance proof differs")
        artifact_context = f"{delta_context}.owner_artifacts.{owner}"
        artifact = _object(
            _field(artifacts, owner, delta_context),
            artifact_context,
        )
        _require_exact_fields(
            artifact,
            frozenset(
                {
                    "production_content_sha256",
                    "test_indexes_content_sha256",
                }
            ),
            artifact_context,
        )
        production_owner_sha256 = _sha256(
            _field(
                artifact,
                "production_content_sha256",
                artifact_context,
            ),
            artifact_context,
        )
        test_owner_sha256 = _sha256(
            _field(
                artifact,
                "test_indexes_content_sha256",
                artifact_context,
            ),
            artifact_context,
        )
        if _field(
            facets[owner], "content_sha256", artifact_context
        ) != (
            production_owner_sha256
            if profile == "production"
            else test_owner_sha256
        ):
            _fail(artifact_context, "owner facet is outside the proof")

    registrations = _array(
        _field(generation, "registrations", generation_context),
        f"{generation_context}.registrations",
    )
    if len(registrations) != expected_total:
        _fail(generation_context, "registration array size differs")
    names: set[str] = set()
    owner_counts = {owner: 0 for owner in owner_order}
    previous_ordinal = -1
    for index, raw_registration in enumerate(registrations):
        registration_context = (
            f"{generation_context}.registrations[{index}]"
        )
        registration = _object(raw_registration, registration_context)
        _require_exact_fields(
            registration,
            GUC_REGISTRATION_FIELDS,
            registration_context,
        )
        generated_live_authority = _validate_generated_live_authority(
            registration,
            _field(
                registration,
                "generated_live_authority",
                registration_context,
            ),
            f"{registration_context}.generated_live_authority",
        )
        ordinal = _integer(
            _field(
                registration,
                "registration_ordinal",
                registration_context,
            ),
            f"{registration_context}.registration_ordinal",
        )
        name = _string(
            _field(registration, "name", registration_context),
            f"{registration_context}.name",
        )
        member = _string(
            _field(registration, "member", registration_context),
            f"{registration_context}.member",
        )
        owner = _string(
            _field(registration, "owner", registration_context),
            f"{registration_context}.owner",
        )
        owner_key = _guc_owner_key(owner, registration_context)
        kind = _string(
            _field(registration, "kind", registration_context),
            f"{registration_context}.kind",
        )
        guc_context = _string(
            _field(registration, "context", registration_context),
            f"{registration_context}.context",
        )
        name_prefix = (
            "bioseq." if owner == "bioseq" else f"bioseq.{owner}."
        )
        expected_member = (
            name.removeprefix(name_prefix).replace(".", "_")
            if name.startswith(name_prefix)
            else ""
        )
        if (
            ordinal <= previous_ordinal
            or member != expected_member
            or name in names
            or kind not in GUC_KIND_STORAGE_KINDS
            or guc_context
            not in {"sighup", "superuser_set", "user_set"}
            or _field(registration, "storage_kind", registration_context)
            != GUC_KIND_STORAGE_KINDS[kind]
            or _field(registration, "source_module", registration_context)
            != _field(
                facets[owner_key],
                "source_module",
                registration_context,
            )
            or _field(registration, "source_module", registration_context)
            != GUC_SOURCE_MODULES[owner_key]
            or _field(registration, "source_path", registration_context)
            != GUC_SOURCE_PATHS[owner_key]
        ):
            _fail(registration_context, "identity/order/ownership differs")
        _string(
            _field(registration, "cpp_storage", registration_context),
            f"{registration_context}.cpp_storage",
        )
        _string(
            _field(registration, "default_expression", registration_context),
            f"{registration_context}.default_expression",
        )
        previous_ordinal = ordinal
        names.add(name)
        owner_counts[owner_key] += 1
        _sha256(
            _field(registration, "source_sha256", registration_context),
            f"{registration_context}.source_sha256",
        )
        test_only = name in GUC_TEST_ONLY_NAMES
        if (
            _field(
                registration,
                "feature_predicate",
                registration_context,
            )
            != (
                "defined(BIOSEQ_TEST_INDEXES)" if test_only else "true"
            )
            or (test_only and profile != "test_indexes")
        ):
            _fail(registration_context, "profile authority differs")
        flags = _string_list(
            _field(registration, "flags", registration_context),
            f"{registration_context}.flags",
            allow_empty=True,
        )
        if any(flag not in {"no_show_all", "not_in_sample"} for flag in flags):
            _fail(registration_context, "unsupported flag")
        hooks = _array(
            _field(registration, "hooks", registration_context),
            f"{registration_context}.hooks",
        )
        if len(hooks) != 3 or any(
            hook is not None and (not isinstance(hook, str) or not hook)
            for hook in hooks
        ):
            _fail(registration_context, "hook tuple differs")
        minimum = _field(
            registration, "minimum_expression", registration_context
        )
        maximum = _field(
            registration, "maximum_expression", registration_context
        )
        bounded = kind in {"integer", "real"}
        if (
            bounded
            and (
                not isinstance(minimum, str)
                or not minimum
                or not isinstance(maximum, str)
                or not maximum
            )
        ) or (not bounded and (minimum is not None or maximum is not None)):
            _fail(registration_context, "bound authority differs")
        unit = _field(registration, "unit", registration_context)
        if unit is not None and (unit != "mb" or kind != "integer"):
            _fail(registration_context, "unit authority differs")
        enum_rows = _array(
            _field(
                registration,
                "enum_rows_name_wire_condition_alias_hidden_compiled",
                registration_context,
            ),
            f"{registration_context}.enum_rows",
        )
        enum_names: set[str] = set()
        visible_enum_names: list[str] = []
        for row_index, raw_row in enumerate(enum_rows):
            row_context = f"{registration_context}.enum_rows[{row_index}]"
            row = _array(raw_row, row_context)
            if (
                len(row) != 6
                or any(
                    not isinstance(row[field], str) or not row[field]
                    for field in (0, 1, 2)
                )
                or (
                    row[3] is not None
                    and (not isinstance(row[3], str) or not row[3])
                )
                or type(row[4]) is not bool
                or type(row[5]) is not bool
                or row[0] in enum_names
            ):
                _fail(row_context, "enum row is malformed")
            enum_names.add(row[0])
            if row[5] and not row[4]:
                visible_enum_names.append(row[0])
        enum_default = _field(
            registration, "enum_default", registration_context
        )
        if (
            (kind == "enum" and (enum_default is None or not enum_rows))
            or (kind != "enum" and (enum_default is not None or enum_rows))
        ):
            _fail(registration_context, "enum authority differs by kind")
        if kind == "enum" and (
            enum_default not in visible_enum_names
            or _field(
                generated_live_authority,
                "visible_enum_values",
                registration_context,
            )
            != visible_enum_names
        ):
            _fail(registration_context, "visible enum authority differs")
    for owner, count in base_counts.items():
        expected_count = count + (
            expected_test_only if owner == "bioseq" else 0
        )
        if owner_counts[owner] != expected_count:
            _fail(generation_context, "owner registration count differs")
    if (
        set(GUC_TEST_ONLY_NAMES).issubset(names)
    ) != (profile == "test_indexes"):
        _fail(generation_context, "test-profile GUC delta differs")
    return generation


def _validate_guc_property_receipt(
    value: Any,
    sha256_value: Any,
    *,
    generation: Mapping[str, Any],
    profile: str,
    context: str,
) -> tuple[list[dict[str, Any]], str]:
    properties = _array(value, context)
    digest = _sha256(sha256_value, f"{context}.sha256")
    registrations = _array(
        _field(generation, "registrations", context),
        f"{context}.manifest_registrations",
    )
    if (
        len(properties) != len(registrations)
        or _artifact_sha256(_canonical_json_bytes(properties)) != digest
    ):
        _fail(context, "GUC properties are malformed or unsealed")
    facets = {
        _field(facet, "owner", context): facet
        for facet in _array(
            _field(generation, "owner_facets", context),
            f"{context}.owner_facets",
        )
    }
    delta = _field(generation, "profile_delta_validation", context)
    projection_base = {
        "profile": profile,
        "registry_module": _field(
            generation, "registry_module", context
        ),
        "registry_content_sha256": _field(
            generation, "registry_content_sha256", context
        ),
        "profile_delta_validation_sha256": _artifact_sha256(
            _canonical_json_bytes(delta)
        ),
    }
    result: list[dict[str, Any]] = []
    for index, raw_property in enumerate(properties):
        property_context = f"{context}[{index}]"
        property_row = _object(raw_property, property_context)
        _require_exact_fields(
            property_row,
            GUC_PROPERTY_FIELDS,
            property_context,
        )
        registration = _object(
            registrations[index],
            f"{property_context}.registration",
        )
        generated_live_authority = _object(
            _field(
                registration,
                "generated_live_authority",
                property_context,
            ),
            f"{property_context}.generated_live_authority",
        )
        name = _field(registration, "name", property_context)
        owner_key = _guc_owner_key(
            _field(registration, "owner", property_context),
            property_context,
        )
        projection = _object(
            _field(property_row, "profile_projection", property_context),
            f"{property_context}.profile_projection",
        )
        _require_exact_fields(
            projection,
            GUC_PROFILE_PROJECTION_FIELDS,
            f"{property_context}.profile_projection",
        )
        expected_projection = {
            **projection_base,
            "owner_facet": facets[owner_key],
        }
        if (
            _field(
                property_row,
                "registration_ordinal",
                property_context,
            )
            != _field(
                registration,
                "registration_ordinal",
                property_context,
            )
            or _field(property_row, "name", property_context) != name
            or _field(property_row, "registration", property_context)
            != registration
            or _sha256(
                _field(
                    property_row,
                    "manifest_registration_sha256",
                    property_context,
                ),
                property_context,
            )
            != _artifact_sha256(_canonical_json_bytes(registration))
            or projection != expected_projection
        ):
            _fail(
                property_context,
                "GUC property does not close over the manifest row",
            )
        settings_context = f"{property_context}.pg_settings"
        settings = _object(
            _field(property_row, "pg_settings", property_context),
            settings_context,
        )
        _require_exact_fields(
            settings,
            GUC_PG_SETTINGS_FIELDS,
            settings_context,
        )
        if settings != generated_live_authority:
            _fail(
                settings_context,
                "live GUC projection differs from generated authority",
            )
        result.append(property_row)
    return result, digest


def _validate_guc_profile_pair(
    production_properties: list[dict[str, Any]],
    test_indexes_properties: list[dict[str, Any]],
    *,
    context: str,
) -> None:
    production_rows = {
        _field(row, "name", context): row
        for row in production_properties
    }
    test_indexes_rows = {
        _field(row, "name", context): row
        for row in test_indexes_properties
    }
    if (
        len(production_rows) != GUC_PROFILE_TOTALS["production"]
        or len(test_indexes_rows) != GUC_PROFILE_TOTALS["test_indexes"]
        or set(production_rows) - set(test_indexes_rows)
        or set(test_indexes_rows) - set(production_rows)
        != set(GUC_TEST_ONLY_NAMES)
    ):
        _fail(
            context,
            "GUC receipts do not prove the exact 73/76 profile delta",
        )

    def semantic_projection(row: Mapping[str, Any]) -> dict[str, Any]:
        result = {
            field_name: value
            for field_name, value in row.items()
            if field_name
            not in {
                "registration_ordinal",
                "manifest_registration_sha256",
                "profile_projection",
            }
        }
        registration = dict(
            _object(result["registration"], "GUC semantic registration")
        )
        registration.pop("registration_ordinal", None)
        result["registration"] = registration
        return result

    for name, production_row in production_rows.items():
        production_semantic = semantic_projection(production_row)
        test_semantic = semantic_projection(test_indexes_rows[name])
        if production_semantic != test_semantic:
            _fail(
                context,
                f"common GUC property differs across profiles: {name}",
            )


def _probe_site_id(sql_identity: str, fault_key: str) -> str:
    preimage = (
        "bioseq.safety_runtime_probe.site\nrevision=2\n"
        + sql_identity
        + "\n"
        + fault_key
        + "\n"
    ).encode("utf-8")
    return "probe:" + _artifact_sha256(preimage)


def _expected_probe_contract(relation: str) -> dict[str, Any]:
    values = RUNTIME_RELATION_CONTRACTS.get(relation)
    if values is None:
        _fail("runtime-relation", f"unknown relation {relation!r}")
    visit_count, trigger_count, ordering = values
    return {
        "abi_revision": 2,
        "expected_visit_count": visit_count,
        "expected_trigger_count": trigger_count,
        "ordering": ordering,
    }


def _expected_runtime_lifecycle(
    fault_key: str,
) -> dict[str, int]:
    if fault_key == BACKEND_SCOPED_VACUUM_CLEANUP_FAULT_KEY:
        return {
            "fault_invocation_count": 0,
            "fmgr_start_count": 0,
            "fmgr_end_count": 0,
            "fmgr_abort_count": 0,
        }
    return {
        "fault_invocation_count": 1,
        "fmgr_start_count": 1,
        "fmgr_end_count": 0,
        "fmgr_abort_count": 1,
    }


def _validate_probe_contract(
    value: Any,
    relation: str,
    context: str,
) -> dict[str, Any]:
    contract = _object(value, context)
    _require_exact_fields(contract, PROBE_CONTRACT_FIELDS, context)
    normalized = {
        "abi_revision": _integer(
            _field(contract, "abi_revision", context),
            f"{context}.abi_revision",
        ),
        "expected_visit_count": _integer(
            _field(contract, "expected_visit_count", context),
            f"{context}.expected_visit_count",
        ),
        "expected_trigger_count": _integer(
            _field(contract, "expected_trigger_count", context),
            f"{context}.expected_trigger_count",
        ),
        "ordering": _string(
            _field(contract, "ordering", context),
            f"{context}.ordering",
        ),
    }
    expected = _expected_probe_contract(relation)
    if normalized != expected:
        _fail(context, "differs from the relation-derived ABI revision 2 contract")
    return normalized


def _runtime_row_matrix(value: Any, context: str) -> list[list[str | None]]:
    rows = _array(value, context)
    if not rows or len(rows) > 64:
        _fail(context, "expected 1..64 recovery rows")
    result: list[list[str | None]] = []
    expected_columns: int | None = None
    for row_index, raw_row in enumerate(rows):
        row = _array(raw_row, f"{context}[{row_index}]")
        if (
            not row
            or len(row) > 64
            or (expected_columns is not None and len(row) != expected_columns)
        ):
            _fail(context, "recovery rows must be a bounded rectangle")
        expected_columns = len(row)
        normalized_row: list[str | None] = []
        for column_index, cell in enumerate(row):
            if cell is not None and not isinstance(cell, str):
                _fail(
                    f"{context}[{row_index}][{column_index}]",
                    "recovery cells must be strings or null",
                )
            normalized_row.append(cell)
        result.append(normalized_row)
    return result


def _runtime_sql_array(value: Any, context: str) -> list[str]:
    statements = _array(value, context)
    if len(statements) > 64:
        _fail(context, "contains more than 64 SQL statements")
    result = [
        _string(statement, f"{context}[{index}]")
        for index, statement in enumerate(statements)
    ]
    if any(
        len(statement.encode("utf-8")) > 1024 * 1024
        for statement in result
    ):
        _fail(context, "SQL statement exceeds 1 MiB")
    return result


def _parse_case_plan(
    path: Path,
    *,
    profile: str,
    expected_sha256: str,
    ledger_sha256: str,
    runtime_inventory_sha256: str,
    sql_manifest_sha256: str,
    runtime_policy_sha256: str,
    inventory: dict[str, Any],
    expected_case_count: int = EXPECTED_PROFILE_CASE_COUNT,
) -> dict[str, dict[str, Any]]:
    context = f"{profile}-case-plan"
    canonical_path = _canonical_regular_file(path, context)
    plan, raw = load_json(canonical_path)
    if _artifact_sha256(raw) != expected_sha256:
        _fail(context, "case-plan bytes are stale")
    _require_exact_fields(plan, CASE_PLAN_FIELDS, context)
    if (
        _integer(
            _field(plan, "schema_revision", context),
            f"{context}.schema_revision",
        )
        != 2
        or _integer(
            _field(plan, "probe_abi_revision", context),
            f"{context}.probe_abi_revision",
        )
        != 2
        or _string(
            _field(plan, "profile", context),
            f"{context}.profile",
        )
        != profile
    ):
        _fail(context, "schema, ABI, or profile differs")
    expected_hashes = {
        "ledger_sha256": ledger_sha256,
        "runtime_inventory_sha256": runtime_inventory_sha256,
        "sql_manifest_sha256": sql_manifest_sha256,
        "runtime_policy_sha256": runtime_policy_sha256,
    }
    for field_name, expected in expected_hashes.items():
        if _sha256(
            _field(plan, field_name, context),
            f"{context}.{field_name}",
        ) != expected:
            _fail(context, f"{field_name} is stale")
    _sha256(
        _field(plan, "template_sha256", context),
        f"{context}.template_sha256",
    )
    plan_cases = _array(
        _field(plan, "cases", context), f"{context}.cases"
    )
    if len(plan_cases) != expected_case_count:
        _fail(
            context,
            f"requires exactly {expected_case_count} cases",
        )
    cases: dict[str, dict[str, Any]] = {}
    invocation_fault_keys: set[tuple[str, str]] = set()
    for index, raw_case in enumerate(plan_cases):
        case_context = f"{context}.cases[{index}]"
        plan_case = _object(raw_case, case_context)
        _require_exact_fields(plan_case, CASE_PLAN_CASE_FIELDS, case_context)
        catalog = _object(
            _field(plan_case, "catalog", case_context),
            f"{case_context}.catalog",
        )
        catalog_context = f"{case_context}.catalog"
        _require_exact_fields(
            catalog,
            CATALOG_ENTRY_FIELDS,
            catalog_context,
        )
        case_id = _string(
            _field(catalog, "case_id", catalog_context),
            f"{catalog_context}.case_id",
        )
        if CASE_ID_PATTERN.fullmatch(case_id) is None or case_id in cases:
            _fail(catalog_context, "case_id is invalid or duplicated")
        sql_identity = _string(
            _field(catalog, "sql_identity", catalog_context),
            f"{catalog_context}.sql_identity",
        )
        invocation_key = _string(
            _field(catalog, "invocation_key", catalog_context),
            f"{catalog_context}.invocation_key",
        )
        fault_key = _fault_key(
            _field(catalog, "fault_key", catalog_context),
            f"{catalog_context}.fault_key",
        )
        inventory_entry = inventory["entries"].get(
            (sql_identity, fault_key)
        )
        if inventory_entry is None:
            _fail(catalog_context, "case is absent from runtime inventory")
        invocation_fault = (invocation_key, fault_key)
        if invocation_fault in invocation_fault_keys:
            _fail(catalog_context, "duplicate invocation/fault key")
        invocation_fault_keys.add(invocation_fault)
        for field_name, expected in {
            "site_id": inventory_entry["site_id"],
            "ledger_site_sha256": inventory_entry["ledger_site_sha256"],
            "relation": inventory_entry["relation"],
        }.items():
            if _field(catalog, field_name, catalog_context) != expected:
                _fail(
                    catalog_context,
                    f"{field_name} differs from runtime inventory",
                )
        if _validate_probe_contract(
            _field(catalog, "probe_contract", catalog_context),
            inventory_entry["relation"],
            f"{catalog_context}.probe_contract",
        ) != inventory_entry["probe_contract"]:
            _fail(catalog_context, "probe contract differs from inventory")
        if _string(
            _field(catalog, "postgres_severity", catalog_context),
            f"{catalog_context}.postgres_severity",
        ) != "error":
            _fail(catalog_context, "runtime cases must exercise ERROR only")
        expected = _object(
            _field(catalog, "expected", catalog_context),
            f"{catalog_context}.expected",
        )
        expected_context = f"{catalog_context}.expected"
        _require_exact_fields(
            expected,
            EXPECTED_RESULT_FIELDS,
            expected_context,
        )
        backend_outcome = _string(
            _field(expected, "backend_outcome", expected_context),
            f"{expected_context}.backend_outcome",
        )
        if backend_outcome not in {
            "recoverable_error",
            "backend_exit",
        }:
            _fail(
                catalog_context,
                "backend_outcome must be recoverable_error or backend_exit",
            )
        backend_exit = backend_outcome == "backend_exit"
        expected_sentinels = sorted(
            _runtime_event_list(
                _field(expected, "destructor_sentinels", expected_context),
                f"{expected_context}.destructor_sentinels",
                allow_empty=True,
            )
        )
        lifetime_free = (
            inventory_entry["ledger_site"]["classification"]
            == "lifetime_free_direct"
        )
        if backend_exit and expected_sentinels:
            _fail(
                catalog_context,
                "backend-exit cases cannot claim unobservable destructors",
            )
        if not backend_exit and (
            (lifetime_free and expected_sentinels)
            or (not lifetime_free and not expected_sentinels)
        ):
            _fail(catalog_context, "destructor expectation is not exact")
        expected_cleanup = sorted(
            _runtime_event_list(
                _field(expected, "cleanup_events", expected_context),
                f"{expected_context}.cleanup_events",
                allow_empty=True,
            )
        )
        expected_pg_cleanup = sorted(
            _runtime_event_list(
                _field(
                    expected,
                    "postgres_cleanup_events",
                    expected_context,
                ),
                f"{expected_context}.postgres_cleanup_events",
                allow_empty=(
                    backend_exit
                    or fault_key
                    == BACKEND_SCOPED_VACUUM_CLEANUP_FAULT_KEY
                ),
            )
        )
        if backend_exit and (expected_cleanup or expected_pg_cleanup):
            _fail(
                catalog_context,
                "backend-exit cases cannot claim unobservable cleanup",
            )
        if (
            not backend_exit
            and fault_key == BACKEND_SCOPED_VACUUM_CLEANUP_FAULT_KEY
            and expected_pg_cleanup
        ):
            _fail(
                catalog_context,
                "backend-scoped vacuum cleanup must not fabricate "
                "PostgreSQL/FMGR cleanup events",
            )
        if (
            not backend_exit
            and fault_key != BACKEND_SCOPED_VACUUM_CLEANUP_FAULT_KEY
            and "fmgr_abort_observed" not in expected_pg_cleanup
        ):
            _fail(catalog_context, "FMGR ABORT expectation is absent")
        raw_sqlstate = _field(expected, "sqlstate", expected_context)
        raw_message = _field(expected, "message", expected_context)
        if backend_exit:
            if raw_sqlstate is not None or raw_message is not None:
                _fail(
                    catalog_context,
                    "backend-exit SQLSTATE/message must be null",
                )
            sqlstate = None
            message = None
        else:
            sqlstate = _string(
                raw_sqlstate,
                f"{expected_context}.sqlstate",
            )
            if SQLSTATE_PATTERN.fullmatch(sqlstate) is None:
                _fail(catalog_context, "expected SQLSTATE is invalid")
            message = _string(
                raw_message,
                f"{expected_context}.message",
            )
        recovery_postcondition = _string(
            _field(expected, "recovery_postcondition", expected_context),
            f"{expected_context}.recovery_postcondition",
        )
        execution = _object(
            _field(plan_case, "execution", case_context),
            f"{case_context}.execution",
        )
        execution_context = f"{case_context}.execution"
        expected_execution_fields = EXECUTION_FIELDS
        if fault_key == BACKEND_SCOPED_VACUUM_CLEANUP_FAULT_KEY:
            expected_execution_fields = (
                EXECUTION_FIELDS | PRE_FAULT_CONTROL_EXECUTION_FIELDS
            )
        _require_exact_fields(
            execution,
            expected_execution_fields,
            execution_context,
        )
        if _string(
            _field(execution, "target_regprocedure", execution_context),
            f"{execution_context}.target_regprocedure",
        ) != sql_identity:
            _fail(case_context, "execution target differs from SQL identity")
        invocation_sql = _string(
            _field(execution, "invocation_sql", execution_context),
            f"{execution_context}.invocation_sql",
        )
        recovery_sql = _string(
            _field(execution, "recovery_sql", execution_context),
            f"{execution_context}.recovery_sql",
        )
        if (
            len(invocation_sql.encode("utf-8")) > 1024 * 1024
            or len(recovery_sql.encode("utf-8")) > 1024 * 1024
        ):
            _fail(case_context, "invocation or recovery SQL is oversized")
        normalized_execution = {
            "target_regprocedure": sql_identity,
            "setup_sql": _runtime_sql_array(
                _field(execution, "setup_sql", execution_context),
                f"{execution_context}.setup_sql",
            ),
            "invocation_sql": invocation_sql,
            "post_error_sql": _runtime_sql_array(
                _field(execution, "post_error_sql", execution_context),
                f"{execution_context}.post_error_sql",
            ),
            "recovery_sql": recovery_sql,
            "recovery_expected_rows": _runtime_row_matrix(
                _field(
                    execution,
                    "recovery_expected_rows",
                    execution_context,
                ),
                f"{execution_context}.recovery_expected_rows",
            ),
            "cleanup_sql": _runtime_sql_array(
                _field(execution, "cleanup_sql", execution_context),
                f"{execution_context}.cleanup_sql",
            ),
        }
        if fault_key == BACKEND_SCOPED_VACUUM_CLEANUP_FAULT_KEY:
            pre_fault_control_sql = _runtime_sql_array(
                _field(
                    execution,
                    "pre_fault_control_sql",
                    execution_context,
                ),
                f"{execution_context}.pre_fault_control_sql",
            )
            if not pre_fault_control_sql:
                _fail(
                    execution_context,
                    "pre_fault_control_sql must not be empty",
                )
            pre_fault_control_recovery_sql = _string(
                _field(
                    execution,
                    "pre_fault_control_recovery_sql",
                    execution_context,
                ),
                f"{execution_context}.pre_fault_control_recovery_sql",
            )
            if (
                len(pre_fault_control_recovery_sql.encode("utf-8"))
                > 1024 * 1024
            ):
                _fail(
                    execution_context,
                    "pre-fault control recovery SQL is oversized",
                )
            normalized_execution.update(
                {
                    "pre_fault_control_sql": pre_fault_control_sql,
                    "pre_fault_control_recovery_sql": (
                        pre_fault_control_recovery_sql
                    ),
                    "pre_fault_control_recovery_expected_rows": (
                        _runtime_row_matrix(
                            _field(
                                execution,
                                (
                                    "pre_fault_control_recovery_"
                                    "expected_rows"
                                ),
                                execution_context,
                            ),
                            (
                                f"{execution_context}."
                                "pre_fault_control_recovery_expected_rows"
                            ),
                        )
                    ),
                }
            )
        cases[case_id] = {
            "catalog": dict(catalog),
            "execution": normalized_execution,
            "expected_backend_outcome": backend_outcome,
            "expected_destructor_sentinels": expected_sentinels,
            "expected_cleanup_events": expected_cleanup,
            "expected_postgres_cleanup_events": expected_pg_cleanup,
            "expected_sqlstate": sqlstate,
            "expected_message": message,
            "expected_recovery_postcondition": recovery_postcondition,
        }
    if not cases:
        _fail(context, "case plan is empty")
    return cases


def _validate_terminal_causality(
    value: Any,
    *,
    backend_exit: bool,
    site_id: str,
    fault_key: str,
    backend_pid: int,
    marker_directory: Path,
    server_log_path: Path,
    marker_uid: int,
    context: str,
) -> dict[str, Any] | None:
    if not backend_exit:
        if value is not None:
            _fail(context, "non-terminal case carries terminal causality")
        return None
    guard_kinds = {
        "scoped_spi_session.emergency_finish_error": "finish_error",
        "scoped_spi_session.emergency_finish_non_ok": "finish_non_ok",
    }
    guard_kind = guard_kinds.get(fault_key)
    if guard_kind is None:
        _fail(context, "backend exit is not an authorized terminal guard")
    causality = _object(value, context)
    _require_exact_fields(causality, TERMINAL_CAUSALITY_FIELDS, context)
    if (
        _field(causality, "marker_directory", context)
        != str(marker_directory)
        or _field(causality, "server_log_path", context)
        != str(server_log_path)
        or not _boolean(
            _field(causality, "postmaster_identity_unchanged", context),
            f"{context}.postmaster_identity_unchanged",
        )
        or not _boolean(
            _field(
                causality,
                "fatal_panic_signal_core_rejected",
                context,
            ),
            f"{context}.fatal_panic_signal_core_rejected",
        )
    ):
        _fail(context, "terminal authority paths or postmaster proof differ")

    marker_context = f"{context}.marker"
    marker = _object(_field(causality, "marker", context), marker_context)
    _require_exact_fields(marker, TERMINAL_MARKER_FIELDS, marker_context)
    if _integer(
        _field(marker, "schema_revision", marker_context),
        f"{marker_context}.schema_revision",
    ) != 1:
        _fail(marker_context, "unsupported terminal marker revision")
    token = _sha256(
        _field(marker, "token", marker_context),
        f"{marker_context}.token",
    )
    expected_payload = (
        '{"schema":"bioseq.postgres_error_safety.terminal_marker",'
        '"schema_revision":1,'
        f'"token":"{token}","site_id":"{site_id}",'
        f'"fault_key":"{fault_key}","guard_kind":"{guard_kind}",'
        f'"backend_pid":{backend_pid}}}\n'
    )
    marker_exact = {
        "path": str(marker_directory / f"terminal-{token}.marker"),
        "payload": expected_payload,
        "schema": "bioseq.postgres_error_safety.terminal_marker",
        "schema_revision": 1,
        "site_id": site_id,
        "fault_key": fault_key,
        "guard_kind": guard_kind,
        "backend_pid": backend_pid,
        "mode": 0o600,
        "uid": marker_uid,
        "link_count": 1,
    }
    for field_name, expected in marker_exact.items():
        if _field(marker, field_name, marker_context) != expected:
            _fail(marker_context, f"{field_name} differs")
    if _sha256(
        _field(marker, "sha256", marker_context),
        f"{marker_context}.sha256",
    ) != hashlib.sha256(expected_payload.encode("utf-8")).hexdigest():
        _fail(marker_context, "payload hash differs")

    log_context = f"{context}.server_log"
    server_log = _object(
        _field(causality, "server_log", context),
        log_context,
    )
    _require_exact_fields(
        server_log,
        TERMINAL_SERVER_LOG_FIELDS,
        log_context,
    )
    interval_text = _string(
        _field(server_log, "interval_text", log_context),
        f"{log_context}.interval_text",
    )
    interval_raw = interval_text.encode("utf-8")
    exit_record = (
        f"server process (PID {backend_pid}) exited with exit code 1"
    )
    lower_interval = interval_text.lower()
    if (
        _field(server_log, "path", log_context) != str(server_log_path)
        or _integer(
            _field(server_log, "device", log_context),
            f"{log_context}.device",
        )
        <= 0
        or _integer(
            _field(server_log, "inode", log_context),
            f"{log_context}.inode",
        )
        <= 0
        or _integer(
            _field(server_log, "offset", log_context),
            f"{log_context}.offset",
        )
        < 0
        or _integer(
            _field(server_log, "interval_size", log_context),
            f"{log_context}.interval_size",
        )
        != len(interval_raw)
        or not interval_raw
        or len(interval_raw) > 1024 * 1024
        or _sha256(
            _field(server_log, "interval_sha256", log_context),
            f"{log_context}.interval_sha256",
        )
        != hashlib.sha256(interval_raw).hexdigest()
        or _field(server_log, "exit_record", log_context) != exit_record
        or exit_record not in interval_text
        or "\0" in interval_text
        or _boolean(
            _field(server_log, "fatal_present", log_context),
            f"{log_context}.fatal_present",
        )
        or _boolean(
            _field(server_log, "panic_present", log_context),
            f"{log_context}.panic_present",
        )
        or _boolean(
            _field(server_log, "signal_present", log_context),
            f"{log_context}.signal_present",
        )
        or _boolean(
            _field(server_log, "core_dump_present", log_context),
            f"{log_context}.core_dump_present",
        )
        or "fatal:" in lower_interval
        or "panic:" in lower_interval
        or "terminated by signal" in lower_interval
        or "segmentation fault" in lower_interval
        or "core dump" in lower_interval
        or "dumped core" in lower_interval
    ):
        _fail(
            log_context,
            "bounded log interval lacks the intended exit or contains a "
            "crash substitution",
        )
    return causality


def _stage_relative_coordinate(value: Any, context: str) -> tuple[str, Path]:
    text = _string(value, context)
    relative = Path(text)
    if (
        relative.is_absolute()
        or not relative.parts
        or any(part in {"", ".", ".."} for part in relative.parts)
        or "\\" in text
        or str(relative) != text
    ):
        _fail(context, "path must be normalized and relative")
    return text, relative


def _stage_absolute_coordinate(value: Any, context: str) -> tuple[str, Path]:
    text = _string(value, context)
    if not _absolute_normalized_path(text):
        _fail(context, "path must be normalized and absolute")
    return text, Path(text)


def _stage_runtime_entry_record(entry: Mapping[str, Any]) -> str:
    link_target = entry["source_link_target"]
    return (
        f"root:{entry['root']}\n"
        f"path:{entry['path']}\n"
        f"kind:{entry['kind']}\n"
        f"source_kind:{entry['source_kind']}\n"
        f"source_path:{entry['source_path']}\n"
        f"source_link_target:{link_target if link_target is not None else '-'}\n"
        f"resolved_source_path:{entry['resolved_source_path']}\n"
        f"size:{entry['size'] if entry['size'] is not None else '-'}\n"
        f"sha256:{entry['sha256'] if entry['sha256'] is not None else '-'}\n"
    )


def _stage_loader_entry_record(entry: Mapping[str, Any]) -> str:
    return (
        f"source:{entry['source']}\n"
        f"staged:{entry['staged']}\n"
        f"size:{entry['size']}\n"
        f"sha256:{entry['sha256']}\n"
    )


def _stage_package_aggregate(
    *,
    name: str,
    module_pathname_override: str | None,
    control_filename: str,
    control_sha256: str,
    install_sql_filename: str,
    install_sql_sha256: str,
    library_filename: str,
    library_sha256: str,
) -> str:
    override = (
        "" if module_pathname_override is None else module_pathname_override
    )
    record = (
        f"artifact:{name}\n"
        f"module_pathname_override:{override}\n"
        f"control:{control_filename}:{control_sha256}\n"
        f"install_sql:{install_sql_filename}:{install_sql_sha256}\n"
        f"library:{library_filename}:{library_sha256}\n"
    ).encode("utf-8")
    return _artifact_sha256(record)


def _validate_stage_runtime_closure(
    value: Any,
    *,
    context: str,
    source: Mapping[str, Any],
    staged: Mapping[str, Any],
    stage_root: Path,
) -> None:
    closure = _object(value, context)
    _require_exact_fields(
        closure,
        STAGE_RUNTIME_CLOSURE_FIELDS,
        context,
    )
    if (
        _string(
            _field(closure, "algorithm", context),
            f"{context}.algorithm",
        )
        != RUNTIME_CLOSURE_ALGORITHM
        or _integer(
            _field(closure, "schema_revision", context),
            f"{context}.schema_revision",
        )
        != RUNTIME_CLOSURE_SCHEMA_REVISION
    ):
        _fail(context, "runtime closure algorithm/schema differs")

    roots = _object(_field(closure, "roots", context), f"{context}.roots")
    _require_exact_fields(roots, STAGE_LAYOUT_FIELDS, f"{context}.roots")
    canonical_roots: dict[str, Path] = {}
    for name in STAGE_LAYOUT_FIELDS:
        root_context = f"{context}.roots.{name}"
        root = _object(_field(roots, name, f"{context}.roots"), root_context)
        _require_exact_fields(
            root,
            STAGE_RUNTIME_ROOT_FIELDS,
            root_context,
        )
        reported = _string(
            _field(root, "reported_source", root_context),
            f"{root_context}.reported_source",
        )
        canonical_text, canonical = _stage_absolute_coordinate(
            _field(root, "canonical_source", root_context),
            f"{root_context}.canonical_source",
        )
        staged_text = _string(
            _field(root, "staged", root_context),
            f"{root_context}.staged",
        )
        if (
            reported != source[name]
            or staged_text != staged[name]
            or str(canonical) != canonical_text
        ):
            _fail(root_context, "runtime root is inconsistent")
        canonical_roots[name] = canonical

    entries = _array(
        _field(closure, "entries", context),
        f"{context}.entries",
    )
    if _integer(
        _field(closure, "entry_count", context),
        f"{context}.entry_count",
    ) != len(entries):
        _fail(context, "runtime closure entry count differs")
    root_order = {"bindir": 0, "sharedir": 1, "pkglibdir": 2}
    runtime_keys: list[tuple[int, str]] = []
    runtime_records: list[str] = []
    runtime_entries: dict[tuple[str, str], Mapping[str, Any]] = {}
    for index, raw_entry in enumerate(entries):
        entry_context = f"{context}.entries[{index}]"
        entry = _object(raw_entry, entry_context)
        _require_exact_fields(
            entry,
            STAGE_RUNTIME_ENTRY_FIELDS,
            entry_context,
        )
        root_name = _string(
            _field(entry, "root", entry_context),
            f"{entry_context}.root",
        )
        if root_name not in root_order:
            _fail(entry_context, "runtime root is unknown")
        relative_text, relative = _stage_relative_coordinate(
            _field(entry, "path", entry_context),
            f"{entry_context}.path",
        )
        kind = _string(
            _field(entry, "kind", entry_context),
            f"{entry_context}.kind",
        )
        source_kind = _string(
            _field(entry, "source_kind", entry_context),
            f"{entry_context}.source_kind",
        )
        if kind not in {"file", "dir"} or source_kind not in {
            "file",
            "dir",
            "symlink",
        }:
            _fail(entry_context, "runtime entry kind is invalid")
        if source_kind != "symlink" and source_kind != kind:
            _fail(entry_context, "runtime source kind differs")
        _stage_absolute_coordinate(
            _field(entry, "source_path", entry_context),
            f"{entry_context}.source_path",
        )
        _, resolved_source = _stage_absolute_coordinate(
            _field(entry, "resolved_source_path", entry_context),
            f"{entry_context}.resolved_source_path",
        )
        try:
            resolved_source.relative_to(canonical_roots[root_name])
        except ValueError:
            _fail(entry_context, "resolved source escapes its runtime root")
        link_target = _field(entry, "source_link_target", entry_context)
        if source_kind == "symlink":
            _string(link_target, f"{entry_context}.source_link_target")
        elif link_target is not None:
            _fail(entry_context, "runtime entry has a spurious link target")

        destination = stage_root / str(staged[root_name]) / relative
        if kind == "dir":
            if entry["size"] is not None or entry["sha256"] is not None:
                _fail(entry_context, "runtime directory has file metadata")
            _canonical_directory(destination, f"{entry_context}.staged")
        else:
            size = _integer(
                _field(entry, "size", entry_context),
                f"{entry_context}.size",
            )
            digest = _sha256(
                _field(entry, "sha256", entry_context),
                f"{entry_context}.sha256",
            )
            _, raw, metadata = _authenticated_file_bytes(
                destination,
                f"{entry_context}.staged",
                require_single_link=False,
            )
            if metadata.st_size != size or _artifact_sha256(raw) != digest:
                _fail(entry_context, "staged runtime file bytes differ")

        key = (root_name, relative_text)
        if key in runtime_entries:
            _fail(entry_context, "runtime coordinate is duplicated")
        runtime_entries[key] = entry
        runtime_keys.append((root_order[root_name], relative_text))
        runtime_records.append(_stage_runtime_entry_record(entry))
    if runtime_keys != sorted(runtime_keys):
        _fail(context, "runtime closure entries are not canonical")
    if _sha256(
        _field(closure, "aggregate_sha256", context),
        f"{context}.aggregate_sha256",
    ) != _artifact_sha256("".join(runtime_records).encode("utf-8")):
        _fail(context, "runtime closure aggregate is stale")

    sentinels = _object(
        _field(closure, "required_sentinels", context),
        f"{context}.required_sentinels",
    )
    for name, raw_sentinel in sentinels.items():
        sentinel_context = f"{context}.required_sentinels.{name}"
        if not name:
            _fail(sentinel_context, "sentinel name is empty")
        sentinel = _object(raw_sentinel, sentinel_context)
        _require_exact_fields(
            sentinel,
            STAGE_RUNTIME_SENTINEL_FIELDS,
            sentinel_context,
        )
        root_name = _string(
            _field(sentinel, "root", sentinel_context),
            f"{sentinel_context}.root",
        )
        if root_name not in root_order:
            _fail(sentinel_context, "sentinel root is unknown")
        relative_text, _ = _stage_relative_coordinate(
            _field(sentinel, "path", sentinel_context),
            f"{sentinel_context}.path",
        )
        kind = _string(
            _field(sentinel, "kind", sentinel_context),
            f"{sentinel_context}.kind",
        )
        if kind == "file":
            digest: str | None = _sha256(
                _field(sentinel, "sha256", sentinel_context),
                f"{sentinel_context}.sha256",
            )
        elif kind == "dir":
            if sentinel["sha256"] is not None:
                _fail(sentinel_context, "directory sentinel has a hash")
            digest = None
        else:
            _fail(sentinel_context, "sentinel kind is invalid")
        runtime_entry = runtime_entries.get((root_name, relative_text))
        if (
            runtime_entry is None
            or runtime_entry["kind"] != kind
            or runtime_entry["sha256"] != digest
        ):
            _fail(sentinel_context, "sentinel is outside runtime closure")

    loader_context = f"{context}.loader_dependencies"
    loader = _object(
        _field(closure, "loader_dependencies", context),
        loader_context,
    )
    _require_exact_fields(
        loader,
        STAGE_LOADER_CLOSURE_FIELDS,
        loader_context,
    )
    loader_entries = _array(
        _field(loader, "entries", loader_context),
        f"{loader_context}.entries",
    )
    if _integer(
        _field(loader, "entry_count", loader_context),
        f"{loader_context}.entry_count",
    ) != len(loader_entries):
        _fail(loader_context, "loader closure entry count differs")
    loader_keys: list[tuple[bytes, bytes]] = []
    loader_records: list[str] = []
    for index, raw_entry in enumerate(loader_entries):
        entry_context = f"{loader_context}.entries[{index}]"
        entry = _object(raw_entry, entry_context)
        _require_exact_fields(
            entry,
            STAGE_LOADER_ENTRY_FIELDS,
            entry_context,
        )
        source_text, _ = _stage_absolute_coordinate(
            _field(entry, "source", entry_context),
            f"{entry_context}.source",
        )
        staged_text, staged_relative = _stage_relative_coordinate(
            _field(entry, "staged", entry_context),
            f"{entry_context}.staged",
        )
        size = _integer(
            _field(entry, "size", entry_context),
            f"{entry_context}.size",
        )
        digest = _sha256(
            _field(entry, "sha256", entry_context),
            f"{entry_context}.sha256",
        )
        _, raw, metadata = _authenticated_file_bytes(
            stage_root / staged_relative,
            f"{entry_context}.staged",
            require_single_link=False,
        )
        if metadata.st_size != size or _artifact_sha256(raw) != digest:
            _fail(entry_context, "staged loader dependency bytes differ")
        loader_keys.append(
            (staged_text.encode("utf-8"), source_text.encode("utf-8"))
        )
        loader_records.append(_stage_loader_entry_record(entry))
    if len(set(loader_keys)) != len(loader_keys) or loader_keys != sorted(
        loader_keys
    ):
        _fail(loader_context, "loader closure entries are not canonical")
    if _sha256(
        _field(loader, "aggregate_sha256", loader_context),
        f"{loader_context}.aggregate_sha256",
    ) != _artifact_sha256("".join(loader_records).encode("utf-8")):
        _fail(loader_context, "loader closure aggregate is stale")


def _parse_stage_manifest_binding(
    path: Path,
    *,
    profile: str,
    expected_dso_sha256: str,
    expected_sql_script_sha256: str,
) -> StageManifestBinding:
    context = f"{profile}-stage-manifest"
    canonical_path = _canonical_regular_file(path, context)
    manifest, raw = load_json(canonical_path)
    _require_exact_fields(manifest, STAGE_MANIFEST_FIELDS, context)
    version = _string(
        _field(manifest, "postgresql_version", context),
        f"{context}.postgresql_version",
    )
    if (
        _string(_field(manifest, "schema", context), f"{context}.schema")
        != STAGE_MANIFEST_SCHEMA
        or _integer(
            _field(manifest, "schema_revision", context),
            f"{context}.schema_revision",
        )
        != STAGE_MANIFEST_SCHEMA_REVISION
        or (
            version != "PostgreSQL 17.10"
            and not version.startswith("PostgreSQL 17.10 ")
        )
    ):
        _fail(context, "stage schema or PostgreSQL release differs")
    _string(
        _field(manifest, "postgresql_configure", context),
        f"{context}.postgresql_configure",
    )
    source = _object(_field(manifest, "source", context), f"{context}.source")
    staged = _object(_field(manifest, "staged", context), f"{context}.staged")
    _require_exact_fields(source, STAGE_LAYOUT_FIELDS, f"{context}.source")
    _require_exact_fields(staged, STAGE_LAYOUT_FIELDS, f"{context}.staged")
    stage_root = _canonical_directory(
        canonical_path.parent,
        f"{context}.root",
    )
    staged_directories: dict[str, Path] = {}
    for field_name in STAGE_LAYOUT_FIELDS:
        _stage_absolute_coordinate(
            _field(source, field_name, f"{context}.source"),
            f"{context}.source.{field_name}",
        )
        _, relative = _stage_relative_coordinate(
            _field(staged, field_name, f"{context}.staged"),
            f"{context}.staged.{field_name}",
        )
        member = _canonical_directory(
            stage_root / relative,
            f"{context}.staged.{field_name}",
        )
        try:
            member.relative_to(stage_root)
        except ValueError:
            _fail(
                f"{context}.staged.{field_name}",
                "staged directory escapes the stage root",
            )
        staged_directories[field_name] = member
    _validate_stage_runtime_closure(
        _field(manifest, "runtime_closure", context),
        context=f"{context}.runtime_closure",
        source=source,
        staged=staged,
        stage_root=stage_root,
    )
    expected_paths = _object(
        _field(manifest, "expected_tool_paths", context),
        f"{context}.expected_tool_paths",
    )
    _require_exact_fields(
        expected_paths,
        frozenset(POSTGRESQL_TOOLS),
        f"{context}.expected_tool_paths",
    )
    signatures: dict[str, dict[str, Any]] = {}
    raw_signatures = _array(
        _field(manifest, "tool_signatures", context),
        f"{context}.tool_signatures",
    )
    if len(raw_signatures) != len(POSTGRESQL_TOOLS):
        _fail(context, "six-tool signature suite is incomplete")
    bindir = _string(staged["bindir"], f"{context}.staged.bindir")
    for index, raw_signature in enumerate(raw_signatures):
        signature_context = f"{context}.tool_signatures[{index}]"
        signature = _object(raw_signature, signature_context)
        _require_exact_fields(
            signature,
            STAGE_TOOL_SIGNATURE_FIELDS,
            signature_context,
        )
        tool = _string(signature["tool"], f"{signature_context}.tool")
        if tool not in POSTGRESQL_TOOLS or tool in signatures:
            _fail(signature_context, "tool is unknown or duplicated")
        expected_relative = str(Path(bindir) / tool)
        if (
            signature["staged_path"] != expected_relative
            or expected_paths[tool] != expected_relative
        ):
            _fail(signature_context, "staged tool path differs")
        tool_version = _string(
            signature["version"],
            f"{signature_context}.version",
        )
        prefix = f"{tool} (PostgreSQL) 17.10"
        if tool_version != prefix and not tool_version.startswith(prefix + " "):
            _fail(signature_context, "staged tool version differs")
        tool_path, tool_raw, tool_metadata = _authenticated_file_bytes(
            stage_root / expected_relative,
            signature_context,
            require_single_link=False,
        )
        try:
            tool_path.relative_to(stage_root)
        except ValueError:
            _fail(signature_context, "staged tool escapes the stage root")
        if (
            stat.S_IMODE(tool_metadata.st_mode) & 0o111 == 0
            or _artifact_sha256(tool_raw)
            != _sha256(
                signature["sha256"],
                f"{signature_context}.sha256",
            )
        ):
            _fail(
                signature_context,
                "staged tool bytes or executable mode differ",
            )
        signatures[tool] = dict(signature)
    extension_context = f"{context}.extension_artifacts"
    extension_closure = _object(
        _field(manifest, "extension_artifacts", context),
        extension_context,
    )
    _require_exact_fields(
        extension_closure,
        STAGE_EXTENSION_CLOSURE_FIELDS,
        extension_context,
    )
    artifact_values = _array(
        _field(extension_closure, "artifacts", extension_context),
        f"{extension_context}.artifacts",
    )
    if len(artifact_values) != 1:
        _fail(extension_context, "exactly one Bioseq artifact is required")
    artifact_context = f"{extension_context}.artifacts[0]"
    artifact = _object(artifact_values[0], artifact_context)
    _require_exact_fields(
        artifact,
        STAGE_EXTENSION_ARTIFACT_FIELDS,
        artifact_context,
    )
    if (
        _string(
            _field(artifact, "name", artifact_context),
            f"{artifact_context}.name",
        )
        != "bioseq"
        or artifact["module_pathname_override"] is not None
    ):
        _fail(artifact_context, "Bioseq artifact identity differs")
    effective_module_name = _string(
        _field(artifact, "effective_module_name", artifact_context),
        f"{artifact_context}.effective_module_name",
    )
    control_context = f"{artifact_context}.control"
    install_context = f"{artifact_context}.install_sql"
    library_context = f"{artifact_context}.library"
    control = _object(
        _field(artifact, "control", artifact_context),
        control_context,
    )
    install_sql = _object(
        _field(artifact, "install_sql", artifact_context),
        install_context,
    )
    library = _object(
        _field(artifact, "library", artifact_context),
        library_context,
    )
    _require_exact_fields(
        control,
        STAGE_CONTROL_ARTIFACT_FIELDS,
        control_context,
    )
    _require_exact_fields(
        install_sql,
        STAGE_INSTALL_SQL_ARTIFACT_FIELDS,
        install_context,
    )
    _require_exact_fields(
        library,
        STAGE_LIBRARY_ARTIFACT_FIELDS,
        library_context,
    )
    sharedir_relative = _string(
        _field(staged, "sharedir", f"{context}.staged"),
        f"{context}.staged.sharedir",
    )
    control_filename = _string(
        _field(control, "filename", control_context),
        f"{control_context}.filename",
    )
    if control_filename != "bioseq.control":
        _fail(control_context, "control filename must be bioseq.control")
    control_source_text, control_source_path = _stage_absolute_coordinate(
        _field(control, "source", control_context),
        f"{control_context}.source",
    )
    if control_source_path.name != control_filename:
        _fail(control_context, "control source filename differs")
    control_relative = _string(
        _field(control, "staged", control_context),
        f"{control_context}.staged",
    )
    expected_control_relative = str(
        Path(sharedir_relative) / "extension" / control_filename
    )
    if control_relative != expected_control_relative:
        _fail(
            f"{control_context}.staged",
            "control path differs from staged sharedir",
        )
    control_path = _canonical_regular_file(
        stage_root / control_relative,
        f"{control_context}.staged",
    )
    control_source_path = _canonical_regular_file(
        Path(control_source_text),
        f"{control_context}.source",
    )
    control_sha256 = _sha256(
        _field(control, "sha256", control_context),
        f"{control_context}.sha256",
    )
    control_source_sha256 = _sha256(
        _field(control, "source_sha256", control_context),
        f"{control_context}.source_sha256",
    )
    if (
        _path_artifact_sha256(control_path, control_context) != control_sha256
        or _path_artifact_sha256(
            control_source_path,
            f"{control_context}.source",
        )
        != control_source_sha256
        or control_source_sha256 != control_sha256
    ):
        _fail(context, "manifest-selected control bytes differ")

    install_version = _string(
        _field(
            install_sql,
            "version",
            install_context,
        ),
        f"{install_context}.version",
    )
    if install_version != "1.0":
        _fail(
            f"{install_context}.version",
            "extension install version must be 1.0",
        )
    install_filename = _string(
        _field(install_sql, "filename", install_context),
        f"{install_context}.filename",
    )
    if install_filename != f"bioseq--{install_version}.sql":
        _fail(install_context, "install SQL filename differs")
    install_source_text, install_source_path = _stage_absolute_coordinate(
        _field(install_sql, "source", install_context),
        f"{install_context}.source",
    )
    if install_source_path.name != install_filename:
        _fail(install_context, "install SQL source filename differs")
    install_sha256 = _sha256(
        _field(
            install_sql,
            "sha256",
            install_context,
        ),
        f"{install_context}.sha256",
    )
    install_relative = str(
        Path(sharedir_relative)
        / "extension"
        / install_filename
    )
    install_staged = _string(
        _field(install_sql, "staged", install_context),
        f"{install_context}.staged",
    )
    if install_staged != install_relative:
        _fail(install_context, "install SQL path differs from staged sharedir")
    install_path = _canonical_regular_file(
        stage_root / install_relative,
        f"{install_context}.staged",
    )
    install_source_path = _canonical_regular_file(
        Path(install_source_text),
        f"{install_context}.source",
    )
    if (
        _path_artifact_sha256(install_path, install_context)
        != install_sha256
        or _path_artifact_sha256(
            install_source_path,
            f"{install_context}.source",
        )
        != install_sha256
        or install_sha256 != expected_sql_script_sha256
    ):
        _fail(context, "manifest-selected SQL bytes differ")

    library_filename = _string(
        _field(
            library,
            "filename",
            library_context,
        ),
        f"{library_context}.filename",
    )
    if (
        Path(library_filename).name != library_filename
        or library_filename in {"", ".", ".."}
    ):
        _fail(
            f"{library_context}.filename",
            "library filename must be one basename",
        )
    library_source_text, library_source_path = _stage_absolute_coordinate(
        _field(library, "source", library_context),
        f"{library_context}.source",
    )
    if library_source_path.name != library_filename:
        _fail(library_context, "library source filename differs")
    if effective_module_name != library_filename.split(".", 1)[0]:
        _fail(artifact_context, "effective module name differs")
    pkglibdir_relative = _string(
        _field(staged, "pkglibdir", f"{context}.staged"),
        f"{context}.staged.pkglibdir",
    )
    library_relative = _string(
        _field(library, "staged", library_context),
        f"{library_context}.staged",
    )
    expected_library_relative = str(
        Path(pkglibdir_relative) / library_filename
    )
    if library_relative != expected_library_relative:
        _fail(
            f"{library_context}.staged",
            "library path differs from staged pkglibdir",
        )
    library_path = _canonical_regular_file(
        stage_root / library_relative,
        f"{library_context}.staged",
    )
    library_source_path = _canonical_regular_file(
        Path(library_source_text),
        f"{library_context}.source",
    )
    library_sha256 = _sha256(
        _field(
            library,
            "sha256",
            library_context,
        ),
        f"{library_context}.sha256",
    )
    if (
        _path_artifact_sha256(library_path, library_context)
        != library_sha256
        or _path_artifact_sha256(
            library_source_path,
            f"{library_context}.source",
        )
        != library_sha256
        or library_sha256 != expected_dso_sha256
    ):
        _fail(context, "manifest-selected DSO bytes differ")
    if _sha256(
        _field(
            extension_closure,
            "aggregate_sha256",
            extension_context,
        ),
        f"{extension_context}.aggregate_sha256",
    ) != _stage_package_aggregate(
        name="bioseq",
        module_pathname_override=None,
        control_filename=control_filename,
        control_sha256=control_sha256,
        install_sql_filename=install_filename,
        install_sql_sha256=install_sha256,
        library_filename=library_filename,
        library_sha256=library_sha256,
    ):
        _fail(context, "Bioseq aggregate hash is stale")

    suite = [signatures[tool] for tool in sorted(POSTGRESQL_TOOLS)]
    return StageManifestBinding(
        manifest_path=canonical_path,
        manifest_sha256=_artifact_sha256(raw),
        tool_signature_suite_sha256=_artifact_sha256(
            _canonical_json_bytes(suite)
        ),
        stage_root=stage_root,
        bindir=staged_directories["bindir"],
        sharedir=staged_directories["sharedir"],
        pkglibdir=staged_directories["pkglibdir"],
        pg_ctl_path=_canonical_regular_file(
            stage_root
            / _string(
                _field(
                    expected_paths,
                    "pg_ctl",
                    f"{context}.expected_tool_paths",
                ),
                f"{context}.expected_tool_paths.pg_ctl",
            ),
            f"{context}.expected_tool_paths.pg_ctl",
        ),
        control_path=control_path,
        control_sha256=control_sha256,
        install_sql_path=install_path,
        install_sql_sha256=install_sha256,
        library_path=library_path,
        library_sha256=library_sha256,
    )


def _configured_absolute_path(value: Any, context: str) -> Path:
    text = _string(value, context)
    path = Path(text)
    if (
        not path.is_absolute()
        or ".." in path.parts
        or str(path) != text
    ):
        _fail(context, "path must be absolute and normalized")
    return path


def _contained_path(path: Path, root: Path, context: str) -> None:
    try:
        path.relative_to(root)
    except ValueError:
        _fail(context, f"path escapes authenticated root {root}")


def _validate_connection_authority(
    value: Any,
    *,
    expected_socket_directory: Path,
    expected_port: str,
    expected_user: str,
    context: str,
) -> Path:
    connection = _object(value, context)
    _require_exact_fields(connection, CONNECTION_FIELDS, context)
    socket_directory = (
        _configured_absolute_path(
            _field(connection, "host", context),
            f"{context}.host",
        )
    )
    if socket_directory != expected_socket_directory:
        _fail(f"{context}.host", "local socket coordinate differs")
    port_text = _string(
        _field(connection, "port", context),
        f"{context}.port",
    )
    if (
        not port_text.isdecimal()
        or port_text.startswith("0")
        or not 1 <= int(port_text) <= 65535
    ):
        _fail(f"{context}.port", "port must be a canonical decimal TCP port")
    if port_text != expected_port:
        _fail(f"{context}.port", "connection port differs from authority")
    if _string(
        _field(connection, "database", context),
        f"{context}.database",
    ) != PRIVATE_DATABASE:
        _fail(f"{context}.database", "dedicated database name differs")
    user = _string(
        _field(connection, "user", context),
        f"{context}.user",
    )
    if not user or "\x00" in user:
        _fail(f"{context}.user", "database user is empty or invalid")
    if user != expected_user:
        _fail(f"{context}.user", "database user differs from authority")
    if _string(
        _field(connection, "password", context),
        f"{context}.password",
    ) != PRIVATE_PASSWORD:
        _fail(f"{context}.password", "private trust-auth marker differs")
    return socket_directory


def _validate_authorization_marker(
    path: Path,
    *,
    data_directory: Path,
    context: str,
    expected_uid: int | None = None,
) -> Path:
    if path != data_directory / AUTHORIZATION_MARKER_NAME:
        _fail(context, "authorization marker path or filename differs")
    canonical, raw, metadata = _authenticated_file_bytes(
        path,
        context,
        require_single_link=True,
    )
    if raw != AUTHORIZATION_MARKER:
        _fail(context, "authorization marker bytes differ")
    if stat.S_IMODE(metadata.st_mode) != 0o600:
        _fail(context, "authorization marker mode must be 0600")
    owner_uid = os.getuid() if expected_uid is None else expected_uid
    if metadata.st_uid != owner_uid:
        _fail(context, "authorization marker is not owned by the current uid")
    return canonical


def _load_retained_cluster_authority(
    *,
    matrix: Mapping[str, Any],
    profile: str,
    candidate_root: Path,
    artifact_identities: dict[tuple[int, int], tuple[Any, ...]],
    expected_uid: int | None = None,
) -> RetainedClusterAuthority:
    context = f"{profile}-cluster-authority"
    record_path_field = f"{profile}_cluster_authority_path"
    record_hash_field = f"{profile}_cluster_authority_sha256"
    record_payload, _, record_path = _load_candidate_artifact(
        candidate_root=candidate_root,
        path_text=_field(matrix, record_path_field, "runtime-matrix"),
        category="cluster_authorities",
        profile=profile,
        stem="cluster_authority",
        suffix="json",
        expected_sha256=_field(
            matrix,
            record_hash_field,
            "runtime-matrix",
        ),
        context=context,
        artifact_identities=artifact_identities,
        semantic_identity=("cluster_authority", profile),
    )
    record = _object(record_payload, context)
    _require_exact_fields(record, CLUSTER_AUTHORITY_FIELDS, context)
    if (
        _integer(
            _field(record, "schema_revision", context),
            f"{context}.schema_revision",
        )
        != 1
        or _string(
            _field(record, "profile", context),
            f"{context}.profile",
        )
        != profile
    ):
        _fail(context, "schema or profile differs")

    system_identifier = _string(
        _field(record, "cluster_system_identifier", context),
        f"{context}.cluster_system_identifier",
    )
    if not system_identifier.isdecimal() or int(system_identifier) <= 0:
        _fail(context, "cluster system identifier is not a positive decimal")
    data_directory = _configured_absolute_path(
        _field(record, "data_directory", context),
        f"{context}.data_directory",
    )
    socket_directory = _configured_absolute_path(
        _field(record, "socket_directory", context),
        f"{context}.socket_directory",
    )
    original_authorization_path = _configured_absolute_path(
        _field(record, "original_authorization_path", context),
        f"{context}.original_authorization_path",
    )
    _contained_path(data_directory, candidate_root, f"{context}.data_directory")
    _contained_path(
        socket_directory,
        candidate_root,
        f"{context}.socket_directory",
    )
    if (
        data_directory == socket_directory
        or original_authorization_path
        != data_directory / AUTHORIZATION_MARKER_NAME
    ):
        _fail(context, "cluster coordinate or marker path differs")
    connection_port = _string(
        _field(record, "connection_port", context),
        f"{context}.connection_port",
    )
    if (
        not connection_port.isdecimal()
        or connection_port.startswith("0")
        or not 1 <= int(connection_port) <= 65535
    ):
        _fail(context, "authority connection port is not canonical")
    connection_user = _string(
        _field(record, "connection_user", context),
        f"{context}.connection_user",
    )
    if not connection_user or "\x00" in connection_user:
        _fail(context, "authority connection user is empty or invalid")

    mode = _integer(
        _field(record, "authorization_mode", context),
        f"{context}.authorization_mode",
    )
    uid = _integer(
        _field(record, "authorization_uid", context),
        f"{context}.authorization_uid",
    )
    current_uid = os.getuid() if expected_uid is None else expected_uid
    if mode != 0o600 or uid != current_uid:
        _fail(context, "authorization mode or current uid differs")
    authorization_sha256 = _sha256(
        _field(record, "authorization_sha256", context),
        f"{context}.authorization_sha256",
    )
    artifact_sha256 = _sha256(
        _field(record, "authorization_artifact_sha256", context),
        f"{context}.authorization_artifact_sha256",
    )
    authorization_payload, authorization_raw, authorization_path = (
        _load_candidate_artifact(
            candidate_root=candidate_root,
            path_text=_field(
                record,
                "authorization_artifact_path",
                context,
            ),
            category="cluster_authorities",
            profile=profile,
            stem="authorization",
            suffix="marker",
            expected_sha256=artifact_sha256,
            context=f"{context}.authorization_artifact",
            artifact_identities=artifact_identities,
            semantic_identity=("cluster_authorization", profile),
            object_payload=False,
        )
    )
    del authorization_payload
    (
        authenticated_authorization_path,
        authenticated_authorization_raw,
        authorization_metadata,
    ) = _authenticated_file_bytes(
        authorization_path,
        f"{context}.authorization_artifact",
        require_single_link=True,
    )
    if (
        authenticated_authorization_path != authorization_path
        or authenticated_authorization_raw != authorization_raw
        or authorization_raw != AUTHORIZATION_MARKER
        or _artifact_sha256(authorization_raw) != authorization_sha256
        or authorization_sha256 != artifact_sha256
        or stat.S_IMODE(authorization_metadata.st_mode) != 0o600
        or authorization_metadata.st_uid != current_uid
    ):
        _fail(context, "retained authorization marker authority differs")
    return RetainedClusterAuthority(
        profile=profile,
        record_path=record_path,
        record_sha256=_sha256(
            _field(matrix, record_hash_field, "runtime-matrix"),
            f"runtime-matrix.{record_hash_field}",
        ),
        cluster_system_identifier=system_identifier,
        data_directory=data_directory,
        socket_directory=socket_directory,
        connection_port=connection_port,
        connection_user=connection_user,
        original_authorization_path=original_authorization_path,
        authorization_artifact_path=authorization_path,
        authorization_sha256=authorization_sha256,
        authorization_mode=mode,
        authorization_uid=uid,
    )


def _validate_cluster_config_authority(
    config: Mapping[str, Any],
    *,
    profile: str,
    candidate_root: Path,
    stage: StageManifestBinding,
    stage_manifest_sha256: str,
    tool_signature_suite_sha256: str,
    runtime_control_dso_path: Path,
    authority: RetainedClusterAuthority,
    context: str,
) -> ClusterConfigAuthority:
    _require_exact_fields(config, CLUSTER_CONFIG_FIELDS, context)
    if (
        _integer(
            _field(config, "schema_revision", context),
            f"{context}.schema_revision",
        )
        != 1
        or _string(
            _field(config, "profile", context),
            f"{context}.profile",
        )
        != profile
        or not _boolean(
            _field(config, "dedicated_safety_cluster", context),
            f"{context}.dedicated_safety_cluster",
        )
    ):
        _fail(context, "schema, profile, or dedicated-cluster flag differs")
    configured_manifest = _canonical_regular_file(
        _configured_absolute_path(
            _field(config, "stage_manifest_path", context),
            f"{context}.stage_manifest_path",
        ),
        f"{context}.stage_manifest_path",
    )
    configured_stage_root = _canonical_directory(
        _configured_absolute_path(
            _field(config, "private_stage_root", context),
            f"{context}.private_stage_root",
        ),
        f"{context}.private_stage_root",
    )
    if (
        configured_manifest != stage.manifest_path
        or configured_stage_root != stage.stage_root
        or _sha256(
            _field(config, "stage_manifest_sha256", context),
            f"{context}.stage_manifest_sha256",
        )
        != stage_manifest_sha256
        or _sha256(
            _field(config, "tool_signature_suite_sha256", context),
            f"{context}.tool_signature_suite_sha256",
        )
        != tool_signature_suite_sha256
    ):
        _fail(context, "stage manifest or six-tool binding differs")
    _contained_path(stage.stage_root, candidate_root, f"{context}.stage_root")

    socket_directory = _validate_connection_authority(
        _field(config, "connection", context),
        expected_socket_directory=authority.socket_directory,
        expected_port=authority.connection_port,
        expected_user=authority.connection_user,
        context=f"{context}.connection",
    )
    data_directory = (
        _configured_absolute_path(
            _field(config, "expected_data_directory", context),
            f"{context}.expected_data_directory",
        )
    )
    if data_directory != authority.data_directory:
        _fail(context, "cluster data coordinate differs")
    terminal_marker_directory = _configured_absolute_path(
        _field(config, "terminal_marker_directory", context),
        f"{context}.terminal_marker_directory",
    )
    if (
        terminal_marker_directory
        != data_directory / "bioseq-safety-terminal-markers"
    ):
        _fail(
            f"{context}.terminal_marker_directory",
            "terminal marker root is not the retained private-cluster "
            "coordinate",
        )
    server_log_path = _configured_absolute_path(
        _field(config, "server_log_path", context),
        f"{context}.server_log_path",
    )
    if server_log_path != data_directory.parent / "server_startup.log":
        _fail(
            f"{context}.server_log_path",
            "server log is not the exact private-cluster log",
        )
    system_identifier = _string(
        _field(config, "expected_system_identifier", context),
        f"{context}.expected_system_identifier",
    )
    if system_identifier != authority.cluster_system_identifier:
        _fail(
            f"{context}.expected_system_identifier",
            "system identifier differs from retained authority",
        )

    authorization_path = _configured_absolute_path(
        _field(config, "authorization_file", context),
        f"{context}.authorization_file",
    )
    if authorization_path != authority.original_authorization_path:
        _fail(
            f"{context}.authorization_file",
            "original authorization coordinate differs",
        )

    expected_paths = {
        "installed_dso_path": stage.library_path,
        "installed_control_dso_path": (
            stage.pkglibdir / runtime_control_dso_path.name
        ),
        "installed_control_path": stage.control_path,
        "installed_sql_path": stage.install_sql_path,
    }
    authenticated_paths: dict[str, Path] = {}
    for field_name, expected_path in expected_paths.items():
        configured_path = _canonical_regular_file(
            _configured_absolute_path(
                _field(config, field_name, context),
                f"{context}.{field_name}",
            ),
            f"{context}.{field_name}",
        )
        if configured_path != expected_path:
            _fail(
                f"{context}.{field_name}",
                "installed path differs from the manifest-selected path",
            )
        authenticated_paths[field_name] = configured_path

    restart_command = [
        _string(item, f"{context}.restart_command[{index}]")
        for index, item in enumerate(
            _array(
                _field(config, "restart_command", context),
                f"{context}.restart_command",
            )
        )
    ]
    expected_restart = [
        str(stage.pg_ctl_path),
        "--pgdata",
        str(data_directory),
        "--wait",
        "--mode=fast",
        "restart",
    ]
    if restart_command != expected_restart:
        _fail(context, "restart command differs from the selected pg_ctl")
    return ClusterConfigAuthority(
        socket_directory=socket_directory,
        data_directory=data_directory,
        terminal_marker_directory=terminal_marker_directory,
        server_log_path=server_log_path,
        system_identifier=system_identifier,
        active_dso_path=authenticated_paths["installed_dso_path"],
        control_dso_path=authenticated_paths["installed_control_dso_path"],
        installed_control_path=authenticated_paths[
            "installed_control_path"
        ],
        installed_sql_path=authenticated_paths["installed_sql_path"],
    )


def _runtime_source_range(value: Any, context: str) -> None:
    span = _object(value, context)
    _require_exact_fields(span, SOURCE_RANGE_FIELDS, context)
    for endpoint_name in ("begin", "end"):
        endpoint_context = f"{context}.{endpoint_name}"
        endpoint = _object(
            _field(span, endpoint_name, context),
            endpoint_context,
        )
        _require_exact_fields(
            endpoint,
            SOURCE_POSITION_FIELDS,
            endpoint_context,
        )
    _source_span(span, context)


def _runtime_call_path(
    value: Any,
    context: str,
) -> list[dict[str, Any]]:
    raw_steps = _array(value, context)
    if not raw_steps:
        _fail(context, "transitive call path must not be empty")
    result: list[dict[str, Any]] = []
    for index, raw_step in enumerate(raw_steps):
        step_context = f"{context}[{index}]"
        step = _object(raw_step, step_context)
        _require_exact_fields(step, CALL_PATH_FIELDS, step_context)
        for field_name in (
            "usr",
            "qualified_declaration",
            "source_file",
        ):
            _string(
                _field(step, field_name, step_context),
                f"{step_context}.{field_name}",
            )
        _runtime_source_range(
            _field(step, "source_span", step_context),
            f"{step_context}.source_span",
        )
        result.append(dict(step))
    return result


def _runtime_anchor(
    value: Any,
    *,
    expected_kind: str,
    context: str,
) -> dict[str, Any]:
    anchor = _object(value, context)
    _require_exact_fields(anchor, RUNTIME_ANCHOR_FIELDS, context)
    if _string(
        _field(anchor, "kind", context),
        f"{context}.kind",
    ) != expected_kind:
        _fail(context, f"anchor kind must be {expected_kind}")
    for field_name in (
        "compiler_stable_id",
        "enclosing_function_usr",
        "source_file",
    ):
        _string(
            _field(anchor, field_name, context),
            f"{context}.{field_name}",
        )
    _runtime_source_range(
        _field(anchor, "expanded_source_span", context),
        f"{context}.expanded_source_span",
    )
    _sha256(
        _field(anchor, "source_fragment_sha256", context),
        f"{context}.source_fragment_sha256",
    )
    return dict(anchor)


def validate_runtime_inventory(
    inventory: dict[str, Any],
    *,
    profile: str,
    inventory_sha256: str,
    ledger_document: dict[str, Any],
    ledger: dict[str, Any],
    ledger_sha256: str,
    inventory_producer_executable_sha256: str,
) -> dict[str, Any]:
    context = f"{profile}-runtime-inventory"
    if _sha256(
        _field(ledger_document, "producer_executable_sha256", profile),
        f"{profile}.producer_executable_sha256",
    ) != inventory_producer_executable_sha256:
        _fail(
            context,
            "inventory producer differs from the ledger producer executable",
        )
    _require_exact_fields(inventory, RUNTIME_INVENTORY_FIELDS, context)
    if _integer(
        _field(inventory, "schema_revision", context),
        f"{context}.schema_revision",
    ) != 1:
        _fail(context, "unsupported schema revision")
    if _integer(
        _field(inventory, "probe_abi_revision", context),
        f"{context}.probe_abi_revision",
    ) != 2:
        _fail(context, "unsupported runtime probe ABI")
    if _string(
        _field(inventory, "profile", context),
        f"{context}.profile",
    ) != profile:
        _fail(context, "profile differs from its command-line role")
    expected_hashes = {
        "ledger_sha256": ledger_sha256,
        "compiler_facts_sha256": ledger_document[
            "compiler_facts_sha256"
        ],
        "sql_manifest_sha256": ledger["sql_manifest_sha256"],
        "source_list_sha256": ledger_document[
            "source_list_sha256"
        ],
        "compile_database_sha256": ledger_document[
            "compile_database_sha256"
        ],
        "compile_graph_sha256": ledger_document[
            "compile_graph_sha256"
        ],
        "pcm_manifest_sha256": ledger_document[
            "pcm_manifest_sha256"
        ],
        "inventory_producer_executable_sha256": (
            inventory_producer_executable_sha256
        ),
    }
    for field_name, expected in expected_hashes.items():
        if _sha256(
            _field(inventory, field_name, context),
            f"{context}.{field_name}",
        ) != expected:
            _fail(context, f"{field_name} is stale")

    entries: dict[tuple[str, str], dict[str, Any]] = {}
    site_ids: set[str] = set()
    stable_anchor_ids: set[str] = set()
    for index, raw_entry in enumerate(
        _array(_field(inventory, "entries", context), f"{context}.entries")
    ):
        entry_context = f"{context}.entries[{index}]"
        entry = _object(raw_entry, entry_context)
        _require_exact_fields(
            entry,
            RUNTIME_INVENTORY_ENTRY_FIELDS,
            entry_context,
        )
        sql_identity = _string(
            _field(entry, "sql_identity", entry_context),
            f"{entry_context}.sql_identity",
        )
        fault_key = _fault_key(
            _field(entry, "fault_key", entry_context),
            f"{entry_context}.fault_key",
        )
        key = (sql_identity, fault_key)
        if key in entries:
            _fail(entry_context, "duplicate SQL identity/fault key")
        site_id = _string(
            _field(entry, "site_id", entry_context),
            f"{entry_context}.site_id",
        )
        if _integer(
            _field(entry, "site_revision", entry_context),
            f"{entry_context}.site_revision",
        ) != 2:
            _fail(entry_context, "unsupported probe site revision")
        if (
            PROBE_SITE_ID_PATTERN.fullmatch(site_id) is None
            or site_id != _probe_site_id(sql_identity, fault_key)
            or site_id in site_ids
        ):
            _fail(entry_context, "site_id is not the exact sealed ABI revision 2 ID")
        site_ids.add(site_id)
        relation = _string(
            _field(entry, "relation", entry_context),
            f"{entry_context}.relation",
        )
        probe_contract = _validate_probe_contract(
            _field(entry, "probe_contract", entry_context),
            relation,
            f"{entry_context}.probe_contract",
        )
        ledger_site_sha256 = _sha256(
            _field(entry, "ledger_site_sha256", entry_context),
            f"{entry_context}.ledger_site_sha256",
        )
        parent = ledger["parents"].get(sql_identity)
        candidate_sites = [] if parent is None else [
            site
            for site in parent["sites"]
            if _artifact_sha256(_canonical_json_bytes(site))
                == ledger_site_sha256
        ]
        if len(candidate_sites) != 1:
            _fail(
                entry_context,
                "ledger_site_sha256 does not select one exact ledger site",
            )
        ledger_site = candidate_sites[0]
        site_anchor = _runtime_anchor(
            _field(entry, "site_anchor", entry_context),
            expected_kind="postgres_error_site",
            context=f"{entry_context}.site_anchor",
        )
        visit_anchor = _runtime_anchor(
            _field(entry, "visit_anchor", entry_context),
            expected_kind="record_site_visit",
            context=f"{entry_context}.visit_anchor",
        )
        raw_trigger = _field(entry, "trigger_anchor", entry_context)
        trigger_anchor: dict[str, Any] | None
        if relation == "natural_at_site":
            if raw_trigger is not None:
                _fail(
                    entry_context,
                    "natural relation requires a null trigger anchor",
                )
            trigger_anchor = None
        else:
            trigger_anchor = _runtime_anchor(
                raw_trigger,
                expected_kind="consume_fault",
                context=f"{entry_context}.trigger_anchor",
            )
        anchors = [site_anchor, visit_anchor]
        if trigger_anchor is not None:
            anchors.append(trigger_anchor)
        anchor_ids = [
            anchor["compiler_stable_id"] for anchor in anchors
        ]
        if (
            len(anchor_ids) != len(set(anchor_ids))
            or any(anchor_id in stable_anchor_ids for anchor_id in anchor_ids)
        ):
            _fail(entry_context, "runtime anchor identity is duplicated")
        stable_anchor_ids.update(anchor_ids)
        if (
            site_anchor["compiler_stable_id"] != ledger_site["site_id"]
            or site_anchor["source_file"] != ledger_site["source_file"]
            or site_anchor["expanded_source_span"]
                != ledger_site["expanded_source_span"]
            or any(
                anchor["enclosing_function_usr"]
                    != site_anchor["enclosing_function_usr"]
                for anchor in anchors
            )
        ):
            _fail(
                entry_context,
                "runtime anchors differ from the exact ledger site",
            )
        call_path = _runtime_call_path(
            _field(entry, "transitive_call_path", entry_context),
            f"{entry_context}.transitive_call_path",
        )
        if (
            call_path != ledger_site["transitive_call_path"]
            or site_anchor["enclosing_function_usr"] not in {
                step["usr"] for step in call_path
            }
        ):
            _fail(
                entry_context,
                "transitive call path differs from the exact ledger site",
            )
        entries[key] = {
            "sql_identity": sql_identity,
            "fault_key": fault_key,
            "site_id": site_id,
            "relation": relation,
            "probe_contract": probe_contract,
            "ledger_site_sha256": ledger_site_sha256,
            "ledger_site": ledger_site,
        }
    if not entries:
        _fail(context, "runtime inventory is empty")
    return {
        "profile": profile,
        "sha256": inventory_sha256,
        "entries": entries,
    }


def validate_runtime_matrix(
    matrix: dict[str, Any],
    production: dict[str, Any],
    test_indexes: dict[str, Any],
    production_inventory: dict[str, Any],
    test_indexes_inventory: dict[str, Any],
    policy_coverage: set[tuple[str, str, str]],
    *,
    production_sha256: str,
    test_indexes_sha256: str,
    production_sql_manifest_sha256: str,
    test_indexes_sql_manifest_sha256: str,
    production_case_plan_sha256: str,
    test_indexes_case_plan_sha256: str,
    production_runtime_inventory_sha256: str,
    test_indexes_runtime_inventory_sha256: str,
    runtime_policy_sha256: str,
    production_dso_sha256: str,
    test_indexes_dso_sha256: str,
    runtime_control_dso_sha256: str,
    runtime_producer_executable_sha256: str,
    production_test_executable_sha256: str,
    test_indexes_test_executable_sha256: str,
    production_ledger_path: Path,
    test_indexes_ledger_path: Path,
    production_sql_manifest_path: Path,
    test_indexes_sql_manifest_path: Path,
    production_sql_script_sha256: str,
    test_indexes_sql_script_sha256: str,
    production_case_plan_path: Path,
    test_indexes_case_plan_path: Path,
    production_stage_manifest_path: Path,
    test_indexes_stage_manifest_path: Path,
    production_dso_path: Path,
    test_indexes_dso_path: Path,
    runtime_control_dso_path: Path,
    runtime_producer_executable_path: Path,
    production_test_executable_path: Path,
    test_indexes_test_executable_path: Path,
    runtime_matrix_path: Path,
    expected_matrix_case_count: int = EXPECTED_MATRIX_CASE_COUNT,
    expected_profile_case_count: int = EXPECTED_PROFILE_CASE_COUNT,
) -> dict[str, Any]:
    candidate_root = _candidate_root(runtime_matrix_path)
    canonical_expected_paths = {
        "production_ledger": _canonical_regular_file(
            production_ledger_path,
            "production-ledger",
        ),
        "test_indexes_ledger": _canonical_regular_file(
            test_indexes_ledger_path,
            "test-indexes-ledger",
        ),
        "production_sql_manifest": _canonical_regular_file(
            production_sql_manifest_path,
            "production-sql-manifest",
        ),
        "test_indexes_sql_manifest": _canonical_regular_file(
            test_indexes_sql_manifest_path,
            "test-indexes-sql-manifest",
        ),
        "production_case_plan": _canonical_regular_file(
            production_case_plan_path,
            "production-case-plan",
        ),
        "test_indexes_case_plan": _canonical_regular_file(
            test_indexes_case_plan_path,
            "test-indexes-case-plan",
        ),
        "production_stage_manifest": _canonical_regular_file(
            production_stage_manifest_path,
            "production-stage-manifest",
        ),
        "test_indexes_stage_manifest": _canonical_regular_file(
            test_indexes_stage_manifest_path,
            "test-indexes-stage-manifest",
        ),
        "production_dso": _canonical_regular_file(
            production_dso_path,
            "production-profile-dso",
        ),
        "test_indexes_dso": _canonical_regular_file(
            test_indexes_dso_path,
            "test-indexes-profile-dso",
        ),
        "runtime_control_dso": _canonical_regular_file(
            runtime_control_dso_path,
            "runtime-control-dso",
        ),
        "runtime_producer": _canonical_regular_file(
            runtime_producer_executable_path,
            "runtime-producer-executable",
        ),
        "production_test_executable": _canonical_regular_file(
            production_test_executable_path,
            "production-runtime-test-executable",
        ),
        "test_indexes_test_executable": _canonical_regular_file(
            test_indexes_test_executable_path,
            "test-indexes-runtime-test-executable",
        ),
    }
    if (
        canonical_expected_paths["production_dso"]
        == canonical_expected_paths["test_indexes_dso"]
        or production_dso_sha256 == test_indexes_dso_sha256
    ):
        _fail(
            "profile-artifacts",
            "production/test_indexes DSO paths and bytes must differ",
        )
    if (
        canonical_expected_paths["production_test_executable"]
        == canonical_expected_paths["test_indexes_test_executable"]
        or production_test_executable_sha256
        == test_indexes_test_executable_sha256
    ):
        _fail(
            "profile-artifacts",
            "production/test_indexes harness paths and bytes must differ",
        )
    expected_path_hashes = {
        "production_dso": production_dso_sha256,
        "test_indexes_dso": test_indexes_dso_sha256,
        "runtime_control_dso": runtime_control_dso_sha256,
        "runtime_producer": runtime_producer_executable_sha256,
        "production_test_executable": production_test_executable_sha256,
        "test_indexes_test_executable": test_indexes_test_executable_sha256,
    }
    for path_name, expected_hash in expected_path_hashes.items():
        if _path_artifact_sha256(
            canonical_expected_paths[path_name],
            path_name,
        ) != expected_hash:
            _fail(path_name, "current artifact hash differs")
    production_manifest, production_manifest_raw = load_json(
        canonical_expected_paths["production_sql_manifest"]
    )
    test_indexes_manifest, test_indexes_manifest_raw = load_json(
        canonical_expected_paths["test_indexes_sql_manifest"]
    )
    if (
        _artifact_sha256(production_manifest_raw)
        != production_sql_manifest_sha256
        or _artifact_sha256(test_indexes_manifest_raw)
        != test_indexes_sql_manifest_sha256
    ):
        _fail(
            "runtime-matrix",
            "current SQL-manifest bytes differ from the matrix inputs",
        )
    guc_generations = {
        "production": _validate_guc_manifest_contract(
            production_manifest,
            profile="production",
            context="production-sql-manifest",
        ),
        "test_indexes": _validate_guc_manifest_contract(
            test_indexes_manifest,
            profile="test_indexes",
            context="test-indexes-sql-manifest",
        ),
    }
    if runtime_control_dso_sha256 in {
        production_dso_sha256,
        test_indexes_dso_sha256,
    }:
        _fail(
            "runtime-control-dso",
            "runtime control helper bytes must differ from profile DSOs",
        )
    production_stage = _parse_stage_manifest_binding(
        canonical_expected_paths["production_stage_manifest"],
        profile="production",
        expected_dso_sha256=production_dso_sha256,
        expected_sql_script_sha256=production_sql_script_sha256,
    )
    test_indexes_stage = _parse_stage_manifest_binding(
        canonical_expected_paths["test_indexes_stage_manifest"],
        profile="test_indexes",
        expected_dso_sha256=test_indexes_dso_sha256,
        expected_sql_script_sha256=test_indexes_sql_script_sha256,
    )
    production_stage_sha256 = production_stage.manifest_sha256
    production_suite_sha256 = (
        production_stage.tool_signature_suite_sha256
    )
    test_indexes_stage_sha256 = test_indexes_stage.manifest_sha256
    test_indexes_suite_sha256 = (
        test_indexes_stage.tool_signature_suite_sha256
    )
    if (
        canonical_expected_paths["production_stage_manifest"]
        == canonical_expected_paths["test_indexes_stage_manifest"]
        or production_stage_sha256 == test_indexes_stage_sha256
    ):
        _fail(
            "profile-artifacts",
            "profile stage-manifest paths and bytes must differ",
        )
    production_plan = _parse_case_plan(
        canonical_expected_paths["production_case_plan"],
        profile="production",
        expected_sha256=production_case_plan_sha256,
        ledger_sha256=production_sha256,
        runtime_inventory_sha256=production_runtime_inventory_sha256,
        sql_manifest_sha256=production_sql_manifest_sha256,
        runtime_policy_sha256=runtime_policy_sha256,
        inventory=production_inventory,
        expected_case_count=expected_profile_case_count,
    )
    test_indexes_plan = _parse_case_plan(
        canonical_expected_paths["test_indexes_case_plan"],
        profile="test_indexes",
        expected_sha256=test_indexes_case_plan_sha256,
        ledger_sha256=test_indexes_sha256,
        runtime_inventory_sha256=test_indexes_runtime_inventory_sha256,
        sql_manifest_sha256=test_indexes_sql_manifest_sha256,
        runtime_policy_sha256=runtime_policy_sha256,
        inventory=test_indexes_inventory,
        expected_case_count=expected_profile_case_count,
    )
    _require_exact_fields(
        matrix,
        RUNTIME_MATRIX_FIELDS,
        "runtime-matrix",
    )
    if _integer(
        _field(matrix, "schema_revision", "runtime-matrix"),
        "runtime-matrix.schema_revision",
    ) != 2:
        _fail("runtime-matrix", "unsupported schema revision")
    expected_hashes = {
        "production_ledger_sha256": production_sha256,
        "test_indexes_ledger_sha256": test_indexes_sha256,
        "production_sql_manifest_sha256": (
            production_sql_manifest_sha256
        ),
        "test_indexes_sql_manifest_sha256": (
            test_indexes_sql_manifest_sha256
        ),
        "production_case_plan_sha256": production_case_plan_sha256,
        "test_indexes_case_plan_sha256": test_indexes_case_plan_sha256,
        "production_runtime_inventory_sha256": (
            production_runtime_inventory_sha256
        ),
        "test_indexes_runtime_inventory_sha256": (
            test_indexes_runtime_inventory_sha256
        ),
        "runtime_policy_sha256": runtime_policy_sha256,
        "production_stage_manifest_sha256": (
            production_stage_sha256
        ),
        "test_indexes_stage_manifest_sha256": (
            test_indexes_stage_sha256
        ),
        "production_tool_signature_suite_sha256": (
            production_suite_sha256
        ),
        "test_indexes_tool_signature_suite_sha256": (
            test_indexes_suite_sha256
        ),
        "production_dso_sha256": production_dso_sha256,
        "test_indexes_dso_sha256": test_indexes_dso_sha256,
        "runtime_control_dso_sha256": runtime_control_dso_sha256,
        "runtime_producer_executable_sha256": (
            runtime_producer_executable_sha256
        ),
        "production_test_executable_sha256": (
            production_test_executable_sha256
        ),
        "test_indexes_test_executable_sha256": (
            test_indexes_test_executable_sha256
        ),
    }
    for field_name, expected in expected_hashes.items():
        if _sha256(
            _field(matrix, field_name, "runtime-matrix"),
            f"runtime-matrix.{field_name}",
        ) != expected:
            _fail("runtime-matrix", f"{field_name} is stale")
    if (
        production["sql_manifest_sha256"]
            != production_sql_manifest_sha256
        or test_indexes["sql_manifest_sha256"]
            != test_indexes_sql_manifest_sha256
    ):
        _fail(
            "runtime-matrix",
            "ledger SQL-manifest binding differs from the runtime "
            "artifact",
        )

    required = derive_ledger_coverage(production, test_indexes) | policy_coverage
    observed: set[tuple[str, str, str]] = set()
    case_ids: set[str] = set()
    seen_profiles: set[str] = set()
    invocation_fault_keys: set[tuple[str, str, str]] = set()
    observed_plan_cases: dict[str, set[str]] = {
        "production": set(),
        "test_indexes": set(),
    }
    artifact_identities: dict[
        tuple[int, int],
        tuple[Any, ...],
    ] = {}
    retained_authorities = {
        profile: _load_retained_cluster_authority(
            matrix=matrix,
            profile=profile,
            candidate_root=candidate_root,
            artifact_identities=artifact_identities,
        )
        for profile in ("production", "test_indexes")
    }
    production_authority = retained_authorities["production"]
    test_indexes_authority = retained_authorities["test_indexes"]
    if (
        production_authority.record_path
        == test_indexes_authority.record_path
        or production_authority.cluster_system_identifier
        == test_indexes_authority.cluster_system_identifier
        or production_authority.data_directory
        == test_indexes_authority.data_directory
        or production_authority.socket_directory
        == test_indexes_authority.socket_directory
        or production_authority.original_authorization_path
        == test_indexes_authority.original_authorization_path
    ):
        _fail(
            "runtime-matrix",
            "profile cluster authority coordinates must be distinct",
        )
    profile_catalogs: dict[str, tuple[Path, str]] = {}
    profile_activations: dict[str, tuple[Path, str]] = {}
    profile_guc_properties: dict[str, tuple[list[dict[str, Any]], str]] = {}
    profile_configs: dict[str, tuple[Path, str]] = {}
    profile_cluster_bindings: dict[str, tuple[Any, ...]] = {}
    cases = _array(_field(matrix, "cases", "runtime-matrix"), "runtime-matrix.cases")
    if len(cases) != expected_matrix_case_count:
        _fail(
            "runtime-matrix",
            f"requires exactly {expected_matrix_case_count} cases",
        )
    for index, raw_case in enumerate(cases):
        context = f"runtime-matrix.cases[{index}]"
        test_case = _object(raw_case, context)
        _require_exact_fields(
            test_case,
            RUNTIME_MATRIX_CASE_FIELDS,
            context,
        )
        case_id = _string(_field(test_case, "case_id", context), f"{context}.case_id")
        if case_id in case_ids:
            _fail(case_id, "duplicate runtime case")
        case_ids.add(case_id)
        profile = _string(
            _field(test_case, "profile", case_id),
            f"{case_id}.profile",
        )
        if profile not in PROFILES:
            _fail(case_id, "unknown runtime profile")
        seen_profiles.add(profile)
        active_inventory = (
            production_inventory
            if profile == "production"
            else test_indexes_inventory
        )
        active_plan = (
            production_plan
            if profile == "production"
            else test_indexes_plan
        )
        plan_case = active_plan.get(case_id)
        if plan_case is None:
            _fail(case_id, "runtime row is absent from the current case plan")
        observed_plan_cases[profile].add(case_id)
        if _integer(
            _field(test_case, "probe_abi_revision", case_id),
            f"{case_id}.probe_abi_revision",
        ) != 2:
            _fail(case_id, "unsupported runtime probe ABI")
        sql_identity = _string(
            _field(test_case, "sql_identity", case_id),
            f"{case_id}.sql_identity",
        )
        fault_key = _fault_key(
            _field(test_case, "fault_key", case_id),
            f"{case_id}.fault_key",
        )
        inventory_entry = active_inventory["entries"].get(
            (sql_identity, fault_key)
        )
        if inventory_entry is None:
            _fail(
                case_id,
                "runtime row is absent from the current runtime inventory",
            )
        site_id = _string(
            _field(test_case, "site_id", case_id),
            f"{case_id}.site_id",
        )
        exact_inventory_fields = {
            "site_id": inventory_entry["site_id"],
            "ledger_site_sha256": inventory_entry[
                "ledger_site_sha256"
            ],
            "relation": inventory_entry["relation"],
        }
        for field_name, expected_value in exact_inventory_fields.items():
            if _field(test_case, field_name, case_id) != expected_value:
                _fail(
                    case_id,
                    f"{field_name} differs from the runtime inventory",
                )
        matrix_contract = _validate_probe_contract(
            _field(test_case, "probe_contract", case_id),
            inventory_entry["relation"],
            f"{case_id}.probe_contract",
        )
        if matrix_contract != inventory_entry["probe_contract"]:
            _fail(
                case_id,
                "probe_contract differs from the runtime inventory",
            )
        planned_catalog = plan_case["catalog"]
        planned_matrix_fields = {
            "case_id": case_id,
            "sql_identity": planned_catalog["sql_identity"],
            "site_id": planned_catalog["site_id"],
            "ledger_site_sha256": planned_catalog[
                "ledger_site_sha256"
            ],
            "invocation_key": planned_catalog["invocation_key"],
            "fault_key": planned_catalog["fault_key"],
            "relation": planned_catalog["relation"],
            "probe_contract": planned_catalog["probe_contract"],
            "expected_backend_outcome": plan_case[
                "expected_backend_outcome"
            ],
            "expected_destructor_sentinels": plan_case[
                "expected_destructor_sentinels"
            ],
            "expected_cleanup_events": plan_case[
                "expected_cleanup_events"
            ],
            "expected_postgres_cleanup_events": plan_case[
                "expected_postgres_cleanup_events"
            ],
            "expected_sqlstate": plan_case["expected_sqlstate"],
            "expected_message": plan_case["expected_message"],
            "expected_recovery_postcondition": plan_case[
                "expected_recovery_postcondition"
            ],
        }
        for field_name, expected_value in planned_matrix_fields.items():
            actual_value = _field(test_case, field_name, case_id)
            if field_name.startswith("expected_") and isinstance(
                expected_value,
                list,
            ):
                actual_value = sorted(
                    _runtime_event_list(
                        actual_value,
                        f"{case_id}.{field_name}",
                        allow_empty=True,
                    )
                )
            if actual_value != expected_value:
                _fail(
                    case_id,
                    f"{field_name} differs from the current case plan",
                )
        ledger_site = inventory_entry["ledger_site"]
        exact_site_coverage = derive_site_coverage(
            profile,
            ledger_site,
        )
        probe_contract = inventory_entry["probe_contract"]
        backend_exit = (
            plan_case["expected_backend_outcome"] == "backend_exit"
        )
        ordering = probe_contract["ordering"]
        expected_visit_count = (
            0 if backend_exit else probe_contract["expected_visit_count"]
        )
        expected_trigger_count = (
            0 if backend_exit else probe_contract["expected_trigger_count"]
        )
        expected_visit_sequence = (
            0
            if backend_exit
            else (2 if ordering == "trigger_before_visit" else 1)
        )
        expected_trigger_sequence = (
            0
            if backend_exit
            else {
                "visit_only": 0,
                "trigger_before_visit": 1,
                "visit_before_trigger": 2,
            }[ordering]
        )
        exact_causal_fields = {
            "site_visit_count": expected_visit_count,
            "fault_trigger_count": expected_trigger_count,
            "site_visit_sequence": expected_visit_sequence,
            "fault_trigger_sequence": expected_trigger_sequence,
        }
        for field_name, expected_value in exact_causal_fields.items():
            if _integer(
                _field(test_case, field_name, case_id),
                f"{case_id}.{field_name}",
            ) != expected_value:
                _fail(
                    case_id,
                    f"{field_name} differs from the ABI revision 2 probe contract",
                )
        invocation_key = _string(
            _field(test_case, "invocation_key", case_id),
            f"{case_id}.invocation_key",
        )
        invocation_fault_key = (
            profile,
            invocation_key,
            fault_key,
        )
        if invocation_fault_key in invocation_fault_keys:
            _fail(case_id, "duplicate profile/invocation/fault key")
        invocation_fault_keys.add(invocation_fault_key)
        row_hashes = (
            {
                "case_plan_sha256": production_case_plan_sha256,
                "ledger_sha256": production_sha256,
                "sql_manifest_sha256":
                    production_sql_manifest_sha256,
                "active_dso_sha256": production_dso_sha256,
                "test_executable_sha256":
                    production_test_executable_sha256,
            }
            if profile == "production"
            else {
                "case_plan_sha256": test_indexes_case_plan_sha256,
                "ledger_sha256": test_indexes_sha256,
                "sql_manifest_sha256":
                    test_indexes_sql_manifest_sha256,
                "active_dso_sha256": test_indexes_dso_sha256,
                "test_executable_sha256":
                    test_indexes_test_executable_sha256,
            }
        )
        row_hashes["producer_executable_sha256"] = (
            runtime_producer_executable_sha256
        )
        for field_name, expected in row_hashes.items():
            if _sha256(
                _field(test_case, field_name, case_id),
                f"{case_id}.{field_name}",
            ) != expected:
                _fail(
                    case_id,
                    f"{field_name} does not bind this runtime row "
                    "to the reviewed artifacts",
                )
        immutable_result_sha256 = _sha256(
            _field(test_case, "immutable_result_sha256", case_id),
            f"{case_id}.immutable_result_sha256",
        )
        result_payload, result_raw, result_path = (
            _load_candidate_artifact(
                candidate_root=candidate_root,
                path_text=_field(
                    test_case,
                    "immutable_result_path",
                    case_id,
                ),
                category="results",
                profile=profile,
                stem=case_id,
                suffix="json",
                expected_sha256=immutable_result_sha256,
                context=f"{case_id}.immutable_result",
                artifact_identities=artifact_identities,
                semantic_identity=("result", profile, case_id),
            )
        )
        immutable_log_sha256 = _sha256(
            _field(test_case, "immutable_test_log_sha256", case_id),
            f"{case_id}.immutable_test_log_sha256",
        )
        log_payload, log_raw, log_path = _load_candidate_artifact(
            candidate_root=candidate_root,
            path_text=_field(
                test_case,
                "immutable_test_log_path",
                case_id,
            ),
            category="logs",
            profile=profile,
            stem=case_id,
            suffix="log.json",
            expected_sha256=immutable_log_sha256,
            context=f"{case_id}.immutable_test_log",
            artifact_identities=artifact_identities,
            semantic_identity=("log", profile, case_id),
        )
        assert isinstance(result_payload, dict)
        assert isinstance(log_payload, dict)
        _require_exact_fields(
            result_payload,
            IMMUTABLE_RUNTIME_RESULT_FIELDS,
            f"{case_id}.immutable_result",
        )
        if _integer(
            _field(
                result_payload,
                "schema_revision",
                f"{case_id}.immutable_result",
            ),
            f"{case_id}.immutable_result.schema_revision",
        ) != 2:
            _fail(case_id, "immutable runtime result schema is unsupported")
        attestation = _object(
            _field(
                result_payload,
                "attestation",
                f"{case_id}.immutable_result",
            ),
            f"{case_id}.immutable_result.attestation",
        )
        _require_exact_fields(
            attestation,
            RUNTIME_ATTESTATION_FIELDS,
            f"{case_id}.immutable_result.attestation",
        )
        if _integer(
            _field(
                attestation,
                "schema_revision",
                f"{case_id}.immutable_result.attestation",
            ),
            f"{case_id}.immutable_result.attestation.schema_revision",
        ) != 1:
            _fail(case_id, "runtime attestation schema is unsupported")
        request_sha256 = _sha256(
            _field(attestation, "request_sha256", case_id),
            f"{case_id}.attestation.request_sha256",
        )
        request_payload, request_raw, request_path = (
            _load_candidate_artifact(
                candidate_root=candidate_root,
                path_text=_field(attestation, "request_path", case_id),
                category="requests",
                profile=profile,
                stem=case_id,
                suffix="json",
                expected_sha256=request_sha256,
                context=f"{case_id}.request",
                artifact_identities=artifact_identities,
                semantic_identity=("request", profile, case_id),
            )
        )
        catalog_sha256 = _sha256(
            _field(attestation, "catalog_sha256", case_id),
            f"{case_id}.attestation.catalog_sha256",
        )
        catalog_payload, catalog_raw, catalog_path = (
            _load_candidate_artifact(
                candidate_root=candidate_root,
                path_text=_field(attestation, "catalog_path", case_id),
                category="catalogs",
                profile=profile,
                stem="case_catalog",
                suffix="json",
                expected_sha256=catalog_sha256,
                context=f"{case_id}.catalog",
                artifact_identities=artifact_identities,
                semantic_identity=("catalog", profile),
            )
        )
        activation_sha256 = _sha256(
            _field(
                attestation,
                "activation_receipt_sha256",
                case_id,
            ),
            f"{case_id}.attestation.activation_receipt_sha256",
        )
        activation_payload, activation_raw, activation_path = (
            _load_candidate_artifact(
                candidate_root=candidate_root,
                path_text=_field(
                    attestation,
                    "activation_receipt_path",
                    case_id,
                ),
                category="activations",
                profile=profile,
                stem="activation",
                suffix="json",
                expected_sha256=activation_sha256,
                context=f"{case_id}.activation_receipt",
                artifact_identities=artifact_identities,
                semantic_identity=("activation", profile),
            )
        )
        cluster_config_sha256 = _sha256(
            _field(attestation, "cluster_config_sha256", case_id),
            f"{case_id}.attestation.cluster_config_sha256",
        )
        config_payload, config_raw, config_path = (
            _load_candidate_artifact(
                candidate_root=candidate_root,
                path_text=_field(
                    attestation,
                    "cluster_config_path",
                    case_id,
                ),
                category="cluster_configs",
                profile=profile,
                stem="cluster_config",
                suffix="json",
                expected_sha256=cluster_config_sha256,
                context=f"{case_id}.cluster_config",
                artifact_identities=artifact_identities,
                semantic_identity=("cluster_config", profile),
            )
        )
        raw_result_sha256 = _sha256(
            _field(attestation, "raw_result_sha256", case_id),
            f"{case_id}.attestation.raw_result_sha256",
        )
        raw_payload, raw_result_raw, raw_result_path = (
            _load_candidate_artifact(
                candidate_root=candidate_root,
                path_text=_field(
                    attestation,
                    "raw_result_path",
                    case_id,
                ),
                category="raw_results",
                profile=profile,
                stem=case_id,
                suffix="json",
                expected_sha256=raw_result_sha256,
                context=f"{case_id}.raw_result",
                artifact_identities=artifact_identities,
                semantic_identity=("raw_result", profile, case_id),
            )
        )
        recovery_artifact_sha256 = _sha256(
            _field(
                attestation,
                "recovery_artifact_sha256",
                case_id,
            ),
            f"{case_id}.attestation.recovery_artifact_sha256",
        )
        recovery_payload, recovery_raw, recovery_path = (
            _load_candidate_artifact(
                candidate_root=candidate_root,
                path_text=_field(
                    attestation,
                    "recovery_artifact_path",
                    case_id,
                ),
                category="recoveries",
                profile=profile,
                stem=case_id,
                suffix="json",
                expected_sha256=recovery_artifact_sha256,
                context=f"{case_id}.recovery_artifact",
                artifact_identities=artifact_identities,
                semantic_identity=("recovery", profile, case_id),
                object_payload=False,
            )
        )
        assert isinstance(request_payload, dict)
        assert isinstance(catalog_payload, dict)
        assert isinstance(activation_payload, dict)
        assert isinstance(config_payload, dict)
        assert isinstance(raw_payload, dict)

        profile_catalog = (catalog_path, catalog_sha256)
        if profile_catalogs.setdefault(profile, profile_catalog) != (
            profile_catalog
        ):
            _fail(case_id, "profile cases use different catalogs")
        profile_activation = (activation_path, activation_sha256)
        if profile_activations.setdefault(profile, profile_activation) != (
            profile_activation
        ):
            _fail(case_id, "profile cases use different activations")
        profile_config = (config_path, cluster_config_sha256)
        if profile_configs.setdefault(profile, profile_config) != (
            profile_config
        ):
            _fail(case_id, "profile cases use different cluster configs")

        _require_exact_fields(
            catalog_payload,
            CATALOG_FIELDS,
            f"{case_id}.catalog",
        )
        active_test_executable_sha256 = (
            production_test_executable_sha256
            if profile == "production"
            else test_indexes_test_executable_sha256
        )
        active_case_plan_sha256 = (
            production_case_plan_sha256
            if profile == "production"
            else test_indexes_case_plan_sha256
        )
        if (
            _integer(
                _field(catalog_payload, "schema_revision", case_id),
                f"{case_id}.catalog.schema_revision",
            )
            != 2
            or _string(
                _field(catalog_payload, "profile", case_id),
                f"{case_id}.catalog.profile",
            )
            != profile
            or _sha256(
                _field(catalog_payload, "case_plan_sha256", case_id),
                f"{case_id}.catalog.case_plan_sha256",
            )
            != active_case_plan_sha256
            or _sha256(
                _field(
                    catalog_payload,
                    "test_executable_sha256",
                    case_id,
                ),
                f"{case_id}.catalog.test_executable_sha256",
            )
            != active_test_executable_sha256
        ):
            _fail(case_id, "catalog profile/artifact binding differs")
        catalog_entries: dict[str, dict[str, Any]] = {}
        for catalog_index, raw_catalog_entry in enumerate(
            _array(
                _field(catalog_payload, "cases", case_id),
                f"{case_id}.catalog.cases",
            )
        ):
            catalog_entry_context = (
                f"{case_id}.catalog.cases[{catalog_index}]"
            )
            catalog_entry = _object(
                raw_catalog_entry,
                catalog_entry_context,
            )
            _require_exact_fields(
                catalog_entry,
                CATALOG_ENTRY_FIELDS,
                catalog_entry_context,
            )
            catalog_case_id = _string(
                _field(
                    catalog_entry,
                    "case_id",
                    catalog_entry_context,
                ),
                f"{catalog_entry_context}.case_id",
            )
            if (
                catalog_case_id in catalog_entries
                or catalog_case_id not in active_plan
                or catalog_entry
                != active_plan[catalog_case_id]["catalog"]
            ):
                _fail(
                    catalog_entry_context,
                    "catalog entry differs from the current case plan",
                )
            catalog_entries[catalog_case_id] = catalog_entry
        if set(catalog_entries) != set(active_plan):
            _fail(case_id, "catalog case set differs from current case plan")
        catalog_entry_sha256 = _artifact_sha256(
            _canonical_json_bytes(planned_catalog)
        )

        active_stage = (
            production_stage
            if profile == "production"
            else test_indexes_stage
        )
        active_stage_manifest_path = active_stage.manifest_path
        active_stage_manifest_sha256 = active_stage.manifest_sha256
        active_suite_sha256 = (
            active_stage.tool_signature_suite_sha256
        )
        active_sql_script_sha256 = (
            production_sql_script_sha256
            if profile == "production"
            else test_indexes_sql_script_sha256
        )
        active_dso_sha256 = (
            production_dso_sha256
            if profile == "production"
            else test_indexes_dso_sha256
        )
        config_authority = _validate_cluster_config_authority(
            config_payload,
            profile=profile,
            candidate_root=candidate_root,
            stage=active_stage,
            stage_manifest_sha256=active_stage_manifest_sha256,
            tool_signature_suite_sha256=active_suite_sha256,
            runtime_control_dso_path=canonical_expected_paths[
                "runtime_control_dso"
            ],
            authority=retained_authorities[profile],
            context=f"{case_id}.cluster_config",
        )
        cluster_data_directory = config_authority.data_directory
        cluster_system_identifier = config_authority.system_identifier
        active_dso_path = config_authority.active_dso_path
        control_dso_path = config_authority.control_dso_path
        installed_control_path = config_authority.installed_control_path
        installed_sql_path = config_authority.installed_sql_path
        if (
            _path_artifact_sha256(active_dso_path, case_id)
            != active_dso_sha256
            or _path_artifact_sha256(control_dso_path, case_id)
            != runtime_control_dso_sha256
            or _path_artifact_sha256(installed_sql_path, case_id)
            != active_sql_script_sha256
            or active_dso_path == control_dso_path
        ):
            _fail(case_id, "installed DSO/control bytes differ")

        _require_exact_fields(
            activation_payload,
            ACTIVATION_RECEIPT_FIELDS,
            f"{case_id}.activation_receipt",
        )
        expected_guc_counts = (
            (GUC_PROFILE_TOTALS["production"], 0)
            if profile == "production"
            else (GUC_PROFILE_TOTALS["test_indexes"], 3)
        )
        activation_exact_fields: dict[str, Any] = {
            "schema_revision": 1,
            "profile": profile,
            "case_plan_sha256": active_case_plan_sha256,
            "test_executable_sha256": active_test_executable_sha256,
            "requested_dso_sha256": active_dso_sha256,
            "cluster_config_sha256": cluster_config_sha256,
            "active_dso_path": str(active_dso_path),
            "active_dso_sha256": active_dso_sha256,
            "cluster_system_identifier": cluster_system_identifier,
            "cluster_data_directory": str(cluster_data_directory),
            "server_version_num": PG17_SERVER_VERSION_NUM,
            "bioseq_guc_count": expected_guc_counts[0],
            "bioseq_test_only_guc_count": expected_guc_counts[1],
            "sql_manifest_sha256": (
                production_sql_manifest_sha256
                if profile == "production"
                else test_indexes_sql_manifest_sha256
            ),
            "sql_script_sha256": active_sql_script_sha256,
            "installed_sql_sha256": active_sql_script_sha256,
            "installed_control_sha256": _path_artifact_sha256(
                installed_control_path,
                f"{case_id}.installed_control",
            ),
            "control_dso_path": str(control_dso_path),
            "control_dso_sha256": runtime_control_dso_sha256,
        }
        for field_name, expected_value in activation_exact_fields.items():
            actual_value = _field(
                activation_payload,
                field_name,
                f"{case_id}.activation_receipt",
            )
            if isinstance(expected_value, int):
                actual_value = _integer(
                    actual_value,
                    f"{case_id}.activation_receipt.{field_name}",
                )
            if actual_value != expected_value:
                _fail(
                    case_id,
                    f"activation receipt {field_name} differs",
                )
        guc_properties, guc_properties_sha256 = (
            _validate_guc_property_receipt(
                _field(
                    activation_payload,
                    "bioseq_guc_properties",
                    f"{case_id}.activation_receipt",
                ),
                _field(
                    activation_payload,
                    "bioseq_guc_properties_sha256",
                    f"{case_id}.activation_receipt",
                ),
                generation=guc_generations[profile],
                profile=profile,
                context=(
                    f"{case_id}.activation_receipt."
                    "bioseq_guc_properties"
                ),
            )
        )
        previous_guc_properties = profile_guc_properties.get(profile)
        current_guc_properties = (
            guc_properties,
            guc_properties_sha256,
        )
        if (
            previous_guc_properties is not None
            and previous_guc_properties != current_guc_properties
        ):
            _fail(
                case_id,
                "activation GUC properties differ within one profile",
            )
        profile_guc_properties[profile] = current_guc_properties
        postmaster_start_time = _string(
            _field(
                activation_payload,
                "postmaster_start_time",
                case_id,
            ),
            f"{case_id}.activation_receipt.postmaster_start_time",
        )
        activation_nonce = _string(
            _field(activation_payload, "activation_nonce", case_id),
            f"{case_id}.activation_receipt.activation_nonce",
        )
        if _integer(
            _field(activation_payload, "extension_oid", case_id),
            f"{case_id}.activation_receipt.extension_oid",
        ) == 0:
            _fail(case_id, "activation extension OID is zero")
        if _sha256(
            _field(
                activation_payload,
                "installed_control_sha256",
                case_id,
            ),
            f"{case_id}.activation_receipt.installed_control_sha256",
        ) != _path_artifact_sha256(installed_control_path, case_id):
            _fail(case_id, "installed control file hash differs")

        _require_exact_fields(
            request_payload,
            REQUEST_FIELDS,
            f"{case_id}.request",
        )
        expected_request_fields = {
            "schema_revision": 2,
            "profile": profile,
            "case_id": case_id,
            "sql_identity": sql_identity,
            "site_id": site_id,
            "ledger_site_sha256": inventory_entry[
                "ledger_site_sha256"
            ],
            "invocation_key": invocation_key,
            "fault_key": fault_key,
            "relation": inventory_entry["relation"],
            "probe_contract": probe_contract,
            **row_hashes,
            "cluster_config_sha256": cluster_config_sha256,
            "stage_manifest_sha256": active_stage_manifest_sha256,
            "tool_signature_suite_sha256": active_suite_sha256,
            "runtime_control_dso_sha256": runtime_control_dso_sha256,
            "activation_receipt_sha256": activation_sha256,
            "catalog_sha256": catalog_sha256,
            "catalog_entry_sha256": catalog_entry_sha256,
        }
        for field_name, expected_value in expected_request_fields.items():
            if _field(request_payload, field_name, case_id) != expected_value:
                _fail(case_id, f"request {field_name} differs")
        request_path_bindings = {
            "case_plan_path": canonical_expected_paths[
                f"{profile}_case_plan"
            ],
            "ledger_path": canonical_expected_paths[
                f"{profile}_ledger"
            ],
            "sql_manifest_path": canonical_expected_paths[
                f"{profile}_sql_manifest"
            ],
            "producer_executable_path": canonical_expected_paths[
                "runtime_producer"
            ],
            "cluster_config_path": config_path,
            "stage_manifest_path": active_stage_manifest_path,
            "activation_receipt_path": activation_path,
            "catalog_path": catalog_path,
            "active_dso_path": active_dso_path,
        }
        for field_name, expected_path in request_path_bindings.items():
            request_bound_path = _canonical_regular_file(
                Path(
                    _string(
                        _field(request_payload, field_name, case_id),
                        f"{case_id}.request.{field_name}",
                    )
                ),
                f"{case_id}.request.{field_name}",
            )
            if request_bound_path != expected_path:
                _fail(case_id, f"request {field_name} differs")

        _require_exact_fields(
            raw_payload,
            RAW_RESULT_FIELDS,
            f"{case_id}.raw_result",
        )
        expected_raw_fields = {
            "schema_revision": 2,
            "probe_abi_revision": 2,
            "profile": profile,
            "case_id": case_id,
            "sql_identity": sql_identity,
            "site_id": site_id,
            "ledger_site_sha256": inventory_entry[
                "ledger_site_sha256"
            ],
            "invocation_key": invocation_key,
            "fault_key": fault_key,
            "relation": inventory_entry["relation"],
            "probe_contract": probe_contract,
            **row_hashes,
            "activation_receipt_sha256": activation_sha256,
            "bioseq_guc_properties_sha256": guc_properties_sha256,
            "cluster_config_sha256": cluster_config_sha256,
            "request_sha256": request_sha256,
            "catalog_sha256": catalog_sha256,
            "catalog_entry_sha256": catalog_entry_sha256,
            "execution_backend": "postgresql",
            "exercised_terminal_severity": False,
        }
        for field_name, expected_value in expected_raw_fields.items():
            if _field(raw_payload, field_name, case_id) != expected_value:
                _fail(case_id, f"raw result {field_name} differs")
        if (
            _field(
                raw_payload,
                "bioseq_guc_properties",
                f"{case_id}.raw_result",
            )
            != guc_properties
        ):
            _fail(
                case_id,
                "raw-result GUC properties differ from activation",
            )

        backend_outcome = _object(
            _field(raw_payload, "backend_outcome", case_id),
            f"{case_id}.raw_result.backend_outcome",
        )
        _require_exact_fields(
            backend_outcome,
            BACKEND_OUTCOME_FIELDS,
            f"{case_id}.raw_result.backend_outcome",
        )
        expected_backend_outcome = _string(
            _field(
                backend_outcome,
                "expected",
                f"{case_id}.raw_result.backend_outcome",
            ),
            f"{case_id}.raw_result.backend_outcome.expected",
        )
        observed_backend_outcome = _string(
            _field(
                backend_outcome,
                "observed",
                f"{case_id}.raw_result.backend_outcome",
            ),
            f"{case_id}.raw_result.backend_outcome.observed",
        )
        initial_backend_pid = _integer(
            _field(
                backend_outcome,
                "initial_backend_pid",
                f"{case_id}.raw_result.backend_outcome",
            ),
            f"{case_id}.raw_result.backend_outcome.initial_backend_pid",
        )
        recovery_backend_pid = _integer(
            _field(
                backend_outcome,
                "recovery_backend_pid",
                f"{case_id}.raw_result.backend_outcome",
            ),
            f"{case_id}.raw_result.backend_outcome.recovery_backend_pid",
        )
        initial_connection_status = _string(
            _field(
                backend_outcome,
                "initial_connection_status",
                f"{case_id}.raw_result.backend_outcome",
            ),
            f"{case_id}.raw_result.backend_outcome.initial_connection_status",
        )
        post_invocation_connection_status = _string(
            _field(
                backend_outcome,
                "post_invocation_connection_status",
                f"{case_id}.raw_result.backend_outcome",
            ),
            (
                f"{case_id}.raw_result.backend_outcome."
                "post_invocation_connection_status"
            ),
        )
        invocation_result_status = _string(
            _field(
                backend_outcome,
                "invocation_result_status",
                f"{case_id}.raw_result.backend_outcome",
            ),
            f"{case_id}.raw_result.backend_outcome.invocation_result_status",
        )
        terminal_guard_observed = _boolean(
            _field(
                backend_outcome,
                "terminal_guard_observed",
                f"{case_id}.raw_result.backend_outcome",
            ),
            f"{case_id}.raw_result.backend_outcome.terminal_guard_observed",
        )
        terminal_causality = _validate_terminal_causality(
            _field(raw_payload, "terminal_causality", case_id),
            backend_exit=backend_exit,
            site_id=site_id,
            fault_key=fault_key,
            backend_pid=initial_backend_pid,
            marker_directory=(
                config_authority.terminal_marker_directory
            ),
            server_log_path=config_authority.server_log_path,
            marker_uid=(
                retained_authorities[profile].authorization_uid
            ),
            context=f"{case_id}.raw_result.terminal_causality",
        )
        if terminal_guard_observed != (terminal_causality is not None):
            _fail(
                case_id,
                "terminal_guard_observed is not derived from terminal "
                "causality",
            )
        if (
            expected_backend_outcome
            != plan_case["expected_backend_outcome"]
            or observed_backend_outcome
            != plan_case["expected_backend_outcome"]
            or initial_backend_pid <= 0
            or recovery_backend_pid <= 0
            or initial_connection_status != "ok"
            or (
                backend_exit
                and (
                    recovery_backend_pid == initial_backend_pid
                    or post_invocation_connection_status != "bad"
                    or invocation_result_status != "connection_lost"
                    or not terminal_guard_observed
                )
            )
            or (
                not backend_exit
                and (
                    recovery_backend_pid != initial_backend_pid
                    or post_invocation_connection_status != "ok"
                    or invocation_result_status != "postgres_error"
                    or terminal_guard_observed
                )
            )
        ):
            _fail(case_id, "backend connection lifecycle evidence differs")

        def observed_events(field_name: str) -> list[str]:
            events: list[str] = []
            for event_index, raw_event in enumerate(
                _array(
                    _field(raw_payload, field_name, case_id),
                    f"{case_id}.raw_result.{field_name}",
                )
            ):
                event_context = (
                    f"{case_id}.raw_result.{field_name}[{event_index}]"
                )
                event = _object(raw_event, event_context)
                _require_exact_fields(
                    event,
                    RUNTIME_EVENT_FIELDS,
                    event_context,
                )
                if _integer(
                    _field(event, "count", event_context),
                    f"{event_context}.count",
                ) != 1:
                    _fail(event_context, "event count is not exactly one")
                events.append(
                    _string(
                        _field(event, "name", event_context),
                        f"{event_context}.name",
                    )
                )
            if len(events) != len(set(events)):
                _fail(case_id, f"{field_name} contains duplicates")
            return sorted(events)

        raw_destructor_events = observed_events("destructor_events")
        raw_cleanup_events = observed_events("cleanup_events")
        raw_postgres_cleanup_events = observed_events(
            "postgres_cleanup_events"
        )
        if (
            raw_destructor_events
            != plan_case["expected_destructor_sentinels"]
            or raw_cleanup_events != plan_case["expected_cleanup_events"]
            or raw_postgres_cleanup_events
            != plan_case["expected_postgres_cleanup_events"]
        ):
            _fail(case_id, "raw runtime events differ from the case plan")
        if (
            sorted(
                _runtime_event_list(
                    _field(
                        result_payload,
                        "observed_destructor_sentinels",
                        case_id,
                    ),
                    f"{case_id}.result.observed_destructor_sentinels",
                    allow_empty=True,
                )
            )
            != raw_destructor_events
            or sorted(
                _runtime_event_list(
                    _field(
                        result_payload,
                        "observed_cleanup_events",
                        case_id,
                    ),
                    f"{case_id}.result.observed_cleanup_events",
                    allow_empty=True,
                )
            )
            != raw_cleanup_events
            or sorted(
                _runtime_event_list(
                    _field(
                        result_payload,
                        "observed_postgres_cleanup_events",
                        case_id,
                    ),
                    (
                        f"{case_id}.result."
                        "observed_postgres_cleanup_events"
                    ),
                    allow_empty=(
                        backend_exit
                        or fault_key
                        == BACKEND_SCOPED_VACUUM_CLEANUP_FAULT_KEY
                    ),
                )
            )
            != raw_postgres_cleanup_events
        ):
            _fail(case_id, "immutable result events differ from raw result")
        postgres_error = _object(
            _field(raw_payload, "postgres_error", case_id),
            f"{case_id}.raw_result.postgres_error",
        )
        _require_exact_fields(
            postgres_error,
            POSTGRES_ERROR_FIELDS,
            f"{case_id}.raw_result.postgres_error",
        )
        expected_postgres_error = (
            {
                "severity": None,
                "sqlstate": None,
                "message": None,
            }
            if backend_exit
            else {
                "severity": "ERROR",
                "sqlstate": plan_case["expected_sqlstate"],
                "message": plan_case["expected_message"],
            }
        )
        if postgres_error != expected_postgres_error:
            _fail(case_id, "raw PostgreSQL error differs from the plan")
        if (
            result_payload["observed_sqlstate"]
            != postgres_error["sqlstate"]
            or result_payload["observed_message"]
            != postgres_error["message"]
        ):
            _fail(case_id, "immutable PostgreSQL error differs from raw result")

        fault_evidence = _object(
            _field(raw_payload, "fault_evidence", case_id),
            f"{case_id}.raw_result.fault_evidence",
        )
        _require_exact_fields(
            fault_evidence,
            FAULT_EVIDENCE_FIELDS,
            f"{case_id}.raw_result.fault_evidence",
        )
        probe_observation_mode = _string(
            _field(
                fault_evidence,
                "observation_mode",
                f"{case_id}.raw_result.fault_evidence",
            ),
            f"{case_id}.raw_result.fault_evidence.observation_mode",
        )
        if probe_observation_mode != (
            "backend_exit" if backend_exit else "backend_snapshot"
        ):
            _fail(
                case_id,
                "fault observation mode differs from backend outcome",
            )
        recovery_probe = _object(
            _field(raw_payload, "recovery_probe", case_id),
            f"{case_id}.raw_result.recovery_probe",
        )
        _require_exact_fields(
            recovery_probe,
            RECOVERY_PROBE_FIELDS,
            f"{case_id}.raw_result.recovery_probe",
        )
        observed_rows = _runtime_row_matrix(
            _field(recovery_probe, "observed_rows", case_id),
            f"{case_id}.raw_result.recovery_probe.observed_rows",
        )
        expected_rows = _runtime_row_matrix(
            _field(recovery_probe, "expected_rows", case_id),
            f"{case_id}.raw_result.recovery_probe.expected_rows",
        )
        if (
            not _boolean(
                _field(recovery_probe, "passed", case_id),
                f"{case_id}.raw_result.recovery_probe.passed",
            )
            or not _boolean(
                _field(recovery_probe, "nonempty_result", case_id),
                f"{case_id}.raw_result.recovery_probe.nonempty_result",
            )
            or _string(
                _field(recovery_probe, "transaction_status", case_id),
                f"{case_id}.raw_result.recovery_probe.transaction_status",
            )
            != "idle"
            or _string(
                _field(recovery_probe, "postcondition", case_id),
                f"{case_id}.raw_result.recovery_probe.postcondition",
            )
            != plan_case["expected_recovery_postcondition"]
            or expected_rows != observed_rows
            or observed_rows
            != plan_case["execution"]["recovery_expected_rows"]
            or _integer(
                _field(recovery_probe, "result_row_count", case_id),
                f"{case_id}.raw_result.recovery_probe.result_row_count",
            )
            != len(observed_rows)
            or recovery_payload != observed_rows
            or recovery_artifact_sha256
            != _artifact_sha256(_canonical_json_bytes(observed_rows))
        ):
            _fail(case_id, "recovery artifact/result/plan join differs")
        if (
            result_payload["observed_recovery_postcondition"]
            != recovery_probe["postcondition"]
            or result_payload["observed_fmgr_abort_count"]
            != fault_evidence["fmgr_abort_count"]
        ):
            _fail(case_id, "immutable recovery/abort differs from raw result")
        cluster_evidence = _object(
            _field(raw_payload, "cluster_evidence", case_id),
            f"{case_id}.raw_result.cluster_evidence",
        )
        _require_exact_fields(
            cluster_evidence,
            CLUSTER_EVIDENCE_FIELDS,
            f"{case_id}.raw_result.cluster_evidence",
        )
        backend_pid = _integer(
            _field(cluster_evidence, "backend_pid", case_id),
            f"{case_id}.raw_result.cluster_evidence.backend_pid",
        )
        cluster_initial_backend_pid = _integer(
            _field(
                cluster_evidence,
                "initial_backend_pid",
                case_id,
            ),
            (
                f"{case_id}.raw_result.cluster_evidence."
                "initial_backend_pid"
            ),
        )
        cluster_recovery_backend_pid = _integer(
            _field(
                cluster_evidence,
                "recovery_backend_pid",
                case_id,
            ),
            (
                f"{case_id}.raw_result.cluster_evidence."
                "recovery_backend_pid"
            ),
        )
        connection_replaced = _boolean(
            _field(
                cluster_evidence,
                "connection_replaced",
                case_id,
            ),
            (
                f"{case_id}.raw_result.cluster_evidence."
                "connection_replaced"
            ),
        )
        if (
            backend_pid == 0
            or backend_pid != initial_backend_pid
            or cluster_initial_backend_pid != initial_backend_pid
            or cluster_recovery_backend_pid != recovery_backend_pid
            or connection_replaced != backend_exit
        ):
            _fail(case_id, "runtime backend lifecycle is inconsistent")
        expected_cluster_fields = {
            "cluster_system_identifier": cluster_system_identifier,
            "cluster_data_directory": str(cluster_data_directory),
            "postmaster_start_time": postmaster_start_time,
            "activation_nonce": activation_nonce,
            "server_version_num": PG17_SERVER_VERSION_NUM,
            "active_dso_path": str(active_dso_path),
            "mapped_target_path": str(active_dso_path),
            "control_dso_path": str(control_dso_path),
            "control_dso_sha256": runtime_control_dso_sha256,
        }
        for field_name, expected_value in expected_cluster_fields.items():
            if _field(cluster_evidence, field_name, case_id) != expected_value:
                _fail(case_id, f"cluster evidence {field_name} differs")
        if _path_artifact_sha256(
            _canonical_regular_file(
                Path(cluster_evidence["mapped_target_path"]),
                f"{case_id}.mapped_target_path",
            ),
            case_id,
        ) != active_dso_sha256:
            _fail(case_id, "mapped target bytes differ")

        expected_attestation = {
            "schema_revision": 1,
            "request_path": str(request_path.relative_to(candidate_root)),
            "request_sha256": request_sha256,
            "catalog_path": str(catalog_path.relative_to(candidate_root)),
            "catalog_sha256": catalog_sha256,
            "catalog_entry_sha256": catalog_entry_sha256,
            "activation_receipt_path": str(
                activation_path.relative_to(candidate_root)
            ),
            "activation_receipt_sha256": activation_sha256,
            "cluster_config_path": str(
                config_path.relative_to(candidate_root)
            ),
            "cluster_config_sha256": cluster_config_sha256,
            "stage_manifest_sha256": active_stage_manifest_sha256,
            "tool_signature_suite_sha256": active_suite_sha256,
            "active_dso_path": str(active_dso_path),
            "active_dso_sha256": active_dso_sha256,
            "cluster_system_identifier": cluster_system_identifier,
            "cluster_data_directory": str(cluster_data_directory),
            "postmaster_start_time": postmaster_start_time,
            "activation_nonce": activation_nonce,
            "server_version_num": PG17_SERVER_VERSION_NUM,
            "backend_pid": backend_pid,
            "expected_backend_outcome": expected_backend_outcome,
            "observed_backend_outcome": observed_backend_outcome,
            "probe_observation_mode": probe_observation_mode,
            "initial_backend_pid": initial_backend_pid,
            "recovery_backend_pid": recovery_backend_pid,
            "connection_replaced": connection_replaced,
            "initial_connection_status": initial_connection_status,
            "post_invocation_connection_status": (
                post_invocation_connection_status
            ),
            "invocation_result_status": invocation_result_status,
            "terminal_guard_observed": terminal_guard_observed,
            "terminal_causality": terminal_causality,
            "probe_abi_revision": 2,
            "site_id": site_id,
            "ledger_site_sha256": inventory_entry[
                "ledger_site_sha256"
            ],
            "fault_key": fault_key,
            "relation": inventory_entry["relation"],
            "probe_contract": probe_contract,
            "fault_invocation_count": fault_evidence[
                "invocation_count"
            ],
            "fmgr_start_count": fault_evidence["fmgr_start_count"],
            "fmgr_end_count": fault_evidence["fmgr_end_count"],
            "fmgr_abort_count": fault_evidence["fmgr_abort_count"],
            "probe_invalid_count": fault_evidence["invalid_count"],
            "probe_overflow_count": fault_evidence["overflow_count"],
            "probe_inside_target": fault_evidence["inside_target"],
            "site_visit_count": fault_evidence["site_visit_count"],
            "fault_trigger_count": fault_evidence[
                "fault_trigger_count"
            ],
            "site_visit_sequence": fault_evidence[
                "site_visit_sequence"
            ],
            "fault_trigger_sequence": fault_evidence[
                "fault_trigger_sequence"
            ],
            "mapped_target_path": str(active_dso_path),
            "mapped_target_sha256": active_dso_sha256,
            "control_dso_path": str(control_dso_path),
            "control_dso_sha256": runtime_control_dso_sha256,
            "recovery_result_row_count": len(observed_rows),
            "recovery_rows_sha256": recovery_artifact_sha256,
            "recovery_artifact_path": str(
                recovery_path.relative_to(candidate_root)
            ),
            "recovery_artifact_sha256": recovery_artifact_sha256,
            "raw_result_path": str(
                raw_result_path.relative_to(candidate_root)
            ),
            "raw_result_sha256": raw_result_sha256,
        }
        for field_name in RUNTIME_ATTESTATION_FIELDS:
            if _field(attestation, field_name, case_id) != (
                expected_attestation[field_name]
            ):
                _fail(
                    case_id,
                    f"runtime attestation {field_name} differs from "
                    "authenticated artifacts",
                )

        _require_exact_fields(
            log_payload,
            CASE_LOG_FIELDS,
            f"{case_id}.immutable_test_log",
        )
        if (
            _integer(
                _field(log_payload, "schema_revision", case_id),
                f"{case_id}.immutable_test_log.schema_revision",
            )
            != 1
            or _string(
                _field(log_payload, "context", case_id),
                f"{case_id}.immutable_test_log.context",
            )
            != f"{profile}/{case_id}"
            or _integer(
                _field(log_payload, "returncode", case_id),
                f"{case_id}.immutable_test_log.returncode",
            )
            != 0
            or not isinstance(log_payload.get("stdout"), str)
            or not isinstance(log_payload.get("stderr"), str)
        ):
            _fail(case_id, "runtime test log identity/status differs")
        log_bindings = _object(
            _field(log_payload, "bindings", case_id),
            f"{case_id}.immutable_test_log.bindings",
        )
        _require_exact_fields(
            log_bindings,
            CASE_LOG_BINDING_FIELDS,
            f"{case_id}.immutable_test_log.bindings",
        )
        expected_log_bindings = {
            "request_sha256": request_sha256,
            "raw_result_sha256": raw_result_sha256,
            "immutable_result_sha256": immutable_result_sha256,
            "activation_receipt_sha256": activation_sha256,
            "catalog_sha256": catalog_sha256,
            "case_plan_sha256": active_case_plan_sha256,
            "runtime_inventory_sha256": (
                production_runtime_inventory_sha256
                if profile == "production"
                else test_indexes_runtime_inventory_sha256
            ),
            "ledger_sha256": (
                production_sha256
                if profile == "production"
                else test_indexes_sha256
            ),
            "sql_manifest_sha256": (
                production_sql_manifest_sha256
                if profile == "production"
                else test_indexes_sql_manifest_sha256
            ),
            "active_dso_sha256": active_dso_sha256,
            "control_dso_sha256": runtime_control_dso_sha256,
            "test_executable_sha256": active_test_executable_sha256,
            "producer_executable_sha256": (
                runtime_producer_executable_sha256
            ),
            "cluster_config_sha256": cluster_config_sha256,
            "stage_manifest_sha256": active_stage_manifest_sha256,
            "tool_signature_suite_sha256": active_suite_sha256,
        }
        if log_bindings != expected_log_bindings:
            _fail(case_id, "runtime test-log hash bindings differ")
        command = [
            _string(item, f"{case_id}.immutable_test_log.command")
            for item in _array(
                _field(log_payload, "command", case_id),
                f"{case_id}.immutable_test_log.command",
            )
        ]
        expected_harness_path = canonical_expected_paths[
            f"{profile}_test_executable"
        ]
        if (
            len(command) != 6
            or _canonical_regular_file(
                Path(command[0]),
                f"{case_id}.immutable_test_log.command[0]",
            )
            != expected_harness_path
            or command[1:3]
            != ["--bioseq-safety-runtime-run", "--request"]
            or _canonical_regular_file(
                Path(command[3]),
                f"{case_id}.immutable_test_log.command[3]",
            )
            != request_path
            or command[4] != "--output"
            or not Path(command[5]).is_absolute()
            or Path(command[5]).name
            != f"{profile}.{case_id}.raw-result.json"
        ):
            _fail(case_id, "runtime test-log command identity differs")

        cluster_binding = (
            cluster_system_identifier,
            cluster_data_directory,
            postmaster_start_time,
            activation_nonce,
            active_dso_path,
            control_dso_path,
            activation_sha256,
            cluster_config_sha256,
            catalog_sha256,
            active_stage_manifest_sha256,
            active_suite_sha256,
        )
        if profile_cluster_bindings.setdefault(
            profile,
            cluster_binding,
        ) != cluster_binding:
            _fail(case_id, "profile cluster binding changes between cases")
        exact_result_fields = {
            "probe_abi_revision": 2,
            "case_id": case_id,
            "profile": profile,
            "sql_identity": sql_identity,
            "site_id": site_id,
            "ledger_site_sha256": inventory_entry[
                "ledger_site_sha256"
            ],
            "invocation_key": invocation_key,
            "fault_key": fault_key,
            "relation": inventory_entry["relation"],
            "expected_backend_outcome": expected_backend_outcome,
            "observed_backend_outcome": observed_backend_outcome,
            "probe_observation_mode": probe_observation_mode,
            "initial_backend_pid": initial_backend_pid,
            "recovery_backend_pid": recovery_backend_pid,
            "connection_replaced": connection_replaced,
            "initial_connection_status": initial_connection_status,
            "post_invocation_connection_status": (
                post_invocation_connection_status
            ),
            "invocation_result_status": invocation_result_status,
            "terminal_guard_observed": terminal_guard_observed,
            "terminal_causality": terminal_causality,
            **exact_causal_fields,
            **row_hashes,
        }
        for field_name, expected in exact_result_fields.items():
            actual = _field(
                result_payload,
                field_name,
                f"{case_id}.immutable_result",
            )
            if (
                field_name == "probe_abi_revision"
                or field_name in exact_causal_fields
            ):
                actual = _integer(
                    actual,
                    f"{case_id}.immutable_result.{field_name}",
                )
            if actual != expected:
                _fail(
                    case_id,
                    f"immutable result {field_name} is stale",
                )
        if _validate_probe_contract(
            _field(
                result_payload,
                "probe_contract",
                f"{case_id}.immutable_result",
            ),
            inventory_entry["relation"],
            f"{case_id}.immutable_result.probe_contract",
        ) != probe_contract:
            _fail(
                case_id,
                "immutable result probe_contract is stale",
            )
        attestation_exact_fields = {
            "server_version_num": PG17_SERVER_VERSION_NUM,
            "probe_abi_revision": 2,
            "site_id": site_id,
            "ledger_site_sha256": inventory_entry[
                "ledger_site_sha256"
            ],
            "fault_key": fault_key,
            "relation": inventory_entry["relation"],
            **(
                {
                    "fault_invocation_count": 0,
                    "fmgr_start_count": 0,
                    "fmgr_end_count": 0,
                    "fmgr_abort_count": 0,
                }
                if backend_exit
                else _expected_runtime_lifecycle(fault_key)
            ),
            "probe_invalid_count": 0,
            "probe_overflow_count": 0,
            "probe_inside_target": False,
            **exact_causal_fields,
        }
        for field_name, expected in attestation_exact_fields.items():
            actual = _field(
                attestation,
                field_name,
                f"{case_id}.immutable_result.attestation",
            )
            if isinstance(expected, int) and not isinstance(expected, bool):
                actual = _integer(
                    actual,
                    f"{case_id}.immutable_result.attestation.{field_name}",
                )
            elif isinstance(expected, bool):
                actual = _boolean(
                    actual,
                    f"{case_id}.immutable_result.attestation.{field_name}",
                )
            if actual != expected:
                _fail(
                    case_id,
                    f"runtime attestation {field_name} differs from ABI revision 2",
                )
        if _validate_probe_contract(
            _field(
                attestation,
                "probe_contract",
                f"{case_id}.immutable_result.attestation",
            ),
            inventory_entry["relation"],
            (
                f"{case_id}.immutable_result.attestation."
                "probe_contract"
            ),
        ) != probe_contract:
            _fail(
                case_id,
                "runtime attestation probe_contract differs from ABI revision 2",
            )
        for field_name in (
            "expected_backend_outcome",
            "observed_backend_outcome",
            "probe_observation_mode",
            "initial_backend_pid",
            "recovery_backend_pid",
            "connection_replaced",
            "initial_connection_status",
            "post_invocation_connection_status",
            "invocation_result_status",
            "terminal_guard_observed",
            "terminal_causality",
            "expected_destructor_sentinels",
            "observed_destructor_sentinels",
            "expected_cleanup_events",
            "observed_cleanup_events",
            "expected_postgres_cleanup_events",
            "observed_postgres_cleanup_events",
            "expected_fmgr_abort_count",
            "observed_fmgr_abort_count",
            "expected_sqlstate",
            "observed_sqlstate",
            "expected_message",
            "observed_message",
            "expected_recovery_postcondition",
            "observed_recovery_postcondition",
            "exercised_terminal_severity",
        ):
            if _field(
                result_payload,
                field_name,
                f"{case_id}.immutable_result",
            ) != _field(test_case, field_name, case_id):
                _fail(
                    case_id,
                    f"{field_name} is not sourced from the immutable "
                    "runtime result",
                )
        if _boolean(
            _field(test_case, "exercised_terminal_severity", case_id),
            f"{case_id}.exercised_terminal_severity",
        ):
            _fail(case_id, "runtime tests must not exercise FATAL or PANIC")

        expected_sentinels = _runtime_event_list(
            _field(test_case, "expected_destructor_sentinels", case_id),
            f"{case_id}.expected_destructor_sentinels",
            allow_empty=True,
        )
        observed_sentinels = _runtime_event_list(
            _field(test_case, "observed_destructor_sentinels", case_id),
            f"{case_id}.observed_destructor_sentinels",
            allow_empty=True,
        )
        lifetime_free_direct = (
            ledger_site["classification"] == "lifetime_free_direct"
        )
        if (
            set(expected_sentinels) != set(observed_sentinels)
            or (
                backend_exit
                and bool(expected_sentinels)
            )
            or (
                not backend_exit
                and (
                    (lifetime_free_direct and expected_sentinels)
                    or (
                        not lifetime_free_direct
                        and not expected_sentinels
                    )
                )
            )
        ):
            _fail(case_id, "destructor sentinels did not unwind exactly once")
        expected_cleanup = _runtime_event_list(
            _field(test_case, "expected_cleanup_events", case_id),
            f"{case_id}.expected_cleanup_events",
            allow_empty=True,
        )
        observed_cleanup = _runtime_event_list(
            _field(test_case, "observed_cleanup_events", case_id),
            f"{case_id}.observed_cleanup_events",
            allow_empty=True,
        )
        expected_postgres_cleanup = _runtime_event_list(
            _field(
                test_case,
                "expected_postgres_cleanup_events",
                case_id,
            ),
            f"{case_id}.expected_postgres_cleanup_events",
            allow_empty=True,
        )
        observed_postgres_cleanup = _runtime_event_list(
            _field(
                test_case,
                "observed_postgres_cleanup_events",
                case_id,
            ),
            f"{case_id}.observed_postgres_cleanup_events",
            allow_empty=True,
        )
        expected_abort_count = _integer(
            _field(test_case, "expected_fmgr_abort_count", case_id),
            f"{case_id}.expected_fmgr_abort_count",
        )
        observed_abort_count = _integer(
            _field(test_case, "observed_fmgr_abort_count", case_id),
            f"{case_id}.observed_fmgr_abort_count",
        )
        backend_scoped_vacuum_cleanup = (
            fault_key
            == BACKEND_SCOPED_VACUUM_CLEANUP_FAULT_KEY
        )
        expected_lifecycle = (
            {
                "fault_invocation_count": 0,
                "fmgr_start_count": 0,
                "fmgr_end_count": 0,
                "fmgr_abort_count": 0,
            }
            if backend_exit
            else _expected_runtime_lifecycle(fault_key)
        )
        if (
            set(expected_cleanup) != set(observed_cleanup)
            or set(expected_postgres_cleanup)
            != set(observed_postgres_cleanup)
            or (
                backend_exit
                and (
                    bool(expected_cleanup)
                    or bool(expected_postgres_cleanup)
                )
            )
            or (
                not backend_exit
                and backend_scoped_vacuum_cleanup
                and bool(expected_postgres_cleanup)
            )
            or (
                not backend_exit
                and not backend_scoped_vacuum_cleanup
                and (
                    not expected_postgres_cleanup
                    or "fmgr_abort_observed"
                    not in expected_postgres_cleanup
                )
            )
            or expected_abort_count
            != expected_lifecycle["fmgr_abort_count"]
            or expected_abort_count != observed_abort_count
        ):
            _fail(case_id, "cleanup/FMGR ABORT evidence is not exact")

        expected_sqlstate_value = _field(
            test_case, "expected_sqlstate", case_id
        )
        observed_sqlstate_value = _field(
            test_case, "observed_sqlstate", case_id
        )
        expected_message_value = _field(
            test_case, "expected_message", case_id
        )
        observed_message_value = _field(
            test_case, "observed_message", case_id
        )
        if backend_exit:
            if (
                expected_sqlstate_value is not None
                or observed_sqlstate_value is not None
                or expected_message_value is not None
                or observed_message_value is not None
            ):
                _fail(
                    case_id,
                    "backend-exit case fabricated PostgreSQL error fields",
                )
        else:
            expected_sqlstate = _string(
                expected_sqlstate_value,
                f"{case_id}.expected_sqlstate",
            )
            observed_sqlstate = _string(
                observed_sqlstate_value,
                f"{case_id}.observed_sqlstate",
            )
            if (
                re.fullmatch(r"[0-9A-Z]{5}", expected_sqlstate) is None
                or expected_sqlstate != observed_sqlstate
                or _string(
                    expected_message_value,
                    f"{case_id}.expected_message",
                )
                != _string(
                    observed_message_value,
                    f"{case_id}.observed_message",
                )
            ):
                _fail(
                    case_id,
                    "SQLSTATE or PostgreSQL message was not preserved",
                )
        if _string(
            _field(test_case, "expected_recovery_postcondition", case_id),
            f"{case_id}.expected_recovery_postcondition",
        ) != _string(
            _field(test_case, "observed_recovery_postcondition", case_id),
            f"{case_id}.observed_recovery_postcondition",
        ):
            _fail(case_id, "recovery postcondition did not hold")

        if not exact_site_coverage <= required:
            _fail(
                case_id,
                "inventory-bound ledger site derives unknown coverage",
            )
        observed.update(exact_site_coverage)
    if observed_plan_cases["production"] != set(production_plan):
        _fail(
            "runtime-matrix",
            "production matrix case set differs from the current case plan",
        )
    if observed_plan_cases["test_indexes"] != set(test_indexes_plan):
        _fail(
            "runtime-matrix",
            "test_indexes matrix case set differs from the current case plan",
        )
    if seen_profiles != set(PROFILES):
        _fail("runtime-matrix", "both compilation profiles need runtime cases")
    if any(
        len(observed_plan_cases[profile])
        != expected_profile_case_count
        for profile in PROFILES
    ):
        _fail(
            "runtime-matrix",
            f"requires exactly {expected_profile_case_count} cases "
            "per profile",
        )
    if set(profile_guc_properties) != set(PROFILES):
        _fail("runtime-matrix", "both GUC profile receipts are required")
    _validate_guc_profile_pair(
        profile_guc_properties["production"][0],
        profile_guc_properties["test_indexes"][0],
        context="runtime-matrix",
    )
    _validate_distinct_private_clusters(profile_cluster_bindings)
    if observed != required:
        _fail("runtime-matrix", "classification/family join has missing keys")
    return {
        "case_count": len(cases),
        "coverage_key_count": len(required),
        "production_stage_manifest_sha256": production_stage_sha256,
        "test_indexes_stage_manifest_sha256": (
            test_indexes_stage_sha256
        ),
        "production_tool_signature_suite_sha256": (
            production_suite_sha256
        ),
        "test_indexes_tool_signature_suite_sha256": (
            test_indexes_suite_sha256
        ),
        "production_cluster_authority_path": str(
            production_authority.record_path.relative_to(candidate_root)
        ),
        "production_cluster_authority_sha256": (
            production_authority.record_sha256
        ),
        "test_indexes_cluster_authority_path": str(
            test_indexes_authority.record_path.relative_to(candidate_root)
        ),
        "test_indexes_cluster_authority_sha256": (
            test_indexes_authority.record_sha256
        ),
        "runtime_control_dso_sha256": runtime_control_dso_sha256,
    }


def _validate_distinct_private_clusters(
    profile_cluster_bindings: Mapping[str, tuple[Any, ...]],
) -> None:
    production_cluster = profile_cluster_bindings.get("production")
    test_indexes_cluster = profile_cluster_bindings.get("test_indexes")
    if (
        production_cluster is None
        or test_indexes_cluster is None
        or production_cluster[0] == test_indexes_cluster[0]
        or production_cluster[1] == test_indexes_cluster[1]
    ):
        _fail(
            "runtime-matrix",
            "profiles require distinct private cluster system identifiers "
            "and data directories",
        )


def parse_nm_symbols(
    text: str,
    *,
    source_tool: str = "nm",
    posix: bool = True,
) -> list[DynamicSymbol]:
    """Parse `nm -D --defined-only`, preferably with `--format=posix`."""

    result: list[DynamicSymbol] = []
    for line_number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line or line.endswith(":"):
            continue
        parts = line.split()
        if posix:
            if len(parts) < 2 or len(parts[1]) != 1:
                _fail(f"{source_tool}:{line_number}", "invalid POSIX nm row")
            name, type_code = parts[0], parts[1]
        else:
            if len(parts) < 3 or len(parts[1]) != 1:
                _fail(f"{source_tool}:{line_number}", "invalid nm row")
            type_code, name = parts[1], parts[2]
        normalized_type = type_code.upper()
        defined = normalized_type != "U"
        binding = (
            "LOCAL"
            if type_code.islower()
            else "WEAK"
            if normalized_type in {"V", "W"}
            else "GLOBAL"
        )
        result.append(
            DynamicSymbol(
                name=name.split("@", 1)[0],
                binding=binding,
                symbol_type=normalized_type,
                source_tool=source_tool,
                defined=defined,
                dynamic_visible=defined and binding != "LOCAL",
            )
        )
    if not result:
        _fail(source_tool, "dynamic symbol inventory is empty")
    return result


READELF_SYMBOL_PATTERN = re.compile(
    r"^\s*\d+:\s+[0-9A-Fa-f]+\s+\d+\s+"
    r"(?P<type>\S+)\s+(?P<bind>\S+)\s+(?P<visibility>\S+)\s+"
    r"(?P<index>\S+)\s+(?P<name>\S+)"
)


def parse_readelf_symbols(
    text: str, *, source_tool: str = "readelf"
) -> list[DynamicSymbol]:
    """Parse `readelf --dyn-syms --wide` output."""

    result: list[DynamicSymbol] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        match = READELF_SYMBOL_PATTERN.match(line)
        if match is None:
            continue
        name = match.group("name").split("@", 1)[0]
        if not name:
            continue
        binding = match.group("bind")
        defined = match.group("index") != "UND"
        visible = (
            defined
            and binding in {"GLOBAL", "WEAK"}
            and match.group("visibility") in {"DEFAULT", "PROTECTED"}
        )
        result.append(
            DynamicSymbol(
                name=name,
                binding=binding,
                symbol_type=match.group("type"),
                source_tool=source_tool,
                defined=defined,
                dynamic_visible=visible,
            )
        )
    if not result:
        _fail(source_tool, "dynamic symbol inventory is empty")
    return result


def _exported_symbol(
    name: str,
    *,
    source_tool: str,
    symbol_type: str = "FUNC",
) -> DynamicSymbol:
    return DynamicSymbol(
        name=name,
        binding="GLOBAL",
        symbol_type=symbol_type,
        source_tool=source_tool,
        defined=True,
        dynamic_visible=True,
    )


def parse_darwin_nm_symbols(
    text: str, *, source_tool: str = "nm -gUj"
) -> list[DynamicSymbol]:
    """Parse Darwin `nm -gUj` names and remove the Mach-O C prefix."""

    result: list[DynamicSymbol] = []
    for raw_line in text.splitlines():
        name = raw_line.strip()
        if not name or name.endswith(":"):
            continue
        if name.startswith("_"):
            name = name[1:]
        if name:
            result.append(
                _exported_symbol(name, source_tool=source_tool)
            )
    if not result:
        _fail(source_tool, "dynamic symbol inventory is empty")
    return result


def parse_llvm_coff_exports(
    text: str, *, source_tool: str = "llvm-readobj --coff-exports"
) -> list[DynamicSymbol]:
    """Parse the `Name:` records in LLVM's PE/COFF export inventory."""

    result: list[DynamicSymbol] = []
    in_export = False
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line == "Export {":
            in_export = True
            continue
        if in_export and line == "}":
            in_export = False
            continue
        if not in_export or not line.startswith("Name:"):
            continue
        name = line.removeprefix("Name:").strip().split(" (", 1)[0]
        if name:
            result.append(
                _exported_symbol(name, source_tool=source_tool)
            )
    if not result:
        _fail(source_tool, "dynamic symbol inventory is empty")
    return result


DUMPBIN_EXPORT_PATTERN = re.compile(
    r"^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(?P<name>\S+)"
)


def parse_dumpbin_exports(
    text: str, *, source_tool: str = "dumpbin /exports"
) -> list[DynamicSymbol]:
    """Parse MSVC dumpbin's export table rows."""

    result: list[DynamicSymbol] = []
    for line in text.splitlines():
        match = DUMPBIN_EXPORT_PATTERN.match(line)
        if match is None:
            continue
        name = match.group("name")
        if name:
            result.append(
                _exported_symbol(name, source_tool=source_tool)
            )
    if not result:
        _fail(source_tool, "dynamic symbol inventory is empty")
    return result


def _run_symbol_tool(command: list[str]) -> subprocess.CompletedProcess[str] | None:
    try:
        return subprocess.run(
            command, check=False, capture_output=True, text=True
        )
    except OSError:
        return None


def inspect_dso_symbols(path: Path) -> tuple[list[DynamicSymbol], str]:
    """Inspect ELF, Mach-O, or PE/COFF exports without invoking a shell."""

    try:
        with path.open("rb") as dso:
            header = dso.read(4)
    except OSError as error:
        raise EvidenceError(f"cannot read DSO {path}: {error}") from error
    is_elf = header == b"\x7fELF"
    is_macho = header in {
        b"\xfe\xed\xfa\xce",
        b"\xce\xfa\xed\xfe",
        b"\xfe\xed\xfa\xcf",
        b"\xcf\xfa\xed\xfe",
        b"\xca\xfe\xba\xbe",
        b"\xbe\xba\xfe\xca",
    }
    is_coff = header[:2] == b"MZ"

    nm_command = [
        "nm",
        "-D",
        "--defined-only",
        "--format=posix",
        str(path),
    ]
    nm_result = _run_symbol_tool(nm_command) if is_elf else None
    readelf_command = ["readelf", "--dyn-syms", "--wide", str(path)]
    readelf_result = (
        _run_symbol_tool(readelf_command) if is_elf else None
    )
    if nm_result is not None and nm_result.returncode == 0:
        return (
            parse_nm_symbols(
                nm_result.stdout,
                source_tool="nm -D --defined-only --format=posix",
            ),
            "nm",
        )
    if readelf_result is not None and readelf_result.returncode == 0:
        return (
            parse_readelf_symbols(
                readelf_result.stdout,
                source_tool="readelf --dyn-syms --wide",
            ),
            "readelf",
        )

    darwin_command = ["nm", "-gUj", str(path)]
    darwin_result = (
        _run_symbol_tool(darwin_command) if is_macho else None
    )
    if darwin_result is not None and darwin_result.returncode == 0:
        return (
            parse_darwin_nm_symbols(
                darwin_result.stdout, source_tool="nm -gUj"
            ),
            "darwin-nm",
        )

    coff_command = ["llvm-readobj", "--coff-exports", str(path)]
    coff_result = (
        _run_symbol_tool(coff_command) if is_coff else None
    )
    if coff_result is not None and coff_result.returncode == 0:
        return (
            parse_llvm_coff_exports(
                coff_result.stdout,
                source_tool="llvm-readobj --coff-exports",
            ),
            "llvm-readobj",
        )

    dumpbin_command = ["dumpbin", "/nologo", "/exports", str(path)]
    dumpbin_result = (
        _run_symbol_tool(dumpbin_command) if is_coff else None
    )
    if dumpbin_result is not None and dumpbin_result.returncode == 0:
        return (
            parse_dumpbin_exports(
                dumpbin_result.stdout,
                source_tool="dumpbin /exports",
            ),
            "dumpbin",
        )

    attempted = [
        ("nm", nm_result),
        ("readelf", readelf_result),
        ("Darwin nm", darwin_result),
        ("llvm-readobj", coff_result),
        ("dumpbin", dumpbin_result),
    ]
    details = "; ".join(
        f"{name}: "
        + (
            "unavailable"
            if result is None
            else result.stderr.strip() or f"exit {result.returncode}"
        )
        for name, result in attempted
    )
    _fail(str(path), f"platform export inspection failed: {details}")


def _one_dynamic_export(
    symbols: dict[str, list[DynamicSymbol]], name: str
) -> None:
    occurrences = symbols.get(name, [])
    if len(occurrences) != 1:
        _fail(name, "DSO symbol must be dynamically visible exactly once")
    if occurrences[0].binding != "GLOBAL":
        _fail(name, "DSO symbol must have GLOBAL binding")


def validate_dso_ownership(
    manifest: dict[str, Any],
    inventory: list[DynamicSymbol],
    *,
    contract: str = "bioseq",
    allowed_dso_exports: tuple[str, ...] = (),
    main_identity_count: int = 272,
    main_symbol_count: int = 270,
    shared_symbols: frozenset[str] = frozenset(
        {"from_array", "mmsearch"}
    ),
    external_symbols: frozenset[str] = EXTERNAL_SYMBOLS,
    required_absent_symbols: frozenset[str] = ABSENT_VARCHAR_SYMBOLS,
    required_present_symbols: frozenset[str] = TARGET_BPCHAR_SYMBOLS,
    require_complete_codegen: bool = True,
) -> dict[str, int]:
    """Join the SQL manifest to the live DSO dynamic-symbol closure.

    Each main symbol must have exactly one generated or handwritten FMGR
    export owner. Complete evidence additionally requires every owner to be
    generated. The Bioseq contract retains its reviewed fixed inventory;
    the manifest contract derives extension-specific counts and ownership
    partitions from the authenticated generated manifest and closes the
    entire export surface over its explicit non-fmgr ABI allowlist.
    """

    _require_current_generated_sql_manifest(
        manifest, "DSO-manifest"
    )
    if contract not in DSO_CLOSURE_CONTRACTS:
        _fail("DSO", f"unknown closure contract {contract!r}")
    manifest_contract = contract == "manifest"
    normalized_allowed_dso_exports: set[str] = set()
    for index, raw_export in enumerate(allowed_dso_exports):
        export = _string(
            raw_export,
            f"DSO.allowed_dso_exports[{index}]",
        )
        if export in normalized_allowed_dso_exports:
            _fail(export, "DSO ABI allowlist contains a duplicate")
        normalized_allowed_dso_exports.add(export)
    if not manifest_contract and normalized_allowed_dso_exports:
        _fail(
            "DSO",
            "explicit non-fmgr ABI exports require the manifest contract",
        )
    if manifest_contract:
        main_identity_count = _integer(
            _field(
                manifest,
                "c_routine_identity_count",
                "DSO-manifest",
            ),
            "DSO-manifest.c_routine_identity_count",
        )
        main_symbol_count = _integer(
            _field(
                manifest,
                "c_link_symbol_inventory_count",
                "DSO-manifest",
            ),
            "DSO-manifest.c_link_symbol_inventory_count",
        )
        external_symbols = frozenset(
            _string_list(
                _field(
                    manifest,
                    "handwritten_fmgr_export_external_symbols",
                    "DSO-manifest",
                ),
                "DSO-manifest.handwritten_fmgr_export_external_symbols",
            )
        )
        required_absent_symbols = frozenset()
        required_present_symbols = frozenset()
    elif external_symbols != EXTERNAL_SYMBOLS:
        _fail(
            "DSO",
            "bioseq_external disposition must contain only its handler",
        )

    identities = _string_list(
        _field(manifest, "c_routine_identities", "DSO-manifest"),
        "DSO-manifest.c_routine_identities",
        allow_empty=False,
    )
    if (
        _integer(
            _field(manifest, "c_routine_identity_count", "DSO-manifest"),
            "DSO-manifest.c_routine_identity_count",
        )
        != main_identity_count
        or len(identities) != main_identity_count
    ):
        _fail("DSO", "main C routine identity total is not exact")

    main_symbols = set(
        _string_list(
            _field(manifest, "c_link_symbols", "DSO-manifest"),
            "DSO-manifest.c_link_symbols",
            allow_empty=False,
        )
    )
    if (
        _integer(
            _field(manifest, "c_link_symbol_inventory_count", "DSO-manifest"),
            "DSO-manifest.c_link_symbol_inventory_count",
        )
        != main_symbol_count
        or len(main_symbols) != main_symbol_count
    ):
        _fail("DSO", "main public fmgr symbol total is not exact")
    if main_symbols & set(external_symbols):
        _fail(
            "DSO",
            (
                "main and external fmgr ownership overlaps"
                if manifest_contract
                else "bioseq and bioseq_external ownership overlaps"
            ),
        )

    finfo_symbols = set(
        _string_list(
            _field(manifest, "c_finfo_symbols", "DSO-manifest"),
            "DSO-manifest.c_finfo_symbols",
            allow_empty=False,
        )
    )
    if finfo_symbols != {f"pg_finfo_{symbol}" for symbol in main_symbols}:
        _fail("DSO", "manifest finfo inventory differs from main owners")
    if (
        _integer(
            _field(
                manifest, "c_finfo_symbol_inventory_count", "DSO-manifest"
            ),
            "DSO-manifest.c_finfo_symbol_inventory_count",
        )
        != main_symbol_count
    ):
        _fail("DSO", "manifest finfo total is not exact")

    owners = _array(
        _field(manifest, "c_symbol_owners", "DSO-manifest"),
        "DSO-manifest.c_symbol_owners",
    )
    if len(owners) != main_symbol_count:
        _fail("DSO", "symbol-owner inventory is not exact")
    grouped_identities: set[str] = set()
    grouped_symbols: set[str] = set()
    generated_owner_symbols: set[str] = set()
    observed_shared_symbols: set[str] = set()
    for index, raw_owner in enumerate(owners):
        context = f"DSO-manifest.c_symbol_owners[{index}]"
        owner = _object(raw_owner, context)
        symbol = _string(_field(owner, "symbol", context), f"{context}.symbol")
        if symbol not in main_symbols or symbol in grouped_symbols:
            _fail(symbol, "symbol owner is missing or duplicated")
        grouped_symbols.add(symbol)
        if _field(owner, "finfo_symbol", symbol) != f"pg_finfo_{symbol}":
            _fail(symbol, "owner finfo symbol is inconsistent")
        if _boolean(
            _field(owner, "generated_wrapper", symbol),
            f"{symbol}.generated_wrapper",
        ):
            generated_owner_symbols.add(symbol)
        owner_identities = _string_list(
            _field(owner, "routine_identities", symbol),
            f"{symbol}.routine_identities",
            allow_empty=False,
        )
        _string_list(
            _field(owner, "implementation_declarations", symbol),
            f"{symbol}.implementation_declarations",
            allow_empty=False,
        )
        if len(owner_identities) > 1:
            observed_shared_symbols.add(symbol)
        for identity in owner_identities:
            if (
                identity not in identities
                or identity in grouped_identities
                or _routine_leaf(identity) != symbol
            ):
                _fail(symbol, "symbol sharing crosses SQL names or identities")
            grouped_identities.add(identity)
    owner_closure_mismatch = (
        grouped_identities != set(identities)
        or grouped_symbols != main_symbols
    )
    sharing_mismatch = (
        not manifest_contract
        and observed_shared_symbols != set(shared_symbols)
    )
    if owner_closure_mismatch or sharing_mismatch:
        _fail(
            "DSO",
            (
                "routine identities do not close over symbol owners"
                if manifest_contract
                else "272 identities do not close over the exact 270 owners"
            ),
        )
    expected_sharing_group_count = (
        len(observed_shared_symbols)
        if manifest_contract
        else len(shared_symbols)
    )
    if _integer(
        _field(manifest, "c_symbol_sharing_group_count", "DSO-manifest"),
        "DSO-manifest.c_symbol_sharing_group_count",
    ) != expected_sharing_group_count:
        _fail("DSO", "manifest sharing-group count is stale")

    generated_wrapper_symbols = set(
        _string_list(
            _field(manifest, "generated_wrapper_symbols", "DSO-manifest"),
            "DSO-manifest.generated_wrapper_symbols",
        )
    )
    if (
        _integer(
            _field(
                manifest,
                "generated_wrapper_symbol_count",
                "DSO-manifest",
            ),
            "DSO-manifest.generated_wrapper_symbol_count",
        )
        != len(generated_wrapper_symbols)
        or generated_wrapper_symbols != generated_owner_symbols
    ):
        _fail(
            "DSO",
            "generated-wrapper inventory disagrees with symbol owners",
        )
    if _integer(
        _field(manifest, "c_wrapper_entity_count", "DSO-manifest"),
        "DSO-manifest.c_wrapper_entity_count",
    ) != len(generated_wrapper_symbols):
        _fail("DSO", "generated wrapper entity total is inconsistent")

    handwritten_fmgr_export_symbols = set(
        _string_list(
            _field(
                manifest,
                "handwritten_fmgr_export_symbols",
                "DSO-manifest",
            ),
            "DSO-manifest.handwritten_fmgr_export_symbols",
        )
    )
    if _integer(
        _field(
            manifest,
            "handwritten_fmgr_export_symbol_count",
            "DSO-manifest",
        ),
        "DSO-manifest.handwritten_fmgr_export_symbol_count",
    ) != len(handwritten_fmgr_export_symbols):
        _fail("DSO", "handwritten FMGR export total is inconsistent")
    handwritten_fmgr_export_external_symbols = set(
        _string_list(
            _field(
                manifest,
                "handwritten_fmgr_export_external_symbols",
                "DSO-manifest",
            ),
            "DSO-manifest.handwritten_fmgr_export_external_symbols",
            allow_empty=manifest_contract,
        )
    )
    if (
        _integer(
            _field(
                manifest,
                "handwritten_fmgr_export_external_symbol_count",
                "DSO-manifest",
            ),
            "DSO-manifest.handwritten_fmgr_export_external_symbol_count",
        )
        != len(external_symbols)
        or handwritten_fmgr_export_external_symbols
        != set(external_symbols)
    ):
        if manifest_contract:
            _fail(
                "DSO",
                "external handwritten FMGR export inventory is inconsistent",
            )
        _fail(
            "DSO",
            "handwritten FMGR export scan must contain exactly the "
            "reviewed bioseq_external handler",
        )
    handwritten_fmgr_export_owners = _array(
        _field(
            manifest,
            "handwritten_fmgr_export_owners",
            "DSO-manifest",
        ),
        "DSO-manifest.handwritten_fmgr_export_owners",
    )
    if _integer(
        _field(
            manifest,
            "handwritten_fmgr_export_owner_count",
            "DSO-manifest",
        ),
        "DSO-manifest.handwritten_fmgr_export_owner_count",
    ) != len(handwritten_fmgr_export_owners):
        _fail("DSO", "handwritten FMGR export owner total is inconsistent")
    scanned_handwritten_fmgr_export_symbols: set[str] = set()
    for index, raw_owner in enumerate(handwritten_fmgr_export_owners):
        context = f"DSO-manifest.handwritten_fmgr_export_owners[{index}]"
        owner = _object(raw_owner, context)
        symbol = _string(
            _field(owner, "symbol", context),
            f"{context}.symbol",
        )
        macro = _string(
            _field(owner, "macro", context),
            f"{context}.macro",
        )
        if (
            macro not in FMGR_OWNER_MACROS
            or symbol in scanned_handwritten_fmgr_export_symbols
        ):
            _fail(
                context,
                "handwritten FMGR export owner is invalid or duplicated",
            )
        scanned_handwritten_fmgr_export_symbols.add(symbol)
        _string(
            _field(owner, "source_module", context),
            f"{context}.source_module",
        )
        _string(
            _field(owner, "source_path", context),
            f"{context}.source_path",
        )
        if _integer(
            _field(owner, "source_line", context),
            f"{context}.source_line",
        ) == 0:
            _fail(context, "handwritten FMGR export owner line is zero")
    if scanned_handwritten_fmgr_export_symbols != (
        handwritten_fmgr_export_symbols
        | handwritten_fmgr_export_external_symbols
    ):
        _fail(
            "DSO",
            "handwritten FMGR export owners disagree with the "
            "ownership partitions",
        )
    if not handwritten_fmgr_export_symbols <= main_symbols:
        _fail(
            "DSO",
            "handwritten FMGR export is not a main-extension owner",
        )
    if generated_wrapper_symbols & handwritten_fmgr_export_symbols:
        _fail("DSO", "generated and handwritten FMGR ownership overlap")
    if (
        generated_wrapper_symbols | handwritten_fmgr_export_symbols
    ) != main_symbols:
        _fail(
            "DSO",
            "generated plus handwritten FMGR ownership does not exactly "
            "cover the main-extension symbols",
        )
    if require_complete_codegen and (
        handwritten_fmgr_export_symbols
        or generated_wrapper_symbols != main_symbols
    ):
        _fail(
            "DSO",
            "final code-generation closure still contains handwritten "
            "FMGR export owners",
        )
    visible_symbols: dict[str, list[DynamicSymbol]] = {}
    for symbol in inventory:
        if symbol.defined and symbol.dynamic_visible:
            visible_symbols.setdefault(symbol.name, []).append(symbol)
    actual_fmgr_symbols = {
        name.removeprefix("pg_finfo_")
        for name in visible_symbols
        if name.startswith("pg_finfo_")
    }
    expected_fmgr_symbols = main_symbols | set(external_symbols)
    if actual_fmgr_symbols != expected_fmgr_symbols:
        expected_description = (
            "the manifest-owned symbols"
            if manifest_contract
            else "the main symbols plus the bioseq_external handler"
        )
        _fail(
            "DSO",
            "fmgr/finfo exports do not exactly match "
            + expected_description,
        )
    for symbol in expected_fmgr_symbols:
        _one_dynamic_export(visible_symbols, symbol)
        _one_dynamic_export(visible_symbols, f"pg_finfo_{symbol}")
    expected_fmgr_exports = expected_fmgr_symbols | {
        f"pg_finfo_{symbol}" for symbol in expected_fmgr_symbols
    }
    reserved_abi_exports = (
        normalized_allowed_dso_exports & expected_fmgr_exports
    )
    if reserved_abi_exports:
        _fail(
            sorted(reserved_abi_exports)[0],
            "non-fmgr ABI allowlist overlaps the fmgr export inventory",
        )
    if manifest_contract:
        expected_dynamic_exports = (
            expected_fmgr_exports | normalized_allowed_dso_exports
        )
        actual_dynamic_exports = set(visible_symbols)
        if actual_dynamic_exports != expected_dynamic_exports:
            missing_exports = sorted(
                expected_dynamic_exports - actual_dynamic_exports
            )
            unexpected_exports = sorted(
                actual_dynamic_exports - expected_dynamic_exports
            )
            missing_sample = ", ".join(missing_exports[:8])
            unexpected_sample = ", ".join(unexpected_exports[:8])
            _fail(
                "DSO",
                "defined dynamic export surface is not exact: "
                f"missing={len(missing_exports)} [{missing_sample}]; "
                f"unexpected={len(unexpected_exports)} "
                f"[{unexpected_sample}]",
            )
        for symbol in normalized_allowed_dso_exports:
            _one_dynamic_export(visible_symbols, symbol)

    for symbol in required_absent_symbols:
        if symbol in visible_symbols or f"pg_finfo_{symbol}" in visible_symbols:
            _fail(symbol, "prohibited symbol remains dynamically visible")
    for symbol in required_present_symbols:
        if symbol not in main_symbols:
            _fail(symbol, "target BPCHAR owner is absent from the manifest")
        _one_dynamic_export(visible_symbols, symbol)
        _one_dynamic_export(visible_symbols, f"pg_finfo_{symbol}")
    if not manifest_contract:
        for symbol in {"seq_concat", "array_cat"}:
            if symbol not in main_symbols:
                _fail(symbol, "distinct concat entrypoint is absent")

    return {
        "main_identity_count": len(identities),
        "main_symbol_count": len(main_symbols),
        "external_symbol_count": len(external_symbols),
        "generated_wrapper_symbol_count": len(generated_wrapper_symbols),
        "handwritten_fmgr_export_symbol_count": len(
            handwritten_fmgr_export_symbols
        ),
        "dynamic_fmgr_symbol_count": len(actual_fmgr_symbols),
        "allowed_dso_export_count": len(
            normalized_allowed_dso_exports
        ),
        "dynamic_export_count": len(visible_symbols),
    }


def _artifact_sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _path_artifact_sha256(path: Path, context: str) -> str:
    try:
        _, raw, _ = _authenticated_file_bytes(
            path,
            context,
            require_single_link=False,
        )
        return _artifact_sha256(raw)
    except (OSError, EvidenceError) as error:
        if isinstance(error, EvidenceError):
            raise
        raise EvidenceError(
            f"{context}: cannot read artifact {path}: {error}"
        ) from error


def _require_byte_equality(
    actual: bytes,
    reviewed_path: Path,
    context: str,
    *,
    actual_path: Path | None = None,
) -> None:
    try:
        (
            canonical_reviewed,
            reviewed,
            reviewed_metadata,
        ) = _authenticated_file_bytes(
            reviewed_path,
            f"{context}.reviewed",
            require_single_link=True,
        )
        if actual_path is not None:
            (
                canonical_actual,
                authenticated_actual,
                actual_metadata,
            ) = _authenticated_file_bytes(
                actual_path,
                f"{context}.candidate",
                require_single_link=True,
            )
            if authenticated_actual != actual:
                _fail(
                    context,
                    "candidate bytes changed after their initial read",
                )
            if (
                actual_metadata.st_dev,
                actual_metadata.st_ino,
            ) == (
                reviewed_metadata.st_dev,
                reviewed_metadata.st_ino,
            ):
                _fail(
                    context,
                    "reviewed artifact aliases the generated candidate "
                    "instead of being an independently promoted file: "
                    f"{reviewed_path}",
                )
            del canonical_actual
    except OSError as error:
        raise EvidenceError(
            f"cannot authenticate reviewed artifact {reviewed_path}: {error}"
        ) from error
    del canonical_reviewed
    if actual != reviewed:
        _fail(context, f"generated artifact differs byte-for-byte from {reviewed_path}")


def _write_report(path: Path | None, report: dict[str, Any]) -> None:
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if path is None:
        print(text, end="")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(text, encoding="utf-8")
    temporary.replace(path)


def _load_dynamic_symbol_input(
    dso: Path | None,
    symbol_dump: Path | None,
    symbol_format: str,
) -> tuple[list[DynamicSymbol], str, str]:
    if dso is not None:
        try:
            dso_raw = dso.read_bytes()
        except OSError as error:
            raise EvidenceError(f"cannot read {dso}: {error}") from error
        dynamic_symbols, dso_inspector = inspect_dso_symbols(dso)
        return (
            dynamic_symbols,
            dso_inspector,
            _artifact_sha256(dso_raw),
        )

    if symbol_dump is None:
        _fail("command-line", "missing --dso or --symbol-dump")
    try:
        symbol_dump_raw = symbol_dump.read_bytes()
        symbol_dump_text = symbol_dump_raw.decode("utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise EvidenceError(
            f"cannot read {symbol_dump}: {error}"
        ) from error
    if symbol_format == "readelf":
        dynamic_symbols = parse_readelf_symbols(symbol_dump_text)
    elif symbol_format == "darwin-nm":
        dynamic_symbols = parse_darwin_nm_symbols(symbol_dump_text)
    elif symbol_format == "coff-exports":
        dynamic_symbols = parse_llvm_coff_exports(symbol_dump_text)
    elif symbol_format == "dumpbin":
        dynamic_symbols = parse_dumpbin_exports(symbol_dump_text)
    else:
        dynamic_symbols = parse_nm_symbols(
            symbol_dump_text,
            posix=symbol_format == "nm-posix",
        )
    return (
        dynamic_symbols,
        symbol_format,
        _artifact_sha256(symbol_dump_raw),
    )


STATIC_VALIDATOR_ARGUMENTS = (
    ("--production-ledger", "production_ledger"),
    ("--test-indexes-ledger", "test_indexes_ledger"),
    ("--reviewed-production-ledger", "reviewed_production_ledger"),
    ("--reviewed-test-indexes-ledger", "reviewed_test_indexes_ledger"),
    ("--production-source-list", "production_source_list"),
    ("--test-indexes-source-list", "test_indexes_source_list"),
    ("--production-compile-database", "production_compile_database"),
    ("--test-indexes-compile-database", "test_indexes_compile_database"),
    ("--production-compile-graph", "production_compile_graph"),
    ("--test-indexes-compile-graph", "test_indexes_compile_graph"),
    ("--production-pcm-manifest", "production_pcm_manifest"),
    ("--test-indexes-pcm-manifest", "test_indexes_pcm_manifest"),
    ("--production-compiler-facts", "production_compiler_facts"),
    ("--test-indexes-compiler-facts", "test_indexes_compiler_facts"),
    (
        "--production-compiler-facts-producer-executable",
        "production_compiler_facts_producer_executable",
    ),
    (
        "--test-indexes-compiler-facts-producer-executable",
        "test_indexes_compiler_facts_producer_executable",
    ),
    ("--provenance", "provenance"),
    ("--production-sql-manifest", "production_sql_manifest"),
    ("--test-indexes-sql-manifest", "test_indexes_sql_manifest"),
)

RUNTIME_VALIDATOR_ARGUMENTS = (
    ("--runtime-policy", "runtime_policy"),
    ("--runtime-matrix", "runtime_matrix"),
    ("--reviewed-runtime-matrix", "reviewed_runtime_matrix"),
    ("--production-case-plan", "production_case_plan"),
    ("--test-indexes-case-plan", "test_indexes_case_plan"),
    (
        "--production-runtime-inventory",
        "production_runtime_inventory",
    ),
    (
        "--test-indexes-runtime-inventory",
        "test_indexes_runtime_inventory",
    ),
    (
        "--runtime-inventory-producer-executable",
        "runtime_inventory_producer_executable",
    ),
    ("--production-profile-dso", "production_profile_dso"),
    ("--test-indexes-profile-dso", "test_indexes_profile_dso"),
    ("--runtime-control-dso", "runtime_control_dso"),
    ("--production-stage-manifest", "production_stage_manifest"),
    (
        "--test-indexes-stage-manifest",
        "test_indexes_stage_manifest",
    ),
    ("--runtime-producer-executable", "runtime_producer_executable"),
    (
        "--production-runtime-test-executable",
        "production_runtime_test_executable",
    ),
    (
        "--test-indexes-runtime-test-executable",
        "test_indexes_runtime_test_executable",
    ),
)

ACTIVE_DSO_VALIDATOR_ARGUMENTS = (
    ("--sql-manifest", "sql_manifest"),
    ("--dso", "dso"),
    ("--symbol-dump", "symbol_dump"),
    ("--dso-contract", "dso_contract"),
    ("--allowed-dso-export", "allowed_dso_export"),
)


def _present_validator_arguments(
    arguments: argparse.Namespace,
    entries: tuple[tuple[str, str], ...],
) -> list[str]:
    return [
        flag
        for flag, attribute in entries
        if getattr(arguments, attribute, None) is not None
    ]


def _require_validator_arguments(
    arguments: argparse.Namespace,
    entries: tuple[tuple[str, str], ...],
) -> None:
    missing = [
        flag
        for flag, attribute in entries
        if getattr(arguments, attribute, None) is None
    ]
    if missing:
        _fail("command-line", "missing " + ", ".join(missing))


def _reject_validator_arguments(
    arguments: argparse.Namespace,
    entries: tuple[tuple[str, str], ...],
    mode: str,
) -> None:
    present = _present_validator_arguments(arguments, entries)
    if present:
        _fail(
            "command-line",
            f"arguments are not valid for {mode}: "
            + ", ".join(present),
        )


def _require_dynamic_symbol_source(
    arguments: argparse.Namespace,
) -> None:
    source_count = sum(
        value is not None
        for value in (arguments.dso, arguments.symbol_dump)
    )
    if source_count == 0:
        _fail("command-line", "missing --dso or --symbol-dump")
    if source_count != 1:
        _fail(
            "command-line",
            "requires exactly one of --dso or --symbol-dump",
        )


def _validate_command_line(arguments: argparse.Namespace) -> str:
    if arguments.self_test:
        _reject_validator_arguments(
            arguments,
            STATIC_VALIDATOR_ARGUMENTS
            + RUNTIME_VALIDATOR_ARGUMENTS
            + ACTIVE_DSO_VALIDATOR_ARGUMENTS,
            "self-test",
        )
        return "self_test"

    if arguments.dso_only:
        _reject_validator_arguments(
            arguments,
            STATIC_VALIDATOR_ARGUMENTS + RUNTIME_VALIDATOR_ARGUMENTS,
            "dso-only",
        )
        if arguments.sql_manifest is None:
            _fail("command-line", "missing --sql-manifest")
        if (
            arguments.allowed_dso_export is not None
            and arguments.dso_contract != "manifest"
        ):
            _fail(
                "command-line",
                "--allowed-dso-export requires --dso-contract manifest",
            )
        _require_dynamic_symbol_source(arguments)
        return "dso"

    _require_validator_arguments(
        arguments,
        STATIC_VALIDATOR_ARGUMENTS,
    )
    if arguments.static_only:
        _reject_validator_arguments(
            arguments,
            RUNTIME_VALIDATOR_ARGUMENTS
            + ACTIVE_DSO_VALIDATOR_ARGUMENTS,
            "static-only",
        )
        return "static"

    _require_validator_arguments(
        arguments,
        RUNTIME_VALIDATOR_ARGUMENTS,
    )
    if arguments.evidence_only:
        _reject_validator_arguments(
            arguments,
            ACTIVE_DSO_VALIDATOR_ARGUMENTS,
            "evidence-only",
        )
        return "evidence"

    if arguments.sql_manifest is None:
        _fail("command-line", "missing --sql-manifest")
    _require_dynamic_symbol_source(arguments)
    if arguments.dso_contract is not None:
        _fail(
            "command-line",
            "--dso-contract is valid only with --dso-only",
        )
    if arguments.allowed_dso_export is not None:
        _fail(
            "command-line",
            "--allowed-dso-export is valid only with --dso-only",
        )
    return "full"


def _run_runtime_artifact_graph_self_tests(
    *,
    sql_identity: str,
    fault_key: str,
    runtime_policy_raw: bytes,
    runtime_ledgers: dict[str, dict[str, Any]],
    runtime_ledger_hashes: dict[str, str],
    runtime_inventories: dict[str, dict[str, Any]],
    runtime_inventory_hashes: dict[str, str],
    runtime_producer_sha256: str,
    require_failure: Any,
) -> None:
    runtime_lifecycle = _expected_runtime_lifecycle(fault_key)
    postgres_cleanup_event_names = (
        []
        if fault_key == BACKEND_SCOPED_VACUUM_CLEANUP_FAULT_KEY
        else ["fmgr_abort_observed"]
    )
    with tempfile.TemporaryDirectory(
        prefix="bioseq-runtime-authenticated-self-test-",
    ) as temporary_directory:
        root = Path(temporary_directory).resolve()
        evidence_root = root / "evidence"
        current_root = root / "current"
        matrix_path = root / "runtime-matrix.json"

        def write(path: Path, raw: bytes) -> Path:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw)
            return path.resolve()

        def publish(
            category: str,
            profile: str,
            stem: str,
            suffix: str,
            raw: bytes,
        ) -> tuple[Path, str, str]:
            digest = _artifact_sha256(raw)
            path = write(
                evidence_root
                / category
                / profile
                / f"{stem}.{digest}.{suffix}",
                raw,
            )
            return path, digest, str(path.relative_to(root))

        owner_order = GUC_OWNER_ORDER
        owner_counts = GUC_PRODUCTION_OWNER_COUNTS
        owner_hashes = {
            profile: {
                owner: _artifact_sha256(
                    f"runtime-guc-owner:{profile}:{owner}".encode()
                )
                for owner in owner_order
            }
            for profile in ("production", "test_indexes")
        }
        registry_hashes = {
            profile: _artifact_sha256(
                f"runtime-guc-registry:{profile}".encode()
            )
            for profile in ("production", "test_indexes")
        }
        source_hashes = {
            owner: _artifact_sha256(
                f"runtime-guc-source:{owner}".encode()
            )
            for owner in owner_order
        }

        def guc_registration(
            owner_key: str,
            member: str,
            ordinal: int,
            *,
            test_only: bool = False,
        ) -> dict[str, Any]:
            group = (
                "bioseq"
                if owner_key == "bioseq"
                else f"bioseq.{owner_key}"
            )
            source_module = GUC_SOURCE_MODULES[owner_key]
            name = f"{group}.{member}"
            enum_setting = (
                name == "bioseq.testing.persisted_index.failure_mode"
            )
            flags = (
                ["no_show_all", "not_in_sample"] if test_only else []
            )
            boot_value = "none" if enum_setting else "off"
            generated_live_authority = {
                "vartype": "enum" if enum_setting else "bool",
                "context": "superuser" if test_only else "user",
                "unit": None,
                "boot_value": boot_value,
                "reset_value": boot_value,
                "current_value": boot_value,
                "minimum": None,
                "maximum": None,
                "visible_enum_values": (
                    ["none", "unsupported_vector_shape"]
                    if enum_setting
                    else []
                ),
                "short_description": f"Fixture setting {name}.",
                "extra_description": None,
                "raw_flags": 0x000084 if test_only else 0,
                "visible_in_show_all": not test_only,
                "included_in_sample": not test_only,
                "hooks": {
                    hook: {
                        "symbol": None,
                        "present": False,
                        "behavior": "none",
                    }
                    for hook in ("check", "assign", "show")
                },
            }
            return {
                "registration_ordinal": ordinal,
                "name": name,
                "owner": owner_key,
                "member": member.replace(".", "_"),
                "kind": "enum" if enum_setting else "bool",
                "cpp_storage": f"fixture::{owner_key}::{member}",
                "storage_kind": "int32_enum" if enum_setting else "bool",
                "source_module": source_module,
                "source_path": GUC_SOURCE_PATHS[owner_key],
                "source_sha256": source_hashes[owner_key],
                "default_expression": "0" if enum_setting else "false",
                "minimum_expression": None,
                "maximum_expression": None,
                "context": "superuser_set" if test_only else "user_set",
                "flags": flags,
                "unit": None,
                "hooks": [None, None, None],
                "feature_predicate": (
                    "defined(BIOSEQ_TEST_INDEXES)"
                    if test_only
                    else "true"
                ),
                "enum_default": "none" if enum_setting else None,
                "enum_rows_name_wire_condition_alias_hidden_compiled": (
                    [
                        [
                            "none",
                            "0",
                            "defined(BIOSEQ_TEST_INDEXES)",
                            None,
                            False,
                            True,
                        ],
                        [
                            "unsupported_vector_shape",
                            "1",
                            "defined(BIOSEQ_TEST_INDEXES)",
                            None,
                            False,
                            True,
                        ],
                    ]
                    if enum_setting
                    else []
                ),
                "generated_live_authority": generated_live_authority,
            }

        common_registrations: list[dict[str, Any]] = []
        for ordinal in range(23):
            common_registrations.append(
                guc_registration(
                    "bioseq",
                    f"fixture_bioseq_{ordinal:02d}",
                    ordinal,
                )
            )
        for ordinal in range(26, 29):
            common_registrations.append(
                guc_registration(
                    "bioseq",
                    f"fixture_bioseq_{ordinal:02d}",
                    ordinal,
                )
            )
        for offset, ordinal in enumerate(range(29, 32)):
            common_registrations.append(
                guc_registration(
                    "scheduler",
                    f"fixture_scheduler_{offset:02d}",
                    ordinal,
                )
            )
        common_registrations.append(
            guc_registration("storage", "fixture_storage_00", 32)
        )
        for offset, ordinal in enumerate(range(33, 76)):
            common_registrations.append(
                guc_registration(
                    "aligner",
                    f"fixture_aligner_{offset:02d}",
                    ordinal,
                )
            )
        test_only_registrations = [
            guc_registration(
                "bioseq",
                member,
                ordinal,
                test_only=True,
            )
            for member, ordinal in zip(
                (
                    "testing.chunked_scanner.enabled",
                    "testing.in_memory_scanner.enabled",
                    "testing.persisted_index.failure_mode",
                ),
                range(23, 26),
                strict=True,
            )
        ]

        profile_delta_validation = {
            "production_registry_content_sha256": (
                registry_hashes["production"]
            ),
            "test_indexes_registry_content_sha256": (
                registry_hashes["test_indexes"]
            ),
            "added_in_test_indexes": list(GUC_TEST_ONLY_NAMES),
            "removed_from_test_indexes": [],
            "changed_in_test_indexes": [],
            "owner_descriptor_profile_invariant": {
                "bioseq": False,
                "scheduler": True,
                "storage": True,
                "aligner": True,
            },
            "owner_artifacts": {
                owner: {
                    "production_content_sha256": owner_hashes[
                        "production"
                    ][owner],
                    "test_indexes_content_sha256": owner_hashes[
                        "test_indexes"
                    ][owner],
                }
                for owner in owner_order
            },
        }

        def guc_manifest(profile: str) -> dict[str, Any]:
            registrations = list(common_registrations)
            if profile == "test_indexes":
                registrations.extend(test_only_registrations)
                registrations.sort(
                    key=lambda registration: registration[
                        "registration_ordinal"
                    ]
                )
            facets = []
            for owner in owner_order:
                source_module = GUC_SOURCE_MODULES[owner]
                facets.append(
                    {
                        "owner": owner,
                        "source_module": source_module,
                        "generated_module": (
                            f"bioseq.postgres.sql.generated.{profile}.{owner}.gucs"
                        ),
                        "content_sha256": owner_hashes[profile][owner],
                        "registration_count": owner_counts[owner]
                        + (
                            3
                            if profile == "test_indexes"
                            and owner == "bioseq"
                            else 0
                        ),
                    }
                )
            return {
                "schema_revision": 1,
                "authority_mode": "annotated_cpp",
                "compiler_profile": profile,
                "script_sha256": _artifact_sha256(
                    f"runtime-sql-{profile}".encode()
                ),
                "guc_generation": {
                    "description_authority": "cleaned_doxygen_only",
                    "owner_facets": facets,
                    "profile": profile,
                    "profile_delta_validation": profile_delta_validation,
                    "registration_count": len(registrations),
                    "registrations": registrations,
                    "registry_content_sha256": (
                        registry_hashes[profile]
                    ),
                    "registry_module": (
                        f"bioseq.postgres.sql.generated.{profile}.guc_registry"
                    ),
                },
            }

        runtime_manifests = {
            profile: guc_manifest(profile)
            for profile in ("production", "test_indexes")
        }
        runtime_manifest_raw = {
            profile: _canonical_json_bytes(manifest)
            for profile, manifest in runtime_manifests.items()
        }
        runtime_guc_properties: dict[
            str, tuple[list[dict[str, Any]], str]
        ] = {}
        for profile, manifest in runtime_manifests.items():
            generation = manifest["guc_generation"]
            facets = {
                facet["owner"]: facet
                for facet in generation["owner_facets"]
            }
            properties: list[dict[str, Any]] = []
            for registration in generation["registrations"]:
                generated_live_authority = registration[
                    "generated_live_authority"
                ]
                owner_key = _guc_owner_key(
                    registration["owner"],
                    "self-test GUC registration",
                )
                properties.append(
                    {
                        "registration_ordinal": registration[
                            "registration_ordinal"
                        ],
                        "name": registration["name"],
                        "manifest_registration_sha256": (
                            _artifact_sha256(
                                _canonical_json_bytes(registration)
                            )
                        ),
                        "registration": registration,
                        "profile_projection": {
                            "profile": profile,
                            "registry_module": generation[
                                "registry_module"
                            ],
                            "registry_content_sha256": generation[
                                "registry_content_sha256"
                            ],
                            "profile_delta_validation_sha256": (
                                _artifact_sha256(
                                    _canonical_json_bytes(
                                        profile_delta_validation
                                    )
                                )
                            ),
                            "owner_facet": facets[owner_key],
                        },
                        "pg_settings": generated_live_authority,
                    }
                )
            runtime_guc_properties[profile] = (
                properties,
                _artifact_sha256(_canonical_json_bytes(properties)),
            )
        for profile in ("production", "test_indexes"):
            runtime_ledgers[profile]["sql_manifest_sha256"] = (
                _artifact_sha256(runtime_manifest_raw[profile])
            )

        runtime_hashes: dict[str, str] = {
            "production_ledger_sha256": runtime_ledger_hashes[
                "production"
            ],
            "test_indexes_ledger_sha256": runtime_ledger_hashes[
                "test_indexes"
            ],
            "production_sql_manifest_sha256": runtime_ledgers[
                "production"
            ]["sql_manifest_sha256"],
            "test_indexes_sql_manifest_sha256": runtime_ledgers[
                "test_indexes"
            ]["sql_manifest_sha256"],
            "production_runtime_inventory_sha256": (
                runtime_inventory_hashes["production"]
            ),
            "test_indexes_runtime_inventory_sha256": (
                runtime_inventory_hashes["test_indexes"]
            ),
            "runtime_policy_sha256": _artifact_sha256(
                runtime_policy_raw
            ),
            "production_dso_sha256": _artifact_sha256(
                b"runtime-production-dso"
            ),
            "test_indexes_dso_sha256": _artifact_sha256(
                b"runtime-test-indexes-dso"
            ),
            "runtime_control_dso_sha256": _artifact_sha256(
                b"runtime-control-dso"
            ),
            "runtime_producer_executable_sha256": (
                runtime_producer_sha256
            ),
            "production_test_executable_sha256": _artifact_sha256(
                b"runtime-production-test-executable"
            ),
            "test_indexes_test_executable_sha256": _artifact_sha256(
                b"runtime-test-indexes-test-executable"
            ),
        }
        current_paths: dict[str, Path] = {
            "production_ledger": write(
                current_root / "production-ledger.json",
                b"runtime-ledger:production",
            ),
            "test_indexes_ledger": write(
                current_root / "test-indexes-ledger.json",
                b"runtime-ledger:test_indexes",
            ),
            "production_sql_manifest": write(
                current_root / "production-sql-manifest.json",
                runtime_manifest_raw["production"],
            ),
            "test_indexes_sql_manifest": write(
                current_root / "test-indexes-sql-manifest.json",
                runtime_manifest_raw["test_indexes"],
            ),
            "production_dso": write(
                current_root / "production" / "bioseq.so",
                b"runtime-production-dso",
            ),
            "test_indexes_dso": write(
                current_root / "test_indexes" / "bioseq.so",
                b"runtime-test-indexes-dso",
            ),
            "runtime_control_dso": write(
                current_root / "runtime-control.so",
                b"runtime-control-dso",
            ),
            "runtime_producer": write(
                current_root / "runtime-evidence.py",
                b"runtime-evidence-producer",
            ),
            "production_test_executable": write(
                current_root / "production-runtime-tests",
                b"runtime-production-test-executable",
            ),
            "test_indexes_test_executable": write(
                current_root / "test-indexes-runtime-tests",
                b"runtime-test-indexes-test-executable",
            ),
        }
        if any(
            _path_artifact_sha256(path, name)
            != runtime_hashes[f"{name}_sha256"]
            for name, path in {
                "production_ledger": current_paths["production_ledger"],
                "test_indexes_ledger": current_paths[
                    "test_indexes_ledger"
                ],
                "production_sql_manifest": current_paths[
                    "production_sql_manifest"
                ],
                "test_indexes_sql_manifest": current_paths[
                    "test_indexes_sql_manifest"
                ],
                "production_dso": current_paths["production_dso"],
                "test_indexes_dso": current_paths["test_indexes_dso"],
                "runtime_control_dso": current_paths[
                    "runtime_control_dso"
                ],
                "runtime_producer_executable": current_paths[
                    "runtime_producer"
                ],
                "production_test_executable": current_paths[
                    "production_test_executable"
                ],
                "test_indexes_test_executable": current_paths[
                    "test_indexes_test_executable"
                ],
            }.items()
        ):
            _fail("self-test", "current runtime fixture hashes differ")

        plans: dict[str, dict[str, Any]] = {}
        plan_cases: dict[str, dict[str, Any]] = {}
        sql_script_hashes: dict[str, str] = {}
        stage_hashes: dict[str, str] = {}
        suite_hashes: dict[str, str] = {}
        stage_manifests: dict[str, dict[str, Any]] = {}
        for profile in ("production", "test_indexes"):
            inventory_entry = runtime_inventories[profile]["entries"][
                (sql_identity, fault_key)
            ]
            case_id = f"runtime-{profile}"
            catalog_entry = {
                "case_id": case_id,
                "sql_identity": sql_identity,
                "site_id": inventory_entry["site_id"],
                "ledger_site_sha256": inventory_entry[
                    "ledger_site_sha256"
                ],
                "invocation_key": f"runtime-invocation-{profile}",
                "fault_key": fault_key,
                "relation": inventory_entry["relation"],
                "probe_contract": inventory_entry["probe_contract"],
                "postgres_severity": "error",
                "expected": {
                    "backend_outcome": "recoverable_error",
                    "destructor_sentinels": [
                        f"runtime_sentinel_{profile}"
                    ],
                    "cleanup_events": [],
                    "postgres_cleanup_events": list(
                        postgres_cleanup_event_names
                    ),
                    "sqlstate": "XX000",
                    "message": f"runtime error {profile}",
                    "recovery_postcondition": (
                        f"runtime recovery {profile}"
                    ),
                },
            }
            execution = {
                "target_regprocedure": sql_identity,
                "setup_sql": [],
                "invocation_sql": "SELECT 1",
                "post_error_sql": [],
                "recovery_sql": "SELECT 'ok'",
                "recovery_expected_rows": [["ok", profile]],
                "cleanup_sql": [],
            }
            if (
                fault_key
                == BACKEND_SCOPED_VACUUM_CLEANUP_FAULT_KEY
            ):
                execution.update(
                    {
                        "pre_fault_control_sql": [
                            "VACUUM runtime_fixture"
                        ],
                        "pre_fault_control_recovery_sql": (
                            "SELECT 'control-ok'"
                        ),
                        "pre_fault_control_recovery_expected_rows": [
                            ["control-ok", profile]
                        ],
                    }
                )
            plan_case = {
                "catalog": catalog_entry,
                "execution": execution,
            }
            plan = {
                "schema_revision": 2,
                "profile": profile,
                "probe_abi_revision": 2,
                "template_sha256": _artifact_sha256(
                    f"runtime-template-{profile}".encode()
                ),
                "ledger_sha256": runtime_ledger_hashes[profile],
                "runtime_inventory_sha256": (
                    runtime_inventory_hashes[profile]
                ),
                "sql_manifest_sha256": runtime_ledgers[profile][
                    "sql_manifest_sha256"
                ],
                "runtime_policy_sha256": _artifact_sha256(
                    runtime_policy_raw
                ),
                "cases": [plan_case],
            }
            plan_raw = _canonical_json_bytes(plan)
            plan_path = write(
                current_root / f"{profile}-case-plan.json",
                plan_raw,
            )
            current_paths[f"{profile}_case_plan"] = plan_path
            runtime_hashes[f"{profile}_case_plan_sha256"] = (
                _artifact_sha256(plan_raw)
            )
            plans[profile] = plan
            plan_cases[profile] = plan_case

            stage_root = root / "stages" / profile
            for directory in (
                stage_root / "bin",
                stage_root / "share" / "extension",
                stage_root / "lib",
            ):
                directory.mkdir(parents=True, exist_ok=True)
            source_layout = {
                "bindir": "/fixture/bin",
                "sharedir": "/fixture/share",
                "pkglibdir": "/fixture/lib",
            }
            staged_layout = {
                "bindir": "bin",
                "sharedir": "share",
                "pkglibdir": "lib",
            }
            tool_signatures: list[dict[str, Any]] = []
            expected_tool_paths: dict[str, str] = {}
            runtime_entries: list[dict[str, Any]] = []
            for tool in POSTGRESQL_TOOLS:
                relative = f"bin/{tool}"
                tool_path = write(
                    stage_root / relative,
                    f"{tool}-postgresql-17.10".encode(),
                )
                tool_path.chmod(0o755)
                signature = {
                    "tool": tool,
                    "staged_path": relative,
                    "version": f"{tool} (PostgreSQL) 17.10 self-test",
                    "sha256": _path_artifact_sha256(
                        tool_path,
                        f"self-test-{tool}",
                    ),
                }
                tool_signatures.append(signature)
                expected_tool_paths[tool] = relative
                runtime_entries.append(
                    {
                        "root": "bindir",
                        "path": tool,
                        "kind": "file",
                        "source_kind": "file",
                        "source_path": f"/fixture/bin/{tool}",
                        "source_link_target": None,
                        "resolved_source_path": f"/fixture/bin/{tool}",
                        "size": tool_path.stat().st_size,
                        "sha256": signature["sha256"],
                    }
                )
            runtime_entries.sort(key=lambda entry: str(entry["path"]))
            runtime_aggregate = _artifact_sha256(
                "".join(
                    _stage_runtime_entry_record(entry)
                    for entry in runtime_entries
                ).encode("utf-8")
            )
            control_bytes = b"runtime-bioseq-control"
            sql_bytes = f"runtime-sql-{profile}".encode()
            dso_bytes = (
                b"runtime-production-dso"
                if profile == "production"
                else b"runtime-test-indexes-dso"
            )
            control_path = write(
                stage_root / "share" / "extension" / "bioseq.control",
                control_bytes,
            )
            sql_path = write(
                stage_root / "share" / "extension" / "bioseq--1.0.sql",
                sql_bytes,
            )
            installed_dso_path = write(
                stage_root / "lib" / "bioseq.so",
                dso_bytes,
            )
            installed_control_dso_path = write(
                stage_root / "lib" / "runtime-control.so",
                b"runtime-control-dso",
            )
            control_sha256 = _path_artifact_sha256(
                control_path,
                "self-test-control",
            )
            sql_script_hashes[profile] = _path_artifact_sha256(
                sql_path,
                "self-test-sql",
            )
            library_sha256 = _path_artifact_sha256(
                installed_dso_path,
                "self-test-stage-dso",
            )
            package_aggregate = _stage_package_aggregate(
                name="bioseq",
                module_pathname_override=None,
                control_filename="bioseq.control",
                control_sha256=control_sha256,
                install_sql_filename="bioseq--1.0.sql",
                install_sql_sha256=sql_script_hashes[profile],
                library_filename="bioseq.so",
                library_sha256=library_sha256,
            )
            stage_manifest = {
                "schema": STAGE_MANIFEST_SCHEMA,
                "schema_revision": STAGE_MANIFEST_SCHEMA_REVISION,
                "postgresql_version": "PostgreSQL 17.10 self-test",
                "postgresql_configure": "--self-test",
                "source": source_layout,
                "staged": staged_layout,
                "runtime_closure": {
                    "algorithm": RUNTIME_CLOSURE_ALGORITHM,
                    "schema_revision": RUNTIME_CLOSURE_SCHEMA_REVISION,
                    "entry_count": len(runtime_entries),
                    "aggregate_sha256": runtime_aggregate,
                    "roots": {
                        name: {
                            "reported_source": source_layout[name],
                            "canonical_source": source_layout[name],
                            "staged": staged_layout[name],
                        }
                        for name in STAGE_LAYOUT_FIELDS
                    },
                    "required_sentinels": {
                        "postgres": {
                            "root": "bindir",
                            "path": "postgres",
                            "kind": "file",
                            "sha256": next(
                                entry["sha256"]
                                for entry in runtime_entries
                                if entry["path"] == "postgres"
                            ),
                        }
                    },
                    "loader_dependencies": {
                        "entry_count": 0,
                        "aggregate_sha256": _artifact_sha256(b""),
                        "entries": [],
                    },
                    "entries": runtime_entries,
                },
                "extension_artifacts": {
                    "aggregate_sha256": package_aggregate,
                    "artifacts": [
                        {
                            "name": "bioseq",
                            "module_pathname_override": None,
                            "effective_module_name": "bioseq",
                            "control": {
                                "source": str(control_path),
                                "filename": "bioseq.control",
                                "staged": (
                                    "share/extension/bioseq.control"
                                ),
                                "source_sha256": control_sha256,
                                "sha256": control_sha256,
                            },
                            "install_sql": {
                                "source": str(sql_path),
                                "filename": "bioseq--1.0.sql",
                                "staged": (
                                    "share/extension/bioseq--1.0.sql"
                                ),
                                "version": "1.0",
                                "sha256": sql_script_hashes[profile],
                            },
                            "library": {
                                "source": str(installed_dso_path),
                                "filename": "bioseq.so",
                                "staged": "lib/bioseq.so",
                                "sha256": library_sha256,
                            },
                        }
                    ],
                },
                "expected_tool_paths": expected_tool_paths,
                "tool_signatures": tool_signatures,
            }
            stage_manifests[profile] = json.loads(
                json.dumps(stage_manifest)
            )
            stage_raw = _canonical_json_bytes(stage_manifest)
            stage_path = write(
                stage_root / "stage-manifest.json",
                stage_raw,
            )
            current_paths[f"{profile}_stage_manifest"] = stage_path
            current_paths[f"{profile}_installed_dso"] = (
                installed_dso_path
            )
            current_paths[f"{profile}_installed_control_dso"] = (
                installed_control_dso_path
            )
            current_paths[f"{profile}_installed_control"] = control_path
            current_paths[f"{profile}_installed_sql"] = sql_path
            stage_hashes[profile] = _artifact_sha256(stage_raw)
            suite_hashes[profile] = _artifact_sha256(
                _canonical_json_bytes(
                    sorted(
                        tool_signatures,
                        key=lambda signature: signature["tool"],
                    )
                )
            )
            runtime_hashes[
                f"{profile}_stage_manifest_sha256"
            ] = stage_hashes[profile]
            runtime_hashes[
                f"{profile}_tool_signature_suite_sha256"
            ] = suite_hashes[profile]

        def parse_self_test_plan(
            profile: str,
            candidate: dict[str, Any],
            label: str,
        ) -> dict[str, dict[str, Any]]:
            raw = _canonical_json_bytes(candidate)
            return _parse_case_plan(
                write(
                    current_root / f"{profile}-{label}-case-plan.json",
                    raw,
                ),
                profile=profile,
                expected_sha256=_artifact_sha256(raw),
                ledger_sha256=runtime_ledger_hashes[profile],
                runtime_inventory_sha256=runtime_inventory_hashes[
                    profile
                ],
                sql_manifest_sha256=runtime_ledgers[profile][
                    "sql_manifest_sha256"
                ],
                runtime_policy_sha256=_artifact_sha256(
                    runtime_policy_raw
                ),
                inventory=runtime_inventories[profile],
                expected_case_count=1,
            )

        for profile in ("production", "test_indexes"):
            authenticated_plan = parse_self_test_plan(
                profile,
                plans[profile],
                "authenticated",
            )
            case_id = plans[profile]["cases"][0]["catalog"]["case_id"]
            if (
                authenticated_plan[case_id]["execution"]
                != plans[profile]["cases"][0]["execution"]
            ):
                _fail(
                    "self-test",
                    "case-plan execution was not authenticated exactly",
                )

        exact_cardinality_plan = json.loads(
            json.dumps(plans["production"])
        )
        exact_cardinality_plan["cases"] = []
        for index in range(EXPECTED_PROFILE_CASE_COUNT):
            cloned_case = json.loads(
                json.dumps(plan_cases["production"])
            )
            cloned_case["catalog"]["case_id"] = (
                f"runtime-production-{index:02d}"
            )
            cloned_case["catalog"]["invocation_key"] = (
                f"runtime-invocation-production-{index:02d}"
            )
            exact_cardinality_plan["cases"].append(cloned_case)

        def parse_exact_cardinality_plan(
            candidate: dict[str, Any],
            label: str,
        ) -> dict[str, dict[str, Any]]:
            raw = _canonical_json_bytes(candidate)
            return _parse_case_plan(
                write(
                    current_root / f"production-{label}-case-plan.json",
                    raw,
                ),
                profile="production",
                expected_sha256=_artifact_sha256(raw),
                ledger_sha256=runtime_ledger_hashes["production"],
                runtime_inventory_sha256=runtime_inventory_hashes[
                    "production"
                ],
                sql_manifest_sha256=runtime_ledgers["production"][
                    "sql_manifest_sha256"
                ],
                runtime_policy_sha256=_artifact_sha256(
                    runtime_policy_raw
                ),
                inventory=runtime_inventories["production"],
            )

        parse_exact_cardinality_plan(
            exact_cardinality_plan,
            "exact-cardinality",
        )
        missing_cardinality_case = json.loads(
            json.dumps(exact_cardinality_plan)
        )
        missing_cardinality_case["cases"].pop()
        require_failure(
            lambda: parse_exact_cardinality_plan(
                missing_cardinality_case,
                "missing-cardinality",
            ),
            "requires exactly 25 cases",
            "case-plan parser accepted a fully resealed 24-case plan",
        )
        extra_cardinality_case = json.loads(
            json.dumps(exact_cardinality_plan)
        )
        extra_case = json.loads(
            json.dumps(extra_cardinality_case["cases"][-1])
        )
        extra_case["catalog"]["case_id"] = "runtime-production-25"
        extra_case["catalog"]["invocation_key"] = (
            "runtime-invocation-production-25"
        )
        extra_cardinality_case["cases"].append(extra_case)
        require_failure(
            lambda: parse_exact_cardinality_plan(
                extra_cardinality_case,
                "extra-cardinality",
            ),
            "requires exactly 25 cases",
            "case-plan parser accepted a fully resealed 26-case plan",
        )

        if (
            fault_key
            == BACKEND_SCOPED_VACUUM_CLEANUP_FAULT_KEY
        ):
            missing_control_field = json.loads(
                json.dumps(plans["production"])
            )
            del missing_control_field["cases"][0]["execution"][
                "pre_fault_control_recovery_sql"
            ]
            require_failure(
                lambda: parse_self_test_plan(
                    "production",
                    missing_control_field,
                    "missing-pre-fault-control-field",
                ),
                "missing fields: pre_fault_control_recovery_sql",
                "backend-scoped plan accepted a partial control contract",
            )

            empty_control_sql = json.loads(
                json.dumps(plans["production"])
            )
            empty_control_sql["cases"][0]["execution"][
                "pre_fault_control_sql"
            ] = []
            require_failure(
                lambda: parse_self_test_plan(
                    "production",
                    empty_control_sql,
                    "empty-pre-fault-control-sql",
                ),
                "pre_fault_control_sql must not be empty",
                "backend-scoped plan accepted an empty control SQL array",
            )

            empty_control_recovery_sql = json.loads(
                json.dumps(plans["production"])
            )
            empty_control_recovery_sql["cases"][0]["execution"][
                "pre_fault_control_recovery_sql"
            ] = ""
            require_failure(
                lambda: parse_self_test_plan(
                    "production",
                    empty_control_recovery_sql,
                    "empty-pre-fault-control-recovery-sql",
                ),
                "expected a non-empty string",
                "backend-scoped plan accepted empty control recovery SQL",
            )

            oversized_control_recovery_sql = json.loads(
                json.dumps(plans["production"])
            )
            oversized_control_recovery_sql["cases"][0]["execution"][
                "pre_fault_control_recovery_sql"
            ] = "x" * (1024 * 1024 + 1)
            require_failure(
                lambda: parse_self_test_plan(
                    "production",
                    oversized_control_recovery_sql,
                    "oversized-pre-fault-control-recovery-sql",
                ),
                "pre-fault control recovery SQL is oversized",
                "backend-scoped plan accepted oversized control recovery SQL",
            )

            empty_control_rows = json.loads(
                json.dumps(plans["production"])
            )
            empty_control_rows["cases"][0]["execution"][
                "pre_fault_control_recovery_expected_rows"
            ] = []
            require_failure(
                lambda: parse_self_test_plan(
                    "production",
                    empty_control_rows,
                    "empty-pre-fault-control-rows",
                ),
                "expected 1..64 recovery rows",
                "backend-scoped plan accepted empty control recovery rows",
            )

            fabricated_postgres_cleanup = json.loads(
                json.dumps(plans["production"])
            )
            fabricated_postgres_cleanup["cases"][0]["catalog"][
                "expected"
            ]["postgres_cleanup_events"] = ["fmgr_abort_observed"]
            require_failure(
                lambda: parse_self_test_plan(
                    "production",
                    fabricated_postgres_cleanup,
                    "fabricated-postgres-cleanup",
                ),
                "must not fabricate PostgreSQL/FMGR cleanup events",
                "backend-scoped plan accepted fabricated FMGR cleanup",
            )
        else:
            unexpected_control_fields = json.loads(
                json.dumps(plans["production"])
            )
            unexpected_control_fields["cases"][0]["execution"].update(
                {
                    "pre_fault_control_sql": [
                        "VACUUM runtime_fixture"
                    ],
                    "pre_fault_control_recovery_sql": (
                        "SELECT 'control-ok'"
                    ),
                    "pre_fault_control_recovery_expected_rows": [
                        ["control-ok", "production"]
                    ],
                }
            )
            require_failure(
                lambda: parse_self_test_plan(
                    "production",
                    unexpected_control_fields,
                    "unexpected-pre-fault-control-fields",
                ),
                "unknown fields:",
                "ordinary fault plan accepted backend-scoped controls",
            )

        results: dict[str, dict[str, Any]] = {}
        matrix_cases: list[dict[str, Any]] = []
        artifact_payloads: dict[
            tuple[str, str],
            tuple[dict[str, Any], bytes, Path, str, str],
        ] = {}
        for profile_index, profile in enumerate(
            ("production", "test_indexes"),
            1,
        ):
            prefix = profile
            plan_case = plan_cases[profile]
            catalog_entry = plan_case["catalog"]
            execution = plan_case["execution"]
            case_id = catalog_entry["case_id"]
            contract = catalog_entry["probe_contract"]
            ordering = contract["ordering"]
            visit_sequence = (
                2 if ordering == "trigger_before_visit" else 1
            )
            trigger_sequence = {
                "visit_only": 0,
                "trigger_before_visit": 1,
                "visit_before_trigger": 2,
            }[ordering]
            system_identifier = str(7000000000000000000 + profile_index)
            data_directory = (
                root / "runs" / profile / "data"
            ).resolve()
            data_directory.mkdir(parents=True)
            socket_directory = (
                root / "runs" / profile / "socket"
            ).resolve()
            socket_directory.mkdir()
            authorization = write(
                data_directory / AUTHORIZATION_MARKER_NAME,
                AUTHORIZATION_MARKER,
            )
            authorization.chmod(0o600)
            terminal_marker_directory = (
                data_directory / "bioseq-safety-terminal-markers"
            )
            terminal_marker_directory.mkdir(mode=0o700)
            server_log_path = (
                root
                / "runs"
                / profile
                / "server_startup.log"
            ).resolve()
            server_log_path.write_bytes(b"")
            (
                retained_authorization_path,
                retained_authorization_sha256,
                retained_authorization_relative,
            ) = publish(
                "cluster_authorities",
                profile,
                "authorization",
                "marker",
                AUTHORIZATION_MARKER,
            )
            retained_authorization_path.chmod(0o600)
            stage_path = current_paths[f"{profile}_stage_manifest"]
            stage_root = stage_path.parent
            config = {
                "schema_revision": 1,
                "profile": profile,
                "dedicated_safety_cluster": True,
                "connection": {
                    "host": str(socket_directory),
                    "port": str(45000 + profile_index),
                    "database": PRIVATE_DATABASE,
                    "user": "runtime_owner",
                    "password": PRIVATE_PASSWORD,
                },
                "private_stage_root": str(stage_root),
                "expected_data_directory": str(data_directory),
                "terminal_marker_directory": str(
                    terminal_marker_directory
                ),
                "server_log_path": str(server_log_path),
                "expected_system_identifier": system_identifier,
                "authorization_file": str(authorization),
                "installed_dso_path": str(
                    current_paths[f"{profile}_installed_dso"]
                ),
                "installed_control_dso_path": str(
                    current_paths[f"{profile}_installed_control_dso"]
                ),
                "installed_control_path": str(
                    current_paths[f"{profile}_installed_control"]
                ),
                "installed_sql_path": str(
                    current_paths[f"{profile}_installed_sql"]
                ),
                "restart_command": [
                    str(stage_root / "bin" / "pg_ctl"),
                    "--pgdata",
                    str(data_directory),
                    "--wait",
                    "--mode=fast",
                    "restart",
                ],
                "stage_manifest_path": str(stage_path),
                "stage_manifest_sha256": stage_hashes[profile],
                "tool_signature_suite_sha256": suite_hashes[profile],
            }
            authority_record = {
                "schema_revision": 1,
                "profile": profile,
                "cluster_system_identifier": system_identifier,
                "data_directory": str(data_directory),
                "socket_directory": str(socket_directory),
                "connection_port": config["connection"]["port"],
                "connection_user": config["connection"]["user"],
                "original_authorization_path": str(authorization),
                "authorization_mode": 0o600,
                "authorization_uid": os.getuid(),
                "authorization_sha256": (
                    retained_authorization_sha256
                ),
                "authorization_artifact_path": (
                    retained_authorization_relative
                ),
                "authorization_artifact_sha256": (
                    retained_authorization_sha256
                ),
            }
            (
                authority_path,
                authority_sha256,
                authority_relative,
            ) = publish(
                "cluster_authorities",
                profile,
                "cluster_authority",
                "json",
                _canonical_json_bytes(authority_record),
            )
            artifact_payloads[(profile, "cluster_authority")] = (
                authority_record,
                _canonical_json_bytes(authority_record),
                authority_path,
                authority_sha256,
                authority_relative,
            )
            runtime_hashes[f"{profile}_cluster_authority_path"] = (
                authority_relative
            )
            runtime_hashes[
                f"{profile}_cluster_authority_sha256"
            ] = authority_sha256
            config_raw = _canonical_json_bytes(config)
            (
                config_path,
                config_sha256,
                config_relative,
            ) = publish(
                "cluster_configs",
                profile,
                "cluster_config",
                "json",
                config_raw,
            )
            artifact_payloads[(profile, "config")] = (
                config,
                config_raw,
                config_path,
                config_sha256,
                config_relative,
            )
            catalog = {
                "schema_revision": 2,
                "profile": profile,
                "case_plan_sha256": runtime_hashes[
                    f"{profile}_case_plan_sha256"
                ],
                "test_executable_sha256": runtime_hashes[
                    f"{profile}_test_executable_sha256"
                ],
                "cases": [catalog_entry],
            }
            catalog_raw = _canonical_json_bytes(catalog)
            (
                catalog_path,
                catalog_sha256,
                catalog_relative,
            ) = publish(
                "catalogs",
                profile,
                "case_catalog",
                "json",
                catalog_raw,
            )
            artifact_payloads[(profile, "catalog")] = (
                catalog,
                catalog_raw,
                catalog_path,
                catalog_sha256,
                catalog_relative,
            )
            active_dso_path = current_paths[
                f"{profile}_installed_dso"
            ]
            control_dso_path = current_paths[
                f"{profile}_installed_control_dso"
            ]
            postmaster_start_time = (
                f"2026-07-25 12:00:0{profile_index}+00"
            )
            activation_nonce = f"runtime-activation-{profile}"
            activation = {
                "schema_revision": 1,
                "profile": profile,
                "case_plan_sha256": runtime_hashes[
                    f"{profile}_case_plan_sha256"
                ],
                "test_executable_sha256": runtime_hashes[
                    f"{profile}_test_executable_sha256"
                ],
                "requested_dso_sha256": runtime_hashes[
                    f"{profile}_dso_sha256"
                ],
                "cluster_config_sha256": config_sha256,
                "active_dso_path": str(active_dso_path),
                "active_dso_sha256": runtime_hashes[
                    f"{profile}_dso_sha256"
                ],
                "cluster_system_identifier": system_identifier,
                "cluster_data_directory": str(data_directory),
                "postmaster_start_time": postmaster_start_time,
                "activation_nonce": activation_nonce,
                "server_version_num": PG17_SERVER_VERSION_NUM,
                "extension_oid": 1000 + profile_index,
                "bioseq_guc_count": GUC_PROFILE_TOTALS[profile],
                "bioseq_test_only_guc_count": (
                    0 if profile == "production" else 3
                ),
                "bioseq_guc_properties": runtime_guc_properties[
                    profile
                ][0],
                "bioseq_guc_properties_sha256": (
                    runtime_guc_properties[profile][1]
                ),
                "installed_control_sha256": (
                    _path_artifact_sha256(
                        current_paths[f"{profile}_installed_control"],
                        "self-test-installed-control",
                    )
                ),
                "installed_sql_sha256": sql_script_hashes[profile],
                "sql_manifest_sha256": runtime_hashes[
                    f"{profile}_sql_manifest_sha256"
                ],
                "sql_script_sha256": sql_script_hashes[profile],
                "control_dso_path": str(control_dso_path),
                "control_dso_sha256": runtime_hashes[
                    "runtime_control_dso_sha256"
                ],
            }
            activation_raw = _canonical_json_bytes(activation)
            (
                activation_path,
                activation_sha256,
                activation_relative,
            ) = publish(
                "activations",
                profile,
                "activation",
                "json",
                activation_raw,
            )
            artifact_payloads[(profile, "activation")] = (
                activation,
                activation_raw,
                activation_path,
                activation_sha256,
                activation_relative,
            )
            catalog_entry_sha256 = _artifact_sha256(
                _canonical_json_bytes(catalog_entry)
            )
            request = {
                "schema_revision": 2,
                "profile": profile,
                "case_id": case_id,
                "sql_identity": sql_identity,
                "site_id": catalog_entry["site_id"],
                "ledger_site_sha256": catalog_entry[
                    "ledger_site_sha256"
                ],
                "invocation_key": catalog_entry["invocation_key"],
                "fault_key": fault_key,
                "relation": catalog_entry["relation"],
                "probe_contract": contract,
                "case_plan_path": str(
                    current_paths[f"{profile}_case_plan"]
                ),
                "case_plan_sha256": runtime_hashes[
                    f"{profile}_case_plan_sha256"
                ],
                "ledger_path": str(
                    current_paths[f"{profile}_ledger"]
                ),
                "ledger_sha256": runtime_hashes[
                    f"{profile}_ledger_sha256"
                ],
                "sql_manifest_path": str(
                    current_paths[f"{profile}_sql_manifest"]
                ),
                "sql_manifest_sha256": runtime_hashes[
                    f"{profile}_sql_manifest_sha256"
                ],
                "active_dso_path": str(active_dso_path),
                "active_dso_sha256": runtime_hashes[
                    f"{profile}_dso_sha256"
                ],
                "test_executable_sha256": runtime_hashes[
                    f"{profile}_test_executable_sha256"
                ],
                "producer_executable_path": str(
                    current_paths["runtime_producer"]
                ),
                "producer_executable_sha256": runtime_hashes[
                    "runtime_producer_executable_sha256"
                ],
                "cluster_config_path": str(config_path),
                "cluster_config_sha256": config_sha256,
                "stage_manifest_path": str(stage_path),
                "stage_manifest_sha256": stage_hashes[profile],
                "tool_signature_suite_sha256": suite_hashes[profile],
                "runtime_control_dso_sha256": runtime_hashes[
                    "runtime_control_dso_sha256"
                ],
                "activation_receipt_path": str(activation_path),
                "activation_receipt_sha256": activation_sha256,
                "catalog_path": str(catalog_path),
                "catalog_sha256": catalog_sha256,
                "catalog_entry_sha256": catalog_entry_sha256,
            }
            request_raw = _canonical_json_bytes(request)
            (
                request_path,
                request_sha256,
                request_relative,
            ) = publish(
                "requests",
                profile,
                case_id,
                "json",
                request_raw,
            )
            artifact_payloads[(profile, "request")] = (
                request,
                request_raw,
                request_path,
                request_sha256,
                request_relative,
            )
            recovery_rows = execution["recovery_expected_rows"]
            recovery_raw = _canonical_json_bytes(recovery_rows)
            (
                recovery_path,
                recovery_sha256,
                recovery_relative,
            ) = publish(
                "recoveries",
                profile,
                case_id,
                "json",
                recovery_raw,
            )
            raw_result = {
                "schema_revision": 2,
                "probe_abi_revision": 2,
                "profile": profile,
                "case_id": case_id,
                "sql_identity": sql_identity,
                "site_id": catalog_entry["site_id"],
                "ledger_site_sha256": catalog_entry[
                    "ledger_site_sha256"
                ],
                "invocation_key": catalog_entry["invocation_key"],
                "fault_key": fault_key,
                "relation": catalog_entry["relation"],
                "probe_contract": contract,
                "case_plan_sha256": runtime_hashes[
                    f"{profile}_case_plan_sha256"
                ],
                "ledger_sha256": runtime_hashes[
                    f"{profile}_ledger_sha256"
                ],
                "sql_manifest_sha256": runtime_hashes[
                    f"{profile}_sql_manifest_sha256"
                ],
                "active_dso_sha256": runtime_hashes[
                    f"{profile}_dso_sha256"
                ],
                "test_executable_sha256": runtime_hashes[
                    f"{profile}_test_executable_sha256"
                ],
                "producer_executable_sha256": runtime_hashes[
                    "runtime_producer_executable_sha256"
                ],
                "activation_receipt_sha256": activation_sha256,
                "bioseq_guc_properties": runtime_guc_properties[
                    profile
                ][0],
                "bioseq_guc_properties_sha256": (
                    runtime_guc_properties[profile][1]
                ),
                "cluster_config_sha256": config_sha256,
                "request_sha256": request_sha256,
                "catalog_sha256": catalog_sha256,
                "catalog_entry_sha256": catalog_entry_sha256,
                "execution_backend": "postgresql",
                "exercised_terminal_severity": False,
                "backend_outcome": {
                    "expected": "recoverable_error",
                    "observed": "recoverable_error",
                    "initial_backend_pid": 12000 + profile_index,
                    "recovery_backend_pid": 12000 + profile_index,
                    "initial_connection_status": "ok",
                    "post_invocation_connection_status": "ok",
                    "invocation_result_status": "postgres_error",
                    "terminal_guard_observed": False,
                },
                "terminal_causality": None,
                "destructor_events": [
                    {
                        "name": f"runtime_sentinel_{profile}",
                        "count": 1,
                    }
                ],
                "cleanup_events": [],
                "postgres_cleanup_events": [
                    {"name": event_name, "count": 1}
                    for event_name in postgres_cleanup_event_names
                ],
                "postgres_error": {
                    "severity": "ERROR",
                    "sqlstate": "XX000",
                    "message": f"runtime error {profile}",
                },
                "fault_evidence": {
                    "observation_mode": "backend_snapshot",
                    "probe_abi_revision": 2,
                    "site_id": catalog_entry["site_id"],
                    "fault_key": fault_key,
                    "relation": catalog_entry["relation"],
                    "probe_contract": contract,
                    "invocation_count": runtime_lifecycle[
                        "fault_invocation_count"
                    ],
                    "fmgr_start_count": runtime_lifecycle[
                        "fmgr_start_count"
                    ],
                    "fmgr_end_count": runtime_lifecycle[
                        "fmgr_end_count"
                    ],
                    "fmgr_abort_count": runtime_lifecycle[
                        "fmgr_abort_count"
                    ],
                    "site_visit_count": contract[
                        "expected_visit_count"
                    ],
                    "fault_trigger_count": contract[
                        "expected_trigger_count"
                    ],
                    "site_visit_sequence": visit_sequence,
                    "fault_trigger_sequence": trigger_sequence,
                    "invalid_count": 0,
                    "overflow_count": 0,
                    "inside_target": False,
                },
                "recovery_probe": {
                    "postcondition": f"runtime recovery {profile}",
                    "passed": True,
                    "transaction_status": "idle",
                    "nonempty_result": True,
                    "expected_rows": recovery_rows,
                    "observed_rows": recovery_rows,
                    "result_row_count": len(recovery_rows),
                },
                "cluster_evidence": {
                    "cluster_system_identifier": system_identifier,
                    "cluster_data_directory": str(data_directory),
                    "postmaster_start_time": postmaster_start_time,
                    "activation_nonce": activation_nonce,
                    "server_version_num": PG17_SERVER_VERSION_NUM,
                    "backend_pid": 12000 + profile_index,
                    "initial_backend_pid": 12000 + profile_index,
                    "recovery_backend_pid": 12000 + profile_index,
                    "connection_replaced": False,
                    "active_dso_path": str(active_dso_path),
                    "mapped_target_path": str(active_dso_path),
                    "control_dso_path": str(control_dso_path),
                    "control_dso_sha256": runtime_hashes[
                        "runtime_control_dso_sha256"
                    ],
                },
            }
            raw_result_raw = _canonical_json_bytes(raw_result)
            (
                raw_result_path,
                raw_result_sha256,
                raw_result_relative,
            ) = publish(
                "raw_results",
                profile,
                case_id,
                "json",
                raw_result_raw,
            )
            artifact_payloads[(profile, "raw_result")] = (
                raw_result,
                raw_result_raw,
                raw_result_path,
                raw_result_sha256,
                raw_result_relative,
            )
            attestation = {
                "schema_revision": 1,
                "request_path": request_relative,
                "request_sha256": request_sha256,
                "catalog_path": catalog_relative,
                "catalog_sha256": catalog_sha256,
                "catalog_entry_sha256": catalog_entry_sha256,
                "activation_receipt_path": activation_relative,
                "activation_receipt_sha256": activation_sha256,
                "cluster_config_path": config_relative,
                "cluster_config_sha256": config_sha256,
                "stage_manifest_sha256": stage_hashes[profile],
                "tool_signature_suite_sha256": suite_hashes[profile],
                "active_dso_path": str(active_dso_path),
                "active_dso_sha256": runtime_hashes[
                    f"{profile}_dso_sha256"
                ],
                "cluster_system_identifier": system_identifier,
                "cluster_data_directory": str(data_directory),
                "postmaster_start_time": postmaster_start_time,
                "activation_nonce": activation_nonce,
                "server_version_num": PG17_SERVER_VERSION_NUM,
                "backend_pid": 12000 + profile_index,
                "expected_backend_outcome": "recoverable_error",
                "observed_backend_outcome": "recoverable_error",
                "probe_observation_mode": "backend_snapshot",
                "initial_backend_pid": 12000 + profile_index,
                "recovery_backend_pid": 12000 + profile_index,
                "connection_replaced": False,
                "initial_connection_status": "ok",
                "post_invocation_connection_status": "ok",
                "invocation_result_status": "postgres_error",
                "terminal_guard_observed": False,
                "terminal_causality": None,
                "probe_abi_revision": 2,
                "site_id": catalog_entry["site_id"],
                "ledger_site_sha256": catalog_entry[
                    "ledger_site_sha256"
                ],
                "fault_key": fault_key,
                "relation": catalog_entry["relation"],
                "probe_contract": contract,
                **runtime_lifecycle,
                "probe_invalid_count": 0,
                "probe_overflow_count": 0,
                "probe_inside_target": False,
                "site_visit_count": contract[
                    "expected_visit_count"
                ],
                "fault_trigger_count": contract[
                    "expected_trigger_count"
                ],
                "site_visit_sequence": visit_sequence,
                "fault_trigger_sequence": trigger_sequence,
                "mapped_target_path": str(active_dso_path),
                "mapped_target_sha256": runtime_hashes[
                    f"{profile}_dso_sha256"
                ],
                "control_dso_path": str(control_dso_path),
                "control_dso_sha256": runtime_hashes[
                    "runtime_control_dso_sha256"
                ],
                "recovery_result_row_count": len(recovery_rows),
                "recovery_rows_sha256": recovery_sha256,
                "recovery_artifact_path": recovery_relative,
                "recovery_artifact_sha256": recovery_sha256,
                "raw_result_path": raw_result_relative,
                "raw_result_sha256": raw_result_sha256,
            }
            immutable_result = {
                "schema_revision": 2,
                "probe_abi_revision": 2,
                "case_id": case_id,
                "profile": profile,
                "sql_identity": sql_identity,
                "site_id": catalog_entry["site_id"],
                "ledger_site_sha256": catalog_entry[
                    "ledger_site_sha256"
                ],
                "invocation_key": catalog_entry["invocation_key"],
                "fault_key": fault_key,
                "relation": catalog_entry["relation"],
                "probe_contract": contract,
                "case_plan_sha256": runtime_hashes[
                    f"{profile}_case_plan_sha256"
                ],
                "ledger_sha256": runtime_hashes[
                    f"{profile}_ledger_sha256"
                ],
                "sql_manifest_sha256": runtime_hashes[
                    f"{profile}_sql_manifest_sha256"
                ],
                "active_dso_sha256": runtime_hashes[
                    f"{profile}_dso_sha256"
                ],
                "producer_executable_sha256": runtime_hashes[
                    "runtime_producer_executable_sha256"
                ],
                "test_executable_sha256": runtime_hashes[
                    f"{profile}_test_executable_sha256"
                ],
                "expected_backend_outcome": "recoverable_error",
                "observed_backend_outcome": "recoverable_error",
                "probe_observation_mode": "backend_snapshot",
                "initial_backend_pid": 12000 + profile_index,
                "recovery_backend_pid": 12000 + profile_index,
                "connection_replaced": False,
                "initial_connection_status": "ok",
                "post_invocation_connection_status": "ok",
                "invocation_result_status": "postgres_error",
                "terminal_guard_observed": False,
                "terminal_causality": None,
                "site_visit_count": contract[
                    "expected_visit_count"
                ],
                "fault_trigger_count": contract[
                    "expected_trigger_count"
                ],
                "site_visit_sequence": visit_sequence,
                "fault_trigger_sequence": trigger_sequence,
                "expected_destructor_sentinels": [
                    f"runtime_sentinel_{profile}"
                ],
                "observed_destructor_sentinels": [
                    f"runtime_sentinel_{profile}"
                ],
                "expected_cleanup_events": [],
                "observed_cleanup_events": [],
                "expected_postgres_cleanup_events": list(
                    postgres_cleanup_event_names
                ),
                "observed_postgres_cleanup_events": list(
                    postgres_cleanup_event_names
                ),
                "expected_fmgr_abort_count": runtime_lifecycle[
                    "fmgr_abort_count"
                ],
                "observed_fmgr_abort_count": runtime_lifecycle[
                    "fmgr_abort_count"
                ],
                "expected_sqlstate": "XX000",
                "observed_sqlstate": "XX000",
                "expected_message": f"runtime error {profile}",
                "observed_message": f"runtime error {profile}",
                "expected_recovery_postcondition": (
                    f"runtime recovery {profile}"
                ),
                "observed_recovery_postcondition": (
                    f"runtime recovery {profile}"
                ),
                "exercised_terminal_severity": False,
                "attestation": attestation,
            }
            immutable_result_raw = _canonical_json_bytes(
                immutable_result
            )
            (
                result_path,
                result_sha256,
                result_relative,
            ) = publish(
                "results",
                profile,
                case_id,
                "json",
                immutable_result_raw,
            )
            results[profile] = immutable_result
            artifact_payloads[(profile, "result")] = (
                immutable_result,
                immutable_result_raw,
                result_path,
                result_sha256,
                result_relative,
            )
            log = {
                "schema_revision": 1,
                "context": f"{profile}/{case_id}",
                "command": [
                    str(
                        current_paths[
                            f"{profile}_test_executable"
                        ]
                    ),
                    "--bioseq-safety-runtime-run",
                    "--request",
                    str(request_path),
                    "--output",
                    str(
                        root
                        / "work"
                        / (
                            f"{profile}.{case_id}."
                            "raw-result.json"
                        )
                    ),
                ],
                "returncode": 0,
                "stdout": "",
                "stderr": "",
                "bindings": {
                    "request_sha256": request_sha256,
                    "raw_result_sha256": raw_result_sha256,
                    "immutable_result_sha256": result_sha256,
                    "activation_receipt_sha256": activation_sha256,
                    "catalog_sha256": catalog_sha256,
                    "case_plan_sha256": runtime_hashes[
                        f"{profile}_case_plan_sha256"
                    ],
                    "runtime_inventory_sha256": runtime_hashes[
                        f"{profile}_runtime_inventory_sha256"
                    ],
                    "ledger_sha256": runtime_hashes[
                        f"{profile}_ledger_sha256"
                    ],
                    "sql_manifest_sha256": runtime_hashes[
                        f"{profile}_sql_manifest_sha256"
                    ],
                    "active_dso_sha256": runtime_hashes[
                        f"{profile}_dso_sha256"
                    ],
                    "control_dso_sha256": runtime_hashes[
                        "runtime_control_dso_sha256"
                    ],
                    "test_executable_sha256": runtime_hashes[
                        f"{profile}_test_executable_sha256"
                    ],
                    "producer_executable_sha256": runtime_hashes[
                        "runtime_producer_executable_sha256"
                    ],
                    "cluster_config_sha256": config_sha256,
                    "stage_manifest_sha256": stage_hashes[profile],
                    "tool_signature_suite_sha256": suite_hashes[
                        profile
                    ],
                },
            }
            log_raw = _canonical_json_bytes(log)
            log_path, log_sha256, log_relative = publish(
                "logs",
                profile,
                case_id,
                "log.json",
                log_raw,
            )
            artifact_payloads[(profile, "log")] = (
                log,
                log_raw,
                log_path,
                log_sha256,
                log_relative,
            )
            matrix_cases.append(
                {
                    **{
                        key: value
                        for key, value in immutable_result.items()
                        if key not in {"schema_revision", "attestation"}
                    },
                    "immutable_test_log_path": log_relative,
                    "immutable_test_log_sha256": log_sha256,
                    "immutable_result_path": result_relative,
                    "immutable_result_sha256": result_sha256,
                }
            )
        matrix = {
            "schema_revision": 2,
            **runtime_hashes,
            "cases": matrix_cases,
        }
        matrix_path.write_bytes(_canonical_json_bytes(matrix))
        validation_arguments = {
            "production_sha256": runtime_hashes[
                "production_ledger_sha256"
            ],
            "test_indexes_sha256": runtime_hashes[
                "test_indexes_ledger_sha256"
            ],
            "production_sql_manifest_sha256": runtime_hashes[
                "production_sql_manifest_sha256"
            ],
            "test_indexes_sql_manifest_sha256": runtime_hashes[
                "test_indexes_sql_manifest_sha256"
            ],
            "production_case_plan_sha256": runtime_hashes[
                "production_case_plan_sha256"
            ],
            "test_indexes_case_plan_sha256": runtime_hashes[
                "test_indexes_case_plan_sha256"
            ],
            "production_runtime_inventory_sha256": runtime_hashes[
                "production_runtime_inventory_sha256"
            ],
            "test_indexes_runtime_inventory_sha256": runtime_hashes[
                "test_indexes_runtime_inventory_sha256"
            ],
            "runtime_policy_sha256": runtime_hashes[
                "runtime_policy_sha256"
            ],
            "production_dso_sha256": runtime_hashes[
                "production_dso_sha256"
            ],
            "test_indexes_dso_sha256": runtime_hashes[
                "test_indexes_dso_sha256"
            ],
            "runtime_control_dso_sha256": runtime_hashes[
                "runtime_control_dso_sha256"
            ],
            "runtime_producer_executable_sha256": runtime_hashes[
                "runtime_producer_executable_sha256"
            ],
            "production_test_executable_sha256": runtime_hashes[
                "production_test_executable_sha256"
            ],
            "test_indexes_test_executable_sha256": runtime_hashes[
                "test_indexes_test_executable_sha256"
            ],
            "production_ledger_path": current_paths[
                "production_ledger"
            ],
            "test_indexes_ledger_path": current_paths[
                "test_indexes_ledger"
            ],
            "production_sql_manifest_path": current_paths[
                "production_sql_manifest"
            ],
            "test_indexes_sql_manifest_path": current_paths[
                "test_indexes_sql_manifest"
            ],
            "production_sql_script_sha256": sql_script_hashes[
                "production"
            ],
            "test_indexes_sql_script_sha256": sql_script_hashes[
                "test_indexes"
            ],
            "production_case_plan_path": current_paths[
                "production_case_plan"
            ],
            "test_indexes_case_plan_path": current_paths[
                "test_indexes_case_plan"
            ],
            "production_stage_manifest_path": current_paths[
                "production_stage_manifest"
            ],
            "test_indexes_stage_manifest_path": current_paths[
                "test_indexes_stage_manifest"
            ],
            "production_dso_path": current_paths["production_dso"],
            "test_indexes_dso_path": current_paths["test_indexes_dso"],
            "runtime_control_dso_path": current_paths[
                "runtime_control_dso"
            ],
            "runtime_producer_executable_path": current_paths[
                "runtime_producer"
            ],
            "production_test_executable_path": current_paths[
                "production_test_executable"
            ],
            "test_indexes_test_executable_path": current_paths[
                "test_indexes_test_executable"
            ],
            "runtime_matrix_path": matrix_path,
            "expected_matrix_case_count": 2,
            "expected_profile_case_count": 1,
        }

        def validate_matrix(
            candidate: dict[str, Any],
            **overrides: Any,
        ) -> None:
            arguments = dict(validation_arguments)
            arguments.update(overrides)
            validate_runtime_matrix(
                candidate,
                runtime_ledgers["production"],
                runtime_ledgers["test_indexes"],
                runtime_inventories["production"],
                runtime_inventories["test_indexes"],
                set(),
                **arguments,
            )

        for profile in ("production", "test_indexes"):
            run_root = root / "runs" / profile
            shutil.rmtree(run_root / "data")
            shutil.rmtree(run_root / "socket")
        validate_matrix(matrix)

        terminal_site_id = "probe:" + "a" * 64
        terminal_fault_key = (
            "scoped_spi_session.emergency_finish_error"
        )
        terminal_backend_pid = 1337
        terminal_token = "b" * 64
        production_config = artifact_payloads[
            ("production", "config")
        ][0]
        terminal_marker_directory = Path(
            production_config["terminal_marker_directory"]
        )
        terminal_server_log_path = Path(
            production_config["server_log_path"]
        )
        terminal_payload = (
            '{"schema":"bioseq.postgres_error_safety.terminal_marker",'
            '"schema_revision":1,'
            f'"token":"{terminal_token}",'
            f'"site_id":"{terminal_site_id}",'
            f'"fault_key":"{terminal_fault_key}",'
            '"guard_kind":"finish_error","backend_pid":1337}\n'
        )
        terminal_interval = (
            "server process (PID 1337) exited with exit code 1\n"
        )
        terminal_causality = {
            "marker": {
                "path": str(
                    terminal_marker_directory
                    / f"terminal-{terminal_token}.marker"
                ),
                "sha256": hashlib.sha256(
                    terminal_payload.encode("utf-8")
                ).hexdigest(),
                "payload": terminal_payload,
                "schema": "bioseq.postgres_error_safety.terminal_marker",
                "schema_revision": 1,
                "token": terminal_token,
                "site_id": terminal_site_id,
                "fault_key": terminal_fault_key,
                "guard_kind": "finish_error",
                "backend_pid": terminal_backend_pid,
                "mode": 0o600,
                "uid": os.getuid(),
                "link_count": 1,
            },
            "server_log": {
                "path": str(terminal_server_log_path),
                "device": 1,
                "inode": 1,
                "offset": 0,
                "interval_size": len(
                    terminal_interval.encode("utf-8")
                ),
                "interval_sha256": hashlib.sha256(
                    terminal_interval.encode("utf-8")
                ).hexdigest(),
                "interval_text": terminal_interval,
                "exit_record": (
                    "server process (PID 1337) exited with exit code 1"
                ),
                "fatal_present": False,
                "panic_present": False,
                "signal_present": False,
                "core_dump_present": False,
            },
            "marker_directory": str(terminal_marker_directory),
            "server_log_path": str(terminal_server_log_path),
            "postmaster_identity_unchanged": True,
            "fatal_panic_signal_core_rejected": True,
        }
        terminal_arguments = {
            "backend_exit": True,
            "site_id": terminal_site_id,
            "fault_key": terminal_fault_key,
            "backend_pid": terminal_backend_pid,
            "marker_directory": terminal_marker_directory,
            "server_log_path": terminal_server_log_path,
            "marker_uid": os.getuid(),
            "context": "self-test-terminal-causality",
        }
        _validate_terminal_causality(
            terminal_causality,
            **terminal_arguments,
        )
        missing_marker = json.loads(json.dumps(terminal_causality))
        del missing_marker["marker"]
        require_failure(
            lambda: _validate_terminal_causality(
                missing_marker,
                **terminal_arguments,
            ),
            "missing fields: marker",
            "terminal causality accepted a missing marker",
        )
        wrong_marker = json.loads(json.dumps(terminal_causality))
        wrong_payload = terminal_payload.replace(
            '"guard_kind":"finish_error"',
            '"guard_kind":"finish_non_ok"',
        )
        wrong_marker["marker"].update(
            {
                "payload": wrong_payload,
                "sha256": hashlib.sha256(
                    wrong_payload.encode("utf-8")
                ).hexdigest(),
            }
        )
        require_failure(
            lambda: _validate_terminal_causality(
                wrong_marker,
                **terminal_arguments,
            ),
            "payload differs",
            "terminal causality accepted a resealed wrong marker",
        )

        def resealed_terminal_interval(extra: str) -> dict[str, Any]:
            candidate = json.loads(json.dumps(terminal_causality))
            interval = terminal_interval + extra
            candidate["server_log"].update(
                {
                    "interval_text": interval,
                    "interval_size": len(interval.encode("utf-8")),
                    "interval_sha256": hashlib.sha256(
                        interval.encode("utf-8")
                    ).hexdigest(),
                }
            )
            return candidate

        for extra, label in (
            ("FATAL: fabricated\n", "FATAL substitution"),
            (
                "server process was terminated by signal 11; "
                "core dumped\n",
                "signal/core substitution",
            ),
        ):
            require_failure(
                lambda extra=extra: _validate_terminal_causality(
                    resealed_terminal_interval(extra),
                    **terminal_arguments,
                ),
                "crash substitution",
                f"terminal causality accepted a resealed {label}",
            )

        production_properties = runtime_guc_properties["production"][0]

        def reject_guc_property_mutation(
            mutation: Any,
            expected: str,
            label: str,
            *,
            refresh_hash: bool = True,
        ) -> None:
            candidate = json.loads(json.dumps(production_properties))
            mutation(candidate)
            digest = (
                _artifact_sha256(_canonical_json_bytes(candidate))
                if refresh_hash
                else runtime_guc_properties["production"][1]
            )
            require_failure(
                lambda: _validate_guc_property_receipt(
                    candidate,
                    digest,
                    generation=runtime_manifests["production"][
                        "guc_generation"
                    ],
                    profile="production",
                    context="self-test GUC receipt",
                ),
                expected,
                "GUC receipt accepted " + label,
            )

        reject_guc_property_mutation(
            lambda value: value.pop(),
            "malformed or unsealed",
            "a missing row",
        )
        reject_guc_property_mutation(
            lambda value: value.append(json.loads(json.dumps(value[-1]))),
            "malformed or unsealed",
            "an extra row",
        )
        reject_guc_property_mutation(
            lambda value: value.__setitem__(
                slice(0, 2),
                [value[1], value[0]],
            ),
            "does not close over the manifest row",
            "reordered rows",
        )
        reject_guc_property_mutation(
            lambda value: value[0].update(
                {"manifest_registration_sha256": "0" * 64}
            ),
            "does not close over the manifest row",
            "a registration hash mutation",
        )
        reject_guc_property_mutation(
            lambda value: value[0]["registration"].update(
                {"member": "mutated"}
            ),
            "does not close over the manifest row",
            "a registration mutation",
        )
        reject_guc_property_mutation(
            lambda value: value[0]["profile_projection"].update(
                {"profile": "test_indexes"}
            ),
            "does not close over the manifest row",
            "a profile projection mutation",
        )
        reject_guc_property_mutation(
            lambda value: value[0]["profile_projection"][
                "owner_facet"
            ].update({"content_sha256": "0" * 64}),
            "does not close over the manifest row",
            "an owner-facet mutation",
        )
        reject_guc_property_mutation(
            lambda value: value[0]["pg_settings"].update(
                {"context": "superuser"}
            ),
            "live GUC projection differs from generated authority",
            "a live pg_settings mutation",
        )
        reject_guc_property_mutation(
            lambda value: value[0].update(
                {"adversarial_unknown": True}
            ),
            "unknown fields: adversarial_unknown",
            "an unknown row field",
        )
        reject_guc_property_mutation(
            lambda value: value[0]["pg_settings"].update(
                {"short_description": "mutated"}
            ),
            "malformed or unsealed",
            "a stale receipt hash",
            refresh_hash=False,
        )
        mutated_test_properties = json.loads(
            json.dumps(runtime_guc_properties["test_indexes"][0])
        )
        mutated_test_properties[0]["pg_settings"][
            "short_description"
        ] = "cross-profile mutation"
        require_failure(
            lambda: _validate_guc_profile_pair(
                production_properties,
                mutated_test_properties,
                context="self-test GUC profiles",
            ),
            "common GUC property differs across profiles",
            "GUC profile join accepted a common-row mutation",
        )
        missing_test_delta = json.loads(
            json.dumps(runtime_guc_properties["test_indexes"][0][:-1])
        )
        require_failure(
            lambda: _validate_guc_profile_pair(
                production_properties,
                missing_test_delta,
                context="self-test GUC profiles",
            ),
            "exact 73/76 profile delta",
            "GUC profile join accepted a missing test-only delta",
        )

        authority_registry: dict[
            tuple[int, int], tuple[Any, ...]
        ] = {}
        production_retained_authority = (
            _load_retained_cluster_authority(
                matrix=matrix,
                profile="production",
                candidate_root=root,
                artifact_identities=authority_registry,
            )
        )

        production_stage_path = current_paths[
            "production_stage_manifest"
        ]
        production_stage_root = production_stage_path.parent
        production_stage_binding = _parse_stage_manifest_binding(
            production_stage_path,
            profile="production",
            expected_dso_sha256=runtime_hashes[
                "production_dso_sha256"
            ],
            expected_sql_script_sha256=sql_script_hashes[
                "production"
            ],
        )
        production_stage_binding.pg_ctl_path.chmod(0o644)
        try:
            require_failure(
                lambda: _parse_stage_manifest_binding(
                    production_stage_path,
                    profile="production",
                    expected_dso_sha256=runtime_hashes[
                        "production_dso_sha256"
                    ],
                    expected_sql_script_sha256=sql_script_hashes[
                        "production"
                    ],
                ),
                "executable mode differ",
                "stage parser accepted a non-executable pg_ctl",
            )
        finally:
            production_stage_binding.pg_ctl_path.chmod(0o755)

        substitute_control = (
            production_stage_binding.sharedir
            / "extension"
            / "substitute.control"
        )
        substitute_control.write_bytes(
            production_stage_binding.control_path.read_bytes()
        )
        substitute_library = (
            production_stage_binding.pkglibdir / "substitute.so"
        )
        substitute_library.write_bytes(
            production_stage_binding.library_path.read_bytes()
        )

        def reject_stage_manifest(
            mutation: Any,
            expected: str,
            label: str,
        ) -> None:
            candidate = json.loads(
                json.dumps(stage_manifests["production"])
            )
            mutation(candidate)
            candidate_path = (
                production_stage_root
                / f"stage-mutation-{label}.json"
            )
            candidate_path.write_bytes(_canonical_json_bytes(candidate))
            require_failure(
                lambda: _parse_stage_manifest_binding(
                    candidate_path,
                    profile="production",
                    expected_dso_sha256=runtime_hashes[
                        "production_dso_sha256"
                    ],
                    expected_sql_script_sha256=sql_script_hashes[
                        "production"
                    ],
                ),
                expected,
                "stage parser accepted " + label,
            )

        reject_stage_manifest(
            lambda value: value.update({"schema": "stage_manifest/v1"}),
            "stage schema or PostgreSQL release differs",
            "legacy-schema-alias",
        )
        reject_stage_manifest(
            lambda value: value.pop("schema_revision"),
            "missing fields: schema_revision",
            "missing-schema-revision",
        )
        reject_stage_manifest(
            lambda value: value.update({"schema_revision": "3"}),
            "expected a non-negative integer",
            "string-schema-revision",
        )
        reject_stage_manifest(
            lambda value: value.update(
                {
                    "bioseq_artifacts": value.pop(
                        "extension_artifacts"
                    )
                }
            ),
            "missing fields: extension_artifacts",
            "legacy-artifact-alias",
        )
        reject_stage_manifest(
            lambda value: value["runtime_closure"].update(
                {"algorithm": "postgresql.copy_selected_files"}
            ),
            "runtime closure algorithm/schema differs",
            "runtime-algorithm-alias",
        )
        reject_stage_manifest(
            lambda value: value["runtime_closure"].update(
                {"schema_revision": "1"}
            ),
            "expected a non-negative integer",
            "string-runtime-schema-revision",
        )
        reject_stage_manifest(
            lambda value: value["runtime_closure"].update(
                {"legacy_entries": []}
            ),
            "unknown fields: legacy_entries",
            "runtime-closure-alias",
        )
        reject_stage_manifest(
            lambda value: value["runtime_closure"]["roots"][
                "bindir"
            ].update({"source": "/fixture/bin"}),
            "unknown fields: source",
            "runtime-root-alias",
        )
        reject_stage_manifest(
            lambda value: value["runtime_closure"].update(
                {"aggregate_sha256": "0" * 64}
            ),
            "runtime closure aggregate is stale",
            "stale-runtime-aggregate",
        )
        reject_stage_manifest(
            lambda value: value["runtime_closure"]["entries"][0].update(
                {"legacy_path": "postgres"}
            ),
            "unknown fields: legacy_path",
            "runtime-entry-alias",
        )
        reject_stage_manifest(
            lambda value: value["extension_artifacts"]["artifacts"][0][
                "control"
            ].pop(
                "staged"
            ),
            "missing fields: staged",
            "missing-control-staged",
        )
        reject_stage_manifest(
            lambda value: value["extension_artifacts"]["artifacts"][0][
                "control"
            ].update({"staged": "share/extension/substitute.control"}),
            "control path differs",
            "substituted-control",
        )
        reject_stage_manifest(
            lambda value: value["extension_artifacts"]["artifacts"][0][
                "install_sql"
            ].update({"version": "2.0"}),
            "extension install version must be 1.0",
            "install-version",
        )
        reject_stage_manifest(
            lambda value: value["extension_artifacts"]["artifacts"][0][
                "install_sql"
            ].update({"sha256": 7}),
            "expected a non-empty string",
            "install-hash-type",
        )
        reject_stage_manifest(
            lambda value: value["extension_artifacts"]["artifacts"][0][
                "library"
            ].update({"filename": "nested/bioseq.so"}),
            "one basename",
            "library-filename",
        )
        reject_stage_manifest(
            lambda value: value["extension_artifacts"]["artifacts"][0][
                "library"
            ].update({"staged": "lib/substitute.so"}),
            "library path differs",
            "substituted-library",
        )
        reject_stage_manifest(
            lambda value: value["staged"].update(
                {"sharedir": "absent-share"}
            ),
            "invalid directory component",
            "absent-sharedir",
        )
        reject_stage_manifest(
            lambda value: value["extension_artifacts"].update(
                {"aggregate_sha256": "0" * 64}
            ),
            "aggregate hash is stale",
            "stale-aggregate",
        )

        production_config = json.loads(
            json.dumps(
                artifact_payloads[("production", "config")][0]
            )
        )
        _validate_cluster_config_authority(
            production_config,
            profile="production",
            candidate_root=root,
            stage=production_stage_binding,
            stage_manifest_sha256=stage_hashes["production"],
            tool_signature_suite_sha256=suite_hashes["production"],
            runtime_control_dso_path=current_paths[
                "runtime_control_dso"
            ],
            authority=production_retained_authority,
            context="self-test-cluster-authority",
        )

        def reject_cluster_config(
            mutation: Any,
            expected: str,
            label: str,
        ) -> None:
            candidate = json.loads(json.dumps(production_config))
            mutation(candidate)
            require_failure(
                lambda: _validate_cluster_config_authority(
                    candidate,
                    profile="production",
                    candidate_root=root,
                    stage=production_stage_binding,
                    stage_manifest_sha256=stage_hashes["production"],
                    tool_signature_suite_sha256=(
                        suite_hashes["production"]
                    ),
                    runtime_control_dso_path=current_paths[
                        "runtime_control_dso"
                    ],
                    authority=production_retained_authority,
                    context="self-test-cluster-authority",
                ),
                expected,
                "cluster authority accepted " + label,
            )

        connection_mutations: dict[str, Any] = {
            "host": str(root / "absent-socket"),
            "port": "45433",
            "database": "postgres",
            "user": "alternate_owner",
            "password": "substituted",
        }
        for field_name, replacement in connection_mutations.items():
            reject_cluster_config(
                lambda value, field_name=field_name, replacement=replacement: (
                    value["connection"].update(
                        {field_name: replacement}
                    )
                ),
                f"connection.{field_name}",
                "connection-" + field_name,
            )
        reject_cluster_config(
            lambda value: value["connection"].pop("port"),
            "missing fields: port",
            "missing-connection-field",
        )
        reject_cluster_config(
            lambda value: value["connection"].update(
                {"unknown": "field"}
            ),
            "unknown fields: unknown",
            "unknown-connection-field",
        )

        for index in range(len(production_config["restart_command"])):
            reject_cluster_config(
                lambda value, index=index: value[
                    "restart_command"
                ].__setitem__(index, "substituted"),
                "restart command differs",
                f"restart-argv-{index}",
            )
        reject_cluster_config(
            lambda value: value["restart_command"].pop(),
            "restart command differs",
            "restart-missing-argument",
        )
        reject_cluster_config(
            lambda value: value["restart_command"].append("extra"),
            "restart command differs",
            "restart-extra-argument",
        )

        selected_path_fields = {
            "installed_dso_path": (
                production_stage_binding.pkglibdir
                / "alternate-bioseq.so"
            ),
            "installed_control_dso_path": (
                production_stage_binding.pkglibdir
                / "alternate-runtime-control.so"
            ),
            "installed_control_path": (
                production_stage_binding.sharedir
                / "extension"
                / "alternate.control"
            ),
            "installed_sql_path": (
                production_stage_binding.sharedir
                / "extension"
                / "alternate.sql"
            ),
        }
        selected_path_sources = {
            "installed_dso_path": (
                production_stage_binding.library_path
            ),
            "installed_control_dso_path": current_paths[
                "production_installed_control_dso"
            ],
            "installed_control_path": (
                production_stage_binding.control_path
            ),
            "installed_sql_path": (
                production_stage_binding.install_sql_path
            ),
        }
        for field_name, alternate_path in selected_path_fields.items():
            alternate_path.write_bytes(
                selected_path_sources[field_name].read_bytes()
            )
            reject_cluster_config(
                lambda value, field_name=field_name,
                alternate_path=alternate_path: value.update(
                    {field_name: str(alternate_path)}
                ),
                "manifest-selected path",
                "substituted-" + field_name,
            )

        alternate_manifest = production_stage_root / "alternate-manifest.json"
        alternate_manifest.write_bytes(production_stage_path.read_bytes())
        reject_cluster_config(
            lambda value: value.update(
                {"stage_manifest_path": str(alternate_manifest)}
            ),
            "stage manifest or six-tool binding differs",
            "substituted-stage-manifest",
        )

        production_authorization = Path(
            production_config["authorization_file"]
        )
        alternate_authorization = (
            production_authorization.parent / "alternate.authorization"
        )
        reject_cluster_config(
            lambda value: value.update(
                {"authorization_file": str(alternate_authorization)}
            ),
            "original authorization coordinate differs",
            "substituted-authorization-path",
        )

        def republish_authority_record(
            profile: str,
            mutation: Any,
        ) -> dict[str, Any]:
            candidate_matrix = json.loads(json.dumps(matrix))
            record = json.loads(
                json.dumps(
                    artifact_payloads[(profile, "cluster_authority")][0]
                )
            )
            mutation(record)
            _, digest, relative = publish(
                "cluster_authorities",
                profile,
                "cluster_authority",
                "json",
                _canonical_json_bytes(record),
            )
            candidate_matrix[
                f"{profile}_cluster_authority_path"
            ] = relative
            candidate_matrix[
                f"{profile}_cluster_authority_sha256"
            ] = digest
            return candidate_matrix

        def load_production_authority(
            candidate: dict[str, Any],
            *,
            expected_uid: int | None = None,
        ) -> RetainedClusterAuthority:
            return _load_retained_cluster_authority(
                matrix=candidate,
                profile="production",
                candidate_root=root,
                artifact_identities={},
                expected_uid=expected_uid,
            )

        retained_authorization = (
            production_retained_authority.authorization_artifact_path
        )
        retained_authorization.write_bytes(b"substituted\n")
        retained_authorization.chmod(0o600)
        require_failure(
            lambda: load_production_authority(matrix),
            "artifact hash is stale",
            "retained authorization bytes mutation was accepted",
        )
        retained_authorization.write_bytes(AUTHORIZATION_MARKER)
        retained_authorization.chmod(0o644)
        require_failure(
            lambda: load_production_authority(matrix),
            "retained authorization marker authority differs",
            "retained authorization mode mutation was accepted",
        )
        retained_authorization.chmod(0o600)
        require_failure(
            lambda: load_production_authority(
                matrix,
                expected_uid=os.getuid() + 1,
            ),
            "authorization mode or current uid differs",
            "retained authorization owner mutation was accepted",
        )

        authority_record_mutations = {
            "authorization-path": (
                lambda value: value.update(
                    {
                        "authorization_artifact_path": (
                            artifact_payloads[("production", "config")][4]
                        )
                    }
                ),
                "normalized relative direct child",
            ),
            "authorization-hash": (
                lambda value: value.update(
                    {"authorization_sha256": "0" * 64}
                ),
                "retained authorization marker authority differs",
            ),
            "authorization-artifact-hash": (
                lambda value: value.update(
                    {"authorization_artifact_sha256": "0" * 64}
                ),
                "artifact hash is stale",
            ),
            "authorization-mode": (
                lambda value: value.update({"authorization_mode": 0o644}),
                "authorization mode or current uid differs",
            ),
            "authorization-uid": (
                lambda value: value.update(
                    {"authorization_uid": os.getuid() + 1}
                ),
                "authorization mode or current uid differs",
            ),
        }
        for label, (
            mutation,
            diagnostic,
        ) in authority_record_mutations.items():
            mutated_matrix = republish_authority_record(
                "production",
                mutation,
            )
            require_failure(
                lambda mutated_matrix=mutated_matrix: (
                    load_production_authority(mutated_matrix)
                ),
                diagnostic,
                f"retained authority accepted {label} mutation",
            )

        authority_coordinate_mutations = {
            "system-identifier": lambda value: value.update(
                {"cluster_system_identifier": "7000000000000000099"}
            ),
            "data-directory": lambda value: value.update(
                {
                    "data_directory": str(
                        root / "runs" / "production" / "alternate-data"
                    ),
                    "original_authorization_path": str(
                        root
                        / "runs"
                        / "production"
                        / "alternate-data"
                        / AUTHORIZATION_MARKER_NAME
                    ),
                }
            ),
            "socket-directory": lambda value: value.update(
                {
                    "socket_directory": str(
                        root / "runs" / "production" / "alternate-socket"
                    )
                }
            ),
            "original-authorization-path": lambda value: value.update(
                {
                    "original_authorization_path": str(
                        root
                        / "runs"
                        / "production"
                        / "data"
                        / "alternate.authorization"
                    )
                }
            ),
            "connection-port": lambda value: value.update(
                {"connection_port": "45433"}
            ),
            "connection-user": lambda value: value.update(
                {"connection_user": "alternate_owner"}
            ),
        }
        for label, mutation in authority_coordinate_mutations.items():
            mutated_matrix = republish_authority_record(
                "production",
                mutation,
            )
            require_failure(
                lambda mutated_matrix=mutated_matrix: validate_matrix(
                    mutated_matrix
                ),
                (
                    "cluster coordinate or marker path differs"
                    if label == "original-authorization-path"
                    else "differs"
                ),
                f"runtime validator accepted authority {label} mutation",
            )

        stale_authority_hash = json.loads(json.dumps(matrix))
        stale_authority_hash[
            "production_cluster_authority_sha256"
        ] = "0" * 64
        require_failure(
            lambda: load_production_authority(stale_authority_hash),
            "artifact hash is stale",
            "retained authority accepted a stale record hash",
        )
        substituted_authority_path = json.loads(json.dumps(matrix))
        substituted_authority_path[
            "production_cluster_authority_path"
        ] = artifact_payloads[("production", "config")][4]
        substituted_authority_path[
            "production_cluster_authority_sha256"
        ] = artifact_payloads[("production", "config")][3]
        require_failure(
            lambda: load_production_authority(
                substituted_authority_path
            ),
            "normalized relative direct child",
            "retained authority accepted a substituted record path",
        )

        production_record = artifact_payloads[
            ("production", "cluster_authority")
        ][0]
        aliased_profile_matrix = republish_authority_record(
            "test_indexes",
            lambda value: value.update(
                {
                    "cluster_system_identifier": production_record[
                        "cluster_system_identifier"
                    ],
                    "data_directory": production_record[
                        "data_directory"
                    ],
                    "socket_directory": production_record[
                        "socket_directory"
                    ],
                    "original_authorization_path": production_record[
                        "original_authorization_path"
                    ],
                }
            ),
        )
        require_failure(
            lambda: validate_matrix(aliased_profile_matrix),
            "profile cluster authority coordinates must be distinct",
            "runtime validator accepted aliased cluster authority",
        )

        authored_matrix = json.loads(json.dumps(matrix))
        authored_matrix["required_coverage"] = []
        require_failure(
            lambda: validate_matrix(authored_matrix),
            "unknown fields: required_coverage",
            "runtime matrix accepted authored coverage",
        )
        substituted_case = json.loads(json.dumps(matrix))
        substituted_case["cases"][0]["expected_message"] = "substitute"
        require_failure(
            lambda: validate_matrix(substituted_case),
            "differs from the current case plan",
            "runtime matrix accepted a substituted planned field",
        )
        missing_case = json.loads(json.dumps(matrix))
        missing_case["cases"].pop()
        require_failure(
            lambda: validate_matrix(missing_case),
            "requires exactly 2 cases",
            "runtime matrix accepted a missing planned case",
        )
        for cardinality, label in (
            (EXPECTED_MATRIX_CASE_COUNT - 1, "49-case"),
            (EXPECTED_MATRIX_CASE_COUNT + 1, "51-case"),
        ):
            resealed_cardinality = json.loads(json.dumps(matrix))
            resealed_cardinality["cases"] = [
                json.loads(
                    json.dumps(matrix["cases"][index % len(matrix["cases"])])
                )
                for index in range(cardinality)
            ]
            require_failure(
                lambda candidate=resealed_cardinality: validate_matrix(
                    candidate,
                    expected_matrix_case_count=(
                        EXPECTED_MATRIX_CASE_COUNT
                    ),
                    expected_profile_case_count=1,
                ),
                "requires exactly 50 cases",
                f"runtime matrix accepted a {label} resealed matrix",
            )
        stale_stage = json.loads(json.dumps(matrix))
        stale_stage["production_stage_manifest_sha256"] = "0" * 64
        require_failure(
            lambda: validate_matrix(stale_stage),
            "production_stage_manifest_sha256 is stale",
            "runtime matrix accepted a stale stage manifest",
        )
        stale_suite = json.loads(json.dumps(matrix))
        stale_suite[
            "production_tool_signature_suite_sha256"
        ] = "0" * 64
        require_failure(
            lambda: validate_matrix(stale_suite),
            "production_tool_signature_suite_sha256 is stale",
            "runtime matrix accepted a stale six-tool suite",
        )
        require_failure(
            lambda: validate_matrix(
                matrix,
                test_indexes_dso_path=current_paths["production_dso"],
                test_indexes_dso_sha256=runtime_hashes[
                    "production_dso_sha256"
                ],
            ),
            "DSO paths and bytes must differ",
            "runtime validator accepted aliased profile DSOs",
        )
        require_failure(
            lambda: validate_matrix(
                matrix,
                test_indexes_test_executable_path=current_paths[
                    "production_test_executable"
                ],
                test_indexes_test_executable_sha256=runtime_hashes[
                    "production_test_executable_sha256"
                ],
            ),
            "harness paths and bytes must differ",
            "runtime validator accepted aliased harnesses",
        )
        require_failure(
            lambda: _validate_distinct_private_clusters(
                {
                    "production": ("same-system", root / "same-data"),
                    "test_indexes": (
                        "same-system",
                        root / "other-data",
                    ),
                }
            ),
            "distinct private cluster system identifiers",
            "runtime validator accepted one PostgreSQL system",
        )
        require_failure(
            lambda: _validate_distinct_private_clusters(
                {
                    "production": (
                        "production-system",
                        root / "same-data",
                    ),
                    "test_indexes": (
                        "test-system",
                        root / "same-data",
                    ),
                }
            ),
            "distinct private cluster system identifiers",
            "runtime validator accepted one data directory",
        )

        def republish_result(
            candidate: dict[str, Any],
            profile: str,
            result: dict[str, Any],
        ) -> None:
            result_raw = _canonical_json_bytes(result)
            path, digest, relative = publish(
                "results",
                profile,
                result["case_id"],
                "json",
                result_raw,
            )
            del path
            index = 0 if profile == "production" else 1
            candidate["cases"][index][
                "immutable_result_path"
            ] = relative
            candidate["cases"][index][
                "immutable_result_sha256"
            ] = digest

        def mutated_value(field_name: str, value: Any) -> Any:
            if isinstance(value, bool):
                return not value
            if isinstance(value, int):
                return value + 1
            if isinstance(value, dict):
                mutated = json.loads(json.dumps(value))
                mutated["ordering"] = "tampered"
                return mutated
            if field_name.endswith("_sha256"):
                return (
                    "f" * 64 if value != "f" * 64 else "e" * 64
                )
            return "tampered"

        for attestation_field in sorted(RUNTIME_ATTESTATION_FIELDS):
            candidate = json.loads(json.dumps(matrix))
            result = json.loads(json.dumps(results["production"]))
            result["attestation"][attestation_field] = mutated_value(
                attestation_field,
                result["attestation"][attestation_field],
            )
            republish_result(candidate, "production", result)
            require_failure(
                lambda candidate=candidate: validate_matrix(candidate),
                "",
                "runtime validator accepted mutated attestation field "
                + attestation_field,
            )

        artifact_field_pairs = {
            "request": ("request_path", "request_sha256"),
            "catalog": ("catalog_path", "catalog_sha256"),
            "activation": (
                "activation_receipt_path",
                "activation_receipt_sha256",
            ),
            "config": (
                "cluster_config_path",
                "cluster_config_sha256",
            ),
            "raw_result": ("raw_result_path", "raw_result_sha256"),
        }
        artifact_categories = {
            "request": ("requests", "runtime-production"),
            "catalog": ("catalogs", "case_catalog"),
            "activation": ("activations", "activation"),
            "config": ("cluster_configs", "cluster_config"),
            "raw_result": ("raw_results", "runtime-production"),
        }
        for artifact_name, (
            path_field,
            hash_field,
        ) in artifact_field_pairs.items():
            candidate = json.loads(json.dumps(matrix))
            result = json.loads(json.dumps(results["production"]))
            payload = json.loads(
                json.dumps(
                    artifact_payloads[("production", artifact_name)][0]
                )
            )
            payload["adversarial_unknown"] = True
            category, stem = artifact_categories[artifact_name]
            _, digest, relative = publish(
                category,
                "production",
                stem,
                "json",
                _canonical_json_bytes(payload),
            )
            result["attestation"][path_field] = relative
            result["attestation"][hash_field] = digest
            republish_result(candidate, "production", result)
            require_failure(
                lambda candidate=candidate: validate_matrix(candidate),
                "",
                f"runtime validator accepted mutated {artifact_name}",
            )

        recovery_candidate = json.loads(json.dumps(matrix))
        recovery_result = json.loads(
            json.dumps(results["production"])
        )
        _, recovery_digest, recovery_relative = publish(
            "recoveries",
            "production",
            "runtime-production",
            "json",
            _canonical_json_bytes([["tampered"]]),
        )
        recovery_result["attestation"][
            "recovery_artifact_path"
        ] = recovery_relative
        recovery_result["attestation"][
            "recovery_artifact_sha256"
        ] = recovery_digest
        recovery_result["attestation"][
            "recovery_rows_sha256"
        ] = recovery_digest
        republish_result(
            recovery_candidate,
            "production",
            recovery_result,
        )
        require_failure(
            lambda: validate_matrix(recovery_candidate),
            "recovery artifact/result/plan join differs",
            "runtime validator accepted mutated recovery rows",
        )

        log_candidate = json.loads(json.dumps(matrix))
        log_payload = json.loads(
            json.dumps(artifact_payloads[("production", "log")][0])
        )
        log_payload["context"] = "substituted/context"
        _, log_digest, log_relative = publish(
            "logs",
            "production",
            "runtime-production",
            "log.json",
            _canonical_json_bytes(log_payload),
        )
        log_candidate["cases"][0]["immutable_test_log_path"] = (
            log_relative
        )
        log_candidate["cases"][0]["immutable_test_log_sha256"] = (
            log_digest
        )
        require_failure(
            lambda: validate_matrix(log_candidate),
            "test log identity/status differs",
            "runtime validator accepted a substituted log",
        )

        absolute_result = json.loads(json.dumps(matrix))
        absolute_result["cases"][0]["immutable_result_path"] = str(
            artifact_payloads[("production", "result")][2]
        )
        require_failure(
            lambda: validate_matrix(absolute_result),
            "normalized relative direct child",
            "runtime validator accepted an absolute result path",
        )
        escaping_result = json.loads(json.dumps(matrix))
        escaping_result["cases"][0][
            "immutable_result_path"
        ] = "../result.json"
        require_failure(
            lambda: validate_matrix(escaping_result),
            "normalized relative direct child",
            "runtime validator accepted a parent escape",
        )

        result_payload = artifact_payloads[("production", "result")]
        result_path = result_payload[2]
        result_raw = result_payload[1]
        symlink_target = write(root / "result-backup.json", result_raw)
        result_path.unlink()
        result_path.symlink_to(symlink_target)
        require_failure(
            lambda: validate_matrix(matrix),
            "symbolic link",
            "runtime validator accepted a symlinked result",
        )
        result_path.unlink()
        result_path.write_bytes(result_raw)

        alias_raw = _canonical_json_bytes({"alias": True})
        alias_digest = _artifact_sha256(alias_raw)
        alias_request = write(
            evidence_root
            / "requests"
            / "production"
            / f"alias.{alias_digest}.json",
            alias_raw,
        )
        external_alias = root.parent / (
            root.name + "-external-hardlink.json"
        )
        external_alias.unlink(missing_ok=True)
        os.link(alias_request, external_alias)
        alias_registry: dict[
            tuple[int, int],
            tuple[Any, ...],
        ] = {}
        try:
            require_failure(
                lambda: _load_candidate_artifact(
                    candidate_root=root,
                    path_text=str(alias_request.relative_to(root)),
                    category="requests",
                    profile="production",
                    stem="alias",
                    suffix="json",
                    expected_sha256=alias_digest,
                    context="self-test-hardlink-request",
                    artifact_identities=alias_registry,
                    semantic_identity=("request", "production", "alias"),
                ),
                "exactly one hard link",
                "runtime artifact loader accepted one referenced artifact "
                "with an external hard link",
            )
        finally:
            external_alias.unlink(missing_ok=True)
        real_component = root / "real-component"
        real_component.mkdir()
        component_file = write(
            real_component / "artifact.json",
            b"{}\n",
        )
        symlink_component = root / "symlink-component"
        symlink_component.symlink_to(real_component, target_is_directory=True)
        require_failure(
            lambda: _canonical_regular_file(
                symlink_component / component_file.name,
                "self-test-symlink-component",
            ),
            "symbolic link",
            "runtime validator accepted a symlink path component",
        )
        real_candidate = root / "real-candidate"
        real_candidate.mkdir()
        write(real_candidate / "runtime-matrix.json", b"{}\n")
        candidate_symlink = root / "candidate-symlink"
        candidate_symlink.symlink_to(
            real_candidate,
            target_is_directory=True,
        )
        require_failure(
            lambda: _candidate_root(
                candidate_symlink / "runtime-matrix.json"
            ),
            "symbolic link",
            "runtime validator accepted a symlinked candidate root",
        )
        fifo_candidate = root / "fifo-candidate"
        fifo_candidate.mkdir()
        fifo_matrix = fifo_candidate / "runtime-matrix.json"
        os.mkfifo(fifo_matrix, mode=0o600)
        require_failure(
            lambda: _candidate_root(fifo_matrix),
            "not a regular non-symlink file",
            "runtime validator accepted a FIFO candidate matrix",
        )


def _run_runtime_contract_self_tests(
    sql_identity: str,
    runtime_policy_raw: bytes,
    fault_key: str,
) -> None:
    producer_sha256 = _artifact_sha256(b"runtime-inventory-producer")
    runtime_producer_sha256 = _artifact_sha256(
        b"runtime-evidence-producer"
    )
    source_span = {
        "begin": {"byte_offset": 100, "line": 10, "column": 3},
        "end": {"byte_offset": 120, "line": 10, "column": 23},
    }
    owner_usr = "c:@F@runtime_fixture"
    call_path = [
        {
            "usr": owner_usr,
            "qualified_declaration": "runtime_fixture(int)",
            "source_file": "fixture/runtime.cppm",
            "source_span": source_span,
        }
    ]
    relations = {
        "production": "injected_routes_to_site",
        "test_indexes": "injected_after_site_success",
    }
    runtime_ledgers: dict[str, dict[str, Any]] = {}
    runtime_ledger_documents: dict[str, dict[str, Any]] = {}
    runtime_ledger_hashes: dict[str, str] = {}
    runtime_inventory_documents: dict[str, dict[str, Any]] = {}

    def anchor(
        *,
        profile: str,
        kind: str,
        compiler_stable_id: str,
    ) -> dict[str, Any]:
        return {
            "kind": kind,
            "compiler_stable_id": compiler_stable_id,
            "enclosing_function_usr": owner_usr,
            "source_file": "fixture/runtime.cppm",
            "expanded_source_span": source_span,
            "source_fragment_sha256": _artifact_sha256(
                f"{profile}:{kind}:{compiler_stable_id}".encode()
            ),
        }

    for profile in ("production", "test_indexes"):
        compiler_site_id = f"clang:runtime:{profile}:error-site"
        ledger_site = {
            "site_id": compiler_site_id,
            "source_file": "fixture/runtime.cppm",
            "expanded_source_span": source_span,
            "transitive_call_path": call_path,
            "classification": "bridged",
            "bridge": "generated_boundary_tail",
            "converter_families": [],
            "owner_families": [],
            "cleanup_phases": [],
        }
        ledger_site_sha256 = _artifact_sha256(
            _canonical_json_bytes(ledger_site)
        )
        sql_manifest_sha256 = _artifact_sha256(
            f"runtime-sql-manifest:{profile}".encode()
        )
        runtime_ledgers[profile] = {
            "profile": profile,
            "sql_manifest_sha256": sql_manifest_sha256,
            "parents": {
                sql_identity: {
                    "sites": [ledger_site],
                }
            },
        }
        ledger_document = {
            "producer_executable_sha256": producer_sha256,
            "compiler_facts_sha256": _artifact_sha256(
                f"compiler-facts:{profile}".encode()
            ),
            "source_list_sha256": _artifact_sha256(
                f"source-list:{profile}".encode()
            ),
            "compile_database_sha256": _artifact_sha256(
                f"compile-database:{profile}".encode()
            ),
            "compile_graph_sha256": _artifact_sha256(
                f"compile-graph:{profile}".encode()
            ),
            "pcm_manifest_sha256": _artifact_sha256(
                f"pcm-manifest:{profile}".encode()
            ),
        }
        runtime_ledger_documents[profile] = ledger_document
        ledger_sha256 = _artifact_sha256(
            f"runtime-ledger:{profile}".encode()
        )
        runtime_ledger_hashes[profile] = ledger_sha256
        relation = relations[profile]
        runtime_inventory_documents[profile] = {
            "schema_revision": 1,
            "profile": profile,
            "probe_abi_revision": 2,
            "ledger_sha256": ledger_sha256,
            "compiler_facts_sha256": ledger_document[
                "compiler_facts_sha256"
            ],
            "sql_manifest_sha256": sql_manifest_sha256,
            "source_list_sha256": ledger_document[
                "source_list_sha256"
            ],
            "compile_database_sha256": ledger_document[
                "compile_database_sha256"
            ],
            "compile_graph_sha256": ledger_document[
                "compile_graph_sha256"
            ],
            "pcm_manifest_sha256": ledger_document[
                "pcm_manifest_sha256"
            ],
            "inventory_producer_executable_sha256": producer_sha256,
            "entries": [
                {
                    "sql_identity": sql_identity,
                    "fault_key": fault_key,
                    "site_id": _probe_site_id(
                        sql_identity,
                        fault_key,
                    ),
                    "site_revision": 2,
                    "relation": relation,
                    "ledger_site_sha256": ledger_site_sha256,
                    "site_anchor": anchor(
                        profile=profile,
                        kind="postgres_error_site",
                        compiler_stable_id=compiler_site_id,
                    ),
                    "visit_anchor": anchor(
                        profile=profile,
                        kind="record_site_visit",
                        compiler_stable_id=(
                            f"clang:runtime:{profile}:visit"
                        ),
                    ),
                    "trigger_anchor": anchor(
                        profile=profile,
                        kind="consume_fault",
                        compiler_stable_id=(
                            f"clang:runtime:{profile}:trigger"
                        ),
                    ),
                    "transitive_call_path": call_path,
                    "probe_contract": _expected_probe_contract(
                        relation
                    ),
                }
            ],
        }

    def require_failure(
        operation: Any,
        expected_diagnostic: str,
        label: str,
    ) -> None:
        try:
            operation()
        except EvidenceError as error:
            if expected_diagnostic not in str(error):
                raise
        else:
            _fail("self-test", label)

    def require_closed_revision_rejections(
        fields: frozenset[str],
        *,
        field_name: str,
        legacy_field_name: str | None,
        expected_revision: int,
        context: str,
    ) -> None:
        baseline = {name: None for name in fields}
        baseline[field_name] = expected_revision

        def validate(candidate: dict[str, Any]) -> None:
            _require_exact_fields(candidate, fields, context)
            if _integer(
                _field(candidate, field_name, context),
                f"{context}.{field_name}",
            ) != expected_revision:
                _fail(context, f"unsupported {field_name}")

        missing = dict(baseline)
        missing.pop(field_name)
        require_failure(
            lambda: validate(missing),
            field_name,
            f"{context} accepted a missing revision",
        )

        wrong_type = dict(baseline)
        wrong_type[field_name] = str(expected_revision)
        require_failure(
            lambda: validate(wrong_type),
            "expected a non-negative integer",
            f"{context} accepted a text revision",
        )

        wrong_revision = dict(baseline)
        wrong_revision[field_name] = expected_revision + 1
        require_failure(
            lambda: validate(wrong_revision),
            f"unsupported {field_name}",
            f"{context} accepted the wrong revision",
        )

        if legacy_field_name is not None:
            legacy = dict(baseline)
            legacy[legacy_field_name] = legacy.pop(field_name)
            require_failure(
                lambda: validate(legacy),
                field_name,
                f"{context} accepted a legacy revision key",
            )

    for (
        fields,
        field_name,
        legacy_field_name,
        expected_revision,
        context,
    ) in (
        (
            LEDGER_FIELDS,
            "schema_revision",
            "schema_version",
            2,
            "ledger-revision",
        ),
        (
            PROVENANCE_FIELDS,
            "schema_revision",
            "schema_version",
            2,
            "provenance-revision",
        ),
        (
            RUNTIME_POLICY_FIELDS,
            "schema_revision",
            "schema_version",
            2,
            "runtime-policy-revision",
        ),
        (
            RUNTIME_MATRIX_FIELDS,
            "schema_revision",
            "schema_version",
            2,
            "runtime-matrix-revision",
        ),
        (
            RUNTIME_MATRIX_CASE_FIELDS,
            "probe_abi_revision",
            "probe_abi_version",
            2,
            "runtime-matrix-case-abi-revision",
        ),
        (
            IMMUTABLE_RUNTIME_RESULT_FIELDS,
            "schema_revision",
            "schema_version",
            2,
            "immutable-result-revision",
        ),
        (
            IMMUTABLE_RUNTIME_RESULT_FIELDS,
            "probe_abi_revision",
            "probe_abi_version",
            2,
            "immutable-result-abi-revision",
        ),
        (
            RUNTIME_ATTESTATION_FIELDS,
            "schema_revision",
            "schema_version",
            1,
            "runtime-attestation-revision",
        ),
        (
            RUNTIME_ATTESTATION_FIELDS,
            "probe_abi_revision",
            "probe_abi_version",
            2,
            "runtime-attestation-abi-revision",
        ),
        (
            CASE_PLAN_FIELDS,
            "schema_revision",
            "schema_version",
            2,
            "case-plan-revision",
        ),
        (
            CASE_PLAN_FIELDS,
            "probe_abi_revision",
            "probe_abi_version",
            2,
            "case-plan-abi-revision",
        ),
        (
            CATALOG_FIELDS,
            "schema_revision",
            "schema_version",
            2,
            "catalog-revision",
        ),
        (
            REQUEST_FIELDS,
            "schema_revision",
            "schema_version",
            2,
            "request-revision",
        ),
        (
            RAW_RESULT_FIELDS,
            "schema_revision",
            "schema_version",
            2,
            "raw-result-revision",
        ),
        (
            RAW_RESULT_FIELDS,
            "probe_abi_revision",
            "probe_abi_version",
            2,
            "raw-result-abi-revision",
        ),
        (
            FAULT_EVIDENCE_FIELDS,
            "probe_abi_revision",
            "probe_abi_version",
            2,
            "fault-evidence-abi-revision",
        ),
        (
            TERMINAL_MARKER_FIELDS,
            "schema_revision",
            "schema_version",
            1,
            "terminal-marker-revision",
        ),
        (
            ACTIVATION_RECEIPT_FIELDS,
            "schema_revision",
            "schema_version",
            1,
            "activation-receipt-revision",
        ),
        (
            CLUSTER_CONFIG_FIELDS,
            "schema_revision",
            "schema_version",
            1,
            "cluster-config-revision",
        ),
        (
            CLUSTER_AUTHORITY_FIELDS,
            "schema_revision",
            "schema_version",
            1,
            "cluster-authority-revision",
        ),
        (
            CASE_LOG_FIELDS,
            "schema_revision",
            "schema_version",
            1,
            "case-log-revision",
        ),
        (
            COMPILER_ANALYSIS_FACT_FIELDS,
            "schema_revision",
            "schema_version",
            2,
            "compiler-facts-revision",
        ),
        (
            RUNTIME_INVENTORY_FIELDS,
            "schema_revision",
            "schema_version",
            1,
            "runtime-inventory-revision",
        ),
        (
            RUNTIME_INVENTORY_FIELDS,
            "probe_abi_revision",
            "probe_abi_version",
            2,
            "runtime-inventory-abi-revision",
        ),
        (
            RUNTIME_INVENTORY_ENTRY_FIELDS,
            "site_revision",
            None,
            2,
            "runtime-site-revision",
        ),
        (
            PROBE_CONTRACT_FIELDS,
            "abi_revision",
            "abi_version",
            2,
            "probe-contract-revision",
        ),
    ):
        require_closed_revision_rejections(
            fields,
            field_name=field_name,
            legacy_field_name=legacy_field_name,
            expected_revision=expected_revision,
            context=context,
        )

    runtime_inventories: dict[str, dict[str, Any]] = {}
    runtime_inventory_hashes: dict[str, str] = {}
    for profile in ("production", "test_indexes"):
        inventory_document = runtime_inventory_documents[profile]
        inventory_raw = _canonical_json_bytes(inventory_document)
        inventory_sha256 = _artifact_sha256(inventory_raw)
        runtime_inventory_hashes[profile] = inventory_sha256
        runtime_inventories[profile] = validate_runtime_inventory(
            inventory_document,
            profile=profile,
            inventory_sha256=inventory_sha256,
            ledger_document=runtime_ledger_documents[profile],
            ledger=runtime_ledgers[profile],
            ledger_sha256=runtime_ledger_hashes[profile],
            inventory_producer_executable_sha256=producer_sha256,
        )

    generic_fault_key = "invoke_pg_as_cpp.cleanup_restore"
    generic_inventory_document = json.loads(
        json.dumps(runtime_inventory_documents["production"])
    )
    generic_inventory_document["entries"][0]["fault_key"] = (
        generic_fault_key
    )
    generic_site_id = _probe_site_id(
        sql_identity,
        generic_fault_key,
    )
    generic_inventory_document["entries"][0]["site_id"] = (
        generic_site_id
    )
    generic_inventory = validate_runtime_inventory(
        generic_inventory_document,
        profile="production",
        inventory_sha256="0" * 64,
        ledger_document=runtime_ledger_documents["production"],
        ledger=runtime_ledgers["production"],
        ledger_sha256=runtime_ledger_hashes["production"],
        inventory_producer_executable_sha256=producer_sha256,
    )
    generic_inventory_key = (sql_identity, generic_fault_key)
    if (
        set(generic_inventory["entries"]) != {
            generic_inventory_key
        }
        or generic_inventory["entries"][
            generic_inventory_key
        ]["site_id"]
            != generic_site_id
    ):
        _fail(
            "self-test",
            "runtime inventory rejected a valid generic fault key",
        )
    for invalid_fault_key in (
        "invalid-key",
        "Invalid",
        "a" * 129,
    ):
        invalid_key_inventory = json.loads(
            json.dumps(
                runtime_inventory_documents["production"]
            )
        )
        invalid_key_inventory["entries"][0]["fault_key"] = (
            invalid_fault_key
        )
        require_failure(
            lambda candidate=invalid_key_inventory: (
                validate_runtime_inventory(
                    candidate,
                    profile="production",
                    inventory_sha256="0" * 64,
                    ledger_document=(
                        runtime_ledger_documents["production"]
                    ),
                    ledger=runtime_ledgers["production"],
                    ledger_sha256=(
                        runtime_ledger_hashes["production"]
                    ),
                    inventory_producer_executable_sha256=(
                        producer_sha256
                    ),
                )
            ),
            "containing only [a-z0-9_.]",
            "runtime inventory accepted an invalid fault key",
        )

    stale_site_inventory = json.loads(
        json.dumps(runtime_inventory_documents["production"])
    )
    stale_site_inventory["entries"][0]["site_id"] = (
        "probe:" + "0" * 64
    )
    require_failure(
        lambda: validate_runtime_inventory(
            stale_site_inventory,
            profile="production",
            inventory_sha256="0" * 64,
            ledger_document=runtime_ledger_documents["production"],
            ledger=runtime_ledgers["production"],
            ledger_sha256=runtime_ledger_hashes["production"],
            inventory_producer_executable_sha256=producer_sha256,
        ),
        "site_id is not the exact sealed ABI revision 2 ID",
        "runtime inventory accepted a stale sealed site ID",
    )
    legacy_site_inventory = json.loads(
        json.dumps(runtime_inventory_documents["production"])
    )
    legacy_site_inventory["entries"][0]["site_id"] = (
        "probe:v2:" + "0" * 64
    )
    require_failure(
        lambda: validate_runtime_inventory(
            legacy_site_inventory,
            profile="production",
            inventory_sha256="0" * 64,
            ledger_document=runtime_ledger_documents["production"],
            ledger=runtime_ledgers["production"],
            ledger_sha256=runtime_ledger_hashes["production"],
            inventory_producer_executable_sha256=producer_sha256,
        ),
        "site_id is not the exact sealed ABI revision 2 ID",
        "runtime inventory accepted a legacy versioned site ID",
    )
    stale_contract_inventory = json.loads(
        json.dumps(runtime_inventory_documents["production"])
    )
    stale_contract_inventory["entries"][0]["probe_contract"][
        "ordering"
    ] = "visit_before_trigger"
    require_failure(
        lambda: validate_runtime_inventory(
            stale_contract_inventory,
            profile="production",
            inventory_sha256="0" * 64,
            ledger_document=runtime_ledger_documents["production"],
            ledger=runtime_ledgers["production"],
            ledger_sha256=runtime_ledger_hashes["production"],
            inventory_producer_executable_sha256=producer_sha256,
        ),
        "relation-derived ABI revision 2 contract",
        "runtime inventory accepted a stale probe contract",
    )
    authored_inventory = json.loads(
        json.dumps(runtime_inventory_documents["production"])
    )
    authored_inventory["entries"][0]["coverage"] = []
    require_failure(
        lambda: validate_runtime_inventory(
            authored_inventory,
            profile="production",
            inventory_sha256="0" * 64,
            ledger_document=runtime_ledger_documents["production"],
            ledger=runtime_ledgers["production"],
            ledger_sha256=runtime_ledger_hashes["production"],
            inventory_producer_executable_sha256=producer_sha256,
        ),
        "unknown fields: coverage",
        "runtime inventory accepted serialized coverage authority",
    )
    require_failure(
        lambda: validate_runtime_inventory(
            runtime_inventory_documents["production"],
            profile="production",
            inventory_sha256=runtime_inventory_hashes["production"],
            ledger_document=runtime_ledger_documents["production"],
            ledger=runtime_ledgers["production"],
            ledger_sha256=runtime_ledger_hashes["production"],
            inventory_producer_executable_sha256="f" * 64,
        ),
        "inventory producer differs from the ledger producer executable",
        "runtime inventory accepted a different codegen producer",
    )

    _run_runtime_artifact_graph_self_tests(
        sql_identity=sql_identity,
        fault_key=fault_key,
        runtime_policy_raw=runtime_policy_raw,
        runtime_ledgers=runtime_ledgers,
        runtime_ledger_hashes=runtime_ledger_hashes,
        runtime_inventories=runtime_inventories,
        runtime_inventory_hashes=runtime_inventory_hashes,
        runtime_producer_sha256=runtime_producer_sha256,
        require_failure=require_failure,
    )
    return


def run_python_self_tests() -> None:
    """Run deterministic positive and negative verifier fixtures."""

    try:
        json.loads(
            b'{"duplicate":1,"duplicate":2}',
            object_pairs_hook=_reject_duplicate_json_keys,
        )
    except EvidenceError:
        pass
    else:
        _fail("self-test", "duplicate JSON object keys were accepted")

    provenance_path = (
        Path(__file__).resolve().parent
        / "tests"
        / "contract"
        / "fixtures"
        / "pg17_provenance.json"
    )
    provenance, provenance_raw = load_json(provenance_path)
    validate_provenance(provenance)
    external_provenance = json.loads(json.dumps(provenance))
    external_record = dict(external_provenance["records"][0])
    external_record.update(
        {
            "record_id": "self-test.standard_library",
            "kind": "standard_library_declaration",
            "upstream_file":
                "/fixture/libcxx/include/self_test.h",
            "severities": [],
            "library_version": "clang:self-test",
            "user_code_callback_free": True,
            "target_usr": "c:@N@std@F@self_test#",
            "target_qualified_name": "std::self_test",
            "authority_id": "clang:self-test",
        }
    )
    external_provenance["records"].append(external_record)
    validate_provenance(external_provenance)

    def require_provenance_failure(
        candidate: dict[str, Any],
        expected: str,
        label: str,
    ) -> None:
        try:
            validate_provenance(candidate)
        except EvidenceError as error:
            if expected not in str(error):
                raise
        else:
            _fail("self-test", label)

    invalid_callback_type = json.loads(
        json.dumps(external_provenance)
    )
    invalid_callback_type["records"][-1][
        "user_code_callback_free"
    ] = "false"
    require_provenance_failure(
        invalid_callback_type,
        "expected a boolean",
        "string callback-free provenance was accepted",
    )
    stale_provenance_schema = json.loads(json.dumps(provenance))
    stale_provenance_schema["schema_revision"] = 1
    require_provenance_failure(
        stale_provenance_schema,
        "unsupported schema revision",
        "schema revision 1 provenance was accepted",
    )
    pfree_index = next(
        index
        for index, record in enumerate(provenance["records"])
        if record["record_id"] == "pg17.pfree"
    )
    missing_pfree_definition = json.loads(json.dumps(provenance))
    missing_pfree_definition["records"][pfree_index].pop(
        "definition"
    )
    require_provenance_failure(
        missing_pfree_definition,
        "missing required field 'definition'",
        "pfree provenance without a definition was accepted",
    )
    stale_pfree_definition = json.loads(json.dumps(provenance))
    stale_pfree_definition["records"][pfree_index][
        "definition"
    ]["fragment_sha256"] = "0" * 64
    require_provenance_failure(
        stale_pfree_definition,
        "declaration/definition provenance is stale",
        "pfree provenance with a stale definition hash was accepted",
    )
    definition_on_seed = json.loads(json.dumps(provenance))
    definition_on_seed["records"][0]["definition"] = json.loads(
        json.dumps(
            provenance["records"][pfree_index]["definition"]
        )
    )
    require_provenance_failure(
        definition_on_seed,
        "non-PostgreSQL-leaf provenance carries a definition chain",
        "a seed record carrying a definition chain was accepted",
    )
    stale_seed_severity = json.loads(json.dumps(provenance))
    stale_seed_severity["records"][0]["severities"] = [
        "fatal",
        "panic",
    ]
    require_provenance_failure(
        stale_seed_severity,
        "PostgreSQL file, byte span, or hash is stale",
        "a required seed with incomplete severity authority was accepted",
    )
    missing_callback_contract = json.loads(
        json.dumps(external_provenance)
    )
    missing_callback_contract["records"][-1].pop(
        "user_code_callback_free"
    )
    require_provenance_failure(
        missing_callback_contract,
        "missing required field",
        "library provenance without callback contract was accepted",
    )
    invalid_nonlibrary_contract = json.loads(
        json.dumps(provenance)
    )
    invalid_nonlibrary_contract["records"][0][
        "user_code_callback_free"
    ] = True
    require_provenance_failure(
        invalid_nonlibrary_contract,
        "non-library provenance carries",
        "non-library callback contract was accepted",
    )
    invalid_nonlibrary_version = json.loads(
        json.dumps(provenance)
    )
    invalid_nonlibrary_version["records"][0][
        "library_version"
    ] = "forbidden-library-version"
    require_provenance_failure(
        invalid_nonlibrary_version,
        "non-library provenance carries",
        "non-library version contract was accepted",
    )
    runtime_policy_path = (
        Path(__file__).resolve().parent
        / "policy"
        / "postgres_error_safety_policy.json"
    )
    runtime_policy, runtime_policy_raw = load_json(runtime_policy_path)
    if runtime_policy.get("schema_revision") != 2:
        _fail(
            "self-test",
            "reviewed runtime policy is not schema revision 2",
        )
    _require_byte_equality(
        runtime_policy_raw,
        runtime_policy_path,
        "self-test-reviewed-byte-equality",
    )
    try:
        _require_byte_equality(
            runtime_policy_raw + b"\n",
            runtime_policy_path,
            "self-test-reviewed-byte-drift",
        )
    except EvidenceError as error:
        if "differs byte-for-byte" not in str(error):
            raise
    else:
        _fail("self-test", "reviewed artifact byte drift was accepted")
    with tempfile.TemporaryDirectory() as temporary:
        review_root = Path(temporary).resolve()
        candidate_path = review_root / "candidate.json"
        independent_path = review_root / "reviewed.json"
        candidate_path.write_bytes(runtime_policy_raw)
        independent_path.write_bytes(runtime_policy_raw)
        _require_byte_equality(
            runtime_policy_raw,
            independent_path,
            "self-test-independent-reviewed-file",
            actual_path=candidate_path,
        )

        symlink_path = review_root / "reviewed-symlink.json"
        symlink_path.symlink_to(candidate_path)
        try:
            _require_byte_equality(
                runtime_policy_raw,
                symlink_path,
                "self-test-reviewed-symlink",
                actual_path=candidate_path,
            )
        except EvidenceError as error:
            if "symbolic link" not in str(error):
                raise
        else:
            _fail("self-test", "reviewed symlink was accepted")

        hardlink_path = review_root / "reviewed-hardlink.json"
        os.link(candidate_path, hardlink_path)
        try:
            _require_byte_equality(
                runtime_policy_raw,
                hardlink_path,
                "self-test-reviewed-hardlink",
                actual_path=candidate_path,
            )
        except EvidenceError as error:
            if "exactly one hard link" not in str(error):
                raise
        else:
            _fail("self-test", "reviewed hardlink was accepted")

        fifo_path = review_root / "reviewed-fifo.json"
        os.mkfifo(fifo_path, mode=0o600)
        try:
            _require_byte_equality(
                runtime_policy_raw,
                fifo_path,
                "self-test-reviewed-fifo",
                actual_path=candidate_path,
            )
        except EvidenceError as error:
            if "not a regular non-symlink file" not in str(error):
                raise
        else:
            _fail("self-test", "reviewed FIFO was accepted")
    reviewed_runtime_coverage = validate_runtime_policy(runtime_policy)
    if {key[0] for key in reviewed_runtime_coverage} != set(PROFILES):
        _fail(
            "self-test",
            "reviewed runtime policy does not cover both compiler profiles",
        )

    def require_runtime_policy_failure(
        candidate: dict[str, Any],
        expected: str,
        label: str,
    ) -> None:
        try:
            validate_runtime_policy(candidate)
        except EvidenceError as error:
            if expected not in str(error):
                raise
        else:
            _fail("self-test", label)

    stale_runtime_policy_revision = json.loads(
        json.dumps(runtime_policy)
    )
    stale_runtime_policy_revision["schema_revision"] = 1
    require_runtime_policy_failure(
        stale_runtime_policy_revision,
        "unsupported schema revision",
        "schema revision 1 runtime policy was accepted",
    )
    missing_field_runtime_policy = json.loads(json.dumps(runtime_policy))
    missing_field_runtime_policy.pop("required_coverage")
    require_runtime_policy_failure(
        missing_field_runtime_policy,
        "missing fields: required_coverage",
        "runtime policy with a missing field was accepted",
    )
    unknown_field_runtime_policy = json.loads(json.dumps(runtime_policy))
    unknown_field_runtime_policy["relation_policy"] = []
    require_runtime_policy_failure(
        unknown_field_runtime_policy,
        "unknown fields: relation_policy",
        "runtime policy with an unknown field was accepted",
    )
    unknown_coverage_field_runtime_policy = json.loads(
        json.dumps(runtime_policy)
    )
    unknown_coverage_field_runtime_policy["required_coverage"][0][
        "relation_policy"
    ] = "forbidden"
    require_runtime_policy_failure(
        unknown_coverage_field_runtime_policy,
        "unknown fields: relation_policy",
        "runtime policy coverage row with an unknown field was accepted",
    )
    incomplete_runtime_policy = json.loads(json.dumps(runtime_policy))
    incomplete_runtime_policy["required_coverage"].pop()
    require_runtime_policy_failure(
        incomplete_runtime_policy,
        "missing mandatory two-profile coverage keys",
        "runtime coverage omission was accepted",
    )

    def validator_arguments(
        *argument_groups: tuple[tuple[str, str], ...],
        **overrides: Any,
    ) -> argparse.Namespace:
        values: dict[str, Any] = {
            "self_test": False,
            "dso_only": False,
            "static_only": False,
            "evidence_only": False,
            "dso": None,
            "symbol_dump": None,
            "sql_manifest": None,
        }
        for _, attribute in (
            STATIC_VALIDATOR_ARGUMENTS
            + RUNTIME_VALIDATOR_ARGUMENTS
            + ACTIVE_DSO_VALIDATOR_ARGUMENTS
        ):
            values.setdefault(attribute, None)
        for group in argument_groups:
            for _, attribute in group:
                values[attribute] = Path(f"/self-test/{attribute}")
        values.update(overrides)
        return argparse.Namespace(**values)

    def require_command_line_failure(
        arguments: argparse.Namespace,
        expected: str,
        label: str,
    ) -> None:
        try:
            _validate_command_line(arguments)
        except EvidenceError as error:
            if expected not in str(error):
                raise
        else:
            _fail("self-test", label)

    static_arguments = validator_arguments(
        STATIC_VALIDATOR_ARGUMENTS,
        static_only=True,
    )
    if _validate_command_line(static_arguments) != "static":
        _fail("self-test", "static-only validator mode changed")
    require_command_line_failure(
        validator_arguments(
            STATIC_VALIDATOR_ARGUMENTS,
            static_only=True,
            runtime_policy=Path("/self-test/runtime-policy"),
        ),
        "not valid for static-only",
        "static-only accepted a runtime argument",
    )
    incomplete_static_arguments = validator_arguments(
        STATIC_VALIDATOR_ARGUMENTS,
        static_only=True,
    )
    incomplete_static_arguments.production_ledger = None
    require_command_line_failure(
        incomplete_static_arguments,
        "missing --production-ledger",
        "static-only accepted a missing static argument",
    )

    evidence_arguments = validator_arguments(
        STATIC_VALIDATOR_ARGUMENTS,
        RUNTIME_VALIDATOR_ARGUMENTS,
        evidence_only=True,
    )
    if _validate_command_line(evidence_arguments) != "evidence":
        _fail("self-test", "evidence-only validator mode changed")
    require_command_line_failure(
        validator_arguments(
            STATIC_VALIDATOR_ARGUMENTS,
            RUNTIME_VALIDATOR_ARGUMENTS,
            evidence_only=True,
            sql_manifest=Path("/self-test/active-manifest"),
        ),
        "not valid for evidence-only",
        "evidence-only accepted an active DSO argument",
    )

    dso_arguments = validator_arguments(
        dso_only=True,
        sql_manifest=Path("/self-test/active-manifest"),
        dso=Path("/self-test/active-dso"),
    )
    if _validate_command_line(dso_arguments) != "dso":
        _fail("self-test", "dso-only validator mode changed")
    manifest_dso_arguments = validator_arguments(
        dso_only=True,
        sql_manifest=Path("/self-test/cache-manifest"),
        dso=Path("/self-test/cache-dso"),
        dso_contract="manifest",
        allowed_dso_export=["Pg_magic_func", "_PG_init"],
    )
    if _validate_command_line(manifest_dso_arguments) != "dso":
        _fail("self-test", "manifest DSO contract mode changed")
    require_command_line_failure(
        validator_arguments(
            dso_only=True,
            sql_manifest=Path("/self-test/cache-manifest"),
            dso=Path("/self-test/cache-dso"),
            allowed_dso_export=["Pg_magic_func"],
        ),
        "requires --dso-contract manifest",
        "Bioseq DSO contract accepted a non-fmgr ABI allowlist",
    )
    require_command_line_failure(
        validator_arguments(
            dso_only=True,
            sql_manifest=Path("/self-test/active-manifest"),
            dso=Path("/self-test/active-dso"),
            runtime_matrix=Path("/self-test/runtime-matrix"),
        ),
        "not valid for dso-only",
        "dso-only accepted a runtime argument",
    )

    require_command_line_failure(
        validator_arguments(
            STATIC_VALIDATOR_ARGUMENTS,
            RUNTIME_VALIDATOR_ARGUMENTS,
            sql_manifest=Path("/self-test/active-manifest"),
        ),
        "missing --dso or --symbol-dump",
        "full validation accepted no dynamic symbol source",
    )
    require_command_line_failure(
        validator_arguments(
            STATIC_VALIDATOR_ARGUMENTS,
            RUNTIME_VALIDATOR_ARGUMENTS,
            sql_manifest=Path("/self-test/active-manifest"),
            dso=Path("/self-test/active-dso"),
            dso_contract="manifest",
        ),
        "--dso-contract is valid only with --dso-only",
        "full Bioseq evidence accepted the manifest DSO contract",
    )
    require_command_line_failure(
        validator_arguments(
            self_test=True,
            production_ledger=Path("/self-test/ledger"),
        ),
        "not valid for self-test",
        "self-test accepted artifact arguments",
    )

    main_symbols = ["array_cat", "seq_concat"]
    identities = [
        "public.array_cat(internal)",
        "public.seq_concat(internal)",
    ]
    manifest = {
        "schema_revision": 1,
        "c_routine_identity_count": 2,
        "c_link_symbol_inventory_count": 2,
        "c_finfo_symbol_inventory_count": 2,
        "c_wrapper_entity_count": 2,
        "generated_wrapper_symbol_count": 2,
        "handwritten_fmgr_export_symbol_count": 0,
        "handwritten_fmgr_export_external_symbol_count": len(
            EXTERNAL_SYMBOLS
        ),
        "handwritten_fmgr_export_owner_count": len(EXTERNAL_SYMBOLS),
        "c_symbol_sharing_group_count": 0,
        "c_routine_identities": identities,
        "c_link_symbols": main_symbols,
        "c_finfo_symbols": [
            "pg_finfo_array_cat",
            "pg_finfo_seq_concat",
        ],
        "c_symbol_owners": [
            {
                "symbol": symbol,
                "finfo_symbol": f"pg_finfo_{symbol}",
                "generated_wrapper": True,
                "routine_identities": [identity],
                "implementation_declarations": [f"fixture::{symbol}"],
            }
            for symbol, identity in zip(main_symbols, identities, strict=True)
        ],
        "generated_wrapper_symbols": main_symbols,
        "handwritten_fmgr_export_symbols": [],
        "handwritten_fmgr_export_external_symbols": sorted(
            EXTERNAL_SYMBOLS
        ),
        "handwritten_fmgr_export_owners": [
            {
                "symbol": symbol,
                "macro": "CXX_PG_FUNCTION_INFO_V1",
                "source_module": "fixture.external",
                "source_path": "fixture/external.cppm",
                "source_line": index + 1,
            }
            for index, symbol in enumerate(sorted(EXTERNAL_SYMBOLS))
        ],
    }

    fixture_policy = LedgerPolicy(
        expected_parent_count=2,
        expected_symbol_count=2,
        shared_symbols=frozenset(),
    )

    def callback_span(
        begin_offset: int,
        end_offset: int,
        line: int,
    ) -> dict[str, Any]:
        return {
            "begin": {
                "byte_offset": begin_offset,
                "line": line,
                "column": 1,
            },
            "end": {
                "byte_offset": end_offset,
                "line": line,
                "column": end_offset - begin_offset + 1,
            },
        }

    def callback_located_span(
        source_file: str,
        begin_offset: int,
        end_offset: int,
        line: int,
    ) -> dict[str, Any]:
        return {
            "source_file": source_file,
            "span": callback_span(begin_offset, end_offset, line),
        }

    def callback_lifetime_digest(object_ids: list[str]) -> str:
        material = "".join(
            f"{len(object_id.encode('utf-8'))}:{object_id}\n"
            for object_id in sorted(object_ids)
        )
        return hashlib.sha256(material.encode("utf-8")).hexdigest()

    def fixture_compiler_facts(profile: str) -> dict[str, Any]:
        source_file = "fixture/callback.cppm"
        root_usr = "c:@N@fixture@F@callback_root#"
        root_name = "fixture::callback_root"
        child_usr = "c:@N@fixture@F@callback_child#"
        child_name = "fixture::callback_child"
        object_id = "fixture-object"
        root_declaration = callback_span(100, 130, 10)
        child_declaration = callback_span(400, 430, 40)
        registration = callback_located_span(
            source_file,
            500,
            525,
            50,
        )
        stable_material = "\n".join(
            (
                "table_am",
                "relation_vacuum",
                "c:@S@TableAmRoutine",
                root_usr,
                source_file,
                "500",
                "525",
            )
        )
        stable_callback_id = (
            "pg-callback:"
            + hashlib.sha256(
                stable_material.encode("utf-8")
            ).hexdigest()[:32]
        )
        child_function = {
            "ast_callsite_count": 0,
            "body_available": True,
            "body_sha256": hashlib.sha256(
                b"fixture-callback-child-body"
            ).hexdigest(),
            "body_span": callback_span(400, 480, 40),
            "calls": [],
            "cfg_block_count": 1,
            "cfg_complete": True,
            "cfg_element_count": 1,
            "cfg_implicit_callsite_count": 0,
            "cfg_sha256": hashlib.sha256(
                b"fixture-callback-child-cfg"
            ).hexdigest(),
            "control_flow_assembly_sites": [],
            "declaration_span": child_declaration,
            "emitted_callsite_count": 0,
            "implicit_call_edges_complete": True,
            "lifetime_analysis_complete": True,
            "lifetime_inventory_sha256": (
                callback_lifetime_digest([])
            ),
            "lifetime_object_ids": [],
            "macro_expansion_complete": True,
            "module_name": "fixture.callback",
            "qualified_name": child_name,
            "source_file": source_file,
            "stable_usr": child_usr,
            "template_instantiation_complete": True,
            "unavailable_bodies": [],
            "unresolved_indirect_targets": [],
        }
        root_call = {
            "analyzer_entered_bridge_callable": False,
            "bridge_primitive_evidence": None,
            "bridge_transition": "none",
            "callable_capture_objects": [],
            "caller_usr": root_usr,
            "constant_integer_arguments": [],
            "expansion": callback_located_span(
                source_file,
                220,
                240,
                22,
            ),
            "generated_boundary_tail": False,
            "live_objects": [
                {
                    "canonical_type": "int",
                    "declaration": callback_located_span(
                        source_file,
                        160,
                        170,
                        16,
                    ),
                    "declaration_usr": "c:callback_fixture_object",
                    "identity_kind": "declaration_usr",
                    "origin": "automatic_object",
                    "stable_object_id": object_id,
                    "trivially_destructible": True,
                }
            ],
            "macro_chain": [],
            "resolution": "direct",
            "spelling": callback_located_span(
                source_file,
                220,
                240,
                22,
            ),
            "stable_site_id": "clang:fixture:callback-child-call",
            "target_set_complete": True,
            "targets": [
                {
                    "domain": "project",
                    "external_mode": "not_external",
                    "external_provenance": None,
                    "pg_no_longjmp_allowlist_evidence": None,
                    "target_qualified_name": child_name,
                    "target_usr": child_usr,
                }
            ],
        }
        root_function = {
            "ast_callsite_count": 1,
            "body_available": True,
            "body_sha256": hashlib.sha256(
                b"fixture-callback-root-body"
            ).hexdigest(),
            "body_span": callback_span(100, 360, 10),
            "calls": [root_call],
            "cfg_block_count": 2,
            "cfg_complete": True,
            "cfg_element_count": 2,
            "cfg_implicit_callsite_count": 0,
            "cfg_sha256": hashlib.sha256(
                b"fixture-callback-root-cfg"
            ).hexdigest(),
            "control_flow_assembly_sites": [],
            "declaration_span": root_declaration,
            "emitted_callsite_count": 1,
            "implicit_call_edges_complete": True,
            "lifetime_analysis_complete": True,
            "lifetime_inventory_sha256": (
                callback_lifetime_digest([object_id])
            ),
            "lifetime_object_ids": [object_id],
            "macro_expansion_complete": True,
            "module_name": "fixture.callback",
            "qualified_name": root_name,
            "source_file": source_file,
            "stable_usr": root_usr,
            "template_instantiation_complete": True,
            "unavailable_bodies": [],
            "unresolved_indirect_targets": [],
        }
        return {
            "callback_roots": [
                {
                    "family": "table_am",
                    "owner_qualified_name": "TableAmRoutine",
                    "owner_usr": "c:@S@TableAmRoutine",
                    "registration": registration,
                    "slot": "relation_vacuum",
                    "stable_callback_id": stable_callback_id,
                    "target_definition": {
                        "source_file": source_file,
                        "span": root_declaration,
                    },
                    "target_qualified_name": root_name,
                    "target_usr": root_usr,
                }
            ],
            "clang_version": "self-test-clang-v1",
            "compile_database_sha256": hashlib.sha256(
                f"compile-database:{profile}".encode()
            ).hexdigest(),
            "compile_graph_sha256": hashlib.sha256(
                f"compile-graph:{profile}".encode()
            ).hexdigest(),
            "compiler_profile": profile,
            "functions": [root_function, child_function],
            "pcm_manifest_sha256": hashlib.sha256(
                f"pcm-manifest:{profile}".encode()
            ).hexdigest(),
            "producer_executable_sha256": hashlib.sha256(
                b"producer-executable"
            ).hexdigest(),
            "profile": profile,
            "root_usrs": [root_usr],
            "runtime_probes": [],
            "schema_revision": 2,
            "source_list_sha256": hashlib.sha256(
                f"source-list:{profile}".encode()
            ).hexdigest(),
            "sql_manifest_sha256": hashlib.sha256(
                b"sql-manifest"
            ).hexdigest(),
        }

    compiler_facts_fixtures = {
        profile: fixture_compiler_facts(profile)
        for profile in ("production", "test_indexes")
    }
    compiler_facts_raw_fixtures = {
        profile: _canonical_json_bytes(facts)
        for profile, facts in compiler_facts_fixtures.items()
    }
    callback_bindings = {
        profile: _compiler_callback_closure_binding(
            facts,
            f"self-test.{profile}.compiler_facts",
        )
        for profile, facts in compiler_facts_fixtures.items()
    }
    expected_callback_binding = (
        1,
        "75a814756404fd3c50cc68ae6d40380c"
        "8bee213f4f64f103ff3f507d743666ae",
    )
    if any(
        binding != expected_callback_binding
        for binding in callback_bindings.values()
    ):
        _fail(
            "self-test",
            "callback canonicalization differs from the independently "
            "calculated C++ analyzer fixture digest",
        )

    deep_callback_facts = json.loads(
        json.dumps(compiler_facts_fixtures["production"])
    )
    deep_source_file = "fixture/callback.cppm"
    deep_function_count = MAX_CALLBACK_CLOSURE_DEPTH + 1
    deep_functions: list[dict[str, Any]] = []

    def deep_callback_call(
        caller_usr: str,
        target_usr: str,
        target_name: str,
        index: int,
    ) -> dict[str, Any]:
        begin_offset = 20_000 + (index + 1) * 20
        line = 2_000 + index + 1
        return {
            "analyzer_entered_bridge_callable": False,
            "bridge_primitive_evidence": None,
            "bridge_transition": "none",
            "callable_capture_objects": [],
            "caller_usr": caller_usr,
            "constant_integer_arguments": [],
            "expansion": callback_located_span(
                deep_source_file,
                begin_offset,
                begin_offset + 5,
                line,
            ),
            "generated_boundary_tail": False,
            "live_objects": [],
            "macro_chain": [],
            "resolution": "direct",
            "spelling": callback_located_span(
                deep_source_file,
                begin_offset,
                begin_offset + 5,
                line,
            ),
            "stable_site_id": f"clang:fixture:deep-call:{index}",
            "target_set_complete": True,
            "targets": [
                {
                    "domain": "project",
                    "external_mode": "not_external",
                    "external_provenance": None,
                    "pg_no_longjmp_allowlist_evidence": None,
                    "target_qualified_name": target_name,
                    "target_usr": target_usr,
                }
            ],
        }

    for index in range(deep_function_count):
        function_usr = f"c:@N@fixture@F@deep_callback_{index}#"
        function_name = f"fixture::deep_callback_{index}"
        begin_offset = 30_000 + index * 30
        line = 3_000 + index
        has_next = index + 1 < deep_function_count
        next_usr = f"c:@N@fixture@F@deep_callback_{index + 1}#"
        next_name = f"fixture::deep_callback_{index + 1}"
        calls = (
            [
                deep_callback_call(
                    function_usr,
                    next_usr,
                    next_name,
                    index,
                )
            ]
            if has_next
            else []
        )
        deep_functions.append(
            {
                "ast_callsite_count": len(calls),
                "body_available": True,
                "body_sha256": hashlib.sha256(
                    f"deep-body:{index}".encode()
                ).hexdigest(),
                "body_span": callback_span(
                    begin_offset,
                    begin_offset + 20,
                    line,
                ),
                "calls": calls,
                "cfg_block_count": 1,
                "cfg_complete": True,
                "cfg_element_count": max(1, len(calls)),
                "cfg_implicit_callsite_count": 0,
                "cfg_sha256": hashlib.sha256(
                    f"deep-cfg:{index}".encode()
                ).hexdigest(),
                "control_flow_assembly_sites": [],
                "declaration_span": callback_span(
                    begin_offset,
                    begin_offset + 10,
                    line,
                ),
                "emitted_callsite_count": len(calls),
                "implicit_call_edges_complete": True,
                "lifetime_analysis_complete": True,
                "lifetime_inventory_sha256": (
                    callback_lifetime_digest([])
                ),
                "lifetime_object_ids": [],
                "macro_expansion_complete": True,
                "module_name": "fixture.callback",
                "qualified_name": function_name,
                "source_file": deep_source_file,
                "stable_usr": function_usr,
                "template_instantiation_complete": True,
                "unavailable_bodies": [],
                "unresolved_indirect_targets": [],
            }
        )
    deep_child = next(
        function
        for function in deep_callback_facts["functions"]
        if function["qualified_name"] == "fixture::callback_child"
    )
    deep_child["calls"] = [
        deep_callback_call(
            deep_child["stable_usr"],
            deep_functions[0]["stable_usr"],
            deep_functions[0]["qualified_name"],
            -1,
        )
    ]
    deep_child["ast_callsite_count"] = 1
    deep_child["emitted_callsite_count"] = 1
    deep_child["cfg_block_count"] = 2
    deep_child["cfg_element_count"] = 2
    deep_callback_facts["functions"].extend(deep_functions)
    try:
        _compiler_callback_closure_binding(
            deep_callback_facts,
            "self-test.deep_callback_facts",
        )
    except EvidenceError as error:
        if "exceeds the reviewed traversal depth" not in str(error):
            raise
    else:
        _fail(
            "self-test",
            "callback graph beyond the reviewed traversal depth "
            "was accepted",
        )

    def fixture_ledger(profile: str) -> dict[str, Any]:
        parents = []
        groups = []
        for index, (symbol, identity) in enumerate(
            zip(main_symbols, identities, strict=True)
        ):
            source_offset = index * 10
            parents.append(
                {
                    "sql_identity": identity,
                    "owner_usr": f"c:@N@fixture@F@{symbol}#",
                    "qualified_declaration": f"fixture::{symbol}",
                    "owner_source_file": "fixture/owners.cppm",
                    "owner_source_span": {
                        "begin": {
                            "byte_offset": source_offset,
                            "line": index + 1,
                            "column": 1,
                        },
                        "end": {
                            "byte_offset": source_offset + 5,
                            "line": index + 1,
                            "column": 6,
                        },
                    },
                    "generated_wrapper": f"fixture::generated::{symbol}",
                    "public_symbol": symbol,
                    "profile": profile,
                    "transitive_closure_sha256": hashlib.sha256(
                        f"{profile}:{identity}".encode()
                    ).hexdigest(),
                    "recoverable_site_count": 0,
                    "terminal_site_count": 0,
                    "audited_stop_count": 0,
                    "analysis": {
                        "template_instantiation_complete": True,
                        "macro_expansion_complete": True,
                        "cfg_complete": True,
                        "lifetime_analysis_complete": True,
                        "call_graph_fixed_point_complete": True,
                        "unresolved_indirect_targets": [],
                        "unavailable_bodies": [],
                        "control_flow_assembly_sites": [],
                    },
                    "sites": [],
                    "audited_stops": [],
                }
            )
            groups.append(
                {
                    "public_symbol": symbol,
                    "finfo_symbol": f"pg_finfo_{symbol}",
                    "generated_wrapper": f"fixture::generated::{symbol}",
                    "sql_identities": [identity],
                }
            )
        return {
            "schema_revision": 2,
            "profile": profile,
            "postgresql_major": 17,
            "postgresql_revision": PG17_REVISION,
            "provenance_bundle_sha256": _artifact_sha256(provenance_raw),
            "source_list_sha256": hashlib.sha256(
                f"source-list:{profile}".encode()
            ).hexdigest(),
            "compile_database_sha256": hashlib.sha256(
                f"compile-database:{profile}".encode()
            ).hexdigest(),
            "producer_executable_sha256": hashlib.sha256(
                b"producer-executable"
            ).hexdigest(),
            "compile_graph_sha256": hashlib.sha256(
                f"compile-graph:{profile}".encode()
            ).hexdigest(),
            "pcm_manifest_sha256": hashlib.sha256(
                f"pcm-manifest:{profile}".encode()
            ).hexdigest(),
            "sql_manifest_sha256": hashlib.sha256(
                b"sql-manifest"
            ).hexdigest(),
            "compiler_facts_sha256": _artifact_sha256(
                compiler_facts_raw_fixtures[profile]
            ),
            "clang_version": "self-test-clang-v1",
            "analyzer_version": "self-test-analyzer-v2",
            "generator_revision": "self-test-generator-v2",
            "callback_root_count": callback_bindings[profile][0],
            "callback_closure_sha256": callback_bindings[profile][1],
            "parents": parents,
            "symbol_groups": groups,
        }

    production_ledger_fixture = fixture_ledger("production")
    test_indexes_ledger_fixture = fixture_ledger("test_indexes")
    with tempfile.TemporaryDirectory(
        prefix="bioseq-safety-self-test-",
    ) as temporary_directory:
        artifact_root = Path(temporary_directory).resolve()
        for profile, ledger_fixture in (
            ("production", production_ledger_fixture),
            ("test_indexes", test_indexes_ledger_fixture),
        ):
            source_list = artifact_root / f"{profile}.sources"
            compile_database = (
                artifact_root / f"{profile}.compile-database"
            )
            producer = artifact_root / "producer"
            compile_graph = (
                artifact_root / f"{profile}.compile-graph"
            )
            pcm_manifest = (
                artifact_root / f"{profile}.pcm-manifest"
            )
            compiler_facts = (
                artifact_root / f"{profile}.compiler-facts"
            )
            source_list.write_bytes(
                f"source-list:{profile}".encode()
            )
            compile_database.write_bytes(
                f"compile-database:{profile}".encode()
            )
            producer.write_bytes(b"producer-executable")
            compile_graph.write_bytes(
                f"compile-graph:{profile}".encode()
            )
            pcm_manifest.write_bytes(
                f"pcm-manifest:{profile}".encode()
            )
            compiler_facts.write_bytes(
                compiler_facts_raw_fixtures[profile]
            )
            validate_current_ledger_inputs(
                ledger_fixture,
                profile,
                source_list=source_list,
                compile_database=compile_database,
                producer_executable=producer,
                compile_graph=compile_graph,
                pcm_manifest=pcm_manifest,
                compiler_facts=compiler_facts,
            )
            if profile == "production":
                mutated_facts = json.loads(
                    json.dumps(compiler_facts_fixtures[profile])
                )
                child_function = next(
                    function
                    for function in mutated_facts["functions"]
                    if function["qualified_name"]
                    == "fixture::callback_child"
                )
                child_function["cfg_sha256"] = hashlib.sha256(
                    b"mutated-callback-child-cfg"
                ).hexdigest()
                mutated_facts_raw = _canonical_json_bytes(
                    mutated_facts
                )
                compiler_facts.write_bytes(mutated_facts_raw)
                mutated_ledger = json.loads(
                    json.dumps(ledger_fixture)
                )
                mutated_ledger["compiler_facts_sha256"] = (
                    _artifact_sha256(mutated_facts_raw)
                )
                try:
                    validate_current_ledger_inputs(
                        mutated_ledger,
                        profile,
                        source_list=source_list,
                        compile_database=compile_database,
                        producer_executable=producer,
                        compile_graph=compile_graph,
                        pcm_manifest=pcm_manifest,
                        compiler_facts=compiler_facts,
                    )
                except EvidenceError as error:
                    if "closure digest differs" not in str(error):
                        raise
                else:
                    _fail(
                        "self-test",
                        "mutated callback closure was accepted after "
                        "refreshing the compiler-facts artifact hash",
                    )

                stale_root_facts = json.loads(
                    json.dumps(compiler_facts_fixtures[profile])
                )
                stale_root_facts["callback_roots"][0]["slot"] = (
                    "scan_bitmap_next_block"
                )
                stale_root_raw = _canonical_json_bytes(
                    stale_root_facts
                )
                compiler_facts.write_bytes(stale_root_raw)
                stale_root_ledger = json.loads(
                    json.dumps(ledger_fixture)
                )
                stale_root_ledger["compiler_facts_sha256"] = (
                    _artifact_sha256(stale_root_raw)
                )
                try:
                    validate_current_ledger_inputs(
                        stale_root_ledger,
                        profile,
                        source_list=source_list,
                        compile_database=compile_database,
                        producer_executable=producer,
                        compile_graph=compile_graph,
                        pcm_manifest=pcm_manifest,
                        compiler_facts=compiler_facts,
                    )
                except EvidenceError as error:
                    if "stable identity is stale" not in str(error):
                        raise
                else:
                    _fail(
                        "self-test",
                        "stale callback-root identity was accepted",
                    )

                compiler_facts.write_bytes(
                    compiler_facts_raw_fixtures[profile]
                )
                count_mutation = json.loads(
                    json.dumps(ledger_fixture)
                )
                count_mutation["callback_root_count"] += 1
                try:
                    validate_compiler_callback_closure(
                        count_mutation,
                        compiler_facts_fixtures[profile],
                        profile,
                    )
                except EvidenceError as error:
                    if "root count or closure digest differs" not in str(
                        error
                    ):
                        raise
                else:
                    _fail(
                        "self-test",
                        "callback-root count mutation was accepted",
                    )

                compiler_facts.write_bytes(
                    b"stale-compiler-facts"
                )
                try:
                    validate_current_ledger_inputs(
                        ledger_fixture,
                        profile,
                        source_list=source_list,
                        compile_database=compile_database,
                        producer_executable=producer,
                        compile_graph=compile_graph,
                        pcm_manifest=pcm_manifest,
                        compiler_facts=compiler_facts,
                    )
                except EvidenceError as error:
                    if "current artifact bytes" not in str(error):
                        raise
                else:
                    _fail(
                        "self-test",
                        "stale current compiler facts were accepted",
                    )

    production_fixture = validate_ledger(
        production_ledger_fixture,
        validate_provenance(provenance),
        _artifact_sha256(provenance_raw),
        fixture_policy,
    )
    test_indexes_fixture = validate_ledger(
        test_indexes_ledger_fixture,
        validate_provenance(provenance),
        _artifact_sha256(provenance_raw),
        fixture_policy,
    )
    validate_profile_pair(production_fixture, test_indexes_fixture)

    if _expected_runtime_lifecycle(
        BACKEND_SCOPED_VACUUM_CLEANUP_FAULT_KEY
    ) != {
        "fault_invocation_count": 0,
        "fmgr_start_count": 0,
        "fmgr_end_count": 0,
        "fmgr_abort_count": 0,
    }:
        _fail(
            "self-test",
            "backend-scoped vacuum cleanup lifecycle changed",
        )
    for near_miss_fault_key in (
        BACKEND_SCOPED_VACUUM_CLEANUP_FAULT_KEY + ".extra",
        "invoke_pg_as_cpp.vacuum_cleanup_restored",
        "invoke_pg_as_cpp.cleanup_restore",
    ):
        if _expected_runtime_lifecycle(near_miss_fault_key) != {
            "fault_invocation_count": 1,
            "fmgr_start_count": 1,
            "fmgr_end_count": 0,
            "fmgr_abort_count": 1,
        }:
            _fail(
                "self-test",
                "near-miss backend fault key received the FMGR exemption",
            )

    _run_runtime_contract_self_tests(
        identities[0],
        runtime_policy_raw,
        GENERATED_BOUNDARY_FAULT_KEY,
    )
    _run_runtime_contract_self_tests(
        identities[0],
        runtime_policy_raw,
        BACKEND_SCOPED_VACUUM_CLEANUP_FAULT_KEY,
    )

    validate_ledger_manifest_closure(
        manifest,
        production_fixture,
        test_indexes_fixture,
        policy=fixture_policy,
    )

    incomplete_manifest = json.loads(json.dumps(manifest))
    incomplete_manifest["c_symbol_owners"][0]["generated_wrapper"] = False
    incomplete_manifest["generated_wrapper_symbols"].remove(main_symbols[0])
    incomplete_manifest["generated_wrapper_symbol_count"] = 1
    incomplete_manifest["c_wrapper_entity_count"] = 1
    incomplete_manifest["handwritten_fmgr_export_symbols"] = [
        main_symbols[0]
    ]
    incomplete_manifest["handwritten_fmgr_export_symbol_count"] = 1
    try:
        validate_ledger_manifest_closure(
            incomplete_manifest,
            production_fixture,
            test_indexes_fixture,
            policy=fixture_policy,
        )
    except EvidenceError as error:
        if "non-generated fmgr owner" not in str(error):
            raise
    else:
        _fail("self-test", "incomplete generated ownership was accepted")

    missing_parent_manifest = json.loads(json.dumps(manifest))
    missing_parent_manifest["c_routine_identities"].pop()
    missing_parent_manifest["c_routine_identity_count"] = 1
    try:
        validate_ledger_manifest_closure(
            missing_parent_manifest,
            production_fixture,
            test_indexes_fixture,
            policy=fixture_policy,
        )
    except EvidenceError as error:
        if "exact parent inventory" not in str(error):
            raise
    else:
        _fail("self-test", "manifest parent omission was accepted")

    mismatched_production_fixture = json.loads(
        json.dumps(
            {
                "profile": production_fixture["profile"],
                "parents": production_fixture["parents"],
                "symbols": sorted(production_fixture["symbols"]),
                "shared_symbols": sorted(
                    production_fixture["shared_symbols"]
                ),
            }
        )
    )
    mismatched_production_fixture["symbols"] = set(
        mismatched_production_fixture["symbols"]
    )
    mismatched_production_fixture["shared_symbols"] = set(
        mismatched_production_fixture["shared_symbols"]
    )
    mismatched_production_fixture["parents"][identities[0]][
        "public_symbol"
    ] = main_symbols[1]
    try:
        validate_ledger_manifest_closure(
            manifest,
            mismatched_production_fixture,
            test_indexes_fixture,
            policy=fixture_policy,
        )
    except EvidenceError as error:
        if "ledger parents differ" not in str(error):
            raise
    else:
        _fail("self-test", "ledger/manifest identity mismatch was accepted")

    exported = main_symbols + sorted(EXTERNAL_SYMBOLS)
    nm_text = "\n".join(
        f"{name} T {index + 1:x} 1"
        for index, name in enumerate(
            [
                item
                for symbol in exported
                for item in (symbol, f"pg_finfo_{symbol}")
            ]
        )
    )
    inventory = parse_nm_symbols(nm_text, source_tool="self-test-nm")
    readelf_fixture = "\n".join(
        [
            "Symbol table '.dynsym' contains 5 entries:",
            "   Num: Value Size Type Bind Vis Ndx Name",
            "     1: 0000000000001000 16 FUNC GLOBAL DEFAULT 12 seq_concat",
            "     2: 0000000000001010 16 FUNC GLOBAL DEFAULT 12 pg_finfo_seq_concat",
            "     3: 0000000000001020 16 FUNC GLOBAL DEFAULT 12 Pg_magic_func",
            "     4: 0000000000001030 16 FUNC GLOBAL DEFAULT 12 _PG_init",
        ]
    )
    if [symbol.name for symbol in parse_readelf_symbols(readelf_fixture)] != [
        "seq_concat",
        "pg_finfo_seq_concat",
        "Pg_magic_func",
        "_PG_init",
    ]:
        _fail("self-test", "readelf parser changed symbol identity")
    darwin_fixture = (
        "_seq_concat\n"
        "_pg_finfo_seq_concat\n"
        "_Pg_magic_func\n"
        "__PG_init\n"
    )
    if [
        symbol.name
        for symbol in parse_darwin_nm_symbols(darwin_fixture)
    ] != [
        "seq_concat",
        "pg_finfo_seq_concat",
        "Pg_magic_func",
        "_PG_init",
    ]:
        _fail("self-test", "Darwin nm parser changed symbol identity")
    coff_fixture = "\n".join(
        [
            "Export {",
            "  Ordinal: 1",
            "  Name: seq_concat",
            "  RVA: 0x1000",
            "}",
            "Export {",
            "  Ordinal: 2",
            "  Name: pg_finfo_seq_concat",
            "  RVA: 0x1010",
            "}",
            "Export {",
            "  Ordinal: 3",
            "  Name: Pg_magic_func",
            "  RVA: 0x1020",
            "}",
            "Export {",
            "  Ordinal: 4",
            "  Name: _PG_init",
            "  RVA: 0x1030",
            "}",
        ]
    )
    if [
        symbol.name
        for symbol in parse_llvm_coff_exports(coff_fixture)
    ] != [
        "seq_concat",
        "pg_finfo_seq_concat",
        "Pg_magic_func",
        "_PG_init",
    ]:
        _fail("self-test", "COFF export parser changed symbol identity")
    dumpbin_fixture = "\n".join(
        [
            "    ordinal hint RVA      name",
            "          1    0 00001000 seq_concat",
            "          2    1 00001010 pg_finfo_seq_concat",
            "          3    2 00001020 Pg_magic_func",
            "          4    3 00001030 _PG_init",
        ]
    )
    if [
        symbol.name
        for symbol in parse_dumpbin_exports(dumpbin_fixture)
    ] != [
        "seq_concat",
        "pg_finfo_seq_concat",
        "Pg_magic_func",
        "_PG_init",
    ]:
        _fail("self-test", "dumpbin parser changed symbol identity")
    validate_dso_ownership(
        manifest,
        inventory,
        main_identity_count=2,
        main_symbol_count=2,
        shared_symbols=frozenset(),
        required_absent_symbols=frozenset(),
        required_present_symbols=frozenset(),
    )
    legacy_revision_manifest = json.loads(json.dumps(manifest))
    legacy_revision_manifest["manifest_version"] = 1
    try:
        validate_dso_ownership(
            legacy_revision_manifest,
            inventory,
            main_identity_count=2,
            main_symbol_count=2,
            shared_symbols=frozenset(),
            required_absent_symbols=frozenset(),
            required_present_symbols=frozenset(),
        )
    except EvidenceError as error:
        if "unknown fields: manifest_version" not in str(error):
            raise
    else:
        _fail(
            "self-test",
            "legacy manifest_version was accepted",
        )

    cache_symbol = "cache_lookup"
    cache_manifest = {
        "schema_revision": 1,
        "c_routine_identity_count": 1,
        "c_link_symbol_inventory_count": 1,
        "c_finfo_symbol_inventory_count": 1,
        "c_wrapper_entity_count": 1,
        "generated_wrapper_symbol_count": 1,
        "handwritten_fmgr_export_symbol_count": 0,
        "handwritten_fmgr_export_external_symbol_count": 0,
        "handwritten_fmgr_export_owner_count": 0,
        "c_symbol_sharing_group_count": 0,
        "c_routine_identities": [
            "bioseq_cache.cache_lookup(text)",
        ],
        "c_link_symbols": [cache_symbol],
        "c_finfo_symbols": [f"pg_finfo_{cache_symbol}"],
        "c_symbol_owners": [
            {
                "symbol": cache_symbol,
                "finfo_symbol": f"pg_finfo_{cache_symbol}",
                "generated_wrapper": True,
                "routine_identities": [
                    "bioseq_cache.cache_lookup(text)",
                ],
                "implementation_declarations": [
                    "fixture::cache_lookup",
                ],
            }
        ],
        "generated_wrapper_symbols": [cache_symbol],
        "handwritten_fmgr_export_symbols": [],
        "handwritten_fmgr_export_external_symbols": [],
        "handwritten_fmgr_export_owners": [],
    }
    cache_abi_exports = (
        "Pg_magic_func",
        "_PG_init",
        "bioseq_cache_storage_janitor_main",
    )
    cache_inventory = parse_nm_symbols(
        "\n".join(
            (
                f"{cache_symbol} T 1 1",
                f"pg_finfo_{cache_symbol} T 2 1",
                "Pg_magic_func T 3 1",
                "_PG_init T 4 1",
                "bioseq_cache_storage_janitor_main T 5 1",
            )
        ),
        source_tool="self-test-cache-nm",
    )
    validate_dso_ownership(
        cache_manifest,
        cache_inventory,
        contract="manifest",
        allowed_dso_exports=cache_abi_exports,
    )
    try:
        validate_dso_ownership(cache_manifest, cache_inventory)
    except EvidenceError as error:
        if "main C routine identity total is not exact" not in str(error):
            raise
    else:
        _fail(
            "self-test",
            "Bioseq DSO defaults accepted a cache manifest",
        )
    stale_cache_manifest = json.loads(json.dumps(cache_manifest))
    stale_cache_manifest["c_link_symbol_inventory_count"] = 2
    try:
        validate_dso_ownership(
            stale_cache_manifest,
            cache_inventory,
            contract="manifest",
        )
    except EvidenceError as error:
        if "main public fmgr symbol total is not exact" not in str(error):
            raise
    else:
        _fail(
            "self-test",
            "manifest DSO contract accepted a stale symbol count",
        )
    cache_inventory_with_unowned_finfo = parse_nm_symbols(
        "\n".join(
            (
                f"{cache_symbol} T 1 1",
                f"pg_finfo_{cache_symbol} T 2 1",
                "Pg_magic_func T 3 1",
                "_PG_init T 4 1",
                "bioseq_cache_storage_janitor_main T 5 1",
                "unowned T 6 1",
                "pg_finfo_unowned T 7 1",
            )
        ),
        source_tool="self-test-cache-extra-nm",
    )
    try:
        validate_dso_ownership(
            cache_manifest,
            cache_inventory_with_unowned_finfo,
            contract="manifest",
            allowed_dso_exports=cache_abi_exports,
        )
    except EvidenceError as error:
        if "manifest-owned symbols" not in str(error):
            raise
    else:
        _fail(
            "self-test",
            "manifest DSO contract accepted an unowned fmgr export",
        )
    cache_inventory_with_cpp_leak = parse_nm_symbols(
        "\n".join(
            (
                f"{cache_symbol} T 1 1",
                f"pg_finfo_{cache_symbol} T 2 1",
                "Pg_magic_func T 3 1",
                "_PG_init T 4 1",
                "bioseq_cache_storage_janitor_main T 5 1",
                "_ZN6bioseq5cache19accidental_exportEv T 6 1",
            )
        ),
        source_tool="self-test-cache-leak-nm",
    )
    try:
        validate_dso_ownership(
            cache_manifest,
            cache_inventory_with_cpp_leak,
            contract="manifest",
            allowed_dso_exports=cache_abi_exports,
        )
    except EvidenceError as error:
        if "defined dynamic export surface is not exact" not in str(error):
            raise
    else:
        _fail(
            "self-test",
            "manifest DSO contract accepted a leaked non-fmgr symbol",
        )

    incremental_manifest = json.loads(json.dumps(manifest))
    incremental_manifest["c_wrapper_entity_count"] = 1
    incremental_manifest["generated_wrapper_symbol_count"] = 1
    incremental_manifest["generated_wrapper_symbols"] = ["array_cat"]
    incremental_manifest["handwritten_fmgr_export_symbol_count"] = 1
    incremental_manifest["handwritten_fmgr_export_symbols"] = [
        "seq_concat"
    ]
    incremental_manifest["handwritten_fmgr_export_owner_count"] = (
        len(EXTERNAL_SYMBOLS) + 1
    )
    incremental_manifest["handwritten_fmgr_export_owners"].append(
        {
            "symbol": "seq_concat",
            "macro": "CXX_PG_FUNCTION_INFO_V1",
            "source_module": "fixture.main",
            "source_path": "fixture/main.cppm",
            "source_line": 1,
        }
    )
    incremental_manifest["c_symbol_owners"][1]["generated_wrapper"] = False
    validate_dso_ownership(
        incremental_manifest,
        inventory,
        main_identity_count=2,
        main_symbol_count=2,
        shared_symbols=frozenset(),
        required_absent_symbols=frozenset(),
        required_present_symbols=frozenset(),
        require_complete_codegen=False,
    )

    try:
        validate_dso_ownership(
            manifest,
            inventory[:-1],
            main_identity_count=2,
            main_symbol_count=2,
            shared_symbols=frozenset(),
            required_absent_symbols=frozenset(),
            required_present_symbols=frozenset(),
        )
    except EvidenceError as error:
        if "main symbols plus the bioseq_external handler" not in str(error):
            raise
    else:
        _fail("self-test", "missing external finfo symbol was accepted")

    bad_manifest = json.loads(json.dumps(incremental_manifest))
    bad_manifest["c_wrapper_entity_count"] = 2
    bad_manifest["generated_wrapper_symbol_count"] = 2
    bad_manifest["generated_wrapper_symbols"].append("seq_concat")
    bad_manifest["c_symbol_owners"][1]["generated_wrapper"] = True
    try:
        validate_dso_ownership(
            bad_manifest,
            inventory,
            main_identity_count=2,
            main_symbol_count=2,
            shared_symbols=frozenset(),
            required_absent_symbols=frozenset(),
            required_present_symbols=frozenset(),
            require_complete_codegen=False,
        )
    except EvidenceError as error:
        if "generated and handwritten FMGR ownership overlap" not in str(
            error
        ):
            raise
    else:
        _fail("self-test", "overlapping fmgr ownership was accepted")

    bad_manifest = json.loads(json.dumps(incremental_manifest))
    bad_manifest["handwritten_fmgr_export_symbol_count"] = 0
    bad_manifest["handwritten_fmgr_export_symbols"] = []
    bad_manifest["handwritten_fmgr_export_owner_count"] = len(
        EXTERNAL_SYMBOLS
    )
    bad_manifest["handwritten_fmgr_export_owners"] = [
        owner
        for owner in bad_manifest["handwritten_fmgr_export_owners"]
        if owner["symbol"] != "seq_concat"
    ]
    try:
        validate_dso_ownership(
            bad_manifest,
            inventory,
            main_identity_count=2,
            main_symbol_count=2,
            shared_symbols=frozenset(),
            required_absent_symbols=frozenset(),
            required_present_symbols=frozenset(),
            require_complete_codegen=False,
        )
    except EvidenceError as error:
        if "does not exactly cover" not in str(error):
            raise
    else:
        _fail("self-test", "incomplete fmgr ownership was accepted")

    try:
        validate_dso_ownership(
            incremental_manifest,
            inventory,
            main_identity_count=2,
            main_symbol_count=2,
            shared_symbols=frozenset(),
            required_absent_symbols=frozenset(),
            required_present_symbols=frozenset(),
        )
    except EvidenceError as error:
        if "still contains handwritten FMGR export owners" not in str(
            error
        ):
            raise
    else:
        _fail("self-test", "incomplete final codegen closure was accepted")

    print("PostgreSQL safety evidence self-tests passed")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    validation_mode = parser.add_mutually_exclusive_group()
    validation_mode.add_argument("--self-test", action="store_true")
    validation_mode.add_argument(
        "--dso-only",
        action="store_true",
        help="validate only the generated SQL manifest against the linked DSO",
    )
    validation_mode.add_argument(
        "--static-only",
        action="store_true",
        help=(
            "validate current/reviewed profile ledgers and their "
            "generated SQL-manifest closure"
        ),
    )
    validation_mode.add_argument(
        "--evidence-only",
        action="store_true",
        help=(
            "validate static ledgers, runtime evidence, and profile "
            "SQL manifests without inspecting an active linked DSO"
        ),
    )
    parser.add_argument(
        "--require-complete-codegen",
        action="store_true",
        help="reject every remaining main-extension macro owner",
    )
    parser.add_argument("--production-ledger", type=Path)
    parser.add_argument("--test-indexes-ledger", type=Path)
    parser.add_argument("--reviewed-production-ledger", type=Path)
    parser.add_argument("--reviewed-test-indexes-ledger", type=Path)
    parser.add_argument("--production-source-list", type=Path)
    parser.add_argument("--test-indexes-source-list", type=Path)
    parser.add_argument("--production-compile-database", type=Path)
    parser.add_argument("--test-indexes-compile-database", type=Path)
    parser.add_argument("--production-compile-graph", type=Path)
    parser.add_argument("--test-indexes-compile-graph", type=Path)
    parser.add_argument("--production-pcm-manifest", type=Path)
    parser.add_argument("--test-indexes-pcm-manifest", type=Path)
    parser.add_argument("--production-compiler-facts", type=Path)
    parser.add_argument("--test-indexes-compiler-facts", type=Path)
    parser.add_argument(
        "--production-compiler-facts-producer-executable",
        type=Path,
    )
    parser.add_argument(
        "--test-indexes-compiler-facts-producer-executable",
        type=Path,
    )
    parser.add_argument("--provenance", type=Path)
    parser.add_argument("--runtime-policy", type=Path)
    parser.add_argument("--runtime-matrix", type=Path)
    parser.add_argument("--reviewed-runtime-matrix", type=Path)
    parser.add_argument("--production-case-plan", type=Path)
    parser.add_argument("--test-indexes-case-plan", type=Path)
    parser.add_argument("--production-runtime-inventory", type=Path)
    parser.add_argument("--test-indexes-runtime-inventory", type=Path)
    parser.add_argument(
        "--runtime-inventory-producer-executable",
        type=Path,
    )
    parser.add_argument("--production-sql-manifest", type=Path)
    parser.add_argument("--test-indexes-sql-manifest", type=Path)
    parser.add_argument("--production-profile-dso", type=Path)
    parser.add_argument("--test-indexes-profile-dso", type=Path)
    parser.add_argument("--runtime-control-dso", type=Path)
    parser.add_argument("--production-stage-manifest", type=Path)
    parser.add_argument("--test-indexes-stage-manifest", type=Path)
    parser.add_argument("--runtime-producer-executable", type=Path)
    parser.add_argument(
        "--production-runtime-test-executable",
        type=Path,
    )
    parser.add_argument(
        "--test-indexes-runtime-test-executable",
        type=Path,
    )
    parser.add_argument("--sql-manifest", type=Path)
    parser.add_argument(
        "--dso-contract",
        choices=sorted(DSO_CLOSURE_CONTRACTS),
        help=(
            "Bioseq uses its reviewed fixed inventory; manifest validates "
            "an extension's exact generated fmgr inventory"
        ),
    )
    parser.add_argument(
        "--allowed-dso-export",
        action="append",
        help=(
            "repeatable explicit non-fmgr ABI export; valid only for "
            "--dso-only --dso-contract manifest"
        ),
    )
    dso_source = parser.add_mutually_exclusive_group()
    dso_source.add_argument("--dso", type=Path)
    dso_source.add_argument("--symbol-dump", type=Path)
    parser.add_argument(
        "--symbol-format",
        choices=(
            "nm-posix",
            "nm",
            "readelf",
            "darwin-nm",
            "coff-exports",
            "dumpbin",
        ),
        default="nm-posix",
    )
    parser.add_argument("--report", type=Path)
    args = parser.parse_args(argv)

    try:
        validation_scope = _validate_command_line(args)
        if validation_scope == "self_test":
            run_python_self_tests()
            return 0
        if validation_scope == "dso":
            dso_contract = args.dso_contract or "bioseq"
            dynamic_symbols, dso_inspector, dso_input_sha256 = (
                _load_dynamic_symbol_input(
                    args.dso,
                    args.symbol_dump,
                    args.symbol_format,
                )
            )
            sql_manifest, sql_manifest_raw = load_json(args.sql_manifest)
            dso_summary = validate_dso_ownership(
                sql_manifest,
                dynamic_symbols,
                contract=dso_contract,
                allowed_dso_exports=tuple(
                    args.allowed_dso_export or ()
                ),
                require_complete_codegen=args.require_complete_codegen,
            )
            _write_report(
                args.report,
                {
                    "schema_revision": 1,
                    "status": "pass",
                    "validation_scope": "dso_closure",
                    "dso_contract": dso_contract,
                    "require_complete_codegen": (
                        args.require_complete_codegen
                    ),
                    "sql_manifest_sha256": _artifact_sha256(
                        sql_manifest_raw
                    ),
                    "dso_input_sha256": dso_input_sha256,
                    "dso_inspector": dso_inspector,
                    **dso_summary,
                },
            )
            return 0

        provenance, provenance_raw = load_json(args.provenance)
        provenance_records = validate_provenance(provenance)
        provenance_sha256 = _artifact_sha256(provenance_raw)

        production_json, production_raw = load_json(args.production_ledger)
        test_indexes_json, test_indexes_raw = load_json(args.test_indexes_ledger)
        _require_byte_equality(
            production_raw,
            args.reviewed_production_ledger,
            "production",
            actual_path=args.production_ledger,
        )
        _require_byte_equality(
            test_indexes_raw,
            args.reviewed_test_indexes_ledger,
            "test_indexes",
            actual_path=args.test_indexes_ledger,
        )
        validate_current_ledger_inputs(
            production_json,
            "production",
            source_list=args.production_source_list,
            compile_database=args.production_compile_database,
            producer_executable=(
                args.production_compiler_facts_producer_executable
            ),
            compile_graph=args.production_compile_graph,
            pcm_manifest=args.production_pcm_manifest,
            compiler_facts=args.production_compiler_facts,
        )
        validate_current_ledger_inputs(
            test_indexes_json,
            "test_indexes",
            source_list=args.test_indexes_source_list,
            compile_database=args.test_indexes_compile_database,
            producer_executable=(
                args.test_indexes_compiler_facts_producer_executable
            ),
            compile_graph=args.test_indexes_compile_graph,
            pcm_manifest=args.test_indexes_pcm_manifest,
            compiler_facts=args.test_indexes_compiler_facts,
        )
        production = validate_ledger(
            production_json, provenance_records, provenance_sha256
        )
        test_indexes = validate_ledger(
            test_indexes_json, provenance_records, provenance_sha256
        )
        validate_profile_pair(production, test_indexes)

        production_manifest, production_manifest_raw = load_json(
            args.production_sql_manifest
        )
        test_indexes_manifest, test_indexes_manifest_raw = load_json(
            args.test_indexes_sql_manifest
        )
        production_manifest_sha256 = _artifact_sha256(
            production_manifest_raw
        )
        test_indexes_manifest_sha256 = _artifact_sha256(
            test_indexes_manifest_raw
        )
        production_sql_script_sha256 = _sha256(
            _field(
                production_manifest,
                "script_sha256",
                "production-sql-manifest",
            ),
            "production-sql-manifest.script_sha256",
        )
        test_indexes_sql_script_sha256 = _sha256(
            _field(
                test_indexes_manifest,
                "script_sha256",
                "test-indexes-sql-manifest",
            ),
            "test-indexes-sql-manifest.script_sha256",
        )
        if (
            production["sql_manifest_sha256"]
                != production_manifest_sha256
            or test_indexes["sql_manifest_sha256"]
                != test_indexes_manifest_sha256
        ):
            _fail(
                "ledger-manifest",
                "profile ledger SQL-manifest hash differs from the "
                "generated manifest bytes",
            )
        manifest_summary = validate_ledger_manifest_closure(
            production_manifest,
            production,
            test_indexes,
            require_complete_codegen=args.require_complete_codegen,
        )
        test_indexes_manifest_summary = (
            validate_ledger_manifest_closure(
                test_indexes_manifest,
                production,
                test_indexes,
                require_complete_codegen=(
                    args.require_complete_codegen
                ),
            )
        )
        if test_indexes_manifest_summary != manifest_summary:
            _fail(
                "ledger-manifest",
                "production/test_indexes SQL-manifest closure "
                "differs",
            )
        if validation_scope == "static":
            _write_report(
                args.report,
                {
                    "schema_revision": 1,
                    "status": "pass",
                    "validation_scope": "ledger_manifest",
                    "require_complete_codegen": (
                        args.require_complete_codegen
                    ),
                    "postgresql_tag": PG17_TAG,
                    "postgresql_revision": PG17_REVISION,
                    "provenance_sha256": provenance_sha256,
                    "production_sql_manifest_sha256":
                        production_manifest_sha256,
                    "test_indexes_sql_manifest_sha256":
                        test_indexes_manifest_sha256,
                    "production_ledger_sha256": _artifact_sha256(
                        production_raw
                    ),
                    "test_indexes_ledger_sha256": _artifact_sha256(
                        test_indexes_raw
                    ),
                    "production_parent_count": len(production["parents"]),
                    "test_indexes_parent_count": len(
                        test_indexes["parents"]
                    ),
                    "public_symbol_count": len(production["symbols"]),
                    **manifest_summary,
                },
            )
            return 0

        (
            production_runtime_inventory_json,
            production_runtime_inventory_raw,
        ) = load_json(args.production_runtime_inventory)
        (
            test_indexes_runtime_inventory_json,
            test_indexes_runtime_inventory_raw,
        ) = load_json(args.test_indexes_runtime_inventory)
        runtime_inventory_producer_executable_sha256 = (
            _path_artifact_sha256(
                args.runtime_inventory_producer_executable,
                "runtime-inventory-producer-executable",
            )
        )
        production_runtime_inventory_sha256 = _artifact_sha256(
            production_runtime_inventory_raw
        )
        test_indexes_runtime_inventory_sha256 = _artifact_sha256(
            test_indexes_runtime_inventory_raw
        )
        production_runtime_inventory = validate_runtime_inventory(
            production_runtime_inventory_json,
            profile="production",
            inventory_sha256=production_runtime_inventory_sha256,
            ledger_document=production_json,
            ledger=production,
            ledger_sha256=_artifact_sha256(production_raw),
            inventory_producer_executable_sha256=(
                runtime_inventory_producer_executable_sha256
            ),
        )
        test_indexes_runtime_inventory = validate_runtime_inventory(
            test_indexes_runtime_inventory_json,
            profile="test_indexes",
            inventory_sha256=test_indexes_runtime_inventory_sha256,
            ledger_document=test_indexes_json,
            ledger=test_indexes,
            ledger_sha256=_artifact_sha256(test_indexes_raw),
            inventory_producer_executable_sha256=(
                runtime_inventory_producer_executable_sha256
            ),
        )

        runtime_policy, runtime_policy_raw = load_json(args.runtime_policy)
        policy_coverage = validate_runtime_policy(runtime_policy)
        runtime_matrix, runtime_matrix_raw = load_json(
            args.runtime_matrix,
            require_single_link=True,
        )
        _require_byte_equality(
            runtime_matrix_raw,
            args.reviewed_runtime_matrix,
            "runtime-matrix",
            actual_path=args.runtime_matrix,
        )
        production_case_plan_sha256 = _path_artifact_sha256(
            args.production_case_plan,
            "production-case-plan",
        )
        test_indexes_case_plan_sha256 = _path_artifact_sha256(
            args.test_indexes_case_plan,
            "test-indexes-case-plan",
        )
        production_dso_sha256 = _path_artifact_sha256(
            args.production_profile_dso,
            "production-profile-dso",
        )
        test_indexes_dso_sha256 = _path_artifact_sha256(
            args.test_indexes_profile_dso,
            "test-indexes-profile-dso",
        )
        runtime_control_dso_sha256 = _path_artifact_sha256(
            args.runtime_control_dso,
            "runtime-control-dso",
        )
        runtime_producer_executable_sha256 = (
            _path_artifact_sha256(
                args.runtime_producer_executable,
                "runtime-producer-executable",
            )
        )
        production_test_executable_sha256 = (
            _path_artifact_sha256(
                args.production_runtime_test_executable,
                "production-runtime-test-executable",
            )
        )
        test_indexes_test_executable_sha256 = (
            _path_artifact_sha256(
                args.test_indexes_runtime_test_executable,
                "test-indexes-runtime-test-executable",
            )
        )
        runtime_summary = validate_runtime_matrix(
            runtime_matrix,
            production,
            test_indexes,
            production_runtime_inventory,
            test_indexes_runtime_inventory,
            policy_coverage,
            production_sha256=_artifact_sha256(production_raw),
            test_indexes_sha256=_artifact_sha256(test_indexes_raw),
            production_sql_manifest_sha256=(
                production_manifest_sha256
            ),
            test_indexes_sql_manifest_sha256=(
                test_indexes_manifest_sha256
            ),
            production_case_plan_sha256=(
                production_case_plan_sha256
            ),
            test_indexes_case_plan_sha256=(
                test_indexes_case_plan_sha256
            ),
            production_runtime_inventory_sha256=(
                production_runtime_inventory_sha256
            ),
            test_indexes_runtime_inventory_sha256=(
                test_indexes_runtime_inventory_sha256
            ),
            runtime_policy_sha256=_artifact_sha256(runtime_policy_raw),
            production_dso_sha256=production_dso_sha256,
            test_indexes_dso_sha256=test_indexes_dso_sha256,
            runtime_control_dso_sha256=runtime_control_dso_sha256,
            runtime_producer_executable_sha256=(
                runtime_producer_executable_sha256
            ),
            production_test_executable_sha256=(
                production_test_executable_sha256
            ),
            test_indexes_test_executable_sha256=(
                test_indexes_test_executable_sha256
            ),
            production_ledger_path=args.production_ledger,
            test_indexes_ledger_path=args.test_indexes_ledger,
            production_sql_manifest_path=args.production_sql_manifest,
            test_indexes_sql_manifest_path=args.test_indexes_sql_manifest,
            production_sql_script_sha256=(
                production_sql_script_sha256
            ),
            test_indexes_sql_script_sha256=(
                test_indexes_sql_script_sha256
            ),
            production_case_plan_path=args.production_case_plan,
            test_indexes_case_plan_path=args.test_indexes_case_plan,
            production_stage_manifest_path=(
                args.production_stage_manifest
            ),
            test_indexes_stage_manifest_path=(
                args.test_indexes_stage_manifest
            ),
            production_dso_path=args.production_profile_dso,
            test_indexes_dso_path=args.test_indexes_profile_dso,
            runtime_control_dso_path=args.runtime_control_dso,
            runtime_producer_executable_path=(
                args.runtime_producer_executable
            ),
            production_test_executable_path=(
                args.production_runtime_test_executable
            ),
            test_indexes_test_executable_path=(
                args.test_indexes_runtime_test_executable
            ),
            runtime_matrix_path=args.runtime_matrix,
        )

        if validation_scope == "evidence":
            _write_report(
                args.report,
                {
                    "schema_revision": 1,
                    "status": "pass",
                    "validation_scope": "ledger_runtime_manifest",
                    "require_complete_codegen": (
                        args.require_complete_codegen
                    ),
                    "postgresql_tag": PG17_TAG,
                    "postgresql_revision": PG17_REVISION,
                    "provenance_sha256": provenance_sha256,
                    "production_sql_manifest_sha256":
                        production_manifest_sha256,
                    "test_indexes_sql_manifest_sha256":
                        test_indexes_manifest_sha256,
                    "production_ledger_sha256": _artifact_sha256(
                        production_raw
                    ),
                    "test_indexes_ledger_sha256": _artifact_sha256(
                        test_indexes_raw
                    ),
                    "runtime_policy_sha256": _artifact_sha256(
                        runtime_policy_raw
                    ),
                    "runtime_matrix_sha256": _artifact_sha256(
                        runtime_matrix_raw
                    ),
                    "production_case_plan_sha256":
                        production_case_plan_sha256,
                    "test_indexes_case_plan_sha256":
                        test_indexes_case_plan_sha256,
                    "production_runtime_inventory_sha256":
                        production_runtime_inventory_sha256,
                    "test_indexes_runtime_inventory_sha256":
                        test_indexes_runtime_inventory_sha256,
                    "runtime_inventory_producer_executable_sha256":
                        runtime_inventory_producer_executable_sha256,
                    "production_dso_sha256":
                        production_dso_sha256,
                    "test_indexes_dso_sha256":
                        test_indexes_dso_sha256,
                    "runtime_producer_executable_sha256":
                        runtime_producer_executable_sha256,
                    "production_test_executable_sha256":
                        production_test_executable_sha256,
                    "test_indexes_test_executable_sha256":
                        test_indexes_test_executable_sha256,
                    "production_parent_count": len(production["parents"]),
                    "test_indexes_parent_count": len(
                        test_indexes["parents"]
                    ),
                    "public_symbol_count": len(production["symbols"]),
                    **runtime_summary,
                    **manifest_summary,
                },
            )
            return 0

        sql_manifest, sql_manifest_raw = load_json(args.sql_manifest)
        active_profile = _field(
            sql_manifest, "compiler_profile", "active-sql-manifest"
        )
        if active_profile not in PROFILES:
            _fail(
                "active-sql-manifest",
                "compiler_profile is not production or "
                "test_indexes",
            )
        active_manifest_sha256 = _artifact_sha256(sql_manifest_raw)
        expected_active_manifest_sha256 = (
            production_manifest_sha256
            if active_profile == "production"
            else test_indexes_manifest_sha256
        )
        if active_manifest_sha256 != expected_active_manifest_sha256:
            _fail(
                "active-sql-manifest",
                "linked DSO manifest is not the selected reviewed "
                "profile manifest",
            )
        dynamic_symbols, dso_inspector, dso_input_sha256 = (
            _load_dynamic_symbol_input(
                args.dso,
                args.symbol_dump,
                args.symbol_format,
            )
        )
        expected_active_dso_sha256 = (
            production_dso_sha256
            if active_profile == "production"
            else test_indexes_dso_sha256
        )
        if dso_input_sha256 != expected_active_dso_sha256:
            _fail(
                "active-DSO",
                "inspected DSO bytes differ from the selected "
                "runtime-matrix profile artifact",
            )
        dso_summary = validate_dso_ownership(
            sql_manifest,
            dynamic_symbols,
            contract="bioseq",
            require_complete_codegen=args.require_complete_codegen,
        )

        _write_report(
            args.report,
            {
                "schema_revision": 1,
                "status": "pass",
                "validation_scope": "ledger_runtime_manifest_dso",
                "dso_contract": "bioseq",
                "require_complete_codegen": args.require_complete_codegen,
                "postgresql_tag": PG17_TAG,
                "postgresql_revision": PG17_REVISION,
                "provenance_sha256": provenance_sha256,
                "sql_manifest_sha256": _artifact_sha256(sql_manifest_raw),
                "production_sql_manifest_sha256":
                    production_manifest_sha256,
                "test_indexes_sql_manifest_sha256":
                    test_indexes_manifest_sha256,
                "production_ledger_sha256": _artifact_sha256(
                    production_raw
                ),
                "test_indexes_ledger_sha256": _artifact_sha256(
                    test_indexes_raw
                ),
                "runtime_policy_sha256": _artifact_sha256(
                    runtime_policy_raw
                ),
                "runtime_matrix_sha256": _artifact_sha256(
                    runtime_matrix_raw
                ),
                "production_case_plan_sha256":
                    production_case_plan_sha256,
                "test_indexes_case_plan_sha256":
                    test_indexes_case_plan_sha256,
                "production_runtime_inventory_sha256":
                    production_runtime_inventory_sha256,
                "test_indexes_runtime_inventory_sha256":
                    test_indexes_runtime_inventory_sha256,
                "runtime_inventory_producer_executable_sha256":
                    runtime_inventory_producer_executable_sha256,
                "production_dso_sha256":
                    production_dso_sha256,
                "test_indexes_dso_sha256":
                    test_indexes_dso_sha256,
                "runtime_producer_executable_sha256":
                    runtime_producer_executable_sha256,
                "production_test_executable_sha256":
                    production_test_executable_sha256,
                "test_indexes_test_executable_sha256":
                    test_indexes_test_executable_sha256,
                "dso_input_sha256": dso_input_sha256,
                "dso_inspector": dso_inspector,
                "production_parent_count": len(production["parents"]),
                "test_indexes_parent_count": len(test_indexes["parents"]),
                "public_symbol_count": len(production["symbols"]),
                **runtime_summary,
                **manifest_summary,
                **dso_summary,
            },
        )
        return 0
    except EvidenceError as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
