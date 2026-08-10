#!/usr/bin/env python3
"""Produce runtime PostgreSQL ERROR-safety evidence.

This program is an evidence orchestrator, not a test-result synthesizer.  It
accepts two separately built profile DSOs and profile-specific PostgreSQL
runtime test executables.  Each test executable must implement this fixed
three-command protocol:

* ``--bioseq-safety-runtime-activate`` installs and activates the requested
  DSO in the profile's PostgreSQL cluster and writes an activation receipt;
* ``--bioseq-safety-runtime-describe`` writes its compiled case catalog;
* ``--bioseq-safety-runtime-run`` executes one catalog case and writes the
  observed PostgreSQL result.

The orchestrator resolves every catalog case to one real schema revision 2 safety
ledger parent/site, rejects taxonomy claims not derived from that site, runs
the cases, independently re-hashes the active DSO and executables, and emits
content-addressed immutable log/result artifacts plus the schema revision 2 runtime
matrix.  FATAL and PANIC cases are rejected before execution.

Cluster configurations, requests, catalogs, activation receipts, raw results,
and recovery rows are copied into the candidate's content-addressed evidence
tree.  The two profiles must use distinct private PostgreSQL systems and data
directories.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, replace
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import signal
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any, Callable, Mapping, Sequence


PROFILES = ("production", "test_indexes")
GUC_PROFILE_COUNTS = {"production": 73, "test_indexes": 76}
GUC_TEST_INDEXES_ONLY_NAMES = (
    "bioseq.testing.chunked_scanner.enabled",
    "bioseq.testing.in_memory_scanner.enabled",
    "bioseq.testing.persisted_index.failure_mode",
)
GUC_OWNER_BASE_COUNTS = {
    "bioseq": 26,
    "scheduler": 3,
    "storage": 1,
    "aligner": 43,
}
GUC_OWNER_SOURCE_MODULES = {
    "bioseq": "bioseq.postgres.configuration.core",
    "scheduler": "bioseq.postgres.configuration.scheduler",
    "storage": "bioseq.postgres.configuration.storage",
    "aligner": "bioseq.postgres.configuration.aligner",
}
EXPECTED_TEMPLATE_COUNT = 25
EXPECTED_PROFILE_CASE_COUNT = 25
EXPECTED_MATRIX_CASE_COUNT = 50
COVERAGE_DIMENSIONS = (
    "classification",
    "bridge",
    "converter_family",
    "owner_family",
    "cleanup_phase",
)
FAULT_MODES = ("injected", "invocation")
POSTGRESQL_VERSION_NUM = 170010
DEFAULT_TIMEOUT_SECONDS = 600
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
SQLSTATE_PATTERN = re.compile(r"[0-9A-Z]{5}")
CASE_ID_PATTERN = re.compile(r"[a-z0-9][a-z0-9_.-]{0,127}")
FAULT_KEY_PATTERN = re.compile(r"[a-z0-9_.]{1,128}")
PROBE_SITE_ID_PATTERN = re.compile(r"probe:[0-9a-f]{64}")
BACKEND_SCOPED_VACUUM_FAULT_KEY = (
    "invoke_pg_as_cpp.vacuum_cleanup_restore"
)
POSTGRESQL_TOOLS = (
    "postgres",
    "initdb",
    "pg_ctl",
    "createdb",
    "dropdb",
    "psql",
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
RUNTIME_POLICY_FIELDS = frozenset(
    {"schema_revision", "required_coverage"}
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
COVERAGE_KEY_FIELDS = frozenset({"profile", "dimension", "value"})
SOURCE_POSITION_FIELDS = frozenset(
    {"byte_offset", "line", "column"}
)
SOURCE_RANGE_FIELDS = frozenset({"begin", "end"})
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
RUNTIME_TEMPLATE_FIELDS = frozenset({"schema_revision", "cases"})
TEMPLATE_CASE_FIELDS = frozenset(
    {
        "case_id",
        "profiles",
        "sql_identity",
        "invocation_key",
        "fault_key",
        "postgres_severity",
        "expected",
        "execution",
    }
)
FORBIDDEN_TEMPLATE_AUTHORITY_FIELDS = frozenset(
    {
        "site_id",
        "site_selector",
        "fault_mode",
        "relation",
        "coverage",
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
POSTGRES_ERROR_FIELDS = frozenset(
    {"severity", "sqlstate", "message"}
)
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
RUNTIME_CLOSURE_FIELDS = frozenset(
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
RUNTIME_ROOT_FIELDS = frozenset(
    {"reported_source", "canonical_source", "staged"}
)
RUNTIME_ENTRY_FIELDS = frozenset(
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
RUNTIME_SENTINEL_FIELDS = frozenset({"root", "path", "kind", "sha256"})
RUNTIME_LOADER_FIELDS = frozenset(
    {"entry_count", "aggregate_sha256", "entries"}
)
RUNTIME_LOADER_ENTRY_FIELDS = frozenset(
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
STAGE_CONTROL_FIELDS = frozenset(
    {"source", "filename", "staged", "source_sha256", "sha256"}
)
STAGE_INSTALL_SQL_FIELDS = frozenset(
    {"source", "filename", "staged", "version", "sha256"}
)
STAGE_LIBRARY_FIELDS = frozenset(
    {"source", "filename", "staged", "sha256"}
)
STAGE_TOOL_SIGNATURE_FIELDS = frozenset(
    {"tool", "staged_path", "version", "sha256"}
)
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
RUNTIME_RELATION_CONTRACTS: dict[str, tuple[int, int, str]] = {
    "natural_at_site": (1, 0, "visit_only"),
    "injected_routes_to_site": (1, 1, "trigger_before_visit"),
    "injected_after_site_success": (1, 1, "visit_before_trigger"),
}
GENERATED_BOUNDARY_FAULT_KEY = "generated_boundary_tail.std_exception"


class EvidenceError(RuntimeError):
    """Raised when runtime evidence does not close the reviewed contract."""


def _fail(context: str, message: str) -> None:
    raise EvidenceError(f"{context}: {message}")


def _reject_duplicate_json_keys(
    pairs: list[tuple[str, Any]],
) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise EvidenceError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


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
            f"{context}: cannot authenticate directory {absolute}: {error}"
        ) from error


def _open_or_create_child_directory(
    parent_descriptor: int,
    component: str,
    context: str,
) -> int:
    if component in {"", ".", ".."} or "/" in component:
        _fail(context, f"invalid output-directory component {component!r}")
    try:
        return os.open(
            component,
            os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
            dir_fd=parent_descriptor,
        )
    except FileNotFoundError:
        try:
            os.mkdir(component, mode=0o700, dir_fd=parent_descriptor)
        except FileExistsError:
            pass
        except OSError as error:
            raise EvidenceError(
                f"{context}: cannot create output directory {component}: "
                f"{error}"
            ) from error
        try:
            return os.open(
                component,
                os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                dir_fd=parent_descriptor,
            )
        except OSError as error:
            raise EvidenceError(
                f"{context}: cannot authenticate created output directory "
                f"{component}: {error}"
            ) from error
    except OSError as error:
        raise EvidenceError(
            f"{context}: output-directory component is not a real directory "
            f"{component}: {error}"
        ) from error


def _secure_descendant_directory(
    root: Path,
    directory: Path,
    context: str,
) -> Path:
    root_absolute = _absolute_path(root)
    directory_absolute = _absolute_path(directory)
    try:
        relative = directory_absolute.relative_to(root_absolute)
    except ValueError:
        _fail(context, "output directory escapes its authenticated root")
    root_descriptor = _open_directory(root_absolute, f"{context}.root")
    descriptor = root_descriptor
    try:
        for component in relative.parts:
            next_descriptor = _open_or_create_child_directory(
                descriptor,
                component,
                context,
            )
            if descriptor != root_descriptor:
                os.close(descriptor)
            descriptor = next_descriptor
        metadata = os.fstat(descriptor)
        if not stat.S_ISDIR(metadata.st_mode):
            _fail(context, "authenticated output parent is not a directory")
    finally:
        if descriptor != root_descriptor:
            os.close(descriptor)
        os.close(root_descriptor)
    return directory_absolute


def _descriptor_bytes(
    descriptor: int,
    context: str,
    *,
    require_single_link: bool,
) -> tuple[bytes, os.stat_result]:
    before = os.fstat(descriptor)
    if not stat.S_ISREG(before.st_mode):
        _fail(context, "artifact is not a regular file")
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


def _read_single_link_file_with_metadata(
    path: Path,
    context: str,
) -> tuple[bytes, os.stat_result]:
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
                f"{context}: cannot open authenticated file {absolute}: "
                f"{error}"
            ) from error
        try:
            return _descriptor_bytes(
                descriptor,
                context,
                require_single_link=True,
            )
        finally:
            os.close(descriptor)
    finally:
        os.close(parent_descriptor)


def _read_single_link_file(path: Path, context: str) -> bytes:
    raw, _ = _read_single_link_file_with_metadata(path, context)
    return raw


def _read_regular_file_with_metadata(
    path: Path,
    context: str,
) -> tuple[bytes, os.stat_result]:
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
                f"{context}: cannot open authenticated file {absolute}: "
                f"{error}"
            ) from error
        try:
            return _descriptor_bytes(
                descriptor,
                context,
                require_single_link=False,
            )
        finally:
            os.close(descriptor)
    finally:
        os.close(parent_descriptor)


def _read_regular_file(path: Path, context: str) -> bytes:
    raw, _ = _read_regular_file_with_metadata(path, context)
    return raw


def _write_all(descriptor: int, raw: bytes, context: str) -> None:
    view = memoryview(raw)
    written = 0
    while written != len(view):
        try:
            count = os.write(descriptor, view[written:])
        except OSError as error:
            raise EvidenceError(
                f"{context}: cannot write temporary artifact: {error}"
            ) from error
        if count <= 0:
            _fail(context, "temporary artifact write made no progress")
        written += count


def _publish_file(
    path: Path,
    raw: bytes,
    *,
    replace_existing: bool,
) -> None:
    absolute = _absolute_path(path)
    context = str(absolute)
    parent_descriptor = _open_directory(absolute.parent, f"{context}.parent")
    temporary_name = (
        f".{absolute.name}.tmp.{os.getpid()}.{secrets.token_hex(12)}"
    )
    temporary_present = False
    try:
        try:
            existing_descriptor = os.open(
                absolute.name,
                os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                dir_fd=parent_descriptor,
            )
        except FileNotFoundError:
            existing_descriptor = -1
        except OSError as error:
            raise EvidenceError(
                f"{context}: existing output cannot be authenticated: {error}"
            ) from error
        if existing_descriptor >= 0:
            try:
                existing, _ = _descriptor_bytes(
                    existing_descriptor,
                    context,
                    require_single_link=True,
                )
            finally:
                os.close(existing_descriptor)
            if existing == raw:
                return
            if not replace_existing:
                _fail(context, "refuses to overwrite different existing bytes")

        try:
            temporary_descriptor = os.open(
                temporary_name,
                (
                    os.O_WRONLY
                    | os.O_CREAT
                    | os.O_EXCL
                    | os.O_NOFOLLOW
                ),
                0o600,
                dir_fd=parent_descriptor,
            )
            temporary_present = True
        except OSError as error:
            raise EvidenceError(
                f"{context}: cannot create exclusive temporary artifact: "
                f"{error}"
            ) from error
        try:
            _write_all(temporary_descriptor, raw, context)
            os.fsync(temporary_descriptor)
            temporary_metadata = os.fstat(temporary_descriptor)
            if (
                not stat.S_ISREG(temporary_metadata.st_mode)
                or temporary_metadata.st_nlink != 1
            ):
                _fail(context, "temporary artifact identity is unsafe")
        finally:
            os.close(temporary_descriptor)

        if replace_existing:
            os.replace(
                temporary_name,
                absolute.name,
                src_dir_fd=parent_descriptor,
                dst_dir_fd=parent_descriptor,
            )
            temporary_present = False
        else:
            try:
                os.link(
                    temporary_name,
                    absolute.name,
                    src_dir_fd=parent_descriptor,
                    dst_dir_fd=parent_descriptor,
                    follow_symlinks=False,
                )
            except FileExistsError as error:
                raise EvidenceError(
                    f"{context}: immutable target appeared during publication"
                ) from error
            os.unlink(temporary_name, dir_fd=parent_descriptor)
            temporary_present = False
        os.fsync(parent_descriptor)

        published_descriptor = os.open(
            absolute.name,
            os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
            dir_fd=parent_descriptor,
        )
        try:
            published, _ = _descriptor_bytes(
                published_descriptor,
                context,
                require_single_link=True,
            )
        finally:
            os.close(published_descriptor)
        if published != raw:
            _fail(context, "published artifact differs after atomic publication")
    except OSError as error:
        raise EvidenceError(
            f"{context}: atomic publication failed: {error}"
        ) from error
    finally:
        if temporary_present:
            try:
                os.unlink(temporary_name, dir_fd=parent_descriptor)
            except FileNotFoundError:
                pass
        os.close(parent_descriptor)


def _load_json(path: Path, context: str) -> tuple[dict[str, Any], bytes]:
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise EvidenceError(f"{context}: cannot read {path}: {error}") from error
    try:
        value = json.loads(
            raw,
            object_pairs_hook=_reject_duplicate_json_keys,
        )
    except json.JSONDecodeError as error:
        raise EvidenceError(
            f"{context}: invalid JSON in {path}: {error}"
        ) from error
    if not isinstance(value, dict):
        _fail(context, "top-level JSON value must be an object")
    return value, raw


def _canonical_json(value: Any) -> bytes:
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        + "\n"
    ).encode("utf-8")


def _sha256_bytes(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _sha256_file(path: Path, context: str) -> str:
    try:
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError as error:
        raise EvidenceError(f"{context}: cannot hash {path}: {error}") from error


def _object(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        _fail(context, "expected an object")
    return value


def _array(value: Any, context: str) -> list[Any]:
    if not isinstance(value, list):
        _fail(context, "expected an array")
    return value


def _field(value: Mapping[str, Any], name: str, context: str) -> Any:
    if name not in value:
        _fail(context, f"missing field {name!r}")
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


def _integer(value: Any, context: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        _fail(context, "expected an integer")
    return value


def _nonnegative_integer(value: Any, context: str) -> int:
    result = _integer(value, context)
    if result < 0:
        _fail(context, "expected a nonnegative integer")
    return result


def _postgresql_version_num(value: Any, context: str) -> int:
    version = _integer(value, context)
    if version != POSTGRESQL_VERSION_NUM:
        _fail(context, "must be exact PostgreSQL 17.10")
    return version


def _boolean(value: Any, context: str) -> bool:
    if not isinstance(value, bool):
        _fail(context, "expected a boolean")
    return value


def _sha256(value: Any, context: str) -> str:
    result = _string(value, context)
    if SHA256_PATTERN.fullmatch(result) is None:
        _fail(context, "expected a lowercase SHA-256")
    return result


def _string_list(
    value: Any,
    context: str,
    *,
    allow_empty: bool = False,
) -> list[str]:
    result = [
        _string(item, f"{context}[{index}]")
        for index, item in enumerate(_array(value, context))
    ]
    if not allow_empty and not result:
        _fail(context, "expected a non-empty string array")
    if len(result) != len(set(result)):
        _fail(context, "contains a duplicate string")
    return result


CoverageKey = tuple[str, str, str]


def _coverage_key(value: Any, context: str) -> CoverageKey:
    key = _object(value, context)
    _require_exact_fields(key, COVERAGE_KEY_FIELDS, context)
    profile = _string(_field(key, "profile", context), f"{context}.profile")
    dimension = _string(
        _field(key, "dimension", context),
        f"{context}.dimension",
    )
    item = _string(_field(key, "value", context), f"{context}.value")
    if profile not in PROFILES:
        _fail(context, "unknown profile")
    if dimension not in COVERAGE_DIMENSIONS:
        _fail(context, "unknown coverage dimension")
    return profile, dimension, item


def _coverage_json(key: CoverageKey) -> dict[str, str]:
    profile, dimension, value = key
    return {
        "profile": profile,
        "dimension": dimension,
        "value": value,
    }


def _coverage_array(
    value: Any,
    context: str,
    *,
    expected_profile: str | None = None,
) -> tuple[CoverageKey, ...]:
    result: set[CoverageKey] = set()
    for index, raw_key in enumerate(_array(value, context)):
        key = _coverage_key(raw_key, f"{context}[{index}]")
        if expected_profile is not None and key[0] != expected_profile:
            _fail(context, "contains a cross-profile key")
        if key in result:
            _fail(context, "contains a duplicate key")
        result.add(key)
    if not result:
        _fail(context, "must contain at least one key")
    return tuple(sorted(result))


def _source_position(value: Any, context: str) -> tuple[int, int, int]:
    position = _object(value, context)
    _require_exact_fields(
        position,
        SOURCE_POSITION_FIELDS,
        context,
    )
    result = tuple(
        _integer(
            _field(position, field_name, context),
            f"{context}.{field_name}",
        )
        for field_name in ("byte_offset", "line", "column")
    )
    if any(item < 0 for item in result):
        _fail(context, "source position values must be non-negative")
    return result


def _source_range(value: Any, context: str) -> dict[str, Any]:
    span = _object(value, context)
    _require_exact_fields(span, SOURCE_RANGE_FIELDS, context)
    begin = _source_position(
        _field(span, "begin", context),
        f"{context}.begin",
    )
    end = _source_position(
        _field(span, "end", context),
        f"{context}.end",
    )
    if end[0] <= begin[0] or end[1:] < begin[1:]:
        _fail(context, "source range is empty or reversed")
    return dict(span)


def _call_path(value: Any, context: str) -> list[dict[str, Any]]:
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
        _source_range(
            _field(step, "source_span", step_context),
            f"{step_context}.source_span",
        )
        result.append(dict(step))
    return result


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


def _probe_contract(
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


def _probe_site_id(sql_identity: str, fault_key: str) -> str:
    preimage = (
        "bioseq.safety_runtime_probe.site\nrevision=2\n"
        + sql_identity
        + "\n"
        + fault_key
        + "\n"
    ).encode("utf-8")
    return "probe:" + _sha256_bytes(preimage)


@dataclass(frozen=True)
class LedgerSite:
    sql_identity: str
    site_id: str
    source_file: str
    callee_or_macro: str
    classification: str
    bridge: str
    converter_families: tuple[str, ...]
    owner_families: tuple[str, ...]
    cleanup_phases: tuple[str, ...]
    owner_usr: str
    expanded_source_span: Mapping[str, Any]
    transitive_call_path: tuple[Mapping[str, Any], ...]
    ledger_site_sha256: str

    def coverage(self, profile: str) -> frozenset[CoverageKey]:
        result: set[CoverageKey] = {
            (profile, "classification", self.classification),
        }
        if self.bridge != "none":
            result.add((profile, "bridge", self.bridge))
        result.update(
            (profile, "converter_family", value)
            for value in self.converter_families
        )
        result.update(
            (profile, "owner_family", value)
            for value in self.owner_families
        )
        result.update(
            (profile, "cleanup_phase", value)
            for value in self.cleanup_phases
        )
        return frozenset(result)


@dataclass(frozen=True)
class Ledger:
    profile: str
    path: Path
    sha256: str
    sql_manifest_sha256: str
    compiler_facts_sha256: str
    source_list_sha256: str
    compile_database_sha256: str
    producer_executable_sha256: str
    compile_graph_sha256: str
    pcm_manifest_sha256: str
    sites_by_parent: Mapping[str, tuple[LedgerSite, ...]]

    @property
    def coverage(self) -> frozenset[CoverageKey]:
        return frozenset(
            key
            for sites in self.sites_by_parent.values()
            for site in sites
            for key in site.coverage(self.profile)
        )


@dataclass(frozen=True)
class RuntimeInventoryEntry:
    sql_identity: str
    fault_key: str
    site_id: str
    relation: str
    ledger_site_sha256: str
    probe_contract: Mapping[str, Any]
    ledger_site: LedgerSite

    @property
    def key(self) -> tuple[str, str]:
        return self.sql_identity, self.fault_key


@dataclass(frozen=True)
class RuntimeInventory:
    profile: str
    path: Path
    sha256: str
    producer_executable_sha256: str
    entries: Mapping[tuple[str, str], RuntimeInventoryEntry]


def _parse_ledger(path: Path, expected_profile: str) -> Ledger:
    ledger, raw = _load_json(path, f"{expected_profile}-ledger")
    context = f"{expected_profile}-ledger"
    _require_exact_fields(ledger, LEDGER_FIELDS, context)
    if _integer(
        _field(ledger, "schema_revision", context),
        f"{context}.schema_revision",
    ) != 2:
        _fail(context, "runtime evidence requires schema revision 2 safety ledgers")
    profile = _string(
        _field(ledger, "profile", context),
        f"{context}.profile",
    )
    if profile != expected_profile:
        _fail(context, "ledger profile differs from its command-line role")
    sql_manifest_sha256 = _sha256(
        _field(ledger, "sql_manifest_sha256", context),
        f"{context}.sql_manifest_sha256",
    )
    ledger_artifact_hashes = {
        field_name: _sha256(
            _field(ledger, field_name, context),
            f"{context}.{field_name}",
        )
        for field_name in (
            "compiler_facts_sha256",
            "source_list_sha256",
            "compile_database_sha256",
            "producer_executable_sha256",
            "compile_graph_sha256",
            "pcm_manifest_sha256",
        )
    }
    parents: dict[str, tuple[LedgerSite, ...]] = {}
    for parent_index, raw_parent in enumerate(
        _array(_field(ledger, "parents", context), f"{context}.parents")
    ):
        parent_context = f"{context}.parents[{parent_index}]"
        parent = _object(raw_parent, parent_context)
        sql_identity = _string(
            _field(parent, "sql_identity", parent_context),
            f"{parent_context}.sql_identity",
        )
        if sql_identity in parents:
            _fail(context, f"duplicate SQL identity {sql_identity!r}")
        owner_usr = _string(
            _field(parent, "owner_usr", parent_context),
            f"{parent_context}.owner_usr",
        )
        sites: list[LedgerSite] = []
        site_ids: set[str] = set()
        for site_index, raw_site in enumerate(
            _array(
                _field(parent, "sites", parent_context),
                f"{parent_context}.sites",
            )
        ):
            site_context = f"{parent_context}.sites[{site_index}]"
            site = _object(raw_site, site_context)
            site_id = _string(
                _field(site, "site_id", site_context),
                f"{site_context}.site_id",
            )
            if site_id in site_ids:
                _fail(parent_context, f"duplicate site id {site_id!r}")
            site_ids.add(site_id)
            possible_severities = _string_list(
                _field(site, "possible_severities", site_context),
                f"{site_context}.possible_severities",
            )
            if possible_severities != ["error"]:
                _fail(
                    site_context,
                    "runtime ledger site must be recoverable ERROR only",
                )
            expanded_source_span = _source_range(
                _field(site, "expanded_source_span", site_context),
                f"{site_context}.expanded_source_span",
            )
            transitive_call_path = _call_path(
                _field(site, "transitive_call_path", site_context),
                f"{site_context}.transitive_call_path",
            )
            sites.append(
                LedgerSite(
                    sql_identity=sql_identity,
                    site_id=site_id,
                    source_file=_string(
                        _field(site, "source_file", site_context),
                        f"{site_context}.source_file",
                    ),
                    callee_or_macro=_string(
                        _field(site, "callee_or_macro", site_context),
                        f"{site_context}.callee_or_macro",
                    ),
                    classification=_string(
                        _field(site, "classification", site_context),
                        f"{site_context}.classification",
                    ),
                    bridge=_string(
                        _field(site, "bridge", site_context),
                        f"{site_context}.bridge",
                    ),
                    converter_families=tuple(
                        sorted(
                            _string_list(
                                _field(
                                    site,
                                    "converter_families",
                                    site_context,
                                ),
                                f"{site_context}.converter_families",
                                allow_empty=True,
                            )
                        )
                    ),
                    owner_families=tuple(
                        sorted(
                            _string_list(
                                _field(
                                    site,
                                    "owner_families",
                                    site_context,
                                ),
                                f"{site_context}.owner_families",
                                allow_empty=True,
                            )
                        )
                    ),
                    cleanup_phases=tuple(
                        sorted(
                            _string_list(
                                _field(
                                    site,
                                    "cleanup_phases",
                                    site_context,
                                ),
                                f"{site_context}.cleanup_phases",
                                allow_empty=True,
                            )
                        )
                    ),
                    owner_usr=owner_usr,
                    expanded_source_span=expanded_source_span,
                    transitive_call_path=tuple(
                        transitive_call_path
                    ),
                    ledger_site_sha256=_sha256_bytes(
                        _canonical_json(site)
                    ),
                )
            )
        parents[sql_identity] = tuple(sites)
    if not parents:
        _fail(context, "ledger has no parents")
    return Ledger(
        profile=profile,
        path=path.resolve(),
        sha256=_sha256_bytes(raw),
        sql_manifest_sha256=sql_manifest_sha256,
        compiler_facts_sha256=ledger_artifact_hashes[
            "compiler_facts_sha256"
        ],
        source_list_sha256=ledger_artifact_hashes[
            "source_list_sha256"
        ],
        compile_database_sha256=ledger_artifact_hashes[
            "compile_database_sha256"
        ],
        producer_executable_sha256=ledger_artifact_hashes[
            "producer_executable_sha256"
        ],
        compile_graph_sha256=ledger_artifact_hashes[
            "compile_graph_sha256"
        ],
        pcm_manifest_sha256=ledger_artifact_hashes[
            "pcm_manifest_sha256"
        ],
        sites_by_parent=parents,
    )


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
    _source_range(
        _field(anchor, "expanded_source_span", context),
        f"{context}.expanded_source_span",
    )
    _sha256(
        _field(anchor, "source_fragment_sha256", context),
        f"{context}.source_fragment_sha256",
    )
    return dict(anchor)


def _parse_runtime_inventory(
    path: Path,
    *,
    profile: str,
    ledger: Ledger,
    producer_executable_sha256: str,
) -> RuntimeInventory:
    context = f"{profile}-runtime-inventory"
    if producer_executable_sha256 != ledger.producer_executable_sha256:
        _fail(
            context,
            "inventory producer differs from the ledger producer executable",
        )
    inventory, raw = _load_json(path, context)
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
        "ledger_sha256": ledger.sha256,
        "compiler_facts_sha256": ledger.compiler_facts_sha256,
        "sql_manifest_sha256": ledger.sql_manifest_sha256,
        "source_list_sha256": ledger.source_list_sha256,
        "compile_database_sha256": ledger.compile_database_sha256,
        "compile_graph_sha256": ledger.compile_graph_sha256,
        "pcm_manifest_sha256": ledger.pcm_manifest_sha256,
        "inventory_producer_executable_sha256": (
            producer_executable_sha256
        ),
    }
    for field_name, expected in expected_hashes.items():
        if _sha256(
            _field(inventory, field_name, context),
            f"{context}.{field_name}",
        ) != expected:
            _fail(context, f"{field_name} is stale")

    entries: dict[tuple[str, str], RuntimeInventoryEntry] = {}
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
        contract = _probe_contract(
            _field(entry, "probe_contract", entry_context),
            relation,
            f"{entry_context}.probe_contract",
        )
        ledger_site_sha256 = _sha256(
            _field(entry, "ledger_site_sha256", entry_context),
            f"{entry_context}.ledger_site_sha256",
        )
        ledger_sites = [
            site
            for site in ledger.sites_by_parent.get(sql_identity, ())
            if site.ledger_site_sha256 == ledger_site_sha256
        ]
        if len(ledger_sites) != 1:
            _fail(
                entry_context,
                "ledger_site_sha256 does not select one exact ledger site",
            )
        ledger_site = ledger_sites[0]
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
        raw_trigger_anchor = _field(
            entry,
            "trigger_anchor",
            entry_context,
        )
        trigger_anchor: dict[str, Any] | None
        if relation == "natural_at_site":
            if raw_trigger_anchor is not None:
                _fail(
                    entry_context,
                    "natural relation requires a null trigger anchor",
                )
            trigger_anchor = None
        else:
            trigger_anchor = _runtime_anchor(
                raw_trigger_anchor,
                expected_kind="consume_fault",
                context=f"{entry_context}.trigger_anchor",
            )
        anchors = [site_anchor, visit_anchor]
        if trigger_anchor is not None:
            anchors.append(trigger_anchor)
        anchor_ids = [
            _string(
                anchor["compiler_stable_id"],
                f"{entry_context}.compiler_stable_id",
            )
            for anchor in anchors
        ]
        if (
            len(anchor_ids) != len(set(anchor_ids))
            or any(anchor_id in stable_anchor_ids for anchor_id in anchor_ids)
        ):
            _fail(entry_context, "runtime anchor identity is duplicated")
        stable_anchor_ids.update(anchor_ids)
        if (
            site_anchor["compiler_stable_id"] != ledger_site.site_id
            or site_anchor["source_file"] != ledger_site.source_file
            or site_anchor["expanded_source_span"]
                != ledger_site.expanded_source_span
            or any(
                anchor["enclosing_function_usr"]
                    != site_anchor["enclosing_function_usr"]
                for anchor in anchors
            )
        ):
            _fail(
                entry_context,
                "runtime anchors differ from the exact ledger owner/site",
            )
        transitive_call_path = _call_path(
            _field(entry, "transitive_call_path", entry_context),
            f"{entry_context}.transitive_call_path",
        )
        if transitive_call_path != list(
            ledger_site.transitive_call_path
        ) or site_anchor["enclosing_function_usr"] not in {
            step["usr"] for step in transitive_call_path
        }:
            _fail(
                entry_context,
                "transitive call path differs from the exact ledger site",
            )
        entries[key] = RuntimeInventoryEntry(
            sql_identity=sql_identity,
            fault_key=fault_key,
            site_id=site_id,
            relation=relation,
            ledger_site_sha256=ledger_site_sha256,
            probe_contract=contract,
            ledger_site=ledger_site,
        )
    if not entries:
        _fail(context, "runtime inventory is empty")
    return RuntimeInventory(
        profile=profile,
        path=path.resolve(),
        sha256=_sha256_bytes(raw),
        producer_executable_sha256=producer_executable_sha256,
        entries=entries,
    )


def _parse_runtime_policy(path: Path) -> tuple[frozenset[CoverageKey], str]:
    policy, raw = _load_json(path, "runtime-policy")
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
    keys = _coverage_array(
        _field(policy, "required_coverage", "runtime-policy"),
        "runtime-policy.required_coverage",
    )
    if {key[0] for key in keys} != set(PROFILES):
        _fail("runtime-policy", "must cover both compilation profiles")
    return frozenset(keys), _sha256_bytes(raw)


def _required_coverage(
    ledgers: Mapping[str, Ledger],
    policy: frozenset[CoverageKey],
) -> frozenset[CoverageKey]:
    ledger_coverage = frozenset(
        key for ledger in ledgers.values() for key in ledger.coverage
    )
    missing_policy = policy - ledger_coverage
    if missing_policy:
        _fail(
            "runtime-policy",
            "claims keys absent from every real ledger site: "
            + ", ".join("/".join(key) for key in sorted(missing_policy)),
        )
    return ledger_coverage | policy


@dataclass(frozen=True)
class CatalogCase:
    case_id: str
    profile: str
    sql_identity: str
    site: LedgerSite
    site_id: str
    ledger_site_sha256: str
    invocation_key: str
    fault_key: str
    relation: str
    probe_contract: Mapping[str, Any]
    expected_backend_outcome: str
    expected_destructor_sentinels: tuple[str, ...]
    expected_cleanup_events: tuple[str, ...]
    expected_postgres_cleanup_events: tuple[str, ...]
    expected_sqlstate: str | None
    expected_message: str | None
    expected_recovery_postcondition: str
    catalog_entry_sha256: str


def _parse_catalog(
    path: Path,
    *,
    profile: str,
    ledger: Ledger,
    inventory: RuntimeInventory,
    test_executable_sha256: str,
    case_plan_sha256: str,
    case_plan_entries: Mapping[str, Mapping[str, Any]],
    required: frozenset[CoverageKey],
    expected_case_count: int = EXPECTED_PROFILE_CASE_COUNT,
) -> tuple[tuple[CatalogCase, ...], str]:
    catalog, raw = _load_json(path, f"{profile}-case-catalog")
    context = f"{profile}-case-catalog"
    _require_exact_fields(catalog, CATALOG_FIELDS, context)
    if _integer(
        _field(catalog, "schema_revision", context),
        f"{context}.schema_revision",
    ) != 2:
        _fail(context, "unsupported schema revision")
    if _string(
        _field(catalog, "profile", context),
        f"{context}.profile",
    ) != profile:
        _fail(context, "catalog profile differs from active profile")
    if _sha256(
        _field(catalog, "test_executable_sha256", context),
        f"{context}.test_executable_sha256",
    ) != test_executable_sha256:
        _fail(context, "catalog came from a different test executable")
    if _sha256(
        _field(catalog, "case_plan_sha256", context),
        f"{context}.case_plan_sha256",
    ) != case_plan_sha256:
        _fail(context, "catalog came from a different compiled case plan")
    catalog_cases = _array(
        _field(catalog, "cases", context), f"{context}.cases"
    )
    if (
        len(catalog_cases) != expected_case_count
        or len(case_plan_entries) != expected_case_count
    ):
        _fail(
            context,
            f"catalog and plan require exactly "
            f"{expected_case_count} profile cases",
        )
    cases: list[CatalogCase] = []
    case_ids: set[str] = set()
    invocation_fault_keys: set[tuple[str, str]] = set()
    observed_coverage: set[CoverageKey] = set()
    for index, raw_entry in enumerate(catalog_cases):
        entry_context = f"{context}.cases[{index}]"
        entry = _object(raw_entry, entry_context)
        _require_exact_fields(
            entry,
            CATALOG_ENTRY_FIELDS,
            entry_context,
        )
        case_id = _string(
            _field(entry, "case_id", entry_context),
            f"{entry_context}.case_id",
        )
        if CASE_ID_PATTERN.fullmatch(case_id) is None:
            _fail(
                entry_context,
                "case_id must be a bounded lowercase artifact-safe name",
            )
        if case_id in case_ids:
            _fail(context, f"duplicate case id {case_id!r}")
        case_ids.add(case_id)
        planned_case = case_plan_entries.get(case_id)
        if (
            planned_case is None
            or _object(
                _field(planned_case, "catalog", f"{entry_context}.plan"),
                f"{entry_context}.plan.catalog",
            )
            != entry
        ):
            _fail(
                entry_context,
                "published catalog entry differs from the current case plan",
            )
        sql_identity = _string(
            _field(entry, "sql_identity", entry_context),
            f"{entry_context}.sql_identity",
        )
        invocation_key = _string(
            _field(entry, "invocation_key", entry_context),
            f"{entry_context}.invocation_key",
        )
        fault_key = _fault_key(
            _field(entry, "fault_key", entry_context),
            f"{entry_context}.fault_key",
        )
        inventory_entry = inventory.entries.get(
            (sql_identity, fault_key)
        )
        if inventory_entry is None:
            _fail(
                entry_context,
                "case is absent from the current runtime inventory",
            )
        site = inventory_entry.ledger_site
        invocation_fault_key = (invocation_key, fault_key)
        if invocation_fault_key in invocation_fault_keys:
            _fail(
                context,
                "duplicate invocation/fault key "
                f"{invocation_key!r}/{fault_key!r}",
            )
        invocation_fault_keys.add(invocation_fault_key)
        exact_inventory_fields = {
            "site_id": inventory_entry.site_id,
            "ledger_site_sha256": (
                inventory_entry.ledger_site_sha256
            ),
            "relation": inventory_entry.relation,
        }
        for field_name, expected_value in (
            exact_inventory_fields.items()
        ):
            if _field(entry, field_name, entry_context) != expected_value:
                _fail(
                    entry_context,
                    f"{field_name} differs from the runtime inventory",
                )
        catalog_contract = _probe_contract(
            _field(entry, "probe_contract", entry_context),
            inventory_entry.relation,
            f"{entry_context}.probe_contract",
        )
        if catalog_contract != dict(
            inventory_entry.probe_contract
        ):
            _fail(
                entry_context,
                "probe_contract differs from the runtime inventory",
            )
        if _string(
            _field(entry, "postgres_severity", entry_context),
            f"{entry_context}.postgres_severity",
        ) != "error":
            _fail(
                entry_context,
                "runtime cases must never exercise FATAL or PANIC",
            )
        expected = _object(
            _field(entry, "expected", entry_context),
            f"{entry_context}.expected",
        )
        _require_exact_fields(
            expected,
            EXPECTED_RESULT_FIELDS,
            f"{entry_context}.expected",
        )
        expected_backend_outcome = _string(
            _field(
                expected,
                "backend_outcome",
                f"{entry_context}.expected",
            ),
            f"{entry_context}.expected.backend_outcome",
        )
        if expected_backend_outcome not in {
            "recoverable_error",
            "backend_exit",
        }:
            _fail(
                entry_context,
                "backend_outcome must be recoverable_error or backend_exit",
            )
        backend_exit = expected_backend_outcome == "backend_exit"
        expected_sentinels = tuple(
            sorted(
                _string_list(
                _field(
                    expected,
                    "destructor_sentinels",
                    f"{entry_context}.expected",
                ),
                f"{entry_context}.expected.destructor_sentinels",
                allow_empty=True,
                )
            )
        )
        if backend_exit and expected_sentinels:
            _fail(
                entry_context,
                "backend-exit cases cannot claim unobservable destructors",
            )
        if (
            not backend_exit
            and not expected_sentinels
            and site.classification != "lifetime_free_direct"
        ):
            _fail(
                entry_context,
                "only lifetime_free_direct may use no C++ destructor sentinel",
            )
        if (
            not backend_exit
            and expected_sentinels
            and site.classification == "lifetime_free_direct"
        ):
            _fail(
                entry_context,
                "lifetime_free_direct must preserve an empty C++ lifetime set",
            )
        expected_cleanup_events = tuple(
            sorted(
                _string_list(
                _field(
                    expected,
                    "cleanup_events",
                    f"{entry_context}.expected",
                ),
                f"{entry_context}.expected.cleanup_events",
                allow_empty=True,
                )
            )
        )
        expected_postgres_cleanup_events = tuple(
            sorted(
                _string_list(
                _field(
                    expected,
                    "postgres_cleanup_events",
                    f"{entry_context}.expected",
                ),
                f"{entry_context}.expected.postgres_cleanup_events",
                allow_empty=(
                    backend_exit
                    or fault_key
                    == BACKEND_SCOPED_VACUUM_FAULT_KEY
                ),
                )
            )
        )
        if backend_exit:
            if expected_cleanup_events or expected_postgres_cleanup_events:
                _fail(
                    entry_context,
                    "backend-exit cases cannot claim unobservable cleanup",
                )
        elif fault_key == BACKEND_SCOPED_VACUUM_FAULT_KEY:
            if expected_postgres_cleanup_events:
                _fail(
                    entry_context,
                    "backend-scoped VACUUM must expect no FMGR cleanup",
                )
        elif "fmgr_abort_observed" not in (
            expected_postgres_cleanup_events
        ):
            _fail(
                entry_context,
                "PostgreSQL cleanup evidence must include FMGR ABORT",
            )
        raw_expected_sqlstate = _field(
            expected, "sqlstate", f"{entry_context}.expected"
        )
        raw_expected_message = _field(
            expected, "message", f"{entry_context}.expected"
        )
        if backend_exit:
            if (
                raw_expected_sqlstate is not None
                or raw_expected_message is not None
            ):
                _fail(
                    entry_context,
                    "backend-exit SQLSTATE/message must be null because the "
                    "backend-local error result is unobservable",
                )
            expected_sqlstate = None
            expected_message = None
        else:
            expected_sqlstate = _string(
                raw_expected_sqlstate,
                f"{entry_context}.expected.sqlstate",
            )
            if SQLSTATE_PATTERN.fullmatch(expected_sqlstate) is None:
                _fail(entry_context, "expected SQLSTATE is not five characters")
            expected_message = _string(
                raw_expected_message,
                f"{entry_context}.expected.message",
            )
        expected_recovery = _string(
            _field(
                expected,
                "recovery_postcondition",
                f"{entry_context}.expected",
            ),
            f"{entry_context}.expected.recovery_postcondition",
        )
        cases.append(
            CatalogCase(
                case_id=case_id,
                profile=profile,
                sql_identity=sql_identity,
                site=site,
                site_id=inventory_entry.site_id,
                ledger_site_sha256=(
                    inventory_entry.ledger_site_sha256
                ),
                invocation_key=invocation_key,
                fault_key=fault_key,
                relation=inventory_entry.relation,
                probe_contract=dict(
                    inventory_entry.probe_contract
                ),
                expected_backend_outcome=expected_backend_outcome,
                expected_destructor_sentinels=expected_sentinels,
                expected_cleanup_events=expected_cleanup_events,
                expected_postgres_cleanup_events=(
                    expected_postgres_cleanup_events
                ),
                expected_sqlstate=expected_sqlstate,
                expected_message=expected_message,
                expected_recovery_postcondition=expected_recovery,
                catalog_entry_sha256=_sha256_bytes(
                    _canonical_json(entry)
                ),
            )
        )
        observed_coverage.update(site.coverage(profile))
    if len(cases) != expected_case_count:
        _fail(
            context,
            "compiled case catalog cardinality differs",
        )
    if case_ids != set(case_plan_entries):
        _fail(context, "catalog case set differs from the current case plan")
    profile_required = {key for key in required if key[0] == profile}
    if observed_coverage != profile_required:
        missing = sorted(profile_required - observed_coverage)
        extra = sorted(observed_coverage - profile_required)
        _fail(
            context,
            "compiled cases do not close exact profile coverage; "
            + "missing="
            + ",".join("/".join(key) for key in missing)
            + " extra="
            + ",".join("/".join(key) for key in extra),
        )
    return tuple(sorted(cases, key=lambda case: case.case_id)), _sha256_bytes(raw)


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
class ClusterAuthoritySnapshot:
    profile: str
    cluster_system_identifier: str
    data_directory: Path
    terminal_marker_directory: Path
    server_log_path: Path
    socket_directory: Path
    connection_port: str
    connection_user: str
    original_authorization_path: Path
    authorization_raw: bytes
    authorization_mode: int
    authorization_uid: int
    authorization_sha256: str


@dataclass(frozen=True)
class PublishedClusterAuthority:
    profile: str
    record_path: Path
    record_sha256: str
    authorization_artifact_path: Path
    authorization_artifact_sha256: str
    snapshot: ClusterAuthoritySnapshot


@dataclass(frozen=True)
class ProfileInputs:
    profile: str
    ledger: Ledger
    runtime_inventory: RuntimeInventory
    runtime_inventory_path: Path
    runtime_inventory_sha256: str
    sql_manifest_path: Path
    sql_manifest_sha256: str
    sql_script_sha256: str
    case_plan_path: Path
    case_plan_sha256: str
    case_plan_runtime_policy_sha256: str
    case_plan_entries: Mapping[str, Mapping[str, Any]]
    dso_path: Path
    dso_sha256: str
    runtime_control_dso_path: Path
    runtime_control_dso_sha256: str
    test_executable_path: Path
    test_executable_sha256: str
    cluster_config_path: Path
    cluster_config_sha256: str
    stage_manifest_path: Path
    stage_manifest_sha256: str
    tool_signature_suite_sha256: str
    stage: StageManifestBinding
    cluster_authority: ClusterAuthoritySnapshot


@dataclass(frozen=True)
class ActivationReceipt:
    profile: str
    path: Path
    sha256: str
    active_dso_path: Path
    active_dso_sha256: str
    cluster_system_identifier: str
    cluster_data_directory: Path
    postmaster_start_time: str
    activation_nonce: str
    server_version_num: int
    extension_oid: int
    guc_properties: tuple[Mapping[str, Any], ...]
    guc_properties_sha256: str
    control_dso_path: Path
    control_dso_sha256: str


@dataclass(frozen=True)
class ChildResult:
    command: tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str


def _run_child(
    command: Sequence[str],
    *,
    timeout_seconds: int,
    context: str,
    cwd: Path,
) -> ChildResult:
    environment = dict(os.environ)
    environment["BIOSEQ_SAFETY_RUNTIME_PROTOCOL"] = "2"
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    try:
        process = subprocess.Popen(
            list(command),
            cwd=cwd,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
    except OSError as error:
        raise EvidenceError(
            f"{context}: cannot execute test artifact: {error}"
        ) from error
    try:
        stdout, stderr = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired as error:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                stdout, stderr = process.communicate(timeout=5)
            except subprocess.TimeoutExpired as kill_error:
                raise EvidenceError(
                    f"{context}: timed-out process group did not terminate"
                ) from kill_error
        else:
            try:
                os.killpg(process.pid, 0)
            except ProcessLookupError:
                pass
            else:
                os.killpg(process.pid, signal.SIGKILL)
        raise EvidenceError(
            f"{context}: test artifact timed out after "
            f"{timeout_seconds}s; process group terminated\n"
            f"stdout:\n{stdout}\nstderr:\n{stderr}"
        ) from error
    result = ChildResult(
        command=tuple(command),
        returncode=process.returncode,
        stdout=stdout,
        stderr=stderr,
    )
    if result.returncode != 0:
        _fail(
            context,
            f"test executable exited {result.returncode}\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}",
        )
    return result


def _write_if_different(path: Path, raw: bytes) -> None:
    _publish_file(path, raw, replace_existing=False)


def _replace_generated(path: Path, raw: bytes) -> None:
    _publish_file(path, raw, replace_existing=True)


def _paths_alias(left: Path, right: Path) -> bool:
    left_absolute = _absolute_path(left)
    right_absolute = _absolute_path(right)
    if left_absolute == right_absolute:
        return True
    if not left_absolute.exists() or not right_absolute.exists():
        return False
    try:
        return os.path.samefile(left_absolute, right_absolute)
    except OSError as error:
        raise EvidenceError(
            "generated-output: cannot compare "
            f"{left_absolute} and {right_absolute}: {error}"
        ) from error


def _validate_generated_output_paths(
    outputs: Mapping[str, Path],
    protected_inputs: Mapping[str, Path],
) -> None:
    output_items = list(outputs.items())
    for index, (label, path) in enumerate(output_items):
        absolute = _absolute_path(path)
        parent_descriptor = _open_directory(
            absolute.parent,
            f"{label}.parent",
        )
        os.close(parent_descriptor)
        try:
            absolute.lstat()
        except FileNotFoundError:
            pass
        except OSError as error:
            raise EvidenceError(
                f"{label}: cannot inspect generated output {absolute}: "
                f"{error}"
            ) from error
        else:
            _read_single_link_file(absolute, label)
        for other_label, other_path in output_items[index + 1 :]:
            if _paths_alias(path, other_path):
                _fail(
                    "generated-output",
                    f"{label} and {other_label} resolve to the same artifact",
                )
        for input_label, input_path in protected_inputs.items():
            if _paths_alias(path, input_path):
                _fail(
                    label,
                    "generated output aliases protected input "
                    f"{input_label}",
                )


def _write_immutable(
    directory: Path,
    stem: str,
    suffix: str,
    raw: bytes,
    *,
    artifact_root: Path,
) -> tuple[Path, str]:
    directory = _secure_descendant_directory(
        artifact_root,
        directory,
        f"immutable-{stem}",
    )
    digest = _sha256_bytes(raw)
    path = directory / f"{stem}.{digest}.{suffix}"
    _write_if_different(path, raw)
    if _sha256_bytes(_read_single_link_file(path, str(path))) != digest:
        _fail(str(path), "immutable artifact changed after publication")
    return _absolute_path(path), digest


def _child_log(
    *,
    result: ChildResult,
    context: str,
    bindings: Mapping[str, Any],
) -> bytes:
    return _canonical_json(
        {
            "schema_revision": 1,
            "context": context,
            "command": list(result.command),
            "returncode": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
            "bindings": dict(bindings),
        }
    )


def _activate_profile(
    inputs: ProfileInputs,
    *,
    work_directory: Path,
    artifact_directory: Path,
    timeout_seconds: int,
) -> tuple[ActivationReceipt, Path, str]:
    receipt_output = work_directory / f"{inputs.profile}.activation.json"
    command = (
        str(inputs.test_executable_path),
        "--bioseq-safety-runtime-activate",
        "--profile",
        inputs.profile,
        "--cluster-config",
        str(inputs.cluster_config_path),
        "--dso",
        str(inputs.dso_path),
        "--output",
        str(receipt_output),
    )
    child = _run_child(
        command,
        timeout_seconds=timeout_seconds,
        context=f"{inputs.profile}-activation",
        cwd=work_directory,
    )
    receipt, receipt_raw = _load_json(
        receipt_output,
        f"{inputs.profile}-activation-receipt",
    )
    context = f"{inputs.profile}-activation-receipt"
    _require_exact_fields(receipt, ACTIVATION_RECEIPT_FIELDS, context)
    sql_manifest, sql_manifest_raw = _load_json(
        inputs.sql_manifest_path,
        f"{context}.sql_manifest",
    )
    if _sha256_bytes(sql_manifest_raw) != inputs.sql_manifest_sha256:
        _fail(context, "SQL manifest changed before activation validation")
    guc_generation = _validate_guc_manifest_generation(
        sql_manifest,
        profile=inputs.profile,
        context=f"{context}.sql_manifest",
    )
    if _integer(
        _field(receipt, "schema_revision", context),
        f"{context}.schema_revision",
    ) != 1:
        _fail(context, "unsupported schema revision")
    if _string(
        _field(receipt, "profile", context),
        f"{context}.profile",
    ) != inputs.profile:
        _fail(context, "receipt profile differs from requested profile")
    if _sha256(
        _field(receipt, "test_executable_sha256", context),
        f"{context}.test_executable_sha256",
    ) != inputs.test_executable_sha256:
        _fail(context, "receipt names a different test executable")
    if _sha256(
        _field(receipt, "requested_dso_sha256", context),
        f"{context}.requested_dso_sha256",
    ) != inputs.dso_sha256:
        _fail(context, "receipt names a different requested DSO")
    if _sha256(
        _field(receipt, "cluster_config_sha256", context),
        f"{context}.cluster_config_sha256",
    ) != inputs.cluster_config_sha256:
        _fail(context, "receipt names a different cluster configuration")
    if _sha256(
        _field(receipt, "case_plan_sha256", context),
        f"{context}.case_plan_sha256",
    ) != inputs.case_plan_sha256:
        _fail(context, "receipt names a different compiled case plan")
    if _sha256(
        _field(receipt, "sql_manifest_sha256", context),
        f"{context}.sql_manifest_sha256",
    ) != inputs.sql_manifest_sha256:
        _fail(context, "receipt names a different SQL manifest")
    if (
        _sha256(
            _field(receipt, "sql_script_sha256", context),
            f"{context}.sql_script_sha256",
        )
        != inputs.sql_script_sha256
        or _sha256(
            _field(receipt, "installed_sql_sha256", context),
            f"{context}.installed_sql_sha256",
        )
        != inputs.sql_script_sha256
    ):
        _fail(context, "installed SQL differs from the manifest script")
    expected_guc_count = GUC_PROFILE_COUNTS[inputs.profile]
    expected_test_only_count = (
        0
        if inputs.profile == "production"
        else len(GUC_TEST_INDEXES_ONLY_NAMES)
    )
    if (
        _integer(
            _field(receipt, "bioseq_guc_count", context),
            f"{context}.bioseq_guc_count",
        )
        != expected_guc_count
        or _integer(
            _field(receipt, "bioseq_test_only_guc_count", context),
            f"{context}.bioseq_test_only_guc_count",
        )
        != expected_test_only_count
    ):
        _fail(context, "GUC profile count differs")
    guc_properties, guc_properties_sha256 = _validate_guc_properties(
        _field(receipt, "bioseq_guc_properties", context),
        _field(receipt, "bioseq_guc_properties_sha256", context),
        generation=guc_generation,
        profile=inputs.profile,
        context=f"{context}.bioseq_guc_properties",
    )
    if (
        _sha256(
            _field(receipt, "installed_control_sha256", context),
            f"{context}.installed_control_sha256",
        )
        != inputs.stage.control_sha256
    ):
        _fail(context, "installed extension control file differs")
    active_dso_path = Path(
        _string(
            _field(receipt, "active_dso_path", context),
            f"{context}.active_dso_path",
        )
    )
    if (
        not active_dso_path.is_absolute()
        or not active_dso_path.is_file()
        or active_dso_path.is_symlink()
    ):
        _fail(
            context,
            "active DSO path must be an existing absolute non-symlink file",
        )
    independently_hashed_dso = _sha256_file(active_dso_path, context)
    active_dso_sha256 = _sha256(
        _field(receipt, "active_dso_sha256", context),
        f"{context}.active_dso_sha256",
    )
    if (
        active_dso_sha256 != inputs.dso_sha256
        or independently_hashed_dso != inputs.dso_sha256
    ):
        _fail(context, "active installed DSO bytes differ from requested DSO")
    control_dso_path = Path(
        _string(
            _field(receipt, "control_dso_path", context),
            f"{context}.control_dso_path",
        )
    )
    control_dso_sha256 = _sha256(
        _field(receipt, "control_dso_sha256", context),
        f"{context}.control_dso_sha256",
    )
    if (
        not control_dso_path.is_absolute()
        or not control_dso_path.is_file()
        or control_dso_path.is_symlink()
        or _sha256_file(control_dso_path, context)
            != control_dso_sha256
        or control_dso_sha256
            != inputs.runtime_control_dso_sha256
        or control_dso_path.resolve() == active_dso_path.resolve()
        or control_dso_sha256 == active_dso_sha256
    ):
        _fail(
            context,
            "runtime control helper is absent, stale, or not a separate DSO",
        )
    server_version_num = _postgresql_version_num(
        _field(receipt, "server_version_num", context),
        f"{context}.server_version_num",
    )
    cluster_data_directory = Path(
        _string(
            _field(receipt, "cluster_data_directory", context),
            f"{context}.cluster_data_directory",
        )
    )
    if not cluster_data_directory.is_absolute():
        _fail(context, "cluster data directory must be absolute")
    extension_oid = _integer(
        _field(receipt, "extension_oid", context),
        f"{context}.extension_oid",
    )
    if extension_oid <= 0:
        _fail(context, "extension OID must be positive")
    immutable_receipt_path, receipt_sha256 = _write_immutable(
        artifact_directory / "activations" / inputs.profile,
        "activation",
        "json",
        receipt_raw,
        artifact_root=artifact_directory,
    )
    log_path, log_sha256 = _write_immutable(
        artifact_directory / "activations" / inputs.profile,
        "activation",
        "log.json",
        _child_log(
            result=child,
            context=f"{inputs.profile}-activation",
            bindings={
                "activation_receipt_sha256": receipt_sha256,
                "requested_dso_sha256": inputs.dso_sha256,
                "control_dso_sha256": control_dso_sha256,
                "case_plan_sha256": inputs.case_plan_sha256,
                "sql_manifest_sha256": inputs.sql_manifest_sha256,
                "sql_script_sha256": inputs.sql_script_sha256,
                "test_executable_sha256": inputs.test_executable_sha256,
                "cluster_config_sha256": inputs.cluster_config_sha256,
            },
        ),
        artifact_root=artifact_directory,
    )
    activation = ActivationReceipt(
        profile=inputs.profile,
        path=immutable_receipt_path,
        sha256=receipt_sha256,
        active_dso_path=active_dso_path.resolve(),
        active_dso_sha256=active_dso_sha256,
        cluster_system_identifier=_string(
            _field(receipt, "cluster_system_identifier", context),
            f"{context}.cluster_system_identifier",
        ),
        cluster_data_directory=cluster_data_directory.resolve(),
        postmaster_start_time=_string(
            _field(receipt, "postmaster_start_time", context),
            f"{context}.postmaster_start_time",
        ),
        activation_nonce=_string(
            _field(receipt, "activation_nonce", context),
            f"{context}.activation_nonce",
        ),
        server_version_num=server_version_num,
        extension_oid=extension_oid,
        guc_properties=guc_properties,
        guc_properties_sha256=guc_properties_sha256,
        control_dso_path=control_dso_path.resolve(),
        control_dso_sha256=control_dso_sha256,
    )
    return activation, log_path, log_sha256


def _describe_profile(
    inputs: ProfileInputs,
    *,
    required: frozenset[CoverageKey],
    work_directory: Path,
    artifact_directory: Path,
    timeout_seconds: int,
) -> tuple[tuple[CatalogCase, ...], Path, str, Path, str]:
    catalog_output = work_directory / f"{inputs.profile}.catalog.json"
    command = (
        str(inputs.test_executable_path),
        "--bioseq-safety-runtime-describe",
        "--profile",
        inputs.profile,
        "--output",
        str(catalog_output),
    )
    child = _run_child(
        command,
        timeout_seconds=timeout_seconds,
        context=f"{inputs.profile}-catalog",
        cwd=work_directory,
    )
    cases, catalog_sha256 = _parse_catalog(
        catalog_output,
        profile=inputs.profile,
        ledger=inputs.ledger,
        inventory=inputs.runtime_inventory,
        test_executable_sha256=inputs.test_executable_sha256,
        case_plan_sha256=inputs.case_plan_sha256,
        case_plan_entries=inputs.case_plan_entries,
        required=required,
    )
    catalog_raw = catalog_output.read_bytes()
    immutable_catalog, published_catalog_sha256 = _write_immutable(
        artifact_directory / "catalogs" / inputs.profile,
        "case_catalog",
        "json",
        catalog_raw,
        artifact_root=artifact_directory,
    )
    if catalog_sha256 != published_catalog_sha256:
        _fail(f"{inputs.profile}-catalog", "catalog changed while publishing")
    log_path, log_sha256 = _write_immutable(
        artifact_directory / "catalogs" / inputs.profile,
        "case_catalog",
        "log.json",
        _child_log(
            result=child,
            context=f"{inputs.profile}-catalog",
            bindings={
                "catalog_sha256": catalog_sha256,
                "ledger_sha256": inputs.ledger.sha256,
                "test_executable_sha256": inputs.test_executable_sha256,
            },
        ),
        artifact_root=artifact_directory,
    )
    return cases, immutable_catalog, catalog_sha256, log_path, log_sha256


def _validate_cluster_relationship(
    production: ActivationReceipt,
    test_indexes: ActivationReceipt,
    mode: str,
) -> None:
    if production.active_dso_sha256 == test_indexes.active_dso_sha256:
        _fail(
            "profile-artifacts",
            "production and test-indexes activated the same DSO bytes",
        )
    if production.control_dso_sha256 != test_indexes.control_dso_sha256:
        _fail(
            "profile-artifacts",
            "both profiles must use the same reviewed runtime control DSO",
        )
    if mode != "isolated":
        _fail(
            "cluster-mode",
            "runtime evidence requires two isolated private clusters",
        )
    if (
        production.cluster_system_identifier
            == test_indexes.cluster_system_identifier
        or production.cluster_data_directory
            == test_indexes.cluster_data_directory
    ):
        _fail(
            "cluster-mode",
            "profiles require distinct PostgreSQL systems and data "
            "directories",
        )
    production_rows = {
        _field(row, "name", "production GUC property"): row
        for row in production.guc_properties
    }
    test_rows = {
        _field(row, "name", "test-indexes GUC property"): row
        for row in test_indexes.guc_properties
    }
    expected_extra = set(GUC_TEST_INDEXES_ONLY_NAMES)
    if (
        len(production_rows) != GUC_PROFILE_COUNTS["production"]
        or len(test_rows) != GUC_PROFILE_COUNTS["test_indexes"]
        or set(production_rows) - set(test_rows)
        or set(test_rows) - set(production_rows) != expected_extra
    ):
        _fail(
            "profile-artifacts",
            "GUC profiles do not have the exact current-profile delta",
        )
    for name, production_row in production_rows.items():
        production_semantic = {
            field: value
            for field, value in production_row.items()
            if field != "profile_projection"
        }
        test_semantic = {
            field: value
            for field, value in test_rows[name].items()
            if field != "profile_projection"
        }
        if production_semantic != test_semantic:
            _fail(
                "profile-artifacts",
                f"common GUC property differs across profiles: {name}",
            )


def _observed_event_names(
    value: Any,
    context: str,
) -> list[str]:
    events = _array(value, context)
    observed: set[str] = set()
    for index, raw_event in enumerate(events):
        event_context = f"{context}[{index}]"
        event = _object(raw_event, event_context)
        _require_exact_fields(
            event,
            RUNTIME_EVENT_FIELDS,
            event_context,
        )
        name = _string(
            _field(event, "name", event_context),
            f"{event_context}.name",
        )
        count = _integer(
            _field(event, "count", event_context),
            f"{event_context}.count",
        )
        if count != 1 or name in observed:
            _fail(
                event_context,
                "each runtime event must be observed exactly once",
            )
        observed.add(name)
    return sorted(observed)


def _row_matrix(value: Any, context: str) -> list[list[Any]]:
    rows = _array(value, context)
    if not rows or len(rows) > 64:
        _fail(context, "expected 1..64 recovery rows")
    expected_columns: int | None = None
    result: list[list[Any]] = []
    for row_index, raw_row in enumerate(rows):
        row_context = f"{context}[{row_index}]"
        row = _array(raw_row, row_context)
        if (
            not row
            or len(row) > 64
            or (
                expected_columns is not None
                and len(row) != expected_columns
            )
        ):
            _fail(context, "recovery rows must be a bounded rectangle")
        expected_columns = len(row)
        for column_index, cell in enumerate(row):
            if cell is not None and not isinstance(cell, str):
                _fail(
                    f"{row_context}[{column_index}]",
                    "recovery cells must be strings or null",
                )
        result.append(list(row))
    return result


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
    ) != _sha256_bytes(expected_payload.encode("utf-8")):
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
        != _sha256_bytes(interval_raw)
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


def _validate_raw_case_result(
    raw_result: Mapping[str, Any],
    *,
    case: CatalogCase,
    inputs: ProfileInputs,
    activation: ActivationReceipt,
    producer_executable_sha256: str,
    request_sha256: str,
    catalog_sha256: str,
) -> tuple[
    list[str],
    list[str],
    list[str],
    str | None,
    str | None,
    str,
    dict[str, Any],
]:
    context = f"{case.profile}/{case.case_id}-raw-result"
    _require_exact_fields(raw_result, RAW_RESULT_FIELDS, context)
    if _integer(
        _field(raw_result, "schema_revision", context),
        f"{context}.schema_revision",
    ) != 2:
        _fail(context, "unsupported schema revision")
    if _integer(
        _field(raw_result, "probe_abi_revision", context),
        f"{context}.probe_abi_revision",
    ) != 2:
        _fail(context, "unsupported runtime probe ABI")
    exact_fields: dict[str, Any] = {
        "profile": case.profile,
        "case_id": case.case_id,
        "sql_identity": case.sql_identity,
        "site_id": case.site_id,
        "ledger_site_sha256": case.ledger_site_sha256,
        "invocation_key": case.invocation_key,
        "fault_key": case.fault_key,
        "relation": case.relation,
        "case_plan_sha256": inputs.case_plan_sha256,
        "ledger_sha256": inputs.ledger.sha256,
        "sql_manifest_sha256": inputs.sql_manifest_sha256,
        "active_dso_sha256": inputs.dso_sha256,
        "test_executable_sha256": inputs.test_executable_sha256,
        "producer_executable_sha256": producer_executable_sha256,
        "activation_receipt_sha256": activation.sha256,
        "bioseq_guc_properties_sha256": (
            activation.guc_properties_sha256
        ),
        "cluster_config_sha256": inputs.cluster_config_sha256,
        "request_sha256": request_sha256,
        "catalog_sha256": catalog_sha256,
        "catalog_entry_sha256": case.catalog_entry_sha256,
        "execution_backend": "postgresql",
    }
    for field_name, expected in exact_fields.items():
        if _field(raw_result, field_name, context) != expected:
            _fail(context, f"{field_name} differs from the executed request")
    if (
        _field(raw_result, "bioseq_guc_properties", context)
        != list(activation.guc_properties)
    ):
        _fail(context, "GUC properties differ from activation")
    if _probe_contract(
        _field(raw_result, "probe_contract", context),
        case.relation,
        f"{context}.probe_contract",
    ) != dict(case.probe_contract):
        _fail(context, "probe_contract differs from the executed request")
    if _boolean(
        _field(raw_result, "exercised_terminal_severity", context),
        f"{context}.exercised_terminal_severity",
    ):
        _fail(context, "runtime tests must never exercise FATAL or PANIC")

    backend_outcome = _object(
        _field(raw_result, "backend_outcome", context),
        f"{context}.backend_outcome",
    )
    _require_exact_fields(
        backend_outcome,
        BACKEND_OUTCOME_FIELDS,
        f"{context}.backend_outcome",
    )
    expected_backend_outcome = _string(
        _field(
            backend_outcome,
            "expected",
            f"{context}.backend_outcome",
        ),
        f"{context}.backend_outcome.expected",
    )
    observed_backend_outcome = _string(
        _field(
            backend_outcome,
            "observed",
            f"{context}.backend_outcome",
        ),
        f"{context}.backend_outcome.observed",
    )
    initial_backend_pid = _integer(
        _field(
            backend_outcome,
            "initial_backend_pid",
            f"{context}.backend_outcome",
        ),
        f"{context}.backend_outcome.initial_backend_pid",
    )
    recovery_backend_pid = _integer(
        _field(
            backend_outcome,
            "recovery_backend_pid",
            f"{context}.backend_outcome",
        ),
        f"{context}.backend_outcome.recovery_backend_pid",
    )
    initial_connection_status = _string(
        _field(
            backend_outcome,
            "initial_connection_status",
            f"{context}.backend_outcome",
        ),
        f"{context}.backend_outcome.initial_connection_status",
    )
    post_invocation_connection_status = _string(
        _field(
            backend_outcome,
            "post_invocation_connection_status",
            f"{context}.backend_outcome",
        ),
        f"{context}.backend_outcome.post_invocation_connection_status",
    )
    invocation_result_status = _string(
        _field(
            backend_outcome,
            "invocation_result_status",
            f"{context}.backend_outcome",
        ),
        f"{context}.backend_outcome.invocation_result_status",
    )
    terminal_guard_observed = _boolean(
        _field(
            backend_outcome,
            "terminal_guard_observed",
            f"{context}.backend_outcome",
        ),
        f"{context}.backend_outcome.terminal_guard_observed",
    )
    backend_exit = case.expected_backend_outcome == "backend_exit"
    terminal_causality = _validate_terminal_causality(
        _field(raw_result, "terminal_causality", context),
        backend_exit=backend_exit,
        site_id=case.site_id,
        fault_key=case.fault_key,
        backend_pid=initial_backend_pid,
        marker_directory=(
            inputs.cluster_authority.terminal_marker_directory
        ),
        server_log_path=inputs.cluster_authority.server_log_path,
        marker_uid=inputs.cluster_authority.authorization_uid,
        context=f"{context}.terminal_causality",
    )
    if terminal_guard_observed != (terminal_causality is not None):
        _fail(
            context,
            "terminal_guard_observed is not derived from terminal causality",
        )
    if (
        expected_backend_outcome != case.expected_backend_outcome
        or observed_backend_outcome != case.expected_backend_outcome
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
        _fail(
            context,
            "backend outcome does not prove the expected connection lifecycle",
        )

    observed_sentinels = _observed_event_names(
        _field(raw_result, "destructor_events", context),
        f"{context}.destructor_events",
    )
    if observed_sentinels != list(case.expected_destructor_sentinels):
        _fail(context, "destructor sentinel set differs from expected")
    observed_cleanup_events = _observed_event_names(
        _field(raw_result, "cleanup_events", context),
        f"{context}.cleanup_events",
    )
    if observed_cleanup_events != list(case.expected_cleanup_events):
        _fail(context, "cleanup-completion event set differs from expected")
    observed_postgres_cleanup_events = _observed_event_names(
        _field(raw_result, "postgres_cleanup_events", context),
        f"{context}.postgres_cleanup_events",
    )
    if (
        observed_postgres_cleanup_events
        != list(case.expected_postgres_cleanup_events)
    ):
        _fail(context, "PostgreSQL cleanup event set differs from expected")

    postgres_error = _object(
        _field(raw_result, "postgres_error", context),
        f"{context}.postgres_error",
    )
    _require_exact_fields(
        postgres_error,
        POSTGRES_ERROR_FIELDS,
        f"{context}.postgres_error",
    )
    if backend_exit:
        if postgres_error != {
            "severity": None,
            "sqlstate": None,
            "message": None,
        }:
            _fail(
                context,
                "backend-exit case fabricated an unobservable PostgreSQL error",
            )
        observed_sqlstate = None
        observed_message = None
    else:
        if _string(
            _field(postgres_error, "severity", f"{context}.postgres_error"),
            f"{context}.postgres_error.severity",
        ) != "ERROR":
            _fail(
                context,
                "runtime result is not a recoverable PostgreSQL ERROR",
            )
        observed_sqlstate = _string(
            _field(postgres_error, "sqlstate", f"{context}.postgres_error"),
            f"{context}.postgres_error.sqlstate",
        )
        observed_message = _string(
            _field(postgres_error, "message", f"{context}.postgres_error"),
            f"{context}.postgres_error.message",
        )
        if (
            observed_sqlstate != case.expected_sqlstate
            or observed_message != case.expected_message
        ):
            _fail(context, "SQLSTATE or PostgreSQL message was not preserved")

    fault_evidence = _object(
        _field(raw_result, "fault_evidence", context),
        f"{context}.fault_evidence",
    )
    _require_exact_fields(
        fault_evidence,
        FAULT_EVIDENCE_FIELDS,
        f"{context}.fault_evidence",
    )
    observation_mode = _string(
        _field(
            fault_evidence,
            "observation_mode",
            f"{context}.fault_evidence",
        ),
        f"{context}.fault_evidence.observation_mode",
    )
    expected_observation_mode = (
        "backend_exit" if backend_exit else "backend_snapshot"
    )
    if observation_mode != expected_observation_mode:
        _fail(
            context,
            "fault evidence observation mode differs from backend outcome",
        )
    fault_exact_fields: dict[str, Any] = {
        "probe_abi_revision": 2,
        "site_id": case.site_id,
        "fault_key": case.fault_key,
        "relation": case.relation,
    }
    for field_name, expected in fault_exact_fields.items():
        if _field(
            fault_evidence,
            field_name,
            f"{context}.fault_evidence",
        ) != expected:
            _fail(
                context,
                f"fault evidence {field_name} differs from inventory",
            )
    if _probe_contract(
        _field(
            fault_evidence,
            "probe_contract",
            f"{context}.fault_evidence",
        ),
        case.relation,
        f"{context}.fault_evidence.probe_contract",
    ) != dict(case.probe_contract):
        _fail(
            context,
            "fault evidence probe_contract differs from inventory",
        )
    invocation_count = _integer(
        _field(
            fault_evidence,
            "invocation_count",
            f"{context}.fault_evidence",
        ),
        f"{context}.fault_evidence.invocation_count",
    )
    site_visit_count = _integer(
        _field(
            fault_evidence,
            "site_visit_count",
            f"{context}.fault_evidence",
        ),
        f"{context}.fault_evidence.site_visit_count",
    )
    fault_trigger_count = _integer(
        _field(
            fault_evidence,
            "fault_trigger_count",
            f"{context}.fault_evidence",
        ),
        f"{context}.fault_evidence.fault_trigger_count",
    )
    site_visit_sequence = _integer(
        _field(
            fault_evidence,
            "site_visit_sequence",
            f"{context}.fault_evidence",
        ),
        f"{context}.fault_evidence.site_visit_sequence",
    )
    fault_trigger_sequence = _integer(
        _field(
            fault_evidence,
            "fault_trigger_sequence",
            f"{context}.fault_evidence",
        ),
        f"{context}.fault_evidence.fault_trigger_sequence",
    )
    fmgr_start_count = _integer(
        _field(
            fault_evidence,
            "fmgr_start_count",
            f"{context}.fault_evidence",
        ),
        f"{context}.fault_evidence.fmgr_start_count",
    )
    fmgr_end_count = _integer(
        _field(
            fault_evidence,
            "fmgr_end_count",
            f"{context}.fault_evidence",
        ),
        f"{context}.fault_evidence.fmgr_end_count",
    )
    fmgr_abort_count = _integer(
        _field(
            fault_evidence,
            "fmgr_abort_count",
            f"{context}.fault_evidence",
        ),
        f"{context}.fault_evidence.fmgr_abort_count",
    )
    invalid_count = _integer(
        _field(
            fault_evidence,
            "invalid_count",
            f"{context}.fault_evidence",
        ),
        f"{context}.fault_evidence.invalid_count",
    )
    overflow_count = _integer(
        _field(
            fault_evidence,
            "overflow_count",
            f"{context}.fault_evidence",
        ),
        f"{context}.fault_evidence.overflow_count",
    )
    inside_target = _boolean(
        _field(
            fault_evidence,
            "inside_target",
            f"{context}.fault_evidence",
        ),
        f"{context}.fault_evidence.inside_target",
    )
    backend_scoped_case = (
        case.fault_key == BACKEND_SCOPED_VACUUM_FAULT_KEY
    )
    expected_fmgr_start_count = (
        0 if backend_exit or backend_scoped_case else 1
    )
    expected_fmgr_abort_count = (
        0 if backend_exit or backend_scoped_case else 1
    )
    if (
        invocation_count != expected_fmgr_start_count
        or fmgr_start_count != expected_fmgr_start_count
        or fmgr_end_count != 0
        or fmgr_abort_count != expected_fmgr_abort_count
        or invalid_count != 0
        or overflow_count != 0
        or inside_target
    ):
        _fail(
            context,
            "FMGR/backend lifecycle or probe integrity counters differ",
        )
    expected_visit_count = (
        0
        if backend_exit
        else case.probe_contract["expected_visit_count"]
    )
    expected_trigger_count = (
        0
        if backend_exit
        else case.probe_contract["expected_trigger_count"]
    )
    ordering = case.probe_contract["ordering"]
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
    if (
        site_visit_count != expected_visit_count
        or fault_trigger_count != expected_trigger_count
        or site_visit_sequence != expected_visit_sequence
        or fault_trigger_sequence != expected_trigger_sequence
    ):
        _fail(
            context,
            "ABI revision 2 visit/trigger count or ordering differs",
        )

    recovery = _object(
        _field(raw_result, "recovery_probe", context),
        f"{context}.recovery_probe",
    )
    _require_exact_fields(
        recovery,
        RECOVERY_PROBE_FIELDS,
        f"{context}.recovery_probe",
    )
    observed_recovery = _string(
        _field(recovery, "postcondition", f"{context}.recovery_probe"),
        f"{context}.recovery_probe.postcondition",
    )
    expected_recovery_rows = _row_matrix(
        _field(
            recovery,
            "expected_rows",
            f"{context}.recovery_probe",
        ),
        f"{context}.recovery_probe.expected_rows",
    )
    observed_recovery_rows = _row_matrix(
        _field(
            recovery,
            "observed_rows",
            f"{context}.recovery_probe",
        ),
        f"{context}.recovery_probe.observed_rows",
    )
    if (
        not _boolean(
            _field(recovery, "passed", f"{context}.recovery_probe"),
            f"{context}.recovery_probe.passed",
        )
        or not _boolean(
            _field(
                recovery,
                "nonempty_result",
                f"{context}.recovery_probe",
            ),
            f"{context}.recovery_probe.nonempty_result",
        )
        or _integer(
            _field(
                recovery,
                "result_row_count",
                f"{context}.recovery_probe",
            ),
            f"{context}.recovery_probe.result_row_count",
        )
        != len(observed_recovery_rows)
        or _string(
            _field(
                recovery,
                "transaction_status",
                f"{context}.recovery_probe",
            ),
            f"{context}.recovery_probe.transaction_status",
        )
        != "idle"
        or expected_recovery_rows != observed_recovery_rows
        or not observed_recovery_rows
        or observed_recovery != case.expected_recovery_postcondition
    ):
        _fail(context, "declared recovery postcondition did not hold")

    cluster = _object(
        _field(raw_result, "cluster_evidence", context),
        f"{context}.cluster_evidence",
    )
    _require_exact_fields(
        cluster,
        CLUSTER_EVIDENCE_FIELDS,
        f"{context}.cluster_evidence",
    )
    cluster_exact_fields: dict[str, Any] = {
        "cluster_system_identifier": activation.cluster_system_identifier,
        "cluster_data_directory": str(activation.cluster_data_directory),
        "postmaster_start_time": activation.postmaster_start_time,
        "activation_nonce": activation.activation_nonce,
        "server_version_num": activation.server_version_num,
    }
    for field_name, expected in cluster_exact_fields.items():
        if _field(cluster, field_name, f"{context}.cluster_evidence") != expected:
            _fail(context, f"cluster evidence {field_name} is stale")
    backend_pid = _integer(
        _field(cluster, "backend_pid", f"{context}.cluster_evidence"),
        f"{context}.cluster_evidence.backend_pid",
    )
    cluster_initial_backend_pid = _integer(
        _field(
            cluster,
            "initial_backend_pid",
            f"{context}.cluster_evidence",
        ),
        f"{context}.cluster_evidence.initial_backend_pid",
    )
    cluster_recovery_backend_pid = _integer(
        _field(
            cluster,
            "recovery_backend_pid",
            f"{context}.cluster_evidence",
        ),
        f"{context}.cluster_evidence.recovery_backend_pid",
    )
    connection_replaced = _boolean(
        _field(
            cluster,
            "connection_replaced",
            f"{context}.cluster_evidence",
        ),
        f"{context}.cluster_evidence.connection_replaced",
    )
    if (
        backend_pid <= 0
        or backend_pid != initial_backend_pid
        or cluster_initial_backend_pid != initial_backend_pid
        or cluster_recovery_backend_pid != recovery_backend_pid
        or connection_replaced != backend_exit
    ):
        _fail(context, "cluster evidence has an invalid backend lifecycle")
    active_dso_path = Path(
        _string(
            _field(cluster, "active_dso_path", f"{context}.cluster_evidence"),
            f"{context}.cluster_evidence.active_dso_path",
        )
    )
    if (
        not active_dso_path.is_absolute()
        or active_dso_path.resolve() != activation.active_dso_path
        or _sha256_file(active_dso_path, context) != inputs.dso_sha256
    ):
        _fail(context, "runtime case did not use the activated DSO bytes")
    mapped_target_path = Path(
        _string(
            _field(
                cluster,
                "mapped_target_path",
                f"{context}.cluster_evidence",
            ),
            f"{context}.cluster_evidence.mapped_target_path",
        )
    )
    control_dso_path = Path(
        _string(
            _field(
                cluster,
                "control_dso_path",
                f"{context}.cluster_evidence",
            ),
            f"{context}.cluster_evidence.control_dso_path",
        )
    )
    if (
        not mapped_target_path.is_absolute()
        or mapped_target_path.resolve() != activation.active_dso_path
        or _sha256_file(mapped_target_path, context)
            != inputs.dso_sha256
        or not control_dso_path.is_absolute()
        or control_dso_path.resolve() != activation.control_dso_path
        or _sha256(
            _field(
                cluster,
                "control_dso_sha256",
                f"{context}.cluster_evidence",
            ),
            f"{context}.cluster_evidence.control_dso_sha256",
        )
        != activation.control_dso_sha256
        or _sha256_file(control_dso_path, context)
        != activation.control_dso_sha256
    ):
        _fail(
            context,
            "mapped target image or runtime control helper differs",
        )

    attestation = {
        "schema_revision": 1,
        "request_sha256": request_sha256,
        "catalog_sha256": catalog_sha256,
        "catalog_entry_sha256": case.catalog_entry_sha256,
        "activation_receipt_path": str(activation.path),
        "activation_receipt_sha256": activation.sha256,
        "cluster_config_sha256": inputs.cluster_config_sha256,
        "stage_manifest_sha256": inputs.stage_manifest_sha256,
        "tool_signature_suite_sha256": (
            inputs.tool_signature_suite_sha256
        ),
        "active_dso_path": str(activation.active_dso_path),
        "active_dso_sha256": activation.active_dso_sha256,
        "cluster_system_identifier": activation.cluster_system_identifier,
        "cluster_data_directory": str(activation.cluster_data_directory),
        "postmaster_start_time": activation.postmaster_start_time,
        "activation_nonce": activation.activation_nonce,
        "server_version_num": activation.server_version_num,
        "backend_pid": backend_pid,
        "expected_backend_outcome": case.expected_backend_outcome,
        "observed_backend_outcome": observed_backend_outcome,
        "probe_observation_mode": observation_mode,
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
        "site_id": case.site_id,
        "ledger_site_sha256": case.ledger_site_sha256,
        "fault_key": case.fault_key,
        "relation": case.relation,
        "probe_contract": dict(case.probe_contract),
        "fault_invocation_count": invocation_count,
        "fmgr_start_count": fmgr_start_count,
        "fmgr_end_count": fmgr_end_count,
        "fmgr_abort_count": fmgr_abort_count,
        "probe_invalid_count": invalid_count,
        "probe_overflow_count": overflow_count,
        "probe_inside_target": inside_target,
        "site_visit_count": site_visit_count,
        "fault_trigger_count": fault_trigger_count,
        "site_visit_sequence": site_visit_sequence,
        "fault_trigger_sequence": fault_trigger_sequence,
        "mapped_target_path": str(mapped_target_path.resolve()),
        "mapped_target_sha256": inputs.dso_sha256,
        "control_dso_path": str(control_dso_path.resolve()),
        "control_dso_sha256": activation.control_dso_sha256,
        "recovery_result_row_count": _integer(
            _field(
                recovery,
                "result_row_count",
                f"{context}.recovery_probe",
            ),
            f"{context}.recovery_probe.result_row_count",
        ),
        "recovery_rows_sha256": _sha256_bytes(
            _canonical_json(observed_recovery_rows)
        ),
    }
    return (
        observed_sentinels,
        observed_cleanup_events,
        observed_postgres_cleanup_events,
        observed_sqlstate,
        observed_message,
        observed_recovery,
        attestation,
    )


def _relative_artifact_path(path: Path, matrix_path: Path) -> str:
    root = matrix_path.parent.resolve()
    try:
        relative = path.resolve(strict=True).relative_to(root)
    except (OSError, ValueError) as error:
        raise EvidenceError(
            f"runtime-artifact: path escapes candidate root: {path}: "
            f"{error}"
        ) from error
    if not relative.parts or relative.parts[0] != "evidence":
        _fail("runtime-artifact", "path is outside candidate/evidence")
    return str(relative)


def _publish_cluster_authority(
    snapshot: ClusterAuthoritySnapshot,
    *,
    artifact_directory: Path,
    matrix_path: Path,
) -> PublishedClusterAuthority:
    context = f"{snapshot.profile}-cluster-authority"
    authorization_path, authorization_sha256 = _write_immutable(
        artifact_directory / "cluster_authorities" / snapshot.profile,
        "authorization",
        "marker",
        snapshot.authorization_raw,
        artifact_root=artifact_directory,
    )
    authorization_raw, authorization_metadata = (
        _read_single_link_file_with_metadata(
            authorization_path,
            f"{context}.authorization_artifact",
        )
    )
    if (
        authorization_raw != AUTHORIZATION_MARKER
        or authorization_sha256 != snapshot.authorization_sha256
        or stat.S_IMODE(authorization_metadata.st_mode) != 0o600
        or authorization_metadata.st_uid != os.getuid()
    ):
        _fail(context, "published authorization marker authority differs")
    record = {
        "schema_revision": 1,
        "profile": snapshot.profile,
        "cluster_system_identifier": (
            snapshot.cluster_system_identifier
        ),
        "data_directory": str(snapshot.data_directory),
        "socket_directory": str(snapshot.socket_directory),
        "connection_port": snapshot.connection_port,
        "connection_user": snapshot.connection_user,
        "original_authorization_path": str(
            snapshot.original_authorization_path
        ),
        "authorization_mode": snapshot.authorization_mode,
        "authorization_uid": snapshot.authorization_uid,
        "authorization_sha256": snapshot.authorization_sha256,
        "authorization_artifact_path": _relative_artifact_path(
            authorization_path,
            matrix_path,
        ),
        "authorization_artifact_sha256": authorization_sha256,
    }
    _require_exact_fields(record, CLUSTER_AUTHORITY_FIELDS, context)
    record_path, record_sha256 = _write_immutable(
        artifact_directory / "cluster_authorities" / snapshot.profile,
        "cluster_authority",
        "json",
        _canonical_json(record),
        artifact_root=artifact_directory,
    )
    return PublishedClusterAuthority(
        profile=snapshot.profile,
        record_path=record_path,
        record_sha256=record_sha256,
        authorization_artifact_path=authorization_path,
        authorization_artifact_sha256=authorization_sha256,
        snapshot=snapshot,
    )


def _run_case(
    *,
    case: CatalogCase,
    inputs: ProfileInputs,
    activation: ActivationReceipt,
    catalog_path: Path,
    catalog_sha256: str,
    producer_executable_path: Path,
    producer_executable_sha256: str,
    work_directory: Path,
    artifact_directory: Path,
    matrix_path: Path,
    timeout_seconds: int,
) -> dict[str, Any]:
    request = {
        "schema_revision": 2,
        "profile": case.profile,
        "case_id": case.case_id,
        "sql_identity": case.sql_identity,
        "site_id": case.site_id,
        "ledger_site_sha256": case.ledger_site_sha256,
        "invocation_key": case.invocation_key,
        "fault_key": case.fault_key,
        "relation": case.relation,
        "probe_contract": dict(case.probe_contract),
        "case_plan_path": str(inputs.case_plan_path),
        "case_plan_sha256": inputs.case_plan_sha256,
        "ledger_path": str(inputs.ledger.path),
        "ledger_sha256": inputs.ledger.sha256,
        "sql_manifest_path": str(inputs.sql_manifest_path),
        "sql_manifest_sha256": inputs.sql_manifest_sha256,
        "active_dso_path": str(activation.active_dso_path),
        "active_dso_sha256": inputs.dso_sha256,
        "test_executable_sha256": inputs.test_executable_sha256,
        "producer_executable_path": str(producer_executable_path),
        "producer_executable_sha256": producer_executable_sha256,
        "cluster_config_path": str(inputs.cluster_config_path),
        "cluster_config_sha256": inputs.cluster_config_sha256,
        "stage_manifest_path": str(inputs.stage_manifest_path),
        "stage_manifest_sha256": inputs.stage_manifest_sha256,
        "tool_signature_suite_sha256": (
            inputs.tool_signature_suite_sha256
        ),
        "runtime_control_dso_sha256": (
            inputs.runtime_control_dso_sha256
        ),
        "activation_receipt_path": str(activation.path),
        "activation_receipt_sha256": activation.sha256,
        "catalog_path": str(catalog_path),
        "catalog_sha256": catalog_sha256,
        "catalog_entry_sha256": case.catalog_entry_sha256,
    }
    request_raw = _canonical_json(request)
    request_path, request_sha256 = _write_immutable(
        artifact_directory / "requests" / case.profile,
        case.case_id,
        "json",
        request_raw,
        artifact_root=artifact_directory,
    )
    raw_result_output = (
        work_directory / f"{case.profile}.{case.case_id}.raw-result.json"
    )
    command = (
        str(inputs.test_executable_path),
        "--bioseq-safety-runtime-run",
        "--request",
        str(request_path),
        "--output",
        str(raw_result_output),
    )
    child = _run_child(
        command,
        timeout_seconds=timeout_seconds,
        context=f"{case.profile}/{case.case_id}",
        cwd=work_directory,
    )
    raw_result, raw_result_raw = _load_json(
        raw_result_output,
        f"{case.profile}/{case.case_id}-raw-result",
    )
    raw_result_path, raw_result_sha256 = _write_immutable(
        artifact_directory / "raw_results" / case.profile,
        case.case_id,
        "json",
        raw_result_raw,
        artifact_root=artifact_directory,
    )
    (
        observed_sentinels,
        observed_cleanup_events,
        observed_postgres_cleanup_events,
        observed_sqlstate,
        observed_message,
        observed_recovery,
        attestation,
    ) = _validate_raw_case_result(
        raw_result,
        case=case,
        inputs=inputs,
        activation=activation,
        producer_executable_sha256=producer_executable_sha256,
        request_sha256=request_sha256,
        catalog_sha256=catalog_sha256,
    )
    plan_case = _object(
        inputs.case_plan_entries[case.case_id],
        f"{case.profile}/{case.case_id}-plan",
    )
    plan_execution = _object(
        _field(
            plan_case,
            "execution",
            f"{case.profile}/{case.case_id}-plan",
        ),
        f"{case.profile}/{case.case_id}-plan.execution",
    )
    raw_recovery = _object(
        _field(
            raw_result,
            "recovery_probe",
            f"{case.profile}/{case.case_id}-raw-result",
        ),
        f"{case.profile}/{case.case_id}-raw-result.recovery_probe",
    )
    observed_recovery_rows = _field(
        raw_recovery,
        "observed_rows",
        f"{case.profile}/{case.case_id}-raw-result.recovery_probe",
    )
    if observed_recovery_rows != _field(
        plan_execution,
        "recovery_expected_rows",
        f"{case.profile}/{case.case_id}-plan.execution",
    ):
        _fail(
            f"{case.profile}/{case.case_id}",
            "recovery rows differ from the current case-plan execution",
        )
    recovery_raw = _canonical_json(observed_recovery_rows)
    recovery_path, recovery_artifact_sha256 = _write_immutable(
        artifact_directory / "recoveries" / case.profile,
        case.case_id,
        "json",
        recovery_raw,
        artifact_root=artifact_directory,
    )
    if recovery_artifact_sha256 != attestation["recovery_rows_sha256"]:
        _fail(
            f"{case.profile}/{case.case_id}",
            "recovery artifact differs from the attested rows",
        )
    attestation.update(
        {
            "request_path": _relative_artifact_path(
                request_path,
                matrix_path,
            ),
            "catalog_path": _relative_artifact_path(
                catalog_path,
                matrix_path,
            ),
            "activation_receipt_path": _relative_artifact_path(
                activation.path,
                matrix_path,
            ),
            "cluster_config_path": _relative_artifact_path(
                inputs.cluster_config_path,
                matrix_path,
            ),
            "raw_result_path": _relative_artifact_path(
                raw_result_path,
                matrix_path,
            ),
            "recovery_artifact_path": _relative_artifact_path(
                recovery_path,
                matrix_path,
            ),
            "recovery_artifact_sha256": recovery_artifact_sha256,
            "raw_result_sha256": raw_result_sha256,
        }
    )
    immutable_result = {
        "schema_revision": 2,
        "probe_abi_revision": 2,
        "case_id": case.case_id,
        "profile": case.profile,
        "sql_identity": case.sql_identity,
        "site_id": case.site_id,
        "ledger_site_sha256": case.ledger_site_sha256,
        "invocation_key": case.invocation_key,
        "fault_key": case.fault_key,
        "relation": case.relation,
        "probe_contract": dict(case.probe_contract),
        "case_plan_sha256": inputs.case_plan_sha256,
        "ledger_sha256": inputs.ledger.sha256,
        "sql_manifest_sha256": inputs.sql_manifest_sha256,
        "active_dso_sha256": inputs.dso_sha256,
        "producer_executable_sha256": producer_executable_sha256,
        "test_executable_sha256": inputs.test_executable_sha256,
        "expected_backend_outcome": case.expected_backend_outcome,
        "observed_backend_outcome": attestation[
            "observed_backend_outcome"
        ],
        "probe_observation_mode": attestation[
            "probe_observation_mode"
        ],
        "initial_backend_pid": attestation["initial_backend_pid"],
        "recovery_backend_pid": attestation["recovery_backend_pid"],
        "connection_replaced": attestation["connection_replaced"],
        "initial_connection_status": attestation[
            "initial_connection_status"
        ],
        "post_invocation_connection_status": attestation[
            "post_invocation_connection_status"
        ],
        "invocation_result_status": attestation[
            "invocation_result_status"
        ],
        "terminal_guard_observed": attestation[
            "terminal_guard_observed"
        ],
        "terminal_causality": attestation["terminal_causality"],
        "site_visit_count": attestation["site_visit_count"],
        "fault_trigger_count": attestation[
            "fault_trigger_count"
        ],
        "site_visit_sequence": attestation[
            "site_visit_sequence"
        ],
        "fault_trigger_sequence": attestation[
            "fault_trigger_sequence"
        ],
        "expected_destructor_sentinels": list(
            case.expected_destructor_sentinels
        ),
        "observed_destructor_sentinels": observed_sentinels,
        "expected_cleanup_events": list(
            case.expected_cleanup_events
        ),
        "observed_cleanup_events": observed_cleanup_events,
        "expected_postgres_cleanup_events": list(
            case.expected_postgres_cleanup_events
        ),
        "observed_postgres_cleanup_events": (
            observed_postgres_cleanup_events
        ),
        "expected_fmgr_abort_count": (
            0
            if (
                case.expected_backend_outcome == "backend_exit"
                or case.fault_key == BACKEND_SCOPED_VACUUM_FAULT_KEY
            )
            else 1
        ),
        "observed_fmgr_abort_count": attestation[
            "fmgr_abort_count"
        ],
        "expected_sqlstate": case.expected_sqlstate,
        "observed_sqlstate": observed_sqlstate,
        "expected_message": case.expected_message,
        "observed_message": observed_message,
        "expected_recovery_postcondition": (
            case.expected_recovery_postcondition
        ),
        "observed_recovery_postcondition": observed_recovery,
        "exercised_terminal_severity": False,
        "attestation": attestation,
    }
    immutable_result_raw = _canonical_json(immutable_result)
    immutable_result_path, immutable_result_sha256 = _write_immutable(
        artifact_directory / "results" / case.profile,
        case.case_id,
        "json",
        immutable_result_raw,
        artifact_root=artifact_directory,
    )
    log_raw = _child_log(
        result=child,
        context=f"{case.profile}/{case.case_id}",
        bindings={
            "request_sha256": request_sha256,
            "raw_result_sha256": _sha256_bytes(raw_result_raw),
            "immutable_result_sha256": immutable_result_sha256,
            "activation_receipt_sha256": activation.sha256,
            "catalog_sha256": catalog_sha256,
            "case_plan_sha256": inputs.case_plan_sha256,
            "runtime_inventory_sha256": (
                inputs.runtime_inventory_sha256
            ),
            "ledger_sha256": inputs.ledger.sha256,
            "sql_manifest_sha256": inputs.sql_manifest_sha256,
            "active_dso_sha256": inputs.dso_sha256,
            "control_dso_sha256": activation.control_dso_sha256,
            "cluster_config_sha256": inputs.cluster_config_sha256,
            "stage_manifest_sha256": inputs.stage_manifest_sha256,
            "tool_signature_suite_sha256": (
                inputs.tool_signature_suite_sha256
            ),
            "test_executable_sha256": inputs.test_executable_sha256,
            "producer_executable_sha256": producer_executable_sha256,
        },
    )
    immutable_log_path, immutable_log_sha256 = _write_immutable(
        artifact_directory / "logs" / case.profile,
        case.case_id,
        "log.json",
        log_raw,
        artifact_root=artifact_directory,
    )
    return {
        **{
            key: value
            for key, value in immutable_result.items()
            if key not in {"schema_revision", "attestation"}
        },
        "immutable_test_log_path": _relative_artifact_path(
            immutable_log_path,
            matrix_path,
        ),
        "immutable_test_log_sha256": immutable_log_sha256,
        "immutable_result_path": _relative_artifact_path(
            immutable_result_path,
            matrix_path,
        ),
        "immutable_result_sha256": immutable_result_sha256,
    }


def _validate_generated_live_authority(
    registration: Mapping[str, Any],
    value: Any,
    context: str,
) -> dict[str, Any]:
    live = _object(value, context)
    _require_exact_fields(live, GUC_GENERATED_LIVE_FIELDS, context)
    kind = _string(
        _field(registration, "kind", context), f"{context}.kind"
    )
    source_context = _string(
        _field(registration, "context", context),
        f"{context}.source_context",
    )
    expected_context = {
        "sighup": "sighup",
        "superuser_set": "superuser",
        "user_set": "user",
    }.get(source_context)
    unit = _field(registration, "unit", context)
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
        _field(registration, "flags", context),
        f"{context}.registration_flags",
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
    registration_hooks = _array(
        _field(registration, "hooks", context),
        f"{context}.registration_hooks",
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
        symbol = registration_hooks[index]
        if (
            _field(hook, "symbol", hook_context) != symbol
            or _field(hook, "present", hook_context)
            is not (symbol is not None)
            or _field(hook, "behavior", hook_context)
            != (f"custom_{name}" if symbol is not None else "none")
        ):
            _fail(hook_context, "generated hook behavior differs")
    return live


def _validate_guc_manifest_generation(
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
    if (
        _string(
            _field(generation, "profile", generation_context),
            f"{generation_context}.profile",
        )
        != profile
        or _string(
            _field(
                generation,
                "description_authority",
                generation_context,
            ),
            f"{generation_context}.description_authority",
        )
        != "cleaned_doxygen_only"
    ):
        _fail(generation_context, "profile or description authority differs")

    if profile not in PROFILES:
        _fail(generation_context, "unknown GUC profile")
    expected_total = GUC_PROFILE_COUNTS[profile]
    expected_test_only = (
        0 if profile == "production" else len(GUC_TEST_INDEXES_ONLY_NAMES)
    )
    if (
        _integer(
            _field(generation, "registration_count", generation_context),
            f"{generation_context}.registration_count",
        )
        != expected_total
    ):
        _fail(generation_context, "registration count differs")

    registry_sha256 = _sha256(
        _field(
            generation,
            "registry_content_sha256",
            generation_context,
        ),
        f"{generation_context}.registry_content_sha256",
    )
    if (
        _string(
            _field(generation, "registry_module", generation_context),
            f"{generation_context}.registry_module",
        )
        != f"bioseq.postgres.sql.generated.{profile}.guc_registry"
    ):
        _fail(generation_context, "registry module differs")

    owner_order = ("bioseq", "scheduler", "storage", "aligner")
    base_counts = GUC_OWNER_BASE_COUNTS
    facets: dict[str, dict[str, Any]] = {}
    owner_facets = _array(
        _field(generation, "owner_facets", generation_context),
        f"{generation_context}.owner_facets",
    )
    if len(owner_facets) != len(owner_order):
        _fail(generation_context, "owner-facet inventory differs")
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
        expected_source = GUC_OWNER_SOURCE_MODULES.get(owner, "")
        expected_count = base_counts.get(owner, -1) + (
            expected_test_only if owner == "bioseq" else 0
        )
        if (
            owner != owner_order[index]
            or _string(
                _field(facet, "source_module", facet_context),
                f"{facet_context}.source_module",
            )
            != expected_source
            or _string(
                _field(facet, "generated_module", facet_context),
                f"{facet_context}.generated_module",
            )
            != f"bioseq.postgres.sql.generated.{profile}.{owner}.gucs"
            or _integer(
                _field(facet, "registration_count", facet_context),
                f"{facet_context}.registration_count",
            )
            != expected_count
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
        != list(GUC_TEST_INDEXES_ONLY_NAMES)
        or _field(delta, "removed_from_test_indexes", delta_context) != []
        or _field(delta, "changed_in_test_indexes", delta_context) != []
    ):
        _fail(delta_context, "test-indexes profile delta differs")
    production_registry_sha256 = _sha256(
        _field(
            delta,
            "production_registry_content_sha256",
            delta_context,
        ),
        f"{delta_context}.production_registry_content_sha256",
    )
    test_registry_sha256 = _sha256(
        _field(
            delta,
            "test_indexes_registry_content_sha256",
            delta_context,
        ),
        f"{delta_context}.test_indexes_registry_content_sha256",
    )
    if registry_sha256 != (
        production_registry_sha256
        if profile == "production"
        else test_registry_sha256
    ):
        _fail(delta_context, "current registry hash is outside the proof")
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
            f"{delta_context}.owner_descriptor_profile_invariant.{owner}",
        ) != (owner != "bioseq"):
            _fail(delta_context, "owner invariance proof differs")
        owner_artifact_context = f"{delta_context}.owner_artifacts.{owner}"
        owner_artifact = _object(
            _field(artifacts, owner, delta_context),
            owner_artifact_context,
        )
        _require_exact_fields(
            owner_artifact,
            frozenset(
                {
                    "production_content_sha256",
                    "test_indexes_content_sha256",
                }
            ),
            owner_artifact_context,
        )
        production_owner_sha256 = _sha256(
            _field(
                owner_artifact,
                "production_content_sha256",
                owner_artifact_context,
            ),
            f"{owner_artifact_context}.production_content_sha256",
        )
        test_owner_sha256 = _sha256(
            _field(
                owner_artifact,
                "test_indexes_content_sha256",
                owner_artifact_context,
            ),
            f"{owner_artifact_context}.test_indexes_content_sha256",
        )
        if _field(
            facets[owner],
            "content_sha256",
            f"{generation_context}.owner_facets",
        ) != (
            production_owner_sha256
            if profile == "production"
            else test_owner_sha256
        ):
            _fail(owner_artifact_context, "current owner facet is unsealed")

    registrations = _array(
        _field(generation, "registrations", generation_context),
        f"{generation_context}.registrations",
    )
    if len(registrations) != expected_total:
        _fail(generation_context, "registration array size differs")
    names: set[str] = set()
    owner_counts = {owner: 0 for owner in owner_order}
    previous_ordinal = -1
    valid_kinds = {"bool", "integer", "real", "string", "enum"}
    valid_contexts = {"sighup", "superuser_set", "user_set"}
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
        owner_key = _string(
            _field(registration, "owner", registration_context),
            f"{registration_context}.owner",
        )
        kind = _string(
            _field(registration, "kind", registration_context),
            f"{registration_context}.kind",
        )
        guc_context = _string(
            _field(registration, "context", registration_context),
            f"{registration_context}.context",
        )
        source_module = _string(
            _field(registration, "source_module", registration_context),
            f"{registration_context}.source_module",
        )
        expected_name_prefix = (
            "bioseq." if owner_key == "bioseq" else f"bioseq.{owner_key}."
        )
        if (
            ordinal <= previous_ordinal
            or not name.startswith(expected_name_prefix)
            or not member
            or name in names
            or owner_key not in facets
            or kind not in valid_kinds
            or guc_context not in valid_contexts
            or source_module
            != _field(
                facets[owner_key],
                "source_module",
                registration_context,
            )
        ):
            _fail(registration_context, "identity/order/ownership differs")
        previous_ordinal = ordinal
        names.add(name)
        owner_counts[owner_key] += 1
        _sha256(
            _field(registration, "source_sha256", registration_context),
            f"{registration_context}.source_sha256",
        )
        for field_name in ("cpp_storage", "storage_kind", "default_expression"):
            _string(
                _field(registration, field_name, registration_context),
                f"{registration_context}.{field_name}",
            )
        source_path_text = _string(
            _field(registration, "source_path", registration_context),
            f"{registration_context}.source_path",
        )
        source_path = Path(source_path_text)
        if (
            source_path.is_absolute()
            or not source_path.parts
            or ".." in source_path.parts
            or source_path_text.startswith("main/")
            or "\\" in source_path_text
            or str(source_path) != source_path_text
        ):
            _fail(
                registration_context,
                "GUC source path is not a repository-relative identity",
            )
        flags = _string_list(
            _field(registration, "flags", registration_context),
            f"{registration_context}.flags",
            allow_empty=True,
        )
        if any(flag not in {"no_show_all", "not_in_sample"} for flag in flags):
            _fail(registration_context, "unsupported GUC flag")
        hooks = _array(
            _field(registration, "hooks", registration_context),
            f"{registration_context}.hooks",
        )
        if len(hooks) != 3 or any(
            hook is not None and (not isinstance(hook, str) or not hook)
            for hook in hooks
        ):
            _fail(registration_context, "hook tuple differs")
        test_only = name in GUC_TEST_INDEXES_ONLY_NAMES
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
        enum_default = _field(
            registration, "enum_default", registration_context
        )
        if (
            kind == "enum"
            and (
                not enum_rows
                or not isinstance(enum_default, str)
                or not enum_default
            )
        ) or (
            kind != "enum"
            and (enum_rows or enum_default is not None)
        ):
            _fail(registration_context, "enum authority differs")
        enum_names: set[str] = set()
        visible_enum_names: list[str] = []
        for row_index, row_value in enumerate(enum_rows):
            row_context = f"{registration_context}.enum_rows[{row_index}]"
            row = _array(row_value, row_context)
            if (
                len(row) != 6
                or not isinstance(row[0], str)
                or not row[0]
                or not isinstance(row[1], str)
                or not row[1]
                or not isinstance(row[2], str)
                or not row[2]
                or (row[3] is not None and (
                    not isinstance(row[3], str) or not row[3]
                ))
                or not isinstance(row[4], bool)
                or not isinstance(row[5], bool)
                or row[0] in enum_names
            ):
                _fail(row_context, "enum row is malformed")
            enum_names.add(row[0])
            if row[5] and not row[4]:
                visible_enum_names.append(row[0])
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

    for owner, base_count in base_counts.items():
        expected_count = base_count + (
            expected_test_only if owner == "bioseq" else 0
        )
        if owner_counts[owner] != expected_count:
            _fail(generation_context, "owner registration count differs")
    observed_test_only_names = set(names).intersection(
        GUC_TEST_INDEXES_ONLY_NAMES
    )
    expected_test_only_names = (
        set() if profile == "production" else set(GUC_TEST_INDEXES_ONLY_NAMES)
    )
    if observed_test_only_names != expected_test_only_names:
        _fail(generation_context, "test-profile GUC delta differs")
    return generation


def _validate_guc_properties(
    value: Any,
    sha256_value: Any,
    *,
    generation: Mapping[str, Any],
    profile: str,
    context: str,
) -> tuple[tuple[Mapping[str, Any], ...], str]:
    properties = _array(value, context)
    registrations = _array(
        _field(generation, "registrations", context),
        f"{context}.manifest_registrations",
    )
    digest = _sha256(sha256_value, f"{context}.sha256")
    if (
        len(properties) != len(registrations)
        or _sha256_bytes(_canonical_json(properties)) != digest
    ):
        _fail(context, "property array is malformed or unsealed")
    facets = {
        _field(facet, "owner", context): facet
        for facet in _array(
            _field(generation, "owner_facets", context),
            f"{context}.owner_facets",
        )
    }
    delta = _field(generation, "profile_delta_validation", context)
    expected_projection_base = {
        "profile": profile,
        "registry_module": _field(
            generation, "registry_module", context
        ),
        "registry_content_sha256": _field(
            generation, "registry_content_sha256", context
        ),
        "profile_delta_validation_sha256": _sha256_bytes(
            _canonical_json(delta)
        ),
    }
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
        property_registration = _object(
            _field(property_row, "registration", property_context),
            f"{property_context}.registration",
        )
        _require_exact_fields(
            property_registration,
            GUC_REGISTRATION_FIELDS,
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
        owner_key = _field(registration, "owner", property_context)
        expected_projection = {
            **expected_projection_base,
            "owner_facet": facets[owner_key],
        }
        projection = _object(
            _field(property_row, "profile_projection", property_context),
            f"{property_context}.profile_projection",
        )
        _require_exact_fields(
            projection,
            GUC_PROFILE_PROJECTION_FIELDS,
            f"{property_context}.profile_projection",
        )
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
            or property_registration != registration
            or _sha256(
                _field(
                    property_row,
                    "manifest_registration_sha256",
                    property_context,
                ),
                f"{property_context}.manifest_registration_sha256",
            )
            != _sha256_bytes(_canonical_json(registration))
            or projection != expected_projection
        ):
            _fail(
                property_context,
                "property does not close over its manifest registration",
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
    return tuple(properties), digest


def _parse_sql_manifest_binding(
    path: Path,
    profile: str,
) -> tuple[str, str]:
    context = f"{profile}-sql-manifest"
    manifest, raw = _load_json(path, context)
    _require_current_generated_sql_manifest(manifest, context)
    if (
        _string(
            _field(manifest, "authority_mode", context),
            f"{context}.authority_mode",
        )
        != "annotated_cpp"
        or _string(
            _field(manifest, "compiler_profile", context),
            f"{context}.compiler_profile",
        )
        != profile
    ):
        _fail(
            context,
            "authority or compiler profile differs",
        )
    _validate_guc_manifest_generation(
        manifest,
        profile=profile,
        context=context,
    )
    return (
        _sha256_bytes(raw),
        _sha256(
            _field(manifest, "script_sha256", context),
            f"{context}.script_sha256",
        ),
    )


def _manifest_absolute_path(value: Any, context: str) -> Path:
    text = _string(value, context)
    path = Path(text)
    if (
        not path.is_absolute()
        or ".." in path.parts
        or str(path) != text
    ):
        _fail(context, "path must be absolute and normalized")
    return path


def _manifest_relative_path(value: Any, context: str) -> Path:
    text = _string(value, context)
    path = Path(text)
    if (
        path.is_absolute()
        or not path.parts
        or any(part in {"", ".", ".."} for part in path.parts)
        or "\\" in text
        or str(path) != text
    ):
        _fail(context, "path must be normalized and relative")
    return path


def _runtime_entry_record(entry: Mapping[str, Any]) -> str:
    link_target = entry["source_link_target"]
    link_target_record = "-" if link_target is None else link_target
    return (
        f"root:{entry['root']}\n"
        f"path:{entry['path']}\n"
        f"kind:{entry['kind']}\n"
        f"source_kind:{entry['source_kind']}\n"
        f"source_path:{entry['source_path']}\n"
        f"source_link_target:{link_target_record}\n"
        f"resolved_source_path:{entry['resolved_source_path']}\n"
        f"size:{entry['size'] if entry['size'] is not None else '-'}\n"
        f"sha256:{entry['sha256'] if entry['sha256'] is not None else '-'}\n"
    )


def _loader_entry_record(entry: Mapping[str, Any]) -> str:
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
    override = "" if module_pathname_override is None else (
        module_pathname_override
    )
    record = (
        f"artifact:{name}\n"
        f"module_pathname_override:{override}\n"
        f"control:{control_filename}:{control_sha256}\n"
        f"install_sql:{install_sql_filename}:{install_sql_sha256}\n"
        f"library:{library_filename}:{library_sha256}\n"
    ).encode("utf-8")
    return _sha256_bytes(record)


def _validate_runtime_closure(
    value: Any,
    *,
    stage_root: Path,
    source: Mapping[str, Any],
    staged: Mapping[str, Any],
    context: str,
) -> None:
    closure = _object(value, context)
    _require_exact_fields(closure, RUNTIME_CLOSURE_FIELDS, context)
    if (
        _string(
            _field(closure, "algorithm", context),
            f"{context}.algorithm",
        )
        != RUNTIME_CLOSURE_ALGORITHM
        or _nonnegative_integer(
            _field(closure, "schema_revision", context),
            f"{context}.schema_revision",
        )
        != RUNTIME_CLOSURE_SCHEMA_REVISION
    ):
        _fail(context, "algorithm or schema revision differs")

    roots_context = f"{context}.roots"
    roots = _object(_field(closure, "roots", context), roots_context)
    _require_exact_fields(roots, STAGE_LAYOUT_FIELDS, roots_context)
    canonical_roots: dict[str, Path] = {}
    for name in ("bindir", "sharedir", "pkglibdir"):
        root_context = f"{roots_context}.{name}"
        root = _object(_field(roots, name, roots_context), root_context)
        _require_exact_fields(root, RUNTIME_ROOT_FIELDS, root_context)
        if (
            _string(
                _field(root, "reported_source", root_context),
                f"{root_context}.reported_source",
            )
            != _field(source, name, f"{context}.source")
            or _string(
                _field(root, "staged", root_context),
                f"{root_context}.staged",
            )
            != _field(staged, name, f"{context}.staged")
        ):
            _fail(root_context, "root differs from the stage layout")
        canonical_roots[name] = _manifest_absolute_path(
            _field(root, "canonical_source", root_context),
            f"{root_context}.canonical_source",
        )

    entries_context = f"{context}.entries"
    entries = _array(_field(closure, "entries", context), entries_context)
    entry_count = _nonnegative_integer(
        _field(closure, "entry_count", context),
        f"{context}.entry_count",
    )
    if len(entries) != entry_count:
        _fail(context, "entry count differs from the runtime closure")
    root_order = {"bindir": 0, "sharedir": 1, "pkglibdir": 2}
    runtime_keys: list[tuple[int, str]] = []
    runtime_records: list[str] = []
    runtime_entries: dict[tuple[str, str], Mapping[str, Any]] = {}
    for index, raw_entry in enumerate(entries):
        entry_context = f"{entries_context}[{index}]"
        entry = _object(raw_entry, entry_context)
        _require_exact_fields(entry, RUNTIME_ENTRY_FIELDS, entry_context)
        root = _string(
            _field(entry, "root", entry_context),
            f"{entry_context}.root",
        )
        if root not in root_order:
            _fail(entry_context, "runtime root is unknown")
        relative = _manifest_relative_path(
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
        if kind not in {"file", "dir"}:
            _fail(entry_context, "runtime kind is invalid")
        if source_kind not in {"file", "dir", "symlink"}:
            _fail(entry_context, "runtime source kind is invalid")
        if source_kind != "symlink" and source_kind != kind:
            _fail(entry_context, "runtime source kind differs")
        _manifest_absolute_path(
            _field(entry, "source_path", entry_context),
            f"{entry_context}.source_path",
        )
        resolved_source = _manifest_absolute_path(
            _field(entry, "resolved_source_path", entry_context),
            f"{entry_context}.resolved_source_path",
        )
        try:
            resolved_source.relative_to(canonical_roots[root])
        except ValueError:
            _fail(entry_context, "resolved source escapes its runtime root")
        link_target = _field(entry, "source_link_target", entry_context)
        if source_kind == "symlink":
            _string(link_target, f"{entry_context}.source_link_target")
        elif link_target is not None:
            _fail(entry_context, "runtime entry has a spurious link target")
        size = _field(entry, "size", entry_context)
        digest = _field(entry, "sha256", entry_context)
        destination = _stage_member(
            stage_root,
            str(Path(_string(staged[root], f"{context}.staged")) / relative),
            f"{entry_context}.staged",
        )
        if kind == "dir":
            if size is not None or digest is not None:
                _fail(entry_context, "runtime directory metadata is invalid")
            if not destination.is_dir():
                _fail(entry_context, "staged runtime directory differs")
        else:
            expected_size = _nonnegative_integer(
                size,
                f"{entry_context}.size",
            )
            expected_sha256 = _sha256(
                digest,
                f"{entry_context}.sha256",
            )
            staged_raw, staged_metadata = _read_regular_file_with_metadata(
                destination,
                f"{entry_context}.staged",
            )
            if (
                staged_metadata.st_size != expected_size
                or _sha256_bytes(staged_raw) != expected_sha256
            ):
                _fail(entry_context, "staged runtime file bytes differ")
        relative_text = str(relative)
        runtime_entries[(root, relative_text)] = entry
        runtime_keys.append((root_order[root], relative_text))
        runtime_records.append(_runtime_entry_record(entry))
    if (
        len(set(runtime_keys)) != len(runtime_keys)
        or runtime_keys != sorted(runtime_keys)
    ):
        _fail(context, "runtime entries are duplicated or noncanonical")
    if _sha256(
        _field(closure, "aggregate_sha256", context),
        f"{context}.aggregate_sha256",
    ) != _sha256_bytes("".join(runtime_records).encode("utf-8")):
        _fail(context, "runtime aggregate hash is stale")

    sentinels_context = f"{context}.required_sentinels"
    sentinels = _object(
        _field(closure, "required_sentinels", context),
        sentinels_context,
    )
    for name, raw_sentinel in sentinels.items():
        sentinel_context = f"{sentinels_context}.{name}"
        if not name:
            _fail(sentinel_context, "sentinel name is empty")
        sentinel = _object(raw_sentinel, sentinel_context)
        _require_exact_fields(
            sentinel,
            RUNTIME_SENTINEL_FIELDS,
            sentinel_context,
        )
        root = _string(
            _field(sentinel, "root", sentinel_context),
            f"{sentinel_context}.root",
        )
        if root not in root_order:
            _fail(sentinel_context, "sentinel root is unknown")
        relative = _manifest_relative_path(
            _field(sentinel, "path", sentinel_context),
            f"{sentinel_context}.path",
        )
        kind = _string(
            _field(sentinel, "kind", sentinel_context),
            f"{sentinel_context}.kind",
        )
        digest = _field(sentinel, "sha256", sentinel_context)
        if kind == "file":
            sentinel_sha256: str | None = _sha256(
                digest,
                f"{sentinel_context}.sha256",
            )
        elif kind == "dir":
            if digest is not None:
                _fail(sentinel_context, "directory sentinel has a hash")
            sentinel_sha256 = None
        else:
            _fail(sentinel_context, "sentinel kind is invalid")
        runtime_entry = runtime_entries.get((root, str(relative)))
        if (
            runtime_entry is None
            or runtime_entry["kind"] != kind
            or runtime_entry["sha256"] != sentinel_sha256
        ):
            _fail(sentinel_context, "sentinel is outside runtime closure")

    loader_context = f"{context}.loader_dependencies"
    loader = _object(
        _field(closure, "loader_dependencies", context),
        loader_context,
    )
    _require_exact_fields(loader, RUNTIME_LOADER_FIELDS, loader_context)
    loader_entries = _array(
        _field(loader, "entries", loader_context),
        f"{loader_context}.entries",
    )
    loader_count = _nonnegative_integer(
        _field(loader, "entry_count", loader_context),
        f"{loader_context}.entry_count",
    )
    if len(loader_entries) != loader_count:
        _fail(loader_context, "entry count differs from the loader closure")
    loader_keys: list[tuple[bytes, bytes]] = []
    loader_records: list[str] = []
    for index, raw_entry in enumerate(loader_entries):
        entry_context = f"{loader_context}.entries[{index}]"
        entry = _object(raw_entry, entry_context)
        _require_exact_fields(
            entry,
            RUNTIME_LOADER_ENTRY_FIELDS,
            entry_context,
        )
        source_path = _manifest_absolute_path(
            _field(entry, "source", entry_context),
            f"{entry_context}.source",
        )
        staged_path = _manifest_relative_path(
            _field(entry, "staged", entry_context),
            f"{entry_context}.staged",
        )
        expected_size = _nonnegative_integer(
            _field(entry, "size", entry_context),
            f"{entry_context}.size",
        )
        expected_sha256 = _sha256(
            _field(entry, "sha256", entry_context),
            f"{entry_context}.sha256",
        )
        staged_dependency = _stage_member(
            stage_root,
            str(staged_path),
            f"{entry_context}.staged",
        )
        staged_raw, staged_metadata = _read_regular_file_with_metadata(
            staged_dependency,
            f"{entry_context}.staged",
        )
        if (
            staged_metadata.st_size != expected_size
            or _sha256_bytes(staged_raw) != expected_sha256
        ):
            _fail(entry_context, "staged loader dependency bytes differ")
        loader_keys.append(
            (
                str(staged_path).encode("utf-8"),
                str(source_path).encode("utf-8"),
            )
        )
        loader_records.append(_loader_entry_record(entry))
    if (
        len(set(loader_keys)) != len(loader_keys)
        or loader_keys != sorted(loader_keys)
    ):
        _fail(
            loader_context,
            "loader entries are duplicated or noncanonical",
        )
    if _sha256(
        _field(loader, "aggregate_sha256", loader_context),
        f"{loader_context}.aggregate_sha256",
    ) != _sha256_bytes("".join(loader_records).encode("utf-8")):
        _fail(loader_context, "loader aggregate hash is stale")


def _parse_stage_extension_artifacts(
    manifest: Mapping[str, Any],
    *,
    stage_root: Path,
    staged: Mapping[str, Any],
    dso_sha256: str,
    sql_script_sha256: str,
    context: str,
) -> tuple[Path, str, Path, str, Path, str]:
    closure_context = f"{context}.extension_artifacts"
    closure = _object(
        _field(manifest, "extension_artifacts", context),
        closure_context,
    )
    _require_exact_fields(
        closure,
        STAGE_EXTENSION_CLOSURE_FIELDS,
        closure_context,
    )
    raw_artifacts = _array(
        _field(closure, "artifacts", closure_context),
        f"{closure_context}.artifacts",
    )
    if len(raw_artifacts) != 1:
        _fail(closure_context, "must contain exactly one Bioseq artifact")
    artifact_context = f"{closure_context}.artifacts[0]"
    artifact = _object(raw_artifacts[0], artifact_context)
    _require_exact_fields(
        artifact,
        STAGE_EXTENSION_ARTIFACT_FIELDS,
        artifact_context,
    )
    artifact_name = _string(
        _field(artifact, "name", artifact_context),
        f"{artifact_context}.name",
    )
    module_pathname_override = _field(
        artifact,
        "module_pathname_override",
        artifact_context,
    )
    if artifact_name != "bioseq" or module_pathname_override is not None:
        _fail(
            artifact_context,
            "artifact name or module pathname override differs",
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
    _require_exact_fields(control, STAGE_CONTROL_FIELDS, control_context)
    _require_exact_fields(
        install_sql,
        STAGE_INSTALL_SQL_FIELDS,
        install_context,
    )
    _require_exact_fields(library, STAGE_LIBRARY_FIELDS, library_context)

    staged_sharedir = _string(
        _field(staged, "sharedir", f"{context}.staged"),
        f"{context}.staged.sharedir",
    )
    control_source = _manifest_absolute_path(
        _field(control, "source", control_context),
        f"{control_context}.source",
    )
    control_filename = _string(
        _field(control, "filename", control_context),
        f"{control_context}.filename",
    )
    if (
        control_filename != "bioseq.control"
        or control_source.name != control_filename
    ):
        _fail(control_context, "control filename is inconsistent")
    expected_control_relative = str(
        Path(staged_sharedir) / "extension" / control_filename
    )
    control_relative = _string(
        _field(control, "staged", control_context),
        f"{control_context}.staged",
    )
    if control_relative != expected_control_relative:
        _fail(control_context, "control path differs from staged sharedir")
    control_path = _stage_member(
        stage_root,
        control_relative,
        f"{control_context}.staged",
    )
    control_source_sha256 = _sha256(
        _field(control, "source_sha256", control_context),
        f"{control_context}.source_sha256",
    )
    control_sha256 = _sha256(
        _field(control, "sha256", control_context),
        f"{control_context}.sha256",
    )
    if (
        _sha256_bytes(
            _read_regular_file(control_source, f"{control_context}.source")
        )
        != control_source_sha256
        or control_source_sha256 != control_sha256
        or _sha256_bytes(_read_regular_file(control_path, control_context))
        != control_sha256
    ):
        _fail(context, "staged control file differs from the manifest")

    install_source = _manifest_absolute_path(
        _field(install_sql, "source", install_context),
        f"{install_context}.source",
    )
    install_version = _string(
        _field(install_sql, "version", install_context),
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
    if (
        install_filename != f"bioseq--{install_version}.sql"
        or install_source.name != install_filename
    ):
        _fail(install_context, "install SQL filename is inconsistent")
    install_sha256 = _sha256(
        _field(install_sql, "sha256", install_context),
        f"{install_context}.sha256",
    )
    expected_install_relative = str(
        Path(staged_sharedir) / "extension" / install_filename
    )
    install_relative = _string(
        _field(install_sql, "staged", install_context),
        f"{install_context}.staged",
    )
    if install_relative != expected_install_relative:
        _fail(install_context, "install SQL path differs from staged sharedir")
    install_path = _stage_member(
        stage_root,
        install_relative,
        f"{install_context}.staged",
    )

    library_source = _manifest_absolute_path(
        _field(library, "source", library_context),
        f"{library_context}.source",
    )
    library_filename = _string(
        _field(library, "filename", library_context),
        f"{library_context}.filename",
    )
    if (
        Path(library_filename).name != library_filename
        or library_filename in {"", ".", ".."}
        or library_source.name != library_filename
    ):
        _fail(
            f"{library_context}.filename",
            "library filename must be one basename",
        )
    if _string(
        _field(artifact, "effective_module_name", artifact_context),
        f"{artifact_context}.effective_module_name",
    ) != library_filename.split(".", 1)[0]:
        _fail(artifact_context, "effective module name differs")
    expected_library_relative = str(
        Path(
            _string(
                _field(staged, "pkglibdir", f"{context}.staged"),
                f"{context}.staged.pkglibdir",
            )
        )
        / library_filename
    )
    library_relative = _string(
        _field(library, "staged", library_context),
        f"{library_context}.staged",
    )
    if library_relative != expected_library_relative:
        _fail(
            f"{library_context}.staged",
            "library path differs from staged pkglibdir",
        )
    library_path = _stage_member(
        stage_root,
        library_relative,
        f"{library_context}.staged",
    )
    library_sha256 = _sha256(
        _field(library, "sha256", library_context),
        f"{library_context}.sha256",
    )
    if (
        _sha256_bytes(
            _read_regular_file(install_source, f"{install_context}.source")
        )
        != install_sha256
        or _sha256_bytes(_read_regular_file(install_path, install_context))
        != install_sha256
        or install_sha256 != sql_script_sha256
        or _sha256_bytes(
            _read_regular_file(library_source, f"{library_context}.source")
        )
        != library_sha256
        or _sha256_bytes(_read_regular_file(library_path, library_context))
        != library_sha256
        or library_sha256 != dso_sha256
        or library_path.name != library_filename
    ):
        _fail(context, "staged SQL or DSO differs from the current profile")

    aggregate = _sha256(
        _field(closure, "aggregate_sha256", closure_context),
        f"{closure_context}.aggregate_sha256",
    )
    if aggregate != _stage_package_aggregate(
        name=artifact_name,
        module_pathname_override=module_pathname_override,
        control_filename=control_filename,
        control_sha256=control_sha256,
        install_sql_filename=install_filename,
        install_sql_sha256=install_sha256,
        library_filename=library_filename,
        library_sha256=library_sha256,
    ):
        _fail(context, "Bioseq aggregate hash is stale")
    return (
        control_path,
        control_sha256,
        install_path,
        install_sha256,
        library_path,
        library_sha256,
    )


def _stage_member(
    stage_root: Path,
    value: Any,
    context: str,
) -> Path:
    relative_text = _string(value, context)
    relative = Path(relative_text)
    if (
        relative.is_absolute()
        or not relative.parts
        or ".." in relative.parts
        or str(relative) != relative_text
    ):
        _fail(context, "stage path must be normalized and relative")
    candidate = stage_root
    for component in relative.parts:
        candidate = candidate / component
        if candidate.is_symlink():
            _fail(context, "stage path contains a symbolic link")
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(stage_root)
    except (OSError, ValueError) as error:
        raise EvidenceError(
            f"{context}: stage path escapes or is absent: {candidate}: "
            f"{error}"
        ) from error
    return resolved


def _parse_stage_manifest(
    path: Path,
    *,
    profile: str,
    dso_sha256: str,
    sql_script_sha256: str,
) -> StageManifestBinding:
    context = f"{profile}-stage-manifest"
    if not path.is_absolute() or not path.is_file() or path.is_symlink():
        _fail(context, "input must be an absolute regular non-symlink file")
    manifest, raw = _load_json(path, context)
    _require_exact_fields(manifest, STAGE_MANIFEST_FIELDS, context)
    if (
        _string(
            _field(manifest, "schema", context),
            f"{context}.schema",
        )
        != STAGE_MANIFEST_SCHEMA
        or _nonnegative_integer(
            _field(manifest, "schema_revision", context),
            f"{context}.schema_revision",
        )
        != STAGE_MANIFEST_SCHEMA_REVISION
    ):
        _fail(context, "unsupported schema")
    version = _string(
        _field(manifest, "postgresql_version", context),
        f"{context}.postgresql_version",
    )
    if version != "PostgreSQL 17.10" and not version.startswith(
        "PostgreSQL 17.10 "
    ):
        _fail(context, "requires exact PostgreSQL 17.10")
    _string(
        _field(manifest, "postgresql_configure", context),
        f"{context}.postgresql_configure",
    )
    stage_root = path.parent.resolve(strict=True)
    source = _object(_field(manifest, "source", context), f"{context}.source")
    staged = _object(_field(manifest, "staged", context), f"{context}.staged")
    _require_exact_fields(source, STAGE_LAYOUT_FIELDS, f"{context}.source")
    _require_exact_fields(staged, STAGE_LAYOUT_FIELDS, f"{context}.staged")
    staged_directories: dict[str, Path] = {}
    for key in STAGE_LAYOUT_FIELDS:
        source_text = _string(
            _field(source, key, f"{context}.source"),
            f"{context}.source.{key}",
        )
        source_path = Path(source_text)
        if (
            not source_path.is_absolute()
            or ".." in source_path.parts
            or str(source_path) != source_text
        ):
            _fail(
                f"{context}.source.{key}",
                "source layout path must be absolute and normalized",
            )
        member = _stage_member(
            stage_root,
            _field(staged, key, f"{context}.staged"),
            f"{context}.staged.{key}",
        )
        if not member.is_dir():
            _fail(f"{context}.staged.{key}", "must name a directory")
        staged_directories[key] = member

    _validate_runtime_closure(
        _field(manifest, "runtime_closure", context),
        stage_root=stage_root,
        source=source,
        staged=staged,
        context=f"{context}.runtime_closure",
    )

    expected_tool_paths = _object(
        _field(manifest, "expected_tool_paths", context),
        f"{context}.expected_tool_paths",
    )
    _require_exact_fields(
        expected_tool_paths,
        frozenset(POSTGRESQL_TOOLS),
        f"{context}.expected_tool_paths",
    )
    raw_signatures = _array(
        _field(manifest, "tool_signatures", context),
        f"{context}.tool_signatures",
    )
    if len(raw_signatures) != len(POSTGRESQL_TOOLS):
        _fail(context, "tool signatures do not close the six-tool suite")
    signatures: dict[str, dict[str, Any]] = {}
    bindir = _string(
        _field(staged, "bindir", f"{context}.staged"),
        f"{context}.staged.bindir",
    )
    for index, raw_signature in enumerate(raw_signatures):
        signature_context = f"{context}.tool_signatures[{index}]"
        signature = _object(raw_signature, signature_context)
        _require_exact_fields(
            signature,
            STAGE_TOOL_SIGNATURE_FIELDS,
            signature_context,
        )
        tool = _string(
            _field(signature, "tool", signature_context),
            f"{signature_context}.tool",
        )
        if tool not in POSTGRESQL_TOOLS or tool in signatures:
            _fail(signature_context, "tool is unknown or duplicated")
        relative = str(Path(bindir) / tool)
        if (
            _string(
                _field(signature, "staged_path", signature_context),
                f"{signature_context}.staged_path",
            )
            != relative
            or _field(
                expected_tool_paths,
                tool,
                f"{context}.expected_tool_paths",
            )
            != relative
        ):
            _fail(signature_context, "tool path differs from staged bindir")
        tool_version = _string(
            _field(signature, "version", signature_context),
            f"{signature_context}.version",
        )
        version_prefix = f"{tool} (PostgreSQL) 17.10"
        if tool_version != version_prefix and not tool_version.startswith(
            version_prefix + " "
        ):
            _fail(signature_context, "tool version is not PostgreSQL 17.10")
        tool_path = _stage_member(
            stage_root,
            relative,
            f"{signature_context}.staged_path",
        )
        tool_raw, tool_metadata = _read_regular_file_with_metadata(
            tool_path,
            signature_context,
        )
        if (
            stat.S_IMODE(tool_metadata.st_mode) & 0o111 == 0
            or _sha256_bytes(tool_raw)
            != _sha256(
                _field(signature, "sha256", signature_context),
                f"{signature_context}.sha256",
            )
        ):
            _fail(
                signature_context,
                "tool bytes or executable mode differ from the manifest",
            )
        signatures[tool] = dict(signature)

    (
        control_path,
        control_sha256,
        install_path,
        install_sha256,
        library_path,
        library_sha256,
    ) = _parse_stage_extension_artifacts(
        manifest,
        stage_root=stage_root,
        staged=staged,
        dso_sha256=dso_sha256,
        sql_script_sha256=sql_script_sha256,
        context=context,
    )
    normalized_signatures = [
        signatures[tool] for tool in sorted(POSTGRESQL_TOOLS)
    ]
    return StageManifestBinding(
        manifest_path=path.resolve(strict=True),
        manifest_sha256=_sha256_bytes(raw),
        tool_signature_suite_sha256=_sha256_bytes(
            _canonical_json(normalized_signatures)
        ),
        stage_root=stage_root,
        bindir=staged_directories["bindir"],
        sharedir=staged_directories["sharedir"],
        pkglibdir=staged_directories["pkglibdir"],
        pg_ctl_path=_stage_member(
            stage_root,
            _field(
                expected_tool_paths,
                "pg_ctl",
                f"{context}.expected_tool_paths",
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


def _authenticated_directory(path: Path, context: str) -> Path:
    descriptor = _open_directory(path, context)
    try:
        if not stat.S_ISDIR(os.fstat(descriptor).st_mode):
            _fail(context, "path is not a real directory")
    finally:
        os.close(descriptor)
    return _absolute_path(path)


def _contained_path(path: Path, root: Path, context: str) -> None:
    try:
        path.relative_to(root)
    except ValueError:
        _fail(context, f"path escapes authenticated root {root}")


def _validate_connection(
    value: Any,
    *,
    candidate_root: Path,
    context: str,
) -> tuple[Path, str, str]:
    connection = _object(value, context)
    _require_exact_fields(connection, CONNECTION_FIELDS, context)
    host = _authenticated_directory(
        _configured_absolute_path(
            _field(connection, "host", context),
            f"{context}.host",
        ),
        f"{context}.host",
    )
    _contained_path(host, candidate_root, f"{context}.host")
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
    if _string(
        _field(connection, "password", context),
        f"{context}.password",
    ) != PRIVATE_PASSWORD:
        _fail(f"{context}.password", "private trust-auth marker differs")
    return host, port_text, user


def _validate_authorization_marker(
    path: Path,
    *,
    data_directory: Path,
    context: str,
    expected_uid: int | None = None,
) -> tuple[Path, bytes, os.stat_result]:
    expected_path = data_directory / AUTHORIZATION_MARKER_NAME
    if path != expected_path:
        _fail(context, "authorization marker path or filename differs")
    raw, metadata = _read_single_link_file_with_metadata(path, context)
    if raw != AUTHORIZATION_MARKER:
        _fail(context, "authorization marker bytes differ")
    if stat.S_IMODE(metadata.st_mode) != 0o600:
        _fail(context, "authorization marker mode must be 0600")
    owner_uid = os.getuid() if expected_uid is None else expected_uid
    if metadata.st_uid != owner_uid:
        _fail(context, "authorization marker is not owned by the current uid")
    return path, raw, metadata


def _validate_cluster_config_authority(
    config: Mapping[str, Any],
    *,
    profile: str,
    candidate_root: Path,
    stage: StageManifestBinding,
    stage_manifest_path: Path,
    runtime_control_dso_path: Path,
    context: str,
) -> ClusterAuthoritySnapshot:
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

    configured_manifest = _configured_absolute_path(
        _field(config, "stage_manifest_path", context),
        f"{context}.stage_manifest_path",
    )
    configured_stage_root = _configured_absolute_path(
        _field(config, "private_stage_root", context),
        f"{context}.private_stage_root",
    )
    if (
        configured_manifest != stage.manifest_path
        or configured_manifest != stage_manifest_path.resolve(strict=True)
        or _sha256(
            _field(config, "stage_manifest_sha256", context),
            f"{context}.stage_manifest_sha256",
        )
        != stage.manifest_sha256
        or _sha256(
            _field(config, "tool_signature_suite_sha256", context),
            f"{context}.tool_signature_suite_sha256",
        )
        != stage.tool_signature_suite_sha256
        or _authenticated_directory(
            configured_stage_root,
            f"{context}.private_stage_root",
        )
        != stage.stage_root
    ):
        _fail(context, "stage manifest or six-tool binding differs")

    socket_directory, connection_port, connection_user = _validate_connection(
        _field(config, "connection", context),
        candidate_root=candidate_root,
        context=f"{context}.connection",
    )
    data_directory = _authenticated_directory(
        _configured_absolute_path(
            _field(config, "expected_data_directory", context),
            f"{context}.expected_data_directory",
        ),
        f"{context}.expected_data_directory",
    )
    _contained_path(
        data_directory,
        candidate_root,
        f"{context}.expected_data_directory",
    )
    if data_directory == socket_directory:
        _fail(context, "data and local-socket directories must be distinct")
    terminal_marker_directory = _authenticated_directory(
        _configured_absolute_path(
            _field(config, "terminal_marker_directory", context),
            f"{context}.terminal_marker_directory",
        ),
        f"{context}.terminal_marker_directory",
    )
    marker_metadata = os.stat(
        terminal_marker_directory, follow_symlinks=False
    )
    if (
        terminal_marker_directory
        != data_directory / "bioseq-safety-terminal-markers"
        or stat.S_IMODE(marker_metadata.st_mode) != 0o700
        or marker_metadata.st_uid != os.getuid()
    ):
        _fail(
            f"{context}.terminal_marker_directory",
            "terminal marker root must be the exact owned mode-0700 "
            "directory under the private data directory",
        )
    server_log_path = _configured_absolute_path(
        _field(config, "server_log_path", context),
        f"{context}.server_log_path",
    )
    _, server_log_metadata = _read_single_link_file_with_metadata(
        server_log_path, f"{context}.server_log_path"
    )
    if (
        server_log_path != data_directory.parent / "server_startup.log"
        or not stat.S_ISREG(server_log_metadata.st_mode)
    ):
        _fail(
            f"{context}.server_log_path",
            "server log is not the exact private-cluster log",
        )
    system_identifier = _string(
        _field(config, "expected_system_identifier", context),
        f"{context}.expected_system_identifier",
    )
    if not system_identifier.isdecimal() or int(system_identifier) <= 0:
        _fail(
            f"{context}.expected_system_identifier",
            "system identifier must be a positive decimal",
        )

    authorization_path = _configured_absolute_path(
        _field(config, "authorization_file", context),
        f"{context}.authorization_file",
    )
    (
        authorization_path,
        authorization_raw,
        authorization_metadata,
    ) = _validate_authorization_marker(
        authorization_path,
        data_directory=data_directory,
        context=f"{context}.authorization_file",
    )

    installed_paths = {
        "installed_dso_path": stage.library_path,
        "installed_control_dso_path": (
            stage.pkglibdir / runtime_control_dso_path.name
        ),
        "installed_control_path": stage.control_path,
        "installed_sql_path": stage.install_sql_path,
    }
    for field_name, expected_path in installed_paths.items():
        configured_path = _configured_absolute_path(
            _field(config, field_name, context),
            f"{context}.{field_name}",
        )
        if configured_path != expected_path:
            _fail(
                f"{context}.{field_name}",
                "installed path differs from the manifest-selected path",
            )

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
    return ClusterAuthoritySnapshot(
        profile=profile,
        cluster_system_identifier=system_identifier,
        data_directory=data_directory,
        terminal_marker_directory=terminal_marker_directory,
        server_log_path=server_log_path,
        socket_directory=socket_directory,
        connection_port=connection_port,
        connection_user=connection_user,
        original_authorization_path=authorization_path,
        authorization_raw=authorization_raw,
        authorization_mode=stat.S_IMODE(authorization_metadata.st_mode),
        authorization_uid=authorization_metadata.st_uid,
        authorization_sha256=_sha256_bytes(authorization_raw),
    )


def _profile_inputs(
    *,
    profile: str,
    ledger_path: Path,
    runtime_inventory_path: Path,
    runtime_inventory_producer_executable_path: Path,
    sql_manifest_path: Path,
    case_plan_path: Path,
    dso_path: Path,
    runtime_control_dso_path: Path,
    test_executable_path: Path,
    cluster_config_path: Path,
    stage_manifest_path: Path,
    candidate_root: Path,
) -> ProfileInputs:
    for path, context in (
        (ledger_path, f"{profile}-ledger"),
        (runtime_inventory_path, f"{profile}-runtime-inventory"),
        (
            runtime_inventory_producer_executable_path,
            "runtime-inventory-producer-executable",
        ),
        (sql_manifest_path, f"{profile}-sql-manifest"),
        (case_plan_path, f"{profile}-case-plan"),
        (dso_path, f"{profile}-dso"),
        (runtime_control_dso_path, "runtime-control-dso"),
        (test_executable_path, f"{profile}-test-executable"),
        (cluster_config_path, f"{profile}-cluster-config"),
        (stage_manifest_path, f"{profile}-stage-manifest"),
    ):
        if not path.is_file() or path.is_symlink():
            _fail(context, f"input is not an existing file: {path}")
    if not os.access(test_executable_path, os.X_OK):
        _fail(
            f"{profile}-test-executable",
            "profile runtime test artifact is not executable",
        )
    ledger = _parse_ledger(ledger_path, profile)
    runtime_inventory_producer_executable_sha256 = _sha256_file(
        runtime_inventory_producer_executable_path,
        "runtime-inventory-producer-executable",
    )
    runtime_inventory = _parse_runtime_inventory(
        runtime_inventory_path,
        profile=profile,
        ledger=ledger,
        producer_executable_sha256=(
            runtime_inventory_producer_executable_sha256
        ),
    )
    (
        sql_manifest_sha256,
        sql_script_sha256,
    ) = _parse_sql_manifest_binding(
        sql_manifest_path,
        profile,
    )
    if ledger.sql_manifest_sha256 != sql_manifest_sha256:
        _fail(
            f"{profile}-ledger",
            "ledger is bound to a different SQL manifest",
        )
    case_plan, case_plan_raw = _load_json(
        case_plan_path,
        f"{profile}-case-plan",
    )
    _require_exact_fields(
        case_plan,
        CASE_PLAN_FIELDS,
        f"{profile}-case-plan",
    )
    plan_cases = _array(
        _field(case_plan, "cases", f"{profile}-case-plan"),
        f"{profile}-case-plan.cases",
    )
    if (
        _integer(
            _field(case_plan, "schema_revision", f"{profile}-case-plan"),
            f"{profile}-case-plan.schema_revision",
        )
        != 2
        or _integer(
            _field(
                case_plan,
                "probe_abi_revision",
                f"{profile}-case-plan",
            ),
            f"{profile}-case-plan.probe_abi_revision",
        )
        != 2
        or _string(
            _field(case_plan, "profile", f"{profile}-case-plan"),
            f"{profile}-case-plan.profile",
        )
        != profile
        or _sha256(
            _field(case_plan, "ledger_sha256", f"{profile}-case-plan"),
            f"{profile}-case-plan.ledger_sha256",
        )
        != ledger.sha256
        or _sha256(
            _field(
                case_plan,
                "runtime_inventory_sha256",
                f"{profile}-case-plan",
            ),
            f"{profile}-case-plan.runtime_inventory_sha256",
        )
        != runtime_inventory.sha256
        or _sha256(
            _field(
                case_plan,
                "sql_manifest_sha256",
                f"{profile}-case-plan",
            ),
            f"{profile}-case-plan.sql_manifest_sha256",
        )
        != sql_manifest_sha256
        or len(plan_cases) != EXPECTED_PROFILE_CASE_COUNT
    ):
        _fail(
            f"{profile}-case-plan",
            "schema, profile, ledger, manifest, or cases differ",
        )
    _sha256(
        _field(
            case_plan,
            "template_sha256",
            f"{profile}-case-plan",
        ),
        f"{profile}-case-plan.template_sha256",
    )
    case_ids: set[str] = set()
    case_plan_entries: dict[str, Mapping[str, Any]] = {}
    invocation_fault_keys: set[tuple[str, str]] = set()
    for index, raw_case in enumerate(plan_cases):
        case_context = f"{profile}-case-plan.cases[{index}]"
        plan_case = _object(raw_case, case_context)
        _require_exact_fields(
            plan_case,
            CASE_PLAN_CASE_FIELDS,
            case_context,
        )
        catalog_entry = _object(
            _field(plan_case, "catalog", case_context),
            f"{case_context}.catalog",
        )
        _require_exact_fields(
            catalog_entry,
            CATALOG_ENTRY_FIELDS,
            f"{case_context}.catalog",
        )
        catalog_context = f"{case_context}.catalog"
        case_id = _string(
            _field(catalog_entry, "case_id", catalog_context),
            f"{catalog_context}.case_id",
        )
        if (
            CASE_ID_PATTERN.fullmatch(case_id) is None
            or case_id in case_ids
        ):
            _fail(
                catalog_context,
                "case_id is invalid or duplicated",
            )
        case_ids.add(case_id)
        case_plan_entries[case_id] = dict(plan_case)
        sql_identity = _string(
            _field(
                catalog_entry,
                "sql_identity",
                catalog_context,
            ),
            f"{catalog_context}.sql_identity",
        )
        invocation_key = _string(
            _field(
                catalog_entry,
                "invocation_key",
                catalog_context,
            ),
            f"{catalog_context}.invocation_key",
        )
        fault_key = _fault_key(
            _field(catalog_entry, "fault_key", catalog_context),
            f"{catalog_context}.fault_key",
        )
        inventory_entry = runtime_inventory.entries.get(
            (sql_identity, fault_key)
        )
        if inventory_entry is None:
            _fail(
                catalog_context,
                "case is absent from the current runtime inventory",
            )
        invocation_fault_key = (invocation_key, fault_key)
        if invocation_fault_key in invocation_fault_keys:
            _fail(
                catalog_context,
                "duplicate invocation/fault key",
            )
        invocation_fault_keys.add(invocation_fault_key)
        exact_inventory_fields = {
            "site_id": inventory_entry.site_id,
            "ledger_site_sha256": (
                inventory_entry.ledger_site_sha256
            ),
            "relation": inventory_entry.relation,
        }
        for field_name, expected_value in (
            exact_inventory_fields.items()
        ):
            if (
                _field(catalog_entry, field_name, catalog_context)
                != expected_value
            ):
                _fail(
                    catalog_context,
                    f"{field_name} differs from the runtime inventory",
                )
        plan_contract = _probe_contract(
            _field(
                catalog_entry,
                "probe_contract",
                catalog_context,
            ),
            inventory_entry.relation,
            f"{catalog_context}.probe_contract",
        )
        if plan_contract != dict(
            inventory_entry.probe_contract
        ):
            _fail(
                catalog_context,
                "probe_contract differs from the runtime inventory",
            )
        if _string(
            _field(
                catalog_entry,
                "postgres_severity",
                catalog_context,
            ),
            f"{catalog_context}.postgres_severity",
        ) != "error":
            _fail(
                catalog_context,
                "runtime plans cannot exercise FATAL or PANIC",
            )
        _plan_expected(
            _field(catalog_entry, "expected", catalog_context),
            site=inventory_entry.ledger_site,
            fault_key=fault_key,
            context=f"{catalog_context}.expected",
        )
        execution = _object(
            _field(plan_case, "execution", case_context),
            f"{case_context}.execution",
        )
        _plan_execution(
            execution,
            sql_identity=sql_identity,
            fault_key=fault_key,
            context=f"{case_context}.execution",
        )
    case_plan_sha256 = _sha256_bytes(case_plan_raw)
    case_plan_runtime_policy_sha256 = _sha256(
        _field(
            case_plan,
            "runtime_policy_sha256",
            f"{profile}-case-plan",
        ),
        f"{profile}-case-plan.runtime_policy_sha256",
    )
    dso_sha256 = _sha256_file(dso_path, f"{profile}-dso")
    runtime_control_dso_sha256 = _sha256_file(
        runtime_control_dso_path,
        "runtime-control-dso",
    )
    if dso_sha256 == runtime_control_dso_sha256:
        _fail(
            "runtime-control-dso",
            "runtime control helper must differ from the profile DSO",
        )
    stage = _parse_stage_manifest(
        stage_manifest_path,
        profile=profile,
        dso_sha256=dso_sha256,
        sql_script_sha256=sql_script_sha256,
    )
    config, config_raw = _load_json(
        cluster_config_path,
        f"{profile}-cluster-config",
    )
    cluster_authority = _validate_cluster_config_authority(
        config,
        profile=profile,
        candidate_root=candidate_root,
        stage=stage,
        stage_manifest_path=stage_manifest_path,
        runtime_control_dso_path=runtime_control_dso_path,
        context=f"{profile}-cluster-config",
    )
    return ProfileInputs(
        profile=profile,
        ledger=ledger,
        runtime_inventory=runtime_inventory,
        runtime_inventory_path=runtime_inventory.path,
        runtime_inventory_sha256=runtime_inventory.sha256,
        sql_manifest_path=sql_manifest_path.resolve(),
        sql_manifest_sha256=sql_manifest_sha256,
        sql_script_sha256=sql_script_sha256,
        case_plan_path=case_plan_path.resolve(),
        case_plan_sha256=case_plan_sha256,
        case_plan_runtime_policy_sha256=(
            case_plan_runtime_policy_sha256
        ),
        case_plan_entries=case_plan_entries,
        dso_path=dso_path.resolve(),
        dso_sha256=dso_sha256,
        runtime_control_dso_path=runtime_control_dso_path.resolve(),
        runtime_control_dso_sha256=runtime_control_dso_sha256,
        test_executable_path=test_executable_path.resolve(),
        test_executable_sha256=_sha256_file(
            test_executable_path,
            f"{profile}-test-executable",
        ),
        cluster_config_path=cluster_config_path.resolve(),
        cluster_config_sha256=_sha256_file(
            cluster_config_path,
            f"{profile}-cluster-config",
        ),
        stage_manifest_path=stage.manifest_path,
        stage_manifest_sha256=stage.manifest_sha256,
        tool_signature_suite_sha256=(
            stage.tool_signature_suite_sha256
        ),
        stage=stage,
        cluster_authority=cluster_authority,
    )


def _validate_distinct_profile_artifacts(
    production: ProfileInputs,
    test_indexes: ProfileInputs,
) -> None:
    if production.dso_path == test_indexes.dso_path:
        _fail("profile-artifacts", "profile DSO paths must be distinct")
    if production.dso_sha256 == test_indexes.dso_sha256:
        _fail("profile-artifacts", "profile DSO bytes must be distinct")
    if production.test_executable_path == test_indexes.test_executable_path:
        _fail(
            "profile-artifacts",
            "profile runtime-test executable paths must be distinct",
        )
    if (
        production.test_executable_sha256
        == test_indexes.test_executable_sha256
    ):
        _fail(
            "profile-artifacts",
            "profile runtime-test executable bytes must be distinct",
        )
    if (
        production.stage_manifest_path
        == test_indexes.stage_manifest_path
        or production.stage_manifest_sha256
        == test_indexes.stage_manifest_sha256
    ):
        _fail(
            "profile-artifacts",
            "profile stage manifests and bytes must be distinct",
        )
    if (
        production.runtime_control_dso_path
        != test_indexes.runtime_control_dso_path
        or production.runtime_control_dso_sha256
        != test_indexes.runtime_control_dso_sha256
    ):
        _fail(
            "profile-artifacts",
            "both profiles must use one current runtime control helper",
        )
    if (
        production.cluster_authority.cluster_system_identifier
        == test_indexes.cluster_authority.cluster_system_identifier
        or production.cluster_authority.data_directory
        == test_indexes.cluster_authority.data_directory
        or production.cluster_authority.socket_directory
        == test_indexes.cluster_authority.socket_directory
        or production.cluster_authority.original_authorization_path
        == test_indexes.cluster_authority.original_authorization_path
    ):
        _fail(
            "profile-artifacts",
            "profile cluster authority coordinates must be distinct",
        )


def _run_evidence(arguments: argparse.Namespace) -> int:
    producer_executable_path = Path(__file__).resolve()
    producer_executable_sha256 = _sha256_file(
        producer_executable_path,
        "runtime-producer-executable",
    )
    output_matrix = _absolute_path(arguments.output_matrix)
    artifact_directory = _absolute_path(arguments.output_directory)
    candidate_root = output_matrix.parent
    if (
        output_matrix.name != "runtime-matrix.json"
        or artifact_directory != candidate_root / "evidence"
    ):
        _fail(
            "runtime-output",
            "matrix and immutable artifacts must use "
            "<candidate>/runtime-matrix.json and <candidate>/evidence",
        )
    candidate_descriptor = _open_directory(
        candidate_root,
        "runtime-candidate-root",
    )
    os.close(candidate_descriptor)
    artifact_directory = _secure_descendant_directory(
        candidate_root,
        artifact_directory,
        "runtime-evidence-root",
    )
    stamp_path = (
        None
        if arguments.stamp is None
        else _absolute_path(arguments.stamp)
    )
    generated_outputs = {"runtime-matrix": output_matrix}
    if stamp_path is not None:
        generated_outputs["runtime-stamp"] = stamp_path
    _validate_generated_output_paths(
        generated_outputs,
        {
            "runtime-producer-executable": producer_executable_path,
            "artifact-directory": artifact_directory,
            "production-ledger": arguments.production_ledger,
            "test-indexes-ledger": arguments.test_indexes_ledger,
            "production-runtime-inventory": (
                arguments.production_runtime_inventory
            ),
            "test-indexes-runtime-inventory": (
                arguments.test_indexes_runtime_inventory
            ),
            "runtime-inventory-producer-executable": (
                arguments.runtime_inventory_producer_executable
            ),
            "production-sql-manifest": arguments.production_sql_manifest,
            "test-indexes-sql-manifest": (
                arguments.test_indexes_sql_manifest
            ),
            "production-case-plan": arguments.production_case_plan,
            "test-indexes-case-plan": arguments.test_indexes_case_plan,
            "runtime-policy": arguments.runtime_policy,
            "production-dso": arguments.production_dso,
            "test-indexes-dso": arguments.test_indexes_dso,
            "production-test-executable": (
                arguments.production_test_executable
            ),
            "test-indexes-test-executable": (
                arguments.test_indexes_test_executable
            ),
            "production-cluster-config": (
                arguments.production_cluster_config
            ),
            "test-indexes-cluster-config": (
                arguments.test_indexes_cluster_config
            ),
            "production-stage-manifest": (
                arguments.production_stage_manifest
            ),
            "test-indexes-stage-manifest": (
                arguments.test_indexes_stage_manifest
            ),
            "runtime-control-dso": arguments.runtime_control_dso,
        },
    )
    production = _profile_inputs(
        profile="production",
        ledger_path=arguments.production_ledger,
        runtime_inventory_path=(
            arguments.production_runtime_inventory
        ),
        runtime_inventory_producer_executable_path=(
            arguments.runtime_inventory_producer_executable
        ),
        sql_manifest_path=arguments.production_sql_manifest,
        case_plan_path=arguments.production_case_plan,
        dso_path=arguments.production_dso,
        runtime_control_dso_path=arguments.runtime_control_dso,
        test_executable_path=arguments.production_test_executable,
        cluster_config_path=arguments.production_cluster_config,
        stage_manifest_path=arguments.production_stage_manifest,
        candidate_root=candidate_root,
    )
    test_indexes = _profile_inputs(
        profile="test_indexes",
        ledger_path=arguments.test_indexes_ledger,
        runtime_inventory_path=(
            arguments.test_indexes_runtime_inventory
        ),
        runtime_inventory_producer_executable_path=(
            arguments.runtime_inventory_producer_executable
        ),
        sql_manifest_path=arguments.test_indexes_sql_manifest,
        case_plan_path=arguments.test_indexes_case_plan,
        dso_path=arguments.test_indexes_dso,
        runtime_control_dso_path=arguments.runtime_control_dso,
        test_executable_path=arguments.test_indexes_test_executable,
        cluster_config_path=arguments.test_indexes_cluster_config,
        stage_manifest_path=arguments.test_indexes_stage_manifest,
        candidate_root=candidate_root,
    )
    _validate_distinct_profile_artifacts(production, test_indexes)
    published_authorities: dict[str, PublishedClusterAuthority] = {}
    for inputs in (production, test_indexes):
        published_authorities[inputs.profile] = (
            _publish_cluster_authority(
                inputs.cluster_authority,
                artifact_directory=artifact_directory,
                matrix_path=output_matrix,
            )
        )
        config_raw = _read_single_link_file(
            inputs.cluster_config_path,
            f"{inputs.profile}-cluster-config-publication",
        )
        immutable_config_path, immutable_config_sha256 = _write_immutable(
            artifact_directory / "cluster_configs" / inputs.profile,
            "cluster_config",
            "json",
            config_raw,
            artifact_root=artifact_directory,
        )
        if immutable_config_sha256 != inputs.cluster_config_sha256:
            _fail(
                f"{inputs.profile}-cluster-config",
                "cluster configuration changed while publishing",
            )
        if inputs.profile == "production":
            production = replace(
                inputs,
                cluster_config_path=immutable_config_path,
            )
        else:
            test_indexes = replace(
                inputs,
                cluster_config_path=immutable_config_path,
            )
    ledgers = {
        production.profile: production.ledger,
        test_indexes.profile: test_indexes.ledger,
    }
    policy, runtime_policy_sha256 = _parse_runtime_policy(
        arguments.runtime_policy
    )
    if (
        production.case_plan_runtime_policy_sha256
        != runtime_policy_sha256
        or test_indexes.case_plan_runtime_policy_sha256
        != runtime_policy_sha256
    ):
        _fail(
            "runtime-policy",
            "compiled case plans are bound to a different runtime policy",
        )
    required = _required_coverage(ledgers, policy)

    with tempfile.TemporaryDirectory(
        prefix="bioseq-safety-runtime-",
    ) as temporary:
        work_directory = Path(temporary).resolve()
        profile_results: dict[
            str,
            tuple[
                ProfileInputs,
                ActivationReceipt,
                tuple[CatalogCase, ...],
                Path,
                str,
            ],
        ] = {}
        matrix_cases: list[dict[str, Any]] = []
        # Keep publication deterministic even though the profiles use
        # separate private clusters.
        for inputs in (production, test_indexes):
            activation, _, _ = _activate_profile(
                inputs,
                work_directory=work_directory,
                artifact_directory=artifact_directory,
                timeout_seconds=arguments.timeout_seconds,
            )
            cases, catalog_path, catalog_sha256, _, _ = _describe_profile(
                inputs,
                required=required,
                work_directory=work_directory,
                artifact_directory=artifact_directory,
                timeout_seconds=arguments.timeout_seconds,
            )
            if len(cases) != EXPECTED_PROFILE_CASE_COUNT:
                _fail(
                    f"{inputs.profile}-case-catalog",
                    f"requires exactly {EXPECTED_PROFILE_CASE_COUNT} "
                    "cases",
                )
            for case in cases:
                matrix_cases.append(
                    _run_case(
                        case=case,
                        inputs=inputs,
                        activation=activation,
                        catalog_path=catalog_path,
                        catalog_sha256=catalog_sha256,
                        producer_executable_path=producer_executable_path,
                        producer_executable_sha256=(
                            producer_executable_sha256
                        ),
                        work_directory=work_directory,
                        artifact_directory=artifact_directory,
                        matrix_path=output_matrix,
                        timeout_seconds=arguments.timeout_seconds,
                    )
                )
            profile_results[inputs.profile] = (
                inputs,
                activation,
                cases,
                catalog_path,
                catalog_sha256,
            )
        if (
            len(matrix_cases) != EXPECTED_MATRIX_CASE_COUNT
            or {
                profile: sum(
                    1
                    for case in matrix_cases
                    if case.get("profile") == profile
                )
                for profile in PROFILES
            }
            != {
                profile: EXPECTED_PROFILE_CASE_COUNT
                for profile in PROFILES
            }
        ):
            _fail(
                "runtime-matrix",
                f"requires exactly {EXPECTED_MATRIX_CASE_COUNT} rows "
                f"and {EXPECTED_PROFILE_CASE_COUNT} per profile",
            )
        _validate_cluster_relationship(
            profile_results["production"][1],
            profile_results["test_indexes"][1],
            arguments.cluster_mode,
        )

    matrix = {
        "schema_revision": 2,
        "production_ledger_sha256": production.ledger.sha256,
        "test_indexes_ledger_sha256": test_indexes.ledger.sha256,
        "production_sql_manifest_sha256": (
            production.sql_manifest_sha256
        ),
        "test_indexes_sql_manifest_sha256": (
            test_indexes.sql_manifest_sha256
        ),
        "production_case_plan_sha256": (
            production.case_plan_sha256
        ),
        "test_indexes_case_plan_sha256": (
            test_indexes.case_plan_sha256
        ),
        "production_runtime_inventory_sha256": (
            production.runtime_inventory_sha256
        ),
        "test_indexes_runtime_inventory_sha256": (
            test_indexes.runtime_inventory_sha256
        ),
        "runtime_policy_sha256": runtime_policy_sha256,
        "production_stage_manifest_sha256": (
            production.stage_manifest_sha256
        ),
        "test_indexes_stage_manifest_sha256": (
            test_indexes.stage_manifest_sha256
        ),
        "production_tool_signature_suite_sha256": (
            production.tool_signature_suite_sha256
        ),
        "test_indexes_tool_signature_suite_sha256": (
            test_indexes.tool_signature_suite_sha256
        ),
        "production_cluster_authority_path": _relative_artifact_path(
            published_authorities["production"].record_path,
            output_matrix,
        ),
        "production_cluster_authority_sha256": (
            published_authorities["production"].record_sha256
        ),
        "test_indexes_cluster_authority_path": _relative_artifact_path(
            published_authorities["test_indexes"].record_path,
            output_matrix,
        ),
        "test_indexes_cluster_authority_sha256": (
            published_authorities["test_indexes"].record_sha256
        ),
        "production_dso_sha256": production.dso_sha256,
        "test_indexes_dso_sha256": test_indexes.dso_sha256,
        "runtime_control_dso_sha256": (
            production.runtime_control_dso_sha256
        ),
        "runtime_producer_executable_sha256": (
            producer_executable_sha256
        ),
        "production_test_executable_sha256": (
            production.test_executable_sha256
        ),
        "test_indexes_test_executable_sha256": (
            test_indexes.test_executable_sha256
        ),
        "cases": sorted(
            matrix_cases,
            key=lambda case: (case["profile"], case["case_id"]),
        ),
    }
    matrix_raw = _canonical_json(matrix)
    matrix_sha256 = _sha256_bytes(matrix_raw)
    _replace_generated(output_matrix, matrix_raw)
    if (
        _read_single_link_file(output_matrix, "runtime-matrix")
        != matrix_raw
    ):
        _fail("runtime-matrix", "published bytes differ from the matrix")
    if stamp_path is not None:
        stamp = {
            "schema_revision": 1,
            "runtime_matrix_path": str(output_matrix),
            "runtime_matrix_sha256": matrix_sha256,
            "runtime_producer_executable_sha256": (
                producer_executable_sha256
            ),
            "case_count": len(matrix_cases),
            "coverage_key_count": len(required),
        }
        _replace_generated(stamp_path, _canonical_json(stamp))
    return 0


def _plan_sql_array(value: Any, context: str) -> list[str]:
    statements = _array(value, context)
    if len(statements) > 64:
        _fail(context, "contains more than 64 SQL statements")
    result: list[str] = []
    for index, raw_statement in enumerate(statements):
        statement = _string(raw_statement, f"{context}[{index}]")
        if len(statement.encode("utf-8")) > 1024 * 1024:
            _fail(f"{context}[{index}]", "SQL statement exceeds 1 MiB")
        result.append(statement)
    return result


def _plan_expected(
    value: Any,
    *,
    site: LedgerSite,
    fault_key: str,
    context: str,
) -> dict[str, Any]:
    expected = _object(value, context)
    _require_exact_fields(expected, EXPECTED_RESULT_FIELDS, context)
    backend_outcome = _string(
        _field(expected, "backend_outcome", context),
        f"{context}.backend_outcome",
    )
    if backend_outcome not in {"recoverable_error", "backend_exit"}:
        _fail(
            context,
            "backend_outcome must be recoverable_error or backend_exit",
        )
    backend_exit = backend_outcome == "backend_exit"
    destructors = sorted(
        _string_list(
            _field(expected, "destructor_sentinels", context),
            f"{context}.destructor_sentinels",
            allow_empty=True,
        )
    )
    if backend_exit and destructors:
        _fail(
            context,
            "backend-exit cases cannot claim unobservable destructors",
        )
    if (
        not backend_exit
        and not destructors
        and site.classification != "lifetime_free_direct"
    ):
        _fail(
            context,
            "only lifetime_free_direct may use no C++ destructor sentinel",
        )
    if (
        not backend_exit
        and destructors
        and site.classification == "lifetime_free_direct"
    ):
        _fail(
            context,
            "lifetime_free_direct must preserve an empty C++ lifetime set",
        )
    cleanup = sorted(
        _string_list(
            _field(expected, "cleanup_events", context),
            f"{context}.cleanup_events",
            allow_empty=True,
        )
    )
    postgres_cleanup = sorted(
        _string_list(
            _field(expected, "postgres_cleanup_events", context),
            f"{context}.postgres_cleanup_events",
            allow_empty=(
                backend_exit
                or fault_key == BACKEND_SCOPED_VACUUM_FAULT_KEY
            ),
        )
    )
    event_pattern = re.compile(r"[a-z0-9][a-z0-9_.]{0,63}")
    for event in destructors + cleanup + postgres_cleanup:
        if event_pattern.fullmatch(event) is None:
            _fail(context, f"invalid runtime event name {event!r}")
    if backend_exit:
        if cleanup or postgres_cleanup:
            _fail(
                context,
                "backend-exit cases cannot claim unobservable cleanup",
            )
    elif fault_key == BACKEND_SCOPED_VACUUM_FAULT_KEY:
        if postgres_cleanup:
            _fail(
                context,
                "backend-scoped VACUUM must expect no FMGR cleanup",
            )
    elif "fmgr_abort_observed" not in postgres_cleanup:
        _fail(context, "PostgreSQL cleanup must include FMGR ABORT")
    raw_sqlstate = _field(expected, "sqlstate", context)
    raw_message = _field(expected, "message", context)
    if backend_exit:
        if raw_sqlstate is not None or raw_message is not None:
            _fail(
                context,
                "backend-exit SQLSTATE/message must be null because the "
                "backend-local error result is unobservable",
            )
        sqlstate = None
        message = None
    else:
        sqlstate = _string(raw_sqlstate, f"{context}.sqlstate")
        if SQLSTATE_PATTERN.fullmatch(sqlstate) is None:
            _fail(context, "expected SQLSTATE is not five characters")
        message = _string(raw_message, f"{context}.message")
    return {
        "backend_outcome": backend_outcome,
        "destructor_sentinels": destructors,
        "cleanup_events": cleanup,
        "postgres_cleanup_events": postgres_cleanup,
        "sqlstate": sqlstate,
        "message": message,
        "recovery_postcondition": _string(
            _field(expected, "recovery_postcondition", context),
            f"{context}.recovery_postcondition",
        ),
    }


def _plan_execution(
    value: Any,
    *,
    sql_identity: str,
    fault_key: str,
    context: str,
) -> dict[str, Any]:
    execution = _object(value, context)
    allowed = EXECUTION_FIELDS | PRE_FAULT_CONTROL_EXECUTION_FIELDS
    unknown = set(execution) - allowed
    if unknown:
        _fail(context, "unknown fields: " + ", ".join(sorted(unknown)))
    present_control_fields = (
        set(execution) & PRE_FAULT_CONTROL_EXECUTION_FIELDS
    )
    if present_control_fields and (
        present_control_fields
        != PRE_FAULT_CONTROL_EXECUTION_FIELDS
    ):
        _fail(
            context,
            "pre-fault control fields must be present together",
        )
    backend_scoped_vacuum = (
        fault_key == BACKEND_SCOPED_VACUUM_FAULT_KEY
    )
    if backend_scoped_vacuum != bool(present_control_fields):
        _fail(
            context,
            "backend-scoped VACUUM requires its sealed success control",
        )
    target = _string(
        _field(execution, "target_regprocedure", context),
        f"{context}.target_regprocedure",
    )
    if target != sql_identity:
        _fail(context, "target_regprocedure differs from SQL identity")
    invocation = _string(
        _field(execution, "invocation_sql", context),
        f"{context}.invocation_sql",
    )
    recovery = _string(
        _field(execution, "recovery_sql", context),
        f"{context}.recovery_sql",
    )
    if (
        len(invocation.encode("utf-8")) > 1024 * 1024
        or len(recovery.encode("utf-8")) > 1024 * 1024
    ):
        _fail(context, "invocation or recovery SQL exceeds 1 MiB")
    result = {
        "target_regprocedure": target,
        "setup_sql": _plan_sql_array(
            execution.get("setup_sql", []),
            f"{context}.setup_sql",
        ),
        "invocation_sql": invocation,
        "post_error_sql": _plan_sql_array(
            execution.get("post_error_sql", []),
            f"{context}.post_error_sql",
        ),
        "recovery_sql": recovery,
        "recovery_expected_rows": _row_matrix(
            _field(execution, "recovery_expected_rows", context),
            f"{context}.recovery_expected_rows",
        ),
        "cleanup_sql": _plan_sql_array(
            execution.get("cleanup_sql", []),
            f"{context}.cleanup_sql",
        ),
    }
    if present_control_fields:
        control_sql = _plan_sql_array(
            _field(
                execution,
                "pre_fault_control_sql",
                context,
            ),
            f"{context}.pre_fault_control_sql",
        )
        if not control_sql:
            _fail(
                context,
                "pre_fault_control_sql must not be empty",
            )
        control_recovery = _string(
            _field(
                execution,
                "pre_fault_control_recovery_sql",
                context,
            ),
            f"{context}.pre_fault_control_recovery_sql",
        )
        if len(control_recovery.encode("utf-8")) > 1024 * 1024:
            _fail(context, "pre-fault control recovery SQL exceeds 1 MiB")
        result.update(
            {
                "pre_fault_control_sql": control_sql,
                "pre_fault_control_recovery_sql": (
                    control_recovery
                ),
                "pre_fault_control_recovery_expected_rows": (
                    _row_matrix(
                        _field(
                            execution,
                            "pre_fault_control_recovery_expected_rows",
                            context,
                        ),
                        (
                            f"{context}."
                            "pre_fault_control_recovery_expected_rows"
                        ),
                    )
                ),
            }
        )
    return result


def _runtime_template_case(
    value: Any,
    context: str,
) -> dict[str, Any]:
    template_case = _object(value, context)
    authored_authority = sorted(
        set(template_case) & FORBIDDEN_TEMPLATE_AUTHORITY_FIELDS
    )
    if authored_authority:
        _fail(
            context,
            "authored runtime authority is forbidden: "
            + ", ".join(authored_authority),
        )
    _require_exact_fields(
        template_case,
        TEMPLATE_CASE_FIELDS,
        context,
    )
    return template_case


def _validate_runtime_template_inventory(
    template: Mapping[str, Any],
    context: str,
) -> list[Any]:
    _require_exact_fields(
        template,
        RUNTIME_TEMPLATE_FIELDS,
        context,
    )
    if _integer(
        _field(template, "schema_revision", context),
        f"{context}.schema_revision",
    ) != 1:
        _fail(context, "unsupported schema revision")
    raw_cases = _array(
        _field(template, "cases", context),
        f"{context}.cases",
    )
    if len(raw_cases) != EXPECTED_TEMPLATE_COUNT:
        _fail(
            context,
            f"requires exactly {EXPECTED_TEMPLATE_COUNT} templates",
        )
    case_ids: set[str] = set()
    for index, raw_case in enumerate(raw_cases):
        case_context = f"{context}.cases[{index}]"
        template_case = _runtime_template_case(raw_case, case_context)
        if _string_list(
            _field(template_case, "profiles", case_context),
            f"{case_context}.profiles",
        ) != list(PROFILES):
            _fail(
                case_context,
                "every template must name both profiles in canonical order",
            )
        case_id = _string(
            _field(template_case, "case_id", case_context),
            f"{case_context}.case_id",
        )
        if CASE_ID_PATTERN.fullmatch(case_id) is None or case_id in case_ids:
            _fail(case_context, "template case ID is invalid or duplicate")
        case_ids.add(case_id)
    return raw_cases


def _generate_plan(arguments: argparse.Namespace) -> int:
    profile = arguments.profile
    if profile not in PROFILES:
        _fail("--profile", "must be production or test_indexes")
    source_paths = (
        arguments.ledger,
        arguments.runtime_inventory,
        arguments.runtime_inventory_producer_executable,
        arguments.sql_manifest,
        arguments.sql_file,
        arguments.control_file,
        arguments.control_dso,
        arguments.runtime_policy,
        arguments.template,
    )
    for path in source_paths:
        if (
            not path.is_absolute()
            or not path.is_file()
            or path.is_symlink()
        ):
            _fail(
                "plan-input",
                f"expected an absolute regular non-symlink file: {path}",
            )
    ledger = _parse_ledger(arguments.ledger, profile)
    inventory_producer_executable_sha256 = _sha256_file(
        arguments.runtime_inventory_producer_executable,
        "runtime-inventory-producer-executable",
    )
    inventory = _parse_runtime_inventory(
        arguments.runtime_inventory,
        profile=profile,
        ledger=ledger,
        producer_executable_sha256=(
            inventory_producer_executable_sha256
        ),
    )
    (
        sql_manifest_sha256,
        sql_script_sha256,
    ) = _parse_sql_manifest_binding(
        arguments.sql_manifest,
        profile,
    )
    if ledger.sql_manifest_sha256 != sql_manifest_sha256:
        _fail("plan-input", "ledger and SQL manifest binding differs")
    if _sha256_file(arguments.sql_file, "plan SQL file") != sql_script_sha256:
        _fail("plan-input", "SQL file differs from its manifest")
    policy, runtime_policy_sha256 = _parse_runtime_policy(
        arguments.runtime_policy
    )
    profile_policy = frozenset(
        key for key in policy if key[0] == profile
    )
    missing_policy = profile_policy - ledger.coverage
    if missing_policy:
        _fail(
            "runtime-policy",
            "claims keys absent from the profile ledger: "
            + ", ".join("/".join(key) for key in sorted(missing_policy)),
        )
    required = ledger.coverage | profile_policy
    template, template_raw = _load_json(
        arguments.template,
        "runtime-case-template",
    )
    raw_cases = _validate_runtime_template_inventory(
        template,
        "runtime-case-template",
    )

    generated_cases: list[dict[str, Any]] = []
    template_case_ids: set[str] = set()
    case_ids: set[str] = set()
    invocation_fault_keys: set[tuple[str, str]] = set()
    observed_coverage: set[CoverageKey] = set()
    for index, raw_case in enumerate(raw_cases):
        context = f"runtime-case-template.cases[{index}]"
        template_case = _runtime_template_case(
            raw_case,
            context,
        )
        profiles = _string_list(
            _field(template_case, "profiles", context),
            f"{context}.profiles",
        )
        if profiles != list(PROFILES):
            _fail(
                context,
                "every template must name both profiles in canonical order",
            )
        base_case_id = _string(
            _field(template_case, "case_id", context),
            f"{context}.case_id",
        )
        if (
            CASE_ID_PATTERN.fullmatch(base_case_id) is None
            or base_case_id in template_case_ids
        ):
            _fail(context, "template case ID is invalid or duplicate")
        template_case_ids.add(base_case_id)
        if profile not in profiles:
            continue
        generated_case_id = f"{profile}.{base_case_id}"
        if (
            CASE_ID_PATTERN.fullmatch(generated_case_id) is None
            or generated_case_id in case_ids
        ):
            _fail(context, "generated case ID is invalid or duplicate")
        case_ids.add(generated_case_id)
        sql_identity = _string(
            _field(template_case, "sql_identity", context),
            f"{context}.sql_identity",
        )
        invocation_key = _string(
            _field(template_case, "invocation_key", context),
            f"{context}.invocation_key",
        )
        fault_key = _fault_key(
            _field(template_case, "fault_key", context),
            f"{context}.fault_key",
        )
        inventory_entry = inventory.entries.get(
            (sql_identity, fault_key)
        )
        if inventory_entry is None:
            _fail(
                context,
                "template key is absent from the runtime inventory",
            )
        site = inventory_entry.ledger_site
        if (invocation_key, fault_key) in invocation_fault_keys:
            _fail(context, "duplicate invocation/fault key")
        invocation_fault_keys.add((invocation_key, fault_key))
        if _string(
            _field(template_case, "postgres_severity", context),
            f"{context}.postgres_severity",
        ) != "error":
            _fail(context, "runtime plans cannot exercise FATAL or PANIC")
        coverage = tuple(sorted(site.coverage(profile)))
        expected = _plan_expected(
            _field(template_case, "expected", context),
            site=site,
            fault_key=fault_key,
            context=f"{context}.expected",
        )
        execution = _plan_execution(
            _field(template_case, "execution", context),
            sql_identity=sql_identity,
            fault_key=fault_key,
            context=f"{context}.execution",
        )
        generated_cases.append(
            {
                "catalog": {
                    "case_id": generated_case_id,
                    "sql_identity": sql_identity,
                    "site_id": inventory_entry.site_id,
                    "ledger_site_sha256": (
                        inventory_entry.ledger_site_sha256
                    ),
                    "invocation_key": invocation_key,
                    "fault_key": fault_key,
                    "relation": inventory_entry.relation,
                    "probe_contract": dict(
                        inventory_entry.probe_contract
                    ),
                    "postgres_severity": "error",
                    "expected": expected,
                },
                "execution": execution,
            }
        )
        observed_coverage.update(coverage)
    if len(generated_cases) != EXPECTED_PROFILE_CASE_COUNT:
        _fail(
            "runtime-case-template",
            f"must generate exactly {EXPECTED_PROFILE_CASE_COUNT} "
            f"cases for {profile}",
        )
    if observed_coverage != required:
        missing = sorted(required - observed_coverage)
        extra = sorted(observed_coverage - required)
        _fail(
            "runtime-case-template",
            "profile cases do not close exact ledger/policy coverage; "
            + "missing="
            + ",".join("/".join(key) for key in missing)
            + " extra="
            + ",".join("/".join(key) for key in extra),
        )
    plan = {
        "schema_revision": 2,
        "profile": profile,
        "probe_abi_revision": 2,
        "template_sha256": _sha256_bytes(template_raw),
        "ledger_sha256": ledger.sha256,
        "runtime_inventory_sha256": inventory.sha256,
        "sql_manifest_sha256": sql_manifest_sha256,
        "runtime_policy_sha256": runtime_policy_sha256,
        "cases": sorted(
            generated_cases,
            key=lambda case: case["catalog"]["case_id"],
        ),
    }
    plan_raw = _canonical_json(plan)
    plan_sha256 = _sha256_bytes(plan_raw)
    plan_path = arguments.output_plan.resolve()
    header_path = arguments.output_header.resolve()
    stamp_path = arguments.stamp.resolve()
    if len({plan_path, header_path, stamp_path}) != 3:
        _fail("plan-output", "plan, header, and stamp paths must differ")
    bindings = {
        "BIOSEQ_SAFETY_RUNTIME_PROFILE": profile,
        "BIOSEQ_SAFETY_RUNTIME_CASE_PLAN": str(plan_path),
        "BIOSEQ_SAFETY_RUNTIME_CASE_PLAN_SHA256": plan_sha256,
        "BIOSEQ_SAFETY_RUNTIME_INVENTORY": str(
            inventory.path
        ),
        "BIOSEQ_SAFETY_RUNTIME_INVENTORY_SHA256": (
            inventory.sha256
        ),
        "BIOSEQ_SAFETY_RUNTIME_CONTROL_FILE": str(
            arguments.control_file.resolve()
        ),
        "BIOSEQ_SAFETY_RUNTIME_SQL_FILE": str(
            arguments.sql_file.resolve()
        ),
        "BIOSEQ_SAFETY_RUNTIME_SQL_MANIFEST": str(
            arguments.sql_manifest.resolve()
        ),
        "BIOSEQ_SAFETY_RUNTIME_CONTROL_DSO": str(
            arguments.control_dso.resolve()
        ),
    }
    header_lines = [
        "#pragma once",
        "#define BIOSEQ_SAFETY_RUNTIME_BINDING_VERSION 2",
    ]
    header_lines.extend(
        f"#define {name} {json.dumps(value, ensure_ascii=True)}"
        for name, value in bindings.items()
    )
    header_raw = ("\n".join(header_lines) + "\n").encode("utf-8")
    stamp = {
        "schema_revision": 2,
        "profile": profile,
        "plan_path": str(plan_path),
        "plan_sha256": plan_sha256,
        "binding_header_path": str(header_path),
        "binding_header_sha256": _sha256_bytes(header_raw),
        "template_sha256": _sha256_bytes(template_raw),
        "ledger_sha256": ledger.sha256,
        "runtime_inventory_sha256": inventory.sha256,
        "runtime_inventory_producer_executable_sha256": (
            inventory_producer_executable_sha256
        ),
        "sql_manifest_sha256": sql_manifest_sha256,
        "sql_script_sha256": sql_script_sha256,
        "runtime_policy_sha256": runtime_policy_sha256,
        "control_file_sha256": _sha256_file(
            arguments.control_file,
            "plan control file",
        ),
        "control_dso_sha256": _sha256_file(
            arguments.control_dso,
            "plan control DSO",
        ),
    }
    _replace_generated(plan_path, plan_raw)
    _replace_generated(header_path, header_raw)
    _replace_generated(stamp_path, _canonical_json(stamp))
    return 0


def _expect_failure(
    operation: Callable[[], Any],
    diagnostic: str,
) -> None:
    try:
        operation()
    except EvidenceError as error:
        if diagnostic not in str(error):
            raise RuntimeError(
                f"self-test expected {diagnostic!r}, got {error}"
            ) from error
        return
    raise RuntimeError(
        f"self-test expected failure containing {diagnostic!r}"
    )


def _expect_closed_revision_rejections(
    fields: frozenset[str],
    *,
    field_name: str,
    legacy_field_name: str | None,
    expected_revision: int,
    context: str,
) -> None:
    baseline = {name: None for name in fields}
    baseline[field_name] = expected_revision

    def validate(candidate: Mapping[str, Any]) -> None:
        _require_exact_fields(candidate, fields, context)
        if _integer(
            _field(candidate, field_name, context),
            f"{context}.{field_name}",
        ) != expected_revision:
            _fail(context, f"unsupported {field_name}")

    missing = dict(baseline)
    missing.pop(field_name)
    _expect_failure(lambda: validate(missing), field_name)

    wrong_type = dict(baseline)
    wrong_type[field_name] = str(expected_revision)
    _expect_failure(lambda: validate(wrong_type), "expected an integer")

    wrong_revision = dict(baseline)
    wrong_revision[field_name] = expected_revision + 1
    _expect_failure(
        lambda: validate(wrong_revision),
        f"unsupported {field_name}",
    )

    if legacy_field_name is not None:
        legacy = dict(baseline)
        legacy[legacy_field_name] = legacy.pop(field_name)
        _expect_failure(lambda: validate(legacy), field_name)


def _run_output_security_self_tests() -> None:
    with tempfile.TemporaryDirectory(
        prefix="bioseq-runtime-output-security-",
    ) as temporary:
        root = Path(temporary).resolve(strict=True)
        outside = root / "outside"
        outside.mkdir()
        sentinel = outside / "sentinel"
        sentinel.write_bytes(b"outside-sentinel")

        candidate = root / "candidate"
        candidate.mkdir()
        evidence = _secure_descendant_directory(
            candidate,
            candidate / "evidence",
            "self-test-evidence-root",
        )
        immutable_path, _ = _write_immutable(
            evidence / "results" / "production",
            "case",
            "json",
            b"{}\n",
            artifact_root=evidence,
        )
        if immutable_path.read_bytes() != b"{}\n":
            raise RuntimeError("self-test immutable publication changed bytes")

        external_immutable = outside / "immutable-hardlink"
        os.link(immutable_path, external_immutable)
        _expect_failure(
            lambda: _write_immutable(
                evidence / "results" / "production",
                "case",
                "json",
                b"{}\n",
                artifact_root=evidence,
            ),
            "exactly one hard link",
        )
        external_immutable.unlink()

        matrix = candidate / "runtime-matrix.json"
        _replace_generated(matrix, b'{"generation":1}\n')
        external_matrix = outside / "matrix-hardlink"
        os.link(matrix, external_matrix)
        _expect_failure(
            lambda: _replace_generated(
                matrix,
                b'{"generation":2}\n',
            ),
            "exactly one hard link",
        )
        if external_matrix.read_bytes() != b'{"generation":1}\n':
            raise RuntimeError(
                "self-test changed an external hard link during publication"
            )
        external_matrix.unlink()

        fifo_candidate = root / "fifo-candidate"
        fifo_candidate.mkdir()
        fifo_matrix = fifo_candidate / "runtime-matrix.json"
        os.mkfifo(fifo_matrix, mode=0o600)
        _expect_failure(
            lambda: _replace_generated(fifo_matrix, b"replacement"),
            "not a regular file",
        )

        evidence_link_candidate = root / "evidence-link-candidate"
        evidence_link_candidate.mkdir()
        (evidence_link_candidate / "evidence").symlink_to(
            outside,
            target_is_directory=True,
        )
        _expect_failure(
            lambda: _secure_descendant_directory(
                evidence_link_candidate,
                evidence_link_candidate / "evidence",
                "self-test-evidence-symlink",
            ),
            "not a real directory",
        )

        category_link = evidence / "linked_category"
        category_link.symlink_to(outside, target_is_directory=True)
        _expect_failure(
            lambda: _write_immutable(
                category_link / "production",
                "case",
                "json",
                b'{"outside":true}\n',
                artifact_root=evidence,
            ),
            "not a real directory",
        )
        category_link.unlink()

        profile_parent = evidence / "linked_profile"
        profile_parent.mkdir()
        (profile_parent / "production").symlink_to(
            outside,
            target_is_directory=True,
        )
        _expect_failure(
            lambda: _write_immutable(
                profile_parent / "production",
                "case",
                "json",
                b'{"outside":true}\n',
                artifact_root=evidence,
            ),
            "not a real directory",
        )

        outside_matrix = outside / "runtime-matrix.json"
        outside_matrix.write_bytes(b"outside-matrix")
        candidate_link = root / "candidate-link"
        candidate_link.symlink_to(outside, target_is_directory=True)
        _expect_failure(
            lambda: _replace_generated(
                candidate_link / "runtime-matrix.json",
                b"replacement",
            ),
            "cannot authenticate directory",
        )
        if outside_matrix.read_bytes() != b"outside-matrix":
            raise RuntimeError(
                "self-test followed a candidate-parent symlink"
            )

        outside_stamp = outside / "runtime.stamp"
        outside_stamp.write_bytes(b"outside-stamp")
        stamp_link = root / "stamp-link"
        stamp_link.symlink_to(outside, target_is_directory=True)
        _expect_failure(
            lambda: _replace_generated(
                stamp_link / "runtime.stamp",
                b"replacement",
            ),
            "cannot authenticate directory",
        )
        if outside_stamp.read_bytes() != b"outside-stamp":
            raise RuntimeError("self-test followed a stamp-parent symlink")
        if sentinel.read_bytes() != b"outside-sentinel":
            raise RuntimeError("self-test changed the outside sentinel")


def _run_stage_and_cluster_authority_self_tests() -> None:
    with tempfile.TemporaryDirectory(
        prefix="bioseq-runtime-authority-",
    ) as temporary:
        candidate_root = Path(temporary).resolve(strict=True)
        stage_root = candidate_root / "stages" / "production"
        bindir = stage_root / "bin"
        sharedir = stage_root / "share"
        pkglibdir = stage_root / "lib"
        (sharedir / "extension").mkdir(parents=True)
        bindir.mkdir()
        pkglibdir.mkdir()

        signatures: list[dict[str, Any]] = []
        expected_paths: dict[str, str] = {}
        for tool in POSTGRESQL_TOOLS:
            relative = f"bin/{tool}"
            tool_path = stage_root / relative
            tool_path.write_bytes(f"{tool}-17.10".encode())
            tool_path.chmod(0o755)
            signatures.append(
                {
                    "tool": tool,
                    "staged_path": relative,
                    "version": (
                        f"{tool} (PostgreSQL) 17.10 self-test"
                    ),
                    "sha256": _sha256_file(
                        tool_path,
                        f"self-test-stage-{tool}",
                    ),
                }
            )
            expected_paths[tool] = relative

        control_path = sharedir / "extension" / "bioseq.control"
        install_path = sharedir / "extension" / "bioseq--1.0.sql"
        library_path = pkglibdir / "bioseq.so"
        control_path.write_bytes(b"self-test-control")
        install_path.write_bytes(b"self-test-install-sql")
        library_path.write_bytes(b"self-test-profile-dso")
        control_sha256 = _sha256_file(control_path, "self-test-control")
        install_sha256 = _sha256_file(install_path, "self-test-install")
        library_sha256 = _sha256_file(library_path, "self-test-library")
        runtime_entries: list[dict[str, Any]] = []
        runtime_paths = [
            *(
                ("bindir", tool, bindir / tool)
                for tool in sorted(POSTGRESQL_TOOLS)
            ),
            ("sharedir", "extension", sharedir / "extension"),
            (
                "sharedir",
                "extension/bioseq--1.0.sql",
                install_path,
            ),
            (
                "sharedir",
                "extension/bioseq.control",
                control_path,
            ),
            ("pkglibdir", "bioseq.so", library_path),
        ]
        for root_name, relative, source_path in runtime_paths:
            is_directory = source_path.is_dir()
            raw = b"" if is_directory else source_path.read_bytes()
            runtime_entries.append(
                {
                    "root": root_name,
                    "path": relative,
                    "kind": "dir" if is_directory else "file",
                    "source_kind": "dir" if is_directory else "file",
                    "source_path": str(source_path),
                    "source_link_target": None,
                    "resolved_source_path": str(source_path),
                    "size": None if is_directory else len(raw),
                    "sha256": (
                        None if is_directory else _sha256_bytes(raw)
                    ),
                }
            )
        runtime_entries.sort(
            key=lambda entry: (
                {"bindir": 0, "sharedir": 1, "pkglibdir": 2}[
                    entry["root"]
                ],
                entry["path"],
            )
        )
        runtime_aggregate = _sha256_bytes(
            "".join(
                _runtime_entry_record(entry) for entry in runtime_entries
            ).encode("utf-8")
        )
        package_aggregate = _stage_package_aggregate(
            name="bioseq",
            module_pathname_override=None,
            control_filename="bioseq.control",
            control_sha256=control_sha256,
            install_sql_filename="bioseq--1.0.sql",
            install_sql_sha256=install_sha256,
            library_filename="bioseq.so",
            library_sha256=library_sha256,
        )
        manifest: dict[str, Any] = {
            "schema": STAGE_MANIFEST_SCHEMA,
            "schema_revision": STAGE_MANIFEST_SCHEMA_REVISION,
            "postgresql_version": "PostgreSQL 17.10 self-test",
            "postgresql_configure": "--self-test",
            "source": {
                "bindir": str(bindir),
                "sharedir": str(sharedir),
                "pkglibdir": str(pkglibdir),
            },
            "staged": {
                "bindir": "bin",
                "sharedir": "share",
                "pkglibdir": "lib",
            },
            "runtime_closure": {
                "algorithm": RUNTIME_CLOSURE_ALGORITHM,
                "schema_revision": RUNTIME_CLOSURE_SCHEMA_REVISION,
                "entry_count": len(runtime_entries),
                "aggregate_sha256": runtime_aggregate,
                "roots": {
                    "bindir": {
                        "reported_source": str(bindir),
                        "canonical_source": str(bindir),
                        "staged": "bin",
                    },
                    "sharedir": {
                        "reported_source": str(sharedir),
                        "canonical_source": str(sharedir),
                        "staged": "share",
                    },
                    "pkglibdir": {
                        "reported_source": str(pkglibdir),
                        "canonical_source": str(pkglibdir),
                        "staged": "lib",
                    },
                },
                "required_sentinels": {
                    "postgres": {
                        "root": "bindir",
                        "path": "postgres",
                        "kind": "file",
                        "sha256": _sha256_file(
                            bindir / "postgres",
                            "self-test-postgres-sentinel",
                        ),
                    },
                    "extension": {
                        "root": "sharedir",
                        "path": "extension",
                        "kind": "dir",
                        "sha256": None,
                    },
                },
                "loader_dependencies": {
                    "entry_count": 0,
                    "aggregate_sha256": _sha256_bytes(b""),
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
                            "staged": "share/extension/bioseq.control",
                            "source_sha256": control_sha256,
                            "sha256": control_sha256,
                        },
                        "install_sql": {
                            "source": str(install_path),
                            "filename": "bioseq--1.0.sql",
                            "staged": (
                                "share/extension/bioseq--1.0.sql"
                            ),
                            "version": "1.0",
                            "sha256": install_sha256,
                        },
                        "library": {
                            "source": str(library_path),
                            "filename": "bioseq.so",
                            "staged": "lib/bioseq.so",
                            "sha256": library_sha256,
                        },
                    }
                ],
            },
            "expected_tool_paths": expected_paths,
            "tool_signatures": signatures,
        }
        manifest_path = stage_root / "stage-manifest.json"
        manifest_path.write_bytes(_canonical_json(manifest))
        stage = _parse_stage_manifest(
            manifest_path,
            profile="production",
            dso_sha256=library_sha256,
            sql_script_sha256=install_sha256,
        )
        stage.pg_ctl_path.chmod(0o644)
        try:
            _expect_failure(
                lambda: _parse_stage_manifest(
                    manifest_path,
                    profile="production",
                    dso_sha256=library_sha256,
                    sql_script_sha256=install_sha256,
                ),
                "executable mode differ",
            )
        finally:
            stage.pg_ctl_path.chmod(0o755)
        (sharedir / "extension" / "substitute.control").write_bytes(
            control_path.read_bytes()
        )
        (pkglibdir / "substitute.so").write_bytes(
            library_path.read_bytes()
        )

        def reject_manifest(
            mutation: Callable[[dict[str, Any]], None],
            diagnostic: str,
            label: str,
        ) -> None:
            candidate = json.loads(json.dumps(manifest))
            mutation(candidate)
            path = stage_root / f"stage-mutation-{label}.json"
            path.write_bytes(_canonical_json(candidate))
            _expect_failure(
                lambda: _parse_stage_manifest(
                    path,
                    profile="production",
                    dso_sha256=library_sha256,
                    sql_script_sha256=install_sha256,
                ),
                diagnostic,
            )

        reject_manifest(
            lambda value: value["extension_artifacts"]["artifacts"][0][
                "control"
            ].pop("staged"),
            "missing fields: staged",
            "missing-control-staged",
        )
        reject_manifest(
            lambda value: value["extension_artifacts"]["artifacts"][0][
                "control"
            ].update({"staged": "share/extension/substitute.control"}),
            "control path differs",
            "substituted-control",
        )
        reject_manifest(
            lambda value: value["extension_artifacts"]["artifacts"][0][
                "install_sql"
            ].update({"version": "2.0"}),
            "extension install version must be 1.0",
            "install-version",
        )
        reject_manifest(
            lambda value: value["extension_artifacts"]["artifacts"][0][
                "library"
            ].update({"staged": "lib/substitute.so"}),
            "library path differs",
            "substituted-library",
        )
        reject_manifest(
            lambda value: value["extension_artifacts"].update(
                {"aggregate_sha256": "0" * 64}
            ),
            "aggregate hash is stale",
            "stale-aggregate",
        )
        reject_manifest(
            lambda value: value["runtime_closure"].update(
                {"aggregate_sha256": "0" * 64}
            ),
            "runtime aggregate hash is stale",
            "stale-runtime-aggregate",
        )
        reject_manifest(
            lambda value: value["runtime_closure"].update(
                {"algorithm": "postgresql.copy_tree"}
            ),
            "algorithm or schema revision differs",
            "runtime-algorithm",
        )
        reject_manifest(
            lambda value: value.update({"schema": "stage_manifest/v1"}),
            "unsupported schema",
            "legacy-stage-schema",
        )
        reject_manifest(
            lambda value: value.update({"legacy_layout": {}}),
            "unknown fields: legacy_layout",
            "legacy-root-alias",
        )
        reject_manifest(
            lambda value: value["source"].update({"sharedir": "relative"}),
            "absolute and normalized",
            "relative-source",
        )

        for selected_path, label in (
            (install_path, "install-sql"),
            (library_path, "library"),
        ):
            backup_path = selected_path.with_name(
                selected_path.name + ".regular"
            )
            selected_path.rename(backup_path)
            os.mkfifo(selected_path, mode=0o600)
            try:
                _expect_failure(
                    lambda: _parse_stage_manifest(
                        manifest_path,
                        profile="production",
                        dso_sha256=library_sha256,
                        sql_script_sha256=install_sha256,
                    ),
                    "not a regular file",
                )
            finally:
                selected_path.unlink()
                backup_path.rename(selected_path)

        data_directory = candidate_root / "runs" / "production" / "data"
        socket_directory = candidate_root / "runs" / "production" / "socket"
        data_directory.mkdir(parents=True)
        socket_directory.mkdir()
        authorization = data_directory / AUTHORIZATION_MARKER_NAME
        authorization.write_bytes(AUTHORIZATION_MARKER)
        authorization.chmod(0o600)
        terminal_marker_directory = (
            data_directory / "bioseq-safety-terminal-markers"
        )
        terminal_marker_directory.mkdir(mode=0o700)
        server_log_path = (
            candidate_root / "runs" / "production" / "server_startup.log"
        )
        server_log_path.write_bytes(b"")
        runtime_control_dso = candidate_root / "current" / "runtime-control.so"
        runtime_control_dso.parent.mkdir()
        runtime_control_dso.write_bytes(b"self-test-runtime-control")
        config: dict[str, Any] = {
            "schema_revision": 1,
            "profile": "production",
            "dedicated_safety_cluster": True,
            "connection": {
                "host": str(socket_directory),
                "port": "45432",
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
            "expected_system_identifier": "7000000000000000001",
            "authorization_file": str(authorization),
            "installed_dso_path": str(library_path),
            "installed_control_dso_path": str(
                pkglibdir / runtime_control_dso.name
            ),
            "installed_control_path": str(control_path),
            "installed_sql_path": str(install_path),
            "restart_command": [
                str(stage.pg_ctl_path),
                "--pgdata",
                str(data_directory),
                "--wait",
                "--mode=fast",
                "restart",
            ],
            "stage_manifest_path": str(manifest_path),
            "stage_manifest_sha256": stage.manifest_sha256,
            "tool_signature_suite_sha256": (
                stage.tool_signature_suite_sha256
            ),
        }
        snapshot = _validate_cluster_config_authority(
            config,
            profile="production",
            candidate_root=candidate_root,
            stage=stage,
            stage_manifest_path=manifest_path,
            runtime_control_dso_path=runtime_control_dso,
            context="self-test-cluster-config",
        )
        evidence_root = _secure_descendant_directory(
            candidate_root,
            candidate_root / "evidence",
            "self-test-cluster-authority-evidence",
        )
        published = _publish_cluster_authority(
            snapshot,
            artifact_directory=evidence_root,
            matrix_path=candidate_root / "runtime-matrix.json",
        )
        published_record, _ = _load_json(
            published.record_path,
            "self-test-published-cluster-authority",
        )
        _require_exact_fields(
            published_record,
            CLUSTER_AUTHORITY_FIELDS,
            "self-test-published-cluster-authority",
        )
        if (
            published_record["authorization_artifact_sha256"]
            != _sha256_bytes(AUTHORIZATION_MARKER)
            or published_record["data_directory"]
            != str(data_directory)
            or published_record["socket_directory"]
            != str(socket_directory)
            or published_record["connection_port"] != "45432"
            or published_record["connection_user"] != "runtime_owner"
        ):
            raise RuntimeError(
                "self-test published cluster authority differs"
            )

        def reject_config(
            mutation: Callable[[dict[str, Any]], None],
            diagnostic: str,
        ) -> None:
            candidate = json.loads(json.dumps(config))
            mutation(candidate)
            _expect_failure(
                lambda: _validate_cluster_config_authority(
                    candidate,
                    profile="production",
                    candidate_root=candidate_root,
                    stage=stage,
                    stage_manifest_path=manifest_path,
                    runtime_control_dso_path=runtime_control_dso,
                    context="self-test-cluster-config",
                ),
                diagnostic,
            )

        connection_mutations: dict[str, Any] = {
            "host": str(candidate_root / "absent-socket"),
            "port": "045432",
            "database": "postgres",
            "user": "",
            "password": "substituted",
        }
        for field_name, substituted in connection_mutations.items():
            reject_config(
                lambda value, field_name=field_name, substituted=substituted: (
                    value["connection"].update(
                        {field_name: substituted}
                    )
                ),
                f"connection.{field_name}",
            )
        reject_config(
            lambda value: value["connection"].update({"unknown": "field"}),
            "unknown fields: unknown",
        )
        reject_config(
            lambda value: value["connection"].pop("port"),
            "missing fields: port",
        )
        for index in range(len(config["restart_command"])):
            reject_config(
                lambda value, index=index: value["restart_command"].__setitem__(
                    index,
                    "substituted",
                ),
                "restart command differs",
            )
        reject_config(
            lambda value: value["restart_command"].append("extra"),
            "restart command differs",
        )
        alternate_dso = pkglibdir / "alternate.so"
        alternate_dso.write_bytes(library_path.read_bytes())
        reject_config(
            lambda value: value.update(
                {"installed_dso_path": str(alternate_dso)}
            ),
            "manifest-selected path",
        )
        alternate_authorization = data_directory / "alternate.authorization"
        alternate_authorization.write_bytes(AUTHORIZATION_MARKER)
        alternate_authorization.chmod(0o600)
        reject_config(
            lambda value: value.update(
                {"authorization_file": str(alternate_authorization)}
            ),
            "path or filename differs",
        )

        authorization.write_bytes(b"substituted\n")
        authorization.chmod(0o600)
        _expect_failure(
            lambda: _validate_authorization_marker(
                authorization,
                data_directory=data_directory,
                context="self-test-authorization-bytes",
            ),
            "marker bytes differ",
        )
        authorization.write_bytes(AUTHORIZATION_MARKER)
        authorization.chmod(0o644)
        _expect_failure(
            lambda: _validate_authorization_marker(
                authorization,
                data_directory=data_directory,
                context="self-test-authorization-mode",
            ),
            "mode must be 0600",
        )
        authorization.chmod(0o600)
        _expect_failure(
            lambda: _validate_authorization_marker(
                authorization,
                data_directory=data_directory,
                context="self-test-authorization-owner",
                expected_uid=os.getuid() + 1,
            ),
            "current uid",
        )


def _self_test() -> int:
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
            "self-test-ledger-revision",
        ),
        (
            RUNTIME_POLICY_FIELDS,
            "schema_revision",
            "schema_version",
            2,
            "self-test-runtime-policy-revision",
        ),
        (
            RUNTIME_INVENTORY_FIELDS,
            "schema_revision",
            "schema_version",
            1,
            "self-test-runtime-inventory-revision",
        ),
        (
            RUNTIME_INVENTORY_FIELDS,
            "probe_abi_revision",
            "probe_abi_version",
            2,
            "self-test-runtime-inventory-abi-revision",
        ),
        (
            RUNTIME_INVENTORY_ENTRY_FIELDS,
            "site_revision",
            None,
            2,
            "self-test-runtime-site-revision",
        ),
        (
            PROBE_CONTRACT_FIELDS,
            "abi_revision",
            "abi_version",
            2,
            "self-test-probe-contract-revision",
        ),
        (
            CATALOG_FIELDS,
            "schema_revision",
            "schema_version",
            2,
            "self-test-catalog-revision",
        ),
        (
            RUNTIME_TEMPLATE_FIELDS,
            "schema_revision",
            "schema_version",
            1,
            "self-test-runtime-template-revision",
        ),
        (
            CASE_PLAN_FIELDS,
            "schema_revision",
            "schema_version",
            2,
            "self-test-case-plan-revision",
        ),
        (
            CASE_PLAN_FIELDS,
            "probe_abi_revision",
            "probe_abi_version",
            2,
            "self-test-case-plan-abi-revision",
        ),
        (
            ACTIVATION_RECEIPT_FIELDS,
            "schema_revision",
            "schema_version",
            1,
            "self-test-activation-receipt-revision",
        ),
        (
            RAW_RESULT_FIELDS,
            "schema_revision",
            "schema_version",
            2,
            "self-test-raw-result-revision",
        ),
        (
            RAW_RESULT_FIELDS,
            "probe_abi_revision",
            "probe_abi_version",
            2,
            "self-test-raw-result-abi-revision",
        ),
        (
            FAULT_EVIDENCE_FIELDS,
            "probe_abi_revision",
            "probe_abi_version",
            2,
            "self-test-fault-evidence-abi-revision",
        ),
        (
            TERMINAL_MARKER_FIELDS,
            "schema_revision",
            "schema_version",
            1,
            "self-test-terminal-marker-revision",
        ),
        (
            CLUSTER_CONFIG_FIELDS,
            "schema_revision",
            "schema_version",
            1,
            "self-test-cluster-config-revision",
        ),
        (
            CLUSTER_AUTHORITY_FIELDS,
            "schema_revision",
            "schema_version",
            1,
            "self-test-cluster-authority-revision",
        ),
    ):
        _expect_closed_revision_rejections(
            fields,
            field_name=field_name,
            legacy_field_name=legacy_field_name,
            expected_revision=expected_revision,
            context=context,
        )

    if POSTGRESQL_VERSION_NUM != 170010:
        raise RuntimeError(
            "self-test PostgreSQL runtime version policy is stale"
        )
    for stale_server_version in (170005, 170007):
        _expect_failure(
            lambda stale=stale_server_version: (
                _postgresql_version_num(
                    stale,
                    "self-test-server-version",
                )
            ),
            "must be exact PostgreSQL 17.10",
        )

    source_span = {
        "begin": {"byte_offset": 100, "line": 10, "column": 3},
        "end": {"byte_offset": 120, "line": 10, "column": 23},
    }
    owner_usr = "c:@F@bioseq_fixture"
    call_path = (
        {
            "usr": owner_usr,
            "qualified_declaration": "bioseq_fixture(int)",
            "source_file": "/fixture.cppm",
            "source_span": source_span,
        },
    )

    def fixture_site(
        *,
        site_id: str,
        ledger_site_sha256: str,
    ) -> LedgerSite:
        return LedgerSite(
            sql_identity="bioseq.fixture(integer)",
            site_id=site_id,
            source_file="/fixture.cppm",
            callee_or_macro="ereport",
            classification="bridged",
            bridge="invoke_pg_as_cpp",
            converter_families=("scalar",),
            owner_families=("bioseq.fixture",),
            cleanup_phases=("spi_finish",),
            owner_usr=owner_usr,
            expanded_source_span=source_span,
            transitive_call_path=call_path,
            ledger_site_sha256=ledger_site_sha256,
        )

    production_site = fixture_site(
        site_id="clang:fixture:production:error-site",
        ledger_site_sha256="a" * 64,
    )
    test_site = fixture_site(
        site_id="clang:fixture:test-indexes:error-site",
        ledger_site_sha256="b" * 64,
    )

    def fixture_ledger(
        *,
        profile: str,
        site: LedgerSite,
        ledger_sha256: str,
        sql_manifest_sha256: str,
    ) -> Ledger:
        return Ledger(
            profile=profile,
            path=Path(f"/{profile}-ledger.json"),
            sha256=ledger_sha256,
            sql_manifest_sha256=sql_manifest_sha256,
            compiler_facts_sha256="c" * 64,
            source_list_sha256="d" * 64,
            compile_database_sha256="e" * 64,
            producer_executable_sha256="5" * 64,
            compile_graph_sha256="f" * 64,
            pcm_manifest_sha256="0" * 64,
            sites_by_parent={site.sql_identity: (site,)},
        )

    production = fixture_ledger(
        profile="production",
        site=production_site,
        ledger_sha256="1" * 64,
        sql_manifest_sha256="2" * 64,
    )
    test_indexes = fixture_ledger(
        profile="test_indexes",
        site=test_site,
        ledger_sha256="3" * 64,
        sql_manifest_sha256="4" * 64,
    )
    all_ledger_coverage = production.coverage | test_indexes.coverage
    runtime_policy_fixture = {
        "schema_revision": 2,
        "required_coverage": [
            _coverage_json(
                ("production", "classification", "bridged")
            ),
            _coverage_json(
                ("test_indexes", "classification", "bridged")
            ),
        ],
    }
    with tempfile.TemporaryDirectory(
        prefix="bioseq-postgres-error-safety-authorization-marker-self-test-"
    ) as temporary:
        root = Path(temporary).resolve(strict=True)
        policy_path = root / "runtime-policy.json"
        policy_path.write_bytes(
            _canonical_json(runtime_policy_fixture)
        )
        parsed_policy, _ = _parse_runtime_policy(policy_path)
        if parsed_policy != frozenset(
            {
                ("production", "classification", "bridged"),
                ("test_indexes", "classification", "bridged"),
            }
        ):
            raise RuntimeError(
                "self-test rejected the exact schema revision 2 runtime policy"
            )
        if _required_coverage(
            {
                "production": production,
                "test_indexes": test_indexes,
            },
            all_ledger_coverage,
        ) != all_ledger_coverage:
            raise RuntimeError(
                "self-test failed inventory-independent coverage derivation"
            )

        expected = {
            "backend_outcome": "recoverable_error",
            "destructor_sentinels": ["fields", "caller"],
            "cleanup_events": [
                "resource_owner.restore",
                "memory_context.restore",
            ],
            "postgres_cleanup_events": ["fmgr_abort_observed"],
            "sqlstate": "XX000",
            "message": "injected PostgreSQL ERROR",
            "recovery_postcondition": "SPI session is usable",
        }
        execution = {
            "target_regprocedure": production_site.sql_identity,
            "setup_sql": [],
            "invocation_sql": "SELECT bioseq.fixture(1)",
            "post_error_sql": [],
            "recovery_sql": "SELECT 'ok'",
            "recovery_expected_rows": [["ok"]],
            "cleanup_sql": [],
        }
        template_case = {
            "case_id": "bridge-error",
            "profiles": ["production", "test_indexes"],
            "sql_identity": production_site.sql_identity,
            "invocation_key": "fixture/invoke",
            "fault_key": GENERATED_BOUNDARY_FAULT_KEY,
            "postgres_severity": "error",
            "expected": expected,
            "execution": execution,
        }
        _runtime_template_case(template_case, "self-test-template")
        exact_template = {
            "schema_revision": 1,
            "cases": [
                {
                    **json.loads(json.dumps(template_case)),
                    "case_id": f"bridge-error-{index:02d}",
                    "invocation_key": f"fixture/invoke/{index:02d}",
                }
                for index in range(EXPECTED_TEMPLATE_COUNT)
            ],
        }
        _validate_runtime_template_inventory(
            exact_template,
            "self-test-exact-template",
        )
        missing_template = json.loads(json.dumps(exact_template))
        missing_template["cases"].pop()
        _expect_failure(
            lambda: _validate_runtime_template_inventory(
                missing_template,
                "self-test-missing-template",
            ),
            "requires exactly 25 templates",
        )
        extra_template = json.loads(json.dumps(exact_template))
        extra_case = json.loads(
            json.dumps(extra_template["cases"][-1])
        )
        extra_case["case_id"] = "bridge-error-25"
        extra_case["invocation_key"] = "fixture/invoke/25"
        extra_template["cases"].append(extra_case)
        _expect_failure(
            lambda: _validate_runtime_template_inventory(
                extra_template,
                "self-test-extra-template",
            ),
            "requires exactly 25 templates",
        )
        profile_subset_template = json.loads(
            json.dumps(exact_template)
        )
        profile_subset_template["cases"][0]["profiles"] = [
            "production"
        ]
        _expect_failure(
            lambda: _validate_runtime_template_inventory(
                profile_subset_template,
                "self-test-profile-subset-template",
            ),
            "must name both profiles in canonical order",
        )
        _plan_expected(
            expected,
            site=production_site,
            fault_key=GENERATED_BOUNDARY_FAULT_KEY,
            context="self-test-plan.expected",
        )
        _plan_execution(
            execution,
            sql_identity=production_site.sql_identity,
            fault_key=GENERATED_BOUNDARY_FAULT_KEY,
            context="self-test-plan.execution",
        )
        backend_expected = dict(expected)
        backend_expected["postgres_cleanup_events"] = []
        _plan_expected(
            backend_expected,
            site=production_site,
            fault_key=BACKEND_SCOPED_VACUUM_FAULT_KEY,
            context="self-test-backend-plan.expected",
        )
        invalid_backend_expected = dict(backend_expected)
        invalid_backend_expected["postgres_cleanup_events"] = [
            "fmgr_abort_observed"
        ]
        _expect_failure(
            lambda: _plan_expected(
                invalid_backend_expected,
                site=production_site,
                fault_key=BACKEND_SCOPED_VACUUM_FAULT_KEY,
                context="self-test-invalid-backend-plan.expected",
            ),
            "must expect no FMGR cleanup",
        )
        backend_execution = dict(execution)
        backend_execution.update(
            {
                "pre_fault_control_sql": ["VACUUM fixture"],
                "pre_fault_control_recovery_sql": "SHOW fixture",
                "pre_fault_control_recovery_expected_rows": [["on"]],
            }
        )
        normalized_backend_execution = _plan_execution(
            backend_execution,
            sql_identity=production_site.sql_identity,
            fault_key=BACKEND_SCOPED_VACUUM_FAULT_KEY,
            context="self-test-backend-plan.execution",
        )
        if normalized_backend_execution[
            "pre_fault_control_recovery_expected_rows"
        ] != [["on"]]:
            raise RuntimeError(
                "self-test rejected backend-scoped success-control rows"
            )
        incomplete_backend_execution = dict(backend_execution)
        incomplete_backend_execution.pop(
            "pre_fault_control_recovery_expected_rows"
        )
        _expect_failure(
            lambda: _plan_execution(
                incomplete_backend_execution,
                sql_identity=production_site.sql_identity,
                fault_key=BACKEND_SCOPED_VACUUM_FAULT_KEY,
                context="self-test-incomplete-backend-plan.execution",
            ),
            "must be present together",
        )
        for forbidden_field in sorted(
            FORBIDDEN_TEMPLATE_AUTHORITY_FIELDS
        ):
            candidate = dict(template_case)
            candidate[forbidden_field] = "authored"
            _expect_failure(
                lambda candidate=candidate: _runtime_template_case(
                    candidate,
                    "self-test-template",
                ),
                "authored runtime authority is forbidden",
            )

        producer_sha256 = "5" * 64
        fault_key = GENERATED_BOUNDARY_FAULT_KEY
        relation = "injected_routes_to_site"
        probe_contract = _expected_probe_contract(relation)
        sealed_site_id = _probe_site_id(
            production_site.sql_identity,
            fault_key,
        )

        def anchor(
            kind: str,
            compiler_stable_id: str,
        ) -> dict[str, Any]:
            return {
                "kind": kind,
                "compiler_stable_id": compiler_stable_id,
                "enclosing_function_usr": owner_usr,
                "source_file": production_site.source_file,
                "expanded_source_span": source_span,
                "source_fragment_sha256": "6" * 64,
            }

        inventory_entry = {
            "sql_identity": production_site.sql_identity,
            "fault_key": fault_key,
            "site_id": sealed_site_id,
            "site_revision": 2,
            "relation": relation,
            "ledger_site_sha256": (
                production_site.ledger_site_sha256
            ),
            "site_anchor": anchor(
                "postgres_error_site",
                production_site.site_id,
            ),
            "visit_anchor": anchor(
                "record_site_visit",
                "clang:fixture:production:visit",
            ),
            "trigger_anchor": anchor(
                "consume_fault",
                "clang:fixture:production:trigger",
            ),
            "transitive_call_path": list(call_path),
            "probe_contract": probe_contract,
        }
        inventory_document = {
            "schema_revision": 1,
            "profile": "production",
            "probe_abi_revision": 2,
            "ledger_sha256": production.sha256,
            "compiler_facts_sha256": (
                production.compiler_facts_sha256
            ),
            "sql_manifest_sha256": (
                production.sql_manifest_sha256
            ),
            "source_list_sha256": production.source_list_sha256,
            "compile_database_sha256": (
                production.compile_database_sha256
            ),
            "compile_graph_sha256": (
                production.compile_graph_sha256
            ),
            "pcm_manifest_sha256": production.pcm_manifest_sha256,
            "inventory_producer_executable_sha256": (
                producer_sha256
            ),
            "entries": [inventory_entry],
        }
        inventory_path = root / "production-runtime-inventory.json"

        def parse_inventory(
            document: Mapping[str, Any],
        ) -> RuntimeInventory:
            inventory_path.write_bytes(_canonical_json(document))
            return _parse_runtime_inventory(
                inventory_path,
                profile="production",
                ledger=production,
                producer_executable_sha256=producer_sha256,
            )

        inventory = parse_inventory(inventory_document)
        inventory_key = (
            production_site.sql_identity,
            fault_key,
        )
        if (
            set(inventory.entries) != {inventory_key}
            or inventory.entries[inventory_key].site_id
                != sealed_site_id
        ):
            raise RuntimeError(
                "self-test rejected valid ABI revision 2 runtime inventory"
            )
        generic_fault_key = "invoke_pg_as_cpp.cleanup_restore"
        generic_inventory_document = json.loads(
            json.dumps(inventory_document)
        )
        generic_inventory_document["entries"][0]["fault_key"] = (
            generic_fault_key
        )
        generic_site_id = _probe_site_id(
            production_site.sql_identity,
            generic_fault_key,
        )
        generic_inventory_document["entries"][0]["site_id"] = (
            generic_site_id
        )
        generic_inventory = parse_inventory(
            generic_inventory_document
        )
        generic_inventory_key = (
            production_site.sql_identity,
            generic_fault_key,
        )
        if (
            set(generic_inventory.entries) != {
                generic_inventory_key
            }
            or generic_inventory.entries[
                generic_inventory_key
            ].site_id
                != generic_site_id
        ):
            raise RuntimeError(
                "self-test rejected a valid generic runtime fault key"
            )
        for invalid_fault_key in (
            "invalid-key",
            "Invalid",
            "a" * 129,
        ):
            invalid_key_inventory = json.loads(
                json.dumps(inventory_document)
            )
            invalid_key_inventory["entries"][0]["fault_key"] = (
                invalid_fault_key
            )
            _expect_failure(
                lambda candidate=invalid_key_inventory: (
                    parse_inventory(candidate)
                ),
                "containing only [a-z0-9_.]",
            )
        stale_site_inventory = json.loads(
            json.dumps(inventory_document)
        )
        stale_site_inventory["entries"][0]["site_id"] = (
            "probe:" + "0" * 64
        )
        _expect_failure(
            lambda: parse_inventory(stale_site_inventory),
            "site_id is not the exact sealed ABI revision 2 ID",
        )
        legacy_site_inventory = json.loads(
            json.dumps(inventory_document)
        )
        legacy_site_inventory["entries"][0]["site_id"] = (
            "probe:v2:" + "0" * 64
        )
        _expect_failure(
            lambda: parse_inventory(legacy_site_inventory),
            "site_id is not the exact sealed ABI revision 2 ID",
        )
        stale_contract_inventory = json.loads(
            json.dumps(inventory_document)
        )
        stale_contract_inventory["entries"][0]["probe_contract"][
            "ordering"
        ] = "visit_before_trigger"
        _expect_failure(
            lambda: parse_inventory(stale_contract_inventory),
            "relation-derived ABI revision 2 contract",
        )
        authored_inventory = json.loads(
            json.dumps(inventory_document)
        )
        authored_inventory["entries"][0]["coverage"] = []
        _expect_failure(
            lambda: parse_inventory(authored_inventory),
            "unknown fields: coverage",
        )
        inventory = parse_inventory(inventory_document)

        dso_path = root / "active-production.so"
        dso_path.write_bytes(b"production-dso")
        test_executable = root / "production-runtime-tests"
        test_executable.write_bytes(b"production-runtime-tests")
        cluster_config = root / "production-cluster.json"
        cluster_config.write_bytes(b"production-cluster-config")
        terminal_marker_directory = root / "terminal-markers"
        terminal_marker_directory.mkdir(mode=0o700)
        server_log_path = root / "server_startup.log"
        server_log_path.write_bytes(b"")
        control_dso = root / "runtime-control.so"
        control_dso.write_bytes(b"runtime-control-dso")
        stage_manifest = root / "production-stage-manifest.json"
        stage_manifest.write_bytes(b"production-stage-manifest")
        case_plan = root / "production-case-plan.json"
        case_plan.write_bytes(b'{"schema_revision":2}\n')
        test_executable_sha256 = _sha256_file(
            test_executable,
            "self-test-executable",
        )
        case_plan_sha256 = _sha256_file(
            case_plan,
            "self-test-case-plan",
        )
        catalog_entry = {
            "case_id": "production.bridge-error",
            "sql_identity": production_site.sql_identity,
            "site_id": sealed_site_id,
            "ledger_site_sha256": (
                production_site.ledger_site_sha256
            ),
            "invocation_key": "fixture/invoke",
            "fault_key": fault_key,
            "relation": relation,
            "probe_contract": probe_contract,
            "postgres_severity": "error",
            "expected": expected,
        }
        catalog = {
            "schema_revision": 2,
            "profile": "production",
            "case_plan_sha256": case_plan_sha256,
            "test_executable_sha256": test_executable_sha256,
            "cases": [catalog_entry],
        }
        catalog_path = root / "catalog.json"

        def parse_catalog(
            document: Mapping[str, Any],
        ) -> tuple[CatalogCase, ...]:
            catalog_path.write_bytes(_canonical_json(document))
            cases, _ = _parse_catalog(
                catalog_path,
                profile="production",
                ledger=production,
                inventory=inventory,
                test_executable_sha256=test_executable_sha256,
                case_plan_sha256=case_plan_sha256,
                case_plan_entries={
                    catalog_entry["case_id"]: {
                        "catalog": catalog_entry,
                        "execution": {},
                    }
                },
                required=production.coverage,
                expected_case_count=1,
            )
            return cases

        parsed_cases = parse_catalog(catalog)
        if len(parsed_cases) != 1:
            raise RuntimeError(
                "self-test rejected inventory-bound case catalog"
            )
        stale_catalog = json.loads(json.dumps(catalog))
        stale_catalog["cases"][0]["relation"] = (
            "injected_after_site_success"
        )
        _expect_failure(
            lambda: parse_catalog(stale_catalog),
            "published catalog entry differs from the current case plan",
        )
        invalid_key_catalog = json.loads(json.dumps(catalog))
        invalid_key_catalog["cases"][0]["fault_key"] = "invalid-key"
        _expect_failure(
            lambda: parse_catalog(invalid_key_catalog),
            "published catalog entry differs from the current case plan",
        )
        authored_catalog = json.loads(json.dumps(catalog))
        authored_catalog["cases"][0]["coverage"] = []
        _expect_failure(
            lambda: parse_catalog(authored_catalog),
            "unknown fields: coverage",
        )
        case = parsed_cases[0]
        inputs = ProfileInputs(
            profile="production",
            ledger=production,
            runtime_inventory=inventory,
            runtime_inventory_path=inventory.path,
            runtime_inventory_sha256=inventory.sha256,
            sql_manifest_path=root / "production-sql-manifest.json",
            sql_manifest_sha256=production.sql_manifest_sha256,
            sql_script_sha256="7" * 64,
            case_plan_path=case_plan.resolve(),
            case_plan_sha256=case_plan_sha256,
            case_plan_runtime_policy_sha256="8" * 64,
            case_plan_entries={
                catalog_entry["case_id"]: {
                    "catalog": catalog_entry,
                    "execution": {},
                }
            },
            dso_path=dso_path,
            dso_sha256=_sha256_file(dso_path, "self-test-dso"),
            runtime_control_dso_path=control_dso,
            runtime_control_dso_sha256=_sha256_file(
                control_dso,
                "self-test-control-dso",
            ),
            test_executable_path=test_executable,
            test_executable_sha256=test_executable_sha256,
            cluster_config_path=cluster_config,
            cluster_config_sha256=_sha256_file(
                cluster_config,
                "self-test-cluster-config",
            ),
            stage_manifest_path=stage_manifest,
            stage_manifest_sha256=_sha256_file(
                stage_manifest,
                "self-test-stage-manifest",
            ),
            tool_signature_suite_sha256="d" * 64,
            stage=StageManifestBinding(
                manifest_path=stage_manifest.resolve(),
                manifest_sha256=_sha256_file(
                    stage_manifest,
                    "self-test-stage-manifest",
                ),
                tool_signature_suite_sha256="d" * 64,
                stage_root=root,
                bindir=root,
                sharedir=root,
                pkglibdir=root,
                pg_ctl_path=test_executable,
                control_path=cluster_config,
                control_sha256=_sha256_file(
                    cluster_config,
                    "self-test-cluster-config",
                ),
                install_sql_path=case_plan,
                install_sql_sha256=case_plan_sha256,
                library_path=dso_path,
                library_sha256=_sha256_file(
                    dso_path,
                    "self-test-dso",
                ),
            ),
            cluster_authority=ClusterAuthoritySnapshot(
                profile="production",
                cluster_system_identifier="7000000000000000001",
                data_directory=root,
                terminal_marker_directory=terminal_marker_directory,
                server_log_path=server_log_path,
                socket_directory=root / "socket",
                connection_port="45432",
                connection_user="runtime_owner",
                original_authorization_path=(
                    root / AUTHORIZATION_MARKER_NAME
                ),
                authorization_raw=AUTHORIZATION_MARKER,
                authorization_mode=0o600,
                authorization_uid=os.getuid(),
                authorization_sha256=_sha256_bytes(
                    AUTHORIZATION_MARKER
                ),
            ),
        )
        activation = ActivationReceipt(
            profile="production",
            path=root / "activation.json",
            sha256="9" * 64,
            active_dso_path=dso_path.resolve(),
            active_dso_sha256=inputs.dso_sha256,
            cluster_system_identifier="cluster-production",
            cluster_data_directory=(root / "pg-production").resolve(),
            postmaster_start_time="2026-07-24 10:00:00+00",
            activation_nonce="activation-production",
            server_version_num=170010,
            extension_oid=1001,
            guc_properties=(),
            guc_properties_sha256=_sha256_bytes(
                _canonical_json([])
            ),
            control_dso_path=control_dso.resolve(),
            control_dso_sha256=_sha256_file(
                control_dso,
                "self-test-control-dso",
            ),
        )
        runtime_producer_sha256 = "a" * 64
        request_sha256 = "b" * 64
        catalog_sha256 = "c" * 64
        raw_result: dict[str, Any] = {
            "schema_revision": 2,
            "probe_abi_revision": 2,
            "profile": case.profile,
            "case_id": case.case_id,
            "sql_identity": case.sql_identity,
            "site_id": case.site_id,
            "ledger_site_sha256": case.ledger_site_sha256,
            "invocation_key": case.invocation_key,
            "fault_key": case.fault_key,
            "relation": case.relation,
            "probe_contract": dict(case.probe_contract),
            "case_plan_sha256": inputs.case_plan_sha256,
            "ledger_sha256": inputs.ledger.sha256,
            "sql_manifest_sha256": inputs.sql_manifest_sha256,
            "active_dso_sha256": inputs.dso_sha256,
            "test_executable_sha256": (
                inputs.test_executable_sha256
            ),
            "producer_executable_sha256": (
                runtime_producer_sha256
            ),
            "activation_receipt_sha256": activation.sha256,
            "bioseq_guc_properties": [],
            "bioseq_guc_properties_sha256": (
                activation.guc_properties_sha256
            ),
            "cluster_config_sha256": inputs.cluster_config_sha256,
            "request_sha256": request_sha256,
            "catalog_sha256": catalog_sha256,
            "catalog_entry_sha256": case.catalog_entry_sha256,
            "execution_backend": "postgresql",
            "exercised_terminal_severity": False,
            "backend_outcome": {
                "expected": "recoverable_error",
                "observed": "recoverable_error",
                "initial_backend_pid": 12345,
                "recovery_backend_pid": 12345,
                "initial_connection_status": "ok",
                "post_invocation_connection_status": "ok",
                "invocation_result_status": "postgres_error",
                "terminal_guard_observed": False,
            },
            "terminal_causality": None,
            "destructor_events": [
                {"name": "caller", "count": 1},
                {"name": "fields", "count": 1},
            ],
            "cleanup_events": [
                {"name": "memory_context.restore", "count": 1},
                {"name": "resource_owner.restore", "count": 1},
            ],
            "postgres_cleanup_events": [
                {"name": "fmgr_abort_observed", "count": 1},
            ],
            "postgres_error": {
                "severity": "ERROR",
                "sqlstate": case.expected_sqlstate,
                "message": case.expected_message,
            },
            "fault_evidence": {
                "observation_mode": "backend_snapshot",
                "probe_abi_revision": 2,
                "site_id": case.site_id,
                "fault_key": case.fault_key,
                "relation": case.relation,
                "probe_contract": dict(case.probe_contract),
                "invocation_count": 1,
                "fmgr_start_count": 1,
                "fmgr_end_count": 0,
                "fmgr_abort_count": 1,
                "site_visit_count": 1,
                "fault_trigger_count": 1,
                "site_visit_sequence": 2,
                "fault_trigger_sequence": 1,
                "invalid_count": 0,
                "overflow_count": 0,
                "inside_target": False,
            },
            "recovery_probe": {
                "postcondition": (
                    case.expected_recovery_postcondition
                ),
                "passed": True,
                "transaction_status": "idle",
                "nonempty_result": True,
                "expected_rows": [["ok"]],
                "observed_rows": [["ok"]],
                "result_row_count": 1,
            },
            "cluster_evidence": {
                "cluster_system_identifier": (
                    activation.cluster_system_identifier
                ),
                "cluster_data_directory": str(
                    activation.cluster_data_directory
                ),
                "postmaster_start_time": (
                    activation.postmaster_start_time
                ),
                "activation_nonce": activation.activation_nonce,
                "server_version_num": activation.server_version_num,
                "backend_pid": 12345,
                "initial_backend_pid": 12345,
                "recovery_backend_pid": 12345,
                "connection_replaced": False,
                "active_dso_path": str(activation.active_dso_path),
                "mapped_target_path": str(
                    activation.active_dso_path
                ),
                "control_dso_path": str(
                    activation.control_dso_path
                ),
                "control_dso_sha256": (
                    activation.control_dso_sha256
                ),
            },
        }
        validation_arguments = {
            "case": case,
            "inputs": inputs,
            "activation": activation,
            "producer_executable_sha256": (
                runtime_producer_sha256
            ),
            "request_sha256": request_sha256,
            "catalog_sha256": catalog_sha256,
        }
        observed = _validate_raw_case_result(
            raw_result,
            **validation_arguments,
        )
        attestation = observed[6]
        if (
            attestation["site_visit_count"] != 1
            or attestation["fault_trigger_count"] != 1
            or attestation["site_visit_sequence"] != 2
            or attestation["fault_trigger_sequence"] != 1
        ):
            raise RuntimeError(
                "self-test rejected valid ABI revision 2 causal evidence"
            )
        backend_case = replace(
            case,
            fault_key=BACKEND_SCOPED_VACUUM_FAULT_KEY,
            expected_postgres_cleanup_events=(),
        )
        backend_raw_result = json.loads(json.dumps(raw_result))
        backend_raw_result["fault_key"] = (
            BACKEND_SCOPED_VACUUM_FAULT_KEY
        )
        backend_raw_result["postgres_cleanup_events"] = []
        backend_raw_result["fault_evidence"]["fault_key"] = (
            BACKEND_SCOPED_VACUUM_FAULT_KEY
        )
        for field_name in (
            "invocation_count",
            "fmgr_start_count",
            "fmgr_end_count",
            "fmgr_abort_count",
        ):
            backend_raw_result["fault_evidence"][field_name] = 0
        backend_validation_arguments = dict(validation_arguments)
        backend_validation_arguments["case"] = backend_case
        backend_observed = _validate_raw_case_result(
            backend_raw_result,
            **backend_validation_arguments,
        )
        if (
            backend_observed[2]
            or backend_observed[6]["fault_invocation_count"] != 0
            or backend_observed[6]["fmgr_start_count"] != 0
            or backend_observed[6]["fmgr_abort_count"] != 0
        ):
            raise RuntimeError(
                "self-test rejected valid backend-scoped VACUUM evidence"
            )
        fabricated_backend_fmgr = json.loads(
            json.dumps(backend_raw_result)
        )
        fabricated_backend_fmgr["fault_evidence"][
            "invocation_count"
        ] = 1
        fabricated_backend_fmgr["fault_evidence"][
            "fmgr_start_count"
        ] = 1
        fabricated_backend_fmgr["fault_evidence"][
            "fmgr_abort_count"
        ] = 1
        _expect_failure(
            lambda: _validate_raw_case_result(
                fabricated_backend_fmgr,
                **backend_validation_arguments,
            ),
            "FMGR/backend lifecycle",
        )

        terminal_fault_key = (
            "scoped_spi_session.emergency_finish_error"
        )
        terminal_site_id = _probe_site_id(
            case.sql_identity,
            terminal_fault_key,
        )
        terminal_case = replace(
            case,
            site_id=terminal_site_id,
            fault_key=terminal_fault_key,
            expected_backend_outcome="backend_exit",
            expected_destructor_sentinels=(),
            expected_cleanup_events=(),
            expected_postgres_cleanup_events=(),
            expected_sqlstate=None,
            expected_message=None,
        )
        terminal_raw_result = json.loads(json.dumps(raw_result))
        terminal_raw_result["site_id"] = terminal_site_id
        terminal_raw_result["fault_key"] = terminal_fault_key
        terminal_raw_result["backend_outcome"] = {
            "expected": "backend_exit",
            "observed": "backend_exit",
            "initial_backend_pid": 12345,
            "recovery_backend_pid": 12346,
            "initial_connection_status": "ok",
            "post_invocation_connection_status": "bad",
            "invocation_result_status": "connection_lost",
            "terminal_guard_observed": True,
        }
        terminal_token = "e" * 64
        terminal_payload = (
            '{"schema":"bioseq.postgres_error_safety.terminal_marker",'
            '"schema_revision":1,'
            f'"token":"{terminal_token}",'
            f'"site_id":"{terminal_site_id}",'
            f'"fault_key":"{terminal_fault_key}",'
            '"guard_kind":"finish_error","backend_pid":12345}\n'
        )
        terminal_interval = (
            "server process (PID 12345) exited with exit code 1\n"
        )
        terminal_raw_result["terminal_causality"] = {
            "marker": {
                "path": str(
                    terminal_marker_directory
                    / f"terminal-{terminal_token}.marker"
                ),
                "sha256": _sha256_bytes(
                    terminal_payload.encode("utf-8")
                ),
                "payload": terminal_payload,
                "schema": "bioseq.postgres_error_safety.terminal_marker",
                "schema_revision": 1,
                "token": terminal_token,
                "site_id": terminal_site_id,
                "fault_key": terminal_fault_key,
                "guard_kind": "finish_error",
                "backend_pid": 12345,
                "mode": 0o600,
                "uid": os.getuid(),
                "link_count": 1,
            },
            "server_log": {
                "path": str(server_log_path),
                "device": 1,
                "inode": 1,
                "offset": 0,
                "interval_size": len(
                    terminal_interval.encode("utf-8")
                ),
                "interval_sha256": _sha256_bytes(
                    terminal_interval.encode("utf-8")
                ),
                "interval_text": terminal_interval,
                "exit_record": (
                    "server process (PID 12345) exited with exit code 1"
                ),
                "fatal_present": False,
                "panic_present": False,
                "signal_present": False,
                "core_dump_present": False,
            },
            "marker_directory": str(terminal_marker_directory),
            "server_log_path": str(server_log_path),
            "postmaster_identity_unchanged": True,
            "fatal_panic_signal_core_rejected": True,
        }
        terminal_raw_result["destructor_events"] = []
        terminal_raw_result["cleanup_events"] = []
        terminal_raw_result["postgres_cleanup_events"] = []
        terminal_raw_result["postgres_error"] = {
            "severity": None,
            "sqlstate": None,
            "message": None,
        }
        terminal_raw_result["fault_evidence"][
            "observation_mode"
        ] = "backend_exit"
        terminal_raw_result["fault_evidence"]["site_id"] = (
            terminal_site_id
        )
        terminal_raw_result["fault_evidence"]["fault_key"] = (
            terminal_fault_key
        )
        for field_name in (
            "invocation_count",
            "fmgr_start_count",
            "fmgr_end_count",
            "fmgr_abort_count",
            "site_visit_count",
            "fault_trigger_count",
            "site_visit_sequence",
            "fault_trigger_sequence",
        ):
            terminal_raw_result["fault_evidence"][field_name] = 0
        terminal_raw_result["cluster_evidence"].update(
            {
                "backend_pid": 12345,
                "initial_backend_pid": 12345,
                "recovery_backend_pid": 12346,
                "connection_replaced": True,
            }
        )
        terminal_validation_arguments = dict(validation_arguments)
        terminal_validation_arguments["case"] = terminal_case
        terminal_observed = _validate_raw_case_result(
            terminal_raw_result,
            **terminal_validation_arguments,
        )
        if (
            terminal_observed[3] is not None
            or terminal_observed[4] is not None
            or terminal_observed[6]["probe_observation_mode"]
            != "backend_exit"
            or not terminal_observed[6]["connection_replaced"]
        ):
            raise RuntimeError(
                "self-test rejected valid terminal backend-exit evidence"
            )

        def terminal_mutation(
            path: tuple[str, ...],
            value: Any,
        ) -> dict[str, Any]:
            candidate = json.loads(json.dumps(terminal_raw_result))
            target: dict[str, Any] = candidate
            for component in path[:-1]:
                target = _object(
                    target[component],
                    "self-test-terminal-mutation",
                )
            target[path[-1]] = value
            return candidate

        terminal_failures = (
            (
                ("backend_outcome", "observed"),
                "recoverable_error",
                "backend outcome",
            ),
            (
                ("backend_outcome", "recovery_backend_pid"),
                12345,
                "backend outcome",
            ),
            (
                (
                    "backend_outcome",
                    "post_invocation_connection_status",
                ),
                "ok",
                "backend outcome",
            ),
            (
                ("fault_evidence", "site_visit_count"),
                1,
                "ABI revision 2 visit/trigger",
            ),
            (
                ("exercised_terminal_severity",),
                True,
                "must never exercise FATAL or PANIC",
            ),
            (
                ("recovery_probe", "passed"),
                False,
                "recovery postcondition",
            ),
        )
        for path, value, diagnostic in terminal_failures:
            _expect_failure(
                lambda path=path, value=value: _validate_raw_case_result(
                    terminal_mutation(path, value),
                    **terminal_validation_arguments,
                ),
                diagnostic,
            )
        missing_terminal_marker = json.loads(
            json.dumps(terminal_raw_result)
        )
        del missing_terminal_marker["terminal_causality"]["marker"]
        _expect_failure(
            lambda: _validate_raw_case_result(
                missing_terminal_marker,
                **terminal_validation_arguments,
            ),
            "missing fields: marker",
        )
        wrong_terminal_marker = json.loads(
            json.dumps(terminal_raw_result)
        )
        wrong_payload = terminal_payload.replace(
            '"guard_kind":"finish_error"',
            '"guard_kind":"finish_non_ok"',
        )
        wrong_terminal_marker["terminal_causality"]["marker"].update(
            {
                "payload": wrong_payload,
                "sha256": _sha256_bytes(
                    wrong_payload.encode("utf-8")
                ),
            }
        )
        _expect_failure(
            lambda: _validate_raw_case_result(
                wrong_terminal_marker,
                **terminal_validation_arguments,
            ),
            "payload differs",
        )

        def resealed_terminal_log(extra: str) -> dict[str, Any]:
            candidate = json.loads(json.dumps(terminal_raw_result))
            interval = terminal_interval + extra
            candidate["terminal_causality"]["server_log"].update(
                {
                    "interval_text": interval,
                    "interval_size": len(interval.encode("utf-8")),
                    "interval_sha256": _sha256_bytes(
                        interval.encode("utf-8")
                    ),
                }
            )
            return candidate

        _expect_failure(
            lambda: _validate_raw_case_result(
                resealed_terminal_log("FATAL: fabricated\n"),
                **terminal_validation_arguments,
            ),
            "crash substitution",
        )
        _expect_failure(
            lambda: _validate_raw_case_result(
                resealed_terminal_log(
                    "server process was terminated by signal 11; "
                    "core dumped\n"
                ),
                **terminal_validation_arguments,
            ),
            "crash substitution",
        )
        fabricated_terminal_events = json.loads(
            json.dumps(terminal_raw_result)
        )
        fabricated_terminal_events["destructor_events"] = [
            {"name": "fabricated", "count": 1}
        ]
        _expect_failure(
            lambda: _validate_raw_case_result(
                fabricated_terminal_events,
                **terminal_validation_arguments,
            ),
            "destructor sentinel set differs",
        )

        def mutate_raw(
            path: tuple[str, ...],
            value: Any,
        ) -> dict[str, Any]:
            candidate = json.loads(json.dumps(raw_result))
            target: dict[str, Any] = candidate
            for component in path[:-1]:
                target = _object(
                    target[component],
                    "self-test-raw-mutation",
                )
            target[path[-1]] = value
            return candidate

        _expect_failure(
            lambda: _validate_raw_case_result(
                mutate_raw(
                    ("fault_evidence", "site_visit_sequence"),
                    1,
                ),
                **validation_arguments,
            ),
            "ABI revision 2 visit/trigger count or ordering differs",
        )
        _expect_failure(
            lambda: _validate_raw_case_result(
                mutate_raw(
                    ("fault_evidence", "fault_trigger_count"),
                    0,
                ),
                **validation_arguments,
            ),
            "ABI revision 2 visit/trigger count or ordering differs",
        )
        authored_raw_result = json.loads(json.dumps(raw_result))
        authored_raw_result["coverage"] = []
        _expect_failure(
            lambda: _validate_raw_case_result(
                authored_raw_result,
                **validation_arguments,
            ),
            "unknown fields: coverage",
        )
        authored_fault_evidence = json.loads(json.dumps(raw_result))
        authored_fault_evidence["fault_evidence"]["mode"] = (
            "invocation"
        )
        _expect_failure(
            lambda: _validate_raw_case_result(
                authored_fault_evidence,
                **validation_arguments,
            ),
            "unknown fields: mode",
        )

        immutable = root / "immutable"
        _write_if_different(immutable, b"first\n")
        _expect_failure(
            lambda: _write_if_different(immutable, b"tampered\n"),
            "refuses to overwrite",
        )
        generated = root / "generated"
        _replace_generated(generated, b"first\n")
        _replace_generated(generated, b"second\n")
        if generated.read_bytes() != b"second\n":
            raise RuntimeError(
                "self-test did not replace a generated artifact"
            )

        timeout_pid_file = root / "timeout-process-group.txt"
        timeout_program = (
            "import os,pathlib,signal,subprocess,sys,time;"
            "signal.signal(signal.SIGTERM,signal.SIG_IGN);"
            "child=subprocess.Popen([sys.executable,'-c',"
            "'import signal,time;"
            "signal.signal(signal.SIGTERM,signal.SIG_IGN);time.sleep(60)']);"
            f"pathlib.Path({str(timeout_pid_file)!r}).write_text("
            "f'{os.getpid()} {child.pid}',encoding='utf-8');"
            "time.sleep(60)"
        )
        _expect_failure(
            lambda: _run_child(
                (sys.executable, "-c", timeout_program),
                timeout_seconds=1,
                context="self-test-timeout-process-group",
                cwd=root,
            ),
            "process group terminated",
        )
        group_pid = int(
            timeout_pid_file.read_text(encoding="utf-8").split()[0]
        )
        deadline = time.monotonic() + 2.0
        while True:
            try:
                os.killpg(group_pid, 0)
            except ProcessLookupError:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    "self-test left a timed-out process group alive"
                )
            time.sleep(0.05)

    duplicate_json = b'{"key":1,"key":2}'
    try:
        json.loads(
            duplicate_json,
            object_pairs_hook=_reject_duplicate_json_keys,
        )
    except EvidenceError:
        pass
    else:
        raise RuntimeError("self-test accepted duplicate JSON keys")
    _run_output_security_self_tests()
    _run_stage_and_cluster_authority_self_tests()
    print("PostgreSQL runtime safety self-tests passed")
    return 0


def _path(value: str) -> Path:
    return Path(value)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Run profile-specific PostgreSQL fault cases and emit the "
            "schema revision 2 runtime safety matrix."
        )
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    run = subparsers.add_parser("run", help="run both profile matrices")
    run.add_argument("--production-ledger", required=True, type=_path)
    run.add_argument("--test-indexes-ledger", required=True, type=_path)
    run.add_argument(
        "--production-runtime-inventory",
        required=True,
        type=_path,
    )
    run.add_argument(
        "--test-indexes-runtime-inventory",
        required=True,
        type=_path,
    )
    run.add_argument(
        "--runtime-inventory-producer-executable",
        required=True,
        type=_path,
    )
    run.add_argument("--production-sql-manifest", required=True, type=_path)
    run.add_argument("--test-indexes-sql-manifest", required=True, type=_path)
    run.add_argument("--production-case-plan", required=True, type=_path)
    run.add_argument("--test-indexes-case-plan", required=True, type=_path)
    run.add_argument("--runtime-policy", required=True, type=_path)
    run.add_argument("--production-dso", required=True, type=_path)
    run.add_argument("--test-indexes-dso", required=True, type=_path)
    run.add_argument("--runtime-control-dso", required=True, type=_path)
    run.add_argument(
        "--production-test-executable",
        required=True,
        type=_path,
    )
    run.add_argument(
        "--test-indexes-test-executable",
        required=True,
        type=_path,
    )
    run.add_argument(
        "--production-cluster-config",
        required=True,
        type=_path,
    )
    run.add_argument(
        "--test-indexes-cluster-config",
        required=True,
        type=_path,
    )
    run.add_argument(
        "--production-stage-manifest",
        required=True,
        type=_path,
    )
    run.add_argument(
        "--test-indexes-stage-manifest",
        required=True,
        type=_path,
    )
    run.add_argument(
        "--cluster-mode",
        choices=("isolated",),
        required=True,
    )
    run.add_argument("--output-directory", required=True, type=_path)
    run.add_argument("--output-matrix", required=True, type=_path)
    run.add_argument("--stamp", type=_path)
    run.add_argument(
        "--timeout-seconds",
        type=int,
        default=DEFAULT_TIMEOUT_SECONDS,
    )
    subparsers.add_parser(
        "self-test",
        help="run bounded synthetic adversarial contract tests",
    )
    plan = subparsers.add_parser(
        "plan",
        help="derive and seal one profile runtime case plan",
    )
    plan.add_argument("--profile", choices=PROFILES, required=True)
    plan.add_argument("--ledger", required=True, type=_path)
    plan.add_argument("--runtime-inventory", required=True, type=_path)
    plan.add_argument(
        "--runtime-inventory-producer-executable",
        required=True,
        type=_path,
    )
    plan.add_argument("--sql-manifest", required=True, type=_path)
    plan.add_argument("--sql-file", required=True, type=_path)
    plan.add_argument("--control-file", required=True, type=_path)
    plan.add_argument("--control-dso", required=True, type=_path)
    plan.add_argument("--runtime-policy", required=True, type=_path)
    plan.add_argument("--template", required=True, type=_path)
    plan.add_argument("--output-plan", required=True, type=_path)
    plan.add_argument("--output-header", required=True, type=_path)
    plan.add_argument("--stamp", required=True, type=_path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = _parser()
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "self-test":
            return _self_test()
        if arguments.command == "plan":
            return _generate_plan(arguments)
        if arguments.timeout_seconds <= 0:
            _fail("--timeout-seconds", "must be positive")
        return _run_evidence(arguments)
    except (EvidenceError, OSError) as error:
        print(f"bioseq runtime safety evidence: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
