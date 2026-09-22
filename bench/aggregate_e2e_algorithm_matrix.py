#!/usr/bin/env python3
"""Merge validated E2E algorithm-matrix shard reports into a sealed ledger.

The q1024 ``ALL_RESULTS.yaml`` and q8 ``Q8_RESULTS.yaml`` ledgers are emitted
as JSON, which is a YAML 1.2 subset. The merger retains no raw timing arrays:
every update is reconstructed from immutable framework reports and their
durable raw JSONL sidecars.

The explicit approved-tuple subcommands separately validate and append the
sealed, self-contained ``ALL_RESULTS_TUPLES.yaml`` publication authority.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import contextlib
import copy
import datetime as dt
import errno
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
from typing import Any, Iterable


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
ALL_RESULTS_SCHEMA_VERSION = 6
LEGACY_RAW_PAYLOAD_SCHEMA_VERSION = 11
BENCH_FRAMEWORK_SOURCE_PATH = REPOSITORY_ROOT / "bench_framework/index.cppm"


def _framework_e2e_raw_payload_schema_version() -> int:
    """Read the single C++ authority for the current E2E raw revision."""
    try:
        source = BENCH_FRAMEWORK_SOURCE_PATH.read_text(encoding="utf-8")
    except OSError as error:
        raise RuntimeError(
            "cannot read benchmark-framework raw payload schema authority"
        ) from error
    declarations = re.findall(
        r"(?m)^[ \t]*inline constexpr std::uint64_t "
        r"e2e_raw_payload_schema_version = ([0-9]+);[ \t]*$",
        source,
    )
    if len(declarations) != 1:
        raise RuntimeError(
            "benchmark-framework raw payload schema authority must have "
            "exactly one constexpr declaration"
        )
    return int(declarations[0])


RAW_PAYLOAD_SCHEMA_VERSION = _framework_e2e_raw_payload_schema_version()
SUPPORTED_RAW_PAYLOAD_SCHEMA_VERSIONS = frozenset({
    LEGACY_RAW_PAYLOAD_SCHEMA_VERSION,
    RAW_PAYLOAD_SCHEMA_VERSION,
})
RAW_PAYLOAD_SCHEMA_POLICY = (
    f"accept_exact_retained_schema_{LEGACY_RAW_PAYLOAD_SCHEMA_VERSION}"
    f"_and_current_schema_{RAW_PAYLOAD_SCHEMA_VERSION}")
MIN_ACCEPTED_REPEATS = 5
EXPECTED_RESULT_CELLS = 1296
EXPECTED_SHARD_CONFIGS = 88
EXPECTED_ALL_REJECTED_SHARDS = 64
EXPECTED_ALL_REJECTED_CELLS = 512
EXPECTED_ALL_REJECTED_RECORDS = 2560
EXPECTED_MIXED_PERSISTED_SHARDS = 16
EXPECTED_QUERY_COUNT = 1024
EXPECTED_SHARD_KINDS = {"no_index_baselines", "persisted_index"}
EXPECTED_SHARD_TOPOLOGY = {
    "no_index_baselines": (8, 82),
    "persisted_index": (80, 8),
}
EXPECTED_METRIC_DISTRIBUTION = {15: 64, 29: 16, 32: 9}
CAMPAIGN_CONTRACT_PATH = (
    REPOSITORY_ROOT / "bench/bench_e2e_algorithm_matrix_campaign_contract.json")
CAMPAIGN_CONTRACT_SCHEMA = "bioseq.e2e.algorithm-matrix-campaign"
CAMPAIGN_CONTRACT_VERSION = 1
OBSERVED_RUNG_CAMPAIGN_CONTRACT_VERSION = 2
LEGACY_RESULT_STATUS_CONTRACT = "legacy_expected_rejections_v1"
OBSERVED_RESULT_STATUS_CONTRACT = "observed_status_v1"
OBSERVED_RUNG_BASELINE_PATH = "baseline-not-applicable"
OBSERVED_RUNG_REQUESTED_PATH_ELIGIBLE = "requested-path-eligible"
OBSERVED_RUNG_REQUESTED_PATH_INELIGIBLE = "requested-path-ineligible"
RUNG_QUERY_COUNTS = (2, 8, 16, 32, 64, 128, 256, 512, 1024)
EXPECTED_CAMPAIGN_ID = "bioseq-e2e-algorithm-matrix-local-v1"
EXPECTED_CAMPAIGN_DIGEST = (
    "011fbb1ef33a214ab80d3568c2341e8d0f8311f82e21a5955bdcb93b3c63b979")
EXPECTED_LEDGER_PATH = "ALL_RESULTS.yaml"
EXPECTED_LEDGER_CAMPAIGN_NAME = "local_e2e_algorithm_matrix"
EXPECTED_LEDGER_CONTRACT_SCHEMA = "bioseq.e2e.algorithm-matrix-all-results"
EXPECTED_LEDGER_CONTRACT_VERSION = 4
APPROVED_TUPLES_SCHEMA = "bioseq.approved-e2e-result-tuples"
APPROVED_TUPLES_LEGACY_VERSION = 2
APPROVED_TUPLES_VERSION = 3
APPROVED_TUPLES_CANDIDATE_SCHEMA = (
    "bioseq-approved-result-append-candidate")
APPROVED_TUPLES_CANDIDATE_VERSION = 2
APPROVED_TUPLES_MANIFEST_SCHEMA = (
    "bioseq-approved-result-verification-manifest")
APPROVED_TUPLES_MANIFEST_VERSION = 2
APPROVED_TUPLES_REMOTE_SCHEMA = "bioseq.remote-approved-e2e-result-tuples"
APPROVED_TUPLES_REMOTE_VERSION = 1
APPROVED_TUPLES_REMOTE_CANDIDATE_VERSION = 3
APPROVED_TUPLES_REMOTE_HOST_SCHEMA = "bioseq.remote-benchmark-host"
APPROVED_TUPLES_REMOTE_HOST_VERSION = 1
APPROVED_TUPLES_REMOTE_PROFILE = "aws_cpu_only_v1"
APPROVED_TUPLES_LEGACY_SHA256 = (
    "d2c31ead65867d24e38f3b2b75a17cf1b6a5fc66bf49b5d83a3848faae918d5c")
APPROVED_TUPLES_LEGACY_PROJECTION_SHA256 = (
    "e463090b47dbe6446b6f1667a8ff23df99c8742be25ad190201a783d04d8cb78")
APPROVED_TUPLES_HISTORICAL_TIMING_SCOPE = "historical_outer_e2e_v1"
APPROVED_TUPLES_QUERY_TIMING_SCOPE = "query_only_framework_iteration_v1"
FAST_LOCAL_TIMING_SCOPE = "query_only_framework_iteration_fast_local_single_sample_v1"
APPROVED_TUPLES_TIMING_SCOPES = (
    APPROVED_TUPLES_HISTORICAL_TIMING_SCOPE,
    APPROVED_TUPLES_QUERY_TIMING_SCOPE,
)
APPROVED_TUPLES_PROOF_CATEGORIES = (
    "source", "build", "fixture", "config", "measurement_contract",
    "chunk_plan",
)
APPROVED_TUPLES_TOP_LEVEL_FIELDS = (
    "schema", "version", "status", "purpose", "authority",
    "projection_provenance", "campaign_identity", "stable_key_contract",
    "terminology", "metric_definitions", "validity_rules", "matrix_scope",
    "run_count_expectations", "benchmark_host_profile",
    "corrected_summaries", "high_priority_families",
    "excluded_and_nonvalid_summary", "deterministic_ordering",
    "append_update_protocol", "invariants", "approved_results",
)
APPROVED_TUPLES_REMOTE_TOP_LEVEL_FIELDS = (
    "schema", "version", "status", "purpose", "authority",
    "projection_provenance", "campaign_identity", "stable_key_contract",
    "matrix_scope", "remote_host_binding", "eligible_stable_keys",
    "hardware_unavailable_cells", "append_update_protocol",
    "approved_results",
)
APPROVED_TUPLES_REMOTE_HOST_FIELDS = (
    "schema", "version", "machine_id", "cloud_provider", "aws_identity",
    "instance_type", "architecture", "cpu", "memory", "storage",
    "receipt_host", "measurement_device", "build_preset", "preset_family",
    "gpu", "descriptor_sha256",
)
APPROVED_TUPLES_REMOTE_AWS_IDENTITY_FIELDS = (
    "account_id", "region", "availability_zone", "instance_id",
    "ec2_architecture",
)
APPROVED_TUPLES_REMOTE_CPU_FIELDS = (
    "architecture", "model", "isa", "vcpu_count", "core_count",
    "threads_per_core",
)
APPROVED_TUPLES_REMOTE_MEMORY_FIELDS = ("physical_mib",)
APPROVED_TUPLES_REMOTE_STORAGE_FIELDS = (
    "kind", "root_device", "volume_id", "capacity_gib", "volume_type",
    "provisioned_iops", "provisioned_throughput_mib_per_s", "encrypted",
    "instance_store_present",
)
APPROVED_TUPLES_REMOTE_GPU_FIELDS = (
    "available", "device_count", "identity", "driver", "vulkan_available",
    "aws_gpu_info",
)
APPROVED_TUPLES_REMOTE_DISPOSITION_FIELDS = (
    "stable_key_schema", "stable_key", "campaign_id", "dataset",
    "target_size_gbp", "alphabet", "kmer_size", "window_size",
    "query_count", "mode", "pipeline", "pipeline_mode", "index_type",
    "requested_algorithms", "execution_attempted", "observed_algorithms",
    "effective_path", "prefilter", "status", "reason", "run_count",
    "metrics",
)
APPROVED_TUPLES_REMOTE_BINDING_FIELDS = (
    "machine_id", "host_descriptor_sha256", "receipt_host_sha256",
)
APPROVED_TUPLES_REMOTE_BASELINE_BINDING_FIELDS = (
    "candidate_stable_key", "baseline_stable_key",
    "host_descriptor_sha256",
)
# This is an admission map, not hardware discovery.  The producing worker must
# seal the matching live descriptor into its build receipt.  Keep these names
# synchronized with build_mcp/build_presets.py; arbitrary presets are rejected.
APPROVED_TUPLES_REMOTE_MACHINE_POLICIES = {
    "C7G": {
        "authority_filename": "ALL_RESULTS_TUPLES_REMOTE_C7G.yaml",
        "account_id": "970376923108",
        "region": "us-east-1",
        "availability_zone": "us-east-1f",
        "instance_id": "i-0cab5f0ab9a1cb002",
        "instance_type": "c7g.xlarge",
        "ec2_architecture": "arm64",
        "architecture": "aarch64",
        "hostname": "ip-172-31-72-135",
        "vcpu_count": 4,
        "core_count": 4,
        "threads_per_core": 1,
        "physical_memory_mib": 8192,
        "root_device": "/dev/sda1",
        "volume_id": "vol-0a68963a0efa7f81b",
        "capacity_gib": 500,
        "volume_type": "gp3",
        "provisioned_iops": 6000,
        "provisioned_throughput_mib_per_s": 500,
        "build_preset": "native-arm64-clang22-linux-e2e",
    },
    "M4": {
        "authority_filename": "ALL_RESULTS_TUPLES_REMOTE_M4.yaml",
        "account_id": "970376923108",
        "region": "us-east-1",
        "availability_zone": "us-east-1f",
        "instance_id": "i-09b8f0b9dc8c60564",
        "instance_type": "m4.xlarge",
        "ec2_architecture": "x86_64",
        "architecture": "x86_64",
        "hostname": "ip-172-31-65-160",
        "vcpu_count": 4,
        "core_count": 2,
        "threads_per_core": 2,
        "physical_memory_mib": 16384,
        "root_device": "/dev/sda1",
        "volume_id": "vol-0d447fc0269e11320",
        "capacity_gib": 500,
        "volume_type": "gp3",
        "provisioned_iops": 6000,
        "provisioned_throughput_mib_per_s": 500,
        "build_preset": "native-x64-clang22-linux-e2e",
    },
    "M7A": {
        "authority_filename": "ALL_RESULTS_TUPLES_REMOTE_M7A.yaml",
        "account_id": "970376923108",
        "region": "us-east-1",
        "availability_zone": "us-east-1f",
        "instance_id": "i-0c724c456e6a87366",
        "instance_type": "m7a.xlarge",
        "ec2_architecture": "x86_64",
        "architecture": "x86_64",
        "hostname": "ip-172-31-77-104",
        "vcpu_count": 4,
        "core_count": 4,
        "threads_per_core": 1,
        "physical_memory_mib": 16384,
        "root_device": "/dev/sda1",
        "volume_id": "vol-09e1fb2d8a352b673",
        "capacity_gib": 500,
        "volume_type": "gp3",
        "provisioned_iops": 6000,
        "provisioned_throughput_mib_per_s": 500,
        "build_preset": "native-x64-clang22-linux-e2e",
    },
    "M7I": {
        "authority_filename": "ALL_RESULTS_TUPLES_REMOTE_M7I.yaml",
        "account_id": "970376923108",
        "region": "us-east-1",
        "availability_zone": "us-east-1f",
        "instance_id": "i-0e647495fa0691f94",
        "instance_type": "m7i.xlarge",
        "ec2_architecture": "x86_64",
        "architecture": "x86_64",
        "hostname": "ip-172-31-73-182",
        "vcpu_count": 4,
        "core_count": 2,
        "threads_per_core": 2,
        "physical_memory_mib": 16384,
        "root_device": "/dev/sda1",
        "volume_id": "vol-032e918f7fc9c0763",
        "capacity_gib": 500,
        "volume_type": "gp3",
        "provisioned_iops": 6000,
        "provisioned_throughput_mib_per_s": 500,
        "build_preset": "native-x64-clang22-linux-e2e",
    },
    "TEST3_C7G": {
        "authority_filename": "ALL_RESULTS_TUPLES_REMOTE_TEST3_C7G.yaml",
        "account_id": "918106144232",
        "region": "us-east-1",
        "availability_zone": "us-east-1f",
        "instance_id": "i-07404edee9cd5e10f",
        "instance_type": "c7g.xlarge",
        "ec2_architecture": "arm64",
        "architecture": "aarch64",
        "hostname": "ip-172-31-69-150",
        "vcpu_count": 4,
        "core_count": 4,
        "threads_per_core": 1,
        "physical_memory_mib": 8192,
        "root_device": "/dev/sda1",
        "volume_id": "vol-0d4993baebd040336",
        "capacity_gib": 500,
        "volume_type": "gp3",
        "provisioned_iops": 6000,
        "provisioned_throughput_mib_per_s": 500,
        "build_preset": "native-arm64-clang22-linux-e2e",
    },
    "TEST3_M4": {
        "authority_filename": "ALL_RESULTS_TUPLES_REMOTE_TEST3_M4.yaml",
        "account_id": "918106144232",
        "region": "us-east-1",
        "availability_zone": "us-east-1f",
        "instance_id": "i-09295b2ebfeb60ab3",
        "instance_type": "m4.xlarge",
        "ec2_architecture": "x86_64",
        "architecture": "x86_64",
        "hostname": "ip-172-31-74-99",
        "vcpu_count": 4,
        "core_count": 2,
        "threads_per_core": 2,
        "physical_memory_mib": 16384,
        "root_device": "/dev/sda1",
        "volume_id": "vol-07e5de9485e8db5d7",
        "capacity_gib": 500,
        "volume_type": "gp3",
        "provisioned_iops": 6000,
        "provisioned_throughput_mib_per_s": 500,
        "build_preset": "native-x64-clang22-linux-e2e",
    },
    "TEST3_M7A": {
        "authority_filename": "ALL_RESULTS_TUPLES_REMOTE_TEST3_M7A.yaml",
        "account_id": "918106144232",
        "region": "us-east-1",
        "availability_zone": "us-east-1f",
        "instance_id": "i-0a0d7fd853ae150bb",
        "instance_type": "m7a.xlarge",
        "ec2_architecture": "x86_64",
        "architecture": "x86_64",
        "hostname": "ip-172-31-72-149",
        "vcpu_count": 4,
        "core_count": 4,
        "threads_per_core": 1,
        "physical_memory_mib": 16384,
        "root_device": "/dev/sda1",
        "volume_id": "vol-05e0dae7beb4b4d65",
        "capacity_gib": 500,
        "volume_type": "gp3",
        "provisioned_iops": 6000,
        "provisioned_throughput_mib_per_s": 500,
        "build_preset": "native-x64-clang22-linux-e2e",
    },
    "TEST3_M7I": {
        "authority_filename": "ALL_RESULTS_TUPLES_REMOTE_TEST3_M7I.yaml",
        "account_id": "918106144232",
        "region": "us-east-1",
        "availability_zone": "us-east-1f",
        "instance_id": "i-044628f5bf4ec153e",
        "instance_type": "m7i.xlarge",
        "ec2_architecture": "x86_64",
        "architecture": "x86_64",
        "hostname": "ip-172-31-76-75",
        "vcpu_count": 4,
        "core_count": 2,
        "threads_per_core": 2,
        "physical_memory_mib": 16384,
        "root_device": "/dev/sda1",
        "volume_id": "vol-06077dc0041ec7464",
        "capacity_gib": 500,
        "volume_type": "gp3",
        "provisioned_iops": 6000,
        "provisioned_throughput_mib_per_s": 500,
        "build_preset": "native-x64-clang22-linux-e2e",
    },
    "TEST4_C7G": {
        "authority_filename": "ALL_RESULTS_TUPLES_REMOTE_TEST4_C7G.yaml",
        "account_id": "665651551858",
        "region": "us-east-1",
        "availability_zone": "us-east-1f",
        "instance_id": "i-04ba9fa8bcc210350",
        "instance_type": "c7g.xlarge",
        "ec2_architecture": "arm64",
        "architecture": "aarch64",
        "hostname": "ip-172-31-77-174",
        "vcpu_count": 4,
        "core_count": 4,
        "threads_per_core": 1,
        "physical_memory_mib": 8192,
        "root_device": "/dev/sda1",
        "volume_id": "vol-0bcedb878c8f3a84e",
        "capacity_gib": 500,
        "volume_type": "gp3",
        "provisioned_iops": 6000,
        "provisioned_throughput_mib_per_s": 500,
        "build_preset": "native-arm64-clang22-linux-e2e",
    },
    "TEST4_M4": {
        "authority_filename": "ALL_RESULTS_TUPLES_REMOTE_TEST4_M4.yaml",
        "account_id": "665651551858",
        "region": "us-east-1",
        "availability_zone": "us-east-1f",
        "instance_id": "i-006a2d87202c3fdee",
        "instance_type": "m4.xlarge",
        "ec2_architecture": "x86_64",
        "architecture": "x86_64",
        "hostname": "ip-172-31-66-178",
        "vcpu_count": 4,
        "core_count": 2,
        "threads_per_core": 2,
        "physical_memory_mib": 16384,
        "root_device": "/dev/sda1",
        "volume_id": "vol-049805b6283ef42e4",
        "capacity_gib": 500,
        "volume_type": "gp3",
        "provisioned_iops": 6000,
        "provisioned_throughput_mib_per_s": 500,
        "build_preset": "native-x64-clang22-linux-e2e",
    },
    "TEST4_M7A": {
        "authority_filename": "ALL_RESULTS_TUPLES_REMOTE_TEST4_M7A.yaml",
        "account_id": "665651551858",
        "region": "us-east-1",
        "availability_zone": "us-east-1f",
        "instance_id": "i-029644d5d0b0c629f",
        "instance_type": "m7a.xlarge",
        "ec2_architecture": "x86_64",
        "architecture": "x86_64",
        "hostname": "ip-172-31-73-52",
        "vcpu_count": 4,
        "core_count": 4,
        "threads_per_core": 1,
        "physical_memory_mib": 16384,
        "root_device": "/dev/sda1",
        "volume_id": "vol-0b48d2ddea493ca85",
        "capacity_gib": 500,
        "volume_type": "gp3",
        "provisioned_iops": 6000,
        "provisioned_throughput_mib_per_s": 500,
        "build_preset": "native-x64-clang22-linux-e2e",
    },
    "TEST4_M7I": {
        "authority_filename": "ALL_RESULTS_TUPLES_REMOTE_TEST4_M7I.yaml",
        "account_id": "665651551858",
        "region": "us-east-1",
        "availability_zone": "us-east-1f",
        "instance_id": "i-0c2570972b289f94a",
        "instance_type": "m7i.xlarge",
        "ec2_architecture": "x86_64",
        "architecture": "x86_64",
        "hostname": "ip-172-31-71-179",
        "vcpu_count": 4,
        "core_count": 2,
        "threads_per_core": 2,
        "physical_memory_mib": 16384,
        "root_device": "/dev/sda1",
        "volume_id": "vol-0cfbd8232b6ec9223",
        "capacity_gib": 500,
        "volume_type": "gp3",
        "provisioned_iops": 6000,
        "provisioned_throughput_mib_per_s": 500,
        "build_preset": "native-x64-clang22-linux-e2e",
    },
}
_replica_registry_path = REPOSITORY_ROOT / "bench/e2e_fleet_replicas.json"
_registered_replicas = (json.loads(_replica_registry_path.read_bytes())["hosts"]
                        if _replica_registry_path.exists() else {})
APPROVED_TUPLES_REMOTE_MACHINE_POLICIES.update({
    name.upper().replace("-", "_"): row["machine_policy"]
    for name, row in _registered_replicas.items()})
APPROVED_TUPLES_NEW_FLEET_MACHINE_IDS = frozenset({
    "TEST3_C7G", "TEST3_M4", "TEST3_M7A", "TEST3_M7I",
    "TEST4_C7G", "TEST4_M4", "TEST4_M7A", "TEST4_M7I",
} | {name.upper().replace("-", "_") for name in _registered_replicas})
APPROVED_TUPLES_NEW_FLEET_CAMPAIGN_CONTRACT_SHA256 = (
    "e80407ca3a515ca04ac7ad616f0c7f5e0ef477f3e09884ec8ab93370e8c55933")
APPROVED_TUPLES_NEW_FLEET_ELIGIBLE_KEY_COUNT = 1296
APPROVED_TUPLES_NEW_FLEET_ELIGIBLE_KEYS_SHA256 = (
    "5aad579ff70013c20835e561350803653c2cb5e0323cd878ff1e40e8501f9a96")
APPROVED_TUPLES_NEW_FLEET_PURPOSE = (
    "Host-local approved E2E timings for one sealed AWS CPU machine. "
    "The authority embeds its exact EC2, CPU, RAM, root-volume, and "
    "no-GPU inventory. Eligibility is the exact sealed 1,296-cell q1024 "
    "campaign stable-key set, independent of locally measured rows; the "
    "source-local hashes record structural bootstrap provenance only and "
    "contribute no result or timing. Linux E2E intervals use "
    "CLOCK_MONOTONIC under the existing strong same-kernel nanosecond-rate "
    "contract; unavailable Vulkan cells are non-executed dispositions, "
    "never zero-time results.")
APPROVED_TUPLES_V2_ROW_FIELDS = (
    "stable_key_schema", "stable_key", "campaign_id", "dataset",
    "target_size_gbp", "alphabet", "kmer_size", "window_size",
    "query_count", "mode", "pipeline", "pipeline_mode", "index_type",
    "requested_algorithms", "observed_algorithms", "effective_path",
    "execution_conformance", "comparison_eligibility", "prefilter",
    "exact_baseline_match_count_equal", "status", "run_count", "metrics",
    "comments",
)
APPROVED_TUPLES_ROW_FIELDS = (
    "stable_key_schema", "stable_key", "timing_scope", "campaign_id", "dataset",
    "target_size_gbp", "alphabet", "kmer_size", "window_size",
    "query_count", "mode", "pipeline", "pipeline_mode", "index_type",
    "requested_algorithms", "observed_algorithms", "effective_path",
    "execution_conformance", "comparison_eligibility", "prefilter",
    "exact_baseline_match_count_equal", "status", "run_count", "metrics",
    "comments",
)
APPROVED_TUPLES_ALGORITHM_FIELDS = (
    "minimizer", "chaining", "base_alignment")
APPROVED_TUPLES_ELIGIBILITY_FIELDS = (
    "requested_path", "effective_path", "scope")
APPROVED_TUPLES_METRIC_FIELDS = ("unit", "p50_a", "p50_b")
QUERY_PREFIX_PROOF_PATH: Path | None = None
ACTIVE_CAMPAIGN_PROFILE = "q1024"

Q1024_CAMPAIGN_ID = EXPECTED_CAMPAIGN_ID
Q1024_CAMPAIGN_DIGEST = EXPECTED_CAMPAIGN_DIGEST
Q1024_CONTRACT_PATH = CAMPAIGN_CONTRACT_PATH
Q1024_CONFIG_PATH = REPOSITORY_ROOT / "bench/bench_e2e_algorithm_matrix_campaign.json"
Q8_CAMPAIGN_ID = "bioseq-e2e-algorithm-matrix-local-q8-v1"
Q8_CAMPAIGN_DIGEST = (
    "e1efe2033ea95f729e147725d2807f9c94eccb83d9cd8711879034c09e70168a")
Q8_CONTRACT_PATH = (
    REPOSITORY_ROOT / "bench/bench_e2e_algorithm_matrix_q8_campaign_manifest.json")
Q8_CONFIG_PATH = REPOSITORY_ROOT / "bench/bench_e2e_algorithm_matrix_q8_campaign.json"
Q8_QUERY_PREFIX_PROOF_PATH = (
    REPOSITORY_ROOT / "bench/bench_e2e_algorithm_matrix_q8_query_prefix_proof.json")
Q8_RESCUE_MANIFEST_PATH = (
    REPOSITORY_ROOT
    / "bench/bench_e2e_algorithm_matrix_q8_ordinal1_rescue_manifest.json")
Q1024_SINGLETON_PARTITION_PATH = (
    REPOSITORY_ROOT
    / "bench/bench_e2e_algorithm_matrix_q1024_singleton_partition_manifest.json")
Q1024_SINGLETON_PARTITION_SHA256 = (
    "6bfe0681b46087860b7d873cd868da8e70be5ecab85b817e6979a29fce6f2c3a")
OBSERVED_RUNG_SINGLETON_PARTITION_SHA256 = {
    2: "4ee3826dd40105a171ceeb1edf1fd8410d73ecda981d89daa7e424a9840ed14d",
    8: "0426c9baccdd5301ef3290d79521eb7e95f259fae62549fd66c94d155d6cd9e5",
    16: "fa1a9d90aa0805d1fe3c4dced93faa0fc525409266b57fc5e186ce33fca5fd53",
    32: "103c81eb16d43d2aacf50bbc84c7cb4ab8d6ded0003c2961d30f282914d0284d",
    64: "419f84df201df48c58ac8bf0481c924d4e81272de0da1f45b98013dd0d6e8e10",
    128: "4b797bc3e083846aeb87833b6c08f674926880675261da54453948d775f3e348",
    256: "c751bd020190e09486810624fd9ac49caed8037b54529789fafe9283c897ca9a",
    512: "11b3aca82bc385e88286dd55dd9e9de4655b5655d5685271f56c2747a0abb907",
    1024: "4913ed26a424736cef6cc86e8749eabcc6266957ac901c9a7261fe1a78c19ca1",
}
Q1024_PACKED_PARTITION_PATH = (
    REPOSITORY_ROOT
    / "bench/bench_e2e_algorithm_matrix_q1024_packed_partition_manifest.json")
Q1024_PACKED_PARTITION_SHA256 = (
    "5351e563a269c013a4a6674c6bea8bce0ef24016d1c4965582d9df670c7252ae")
Q1024_PACKED_PATH_PATTERN = re.compile(
    r"bench/e2e_algorithm_matrix_q1024_packed_shards/"
    r"bench_e2e_algorithm_matrix_q1024_packed_[a-z0-9_]+\.json")
Q1024_DEADLINE_V2_MANIFEST_PATH = (
    REPOSITORY_ROOT / "bench/bench_e2e_algorithm_matrix_deadline_v2_manifest.json")
Q1024_DEADLINE_V2_MANIFEST_SHA256 = (
    "bf2abc14dbda84079d6384e330b1fdc752e1754360cc61d9fec42af1e2d1b31c")
Q1024_DEADLINE_V2_CHECKPOINT_SHA256 = (
    "401751c75a95e8814d0a1a62d289b093c9a097b43789dc55ec89b4f4b86e4053")
Q1024_S059_PRESERVATION_CHECKPOINT_SHA256 = (
    "6bf0e0384e2f0577bb5a7dfd048d21c4b31a7a391598e2fedcdc3e408e97adb3")
Q1024_S059_PRESERVATION_RESULT_SHA256 = (
    "906b5c9e07709bae987c53c25686dd073f01a38a6e63dc5ac73ac770acac759c")
Q1024_S059_PRESERVATION_STABLE_KEY = (
    'bioseq-e2e-cell-v2:["bioseq-e2e-algorithm-matrix-local-v1",'
    '"matrix_2gbp_dna4_k15_w10","dna4",2,15,10,"no_index","bioseq",'
    '"minimizers_per_fragment","scalar","scalar_minimap2","ksw2"]')
Q1024_S059_PRESERVATION_AUTHORITY = (
    "deadline_v2_checkpoint:s059:explicit_user_preservation_rule")
Q1024_SINGLETON_PATH_PATTERN = re.compile(
    r"bench/e2e_algorithm_matrix_shards/"
    r"bench_e2e_algorithm_matrix_campaign_shard_singleton_"
    r"[0-9]{4}_[0-9a-f]{16}\.json")
Q1024_DETERMINISTIC_FIRST_SINGLETON = (
    "bench/e2e_algorithm_matrix_shards/"
    "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
    "0081_423ec685980909f5.json")
Q1024_DETERMINISTIC_FIRST_INGESTION = (
    "bench/e2e_algorithm_matrix_deadline_v2_shards/"
    "bench_e2e_algorithm_matrix_deadline_v2_s056_q1024_q1024_anchor_"
    "m2_d4_k15_w10_no_index_minimap2.json")
Q1024_DEADLINE_V1_RECEIPT_CONFIG_ALIASES = {
    56: (
        "bench/e2e_algorithm_matrix_deadline_shards/"
        "bench_e2e_algorithm_matrix_deadline_s056_q1024_q1024_anchor_"
        "m2_d4_k15_w10_no_index_minimap2.json"),
    57: (
        "bench/e2e_algorithm_matrix_deadline_shards/"
        "bench_e2e_algorithm_matrix_deadline_s057_q1024_q1024_anchor_"
        "m2_d4_k15_w10_just_index_minimap2.json"),
}
Q1024_SEALED_ADOPTION_RUN_RECEIPTS = {
    56: "rr_f6564879423bfcd46562dbb1920d6111",
    57: "rr_aa3f5446efbfe0d2de6fdae382532ca9",
    58: "rr_1e43d9e01a8a6eee985b4240ffe98d64",
}
BUILD_RECEIPT_STORE_PATH = Path("/tmp/bioseq_build_mcp_receipts.json")
BIOSEQ_ALGORITHMS = {
    "bioseq_algorithm_matrix_no_index": "no_index",
    "bioseq_algorithm_matrix_just_index": "just_index",
}
BASELINE_ALGORITHMS = {
    "minimap2_no_index": "no_index",
    "minimap2_just_index": "just_index",
}
REQUIRED_MINIMAP_VALIDITY = (
    "complete",
    "cleanup_complete",
    "split_io_valid",
    "clock_valid",
    "buffer_valid",
    "intervals_valid",
    "arithmetic_valid",
    "capacity_contract_valid",
    "query_count_valid",
    "index_part_count_valid",
    "phase_count_contract_valid",
    "workload_valid",
    "valid",
)
STABLE_KEY_FIELDS = (
    "campaign_id",
    "dataset",
    "alphabet",
    "target_size_gbp",
    "kmer_size",
    "window_size",
    "index_mode",
    "engine",
    "bioseq_index_name",
    "requested_minimizer_algorithm",
    "requested_chaining_algorithm",
    "requested_base_alignment_algorithm",
)
RAW_MANIFEST_FIELDS = {
    "schema",
    "path",
    "run_id",
    "complete",
    "record_count",
    "prefix_bytes",
    "append_order_checksum",
    "schedule_sorted_checksum",
    "outer_e2e_ns",
    "index_and_map_ns",
    "cells",
}
RAW_CELL_FIELDS = {
    "benchmark",
    "test_case",
    "algorithm",
    "tuple_index",
    "group_index",
    "record_count",
    "schedule_sorted_checksum",
    "outer_e2e_ns",
    "first_repeat_outer_e2e_ns",
    "steady_state_outer_e2e_ns",
    "index_and_map_ns",
}
RAW_SCHEDULE_FIELDS = {
    "benchmark_index",
    "test_case_index",
    "tuple_index",
    "group_index",
    "repeat",
    "mode",
    "configured_seed",
    "shuffle_seed",
    "order_position",
}
REPORT_FIELDS = {
    "meta",
    "raw_evidence",
    "paired_rule_evidence",
    "rule_violations",
    "regressions",
    "sanity_warnings",
    "benchmarks",
}
ACCEPTED_DISPOSITIONS = {
    "accepted",
    "accepted_fallback",
    "accepted_behavior_difference",
}
EXPECTED_REJECTION_DISPOSITIONS = {"rejected_setup", "rejected_sql"}
INVALID_DISPOSITIONS = {
    "invalid",
    "rejected_attestation",
    "rejected_correctness",
    "skipped_after_prior_attestation_rejection",
}
MEASURED_QUERY_CONNECTION_FAILURE_SCHEMA = (
    "bioseq.e2e.measured-query-connection-failure")
MEASURED_QUERY_CONNECTION_FAILURE_REASON = (
    "**INVALID**: measured PostgreSQL query lost its backend connection; "
    "same postmaster/database and a changed live backend were proven")
ATTESTATION_FIELDS_V11 = {
    "actual_backend",
    "actual_target_index_source",
    "backend_declines",
    "backend_fallbacks",
    "block_aligner_execution_stats",
    "documented_minimizer_fallback_selected",
    "hash_major_index_reader_stats",
    "mapq_executed",
    "mapq_execution_available",
    "mapq_execution_conflict",
    "minimizer_fragment_reader_stats",
    "nthash_vector_activity",
    "observed_base_alignment_algorithm",
    "observed_chaining_algorithm",
    "observed_minimizer_algorithm",
    "observed_simd_alignment_algorithm",
    "observed_simd_chaining_policy",
    "persisted_gpu_operation_stats_accepted",
    "prefilter_execution_stats",
    "requested_backend",
    "requested_base_alignment_algorithm",
    "requested_chaining_algorithm",
    "requested_minimizer_algorithm",
    "requested_simd_alignment_algorithm",
    "requested_simd_chaining_policy",
    "requested_target_index_source",
    "stage_engine_matrix_valid",
    "stats_saturated",
    "validation_accepted",
    "validation_attached_to_measurement",
    "validation_present",
    "vulkan_synthetic_constant_mapq_not_executed",
}
ATTESTATION_FIELDS_V12 = (
    ATTESTATION_FIELDS_V11
    - {"vulkan_synthetic_constant_mapq_not_executed"}
    | {
        "numeric_mapq_matches_minimap2_semantics_claimed",
        "vulkan_physical_mapq_executed",
    }
)
# Retain the established internal name for historical fixture consumers.
ATTESTATION_FIELDS = ATTESTATION_FIELDS_V11
PREFILTER_EXECUTION_STATS_FIELDS = {
    "available",
    "dispatches",
    "sneaky_snake_dispatches",
    "gatekeeper_dispatches",
    "scalar_dispatches",
    "simd_dispatches",
    "vulkan_dispatches",
    "jobs",
    "evaluated_jobs",
    "length_64_jobs",
    "length_difference_jobs",
    "unsupported_jobs",
    "rejected_jobs",
    "rejected_regions",
    "surviving_regions",
    "mask_rejection_candidates",
    "exact_guard_activations",
    "guard_confirmed_rejections",
    "hamming_exact_rejections",
    "saturated",
}
BLOCK_ALIGNER_EXECUTION_STATS_FIELDS = {
    "available",
    "consistent",
    "executed_calls",
    "nonempty_calls",
    "nonempty_calls_with_simd",
    "simd_dp_vector_batches",
    "simd_dp_cells",
    "forbidden_ksw2_calls",
    "forbidden_scalar_biological_fallbacks",
    "saturated",
}
SIMD_EXECUTION_STATS_FIELDS = {
    "schema",
    "revision",
    "requested_minimizer_algorithm",
    "observed_minimizer_algorithm",
    "requested_chaining_algorithm",
    "observed_chaining_algorithm",
    "requested_base_alignment_algorithm",
    "observed_base_alignment_algorithm",
    "requested_policy",
    "observed_policy",
    "requested_alignment_algorithm",
    "observed_alignment_algorithm",
    "downstream_active_wall_available",
    "downstream_active_wall_ns",
    "downstream_active_wall_invocation_count",
    "downstream_active_wall_unavailable_invocation_count",
    "downstream_work_ns",
    "downstream_work_saturated",
    "mapq_execution_available",
    "mapq_executed",
    "mapq_execution_conflict",
    "benchmark_e2e_interval_union",
    "nthash_vector_batches",
    "nthash_resident_vector_stream_kernel_calls",
    "simd_chaining_exact_policy_calls",
    "simd_chaining_bounded_policy_calls",
    "simd_chaining_bounded_score_batches",
    "simd_chaining_bounded_cutoff_stops",
    "simd_chaining_bounded_candidates_skipped",
    "simd_chaining_policy_rejections",
    "are_simd_chaining_policy_stats_saturated",
    "block_aligner_execution_stats",
    "prefilter_execution_stats",
}
ALIGNMENT_SNAPSHOT_STAGES = (
    "query_sketch",
    "target_sketch",
    "target_index_build",
    "persisted_index_decode",
    "partition_selection",
    "device_index_upload",
    "seed_lookup",
    "chain_score",
    "chain_backtrack",
    "region_generation",
    "primary_secondary_reduction",
    "divergence_estimation",
    "mapq",
    "target_window_plan",
    "target_window_io",
    "target_window_upload",
    "cigar_job_generation",
    "left_extension",
    "gap_extension",
    "right_extension",
)
FULL_DECLARED_METRIC_NAMES = (
    "wall_clock_ns",
    "region_total_ns",
    "tuple_accepted",
    "match_count",
    "expected_match_count",
    "correct_match_count",
    "exact_primary_count_accepted",
    "correct_cigar_count",
    "cigar_replay_count",
    "mapq_result_count",
    "pipeline_validation_accepted",
    "stage_engine_matrix_valid",
    "backend_declines",
    "backend_fallbacks",
    "prefilter_activity_count",
    "measured_region_ns_manual",
    "cigar_or_mapq_interval_union_ns",
    "outer_e2e_minus_cigar_or_mapq_union_ns",
    "benchmark_e2e_interval_schema_revision",
    "benchmark_e2e_interval_available",
    "benchmark_e2e_interval_count",
    "benchmark_e2e_cigar_interval_count",
    "benchmark_e2e_mapq_interval_count",
    "benchmark_e2e_interval_finalization_count",
    "minimap2_receipt_available",
    "minimap2_cigar_interval_count",
    "minimap2_mapq_interval_count",
    "downstream_active_wall_ns",
    "downstream_active_wall_available",
    "downstream_work_ns",
    "mapq_execution_available",
    "mapq_executed",
)
ALL_REJECTED_METRIC_NAMES = FULL_DECLARED_METRIC_NAMES[:15]
MINIMAP2_RECEIPT_METRIC_NAMES = {
    "minimap2_receipt_available",
    "minimap2_cigar_interval_count",
    "minimap2_mapq_interval_count",
}
MIXED_PERSISTED_METRIC_NAMES = tuple(
    name for name in FULL_DECLARED_METRIC_NAMES
    if name not in MINIMAP2_RECEIPT_METRIC_NAMES
)
BIOSEQ_ONLY_EVALUATION_METRIC_NAMES = {
    "benchmark_e2e_interval_schema_revision",
    "benchmark_e2e_interval_available",
    "benchmark_e2e_interval_count",
    "benchmark_e2e_cigar_interval_count",
    "benchmark_e2e_mapq_interval_count",
    "benchmark_e2e_interval_finalization_count",
    "downstream_active_wall_ns",
    "downstream_active_wall_available",
    "downstream_work_ns",
    "mapq_execution_available",
    "mapq_executed",
}
MINIMAP2_BASELINE_METRIC_NAMES = tuple(
    name for name in FULL_DECLARED_METRIC_NAMES
    if name not in BIOSEQ_ONLY_EVALUATION_METRIC_NAMES
)


def _observed_rung_execution_contract_id(query_count: int) -> str:
    """Return the collision-free execution identity for one q rung."""
    return f"bioseq-e2e-algorithm-matrix-local-q{query_count}-observed-v1"


def _observed_rung_singleton_partition_path(query_count: int) -> Path:
    return (
        REPOSITORY_ROOT
        / f"bench/bench_e2e_algorithm_matrix_q{query_count}_"
          "observed_singleton_partition_manifest.json")


def _observed_rung_singleton_path_pattern(query_count: int) -> re.Pattern[str]:
    prefix = f"bench_e2e_algorithm_matrix_q{query_count}_observed_singleton"
    return re.compile(
        rf"bench/e2e_algorithm_matrix_q{query_count}_observed_singletons/"
        rf"{prefix}_[0-9]{{4}}_[0-9a-f]{{16}}\.json")


def _allowed_cell_statuses(engine: str, result_status_contract: str) -> list[str]:
    """Return the exact publishable statuses sealed by a campaign manifest."""
    if engine == "minimap2" or result_status_contract == OBSERVED_RESULT_STATUS_CONTRACT:
        return ["accepted"]
    if (engine == "bioseq"
            and result_status_contract == LEGACY_RESULT_STATUS_CONTRACT):
        return ["accepted", "rejected"]
    raise ContractError(
        f"unknown engine/result-status contract {engine!r}/{result_status_contract!r}")


def _activate_campaign_profile(ledger_path: str) -> None:
    """Select one exact ledger/campaign pair before reading any evidence."""
    global ACTIVE_CAMPAIGN_PROFILE
    global ALL_RESULTS_SCHEMA_VERSION
    global CAMPAIGN_CONTRACT_PATH
    global EXPECTED_CAMPAIGN_DIGEST
    global EXPECTED_CAMPAIGN_ID
    global EXPECTED_LEDGER_CAMPAIGN_NAME
    global EXPECTED_LEDGER_CONTRACT_SCHEMA
    global EXPECTED_LEDGER_CONTRACT_VERSION
    global EXPECTED_LEDGER_PATH
    global EXPECTED_METRIC_DISTRIBUTION
    global EXPECTED_QUERY_COUNT
    global EXPECTED_SHARD_CONFIGS
    global EXPECTED_SHARD_KINDS
    global EXPECTED_SHARD_TOPOLOGY
    global QUERY_PREFIX_PROOF_PATH

    if ledger_path == "ALL_RESULTS.yaml":
        ACTIVE_CAMPAIGN_PROFILE = "q1024"
        ALL_RESULTS_SCHEMA_VERSION = 6
        CAMPAIGN_CONTRACT_PATH = Q1024_CONTRACT_PATH
        EXPECTED_CAMPAIGN_ID = Q1024_CAMPAIGN_ID
        EXPECTED_CAMPAIGN_DIGEST = Q1024_CAMPAIGN_DIGEST
        EXPECTED_LEDGER_PATH = "ALL_RESULTS.yaml"
        EXPECTED_LEDGER_CAMPAIGN_NAME = "local_e2e_algorithm_matrix"
        EXPECTED_LEDGER_CONTRACT_SCHEMA = (
            "bioseq.e2e.algorithm-matrix-all-results")
        EXPECTED_LEDGER_CONTRACT_VERSION = 4
        EXPECTED_QUERY_COUNT = 1024
        EXPECTED_SHARD_CONFIGS = 88
        EXPECTED_SHARD_KINDS = {"no_index_baselines", "persisted_index"}
        EXPECTED_SHARD_TOPOLOGY = {
            "no_index_baselines": (8, 82),
            "persisted_index": (80, 8),
        }
        EXPECTED_METRIC_DISTRIBUTION = {15: 64, 29: 16, 32: 9}
        QUERY_PREFIX_PROOF_PATH = None
        return
    if ledger_path == "Q8_RESULTS.yaml":
        ACTIVE_CAMPAIGN_PROFILE = "q8"
        ALL_RESULTS_SCHEMA_VERSION = 1
        CAMPAIGN_CONTRACT_PATH = Q8_CONTRACT_PATH
        EXPECTED_CAMPAIGN_ID = Q8_CAMPAIGN_ID
        EXPECTED_CAMPAIGN_DIGEST = Q8_CAMPAIGN_DIGEST
        EXPECTED_LEDGER_PATH = "Q8_RESULTS.yaml"
        EXPECTED_LEDGER_CAMPAIGN_NAME = "local_e2e_algorithm_matrix_q8"
        EXPECTED_LEDGER_CONTRACT_SCHEMA = (
            "bioseq.e2e.algorithm-matrix-q8-working-results")
        EXPECTED_LEDGER_CONTRACT_VERSION = 1
        EXPECTED_QUERY_COUNT = 8
        EXPECTED_SHARD_CONFIGS = 256
        EXPECTED_SHARD_KINDS = {
            "no_index_micro", "minimap2_baseline", "persisted_index"}
        EXPECTED_SHARD_TOPOLOGY = {
            "no_index_micro": (160, 4),
            "minimap2_baseline": (16, 1),
            "persisted_index": (80, 8),
        }
        EXPECTED_METRIC_DISTRIBUTION = {15: 64, 21: 16, 29: 176, 32: 1}
        QUERY_PREFIX_PROOF_PATH = Q8_QUERY_PREFIX_PROOF_PATH
        return
    raise ContractError(
        "--all-results must be exactly ALL_RESULTS.yaml or Q8_RESULTS.yaml")


def _activate_observed_rung_profile(
        manifest_path: Path, expected_digest: str, query_count: int) -> None:
    """Select one read-only observed-status rung without mutating a ledger."""
    global ACTIVE_CAMPAIGN_PROFILE
    global CAMPAIGN_CONTRACT_PATH
    global EXPECTED_CAMPAIGN_DIGEST
    global EXPECTED_CAMPAIGN_ID
    global EXPECTED_QUERY_COUNT
    global QUERY_PREFIX_PROOF_PATH

    if query_count not in RUNG_QUERY_COUNTS:
        raise ContractError(
            "--query-count must be one exact DOC0433 q-ladder value")
    if re.fullmatch(r"[0-9a-f]{64}", expected_digest) is None:
        raise ContractError(
            "--expected-campaign-digest must be 64 lowercase hexadecimal characters")
    ACTIVE_CAMPAIGN_PROFILE = "observed_rung"
    CAMPAIGN_CONTRACT_PATH = manifest_path
    EXPECTED_CAMPAIGN_ID = Q1024_CAMPAIGN_ID
    EXPECTED_CAMPAIGN_DIGEST = expected_digest
    EXPECTED_QUERY_COUNT = query_count
    QUERY_PREFIX_PROOF_PATH = None


class ContractError(RuntimeError):
    """Raised when evidence cannot be accepted without guessing."""


class ApprovedTupleDuplicateError(ContractError):
    """Exit 3: approved tuple identity, ordering, or replay conflict."""


class ApprovedTupleProofError(ContractError):
    """Exit 4: append seal, proof, binding, or repeat evidence failed."""


class ApprovedTupleIOError(ContractError):
    """Exit 5: append locking or durable replacement failed."""


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ContractError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise ContractError(f"non-finite JSON token {value!r}")


def _loads_json(text: str, label: str) -> Any:
    try:
        return json.loads(
            text,
            object_pairs_hook=_strict_object,
            parse_constant=_reject_constant,
        )
    except (json.JSONDecodeError, UnicodeError, ContractError) as exc:
        raise ContractError(f"{label} is not strict JSON: {exc}") from exc


def _loads_gate8_explain_json(text: str) -> Any:
    """Read the known Vulkan static/runtime property overlap, not arbitrary duplicates."""
    normalized_overlap = False

    def explain_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        nonlocal normalized_overlap
        positions = [index for index, (key, _) in enumerate(pairs)
                     if key == "uses_max_chain_skip"]
        if len(positions) > 1:
            # The unchanged Vulkan emitter supplies this constant in both
            # populate_runtime_params and static_properties. Retain the
            # original EXPLAIN bytes; normalize only this identical overlap.
            owner = {
                "Stage Name": "chain_stage",
                "Algorithm": "Vulkan tiled DP",
                "accelerator": "vulkan",
                "score_generation": "g3sa-tiled-dp",
                "backtrack": "bioseq-cpu",
            }
            if (normalized_overlap or len(positions) != 2
                    or any(pairs[index][1] != "false" for index in positions)
                    or any([value for key, value in pairs if key == name] != [expected]
                           for name, expected in owner.items())):
                raise ContractError(
                    "duplicate uses_max_chain_skip is not the known Vulkan property overlap")
            duplicate = positions[1]
            pairs = pairs[:duplicate] + pairs[duplicate + 1:]
            normalized_overlap = True
        return _strict_object(pairs)

    try:
        return json.loads(
            text, object_pairs_hook=explain_object, parse_constant=_reject_constant)
    except (json.JSONDecodeError, UnicodeError, ContractError) as exc:
        raise ContractError(f"Gate-8 EXPLAIN JSON is not valid: {exc}") from exc


def _canonical_repository_relative(value: str, label: str) -> str:
    """Reject absolute, aliased, or platform-dependent repository paths."""
    if not isinstance(value, str) or not value:
        raise ContractError(f"{label} must be a non-empty repository-relative path")
    if (value.startswith("/") or "\\" in value
            or any(ord(character) < 0x20 or ord(character) == 0x7F
                   for character in value)):
        raise ContractError(f"{label} is not a canonical repository-relative path")
    components = value.split("/")
    if any(component in {"", ".", ".."} for component in components):
        raise ContractError(f"{label} contains a lexical path alias")
    if Path(value).as_posix() != value:
        raise ContractError(f"{label} is not in canonical repository form")
    return value


def _canonical_repository_file(value: str, label: str) -> Path:
    """Resolve no aliases: every parent is a real directory and leaf a file."""
    relative = _canonical_repository_relative(value, label)
    current = REPOSITORY_ROOT
    components = relative.split("/")
    for index, component in enumerate(components):
        current = current / component
        try:
            metadata = current.lstat()
        except OSError as exc:
            raise ContractError(f"cannot inspect {label} {current}: {exc}") from exc
        if stat.S_ISLNK(metadata.st_mode):
            raise ContractError(f"{label} may not traverse a symlink: {current}")
        if index + 1 == len(components):
            if not stat.S_ISREG(metadata.st_mode):
                raise ContractError(
                    f"{label} must be a regular non-symlink file: {current}")
        elif not stat.S_ISDIR(metadata.st_mode):
            raise ContractError(
                f"{label} parent component is not a directory: {current}")
    return current


def _verified_repository_file(path: Path, label: str) -> Path:
    """Verify an internally constructed absolute path without canonicalizing it."""
    try:
        relative = path.relative_to(REPOSITORY_ROOT).as_posix()
    except ValueError as exc:
        raise ContractError(f"{label} must stay inside the repository") from exc
    verified = _canonical_repository_file(relative, label)
    if verified != path:
        raise ContractError(f"{label} path is not lexically canonical")
    return verified


def _read_regular(path: Path, label: str) -> bytes:
    _verified_repository_file(path, label)
    flags = os.O_RDONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        raise ContractError(f"cannot open {label} {path}: {exc}") from exc
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise ContractError(f"{label} changed from a regular file while opening")
        with os.fdopen(descriptor, "rb", closefd=False) as handle:
            return handle.read()
    except OSError as exc:
        raise ContractError(f"cannot read {label} {path}: {exc}") from exc
    finally:
        os.close(descriptor)


def _read_external_regular(path: Path, label: str) -> bytes:
    """Read one explicitly sealed non-repository artifact without following links."""
    if not path.is_absolute():
        raise ContractError(f"{label} must use its sealed absolute path")
    try:
        metadata = path.lstat()
    except OSError as exc:
        raise ContractError(f"cannot inspect {label} {path}: {exc}") from exc
    if not stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        raise ContractError(f"{label} must be a regular non-symlink file")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as exc:
        raise ContractError(f"cannot open {label} {path}: {exc}") from exc
    try:
        opened = os.fstat(descriptor)
        if (not stat.S_ISREG(opened.st_mode)
                or opened.st_dev != metadata.st_dev
                or opened.st_ino != metadata.st_ino):
            raise ContractError(f"{label} changed while opening")
        with os.fdopen(descriptor, "rb", closefd=False) as handle:
            return handle.read()
    finally:
        os.close(descriptor)


def _read_json_file(path: Path, label: str) -> tuple[dict[str, Any], bytes]:
    raw = _read_regular(path, label)
    try:
        text = raw.decode("utf-8")
    except UnicodeError as exc:
        raise ContractError(f"{label} is not UTF-8: {exc}") from exc
    value = _loads_json(text, label)
    if not isinstance(value, dict):
        raise ContractError(f"{label} root must be an object")
    return value, raw


def _sha256(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _relative(path: Path) -> str:
    verified = _verified_repository_file(path, "repository evidence path")
    return verified.relative_to(REPOSITORY_ROOT).as_posix()


def _require_dict(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ContractError(f"{label} must be an object")
    return value


def _require_list(value: Any, label: str) -> list[Any]:
    if not isinstance(value, list):
        raise ContractError(f"{label} must be an array")
    return value


def _require_int(value: Any, label: str, *, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ContractError(f"{label} must be an integer >= {minimum}")
    return value


def _require_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise ContractError(f"{label} must be a non-empty string")
    return value


def _require_text(value: Any, label: str) -> str:
    if not isinstance(value, str):
        raise ContractError(f"{label} must be a string")
    return value


def _require_bool(value: Any, label: str) -> bool:
    if not isinstance(value, bool):
        raise ContractError(f"{label} must be a Boolean")
    return value


def _require_exact_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise ContractError(
            f"{label} has the wrong exact keys; missing={missing}, extra={extra}")


def _require_hex(value: Any, label: str, digits: int) -> str:
    text = _require_string(value, label)
    if len(text) != digits or any(character not in "0123456789abcdef" for character in text):
        raise ContractError(f"{label} must be {digits} lowercase hexadecimal digits")
    return text


def _canonical_bytes(value: Any) -> bytes:
    try:
        return json.dumps(
            value,
            ensure_ascii=True,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise ContractError(f"value is not canonically serializable: {exc}") from exc


BUILD_RECEIPT_FIELDS = {
    "artifact_counts", "authorized_artifacts", "binary_subdir", "build_dir",
    "build_phase_completed_at", "build_phase_duration_sec",
    "build_phase_started_at", "build_receipt_id", "build_timeout_sec",
    "configuration", "configure_completed_at", "configure_duration_sec",
    "content_sha256", "created_at", "debug", "exit_code", "fresh", "host",
    "kind", "mode", "preset", "preset_family", "runtime_artifacts",
    "schema_revision", "source_commit", "source_graph_fingerprint",
    "source_repository_root", "status", "target_timings", "targets",
    "termination_evidence",
}
RUN_RECEIPT_FIELDS = {
    "authorized_artifacts_sha256", "bench_e2e_config", "bench_flags",
    "build_receipt_id", "content_sha256", "created_at", "exit_code", "host",
    "kind", "mode", "reports", "reused_build_receipt",
    "reused_build_receipt_content_sha256", "runtime_artifacts",
    "schema_revision", "source_commit", "source_graph_fingerprint", "status",
    "test_flags",
}


def _validate_content_seal(
        value: Any, expected_keys: set[str], label: str) -> dict[str, Any]:
    receipt = _require_dict(value, label)
    _require_exact_keys(receipt, expected_keys, label)
    claimed = _require_hex(receipt.get("content_sha256"), f"{label} seal", 64)
    payload = copy.deepcopy(receipt)
    del payload["content_sha256"]
    if _sha256(_canonical_bytes(payload)) != claimed:
        raise ContractError(f"{label} content seal does not recompute")
    return receipt


def _approved_e2e_receipt_preset_matches(receipt: dict[str, Any]) -> bool:
    """Admit only the local preset or one closed remote Clang E2E preset."""
    if receipt.get("preset_family") != "e2e":
        return False
    preset = receipt.get("preset")
    if preset == "bench-e2e-opt":
        return True
    host = receipt.get("host")
    if not isinstance(host, dict) or host.get("system") != "Linux":
        return False
    matching_hosts = [
        policy for policy in APPROVED_TUPLES_REMOTE_MACHINE_POLICIES.values()
        if ((policy["build_preset"] == preset
             or (policy["build_preset"] == "native-x64-clang22-linux-e2e"
                 and preset == "aws-x64-linux-e2e"))
            and policy["architecture"] == host.get("machine")
            and policy["hostname"] == host.get("node"))
    ]
    return len(matching_hosts) == 1


def _validate_build_receipt_snapshot(
        value: Any, label: str) -> dict[str, Any]:
    imported = isinstance(value, dict) and value.get("kind") == "bioseq-e2e-artifact-import-receipt"
    receipt = _validate_content_seal(value, BUILD_RECEIPT_FIELDS | ({"artifact_import"} if imported else set()), label)
    if imported:
        provenance = _require_dict(receipt.get("artifact_import"), label + " artifact import")
        origin = _validate_build_receipt_snapshot(provenance.get("source_receipt"), label + " original build")
        if origin.get("kind") != "bioseq-build-receipt":
            raise ContractError(label + " import must refer directly to an original build")
        changed = {"kind", "schema_revision", "build_receipt_id", "created_at",
                   "host", "artifact_counts", "content_sha256"}
        if any(receipt[key] != origin[key] for key in origin if key not in changed):
            raise ContractError(label + " import altered original artifact/configuration/source evidence")
        if any(receipt["host"].get(key) != origin["host"].get(key) for key in ("system", "machine")):
            raise ContractError(label + " import changed platform/architecture")
    if (receipt.get("kind") not in {"bioseq-build-receipt", "bioseq-e2e-artifact-import-receipt"}
            or receipt.get("schema_revision") != (3 if imported else 2)
            or receipt.get("status") != "completed"
            or receipt.get("exit_code") != 0
            or receipt.get("mode") != "benchmark-e2e"
            or receipt.get("debug") is not False
            or not _approved_e2e_receipt_preset_matches(receipt)
            or receipt.get("binary_subdir") != "bioseq"
            or receipt.get("source_repository_root") != str(REPOSITORY_ROOT)):
        raise ContractError(
            f"{label} is not an exact completed approved E2E receipt")
    _require_string(receipt.get("build_receipt_id"), f"{label} id")
    _require_string(receipt.get("source_commit"), f"{label} source commit")
    _require_string(
        receipt.get("source_graph_fingerprint"), f"{label} source graph")
    runtime = _require_list(receipt.get("runtime_artifacts"), f"{label} runtime")
    if not any(
            isinstance(item, dict)
            and isinstance(item.get("path"), str)
            and Path(item["path"]).name == "bioseq_benchmark_e2e"
            and isinstance(item.get("sha256"), str)
            for item in runtime):
        raise ContractError(f"{label} does not seal bioseq_benchmark_e2e")
    _require_list(
        receipt.get("authorized_artifacts"), f"{label} authorized artifacts")
    return receipt


def _sealed_report_config_location(
        value: Any,
        config_path: Path,
        config_entry: dict[str, Any],
        label: str,
) -> Path:
    text = _require_string(value, label)
    if Path(text).is_absolute():
        execution_authority = config_entry.get("execution_authority")
        exact_observed_rung_location = False
        if (ACTIVE_CAMPAIGN_PROFILE == "observed_rung"
                and execution_authority == "observed_rung_singleton"):
            manifest_location = _canonical_repository_file(
                _require_string(
                    config_entry.get("path"),
                    "observed singleton manifest config path"),
                "observed singleton manifest config")
            exact_observed_rung_location = (
                config_path == manifest_location
                and text == str(manifest_location))
        authorized_paths = {config_path}
        if execution_authority == "q1024_deadline_v2_adoption":
            adoption = _require_dict(
                config_entry.get("sealed_evidence"),
                "q1024 adoption evidence")
            receipt_config = _require_dict(
                adoption.get("run_receipt_config"),
                "q1024 adoption run-receipt config")
            authorized_paths.add(_canonical_repository_file(
                _require_string(
                    receipt_config.get("path"),
                    "q1024 adoption run-receipt config path"),
                "q1024 adoption run-receipt config"))
        if (not exact_observed_rung_location
                and (execution_authority not in {
                    "q8_rescue", "q1024_deadline_v2_adoption"}
                    or text not in {str(path) for path in authorized_paths})):
            raise ContractError(
                f"{label} absolute spelling is not manifest authority")
        return _verified_repository_file(Path(text), label)
    return _canonical_repository_file(text, label)


def _validate_run_receipt_snapshot(
        run_value: Any,
        build_value: Any,
        report_path: Path,
        config_path: Path,
        label: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    run = _validate_content_seal(run_value, RUN_RECEIPT_FIELDS, label)
    build = _validate_build_receipt_snapshot(
        build_value, f"{label} reused build receipt")
    report_raw = _read_regular(report_path, f"{label} report")
    config_raw = _read_regular(config_path, f"{label} config")
    expected_report = [{
        "path": str(report_path),
        "size": len(report_raw),
        "sha256": _sha256(report_raw),
    }]
    expected_config = {
        "path": str(config_path),
        "sha256": _sha256(config_raw),
    }
    if (run.get("kind") != "bioseq-run-receipt"
            or run.get("schema_revision") != 1
            or run.get("status") != "completed"
            or run.get("exit_code") != 0
            or run.get("mode") != "benchmark-e2e"
            or run.get("bench_flags") not in (
                ["--filter", "benchmark=bench_e2e"],
                ["--filter", "benchmark=bench_e2e", "--debug"],
            )
            or run.get("test_flags") != []
            or run.get("bench_e2e_config") != expected_config
            or run.get("reports") != expected_report
            or run.get("reused_build_receipt")
                != build.get("build_receipt_id")
            or run.get("reused_build_receipt_content_sha256")
                != build.get("content_sha256")
            or run.get("source_commit") != build.get("source_commit")
            or run.get("source_graph_fingerprint")
                != build.get("source_graph_fingerprint")
            or run.get("host") != build.get("host")
            or run.get("runtime_artifacts") != build.get("runtime_artifacts")
            or run.get("authorized_artifacts_sha256")
                != _sha256(_canonical_bytes(build.get("authorized_artifacts")))):
        raise ContractError(
            f"{label} does not bind the exact report/config/build execution")
    _require_string(run.get("build_receipt_id"), f"{label} id")
    return run, build


def _fnv1a(texts: Iterable[str]) -> str:
    state = 14_695_981_039_346_656_037
    for text in texts:
        for byte in text.encode("utf-8"):
            state ^= byte
            state = (state * 1_099_511_628_211) & ((1 << 64) - 1)
    return f"{state:016x}"


def _fnv1a_u64(state: int, value: int) -> int:
    for shift in range(0, 64, 8):
        state ^= (value >> shift) & 0xFF
        state = (state * 1_099_511_628_211) & ((1 << 64) - 1)
    return state


def _schedule_identity(
        benchmark: str,
        test_case: str,
        group_index: int,
        repeat: int) -> int:
    state = 14_695_981_039_346_656_037
    state = _fnv1a_u64(state, len(benchmark.encode("utf-8")))
    for byte in benchmark.encode("utf-8"):
        state ^= byte
        state = (state * 1_099_511_628_211) & ((1 << 64) - 1)
    state = _fnv1a_u64(state, len(test_case.encode("utf-8")))
    for byte in test_case.encode("utf-8"):
        state ^= byte
        state = (state * 1_099_511_628_211) & ((1 << 64) - 1)
    state = _fnv1a_u64(state, group_index)
    return _fnv1a_u64(state, repeat)


def _splitmix64_next(state: int) -> tuple[int, int]:
    mask = (1 << 64) - 1
    state = (state + 0x9E3779B97F4A7C15) & mask
    value = state
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & mask
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & mask
    return state, (value ^ (value >> 31)) & mask


def _schedule_shuffle_seed(
        configured_seed: int,
        benchmark: str,
        test_case: str,
        group_index: int,
        repeat: int) -> int:
    _, value = _splitmix64_next(
        configured_seed ^ _schedule_identity(
            benchmark, test_case, group_index, repeat))
    return value


def _fisher_yates_order(count: int, shuffle_seed: int) -> list[int]:
    order = list(range(count))
    state = shuffle_seed
    for remaining in range(count, 1, -1):
        state, random_value = _splitmix64_next(state)
        swap_index = random_value % remaining
        order[remaining - 1], order[swap_index] = (
            order[swap_index], order[remaining - 1])
    return order


def _duration_summary(samples: list[int]) -> dict[str, Any]:
    if not samples:
        return {"count": 0, "min": None, "p50": None, "mean": None, "p95": None}
    ordered = sorted(samples)
    percentile = lambda percent: ordered[(percent * len(ordered) + 99) // 100 - 1]
    return {
        "count": len(ordered),
        "min": ordered[0],
        "p50": percentile(50),
        "mean": sum(ordered) / len(ordered),
        "p95": percentile(95),
    }


def _non_null_numeric_timing_paths(
        value: Any, path: str = "raw") -> list[str]:
    """Return every recursively retained E2E duration-valued field."""
    retained: list[str] = []
    if isinstance(value, dict):
        for key, child in value.items():
            child_path = f"{path}.{key}"
            if ((key.endswith("_ns") or key.endswith("_ns_manual"))
                    and child is not None):
                retained.append(child_path)
            retained.extend(_non_null_numeric_timing_paths(child, child_path))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            retained.extend(
                _non_null_numeric_timing_paths(child, f"{path}[{index}]"))
    return retained


def _validate_duration_summary(
        value: Any,
        expected: dict[str, Any],
        label: str,
) -> None:
    summary = _require_dict(value, label)
    _require_exact_keys(summary, {"count", "min", "p50", "mean", "p95"}, label)
    count = _require_int(summary.get("count"), f"{label}.count")
    if count == 0:
        if any(summary.get(key) is not None for key in ("min", "p50", "mean", "p95")):
            raise ContractError(f"{label} empty summary must contain only null statistics")
    else:
        for key in ("min", "p50", "p95"):
            _require_int(summary.get(key), f"{label}.{key}")
        mean = summary.get("mean")
        if (isinstance(mean, bool) or not isinstance(mean, (int, float))
                or not math.isfinite(mean) or mean < 0):
            raise ContractError(f"{label}.mean must be one finite nonnegative number")
    if summary != expected:
        raise ContractError(f"{label} does not recompute exactly")


def _stable_key_for_identity(identity: dict[str, Any]) -> str:
    if tuple(identity) != STABLE_KEY_FIELDS:
        raise ContractError("stable identity field order changed")
    return "bioseq-e2e-cell-v2:" + json.dumps(
        list(identity.values()), ensure_ascii=True, separators=(",", ":"))


def _approved_identity_from_stable_key(value: Any, label: str) -> dict[str, Any]:
    stable_key = _require_string(value, label)
    prefix = "bioseq-e2e-cell-v2:"
    if not stable_key.startswith(prefix):
        raise ContractError(f"{label} is not a bioseq-e2e-cell-v2 key")
    decoded = _loads_json(stable_key[len(prefix):], label)
    if not isinstance(decoded, list) or len(decoded) != len(STABLE_KEY_FIELDS):
        raise ContractError(f"{label} does not contain the canonical identity width")
    identity = dict(zip(STABLE_KEY_FIELDS, decoded))
    if _stable_key_for_identity(identity) != stable_key:
        raise ContractError(f"{label} is not canonically encoded")
    return identity


def _remote_identity_requires_vulkan(identity: dict[str, Any]) -> bool:
    return any(
        isinstance(identity[field], str)
        and identity[field].startswith("vulkan_")
        for field in (
            "requested_minimizer_algorithm",
            "requested_chaining_algorithm",
            "requested_base_alignment_algorithm",
        )
    )


def _remote_baseline_stable_key(row: dict[str, Any]) -> str:
    identity = _approved_row_identity(row)
    identity.update({
        "engine": "minimap2",
        "bioseq_index_name": None,
        "requested_minimizer_algorithm": None,
        "requested_chaining_algorithm": None,
        "requested_base_alignment_algorithm": None,
    })
    return _stable_key_for_identity(identity)


def _execution_descriptors_from_config(
        config: dict[str, Any], campaign_id: str,
        result_status_contract: str = LEGACY_RESULT_STATUS_CONTRACT,
        *, query_count: int | None = None,
) -> list[dict[str, Any]]:
    """Expand one sealed config using the framework's lexicographic map order."""
    params = _require_dict(config.get("params"), "execution config params")
    bench = _require_dict(config.get("bench_e2e"), "execution config bench_e2e")
    matrix = _require_dict(bench.get("matrix"), "execution config matrix")
    over = _require_dict(matrix.get("over"), "execution config matrix.over")
    expanded_tuples: list[tuple[str, dict[str, Any]]] = []
    for algorithm in sorted(over):
        declarations = _require_dict(
            over.get(algorithm), f"execution axes for {algorithm}")
        schema = _require_dict(
            params.get(algorithm), f"execution params for {algorithm}")
        partial: list[dict[str, Any]] = [{}]
        for parameter in sorted(declarations):
            if parameter not in schema:
                raise ContractError(
                    f"execution axis {algorithm}.{parameter} has no schema")
            values = _require_list(
                declarations.get(parameter),
                f"execution axis {algorithm}.{parameter}")
            if not values or any(isinstance(value, (dict, list)) or value is None
                                 for value in values):
                raise ContractError(
                    f"execution axis {algorithm}.{parameter} is empty or nonscalar")
            next_partial: list[dict[str, Any]] = []
            for accumulated in partial:
                for value in values:
                    item = dict(accumulated)
                    item[parameter] = value
                    next_partial.append(item)
            partial = next_partial
        for item in partial:
            for parameter, raw_declaration in schema.items():
                declaration = _require_dict(
                    raw_declaration, f"execution param {algorithm}.{parameter}")
                if parameter not in item:
                    if "default" not in declaration:
                        raise ContractError(
                            f"execution param {algorithm}.{parameter} has no value")
                    item[parameter] = declaration["default"]
            expanded_tuples.append((algorithm, dict(sorted(item.items()))))

    repeats = _require_dict(matrix.get("for"), "execution config matrix.for")
    test_cases = _require_dict(config.get("test_cases"), "execution test cases")
    expected_query_count = EXPECTED_QUERY_COUNT if query_count is None else query_count
    if set(repeats) != set(test_cases):
        raise ContractError("execution test-case ownership differs from matrix.for")
    result: list[dict[str, Any]] = []
    for test_case_index, (test_case, repeat_count) in enumerate(repeats.items()):
        if repeat_count != MIN_ACCEPTED_REPEATS:
            raise ContractError("execution config must own exactly five repeats")
        test_case_value = _require_dict(
            test_cases.get(test_case), f"execution test case {test_case}")
        data = _require_dict(
            test_case_value.get("data"), f"execution test case {test_case} data")
        target_size = _require_int(
            data.get("reference_length"), f"{test_case} reference_length", minimum=1)
        if target_size % 1_000_000_000 != 0:
            raise ContractError("execution target size is not whole Gbp")
        if data.get("fragment_count") != expected_query_count:
            raise ContractError(
                f"execution query count is not exact {expected_query_count}")
        for tuple_index, (algorithm, parameters) in enumerate(expanded_tuples):
            if algorithm in BIOSEQ_ALGORITHMS:
                engine = "bioseq"
                mode = BIOSEQ_ALGORITHMS[algorithm]
                index_name = parameters.get("minimizer_index")
                minimizer = parameters.get("minimizer_algorithm")
                chaining = parameters.get("chaining_algorithm")
                alignment = parameters.get("base_alignment_algorithm")
                allowed_statuses = _allowed_cell_statuses(
                    engine, result_status_contract)
                if any(value is None for value in (
                        index_name, minimizer, chaining, alignment)):
                    raise ContractError("Bioseq execution tuple is incomplete")
            elif algorithm in BASELINE_ALGORITHMS:
                engine = "minimap2"
                mode = BASELINE_ALGORITHMS[algorithm]
                index_name = minimizer = chaining = alignment = None
                allowed_statuses = _allowed_cell_statuses(
                    engine, result_status_contract)
            else:
                raise ContractError(
                    f"execution config uses unknown algorithm {algorithm!r}")
            identity = {
                "campaign_id": campaign_id,
                "dataset": _require_string(
                    data.get("size_label"), f"{test_case} size_label"),
                "alphabet": _require_string(
                    data.get("alphabet"), f"{test_case} alphabet"),
                "target_size_gbp": target_size // 1_000_000_000,
                "kmer_size": _require_int(
                    data.get("kmer_size"), f"{test_case} kmer_size", minimum=1),
                "window_size": _require_int(
                    data.get("window_size"), f"{test_case} window_size", minimum=1),
                "index_mode": mode,
                "engine": engine,
                "bioseq_index_name": index_name,
                "requested_minimizer_algorithm": minimizer,
                "requested_chaining_algorithm": chaining,
                "requested_base_alignment_algorithm": alignment,
            }
            result.append({
                "stable_key": _stable_key_for_identity(identity),
                "identity": identity,
                "test_case": test_case,
                "test_case_index": test_case_index,
                "algorithm": algorithm,
                "tuple_index": tuple_index,
                "params": parameters,
                "allowed_statuses": allowed_statuses,
            })
    keys = [cell["stable_key"] for cell in result]
    if len(keys) != len(set(keys)):
        raise ContractError("execution config expands the same stable key twice")
    return result


def _config_nonblocking_rule_names(config: dict[str, Any]) -> list[str]:
    metrics = _require_dict(
        _require_dict(
            _require_dict(config.get("bench_e2e"), "config bench_e2e").get("matrix"),
            "config matrix").get("metrics"),
        "config metrics")
    match_count = _require_dict(metrics.get("match_count"), "match_count metric")
    result: list[str] = []
    for raw_rule in _require_list(match_count.get("rules"), "match_count rules"):
        rule = _require_list(raw_rule, "match_count rule")
        if len(rule) != 4:
            raise ContractError("match_count rule must have four entries")
        result.append(_require_string(rule[0], "match_count rule name"))
    if len(result) != len(set(result)):
        raise ContractError("match_count rule names are duplicated")
    return result


def _canonical_cell_rejection_disposition(
        identity: dict[str, Any],
        result_status_contract: str = LEGACY_RESULT_STATUS_CONTRACT,
) -> str | None:
    """Return a sealed legacy rejection for one persisted identity."""
    if result_status_contract == OBSERVED_RESULT_STATUS_CONTRACT:
        return None
    if result_status_contract != LEGACY_RESULT_STATUS_CONTRACT:
        raise ContractError("unknown result-status contract")
    if (identity["engine"] != "bioseq"
            or identity["index_mode"] != "just_index"):
        return None
    profile = (identity["kmer_size"], identity["window_size"])
    if profile == (20, 15):
        return "rejected_setup"
    if profile != (15, 10):
        return None
    if identity["alphabet"] == "dna5":
        return "rejected_setup"
    # The canonical config digest seals MAPQ projection on. Runtime therefore
    # rejects even Vulkan/Vulkan/Block at its complete-all-GPU selector.
    if (identity["alphabet"] == "dna4"
            and identity["requested_minimizer_algorithm"]
                == "vulkan_minimap_integer"):
        return "rejected_sql"
    return None


def _expected_all_records_disposition(
        cells: list[dict[str, Any]],
        result_status_contract: str = LEGACY_RESULT_STATUS_CONTRACT,
) -> str | None:
    """Return one disposition only when every contract cell shares it."""
    if not cells:
        return None
    dispositions = {
        _canonical_cell_rejection_disposition(
            cell["identity"], result_status_contract)
        for cell in cells
    }
    if None in dispositions or len(dispositions) != 1:
        return None
    return dispositions.pop()


def _validate_source_evidence(value: Any, label: str) -> None:
    """Recompute one proof's exact source hash and snippet locations."""
    evidence = _require_dict(value, label)
    _require_exact_keys(evidence, {"path", "sha256", "evidence"}, label)
    source_path = _canonical_repository_file(
        _require_string(evidence.get("path"), f"{label} path"),
        f"{label} source",
    )
    source_raw = _read_regular(source_path, f"{label} source")
    if _require_hex(evidence.get("sha256"), f"{label} sha256", 64) \
            != _sha256(source_raw):
        raise ContractError(f"{label} source hash changed")
    try:
        source_text = source_raw.decode("utf-8")
    except UnicodeError as exc:
        raise ContractError(f"{label} source is not UTF-8: {exc}") from exc
    snippets = _require_list(evidence.get("evidence"), f"{label} snippets")
    if not snippets:
        raise ContractError(f"{label} must contain at least one source snippet")
    for index, raw_snippet in enumerate(snippets):
        snippet = _require_dict(raw_snippet, f"{label} snippet[{index}]")
        _require_exact_keys(
            snippet, {"line", "statement"}, f"{label} snippet[{index}]")
        statement = _require_string(
            snippet.get("statement"), f"{label} snippet[{index}] statement")
        offset = source_text.find(statement)
        expected_line = _require_int(
            snippet.get("line"), f"{label} snippet[{index}] line", minimum=1)
        if offset < 0 or source_text.count("\n", 0, offset) + 1 != expected_line:
            raise ContractError(f"{label} source snippet location changed")


def _validate_q8_query_prefix_proof(
        contract: dict[str, Any], contract_raw: bytes) -> tuple[str, str, str]:
    """Validate and hash the q8 ordered-prefix authority in full."""
    if QUERY_PREFIX_PROOF_PATH is None:
        raise AssertionError("q8 prefix-proof validation selected without a path")
    proof, proof_raw = _read_json_file(
        QUERY_PREFIX_PROOF_PATH, "q8 query-prefix proof")
    _require_exact_keys(proof, {
        "schema",
        "version",
        "verdict",
        "campaign_binding",
        "q1024_binding",
        "query_generation_contract",
        "matrix_and_repeat_contract",
        "timeout_and_shard_contract",
        "ownership_and_resume_contract",
        "rescue_execution_authority",
    }, "q8 query-prefix proof")
    if (proof.get("schema")
            != "bioseq.e2e.algorithm-matrix-q8-prefix-proof"
            or proof.get("version") != 2
            or proof.get("verdict")
                != "proven_exact_fixed_seed_ordered_prefix"):
        raise ContractError("q8 query-prefix proof schema/verdict changed")

    q8_config, q8_config_raw = _read_json_file(
        Q8_CONFIG_PATH, "q8 campaign config")
    q1024_contract, q1024_contract_raw = _read_json_file(
        Q1024_CONTRACT_PATH, "q1024 campaign contract")
    q1024_config, q1024_config_raw = _read_json_file(
        Q1024_CONFIG_PATH, "q1024 campaign config")
    q1024_campaign = _require_dict(
        q1024_contract.get("campaign"), "q1024 campaign identity")
    if (q1024_campaign.get("campaign_id") != Q1024_CAMPAIGN_ID
            or q1024_campaign.get("campaign_digest") != Q1024_CAMPAIGN_DIGEST):
        raise ContractError("q8 prefix proof no longer binds canonical q1024")

    campaign_binding = _require_dict(
        proof.get("campaign_binding"), "q8 proof campaign binding")
    q1024_binding = _require_dict(
        proof.get("q1024_binding"), "q8 proof q1024 binding")
    binding_keys = {
        "campaign_id", "campaign_digest", "manifest", "manifest_sha256",
        "config", "config_sha256",
    }
    _require_exact_keys(
        campaign_binding, binding_keys, "q8 proof campaign binding")
    _require_exact_keys(
        q1024_binding, binding_keys, "q8 proof q1024 binding")
    expected_q8_binding = {
        "campaign_id": Q8_CAMPAIGN_ID,
        "campaign_digest": Q8_CAMPAIGN_DIGEST,
        "manifest": _relative(Q8_CONTRACT_PATH),
        "manifest_sha256": _sha256(contract_raw),
        "config": _relative(Q8_CONFIG_PATH),
        "config_sha256": _sha256(q8_config_raw),
    }
    expected_q1024_binding = {
        "campaign_id": Q1024_CAMPAIGN_ID,
        "campaign_digest": Q1024_CAMPAIGN_DIGEST,
        "manifest": _relative(Q1024_CONTRACT_PATH),
        "manifest_sha256": _sha256(q1024_contract_raw),
        "config": _relative(Q1024_CONFIG_PATH),
        "config_sha256": _sha256(q1024_config_raw),
    }
    if campaign_binding != expected_q8_binding:
        raise ContractError("q8 prefix proof campaign binding changed")
    if q1024_binding != expected_q1024_binding:
        raise ContractError("q8 prefix proof q1024 binding changed")

    query_contract = _require_dict(
        proof.get("query_generation_contract"),
        "q8 query-generation contract")
    _require_exact_keys(query_contract, {
        "fragment_count_is_query_count",
        "reference_generation_is_independent_of_fragment_count",
        "fragment_stream_key",
        "first_fragment_index",
        "q8_fragment_indices",
        "q8_is_subset_of_q1024",
        "q8_is_ordered_prefix_of_q1024",
        "parallel_schedule_cannot_change_per_index_streams",
        "benchmark_loader",
        "sequence_generator",
        "datasets",
    }, "q8 query-generation contract")
    if ({
            key: query_contract.get(key) for key in (
                "fragment_count_is_query_count",
                "reference_generation_is_independent_of_fragment_count",
                "fragment_stream_key",
                "first_fragment_index",
                "q8_fragment_indices",
                "q8_is_subset_of_q1024",
                "q8_is_ordered_prefix_of_q1024",
                "parallel_schedule_cannot_change_per_index_streams",
            )
        } != {
            "fragment_count_is_query_count": True,
            "reference_generation_is_independent_of_fragment_count": True,
            "fragment_stream_key":
                "seed_stream(seed, k_reference_stream_space + fragment_index)",
            "first_fragment_index": 0,
            "q8_fragment_indices": list(range(8)),
            "q8_is_subset_of_q1024": True,
            "q8_is_ordered_prefix_of_q1024": True,
            "parallel_schedule_cannot_change_per_index_streams": True,
        }):
        raise ContractError("q8 ordered-prefix generation contract changed")
    _validate_source_evidence(
        query_contract.get("benchmark_loader"), "q8 benchmark-loader evidence")
    _validate_source_evidence(
        query_contract.get("sequence_generator"), "q8 sequence-generator evidence")

    q8_test_cases = _require_dict(q8_config.get("test_cases"), "q8 test cases")
    q1024_test_cases = _require_dict(
        q1024_config.get("test_cases"), "q1024 test cases")
    if tuple(q8_test_cases) != tuple(q1024_test_cases) or len(q8_test_cases) != 8:
        raise ContractError("q8/q1024 dataset membership changed")
    dataset_proofs = _require_list(
        query_contract.get("datasets"), "q8 dataset prefix proofs")
    if len(dataset_proofs) != len(q8_test_cases):
        raise ContractError("q8 dataset prefix-proof cardinality changed")
    for dataset, raw_dataset_proof in zip(q8_test_cases, dataset_proofs):
        dataset_proof = _require_dict(
            raw_dataset_proof, f"q8 dataset proof {dataset}")
        _require_exact_keys(dataset_proof, {
            "test_case",
            "alphabet",
            "reference_length",
            "seed",
            "q1024_fragment_count",
            "q8_fragment_count",
            "generation_data_without_fragment_count_sha256",
            "q8_fragment_indices",
            "relation",
        }, f"q8 dataset proof {dataset}")
        q8_data = copy.deepcopy(_require_dict(
            _require_dict(q8_test_cases[dataset], f"q8 {dataset}").get("data"),
            f"q8 {dataset} data"))
        q1024_data = copy.deepcopy(_require_dict(
            _require_dict(
                q1024_test_cases[dataset], f"q1024 {dataset}").get("data"),
            f"q1024 {dataset} data"))
        q8_count = q8_data.pop("fragment_count", None)
        q1024_count = q1024_data.pop("fragment_count", None)
        expected_dataset_proof = {
            "test_case": dataset,
            "alphabet": q8_data.get("alphabet"),
            "reference_length": q8_data.get("reference_length"),
            "seed": q8_data.get("seed"),
            "q1024_fragment_count": q1024_count,
            "q8_fragment_count": q8_count,
            "generation_data_without_fragment_count_sha256":
                _sha256(_canonical_bytes(q8_data)),
            "q8_fragment_indices": list(range(8)),
            "relation": "exact ordered prefix of q1024 indices 0..7",
        }
        if (q8_count != 8 or q1024_count != 1024
                or q8_data != q1024_data
                or dataset_proof != expected_dataset_proof):
            raise ContractError(f"q8 dataset prefix proof changed for {dataset}")

    matrix_contract = _require_dict(
        proof.get("matrix_and_repeat_contract"), "q8 matrix proof")
    _require_exact_keys(matrix_contract, {
        "result_cells",
        "records",
        "repetitions_per_cell",
        "repeat_indices",
        "iteration_order",
        "full_matrix_and_metric_declarations_byte_equivalent",
        "non_campaign_cell_identity_fields_preserved",
        "ksw2_batched_excluded",
        "cigar_projection",
        "mapq_projection",
        "prefilter_enabled",
        "sealed_matrix_evidence",
    }, "q8 matrix proof")
    if ({key: matrix_contract.get(key) for key in matrix_contract
         if key != "sealed_matrix_evidence"} != {
            "result_cells": EXPECTED_RESULT_CELLS,
            "records": EXPECTED_RESULT_CELLS * MIN_ACCEPTED_REPEATS,
            "repetitions_per_cell": MIN_ACCEPTED_REPEATS,
            "repeat_indices": list(range(MIN_ACCEPTED_REPEATS)),
            "iteration_order": {"mode": "interleaved_seeded", "seed": 333},
            "full_matrix_and_metric_declarations_byte_equivalent": True,
            "non_campaign_cell_identity_fields_preserved": True,
            "ksw2_batched_excluded": True,
            "cigar_projection": True,
            "mapq_projection": True,
            "prefilter_enabled": False,
        }
            or q8_config.get("params") != q1024_config.get("params")
            or _require_dict(q8_config.get("bench_e2e"), "q8 bench_e2e")
                .get("matrix")
                != _require_dict(
                    q1024_config.get("bench_e2e"), "q1024 bench_e2e")
                    .get("matrix")
            or q8_config.get("regression_rules")
                != q1024_config.get("regression_rules")):
        raise ContractError("q8 matrix/repeat projection changed")
    _validate_source_evidence(
        matrix_contract.get("sealed_matrix_evidence"),
        "q8 sealed-matrix evidence")

    timeout_contract = _require_dict(
        proof.get("timeout_and_shard_contract"), "q8 timeout/shard proof")
    if timeout_contract != {
        "runtime_hard_timeout_seconds": 14_400,
        "required_shards": 256,
        "no_index_micro_shards": 160,
        "maximum_bioseq_cells_per_no_index_shard": 4,
        "isolated_minimap2_baseline_shards": 16,
        "maximum_cells_per_minimap2_baseline_shard": 1,
        "persisted_index_shards": 80,
        "maximum_cells_per_persisted_index_shard": 8,
    }:
        raise ContractError("q8 timeout/shard topology proof changed")

    ownership = _require_dict(
        proof.get("ownership_and_resume_contract"), "q8 ownership proof")
    q8_ownership = _require_dict(
        contract.get("shard_ownership"), "q8 manifest ownership")
    q1024_ownership = _require_dict(
        q1024_contract.get("shard_ownership"), "q1024 manifest ownership")
    q8_monoliths = [
        entry for entry in _require_list(contract.get("configs"), "q8 configs")
        if isinstance(entry, dict) and entry.get("role") == "monolithic"]
    if len(q8_monoliths) != 1 or ownership != {
        "exact_nonoverlapping": True,
        "all_cells_appear_exactly_once": True,
        "stable_key_schema": "bioseq-e2e-cell-v2",
        "stable_key_fields": list(STABLE_KEY_FIELDS),
        "stable_keys_sha256": q8_ownership.get("stable_keys_sha256"),
        "monolithic_stable_keys_sha256":
            q8_monoliths[0].get("expected_stable_keys_sha256"),
        "q1024_stable_keys_sha256":
            q1024_ownership.get("stable_keys_sha256"),
        "campaign_namespace_is_collision_free": True,
        "q8_is_eligible_for_q1024_automatic_merge": False,
        "aggregation_authority":
            "q8 reports must be resumed and aggregated only against this q8 manifest",
    }:
        raise ContractError("q8 ownership/resume proof changed")
    rescue = _require_dict(
        proof.get("rescue_execution_authority"),
        "q8 rescue execution authority")
    _require_exact_keys(rescue, {
        "schema", "version", "ordinal", "manifest", "manifest_sha256",
        "canonical_parent_union_cells", "canonical_parent_union_records",
    }, "q8 rescue execution authority")
    rescue_path = _canonical_repository_file(
        _require_string(rescue.get("manifest"), "q8 rescue manifest path"),
        "q8 rescue manifest")
    rescue_raw = _read_regular(rescue_path, "q8 rescue manifest")
    rescue_sha256 = _require_hex(
        rescue.get("manifest_sha256"), "q8 rescue manifest SHA-256", 64)
    if (rescue_path != Q8_RESCUE_MANIFEST_PATH
            or rescue.get("schema") != "bioseq.e2e.algorithm-matrix-q8-rescue"
            or rescue.get("version") != 2
            or rescue.get("ordinal") != 1
            or rescue.get("canonical_parent_union_cells") != 4
            or rescue.get("canonical_parent_union_records") != 20
            or _sha256(rescue_raw) != rescue_sha256):
        raise ContractError("q8 rescue execution-authority pin changed")
    return _relative(QUERY_PREFIX_PROOF_PATH), _sha256(proof_raw), rescue_sha256


def _public_config_entry(entry: dict[str, Any]) -> dict[str, Any]:
    return {
        key: copy.deepcopy(value)
        for key, value in entry.items()
        if key not in {
            "descriptors_by_key", "execution_descriptors_by_key",
            "execution_authority", "sealed_evidence", "canonical_parent",
        }
    }


def _execution_entry(
        *,
        path: str,
        config_sha256: str,
        parent: dict[str, Any],
        canonical_cell: dict[str, Any],
        execution_cell: dict[str, Any],
        declared_metric_names: list[str],
        expected_disposition: str | None,
        authority: str,
    sealed_evidence: dict[str, Any] | None = None,
) -> dict[str, Any]:
    return _multi_execution_entry(
        path=path,
        config_sha256=config_sha256,
        parent=parent,
        canonical_cells=[canonical_cell],
        execution_cells=[execution_cell],
        declared_metric_names=declared_metric_names,
        allowed_nonblocking_rule_names=[],
        expected_disposition=expected_disposition,
        authority=authority,
        sealed_evidence=sealed_evidence,
    )


def _multi_execution_entry(
        *,
        path: str,
        config_sha256: str,
        parent: dict[str, Any],
        canonical_cells: list[dict[str, Any]],
        execution_cells: list[dict[str, Any]],
        declared_metric_names: list[str],
        allowed_nonblocking_rule_names: list[str],
        expected_disposition: str | None,
        authority: str,
        sealed_evidence: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Build one exact execution owner for one or more canonical cells."""
    if not canonical_cells or len(canonical_cells) != len(execution_cells):
        raise ContractError("execution entry cell cardinality is invalid")
    canonical_by_key = {
        cell["stable_key"]: cell for cell in canonical_cells}
    execution_by_key = {
        cell["stable_key"]: cell for cell in execution_cells}
    if (len(canonical_by_key) != len(canonical_cells)
            or set(canonical_by_key) != set(execution_by_key)):
        raise ContractError("execution entry stable-key mapping is not bijective")
    stable_keys = sorted(canonical_by_key)
    return {
        "path": path,
        "config_sha256": config_sha256,
        "role": "shard",
        "shard_kind": parent["shard_kind"],
        "expected_result_cells": len(canonical_cells),
        "expected_raw_records": len(canonical_cells) * MIN_ACCEPTED_REPEATS,
        "expected_stable_keys_sha256": _sha256(
            "\n".join(stable_keys).encode("utf-8")),
        "allowed_nonblocking_rule_names": allowed_nonblocking_rule_names,
        "declared_metric_names": declared_metric_names,
        "expected_all_records_disposition": expected_disposition,
        "cells": execution_cells,
        "descriptors_by_key": canonical_by_key,
        "execution_descriptors_by_key": execution_by_key,
        "execution_authority": authority,
        "sealed_evidence": sealed_evidence,
        "canonical_parent": parent,
    }


def _load_q8_rescue_manifest(
        contract: dict[str, Any],
        contract_raw: bytes,
        configs_by_path: dict[Path, dict[str, Any]],
        expected_manifest_sha256: str,
) -> dict[Path, dict[str, Any]]:
    manifest, manifest_raw = _read_json_file(
        Q8_RESCUE_MANIFEST_PATH, "q8 rescue manifest")
    if _sha256(manifest_raw) != expected_manifest_sha256:
        raise ContractError("q8 rescue manifest bytes differ from the pinned SHA-256")
    _require_exact_keys(manifest, {
        "schema", "version", "ordinal", "created_at", "purpose",
        "campaign_binding", "parent", "build_receipt", "children",
        "union_proof", "interrupted_parent_evidence",
    }, "q8 rescue manifest")
    if (manifest.get("schema") != "bioseq.e2e.algorithm-matrix-q8-rescue"
            or manifest.get("version") != 2
            or manifest.get("ordinal") != 1):
        raise ContractError("q8 rescue manifest schema/ordinal changed")
    binding = _require_dict(
        manifest.get("campaign_binding"), "q8 rescue campaign binding")
    expected_binding = {
        "campaign_id": Q8_CAMPAIGN_ID,
        "campaign_digest": Q8_CAMPAIGN_DIGEST,
        "canonical_manifest": _relative(Q8_CONTRACT_PATH),
        "canonical_manifest_sha256": _sha256(contract_raw),
        "query_prefix_proof": _relative(Q8_QUERY_PREFIX_PROOF_PATH),
    }
    if binding != expected_binding:
        raise ContractError("q8 rescue campaign/prefix binding changed")
    parent_value = _require_dict(manifest.get("parent"), "q8 rescue parent")
    parent_path = _canonical_repository_file(
        _require_string(parent_value.get("path"), "q8 rescue parent path"),
        "q8 rescue parent config")
    parent = configs_by_path.get(parent_path)
    if parent is None or parent_value != _public_config_entry(parent):
        raise ContractError("q8 rescue parent is not one exact canonical shard")
    if (parent.get("expected_result_cells") != 4
            or parent.get("expected_raw_records") != 20
            or parent.get("expected_all_records_disposition") is not None):
        raise ContractError("q8 rescue parent is not the exact four-cell timed shard")
    parent_config, parent_raw = _read_json_file(parent_path, "q8 rescue parent")
    if _sha256(parent_raw) != parent["config_sha256"]:
        raise ContractError("q8 rescue parent config hash changed")

    build = _validate_build_receipt_snapshot(
        manifest.get("build_receipt"), "q8 rescue build receipt")
    if (build.get("build_receipt_id")
            != "br_7d3103c3a6f867a41c4028e335e1403b"):
        raise ContractError("q8 rescue build receipt identity changed")
    proof = _require_dict(manifest.get("union_proof"), "q8 rescue union proof")
    if proof != {
        "child_config_count": 4,
        "expected_result_cells": 4,
        "expected_raw_records": 20,
        "repetitions_per_cell": 5,
        "repeat_indices": list(range(5)),
        "parent_union_exact": True,
        "duplicate_cells_permitted": False,
        "stable_keys_sha256": parent["expected_stable_keys_sha256"],
        "only_permitted_config_differences": [
            "/bench_e2e/matrix/over/bioseq_algorithm_matrix_no_index/"
            "base_alignment_algorithm",
            "/bench_e2e/matrix/over/bioseq_algorithm_matrix_no_index/"
            "minimizer_index",
        ],
        "only_normalized_descriptor_axes": ["test_case_index", "tuple_index"],
        "prefilter_enabled": False,
        "pipeline_mode": "task_parallel",
        "cigar_projection": True,
        "mapq_projection": True,
        "excluded_base_alignment_algorithms": ["ksw2_batched"],
        "partial_parent_records_are_provenance_only": True,
    }:
        raise ContractError("q8 rescue union/invariant proof changed")
    interrupted = _require_dict(
        manifest.get("interrupted_parent_evidence"),
        "q8 interrupted-parent provenance")
    _require_exact_keys(
        interrupted, {"reason", "partial_evidence", "replacement_rule"},
        "q8 interrupted-parent provenance")

    children = _require_list(manifest.get("children"), "q8 rescue children")
    if len(children) != 4:
        raise ContractError("q8 rescue must contain exactly four children")
    result: dict[Path, dict[str, Any]] = {}
    keys: list[str] = []
    for ordinal, raw_child in enumerate(children):
        child = _require_dict(raw_child, f"q8 rescue child[{ordinal}]")
        _require_exact_keys(child, {
            "ordinal", "path", "config_sha256", "parent_test_case_index",
            "parent_tuple_index", "stable_key", "stable_key_sha256",
            "expected_stable_keys_sha256", "expected_result_cells",
            "expected_raw_records", "repetitions_per_cell", "canonical_cell",
            "execution_cell", "report", "worker_log", "run_receipt_id",
            "run_receipt_sha256", "run_receipt",
        }, f"q8 rescue child[{ordinal}]")
        if child.get("ordinal") != ordinal:
            raise ContractError("q8 rescue child ordinal order changed")
        parent_tuple = _require_int(
            child.get("parent_tuple_index"), "q8 rescue parent tuple")
        parent_case = _require_int(
            child.get("parent_test_case_index"), "q8 rescue parent case")
        canonical_matches = [
            cell for cell in parent["cells"]
            if cell["tuple_index"] == parent_tuple
            and cell["test_case_index"] == parent_case
        ]
        if len(canonical_matches) != 1:
            raise ContractError("q8 rescue child has no unique parent cell")
        canonical_cell = canonical_matches[0]
        if child.get("canonical_cell") != canonical_cell:
            raise ContractError("q8 rescue canonical cell changed")
        execution_cell = copy.deepcopy(canonical_cell)
        execution_cell["test_case_index"] = 0
        execution_cell["tuple_index"] = 0
        if child.get("execution_cell") != execution_cell:
            raise ContractError(
                "q8 rescue normalization changed more than matrix coordinates")
        stable_key = canonical_cell["stable_key"]
        stable_digest = _sha256(stable_key.encode("utf-8"))
        if (child.get("stable_key") != stable_key
                or child.get("stable_key_sha256") != stable_digest
                or child.get("expected_stable_keys_sha256") != stable_digest
                or child.get("expected_result_cells") != 1
                or child.get("expected_raw_records") != 5
                or child.get("repetitions_per_cell") != 5):
            raise ContractError("q8 rescue child cardinality/stable-key proof changed")
        child_path = _canonical_repository_file(
            _require_string(child.get("path"), "q8 rescue child path"),
            "q8 rescue child config")
        if child_path.parent != REPOSITORY_ROOT / "bench/e2e_algorithm_matrix_q8_rescue_shards":
            raise ContractError("q8 rescue child escaped its exact directory")
        child_config, child_raw = _read_json_file(
            child_path, "q8 rescue child config")
        config_sha = _require_hex(
            child.get("config_sha256"), "q8 rescue child config SHA", 64)
        if _sha256(child_raw) != config_sha:
            raise ContractError("q8 rescue child config hash changed")
        expected_child_config = copy.deepcopy(parent_config)
        axes = expected_child_config["bench_e2e"]["matrix"]["over"][
            "bioseq_algorithm_matrix_no_index"]
        axes["base_alignment_algorithm"] = [
            canonical_cell["params"]["base_alignment_algorithm"]]
        axes["minimizer_index"] = [
            canonical_cell["params"]["minimizer_index"]]
        if child_config != expected_child_config:
            raise ContractError(
                "q8 rescue child differs beyond its two permitted matrix axes")

        report = _require_dict(child.get("report"), "q8 rescue report evidence")
        _require_exact_keys(report, {
            "path", "sha256", "size", "raw_sidecar", "run_id",
            "schedule_sorted_checksum",
        }, "q8 rescue report evidence")
        report_path = _canonical_repository_file(
            _require_string(report.get("path"), "q8 rescue report path"),
            "q8 rescue report")
        report_raw = _read_regular(report_path, "q8 rescue report")
        if (len(report_raw) != report.get("size")
                or _sha256(report_raw) != report.get("sha256")):
            raise ContractError("q8 rescue report artifact hash/size changed")
        raw_evidence = _require_dict(
            report.get("raw_sidecar"), "q8 rescue raw evidence")
        _require_exact_keys(
            raw_evidence, {"path", "sha256", "size"},
            "q8 rescue raw evidence")
        raw_path = _canonical_repository_file(
            _require_string(raw_evidence.get("path"), "q8 rescue raw path"),
            "q8 rescue raw sidecar")
        raw_bytes = _read_regular(raw_path, "q8 rescue raw sidecar")
        if (len(raw_bytes) != raw_evidence.get("size")
                or _sha256(raw_bytes) != raw_evidence.get("sha256")):
            raise ContractError("q8 rescue raw artifact hash/size changed")
        log = _require_dict(child.get("worker_log"), "q8 rescue worker log")
        _require_exact_keys(log, {"path", "sha256", "size"}, "q8 rescue worker log")
        log_path = Path(_require_string(log.get("path"), "q8 rescue log path"))
        log_raw = _read_external_regular(log_path, "q8 rescue worker log")
        if (len(log_raw) != log.get("size")
                or _sha256(log_raw) != log.get("sha256")):
            raise ContractError("q8 rescue worker-log hash/size changed")
        run, reused = _validate_run_receipt_snapshot(
            child.get("run_receipt"), build, report_path, child_path,
            "q8 rescue run receipt")
        if (child.get("run_receipt_id") != run.get("build_receipt_id")
                or child.get("run_receipt_sha256") != run.get("content_sha256")
                or reused != build):
            raise ContractError("q8 rescue receipt identity changed")
        report_document = _loads_json(
            report_raw.decode("utf-8"), "q8 rescue sealed report")
        raw_manifest = _require_dict(
            _require_dict(report_document, "q8 rescue report").get("raw_evidence"),
            "q8 rescue raw manifest")
        if (raw_manifest.get("record_count") != 5
                or raw_manifest.get("complete") is not True
                or raw_manifest.get("run_id") != report.get("run_id")
                or raw_manifest.get("schedule_sorted_checksum")
                    != report.get("schedule_sorted_checksum")):
            raise ContractError("q8 rescue sealed report completeness changed")
        entry = _execution_entry(
            path=_relative(child_path),
            config_sha256=config_sha,
            parent=parent,
            canonical_cell=canonical_cell,
            execution_cell=execution_cell,
            declared_metric_names=list(parent["declared_metric_names"]),
            expected_disposition=None,
            authority="q8_rescue",
            sealed_evidence=child,
        )
        if child_path in configs_by_path or child_path in result:
            raise ContractError("q8 rescue child path collides with campaign authority")
        result[child_path] = entry
        keys.append(stable_key)
    if (_sha256("\n".join(sorted(keys)).encode("utf-8"))
            != parent["expected_stable_keys_sha256"]):
        raise ContractError("q8 rescue children do not exactly union the parent")
    return result


def _load_q1024_singleton_partition(
        contract: dict[str, Any],
        contract_raw: bytes,
        configs_by_path: dict[Path, dict[str, Any]],
) -> dict[Path, dict[str, Any]]:
    manifest, manifest_raw = _read_json_file(
        Q1024_SINGLETON_PARTITION_PATH, "q1024 singleton partition")
    if _sha256(manifest_raw) != Q1024_SINGLETON_PARTITION_SHA256:
        raise ContractError(
            "q1024 singleton partition bytes differ from the pinned SHA-256")
    _require_exact_keys(manifest, {
        "schema", "version", "campaign_binding", "partition_contract",
        "parents", "children",
    }, "q1024 singleton partition")
    if (manifest.get("schema")
            != "bioseq.e2e.algorithm-matrix-q1024-singleton-partition"
            or manifest.get("version") != 1):
        raise ContractError("q1024 singleton partition schema changed")
    binding = _require_dict(
        manifest.get("campaign_binding"), "q1024 singleton campaign binding")
    ownership = _require_dict(contract.get("shard_ownership"), "q1024 ownership")
    expected_binding = {
        "campaign_id": Q1024_CAMPAIGN_ID,
        "campaign_digest": Q1024_CAMPAIGN_DIGEST,
        "canonical_manifest": _relative(Q1024_CONTRACT_PATH),
        "canonical_manifest_sha256": _sha256(contract_raw),
        "canonical_parent_config_count": 88,
        "canonical_result_cells": 1296,
        "canonical_raw_records": 6480,
        "canonical_stable_keys_sha256": ownership["stable_keys_sha256"],
    }
    if binding != expected_binding:
        raise ContractError("q1024 singleton canonical campaign binding changed")
    partition = _require_dict(
        manifest.get("partition_contract"), "q1024 singleton contract")
    digest = _require_hex(
        partition.get("partition_digest"), "q1024 partition digest", 64)
    digest_payload = copy.deepcopy(manifest)
    del digest_payload["partition_contract"]["partition_digest"]
    if _sha256(_canonical_bytes(digest_payload)) != digest:
        raise ContractError("q1024 singleton partition digest does not recompute")
    expected_partition = {
        "child_config_count": 1296,
        "cells_per_child": 1,
        "records_per_child": 5,
        "repetitions_per_cell": 5,
        "repeat_indices": list(range(5)),
        "normalization_axes": ["test_case_index", "tuple_index"],
        "canonical_ownership_preserved": True,
        "parent_unions_exact": True,
        "global_union_exact": True,
        "stable_keys_sha256": ownership["stable_keys_sha256"],
        "metric_count_distribution": {"15": 512, "21": 16, "29": 768},
        "cell_disposition_distribution": {
            "timed_bioseq": 768,
            "minimap2_baseline": 16,
            "rejected_setup": 480,
            "rejected_sql": 32,
        },
        "prefilter_enabled": False,
        "pipeline_mode": "task_parallel",
        "cold_no_index_persistence": "off",
        "cigar_projection": True,
        "mapq_projection": True,
        "base_alignment_algorithms": ["ksw2", "block_aligner"],
        "excluded_base_alignment_algorithms": ["ksw2_batched"],
        "q1_or_q8_membership_permitted": False,
        "reserved_filename_regex": Q1024_SINGLETON_PATH_PATTERN.pattern,
        "deterministic_first_launch": Q1024_DETERMINISTIC_FIRST_SINGLETON,
        "partition_digest_algorithm": (
            "sha256-canonical-json-without-partition_digest"),
        "partition_digest": digest,
    }
    if partition != expected_partition:
        raise ContractError("q1024 singleton execution contract changed")

    canonical_entries = [
        entry for entry in configs_by_path.values()
        if entry.get("role") == "shard"]
    canonical_by_relative = {entry["path"]: entry for entry in canonical_entries}
    parents = _require_list(manifest.get("parents"), "q1024 singleton parents")
    if len(parents) != 88:
        raise ContractError("q1024 singleton parent count changed")
    parent_children: dict[str, list[int]] = {}
    for ordinal, raw_parent in enumerate(parents):
        parent_proof = _require_dict(
            raw_parent, f"q1024 singleton parent[{ordinal}]")
        _require_exact_keys(parent_proof, {
            "ordinal", "path", "config_sha256", "expected_result_cells",
            "expected_raw_records", "expected_stable_keys_sha256",
            "child_ordinals",
        }, f"q1024 singleton parent[{ordinal}]")
        parent = canonical_by_relative.get(parent_proof.get("path"))
        if (parent_proof.get("ordinal") != ordinal
                or parent is None
                or parent_proof.get("config_sha256") != parent["config_sha256"]
                or parent_proof.get("expected_result_cells")
                    != parent["expected_result_cells"]
                or parent_proof.get("expected_raw_records")
                    != parent["expected_raw_records"]
                or parent_proof.get("expected_stable_keys_sha256")
                    != parent["expected_stable_keys_sha256"]):
            raise ContractError("q1024 singleton parent proof changed")
        ordinals = _require_list(
            parent_proof.get("child_ordinals"), "q1024 parent child ordinals")
        if (len(ordinals) != parent["expected_result_cells"]
                or any(isinstance(value, bool) or not isinstance(value, int)
                       for value in ordinals)):
            raise ContractError("q1024 singleton parent child range changed")
        parent_children[parent["path"]] = ordinals

    children = _require_list(manifest.get("children"), "q1024 singleton children")
    if len(children) != 1296:
        raise ContractError("q1024 singleton child count changed")
    result: dict[Path, dict[str, Any]] = {}
    global_keys: list[str] = []
    parent_keys: dict[str, list[str]] = {path: [] for path in parent_children}
    metric_distribution: dict[int, int] = {}
    disposition_distribution = {
        "timed_bioseq": 0, "minimap2_baseline": 0,
        "rejected_setup": 0, "rejected_sql": 0,
    }
    for ordinal, raw_child in enumerate(children):
        child = _require_dict(raw_child, f"q1024 singleton child[{ordinal}]")
        _require_exact_keys(child, {
            "ordinal", "path", "config_sha256", "parent_path",
            "parent_config_sha256", "parent_expected_stable_keys_sha256",
            "parent_test_case_index", "parent_tuple_index", "stable_key",
            "stable_key_sha256", "expected_stable_keys_sha256",
            "expected_result_cells", "expected_raw_records",
            "repetitions_per_cell", "declared_metric_names",
            "expected_all_records_disposition", "canonical_cell",
            "execution_cell",
        }, f"q1024 singleton child[{ordinal}]")
        if child.get("ordinal") != ordinal:
            raise ContractError("q1024 singleton child ordinal changed")
        child_relative = _require_string(
            child.get("path"), "q1024 singleton child path")
        if Q1024_SINGLETON_PATH_PATTERN.fullmatch(child_relative) is None:
            raise ContractError("q1024 singleton child escaped reserved path syntax")
        parent = canonical_by_relative.get(child.get("parent_path"))
        if (parent is None
                or child.get("parent_config_sha256") != parent["config_sha256"]
                or child.get("parent_expected_stable_keys_sha256")
                    != parent["expected_stable_keys_sha256"]
                or ordinal not in parent_children[parent["path"]]):
            raise ContractError("q1024 singleton child parent binding changed")
        parent_tuple = _require_int(
            child.get("parent_tuple_index"), "q1024 parent tuple")
        parent_case = _require_int(
            child.get("parent_test_case_index"), "q1024 parent case")
        matches = [
            cell for cell in parent["cells"]
            if cell["tuple_index"] == parent_tuple
            and cell["test_case_index"] == parent_case
        ]
        if len(matches) != 1 or child.get("canonical_cell") != matches[0]:
            raise ContractError("q1024 singleton canonical descriptor changed")
        canonical_cell = matches[0]
        execution_cell = copy.deepcopy(canonical_cell)
        execution_cell["test_case_index"] = 0
        execution_cell["tuple_index"] = 0
        if child.get("execution_cell") != execution_cell:
            raise ContractError(
                "q1024 singleton normalized more than the two matrix coordinates")
        stable_key = canonical_cell["stable_key"]
        stable_digest = _sha256(stable_key.encode("utf-8"))
        if (child.get("stable_key") != stable_key
                or child.get("stable_key_sha256") != stable_digest
                or child.get("expected_stable_keys_sha256") != stable_digest
                or child.get("expected_result_cells") != 1
                or child.get("expected_raw_records") != 5
                or child.get("repetitions_per_cell") != 5
                or child_relative != (
                    "bench/e2e_algorithm_matrix_shards/"
                    "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
                    f"{ordinal:04d}_{stable_digest[:16]}.json")):
            raise ContractError("q1024 singleton stable-key/cardinality seal changed")
        disposition = _canonical_cell_rejection_disposition(
            canonical_cell["identity"])
        expected_metrics = (
            ALL_REJECTED_METRIC_NAMES
            if disposition is not None else
            MINIMAP2_BASELINE_METRIC_NAMES
            if canonical_cell["identity"]["engine"] == "minimap2" else
            MIXED_PERSISTED_METRIC_NAMES)
        declared = _require_list(
            child.get("declared_metric_names"), "q1024 singleton metrics")
        if (declared != list(expected_metrics)
                or child.get("expected_all_records_disposition") != disposition):
            raise ContractError("q1024 singleton metrics/disposition changed")
        metric_distribution[len(declared)] = (
            metric_distribution.get(len(declared), 0) + 1)
        bucket = disposition or (
            "minimap2_baseline"
            if canonical_cell["identity"]["engine"] == "minimap2"
            else "timed_bioseq")
        disposition_distribution[bucket] += 1
        child_path = _canonical_repository_file(
            child_relative, "q1024 singleton config")
        child_raw = _read_regular(child_path, "q1024 singleton config")
        config_sha = _require_hex(
            child.get("config_sha256"), "q1024 singleton config SHA", 64)
        if _sha256(child_raw) != config_sha:
            raise ContractError("q1024 singleton config hash changed")
        entry = _execution_entry(
            path=child_relative,
            config_sha256=config_sha,
            parent=parent,
            canonical_cell=canonical_cell,
            execution_cell=execution_cell,
            declared_metric_names=declared,
            expected_disposition=disposition,
            authority="q1024_singleton",
        )
        if child_path in configs_by_path or child_path in result:
            raise ContractError("q1024 singleton path collides with authority")
        result[child_path] = entry
        global_keys.append(stable_key)
        parent_keys[parent["path"]].append(stable_key)
    for path, keys in parent_keys.items():
        if (_sha256("\n".join(sorted(keys)).encode("utf-8"))
                != canonical_by_relative[path]["expected_stable_keys_sha256"]):
            raise ContractError("q1024 singleton parent union is incomplete")
    if (_sha256("\n".join(sorted(global_keys)).encode("utf-8"))
            != ownership["stable_keys_sha256"]
            or len(set(global_keys)) != 1296
            or metric_distribution != {15: 512, 21: 16, 29: 768}
            or disposition_distribution != {
                "timed_bioseq": 768, "minimap2_baseline": 16,
                "rejected_setup": 480, "rejected_sql": 32,
            }):
        raise ContractError("q1024 singleton global union proof changed")
    return result


def _load_observed_rung_singleton_partition(
        contract: dict[str, Any], contract_raw: bytes,
        configs_by_path: dict[Path, dict[str, Any]],
) -> tuple[dict[Path, dict[str, Any]], str, str]:
    """Load one pinned accepted-only singleton authority for an observed rung."""
    query_count = EXPECTED_QUERY_COUNT
    partition_path = _observed_rung_singleton_partition_path(query_count)
    expected_manifest_sha = OBSERVED_RUNG_SINGLETON_PARTITION_SHA256.get(
        query_count)
    if expected_manifest_sha is None:
        raise ContractError("observed singleton rung has no pinned manifest SHA-256")
    manifest, manifest_raw = _read_json_file(
        partition_path, f"q{query_count} observed singleton partition")
    if _sha256(manifest_raw) != expected_manifest_sha:
        raise ContractError(
            "observed singleton partition bytes differ from the pinned SHA-256")
    _require_exact_keys(manifest, {
        "schema", "version", "campaign_binding", "partition_contract",
        "parents", "children",
    }, "observed singleton partition")
    if (manifest.get("schema")
            != "bioseq.e2e.algorithm-matrix-observed-rung-singleton-partition"
            or manifest.get("version") != 1):
        raise ContractError("observed singleton partition schema changed")

    campaign = _require_dict(contract.get("campaign"), "observed campaign")
    source = _require_dict(contract.get("source"), "observed campaign source")
    rung = _require_dict(contract.get("rung"), "observed rung")
    ownership = _require_dict(
        contract.get("shard_ownership"), "observed ownership")
    binding = _require_dict(
        manifest.get("campaign_binding"), "observed singleton binding")
    expected_binding = {
        "campaign_id": campaign["campaign_id"],
        "campaign_digest": campaign["campaign_digest"],
        "execution_contract_id": campaign["execution_contract_id"],
        "result_status_contract": OBSERVED_RESULT_STATUS_CONTRACT,
        "query_count": query_count,
        "canonical_manifest": _relative(CAMPAIGN_CONTRACT_PATH),
        "canonical_manifest_sha256": _sha256(contract_raw),
        "canonical_config": source["full_config"],
        "canonical_config_sha256": source["full_config_sha256"],
        "canonical_parent_config_count": 256,
        "canonical_result_cells": EXPECTED_RESULT_CELLS,
        "canonical_raw_records": EXPECTED_RESULT_CELLS * MIN_ACCEPTED_REPEATS,
        "canonical_stable_keys_sha256": ownership["stable_keys_sha256"],
        "fixture_projection_sha256": rung["fixture_projection_sha256"],
        "predecessor_execution_contract_id": rung[
            "predecessor_execution_contract_id"],
    }
    if binding != expected_binding:
        raise ContractError("observed singleton campaign binding changed")

    pattern = _observed_rung_singleton_path_pattern(query_count)
    partition = _require_dict(
        manifest.get("partition_contract"), "observed singleton contract")
    digest = _require_hex(
        partition.get("partition_digest"),
        "observed singleton partition digest", 64)
    digest_payload = copy.deepcopy(manifest)
    del digest_payload["partition_contract"]["partition_digest"]
    expected_partition = {
        "child_config_count": EXPECTED_RESULT_CELLS,
        "cells_per_child": 1,
        "records_per_child": MIN_ACCEPTED_REPEATS,
        "repetitions_per_cell": MIN_ACCEPTED_REPEATS,
        "repeat_indices": list(range(MIN_ACCEPTED_REPEATS)),
        "normalization_axes": ["test_case_index", "tuple_index"],
        "canonical_ownership_preserved": True,
        "parent_unions_exact": True,
        "global_union_exact": True,
        "stable_keys_sha256": ownership["stable_keys_sha256"],
        "metric_count_distribution": {"21": 16, "29": 1280},
        "cell_disposition_distribution": {
            "timed_bioseq": 1280,
            "minimap2_baseline": 16,
        },
        "accepted_only": True,
        "prefilter_enabled": False,
        "pipeline_mode": "task_parallel",
        "cold_no_index_persistence": "off",
        "cigar_projection": True,
        "mapq_projection": True,
        "paired_minimap2_oracle": False,
        "base_alignment_algorithms": ["ksw2", "block_aligner"],
        "excluded_base_alignment_algorithms": ["ksw2_batched"],
        "cross_rung_membership_permitted": False,
        "reserved_filename_regex": pattern.pattern,
        "deterministic_first_launch": (
            f"bench/e2e_algorithm_matrix_q{query_count}_observed_singletons/"
            f"bench_e2e_algorithm_matrix_q{query_count}_observed_singleton_"
            "0000_0ed6b7419bc8e21f.json"),
        "partition_digest_algorithm": (
            "sha256-canonical-json-without-partition_digest"),
        "partition_digest": digest,
    }
    if (_sha256(_canonical_bytes(digest_payload)) != digest
            or partition != expected_partition):
        raise ContractError("observed singleton execution contract changed")

    canonical_entries = [
        entry for entry in configs_by_path.values()
        if entry.get("role") == "shard"]
    canonical_by_relative = {
        entry["path"]: entry for entry in canonical_entries}
    parents = _require_list(
        manifest.get("parents"), "observed singleton parents")
    if len(parents) != 256 or len(canonical_by_relative) != 256:
        raise ContractError("observed singleton parent count changed")
    parent_children: dict[str, set[int]] = {}
    claimed_ordinals: list[int] = []
    for ordinal, raw_parent in enumerate(parents):
        proof = _require_dict(
            raw_parent, f"observed singleton parent[{ordinal}]")
        _require_exact_keys(proof, {
            "ordinal", "path", "config_sha256", "expected_result_cells",
            "expected_raw_records", "expected_stable_keys_sha256",
            "child_ordinals",
        }, f"observed singleton parent[{ordinal}]")
        parent = canonical_by_relative.get(proof.get("path"))
        child_ordinals = _require_list(
            proof.get("child_ordinals"), "observed parent child ordinals")
        if (proof.get("ordinal") != ordinal
                or parent is None
                or proof.get("config_sha256") != parent["config_sha256"]
                or proof.get("expected_result_cells")
                    != parent["expected_result_cells"]
                or proof.get("expected_raw_records")
                    != parent["expected_raw_records"]
                or proof.get("expected_stable_keys_sha256")
                    != parent["expected_stable_keys_sha256"]
                or parent.get("expected_all_records_disposition") is not None
                or parent.get("allowed_nonblocking_rule_names") != []
                or len(child_ordinals) != parent["expected_result_cells"]
                or any(isinstance(value, bool) or not isinstance(value, int)
                       for value in child_ordinals)):
            raise ContractError("observed singleton parent proof changed")
        parent_children[parent["path"]] = set(child_ordinals)
        claimed_ordinals.extend(child_ordinals)
    if (len(claimed_ordinals) != EXPECTED_RESULT_CELLS
            or sorted(claimed_ordinals) != list(range(EXPECTED_RESULT_CELLS))):
        raise ContractError("observed singleton parent ordinal partition changed")

    exact_child_keys = {
        "ordinal", "path", "config_sha256", "parent_path",
        "parent_config_sha256", "parent_expected_stable_keys_sha256",
        "parent_test_case_index", "parent_tuple_index", "stable_key",
        "stable_key_sha256", "expected_stable_keys_sha256",
        "expected_result_cells", "expected_raw_records",
        "repetitions_per_cell", "declared_metric_names",
        "expected_all_records_disposition", "canonical_cell",
        "execution_cell",
    }
    children = _require_list(
        manifest.get("children"), "observed singleton children")
    if len(children) != EXPECTED_RESULT_CELLS:
        raise ContractError("observed singleton child count changed")
    result: dict[Path, dict[str, Any]] = {}
    global_keys: list[str] = []
    parent_keys: dict[str, list[str]] = {
        path: [] for path in canonical_by_relative}
    metric_distribution: dict[int, int] = {}
    disposition_distribution = {
        "timed_bioseq": 0,
        "minimap2_baseline": 0,
    }
    for ordinal, raw_child in enumerate(children):
        child = _require_dict(
            raw_child, f"observed singleton child[{ordinal}]")
        _require_exact_keys(
            child, exact_child_keys, f"observed singleton child[{ordinal}]")
        child_relative = _require_string(
            child.get("path"), "observed singleton child path")
        parent = canonical_by_relative.get(child.get("parent_path"))
        if (child.get("ordinal") != ordinal
                or pattern.fullmatch(child_relative) is None
                or parent is None
                or ordinal not in parent_children[parent["path"]]
                or child.get("parent_config_sha256")
                    != parent["config_sha256"]
                or child.get("parent_expected_stable_keys_sha256")
                    != parent["expected_stable_keys_sha256"]):
            raise ContractError("observed singleton child parent binding changed")
        parent_tuple = _require_int(
            child.get("parent_tuple_index"), "observed parent tuple")
        parent_case = _require_int(
            child.get("parent_test_case_index"), "observed parent case")
        matches = [
            cell for cell in parent["cells"]
            if cell["tuple_index"] == parent_tuple
            and cell["test_case_index"] == parent_case]
        if len(matches) != 1 or child.get("canonical_cell") != matches[0]:
            raise ContractError("observed singleton canonical descriptor changed")
        canonical_cell = matches[0]
        execution_cell = copy.deepcopy(canonical_cell)
        execution_cell["test_case_index"] = 0
        execution_cell["tuple_index"] = 0
        stable_key = canonical_cell["stable_key"]
        stable_digest = _sha256(stable_key.encode("utf-8"))
        expected_path = (
            f"bench/e2e_algorithm_matrix_q{query_count}_observed_singletons/"
            f"bench_e2e_algorithm_matrix_q{query_count}_observed_singleton_"
            f"{ordinal:04d}_{stable_digest[:16]}.json")
        declared = _require_list(
            child.get("declared_metric_names"), "observed singleton metrics")
        identity = _require_dict(
            canonical_cell.get("identity"), "observed canonical identity")
        expected_metric_count = (
            21 if identity.get("engine") == "minimap2" else 29)
        if (child_relative != expected_path
                or child.get("execution_cell") != execution_cell
                or canonical_cell.get("allowed_statuses") != ["accepted"]
                or canonical_cell.get("stable_key") != stable_key
                or child.get("stable_key") != stable_key
                or child.get("stable_key_sha256") != stable_digest
                or child.get("expected_stable_keys_sha256") != stable_digest
                or child.get("expected_result_cells") != 1
                or child.get("expected_raw_records") != MIN_ACCEPTED_REPEATS
                or child.get("repetitions_per_cell") != MIN_ACCEPTED_REPEATS
                or child.get("expected_all_records_disposition") is not None
                or declared != parent["declared_metric_names"]
                or len(declared) != expected_metric_count):
            raise ContractError(
                "observed singleton ownership/status/metrics proof changed")
        child_path = _canonical_repository_file(
            child_relative, "observed singleton config")
        child_raw = _read_regular(child_path, "observed singleton config")
        config_sha = _require_hex(
            child.get("config_sha256"), "observed singleton config SHA", 64)
        if _sha256(child_raw) != config_sha:
            raise ContractError("observed singleton config hash changed")
        entry = _execution_entry(
            path=child_relative,
            config_sha256=config_sha,
            parent=parent,
            canonical_cell=canonical_cell,
            execution_cell=execution_cell,
            declared_metric_names=declared,
            expected_disposition=None,
            authority="observed_rung_singleton",
            sealed_evidence={
                "partition_manifest": _relative(partition_path),
                "partition_manifest_sha256": expected_manifest_sha,
                "child_ordinal": ordinal,
                "stable_key_sha256": stable_digest,
            },
        )
        if child_path in configs_by_path or child_path in result:
            raise ContractError("observed singleton path collides with authority")
        result[child_path] = entry
        global_keys.append(stable_key)
        parent_keys[parent["path"]].append(stable_key)
        metric_distribution[len(declared)] = (
            metric_distribution.get(len(declared), 0) + 1)
        bucket = (
            "minimap2_baseline"
            if identity.get("engine") == "minimap2" else "timed_bioseq")
        disposition_distribution[bucket] += 1
    for path, keys in parent_keys.items():
        if (_sha256("\n".join(sorted(keys)).encode("utf-8"))
                != canonical_by_relative[path]["expected_stable_keys_sha256"]):
            raise ContractError("observed singleton parent union is incomplete")
    if (_sha256("\n".join(sorted(global_keys)).encode("utf-8"))
            != ownership["stable_keys_sha256"]
            or len(set(global_keys)) != EXPECTED_RESULT_CELLS
            or metric_distribution != {21: 16, 29: 1280}
            or disposition_distribution != {
                "timed_bioseq": 1280,
                "minimap2_baseline": 16,
            }):
        raise ContractError("observed singleton global union proof changed")
    return result, _relative(partition_path), expected_manifest_sha


def _canonical_parent_for_stable_key(
        stable_key: str,
        canonical_parents: list[dict[str, Any]],
) -> dict[str, Any]:
    matches = [
        parent for parent in canonical_parents
        if stable_key in parent["descriptors_by_key"]]
    if len(matches) != 1:
        raise ContractError(
            "canonical stable key does not have one exact parent shard")
    return matches[0]


def _load_q1024_packed_partition(
        contract: dict[str, Any],
        contract_raw: bytes,
        configs_by_path: dict[Path, dict[str, Any]],
        singleton_entries: dict[Path, dict[str, Any]],
) -> tuple[dict[Path, dict[str, Any]], dict[Path, dict[str, Any]]]:
    """Load the packed/adopted owners and the disjoint singleton complement."""
    manifest, manifest_raw = _read_json_file(
        Q1024_PACKED_PARTITION_PATH, "q1024 packed partition")
    if _sha256(manifest_raw) != Q1024_PACKED_PARTITION_SHA256:
        raise ContractError(
            "q1024 packed partition bytes differ from the pinned SHA-256")
    _require_exact_keys(manifest, {
        "schema", "version", "campaign_binding", "execution_contract",
        "packed_members", "adoptions", "retained_singletons",
        "optional_singletons", "execution_plan",
    }, "q1024 packed partition")
    if (manifest.get("schema")
            != "bioseq.e2e.algorithm-matrix-q1024-packed-partition"
            or manifest.get("version") != 1):
        raise ContractError("q1024 packed partition schema changed")

    ownership = _require_dict(contract.get("shard_ownership"), "q1024 ownership")
    deadline_manifest, deadline_raw = _read_json_file(
        Q1024_DEADLINE_V2_MANIFEST_PATH, "deadline-v2 adoption manifest")
    if _sha256(deadline_raw) != Q1024_DEADLINE_V2_MANIFEST_SHA256:
        raise ContractError("deadline-v2 adoption manifest SHA-256 changed")
    deadline_campaign = _require_dict(
        deadline_manifest.get("campaign"), "deadline-v2 campaign")
    expected_binding = {
        "campaign_id": Q1024_CAMPAIGN_ID,
        "campaign_digest": Q1024_CAMPAIGN_DIGEST,
        "canonical_manifest": _relative(Q1024_CONTRACT_PATH),
        "canonical_manifest_sha256": _sha256(contract_raw),
        "singleton_manifest": _relative(Q1024_SINGLETON_PARTITION_PATH),
        "singleton_manifest_sha256": Q1024_SINGLETON_PARTITION_SHA256,
        "deadline_v2_manifest": _relative(Q1024_DEADLINE_V2_MANIFEST_PATH),
        "deadline_v2_manifest_sha256": Q1024_DEADLINE_V2_MANIFEST_SHA256,
        "deadline_v2_campaign_id": "bioseq-e2e-algorithm-matrix-deadline-v2",
        "deadline_v2_campaign_digest": (
            "44a85360a5e975ec8a6d70ac0b06486c51d748770c2ada46106d2ccc5db4728a"),
    }
    if (_require_dict(
            manifest.get("campaign_binding"), "q1024 packed binding")
            != expected_binding
            or deadline_campaign.get("campaign_id")
                != expected_binding["deadline_v2_campaign_id"]
            or deadline_campaign.get("campaign_digest")
                != expected_binding["deadline_v2_campaign_digest"]):
        raise ContractError("q1024 packed campaign/adoption binding changed")

    execution_contract = _require_dict(
        manifest.get("execution_contract"), "q1024 packed execution contract")
    digest = _require_hex(
        execution_contract.get("partition_digest"),
        "q1024 packed partition digest", 64)
    digest_payload = copy.deepcopy(manifest)
    del digest_payload["execution_contract"]["partition_digest"]
    if _sha256(_canonical_bytes(digest_payload)) != digest:
        raise ContractError("q1024 packed partition digest does not recompute")

    canonical_cells = {
        stable_key: cell
        for stable_key, cell in next(
            entry for entry in configs_by_path.values()
            if entry.get("role") == "monolithic")["descriptors_by_key"].items()
    }
    canonical_parents = [
        entry for entry in configs_by_path.values()
        if entry.get("role") == "shard"]
    canonical_parent_by_relative = {
        parent["path"]: parent for parent in canonical_parents}
    singleton_by_key: dict[str, tuple[Path, dict[str, Any]]] = {}
    singleton_by_relative: dict[str, tuple[Path, dict[str, Any]]] = {}
    for path, entry in singleton_entries.items():
        if len(entry["descriptors_by_key"]) != 1:
            raise ContractError("q1024 singleton authority is no longer singular")
        stable_key = next(iter(entry["descriptors_by_key"]))
        singleton_by_key[stable_key] = (path, entry)
        singleton_by_relative[entry["path"]] = (path, entry)
    if len(singleton_by_key) != EXPECTED_RESULT_CELLS:
        raise ContractError("q1024 singleton lookup union changed")

    member_keys = {
        "ordinal", "id", "kind", "path", "config_sha256",
        "source_parent_path", "source_parent_config_sha256",
        "expected_result_cells", "expected_raw_records",
        "repetitions_per_cell", "stable_keys",
        "expected_stable_keys_sha256", "allowed_nonblocking_rule_names",
        "declared_metric_names", "expected_all_records_disposition",
        "planned_supported_cells", "planned_selector_rejection_cells",
        "canonical_cells", "execution_cells",
    }
    packed_entries: dict[Path, dict[str, Any]] = {}
    packed_stable_keys: list[str] = []
    packed_ids: set[str] = set()
    existing_members = 0
    generated_members = 0
    planned_supported = 0
    planned_selector_rejections = 0
    members = _require_list(manifest.get("packed_members"), "q1024 packed members")
    for ordinal, raw_member in enumerate(members):
        member = _require_dict(raw_member, f"q1024 packed member[{ordinal}]")
        _require_exact_keys(member, member_keys, f"q1024 packed member[{ordinal}]")
        member_id = _require_string(member.get("id"), "q1024 packed member id")
        if member.get("ordinal") != ordinal or member_id in packed_ids:
            raise ContractError("q1024 packed member ordinal/id changed")
        packed_ids.add(member_id)
        kind = _require_string(member.get("kind"), "q1024 packed member kind")
        if kind not in {
                "canonical_parent", "generated_persisted_residual",
                "generated_minimap2_pair"}:
            raise ContractError("q1024 packed member kind changed")
        relative = _require_string(member.get("path"), "q1024 packed path")
        parent_relative = _require_string(
            member.get("source_parent_path"), "q1024 packed parent path")
        parent = canonical_parent_by_relative.get(parent_relative)
        if (parent is None
                or member.get("source_parent_config_sha256")
                    != parent["config_sha256"]):
            raise ContractError("q1024 packed member parent binding changed")
        if kind == "canonical_parent":
            if relative != parent_relative:
                raise ContractError("q1024 packed canonical parent path changed")
            existing_members += 1
        else:
            if Q1024_PACKED_PATH_PATTERN.fullmatch(relative) is None:
                raise ContractError("q1024 generated packed path escaped authority")
            generated_members += 1
        path = _canonical_repository_file(relative, "q1024 packed config")
        config, config_raw = _read_json_file(path, "q1024 packed config")
        config_sha = _require_hex(
            member.get("config_sha256"), "q1024 packed config SHA", 64)
        if (_sha256(config_raw) != config_sha
                or (kind == "canonical_parent"
                    and config_sha != parent["config_sha256"])):
            raise ContractError("q1024 packed config bytes changed")
        execution_cells = _execution_descriptors_from_config(
            config, Q1024_CAMPAIGN_ID)
        if member.get("execution_cells") != execution_cells:
            raise ContractError("q1024 packed execution descriptors changed")
        stable_keys = _require_list(
            member.get("stable_keys"), "q1024 packed stable keys")
        if (stable_keys != [cell["stable_key"] for cell in execution_cells]
                or len(stable_keys) != len(set(stable_keys))):
            raise ContractError("q1024 packed stable-key order/membership changed")
        try:
            expected_canonical_cells = [
                canonical_cells[stable_key] for stable_key in stable_keys]
        except KeyError as exc:
            raise ContractError(
                "q1024 packed config owns a noncanonical stable key") from exc
        if member.get("canonical_cells") != expected_canonical_cells:
            raise ContractError("q1024 packed canonical descriptors changed")
        expected_count = len(stable_keys)
        expected_digest = _sha256(
            "\n".join(sorted(stable_keys)).encode("utf-8"))
        metrics = list(_require_dict(
            _require_dict(
                _require_dict(config.get("bench_e2e"), "packed bench_e2e").get(
                    "matrix"), "packed matrix").get("metrics"),
            "packed metrics"))
        rules = _config_nonblocking_rule_names(config)
        expected_disposition = _expected_all_records_disposition(
            expected_canonical_cells)
        supported = sum(
            cell["identity"]["engine"] == "minimap2"
            or (cell["identity"]["requested_minimizer_algorithm"]
                    != "vulkan_minimap_integer"
                and cell["identity"]["requested_chaining_algorithm"]
                    != "vulkan_tiled_dynamic_programming")
            for cell in expected_canonical_cells)
        selector_rejections = expected_count - supported
        if kind == "generated_persisted_residual" and supported > 0:
            if metrics != list(MIXED_PERSISTED_METRIC_NAMES):
                raise ContractError(
                    "supported generated persisted residual lacks exact mixed metrics")
            expected_disposition = None
        if (member.get("expected_result_cells") != expected_count
                or member.get("expected_raw_records") != expected_count * 5
                or member.get("repetitions_per_cell") != 5
                or member.get("expected_stable_keys_sha256") != expected_digest
                or member.get("allowed_nonblocking_rule_names") != rules
                or member.get("declared_metric_names") != metrics
                or member.get("expected_all_records_disposition")
                    != expected_disposition
                or member.get("planned_supported_cells") != supported
                or member.get("planned_selector_rejection_cells")
                    != selector_rejections):
            raise ContractError("q1024 packed member cardinality/metric proof changed")
        entry = _multi_execution_entry(
            path=relative,
            config_sha256=config_sha,
            parent=parent,
            canonical_cells=copy.deepcopy(expected_canonical_cells),
            execution_cells=copy.deepcopy(execution_cells),
            declared_metric_names=metrics,
            allowed_nonblocking_rule_names=rules,
            expected_disposition=expected_disposition,
            authority="q1024_packed",
            sealed_evidence=copy.deepcopy(member),
        )
        if path in packed_entries:
            raise ContractError("q1024 packed path is duplicated")
        packed_entries[path] = entry
        packed_stable_keys.extend(stable_keys)
        planned_supported += supported
        planned_selector_rejections += selector_rejections
    if (len(members) != 25 or existing_members != 15 or generated_members != 10
            or len(packed_stable_keys) != 150
            or len(set(packed_stable_keys)) != 150
            or planned_supported != 115
            or planned_selector_rejections != 35):
        raise ContractError("q1024 packed member coverage changed")

    deadline_schedule = _require_list(
        deadline_manifest.get("schedule"), "deadline-v2 schedule")
    adoption_keys = {
        "ordinal", "source_ordinal", "source_path", "source_config_sha256",
        "source_stable_key", "source_stable_key_sha256",
        "canonical_stable_key", "canonical_stable_key_sha256",
        "canonical_singleton_path", "canonical_singleton_config_sha256",
        "expected_result_cells", "expected_raw_records", "repetitions_per_cell",
        "declared_metric_names", "expected_all_records_disposition",
        "canonical_cell", "execution_cell", "run_receipt_config",
        "run_receipt_id", "sealed_artifacts", "ingestion_policy",
    }
    adoption_entries: dict[Path, dict[str, Any]] = {}
    adopted_stable_keys: list[str] = []
    adoptions = _require_list(manifest.get("adoptions"), "q1024 adoptions")
    for ordinal, raw_adoption in enumerate(adoptions):
        adoption = _require_dict(raw_adoption, f"q1024 adoption[{ordinal}]")
        _require_exact_keys(adoption, adoption_keys, f"q1024 adoption[{ordinal}]")
        source_ordinal = _require_int(
            adoption.get("source_ordinal"), "q1024 adoption source ordinal")
        if (adoption.get("ordinal") != ordinal
                or source_ordinal != 56 + ordinal
                or source_ordinal >= len(deadline_schedule)):
            raise ContractError("q1024 adoption ordinal changed")
        source = _require_dict(
            deadline_schedule[source_ordinal], "deadline-v2 adoption source")
        source_path = _require_string(
            adoption.get("source_path"), "q1024 adoption source path")
        source_identity = _require_dict(
            source.get("identity"), "deadline-v2 adoption identity")
        stable_key = _require_string(
            adoption.get("canonical_stable_key"),
            "q1024 adoption canonical stable key")
        canonical_cell = canonical_cells.get(stable_key)
        singleton_pair = singleton_by_key.get(stable_key)
        if canonical_cell is None or singleton_pair is None:
            raise ContractError("q1024 adoption has no canonical singleton")
        singleton_path, singleton_entry = singleton_pair
        normalized = copy.deepcopy(canonical_cell)
        normalized["test_case"] = (
            f"{source_identity.get('dataset')}_Q1024_DEADLINE")
        normalized["test_case_index"] = 0
        normalized["tuple_index"] = 0
        identity = canonical_cell["identity"]
        source_matches = (
            source_identity.get("campaign_id")
                == expected_binding["deadline_v2_campaign_id"]
            and source_identity.get("tier") == "q1024_anchor"
            and source_identity.get("query_count") == EXPECTED_QUERY_COUNT
            and source_identity.get("dataset") == canonical_cell["test_case"]
            and all(source_identity.get(field) == identity.get(field)
                    for field in (
                        "index_mode", "engine", "bioseq_index_name",
                        "requested_minimizer_algorithm",
                        "requested_chaining_algorithm",
                        "requested_base_alignment_algorithm")))
        expected_metrics = (
            list(MINIMAP2_BASELINE_METRIC_NAMES)
            if identity["engine"] == "minimap2"
            else list(MIXED_PERSISTED_METRIC_NAMES))
        stable_digest = _sha256(stable_key.encode("utf-8"))
        receipt_config = _require_dict(
            adoption.get("run_receipt_config"),
            "q1024 adoption run-receipt config")
        _require_exact_keys(
            receipt_config, {"path", "sha256"},
            "q1024 adoption run-receipt config")
        receipt_config_path = _require_string(
            receipt_config.get("path"),
            "q1024 adoption run-receipt config path")
        expected_receipt_config_path = (
            Q1024_DEADLINE_V1_RECEIPT_CONFIG_ALIASES.get(
                source_ordinal, source_path))
        if source_ordinal in Q1024_DEADLINE_V1_RECEIPT_CONFIG_ALIASES:
            alias_path = _canonical_repository_file(
                receipt_config_path, "q1024 adoption run-receipt config")
            alias_raw = _read_regular(
                alias_path, "q1024 adoption run-receipt config")
            if _sha256(alias_raw) != source.get("config_sha256"):
                raise ContractError(
                    "deadline-v1 receipt alias is not byte-identical to "
                    "the adopted deadline-v2 member")
        if (not source_matches
                or adoption.get("source_path") != source.get("path")
                or adoption.get("source_config_sha256")
                    != source.get("config_sha256")
                or adoption.get("source_stable_key") != source.get("stable_key")
                or adoption.get("source_stable_key_sha256")
                    != source.get("stable_key_sha256")
                or adoption.get("canonical_stable_key_sha256") != stable_digest
                or adoption.get("canonical_singleton_path")
                    != singleton_entry["path"]
                or adoption.get("canonical_singleton_config_sha256")
                    != singleton_entry["config_sha256"]
                or adoption.get("expected_result_cells") != 1
                or adoption.get("expected_raw_records") != 5
                or adoption.get("repetitions_per_cell") != 5
                or adoption.get("declared_metric_names") != expected_metrics
                or source.get("metric_names") != expected_metrics
                or adoption.get("expected_all_records_disposition") is not None
                or adoption.get("canonical_cell") != canonical_cell
                or adoption.get("execution_cell") != normalized):
            raise ContractError("q1024 deadline-v2 adoption mapping changed")
        if (receipt_config_path != expected_receipt_config_path
                or receipt_config.get("sha256")
                    != source.get("config_sha256")):
            raise ContractError(
                "q1024 deadline-v2 adoption run-receipt alias changed")
        artifacts = adoption.get("sealed_artifacts")
        if source_ordinal < 59:
            if adoption.get("run_receipt_id") \
                    != Q1024_SEALED_ADOPTION_RUN_RECEIPTS[source_ordinal]:
                raise ContractError(
                    "completed q1024 adoption run receipt changed")
            artifact_value = _require_dict(
                artifacts, "q1024 sealed adoption artifacts")
            _require_exact_keys(
                artifact_value, {"report", "raw_sidecar"},
                "q1024 sealed adoption artifacts")
            for label in ("report", "raw_sidecar"):
                evidence = _require_dict(
                    artifact_value.get(label), f"q1024 adoption {label}")
                _require_exact_keys(
                    evidence, {"path", "sha256"}, f"q1024 adoption {label}")
                _canonical_repository_relative(
                    _require_string(evidence.get("path"), f"adoption {label} path"),
                    f"adoption {label} path")
                _require_hex(
                    evidence.get("sha256"), f"adoption {label} SHA-256", 64)
            if adoption.get("ingestion_policy") \
                    != "exact_sealed_artifacts_plus_run_receipt":
                raise ContractError("completed q1024 adoption policy changed")
        elif (artifacts is not None
                or adoption.get("run_receipt_id") is not None
                or adoption.get("ingestion_policy")
                    != "terminal_validated_artifacts_plus_run_receipt"):
            raise ContractError("s059 adoption must await terminal artifacts")
        parent = _canonical_parent_for_stable_key(
            stable_key, canonical_parents)
        path = REPOSITORY_ROOT / _canonical_repository_relative(
            source_path, "q1024 adoption config")
        entry = _multi_execution_entry(
            path=source_path,
            config_sha256=_require_hex(
                adoption.get("source_config_sha256"),
                "q1024 adoption config SHA", 64),
            parent=parent,
            canonical_cells=[copy.deepcopy(canonical_cell)],
            execution_cells=[copy.deepcopy(normalized)],
            declared_metric_names=expected_metrics,
            allowed_nonblocking_rule_names=[],
            expected_disposition=None,
            authority="q1024_deadline_v2_adoption",
            sealed_evidence=copy.deepcopy(adoption),
        )
        if path in adoption_entries or path in configs_by_path:
            raise ContractError("q1024 adoption path collides with campaign authority")
        adoption_entries[path] = entry
        adopted_stable_keys.append(stable_key)
    if (len(adoptions) != 4 or len(set(adopted_stable_keys)) != 4
            or set(adopted_stable_keys) & set(packed_stable_keys)):
        raise ContractError("q1024 adoption union overlaps packed ownership")

    replacement_keys = set(packed_stable_keys) | set(adopted_stable_keys)
    fallback_keys = set(canonical_cells) - replacement_keys
    fallback_entries = {
        path: entry for stable_key, (path, entry) in singleton_by_key.items()
        if stable_key in fallback_keys
    }
    if (len(replacement_keys) != 154 or len(fallback_keys) != 1142
            or len(fallback_entries) != 1142
            or replacement_keys & fallback_keys
            or replacement_keys | fallback_keys != set(canonical_cells)):
        raise ContractError("q1024 hybrid singleton complement changed")

    singleton_summary_keys = {
        "role", "ordinal", "path", "config_sha256", "stable_key",
        "stable_key_sha256"}
    retained_keys: list[str] = []
    for label, expected_count in (
            ("retained_singletons", 3), ("optional_singletons", 2)):
        summaries = _require_list(manifest.get(label), f"q1024 {label}")
        if len(summaries) != expected_count:
            raise ContractError(f"q1024 {label} count changed")
        for raw_summary in summaries:
            summary = _require_dict(raw_summary, f"q1024 {label} member")
            _require_exact_keys(
                summary, singleton_summary_keys, f"q1024 {label} member")
            pair = singleton_by_relative.get(summary.get("path"))
            if pair is None:
                raise ContractError(f"q1024 {label} member is not a singleton")
            _, entry = pair
            stable_key = next(iter(entry["descriptors_by_key"]))
            ordinal_match = re.search(
                r"_singleton_([0-9]{4})_[0-9a-f]{16}\.json$",
                entry["path"])
            if (ordinal_match is None
                    or summary.get("ordinal") != int(ordinal_match.group(1))
                    or summary.get("config_sha256") != entry["config_sha256"]
                    or summary.get("stable_key") != stable_key
                    or summary.get("stable_key_sha256")
                        != _sha256(stable_key.encode("utf-8"))
                    or stable_key not in fallback_keys):
                raise ContractError(f"q1024 {label} binding changed")
            if label == "retained_singletons":
                retained_keys.append(stable_key)
    if len(set(retained_keys)) != 3:
        raise ContractError("q1024 retained singleton schedule overlaps")

    packed_digest = _sha256(
        "\n".join(sorted(packed_stable_keys)).encode("utf-8"))
    adopted_digest = _sha256(
        "\n".join(sorted(adopted_stable_keys)).encode("utf-8"))
    replacement_digest = _sha256(
        "\n".join(sorted(replacement_keys)).encode("utf-8"))
    fallback_digest = _sha256(
        "\n".join(sorted(fallback_keys)).encode("utf-8"))
    scheduled_keys = replacement_keys | set(retained_keys)
    expected_execution_contract = {
        "authority_mode": "hybrid_exact_disjoint",
        "packed_config_count": 25,
        "existing_canonical_parent_config_count": 15,
        "generated_packed_config_count": 10,
        "packed_result_cells": 150,
        "packed_raw_records": 750,
        "adopted_config_count": 4,
        "adopted_result_cells": 4,
        "adopted_raw_records": 20,
        "superseded_singleton_cells": 154,
        "singleton_fallback_cells": 1142,
        "hybrid_execution_config_count": 1171,
        "canonical_result_cells": 1296,
        "canonical_raw_records": 6480,
        "repetitions_per_cell": 5,
        "repeat_indices": list(range(5)),
        "retained_scheduled_singletons": 3,
        "scheduled_result_cells": 157,
        "scheduled_supported_cells": 122,
        "scheduled_selector_rejection_cells": 35,
        "exact_nonoverlapping": True,
        "canonical_union_exact": True,
        "duplicate_stable_key_repeat_permitted": False,
        "canonical_stable_keys_sha256": ownership["stable_keys_sha256"],
        "packed_stable_keys_sha256": packed_digest,
        "adopted_stable_keys_sha256": adopted_digest,
        "superseded_stable_keys_sha256": replacement_digest,
        "singleton_fallback_stable_keys_sha256": fallback_digest,
        "scheduled_stable_keys_sha256": _sha256(
            "\n".join(sorted(scheduled_keys)).encode("utf-8")),
        "prefilter_enabled": False,
        "pipeline_mode": "task_parallel",
        "cold_no_index_persistence": "off",
        "cigar_projection": True,
        "mapq_projection": True,
        "base_alignment_algorithms": ["ksw2", "block_aligner"],
        "excluded_base_alignment_algorithms": ["ksw2_batched"],
        "q1_or_q8_membership_permitted": False,
        "generated_path_prefix": (
            "bench/e2e_algorithm_matrix_q1024_packed_shards/"),
        "generated_filename_prefix": (
            "bench_e2e_algorithm_matrix_q1024_packed_"),
        "deterministic_first_ingestion": Q1024_DETERMINISTIC_FIRST_INGESTION,
        "partition_digest_algorithm": (
            "sha256-canonical-json-without-partition_digest"),
        "partition_digest": digest,
    }
    if execution_contract != expected_execution_contract:
        raise ContractError("q1024 packed execution contract changed")

    plan = _require_dict(manifest.get("execution_plan"), "q1024 execution plan")
    _require_exact_keys(plan, {
        "status", "adoption_ingestion_order", "run_order", "six_day",
        "seven_day", "stretch", "runtime_risks", "disk_preflight",
        "remaining_scope_after_six_day", "remaining_scope_after_seven_day",
    }, "q1024 execution plan")
    if (plan.get("status") != "planned_not_executed"
            or plan.get("adoption_ingestion_order") != [56, 57, 58, 59]
            or not isinstance(plan.get("run_order"), list)
            or _require_dict(plan.get("six_day"), "six-day plan").get(
                "supported_cells_including_adoptions") != 89
            or _require_dict(plan.get("seven_day"), "seven-day plan").get(
                "supported_cells_including_adoptions") != 90
            or _require_dict(plan.get("disk_preflight"), "disk preflight").get(
                "required_free_gib_for_target_ge_4gbp_mps") != 50):
        raise ContractError("q1024 packed execution plan changed")

    result = dict(packed_entries)
    for path, entry in adoption_entries.items():
        if path in result:
            raise ContractError("q1024 packed/adoption paths collide")
        result[path] = entry
    return result, fallback_entries


def _rung_fixture_projection_sha256(
        config: dict[str, Any], query_count: int) -> str:
    """Recompute the compact fixed-seed fixture-prefix identity."""
    test_cases = _require_dict(config.get("test_cases"), "rung test cases")
    datasets: list[dict[str, Any]] = []
    for dataset in (
            "M2_D4_K15_W10", "M2_D4_K20_W15",
            "M2_D5_K15_W10", "M2_D5_K20_W15",
            "M4_D4_K15_W10", "M4_D4_K20_W15",
            "M4_D5_K15_W10", "M4_D5_K20_W15"):
        test_case = _require_dict(
            test_cases.get(dataset), f"rung test case {dataset}")
        data = copy.deepcopy(_require_dict(
            test_case.get("data"), f"rung test case {dataset} data"))
        if data.pop("fragment_count", None) != query_count:
            raise ContractError(
                f"rung test case {dataset} has the wrong query count")
        datasets.append({"test_case": dataset, "data": data})
    if len(test_cases) != len(datasets):
        raise ContractError("observed rung dataset membership changed")
    return _sha256(_canonical_bytes({
        "query_ordinal_interval": {
            "begin": 0,
            "end_exclusive": query_count,
        },
        "datasets": datasets,
    }))


def _expected_observed_rung_config(
        canonical: dict[str, Any], query_count: int) -> dict[str, Any]:
    """Derive the only allowed q-rung projection from canonical q1024."""
    projected = copy.deepcopy(canonical)
    test_cases = _require_dict(
        projected.get("test_cases"), "canonical q1024 test cases")
    for dataset, raw_test_case in test_cases.items():
        test_case = _require_dict(raw_test_case, f"canonical test case {dataset}")
        data = _require_dict(
            test_case.get("data"), f"canonical test case {dataset} data")
        if data.get("fragment_count") != 1024:
            raise ContractError("canonical campaign query count changed")
        data["fragment_count"] = query_count
        test_case["description"] = (
            f"{data['reference_length'] // 1_000_000_000} Gbp "
            f"{data['alphabet']}, k{data['kmer_size']}/w{data['window_size']}, "
            f"{query_count} deterministic fixed-seed-prefix queries")
    benchmark = _require_dict(
        projected.get("bench_e2e"), "canonical q1024 benchmark")
    benchmark["tags"] = [
        "e2e", "algorithm-matrix", f"q{query_count}-campaign"]
    benchmark["description"] = (
        f"Five-repeat q{query_count} fixed-seed prefix of the sealed "
        "2/4 Gbp dna4/dna5 matrix with CIGAR+MAPQ, both k/w values, both "
        "persisted index policies, cold persisted/transient modes, and "
        "minimap2 parity baselines.")
    return projected


def _validate_canonical_q1024_source_binding(
        manifest: dict[str, Any], config_raw: bytes) -> None:
    """Revalidate the immutable q1024 manifest-to-config authority."""
    if (manifest.get("schema") != CAMPAIGN_CONTRACT_SCHEMA
            or manifest.get("version") != CAMPAIGN_CONTRACT_VERSION):
        raise ContractError("canonical q1024 manifest schema/version changed")
    campaign = _require_dict(
        manifest.get("campaign"), "canonical q1024 campaign")
    if (campaign.get("campaign_id") != Q1024_CAMPAIGN_ID
            or campaign.get("campaign_digest") != Q1024_CAMPAIGN_DIGEST
            or campaign.get("campaign_digest_algorithm")
                != "sha256-canonical-json-without-campaign_digest"):
        raise ContractError("canonical q1024 campaign identity changed")
    digest_payload = copy.deepcopy(manifest)
    del digest_payload["campaign"]["campaign_digest"]
    if _sha256(_canonical_bytes(digest_payload)) != Q1024_CAMPAIGN_DIGEST:
        raise ContractError("canonical q1024 campaign digest does not recompute")
    source = _require_dict(
        manifest.get("source"), "canonical q1024 source binding")
    if (source.get("full_config") != _relative(Q1024_CONFIG_PATH)
            or source.get("full_config_sha256") != _sha256(config_raw)):
        raise ContractError(
            "canonical q1024 manifest no longer binds its source config")


def _validate_observed_rung_contract(
        value: Any, projected_config: dict[str, Any]) -> str | None:
    """Validate the additive v2 rung and its exact canonical projection."""
    rung = _require_dict(value, "observed rung contract")
    _require_exact_keys(rung, {
        "schema", "version", "query_count", "ladder_query_counts",
        "predecessor_query_count", "predecessor_execution_contract_id",
        "query_ordinal_interval", "projection",
        "fixture_projection_sha256", "matrix_contract",
        "canonical_campaign_binding",
    }, "observed rung contract")
    if (rung.get("schema")
            != "bioseq.e2e.algorithm-matrix-observed-rung"
            or rung.get("version") != 1
            or rung.get("query_count") != EXPECTED_QUERY_COUNT
            or rung.get("ladder_query_counts") != list(RUNG_QUERY_COUNTS)
            or rung.get("projection")
                != "exact_fixed_seed_ordered_prefix_of_q1024"):
        raise ContractError("observed rung identity/projection changed")
    rung_index = RUNG_QUERY_COUNTS.index(EXPECTED_QUERY_COUNT)
    expected_predecessor = (
        RUNG_QUERY_COUNTS[rung_index - 1] if rung_index != 0 else None)
    expected_predecessor_execution_id = (
        _observed_rung_execution_contract_id(expected_predecessor)
        if expected_predecessor is not None else None)
    if (rung.get("predecessor_query_count") != expected_predecessor
            or rung.get("predecessor_execution_contract_id")
                != expected_predecessor_execution_id
            or rung.get("query_ordinal_interval") != {
                "begin": 0, "end_exclusive": EXPECTED_QUERY_COUNT}):
        raise ContractError("observed rung order or query interval changed")
    if _require_dict(rung.get("matrix_contract"), "rung matrix contract") != {
        "all_axes_preserved": True,
        "repetitions_per_cell": MIN_ACCEPTED_REPEATS,
        "prefilter_enabled": False,
        "base_alignment_algorithms": ["ksw2", "block_aligner"],
        "excluded_base_alignment_algorithms": ["ksw2_batched"],
    }:
        raise ContractError("observed rung matrix contract changed")

    binding = _require_dict(
        rung.get("canonical_campaign_binding"), "canonical q1024 binding")
    _require_exact_keys(binding, {
        "campaign_id", "campaign_digest", "manifest", "manifest_sha256",
        "config", "config_sha256",
    }, "canonical q1024 binding")
    canonical_manifest_path = _canonical_repository_file(
        _require_string(binding.get("manifest"), "canonical manifest path"),
        "canonical q1024 manifest")
    canonical_config_path = _canonical_repository_file(
        _require_string(binding.get("config"), "canonical config path"),
        "canonical q1024 config")
    if (canonical_manifest_path != Q1024_CONTRACT_PATH
            or canonical_config_path != Q1024_CONFIG_PATH):
        raise ContractError("observed rung does not bind canonical q1024 paths")
    canonical_manifest, canonical_manifest_raw = _read_json_file(
        canonical_manifest_path, "canonical q1024 manifest")
    canonical_config, canonical_config_raw = _read_json_file(
        canonical_config_path, "canonical q1024 config")
    _validate_canonical_q1024_source_binding(
        canonical_manifest, canonical_config_raw)
    canonical_campaign = _require_dict(
        canonical_manifest.get("campaign"), "canonical q1024 campaign")
    if (binding.get("campaign_id") != Q1024_CAMPAIGN_ID
            or binding.get("campaign_digest") != Q1024_CAMPAIGN_DIGEST
            or canonical_campaign.get("campaign_id") != Q1024_CAMPAIGN_ID
            or canonical_campaign.get("campaign_digest") != Q1024_CAMPAIGN_DIGEST
            or _require_hex(
                binding.get("manifest_sha256"), "canonical manifest SHA", 64)
                != _sha256(canonical_manifest_raw)
            or _require_hex(
                binding.get("config_sha256"), "canonical config SHA", 64)
                != _sha256(canonical_config_raw)):
        raise ContractError("observed rung canonical q1024 binding changed")
    if projected_config != _expected_observed_rung_config(
            canonical_config, EXPECTED_QUERY_COUNT):
        raise ContractError(
            "observed rung differs beyond query count and rung description")
    if _require_hex(
            rung.get("fixture_projection_sha256"),
            "rung fixture projection SHA", 64) != _rung_fixture_projection_sha256(
                projected_config, EXPECTED_QUERY_COUNT):
        raise ContractError("observed rung fixture projection does not recompute")
    return expected_predecessor_execution_id


def _load_campaign_contract() -> dict[str, Any]:
    contract, raw = _read_json_file(CAMPAIGN_CONTRACT_PATH, "campaign contract")
    observed_rung = ACTIVE_CAMPAIGN_PROFILE == "observed_rung"
    observed_rung_contract = (
        _require_dict(contract.get("rung"), "observed rung contract")
        if observed_rung else None)
    expected_top_level = {
        "schema", "version", "campaign", "source", "configs",
        "shard_ownership",
    }
    if observed_rung:
        expected_top_level.add("rung")
    _require_exact_keys(
        contract,
        expected_top_level,
        "campaign contract",
    )
    if (contract.get("schema") != CAMPAIGN_CONTRACT_SCHEMA
            or contract.get("version") != (
                OBSERVED_RUNG_CAMPAIGN_CONTRACT_VERSION
                if observed_rung else CAMPAIGN_CONTRACT_VERSION)):
        raise ContractError("campaign contract schema/version is not canonical")
    campaign = _require_dict(contract.get("campaign"), "campaign contract identity")
    expected_campaign_fields = {
        "campaign_id",
        "campaign_digest_algorithm",
        "stable_key_schema",
        "stable_key_fields",
        "repetitions_per_cell",
        "expected_result_cells",
        "expected_raw_records",
        "required_shard_configs",
        "publication_status_contract",
        "campaign_digest",
    }
    if observed_rung:
        expected_campaign_fields.update({
            "query_count", "result_status_contract", "execution_contract_id"})
    _require_exact_keys(
        campaign, expected_campaign_fields, "campaign contract identity")
    campaign_id = _require_string(campaign.get("campaign_id"), "campaign_id")
    campaign_digest = _require_hex(
        campaign.get("campaign_digest"), "campaign_digest", 64)
    if (campaign_id != EXPECTED_CAMPAIGN_ID
            or campaign_digest != EXPECTED_CAMPAIGN_DIGEST):
        raise ContractError(
            "campaign identity/digest is not the audited canonical campaign")
    if campaign.get("campaign_digest_algorithm") \
            != "sha256-canonical-json-without-campaign_digest":
        raise ContractError("campaign digest algorithm changed")
    result_status_contract = (
        _require_string(
            campaign.get("result_status_contract"), "result-status contract")
        if observed_rung else LEGACY_RESULT_STATUS_CONTRACT)
    if (result_status_contract != (
            OBSERVED_RESULT_STATUS_CONTRACT
            if observed_rung else LEGACY_RESULT_STATUS_CONTRACT)):
        raise ContractError("campaign result-status contract changed")
    if (observed_rung
            and campaign.get("query_count") != EXPECTED_QUERY_COUNT):
        raise ContractError("observed campaign query count changed")
    execution_contract_id = (
        _require_string(
            campaign.get("execution_contract_id"), "execution-contract identity")
        if observed_rung else None)
    if (observed_rung
            and execution_contract_id
                != _observed_rung_execution_contract_id(EXPECTED_QUERY_COUNT)):
        raise ContractError("observed execution-contract identity changed")
    required_shard_configs = _require_int(
        campaign.get("required_shard_configs"), "required shard configs")
    if observed_rung:
        if required_shard_configs == 88:
            expected_shard_kinds = {
                "no_index_baselines", "persisted_index"}
            expected_shard_topology = {
                "no_index_baselines": (8, 82),
                "persisted_index": (80, 8),
            }
            expected_metric_distribution = {29: 80, 32: 9}
        elif required_shard_configs == 256:
            expected_shard_kinds = {
                "no_index_micro", "minimap2_baseline", "persisted_index"}
            expected_shard_topology = {
                "no_index_micro": (160, 4),
                "minimap2_baseline": (16, 1),
                "persisted_index": (80, 8),
            }
            expected_metric_distribution = {21: 16, 29: 240, 32: 1}
        else:
            raise ContractError(
                "observed rung must use the existing 88- or 256-shard topology")
    else:
        expected_shard_kinds = EXPECTED_SHARD_KINDS
        expected_shard_topology = EXPECTED_SHARD_TOPOLOGY
        expected_metric_distribution = EXPECTED_METRIC_DISTRIBUTION
    if (campaign.get("stable_key_schema") != "bioseq-e2e-cell-v2"
            or campaign.get("stable_key_fields") != list(STABLE_KEY_FIELDS)
            or campaign.get("repetitions_per_cell") != MIN_ACCEPTED_REPEATS
            or campaign.get("expected_result_cells") != EXPECTED_RESULT_CELLS
            or campaign.get("expected_raw_records")
                != EXPECTED_RESULT_CELLS * MIN_ACCEPTED_REPEATS
            or (not observed_rung
                and required_shard_configs != EXPECTED_SHARD_CONFIGS)):
        raise ContractError("campaign cardinality/stable-key identity changed")
    status_contract = _require_dict(
        campaign.get("publication_status_contract"), "publication status contract")
    expected_status_contract = (
        {
            "status_source": "observed",
            "minimap2": ["accepted"],
            "bioseq": ["accepted"],
            "rejected_dispositions": ["rejected_setup", "rejected_sql"],
            "nonpublishable_statuses": ["rejected", "invalid", "pending"],
        }
        if observed_rung else
        {
            "minimap2": ["accepted"],
            "bioseq": ["accepted", "rejected"],
            "rejected_dispositions": ["rejected_setup", "rejected_sql"],
            "nonpublishable_statuses": ["invalid", "pending"],
        })
    if status_contract != expected_status_contract:
        raise ContractError("campaign publication status contract changed")
    digest_payload = copy.deepcopy(contract)
    del digest_payload["campaign"]["campaign_digest"]
    if _sha256(_canonical_bytes(digest_payload)) != campaign_digest:
        raise ContractError("campaign contract digest does not recompute")

    source = _require_dict(contract.get("source"), "campaign contract source")
    _require_exact_keys(
        source, {"full_config", "full_config_sha256", "generator"},
        "campaign contract source")
    full_config_rel = _require_string(source.get("full_config"), "full config path")
    full_config_digest = _require_hex(
        source.get("full_config_sha256"), "full config digest", 64)
    generator_path = _require_string(
        source.get("generator"), "campaign generator path")
    if (observed_rung
            and generator_path != "bench/generate_e2e_algorithm_matrix_shards.py"):
        raise ContractError("observed rung names a noncanonical generator")
    projected_config: dict[str, Any] | None = None
    predecessor_execution_contract_id: str | None = None
    canonical_fixture_projection_sha256: str | None = None
    if observed_rung:
        full_config_path = _canonical_repository_file(
            full_config_rel, "observed rung full config")
        projected_config, projected_config_raw = _read_json_file(
            full_config_path, "observed rung full config")
        if _sha256(projected_config_raw) != full_config_digest:
            raise ContractError("observed rung full-config digest changed")
        predecessor_execution_contract_id = _validate_observed_rung_contract(
            observed_rung_contract, projected_config)
    elif ACTIVE_CAMPAIGN_PROFILE == "q1024":
        full_config_path = _canonical_repository_file(
            full_config_rel, "q1024 full config")
        canonical_config, canonical_config_raw = _read_json_file(
            full_config_path, "q1024 full config")
        if _sha256(canonical_config_raw) != full_config_digest:
            raise ContractError("q1024 full-config digest changed")
        canonical_fixture_projection_sha256 = (
            _rung_fixture_projection_sha256(
                canonical_config, EXPECTED_QUERY_COUNT))

    entries = _require_list(contract.get("configs"), "campaign configs")
    if len(entries) != required_shard_configs + 1:
        raise ContractError(
            "campaign contract must contain one monolith and exactly "
            f"{required_shard_configs} shards")
    configs_by_path: dict[Path, dict[str, Any]] = {}
    shard_entries: list[dict[str, Any]] = []
    monolithic: dict[str, Any] | None = None
    for raw_entry in entries:
        entry = _require_dict(raw_entry, "campaign config entry")
        _require_exact_keys(entry, {
            "path",
            "config_sha256",
            "role",
            "shard_kind",
            "expected_result_cells",
            "expected_raw_records",
            "expected_stable_keys_sha256",
            "allowed_nonblocking_rule_names",
            "declared_metric_names",
            "expected_all_records_disposition",
            "cells",
        }, "campaign config entry")
        relative = _require_string(entry.get("path"), "campaign config path")
        if Path(relative).is_absolute():
            raise ContractError("campaign config paths must be repository-relative")
        resolved = _canonical_repository_file(relative, "campaign config")
        if resolved in configs_by_path:
            raise ContractError(f"duplicate campaign config entry {relative}")
        _require_hex(entry.get("config_sha256"), "campaign config digest", 64)
        role = _require_string(entry.get("role"), "campaign config role")
        if role == "monolithic":
            if monolithic is not None or entry.get("shard_kind") is not None:
                raise ContractError("campaign must have one untyped monolithic config")
            monolithic = entry
        elif role == "shard":
            if entry.get("shard_kind") not in expected_shard_kinds:
                raise ContractError("campaign shard kind is not canonical")
            shard_entries.append(entry)
        else:
            raise ContractError(f"unknown campaign config role {role!r}")

        expected_count = _require_int(
            entry.get("expected_result_cells"), "expected config cells", minimum=1)
        if entry.get("expected_raw_records") != expected_count * MIN_ACCEPTED_REPEATS:
            raise ContractError("config raw-record cardinality is inconsistent")
        expected_key_digest = _require_hex(
            entry.get("expected_stable_keys_sha256"),
            "expected stable-key digest",
            64,
        )
        rule_names = _require_list(
            entry.get("allowed_nonblocking_rule_names"),
            "allowed nonblocking rules",
        )
        if (any(not isinstance(name, str) or not name for name in rule_names)
                or len(rule_names) != len(set(rule_names))):
            raise ContractError("allowed nonblocking rule names are malformed")
        declared_metric_names = _require_list(
            entry.get("declared_metric_names"), "declared metric names")
        if (any(not isinstance(name, str) or not name
                for name in declared_metric_names)
                or len(declared_metric_names) != len(set(declared_metric_names))):
            raise ContractError("declared metric names are malformed")
        cells = _require_list(entry.get("cells"), "campaign config cells")
        if len(cells) != expected_count:
            raise ContractError("campaign config cell list is truncated or extended")
        keys: list[str] = []
        descriptors_by_key: dict[str, dict[str, Any]] = {}
        for raw_cell in cells:
            cell = _require_dict(raw_cell, "campaign cell descriptor")
            _require_exact_keys(cell, {
                "stable_key",
                "identity",
                "test_case",
                "test_case_index",
                "algorithm",
                "tuple_index",
                "params",
                "allowed_statuses",
            }, "campaign cell descriptor")
            stable_key = _require_string(cell.get("stable_key"), "expected stable key")
            identity = _require_dict(cell.get("identity"), "expected stable identity")
            if tuple(identity) != STABLE_KEY_FIELDS or identity.get("campaign_id") != campaign_id:
                raise ContractError("campaign cell identity fields are not canonical")
            if _stable_key_for_identity(identity) != stable_key:
                raise ContractError("campaign cell stable key does not recompute")
            _require_string(cell.get("test_case"), "expected test case")
            _require_int(cell.get("test_case_index"), "expected test-case index")
            _require_string(cell.get("algorithm"), "expected algorithm")
            _require_int(cell.get("tuple_index"), "expected tuple index")
            params = _require_dict(cell.get("params"), "expected params")
            if any(isinstance(value, (dict, list)) or value is None for value in params.values()):
                raise ContractError("expected params must contain scalar values")
            allowed_statuses = _require_list(
                cell.get("allowed_statuses"), "allowed cell statuses")
            expected_statuses = _allowed_cell_statuses(
                identity.get("engine"), result_status_contract)
            if allowed_statuses != expected_statuses:
                raise ContractError("campaign cell has non-canonical allowed statuses")
            if stable_key in descriptors_by_key:
                raise ContractError("campaign config repeats a stable key")
            descriptors_by_key[stable_key] = cell
            keys.append(stable_key)
        if _sha256("\n".join(sorted(keys)).encode("utf-8")) != expected_key_digest:
            raise ContractError("campaign config stable-key digest does not recompute")
        expected_disposition = _expected_all_records_disposition(
            cells, result_status_contract)
        if entry.get("expected_all_records_disposition") != expected_disposition:
            raise ContractError(
                "campaign config all-record disposition is not canonical")
        expected_metrics = (
            ALL_REJECTED_METRIC_NAMES
            if expected_disposition is not None
            else MIXED_PERSISTED_METRIC_NAMES
            if entry.get("shard_kind") in {"persisted_index", "no_index_micro"}
            else MINIMAP2_BASELINE_METRIC_NAMES
            if entry.get("shard_kind") == "minimap2_baseline"
            else FULL_DECLARED_METRIC_NAMES)
        if declared_metric_names != list(expected_metrics):
            raise ContractError(
                "campaign config declared metric set is not canonical")
        entry["descriptors_by_key"] = descriptors_by_key
        entry["execution_descriptors_by_key"] = descriptors_by_key
        entry["execution_authority"] = "canonical"
        entry["sealed_evidence"] = None
        configs_by_path[resolved] = entry

    all_rejected_entries = [
        entry for entry in shard_entries
        if entry["expected_all_records_disposition"] is not None
    ]
    mixed_persisted_entries = [
        entry for entry in shard_entries
        if entry["shard_kind"] == "persisted_index"
        and entry["expected_all_records_disposition"] is None
    ]
    profile_rejected_shards = sum(
        all((cell["identity"]["kmer_size"], cell["identity"]["window_size"])
            == (20, 15) for cell in entry["cells"])
        for entry in all_rejected_entries
    )
    dna5_rejected_shards = sum(
        all(cell["identity"]["alphabet"] == "dna5"
            and (cell["identity"]["kmer_size"], cell["identity"]["window_size"])
                == (15, 10) for cell in entry["cells"])
        for entry in all_rejected_entries
    )
    dna4_vulkan_rejected_shards = sum(
        all(cell["identity"]["alphabet"] == "dna4"
            and (cell["identity"]["kmer_size"], cell["identity"]["window_size"])
                == (15, 10)
            and cell["identity"]["requested_minimizer_algorithm"]
                == "vulkan_minimap_integer"
            for cell in entry["cells"])
        for entry in all_rejected_entries
    )
    if observed_rung:
        if (all_rejected_entries
                or len(mixed_persisted_entries) != 80
                or profile_rejected_shards
                or dna5_rejected_shards
                or dna4_vulkan_rejected_shards):
            raise ContractError(
                "observed-status campaign retained a predicted rejection or "
                "lost one of its 80 full-metric persisted shards")
    elif (len(all_rejected_entries) != EXPECTED_ALL_REJECTED_SHARDS
            or len(mixed_persisted_entries)
                != EXPECTED_MIXED_PERSISTED_SHARDS
            or sum(entry["expected_all_records_disposition"] == "rejected_setup"
                   for entry in all_rejected_entries) != 60
            or sum(entry["expected_all_records_disposition"] == "rejected_sql"
                   for entry in all_rejected_entries) != 4
            or (profile_rejected_shards, dna5_rejected_shards,
                dna4_vulkan_rejected_shards) != (40, 20, 4)
            or sum(entry["expected_result_cells"] for entry in all_rejected_entries)
                != EXPECTED_ALL_REJECTED_CELLS
            or sum(entry["expected_raw_records"] for entry in all_rejected_entries)
                != EXPECTED_ALL_REJECTED_RECORDS):
        raise ContractError(
            "campaign all-rejected membership is not exact 40/20/4 and "
            "64 shards/512 cells/2560 records")

    metric_distribution: dict[int, int] = {}
    for entry in [monolithic, *shard_entries]:
        if entry is None:
            continue
        metric_count = len(entry["declared_metric_names"])
        metric_distribution[metric_count] = (
            metric_distribution.get(metric_count, 0) + 1)
    if metric_distribution != expected_metric_distribution:
        raise ContractError(
            "campaign metric applicability distribution is not exact: "
            f"actual={metric_distribution} expected={expected_metric_distribution}")

    actual_topology: dict[str, list[int]] = {}
    for entry in shard_entries:
        kind = entry["shard_kind"]
        bucket = actual_topology.setdefault(kind, [])
        bucket.append(entry["expected_result_cells"])
    if set(actual_topology) != set(expected_shard_topology):
        raise ContractError("campaign shard-kind topology changed")
    for kind, (expected_count, expected_cells) in expected_shard_topology.items():
        cell_counts = actual_topology[kind]
        if (len(cell_counts) != expected_count
                or any(value != expected_cells for value in cell_counts)):
            raise ContractError(
                f"campaign {kind} topology is not exact "
                f"{expected_count}x{expected_cells} cells")

    if monolithic is None or len(shard_entries) != required_shard_configs:
        raise ContractError("campaign monolith/shard count is not canonical")
    if (monolithic.get("path") != full_config_rel
            or monolithic.get("config_sha256") != full_config_digest
            or len(monolithic["descriptors_by_key"]) != EXPECTED_RESULT_CELLS):
        raise ContractError("campaign source does not match the monolithic entry")
    canonical_cells = monolithic["descriptors_by_key"]
    shard_owner: dict[str, str] = {}
    for entry in shard_entries:
        for stable_key in entry["descriptors_by_key"]:
            if stable_key in shard_owner:
                raise ContractError(
                    f"campaign shards overlap on {stable_key}: "
                    f"{shard_owner[stable_key]} and {entry['path']}")
            if stable_key not in canonical_cells:
                raise ContractError("campaign shard owns a non-canonical stable key")
            shard_owner[stable_key] = entry["path"]
    if set(shard_owner) != set(canonical_cells):
        raise ContractError("campaign shard ownership is incomplete")

    ownership = _require_dict(contract.get("shard_ownership"), "shard ownership")
    _require_exact_keys(ownership, {
        "exact_nonoverlapping", "shard_paths", "stable_keys_sha256", "resume_semantics"
    }, "shard ownership")
    if ownership.get("exact_nonoverlapping") is not True:
        raise ContractError("campaign shard ownership is not sealed exact/nonoverlapping")
    shard_paths = _require_list(ownership.get("shard_paths"), "shard paths")
    if shard_paths != [entry["path"] for entry in shard_entries]:
        raise ContractError("campaign shard path order/membership changed")
    if (_require_hex(ownership.get("stable_keys_sha256"), "ownership key digest", 64)
            != _sha256("\n".join(sorted(shard_owner)).encode("utf-8"))):
        raise ContractError("campaign ownership stable-key digest does not recompute")
    _require_string(ownership.get("resume_semantics"), "resume semantics")
    prefix_proof_path = None
    prefix_proof_sha256 = None
    q8_rescue_manifest_sha256 = None
    if QUERY_PREFIX_PROOF_PATH is not None:
        (prefix_proof_path, prefix_proof_sha256,
         q8_rescue_manifest_sha256) = (
            _validate_q8_query_prefix_proof(contract, raw))
    execution_authority_path = None
    execution_authority_sha256 = None
    if observed_rung:
        (singleton_entries, execution_authority_path,
         execution_authority_sha256) = (
            _load_observed_rung_singleton_partition(
                contract, raw, configs_by_path))
        configs_by_path.update(singleton_entries)
    elif ACTIVE_CAMPAIGN_PROFILE == "q8":
        rescue_entries = _load_q8_rescue_manifest(
            contract, raw, configs_by_path,
            _require_hex(q8_rescue_manifest_sha256,
                         "q8 rescue manifest SHA-256", 64))
        configs_by_path.update(rescue_entries)
        execution_authority_path = _relative(Q8_RESCUE_MANIFEST_PATH)
        execution_authority_sha256 = q8_rescue_manifest_sha256
    elif ACTIVE_CAMPAIGN_PROFILE == "q1024":
        singleton_entries = _load_q1024_singleton_partition(
            contract, raw, configs_by_path)
        hybrid_entries, singleton_fallback_entries = (
            _load_q1024_packed_partition(
                contract, raw, configs_by_path, singleton_entries))
        configs_by_path.update(singleton_fallback_entries)
        configs_by_path.update(hybrid_entries)
        execution_authority_path = _relative(Q1024_PACKED_PARTITION_PATH)
        execution_authority_sha256 = Q1024_PACKED_PARTITION_SHA256
    return {
        "campaign_id": campaign_id,
        "campaign_digest": campaign_digest,
        "execution_contract_id": execution_contract_id,
        "predecessor_execution_contract_id":
            predecessor_execution_contract_id,
        "query_count": EXPECTED_QUERY_COUNT,
        "result_status_contract": result_status_contract,
        "contract_sha256": _sha256(raw),
        "contract_path": _relative(CAMPAIGN_CONTRACT_PATH),
        "query_prefix_proof": prefix_proof_path,
        "query_prefix_proof_sha256": prefix_proof_sha256,
        "execution_authority": execution_authority_path,
        "execution_authority_sha256": execution_authority_sha256,
        "singleton_execution_authority": (
            _relative(Q1024_SINGLETON_PARTITION_PATH)
            if ACTIVE_CAMPAIGN_PROFILE == "q1024" else None),
        "singleton_execution_authority_sha256": (
            Q1024_SINGLETON_PARTITION_SHA256
            if ACTIVE_CAMPAIGN_PROFILE == "q1024" else None),
        "deterministic_first_ingestion": (
            Q1024_DETERMINISTIC_FIRST_INGESTION
            if ACTIVE_CAMPAIGN_PROFILE == "q1024" else None),
        "fixture_projection_sha256": (
            observed_rung_contract["fixture_projection_sha256"]
            if observed_rung_contract is not None
            else canonical_fixture_projection_sha256),
        "prefilter_enabled": False,
        "cigar_projection": True,
        "mapq_projection": True,
        "excluded_base_alignment_algorithms": ["ksw2_batched"],
        "configs_by_path": configs_by_path,
        "canonical_cells": canonical_cells,
    }


def _validate_config(
        path: Path,
        campaign_contract: dict[str, Any],
) -> tuple[dict[str, Any], str, dict[str, Any]]:
    entry = campaign_contract["configs_by_path"].get(path)
    if entry is None:
        raise ContractError(
            "config is not an exact path/digest member of the canonical campaign contract; "
            "an arbitrary campaign_shard_* basename is never authority"
        )
    if (ACTIVE_CAMPAIGN_PROFILE == "q1024"
            and entry.get("execution_authority") not in {
                "q1024_singleton", "q1024_packed",
                "q1024_deadline_v2_adoption"}):
        raise ContractError(
            "q1024 execution requires one exact hybrid packed/adopted/fallback "
            "member; unselected canonical configs remain topology authority only")
    if (ACTIVE_CAMPAIGN_PROFILE == "observed_rung"
            and entry.get("execution_authority")
                != "observed_rung_singleton"):
        raise ContractError(
            "observed-rung execution requires one exact singleton-partition "
            "member; parent configs remain topology authority only")
    config, raw = _read_json_file(path, "campaign config")
    config_digest = _sha256(raw)
    if config_digest != entry["config_sha256"]:
        raise ContractError("campaign config bytes differ from the canonical digest")
    bench = _require_dict(config.get("bench_e2e"), "config.bench_e2e")
    if (bench.get("fast") is not False
            or bench.get("concurrency") != {"bench_lock": True, "concurrency": False}
            or bench.get("iteration_order") != {"mode": "interleaved_seeded", "seed": 333}):
        raise ContractError("campaign config scheduling/serialization contract changed")
    matrix = _require_dict(bench.get("matrix"), "config.bench_e2e.matrix")
    metrics = _require_dict(matrix.get("metrics"), "config metrics")
    if list(metrics) != entry["declared_metric_names"]:
        raise ContractError(
            "campaign config metric declarations differ from the sealed exact set")
    if "match_count" not in metrics:
        raise ContractError("campaign config is missing match_count")
    if entry["expected_all_records_disposition"] is not None:
        if any(name in metrics for name in FULL_DECLARED_METRIC_NAMES[15:]):
            raise ContractError(
                "all-rejected shard declares accepted-only timing metrics")
    else:
        for metric in (
            "cigar_or_mapq_interval_union_ns",
            "outer_e2e_minus_cigar_or_mapq_union_ns",
        ):
            if metric not in metrics:
                raise ContractError(f"campaign config is missing metric {metric!r}")
    repeats = _require_dict(matrix.get("for"), "config matrix.for")
    if not repeats or any(_require_int(value, f"repeat count for {key}", minimum=1)
                          < MIN_ACCEPTED_REPEATS
                          for key, value in repeats.items()):
        raise ContractError("every campaign shard test case must request at least five repeats")
    if any(value != MIN_ACCEPTED_REPEATS for value in repeats.values()):
        raise ContractError("canonical campaign configs own exactly five repeats per cell")
    test_cases = _require_dict(config.get("test_cases"), "config test_cases")
    if set(test_cases) != set(repeats):
        raise ContractError("campaign config test-case ownership differs from matrix.for")
    for test_case, test_case_value in test_cases.items():
        data = _require_dict(
            _require_dict(test_case_value, f"test case {test_case}").get("data"),
            f"test case {test_case} data")
        if _require_int(
                data.get("fragment_count"),
                f"{test_case} fragment_count", minimum=1) != EXPECTED_QUERY_COUNT:
            raise ContractError(
                f"campaign config query_count is not exact {EXPECTED_QUERY_COUNT}")
    if config.get("regression_rules") != []:
        raise ContractError("canonical campaign configs may not declare regressions")
    rules = _require_list(
        _require_dict(metrics.get("match_count"), "match_count metric").get("rules"),
        "match_count rules",
    )
    observed_rule_names: list[str] = []
    for rule_value in rules:
        rule = _require_list(rule_value, "match_count rule")
        if len(rule) != 4:
            raise ContractError("campaign match-count rule must have four entries")
        name = _require_string(rule[0], "match-count rule name")
        comparator = _require_dict(rule[2], "match-count comparator")
        if (comparator.get("compare") != "="
                or comparator.get("tolerance%") != 0
                or comparator.get("aggregate") != "min"
                or comparator.get("pairing") != "by_iteration_exact"
                or comparator.get("blocking") is not False):
            raise ContractError(
                "campaign parity rules must remain exact same-repeat nonblocking diagnostics")
        observed_rule_names.append(name)
    if observed_rule_names != entry["allowed_nonblocking_rule_names"]:
        raise ContractError("campaign parity-rule membership differs from the contract")
    if ACTIVE_CAMPAIGN_PROFILE == "observed_rung":
        execution_cells = _execution_descriptors_from_config(
            config,
            campaign_contract["campaign_id"],
            campaign_contract["result_status_contract"],
        )
        if execution_cells != entry["cells"]:
            raise ContractError(
                "observed rung config expansion differs from its manifest cells")
    return config, config_digest, entry


def _canonical_record(record: dict[str, Any]) -> str:
    try:
        return json.dumps(
            record,
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        )
    except (TypeError, ValueError) as exc:
        raise ContractError(f"raw record is not canonically serializable: {exc}") from exc


def _validate_raw_sidecar(
    report_path: Path,
    report: dict[str, Any],
    config: dict[str, Any],
    config_entry: dict[str, Any],
    *, expected_repeats: int = MIN_ACCEPTED_REPEATS,
) -> tuple[list[dict[str, Any]], dict[str, Any], Path, str]:
    if expected_repeats not in (1, MIN_ACCEPTED_REPEATS):
        raise ContractError("unsupported explicit repeat policy")
    manifest = _require_dict(report.get("raw_evidence"), "report.raw_evidence")
    _require_exact_keys(manifest, RAW_MANIFEST_FIELDS, "raw evidence manifest")
    if (_require_int(manifest.get("schema"), "raw evidence schema") != 1
            or manifest.get("complete") is not True):
        raise ContractError("report raw evidence must be complete schema 1")
    run_id = _require_string(manifest.get("run_id"), "raw evidence run_id")
    sidecar_name = _require_string(manifest.get("path"), "raw evidence path")
    report_match = re.fullmatch(r"report_([0-9]+)\.json", report_path.name)
    if report_match is None:
        raise ContractError("framework report basename must be report_<N>.json")
    expected_sidecar = f"report_{report_match.group(1)}.raw.jsonl"
    if sidecar_name != expected_sidecar or Path(sidecar_name).name != sidecar_name:
        raise ContractError("raw evidence path must exactly match the report sidecar basename")
    sidecar_path = report_path.parent / sidecar_name
    raw = _read_regular(sidecar_path, "raw evidence sidecar")
    if not raw.endswith(b"\n"):
        raise ContractError("raw evidence sidecar must end with one complete line")
    raw_lines = raw.splitlines(keepends=True)
    record_count = _require_int(manifest.get("record_count"), "raw record_count")
    if record_count != config_entry["expected_raw_records"]:
        raise ContractError("raw manifest record count differs from canonical config ownership")
    if len(raw_lines) != record_count + 2:
        raise ContractError("raw sidecar line count does not match its manifest")

    decoded: list[dict[str, Any]] = []
    for index, line in enumerate(raw_lines):
        if not line.endswith(b"\n"):
            raise ContractError(f"raw sidecar line {index} is incomplete")
        try:
            parsed = _loads_json(line[:-1].decode("utf-8"), f"raw sidecar line {index}")
        except UnicodeError as exc:
            raise ContractError(f"raw sidecar line {index} is not UTF-8") from exc
        parsed_object = _require_dict(parsed, f"raw sidecar line {index}")
        if line[:-1] != _canonical_record(parsed_object).encode("utf-8"):
            raise ContractError(
                f"raw sidecar line {index} is not the framework canonical encoding")
        decoded.append(parsed_object)
    header = decoded[0]
    if (set(header) != {"kind", "schema", "run_id"}
            or header.get("kind") != "header"
            or _require_int(header.get("schema"), "raw header schema") != 1
            or header.get("run_id") != run_id):
        raise ContractError("raw sidecar header does not match the report manifest")
    footer = decoded[-1]
    if (set(footer) != {"kind", "schema", "manifest"}
            or footer.get("kind") != "footer"
            or _require_int(footer.get("schema"), "raw footer schema") != 1
            or _canonical_bytes(footer.get("manifest"))
                != _canonical_bytes(manifest)):
        raise ContractError("raw sidecar terminal footer is not authoritative")
    prefix_bytes = sum(len(line) for line in raw_lines[:-1])
    if prefix_bytes != _require_int(manifest.get("prefix_bytes"), "prefix_bytes", minimum=1):
        raise ContractError("raw sidecar prefix byte count is inconsistent")

    records = decoded[1:-1]
    matrix = _require_dict(
        _require_dict(config.get("bench_e2e"), "config bench_e2e").get("matrix"),
        "config matrix",
    )
    schedule_config = _require_dict(
        _require_dict(config.get("bench_e2e"), "config bench_e2e").get("iteration_order"),
        "iteration order",
    )
    configured_seed = _require_int(schedule_config.get("seed"), "iteration seed")
    if schedule_config.get("mode") != "interleaved_seeded":
        raise ContractError("campaign raw schedule must be interleaved_seeded")
    configured_seed_text = f"{configured_seed:016x}"
    test_case_indices = {
        name: index for index, name in enumerate(
            _require_dict(matrix.get("for"), "config matrix.for"))
    }
    descriptors_by_route: dict[tuple[str, str, int], dict[str, Any]] = {}
    tuple_indices_by_case: dict[str, list[int]] = {}
    for descriptor in config_entry["cells"]:
        route = (
            descriptor["test_case"],
            descriptor["algorithm"],
            descriptor["tuple_index"],
        )
        if route in descriptors_by_route:
            raise ContractError("campaign contract repeats a raw cell route")
        descriptors_by_route[route] = descriptor
        tuple_indices_by_case.setdefault(descriptor["test_case"], []).append(
            descriptor["tuple_index"])
    for test_case, tuple_indices in tuple_indices_by_case.items():
        unique_indices = sorted(set(tuple_indices))
        if unique_indices != list(range(len(unique_indices))):
            raise ContractError(
                f"canonical tuple indices for {test_case} are not contiguous")
        tuple_indices_by_case[test_case] = unique_indices

    checksums: list[str] = []
    schedule_checksums: list[tuple[str, str]] = []
    all_outer: list[int] = []
    all_index_and_map: list[int] = []
    cell_records: dict[tuple[str, str, int], list[dict[str, Any]]] = {}
    batch_records: dict[tuple[str, int], list[tuple[int, int, int]]] = {}
    seen_cell_repeats: set[tuple[tuple[str, str, int], int]] = set()
    for index, record in enumerate(records):
        if _require_int(record.get("raw_record_id"), "raw record id") != index:
            raise ContractError("raw record ids must be exact append-order ordinals")
        checksum = _require_hex(record.get("record_checksum"), "record checksum", 16)
        without_checksum = dict(record)
        del without_checksum["record_checksum"]
        if _fnv1a((_canonical_record(without_checksum),)) != checksum:
            raise ContractError(f"raw record {index} checksum does not recompute")

        if (_require_int(record.get("schema"), "raw framework schema") != 1
                or record.get("benchmark") != "bench_e2e"):
            raise ContractError("raw framework envelope identity is malformed")
        test_case = _require_string(record.get("test_case"), "raw test_case")
        algorithm = _require_string(record.get("algorithm"), "raw algorithm")
        tuple_index = _require_int(record.get("tuple_index"), "raw tuple_index")
        repeat = _require_int(record.get("repeat"), "raw repeat")
        group_index = _require_int(record.get("group_index"), "raw group_index")
        group_count = _require_int(record.get("group_count"), "raw group_count", minimum=1)
        if "outer_e2e_ns" not in record or "index_and_map_ns" not in record:
            raise ContractError(
                "raw E2E record is missing a required timing-nullability field")
        outer_value = record["outer_e2e_ns"]
        outer_e2e_ns = None if outer_value is None else _require_int(
            outer_value, "raw outer_e2e_ns")
        tuple_status = _require_dict(
            record.get("tuple_status"), "raw tuple_status")
        accepted_raw = tuple_status.get("accepted")
        if isinstance(accepted_raw, bool) or accepted_raw not in (0, 1):
            raise ContractError(
                "raw tuple_status.accepted must be integer 0 or 1")
        if (outer_e2e_ns is not None) != (accepted_raw == 1):
            raise ContractError(
                "framework outer timing presence contradicts E2E tuple disposition")
        if group_index != 0 or group_count != 1 or repeat >= expected_repeats:
            raise ContractError("campaign raw cell group/repeat shape differs from explicit repeat policy")
        route = (test_case, algorithm, tuple_index)
        descriptor = descriptors_by_route.get(route)
        if descriptor is None:
            raise ContractError("raw record route is not owned by the canonical config")
        params = _require_dict(record.get("params"), "raw params")
        if _canonical_bytes(params) != _canonical_bytes(descriptor["params"]):
            raise ContractError("raw record params differ from the canonical tuple")
        cell_repeat = (route, repeat)
        if cell_repeat in seen_cell_repeats:
            raise ContractError("raw sidecar repeats one canonical cell/repeat")
        seen_cell_repeats.add(cell_repeat)

        index_and_map_value = record.get("index_and_map_ns")
        if index_and_map_value is None:
            index_and_map_ns = None
        else:
            index_and_map_ns = _require_int(
                index_and_map_value, "raw index_and_map_ns")
            all_index_and_map.append(index_and_map_ns)
        schedule = _require_dict(record.get("schedule"), "raw record schedule")
        _require_exact_keys(schedule, RAW_SCHEDULE_FIELDS, "raw record schedule")
        schedule_benchmark = _require_int(
            schedule.get("benchmark_index"), "schedule.benchmark_index")
        schedule_case = _require_int(
            schedule.get("test_case_index"), "schedule.test_case_index")
        schedule_tuple = _require_int(
            schedule.get("tuple_index"), "schedule.tuple_index")
        schedule_group = _require_int(
            schedule.get("group_index"), "schedule.group_index")
        schedule_repeat = _require_int(
            schedule.get("repeat"), "schedule.repeat")
        order_position = _require_int(
            schedule.get("order_position"), "schedule.order_position")
        if (schedule_benchmark != 0
                or schedule_case != test_case_indices.get(test_case)
                or schedule_tuple != tuple_index
                or schedule_group != group_index
                or schedule_repeat != repeat
                or schedule.get("mode") != "interleaved_seeded"
                or schedule.get("configured_seed") != configured_seed_text):
            raise ContractError("raw schedule envelope differs from canonical config/program order")
        expected_shuffle_seed = _schedule_shuffle_seed(
            configured_seed, "bench_e2e", test_case, group_index, repeat)
        if (_require_hex(schedule.get("shuffle_seed"), "schedule.shuffle_seed", 16)
                != f"{expected_shuffle_seed:016x}"):
            raise ContractError("raw schedule shuffle seed does not recompute")
        if order_position >= len(tuple_indices_by_case[test_case]):
            raise ContractError("raw schedule order_position exceeds tuple cardinality")

        schedule_key = ":".join(
            f"{value:020d}" for value in (
                schedule_benchmark,
                schedule_case,
                schedule_tuple,
                schedule_group,
                schedule_repeat,
            ))
        checksums.append(checksum)
        schedule_checksums.append((schedule_key, checksum))
        if outer_e2e_ns is not None:
            all_outer.append(outer_e2e_ns)
        enriched = {
            "record": record,
            "checksum": checksum,
            "repeat": repeat,
            "outer_e2e_ns": outer_e2e_ns,
            "index_and_map_ns": index_and_map_ns,
        }
        cell_records.setdefault(route, []).append(enriched)
        batch_records.setdefault((test_case, repeat), []).append(
            (order_position, tuple_index, expected_shuffle_seed))

    expected_cell_repeats = {
        (
            (descriptor["test_case"], descriptor["algorithm"], descriptor["tuple_index"]),
            repeat,
        )
        for descriptor in config_entry["cells"]
        for repeat in range(expected_repeats)
    }
    if seen_cell_repeats != expected_cell_repeats:
        raise ContractError("raw sidecar canonical cell/repeat membership is truncated or forged")

    for (test_case, repeat), observed_entries in batch_records.items():
        tuple_indices = tuple_indices_by_case[test_case]
        if len(observed_entries) != len(tuple_indices):
            raise ContractError("raw interleaved batch cardinality is incomplete")
        observed_by_position: dict[int, tuple[int, int]] = {}
        for position, tuple_index, shuffle_seed in observed_entries:
            if position in observed_by_position:
                raise ContractError("raw schedule repeats an order_position")
            observed_by_position[position] = (tuple_index, shuffle_seed)
        expected_shuffle_seed = _schedule_shuffle_seed(
            configured_seed, "bench_e2e", test_case, 0, repeat)
        expected_order = _fisher_yates_order(len(tuple_indices), expected_shuffle_seed)
        expected_by_position = {
            position: (tuple_indices[selected], expected_shuffle_seed)
            for position, selected in enumerate(expected_order)
        }
        if observed_by_position != expected_by_position:
            raise ContractError("raw schedule does not equal canonical Fisher-Yates order")

    if _fnv1a(checksums) != _require_hex(
            manifest.get("append_order_checksum"), "append-order checksum", 16):
        raise ContractError("raw append-order checksum does not recompute")
    schedule_text: list[str] = []
    for key, checksum in sorted(schedule_checksums):
        schedule_text.extend((key, checksum))
    if _fnv1a(schedule_text) != _require_hex(
            manifest.get("schedule_sorted_checksum"), "schedule checksum", 16):
        raise ContractError("raw schedule checksum does not recompute")

    expected_cell_summaries: list[dict[str, Any]] = []
    for descriptor in config_entry["cells"]:
        route = (
            descriptor["test_case"],
            descriptor["algorithm"],
            descriptor["tuple_index"],
        )
        selected = sorted(cell_records.get(route, []), key=lambda item: item["repeat"])
        if [item["repeat"] for item in selected] != list(range(expected_repeats)):
            raise ContractError("raw cell does not own the exact configured repeat ordinals")
        outer_values = [
            item["outer_e2e_ns"] for item in selected
            if item["outer_e2e_ns"] is not None
        ]
        first_repeat_outer_values = [
            item["outer_e2e_ns"] for item in selected
            if item["repeat"] == 0
            and item["outer_e2e_ns"] is not None
        ]
        steady_state_outer_values = [
            item["outer_e2e_ns"] for item in selected
            if item["repeat"] != 0
            and item["outer_e2e_ns"] is not None
        ]
        index_values = [
            item["index_and_map_ns"] for item in selected
            if item["index_and_map_ns"] is not None
        ]
        cell_checksum = _fnv1a(sorted(item["checksum"] for item in selected))
        expected_cell_summaries.append({
            "benchmark": "bench_e2e",
            "test_case": descriptor["test_case"],
            "algorithm": descriptor["algorithm"],
            "tuple_index": descriptor["tuple_index"],
            "group_index": 0,
            "record_count": expected_repeats,
            "schedule_sorted_checksum": cell_checksum,
            "outer_e2e_ns": _duration_summary(outer_values),
            "first_repeat_outer_e2e_ns": _duration_summary(
                first_repeat_outer_values),
            "steady_state_outer_e2e_ns": _duration_summary(
                steady_state_outer_values),
            "index_and_map_ns": _duration_summary(index_values),
        })
    manifest_cells = _require_list(manifest.get("cells"), "raw manifest cells")
    for index, cell in enumerate(manifest_cells):
        cell = _require_dict(cell, "raw manifest cell")
        _require_exact_keys(
            cell, RAW_CELL_FIELDS, "raw manifest cell")
        for key in ("benchmark", "test_case", "algorithm"):
            _require_string(cell.get(key), f"raw manifest cell[{index}].{key}")
        for key in ("tuple_index", "group_index", "record_count"):
            _require_int(cell.get(key), f"raw manifest cell[{index}].{key}")
        _require_hex(
            cell.get("schedule_sorted_checksum"),
            f"raw manifest cell[{index}].schedule_sorted_checksum", 16)
        expected_cell = expected_cell_summaries[index] \
            if index < len(expected_cell_summaries) else None
        if expected_cell is not None:
            for key in (
                    "outer_e2e_ns", "first_repeat_outer_e2e_ns",
                    "steady_state_outer_e2e_ns", "index_and_map_ns"):
                _validate_duration_summary(
                    cell.get(key), expected_cell[key],
                    f"raw manifest cell[{index}].{key}")
    if _canonical_bytes(manifest_cells) != _canonical_bytes(expected_cell_summaries):
        raise ContractError("raw manifest cell summaries do not recompute exactly")
    _validate_duration_summary(
        manifest.get("outer_e2e_ns"), _duration_summary(all_outer),
        "raw manifest outer_e2e_ns")
    _validate_duration_summary(
        manifest.get("index_and_map_ns"), _duration_summary(all_index_and_map),
        "raw manifest index_and_map_ns")

    benchmark_report = _require_dict(
        _require_dict(report.get("benchmarks"), "report.benchmarks").get("bench_e2e"),
        "report benchmark bench_e2e",
    )
    execution_reports = _require_list(
        benchmark_report.get("execution_tuples"),
        "benchmark execution tuples")
    descriptors_by_tuple: dict[int, list[dict[str, Any]]] = {}
    for descriptor in config_entry["cells"]:
        descriptors_by_tuple.setdefault(
            descriptor["tuple_index"], []).append(descriptor)
    if sorted(descriptors_by_tuple) != list(range(len(execution_reports))):
        raise ContractError(
            "framework execution-tuple rows do not cover exact tuple ordinals")
    for tuple_index, execution_value in enumerate(execution_reports):
        execution = _require_dict(
            execution_value, "framework execution tuple")
        _require_exact_keys(
            execution, {"tuple", "test_cases"},
            "framework execution tuple")
        descriptors = descriptors_by_tuple[tuple_index]
        expected_algorithm = descriptors[0]["algorithm"]
        expected_params = descriptors[0]["params"]
        if any(
                descriptor["algorithm"] != expected_algorithm
                or descriptor["params"] != expected_params
                for descriptor in descriptors):
            raise ContractError(
                "canonical tuple ordinal changes algorithm/params by test case")
        tuple_identity = _require_dict(
            execution.get("tuple"), "framework tuple identity")
        if tuple_identity != {expected_algorithm: expected_params}:
            raise ContractError(
                "framework tuple identity differs from canonical config")
        test_case_reports = _require_dict(
            execution.get("test_cases"), "framework tuple test_cases")
        expected_test_cases = {
            descriptor["test_case"] for descriptor in descriptors
        }
        if set(test_case_reports) != expected_test_cases:
            raise ContractError(
                "framework tuple test-case coverage differs from canonical config")
        for descriptor in descriptors:
            route = (
                descriptor["test_case"], descriptor["algorithm"], tuple_index)
            raw_for_cell = cell_records.get(route, [])
            accepted_count = sum(
                _require_dict(
                    item["record"].get("tuple_status"),
                    "raw tuple_status").get("accepted") == 1
                for item in raw_for_cell
            )
            test_case_report = _require_dict(
                test_case_reports.get(descriptor["test_case"]),
                "framework test-case timing projection")
            all_regions = _require_dict(
                test_case_report.get("all_regions"),
                "framework all_regions timing projection")
            for metric_name in ("wall_clock_ns", "region_total_ns"):
                metric_value = all_regions.get(metric_name)
                if accepted_count == 0:
                    if metric_name in all_regions:
                        raise ContractError(
                            "rejected E2E cell contaminated framework-owned "
                            f"{metric_name} aggregation")
                else:
                    metric_report = _require_dict(
                        metric_value,
                        f"framework accepted {metric_name} aggregation")
                    if _require_int(
                            metric_report.get("samples_count"),
                            f"framework {metric_name} samples_count") \
                            != accepted_count:
                        raise ContractError(
                            "framework accepted timing sample count differs "
                            "from exact raw tuple dispositions")
            if accepted_count == 0:
                for region_name, region_value in test_case_report.items():
                    region = _require_dict(
                        region_value,
                        f"framework region {region_name}")
                    if "wall_clock_ns" in region \
                            or "region_total_ns" in region:
                        raise ContractError(
                            "rejected E2E cell retained a framework-owned "
                            "wall/region timing metric")
    embedded = _require_list(benchmark_report.get("raw_samples"), "embedded raw samples")
    if _canonical_bytes(embedded) != _canonical_bytes(records):
        raise ContractError("embedded framework raw records differ from the durable sidecar")
    return records, manifest, sidecar_path, _sha256(raw)


def _stable_identity(
        record: dict[str, Any],
        campaign_id: str,
        payload_schema_version: int | None = None,
) -> tuple[dict[str, Any], str, str]:
    if payload_schema_version is None:
        payload_schema_version = _require_int(
            record.get("payload_schema_version"), "raw payload schema version")
        if (record.get("payload_schema") != "bioseq.e2e.raw-sample"
                or payload_schema_version
                    not in SUPPORTED_RAW_PAYLOAD_SCHEMA_VERSIONS):
            raise ContractError(
                "stable identity requires a supported E2E raw payload schema")
    algorithm = _require_string(record.get("algorithm"), "algorithm")
    if algorithm in BIOSEQ_ALGORITHMS:
        engine = "bioseq"
        mode = BIOSEQ_ALGORITHMS[algorithm]
    elif algorithm in BASELINE_ALGORITHMS:
        engine = "minimap2"
        mode = BASELINE_ALGORITHMS[algorithm]
    else:
        raise ContractError(f"unexpected algorithm-matrix record {algorithm!r}")
    pairing = _require_dict(record.get("pairing"), "record.pairing")
    pairing_fields = {
        "dataset_key", "alphabet", "target_size_bp", "kmer_size",
        "window_size", "mode", "index_key", "repeat_field",
    }
    if payload_schema_version == RAW_PAYLOAD_SCHEMA_VERSION:
        pairing_fields |= {
            "generated_target_content_crc64",
            "persisted_source_content_crc64",
            "generated_target_matches_persisted_source",
            "minimap2_setup_receipt",
        }
    elif payload_schema_version != LEGACY_RAW_PAYLOAD_SCHEMA_VERSION:
        raise ContractError("record pairing uses an unsupported payload schema")
    _require_exact_keys(pairing, pairing_fields, "record.pairing")
    if pairing.get("mode") != mode or pairing.get("repeat_field") != "framework_envelope.repeat":
        raise ContractError("record pairing mode/repeat contract is inconsistent")
    target_size_bp = _require_int(pairing.get("target_size_bp"), "target_size_bp", minimum=1)
    if target_size_bp % 1_000_000_000 != 0:
        raise ContractError("target size must be an exact integer Gbp campaign size")
    params = _require_dict(record.get("params"), "record.params")
    if engine == "bioseq":
        index_name = _require_string(params.get("minimizer_index"), "minimizer_index")
        requested_minimizer = _require_string(
            params.get("minimizer_algorithm"), "requested minimizer")
        requested_chaining = _require_string(
            params.get("chaining_algorithm"), "requested chaining")
        requested_base = _require_string(
            params.get("base_alignment_algorithm"), "requested base alignment")
    else:
        index_name = None
        requested_minimizer = None
        requested_chaining = None
        requested_base = None
    expected_pairing_index = (
        "none" if mode == "no_index" else
        "minimap2_mmi" if engine == "minimap2" else index_name)
    if pairing.get("index_key") != expected_pairing_index:
        raise ContractError("record pairing index identity is inconsistent")
    identity = {
        "campaign_id": campaign_id,
        "dataset": _require_string(pairing.get("dataset_key"), "dataset key"),
        "alphabet": _require_string(pairing.get("alphabet"), "alphabet"),
        "target_size_gbp": target_size_bp // 1_000_000_000,
        "kmer_size": _require_int(pairing.get("kmer_size"), "kmer_size", minimum=1),
        "window_size": _require_int(pairing.get("window_size"), "window_size", minimum=1),
        "index_mode": mode,
        "engine": engine,
        "bioseq_index_name": index_name,
        "requested_minimizer_algorithm": requested_minimizer,
        "requested_chaining_algorithm": requested_chaining,
        "requested_base_alignment_algorithm": requested_base,
    }
    if tuple(identity) != STABLE_KEY_FIELDS:
        raise AssertionError("stable identity field order changed")
    stable_key = _stable_key_for_identity(identity)
    return identity, stable_key, algorithm


def _validate_pairing_evidence(
        record: dict[str, Any],
        payload_schema_version: int,
        paired: bool,
) -> tuple[str, str, bytes] | None:
    """Validate current pairing evidence without changing retained v11 rows."""
    if payload_schema_version == LEGACY_RAW_PAYLOAD_SCHEMA_VERSION:
        return None
    pairing = _require_dict(record.get("pairing"), "record.pairing")
    evidence_fields = (
        "generated_target_content_crc64",
        "persisted_source_content_crc64",
        "generated_target_matches_persisted_source",
        "minimap2_setup_receipt",
    )
    if not paired:
        if any(pairing.get(field) is not None for field in evidence_fields):
            raise ContractError(
                "ordinary current-schema row retained paired target/setup evidence")
        return None

    generated = _require_hex(
        pairing.get("generated_target_content_crc64"),
        "pairing generated target CRC64", 16)
    persisted = _require_hex(
        pairing.get("persisted_source_content_crc64"),
        "pairing persisted source CRC64", 16)
    matches = _require_bool(
        pairing.get("generated_target_matches_persisted_source"),
        "pairing generated/persisted target match")
    if not matches or generated != persisted:
        raise ContractError(
            "paired target CRC64 evidence does not prove identical content")

    setup = _require_dict(
        pairing.get("minimap2_setup_receipt"),
        "pairing minimap2 setup receipt")
    _require_exact_keys(setup, {
        "checksum", "artifact_path", "artifact_digest", "validated_summary",
        "preserved_external_evidence",
    }, "pairing minimap2 setup receipt")
    _require_hex(setup.get("checksum"), "minimap2 setup receipt checksum", 16)
    _require_string(
        setup.get("artifact_path"), "minimap2 setup receipt artifact path")
    _require_hex(
        setup.get("artifact_digest"),
        "minimap2 setup receipt artifact digest", 16)
    _validate_minimap2_validated_summary(
        setup.get("validated_summary"), "minimap2 setup validated summary")
    if _require_bool(
            setup.get("preserved_external_evidence"),
            "minimap2 setup preserved-external-evidence flag") is not True:
        raise ContractError(
            "minimap2 setup receipt does not preserve its external evidence")
    return generated, persisted, _canonical_bytes(setup)


def _validate_framework_seal(
        report: dict[str, Any],
        config: dict[str, Any],
        config_path: Path,
        config_entry: dict[str, Any],
) -> None:
    _require_exact_keys(report, REPORT_FIELDS, "framework report")
    meta = _require_dict(report.get("meta"), "framework report meta")
    _require_exact_keys(
        meta,
        {"commit", "dirty", "timestamp", "created", "fast", "history_eligible"},
        "framework report meta",
    )
    _require_text(meta.get("commit"), "meta.commit")
    _require_bool(meta.get("dirty"), "meta.dirty")
    _require_int(meta.get("timestamp"), "meta.timestamp")
    _require_string(meta.get("created"), "meta.created")
    if meta.get("fast") is not False:
        raise ContractError("campaign report unexpectedly carries fast=true")
    if meta.get("history_eligible") is not True:
        raise ContractError(
            "standalone ingestion requires the framework's successful "
            "meta.history_eligible=true seal")

    benchmarks = _require_dict(report.get("benchmarks"), "framework benchmarks")
    if set(benchmarks) != {"bench_e2e"}:
        raise ContractError("campaign report must contain exactly bench_e2e")
    benchmark = _require_dict(benchmarks.get("bench_e2e"), "bench_e2e report")
    _require_exact_keys(benchmark, {
        "title",
        "description",
        "location",
        "test_case_descriptions",
        "execution_tuples",
        "raw_samples",
    }, "bench_e2e report")
    location = _sealed_report_config_location(
        benchmark.get("location"), config_path, config_entry,
        "report config location")
    expected_location = config_path
    if config_entry.get("execution_authority") \
            == "q1024_deadline_v2_adoption":
        adoption = _require_dict(
            config_entry.get("sealed_evidence"),
            "q1024 adoption evidence")
        receipt_config = _require_dict(
            adoption.get("run_receipt_config"),
            "q1024 adoption run-receipt config")
        expected_location = _canonical_repository_file(
            _require_string(
                receipt_config.get("path"),
                "q1024 adoption run-receipt config path"),
            "q1024 adoption run-receipt config")
    if location != expected_location:
        raise ContractError("report config location differs from the requested campaign config")
    _require_string(benchmark.get("title"), "benchmark title")
    _require_text(benchmark.get("description"), "benchmark description")
    _require_dict(benchmark.get("test_case_descriptions"), "test-case descriptions")
    _require_list(benchmark.get("execution_tuples"), "execution tuples")
    _require_list(benchmark.get("raw_samples"), "embedded raw samples")

    paired = _require_list(
        report.get("paired_rule_evidence"), "paired-rule evidence")
    if paired:
        raise ContractError(
            "campaign exact-pair rules must not emit ratio-paired evidence")
    regressions = _require_list(report.get("regressions"), "framework regressions")
    if regressions:
        raise ContractError("campaign config declares no regressions; report must contain none")

    rules = config["bench_e2e"]["matrix"]["metrics"]["match_count"]["rules"]
    rule_test_case = {rule[0]: rule[1]["test_case"] for rule in rules}
    allowed_rules = set(config_entry["allowed_nonblocking_rule_names"])
    violations = _require_list(report.get("rule_violations"), "rule violations")
    for raw_violation in violations:
        violation = _require_dict(raw_violation, "rule violation")
        _require_exact_keys(violation, {
            "location",
            "benchmark",
            "rule_name",
            "execution_tuple",
            "test_case",
            "metric",
            "comparison_mode",
            "warmup_repetitions",
            "blocking",
            "args",
            "args_present",
        }, "rule violation")
        name = _require_string(violation.get("rule_name"), "rule violation name")
        violation_location = _sealed_report_config_location(
            violation.get("location"), config_path, config_entry,
            "violation config location")
        if (name not in allowed_rules
                or violation_location != config_path
                or violation.get("benchmark") != "bench_e2e"
                or violation.get("test_case") != rule_test_case.get(name)
                or violation.get("metric") != "match_count"
                or violation.get("comparison_mode") != "paired_iteration_exact"
                or violation.get("warmup_repetitions") != 0
                or violation.get("blocking") is not False):
            raise ContractError(
                "framework report contains a finding outside the intentionally "
                "nonblocking campaign parity-rule allowlist")
        _require_dict(violation.get("execution_tuple"), "violation execution tuple")
        args = _require_list(violation.get("args"), "violation args")
        present = _require_list(violation.get("args_present"), "violation args_present")
        if (len(args) != 2 or any(
                isinstance(value, bool) or not isinstance(value, (int, float))
                or not math.isfinite(value) for value in args)):
            raise ContractError("rule violation args must be two finite numbers")
        if len(present) != 2 or any(not isinstance(value, bool) for value in present):
            raise ContractError("rule violation args_present must be two Booleans")

    warnings = _require_list(report.get("sanity_warnings"), "sanity warnings")
    for raw_warning in warnings:
        warning = _require_dict(raw_warning, "sanity warning")
        _require_exact_keys(warning, {
            "type", "metric", "message", "warning_since", "warning_since_commit"
        }, "sanity warning")
        _require_string(warning.get("type"), "sanity warning type")
        _require_string(warning.get("metric"), "sanity warning metric")
        _require_string(warning.get("message"), "sanity warning message")
        for key in ("warning_since", "warning_since_commit"):
            if warning.get(key) is not None:
                _require_string(warning.get(key), f"sanity warning {key}")


def _validate_minimap2_validated_summary(
        value: Any, label: str,
) -> dict[str, Any]:
    """Apply the existing minimap2 validated-summary semantic contract."""
    summary = _require_dict(value, label)
    if (
        summary.get("schema")
            != "bioseq.minimap2.e2e-phase-receipt/validated-summary"
        or summary.get("version") != 2
        or summary.get("receipt_schema_version") != 2
        or summary.get("child_lifetime_envelope_contained") is not True
        or summary.get("framework_repeat_association") != "program_order"
        or summary.get("framework_outer_timestamp_containment_claimed") is not False
    ):
        raise ContractError("minimap2 summary conflates child lifetime and framework outer")
    validity = _require_dict(summary.get("validity"), f"{label}.validity")
    if any(validity.get(key) is not True for key in REQUIRED_MINIMAP_VALIDITY):
        raise ContractError("minimap2 receipt lacks required accepted validity evidence")
    return summary


def _validate_receipt(record: dict[str, Any], union_ns: int, engine: str) -> None:
    receipt = _require_dict(record.get("interval_receipt"), "interval receipt")
    receipt_union = _require_int(receipt.get("union_ns"), "interval receipt union")
    if receipt.get("available") is not True or receipt_union != union_ns:
        raise ContractError("accepted interval receipt does not own the exact union")
    if engine == "bioseq":
        _require_exact_keys(receipt, {
            "source", "stats_revision", "available", "union_ns",
            "interval_count", "cigar_interval_count", "mapq_interval_count",
            "finalization_count", "failure_code", "physical_interval_semantics",
            "origins",
        }, "Bioseq interval receipt")
        if (
            receipt.get("source") != "bioseq_postgresql_revision6"
            or _require_int(receipt.get("stats_revision"), "Bioseq stats revision") != 6
            or _require_int(
                receipt.get("finalization_count"), "Bioseq finalization count") != 1
            or receipt.get("failure_code") != "none"
            or receipt.get("physical_interval_semantics")
                != "half_open_monotonic_same_statement_union"
        ):
            raise ContractError("Bioseq interval receipt is not accepted revision 6 evidence")
        interval_count = _require_int(
            receipt.get("interval_count"), "Bioseq interval count", minimum=1)
        cigar_count = _require_int(
            receipt.get("cigar_interval_count"), "Bioseq CIGAR interval count")
        mapq_count = _require_int(
            receipt.get("mapq_interval_count"), "Bioseq MAPQ interval count")
        if cigar_count + mapq_count != interval_count:
            raise ContractError("Bioseq interval receipt phase counts are inconsistent")
        if not _require_list(receipt.get("origins"), "Bioseq interval origins"):
            raise ContractError("Bioseq interval receipt has no operation origins")
        return
    _require_exact_keys(receipt, {
        "source", "available", "union_ns", "cigar_union_ns", "mapq_union_ns",
        "cigar_interval_count", "mapq_interval_count", "child_pid", "checksum",
        "artifact", "validated_summary", "raw_intervals_embedded",
    }, "minimap2 interval receipt")
    if receipt.get("source") != "minimap2_phase_receipt_v2":
        raise ContractError("minimap2 interval receipt source is not schema v2")
    cigar_union = _require_int(receipt.get("cigar_union_ns"), "minimap2 CIGAR union")
    mapq_union = _require_int(receipt.get("mapq_union_ns"), "minimap2 MAPQ union")
    if (union_ns > cigar_union + mapq_union
            or _require_int(
                receipt.get("cigar_interval_count"),
                "minimap2 CIGAR interval count", minimum=1) < 1
            or _require_int(
                receipt.get("mapq_interval_count"),
                "minimap2 MAPQ interval count", minimum=1) < 1
            or _require_int(receipt.get("child_pid"), "minimap2 child pid", minimum=1) < 1
            or receipt.get("raw_intervals_embedded") is not False):
        raise ContractError("minimap2 interval receipt count/union shape is invalid")
    _require_hex(receipt.get("checksum"), "minimap2 receipt checksum", 16)
    artifact = _require_dict(receipt.get("artifact"), "minimap2 receipt artifact")
    _require_exact_keys(
        artifact, {"path", "digest_algorithm", "digest"},
        "minimap2 receipt artifact")
    _require_string(artifact.get("path"), "minimap2 receipt artifact path")
    if artifact.get("digest_algorithm") != "fnv-1a-64_raw_receipt_bytes":
        raise ContractError("minimap2 receipt artifact digest algorithm changed")
    _require_hex(artifact.get("digest"), "minimap2 receipt artifact digest", 16)
    _validate_minimap2_validated_summary(
        receipt.get("validated_summary"), "minimap2 validated summary")


def _parse_vulkan_hardware_rejection_detail(
        detail: str,
) -> dict[str, int] | None:
    match = re.fullmatch(
        r"Vulkan segment minimizer pair path selected but strict Vulkan "
        r"execution was rejected \(reason=hardware_device_unavailable "
        r"bp=(?P<bp>0|[1-9][0-9]*) "
        r"padded_bytes=(?P<padded_bytes>0|[1-9][0-9]*) "
        r"max_min=(?P<max_min>0|[1-9][0-9]*) "
        r"query_gate_words=(?P<query_gate_words>0|[1-9][0-9]*) "
        r"w=(?P<window_size>0|[1-9][0-9]*) "
        r"k=(?P<kmer_size>0|[1-9][0-9]*)\)",
        detail,
    )
    if match is None:
        return None
    parsed = {key: int(value) for key, value in match.groupdict().items()}
    if parsed["bp"] <= 0:
        return None
    return parsed


def _validate_attestation_shape(
        record: dict[str, Any],
        attestation: dict[str, Any],
        payload_schema_version: int | None = None,
) -> int:
    """Validate the exact versioned attestation shape and MAPQ sentinels."""
    if payload_schema_version is None:
        payload_schema_version = _require_int(
            record.get("payload_schema_version"), "raw payload schema version")
        if (record.get("payload_schema") != "bioseq.e2e.raw-sample"
                or payload_schema_version
                    not in SUPPORTED_RAW_PAYLOAD_SCHEMA_VERSIONS):
            raise ContractError(
                "attestation requires a supported E2E raw payload schema")
    elif payload_schema_version not in SUPPORTED_RAW_PAYLOAD_SCHEMA_VERSIONS:
        raise ContractError("attestation uses an unsupported payload schema")
    fields = (
        ATTESTATION_FIELDS_V12
        if payload_schema_version == RAW_PAYLOAD_SCHEMA_VERSION
        else ATTESTATION_FIELDS_V11
    )
    _require_exact_keys(attestation, fields, "attestation")
    if payload_schema_version == RAW_PAYLOAD_SCHEMA_VERSION:
        if _require_bool(
                attestation.get(
                    "numeric_mapq_matches_minimap2_semantics_claimed"),
                "attestation numeric MAPQ semantics claim") is not False:
            raise ContractError(
                "algorithm-matrix attestation claimed numeric minimap2 MAPQ semantics")
        physical_vulkan_mapq = attestation.get(
            "vulkan_physical_mapq_executed")
        if attestation.get("observed_base_alignment_algorithm") \
                == "vulkan_simple_cigar":
            _require_bool(
                physical_vulkan_mapq,
                "attestation Vulkan physical MAPQ execution")
        elif physical_vulkan_mapq is not None:
            raise ContractError(
                "non-Vulkan-simple-CIGAR attestation retained physical MAPQ evidence")
    return payload_schema_version


def _validate_expected_rejection(
        status: dict[str, Any],
        identity: dict[str, Any],
        disposition: str,
        reason: str,
        mapq_projection: bool,
        cigar_projection: bool,
        prefilter_enabled: bool,
) -> None:
    if identity["engine"] != "bioseq":
        raise ContractError("minimap2 rows cannot be expected capability rejections")
    diagnostics = _require_dict(status.get("sql_diagnostics"), "SQL diagnostics")
    # Exact keys make uncontracted PostgreSQL hint/connection strings a
    # fail-closed schema violation instead of latent rejection identity.
    _require_exact_keys(
        diagnostics,
        {"result_status", "sqlstate", "primary", "detail"},
        "SQL diagnostics",
    )
    for key in diagnostics:
        _require_text(diagnostics.get(key), f"SQL diagnostics.{key}")

    if diagnostics.get("result_status") != "PGRES_FATAL_ERROR":
        raise ContractError("expected rejection lacks exact PostgreSQL error authority")

    minimizer = identity["requested_minimizer_algorithm"]
    chaining = identity["requested_chaining_algorithm"]
    alignment = identity["requested_base_alignment_algorithm"]
    persisted = identity["index_mode"] == "just_index"
    complete_vulkan = (
        minimizer == "vulkan_minimap_integer"
        and chaining == "vulkan_tiled_dynamic_programming"
        and alignment == "block_aligner"
        and not mapq_projection
    )
    requests_vulkan = (
        minimizer == "vulkan_minimap_integer"
        or chaining == "vulkan_tiled_dynamic_programming"
    )
    primary = diagnostics["primary"]
    detail = diagnostics["detail"]
    sqlstate = diagnostics["sqlstate"]

    if disposition == "rejected_setup" and sqlstate == "22023":
        expected_primary = 'bioseq.seq alphabet "dna5" is not supported'
        expected_reason = (
            "allowlisted untimed bioseq.seq typmod capability rejection "
            f"identity={minimizer}|{identity['bioseq_index_name']} "
            f"SQLSTATE=22023 primary={expected_primary}")
        if (identity["alphabet"] != "dna5"
                or primary != expected_primary
                or detail != ""
                or reason != expected_reason):
            raise ContractError(
                "Bioseq typmod setup rejection is not the exact dna5 profile")
        return

    if (sqlstate == "22023"
            and persisted
            and requests_vulkan
            and primary ==
                "required persisted GPU grouped-SoA target is unavailable"
            and detail ==
                "GPU prepared-target identity, pinned k/w, or all-GPU selector is invalid"):
        return

    if disposition == "rejected_setup" and sqlstate == "55000":
        expected_primary = (
            "bioseq current-generation index: READY generation minimizer "
            "storage contract disagrees")
        expected_reason = (
            "allowlisted untimed READY-generation audit capability rejection "
            f"identity={minimizer}|{identity['bioseq_index_name']} "
            f"SQLSTATE=55000 primary={expected_primary}")
        if (not persisted
                or (identity["kmer_size"], identity["window_size"]) != (20, 15)
                or minimizer not in {
                    "scalar",
                    "simd_minimap2_reference",
                    "two_stack_nthash",
                    "bounded_nthash_simd",
                    "vulkan_minimap_integer",
                }
                or identity["bioseq_index_name"] not in {
                    "minimizers_per_fragment",
                    "minimizers_per_sequence",
                }
                or primary != expected_primary
                or detail != ""
                or reason != expected_reason):
            raise ContractError(
                "READY-generation audit setup rejection is not the exact "
                "k20/w15 profile")
        return

    if disposition == "rejected_setup" and sqlstate == "XX000":
        expected_primary = "bioseq.update_index raised a C++ exception"
        expected_reason = (
            "allowlisted untimed bioseq.update_index Vulkan hardware "
            "capability rejection "
            f"identity={minimizer}|{identity['bioseq_index_name']} "
            f"SQLSTATE=XX000 primary={expected_primary}")
        diagnostic = _parse_vulkan_hardware_rejection_detail(detail)
        if (not persisted
                or identity["dataset"] != "matrix_2gbp_dna4_k15_w10"
                or identity["alphabet"] != "dna4"
                or identity["target_size_gbp"] != 2
                or (identity["kmer_size"], identity["window_size"]) != (15, 10)
                or minimizer != "vulkan_minimap_integer"
                or identity["bioseq_index_name"] not in {
                    "minimizers_per_fragment",
                    "minimizers_per_sequence",
                }
                or chaining not in {
                    "scalar_minimap2",
                    "simd_minimap2",
                    "range_minimum_query",
                    "vulkan_tiled_dynamic_programming",
                }
                or alignment not in {"ksw2", "block_aligner"}
                or not cigar_projection
                or not mapq_projection
                or prefilter_enabled
                or primary != expected_primary
                or diagnostic is None
                or diagnostic["window_size"] != identity["window_size"]
                or diagnostic["kmer_size"] != identity["kmer_size"]
                or reason != expected_reason):
            raise ContractError(
                "Vulkan hardware setup rejection is not the exact persisted "
                "2G/dna4/k15/w10 profile")
        return

    if sqlstate != "0A000":
        raise ContractError("expected rejection lacks exact PostgreSQL error authority")

    if disposition == "rejected_setup":
        if not persisted:
            raise ContractError("setup rejection is impossible for this canonical cell")
        backend = {
            "scalar": "scalar",
            "simd_minimap2_reference": "scalar",
            "two_stack_nthash": "exact_simd",
            "bounded_nthash_simd": "bounded_nthash_simd",
            "vulkan_minimap_integer": "vulkan_segment",
        }.get(minimizer)
        if backend is None:
            raise ContractError("setup rejection names an unknown minimizer backend")
        profile = (identity["kmer_size"], identity["window_size"])
        if profile == (20, 15):
            expected_primary = (
                "bioseq.update_index does not support the active minimizer "
                "persistence profile")
            expected_detail = (
                f"index_name={identity['bioseq_index_name']} backend={backend} "
                "k=20 w=15 supported_k=15 supported_w=10")
        elif profile == (15, 10) and identity["alphabet"] == "dna5":
            if minimizer == "bounded_nthash_simd":
                expected_primary = (
                    "Bounded ntHash SIMD minimizer index build is unsupported "
                    "for this source")
                expected_detail = (
                    "bioseq.update_index: status=unsupported_alphabet "
                    "grouped minimizer generation preflight: checked bounded "
                    "ntHash route failed (unsupported_alphabet)")
            else:
                expected_primary = (
                    "READY minimizer backend has no canonical grouped-SoA profile")
                expected_detail = f"backend={backend} wire_error=24"
        else:
            raise ContractError(
                "setup rejection is impossible for this canonical cell")
        expected_reason = (
            "allowlisted untimed bioseq.update_index capability rejection "
            f"identity={minimizer}|{identity['bioseq_index_name']} "
            f"SQLSTATE=0A000 primary={expected_primary}")
        if (primary != expected_primary or detail != expected_detail
                or reason != expected_reason):
            raise ContractError("persisted setup rejection is not the exact allowlisted profile")
        return

    allowed = False
    if (primary == "Exact SIMD minimizer execution is unsupported for this request"
            and detail == "capability=exact_simd_minimizer; fallback=forbidden"):
        allowed = minimizer == "two_stack_nthash"
    elif (primary == "Bounded ntHash SIMD minimizer execution is unsupported for this request"
            and detail == "capability=bounded_nthash_simd_minimizer; fallback=forbidden"):
        allowed = minimizer == "bounded_nthash_simd"
    elif (primary == "SIMD chaining execution is unsupported for this request"
            and detail == "capability=simd_chaining; fallback=forbidden"):
        allowed = chaining == "simd_minimap2"
    elif (primary ==
            "persisted GPU grouped-SoA execution does not yet support CIGAR projection"
            and detail ==
            "the direct path can execute query sketching, persisted Shape lookup, GPU chaining, "
            "backtracking, and coordinate projection, but no genuine GPU target-window reader "
            "is wired for alignment extension"):
        allowed = persisted and complete_vulkan
    elif (primary == "Vulkan algorithm tuple is unsupported"
            and detail ==
            "requested Vulkan components do not form the complete supported all-GPU tuple; "
            "generic CPU/SIMD biological fallback is forbidden"):
        allowed = requests_vulkan and not complete_vulkan
    elif (primary == "Vulkan simple-CIGAR span exceeds the supported ABI limit"
            and detail ==
            "simple_cigar_max_span=16384; one or more selected alignment spans exceeded this limit"):
        allowed = not persisted and complete_vulkan
    expected_reason = (
        f"allowlisted PostgreSQL tuple rejection SQLSTATE=0A000 primary={primary}")
    if not allowed or reason != expected_reason:
        raise ContractError("measured SQL rejection is not the exact tuple allowlist")


def _validate_unexecuted_measured_connection_attestation(record: dict[str, Any]) -> None:
    """Require the exact no-route attestation emitted after backend connection loss."""
    attestation = _require_dict(record.get("attestation"), "attestation")
    payload_schema_version = _validate_attestation_shape(record, attestation)
    empty_text_fields = {
        "actual_backend",
        "actual_target_index_source",
        "observed_base_alignment_algorithm",
        "observed_chaining_algorithm",
        "observed_minimizer_algorithm",
        "observed_simd_alignment_algorithm",
        "observed_simd_chaining_policy",
        "requested_backend",
        "requested_base_alignment_algorithm",
        "requested_chaining_algorithm",
        "requested_minimizer_algorithm",
        "requested_simd_alignment_algorithm",
        "requested_simd_chaining_policy",
        "requested_target_index_source",
    }
    for field in empty_text_fields:
        if _require_text(
                attestation.get(field), f"attestation.{field}") != "":
            raise ContractError("measured connection failure retained a route identity")
    for field in {
            "backend_declines", "backend_fallbacks",
            "persisted_gpu_operation_stats_accepted",
            "stage_engine_matrix_valid", "stats_saturated"}:
        value = attestation.get(field)
        if isinstance(value, bool) or not isinstance(value, int) or value != -1:
            raise ContractError(
                f"measured connection failure attestation.{field} must be -1")
    for field in {
            "validation_accepted", "validation_attached_to_measurement",
            "validation_present"}:
        value = attestation.get(field)
        if isinstance(value, bool) or not isinstance(value, int) or value != 0:
            raise ContractError(
                f"measured connection failure attestation.{field} must be integer 0")
    for field in {
            "documented_minimizer_fallback_selected", "nthash_vector_activity"}:
        if _require_bool(attestation.get(field), f"attestation.{field}"):
            raise ContractError("measured connection failure claimed algorithm work")
    for field in {
            "hash_major_index_reader_stats", "mapq_executed",
            "mapq_execution_available", "mapq_execution_conflict",
            "minimizer_fragment_reader_stats", "prefilter_execution_stats"}:
        if attestation.get(field) is not None:
            raise ContractError(
                f"measured connection failure attestation.{field} must be null")
    if (payload_schema_version == LEGACY_RAW_PAYLOAD_SCHEMA_VERSION
            and attestation.get("vulkan_synthetic_constant_mapq_not_executed")
                is not None):
        raise ContractError(
            "measured connection failure legacy Vulkan MAPQ sentinel must be null")
    block = _require_dict(
        attestation.get("block_aligner_execution_stats"),
        "attestation.block_aligner_execution_stats",
    )
    _require_exact_keys(
        block,
        BLOCK_ALIGNER_EXECUTION_STATS_FIELDS,
        "attestation.block_aligner_execution_stats",
    )
    for field, value in block.items():
        if isinstance(value, bool) or not isinstance(value, int) or value != -1:
            raise ContractError(
                "measured connection failure retained Block Aligner evidence")


def _validate_measured_query_connection_failure(
        record: dict[str, Any],
        status: dict[str, Any],
        identity: dict[str, Any],
        reason: str,
) -> None:
    """Validate one backend replacement without treating it as SQL rejection."""
    if identity["engine"] != "bioseq":
        raise ContractError("minimap2 cannot claim PostgreSQL connection-loss evidence")
    if reason != MEASURED_QUERY_CONNECTION_FAILURE_REASON:
        raise ContractError("measured connection failure reason is not exact")
    diagnostics = _require_dict(status.get("sql_diagnostics"), "SQL diagnostics")
    if (diagnostics.get("result_status") not in {"PGRES_FATAL_ERROR", "null_result"}
            or any(diagnostics.get(field) != ""
                   for field in ("sqlstate", "primary", "detail"))):
        raise ContractError(
            "measured connection failure retained server-error diagnostics")

    postgres = _require_dict(record.get("postgres"), "PostgreSQL evidence")
    _require_exact_keys(
        postgres,
        {"measured_query_connection_failure"},
        "PostgreSQL connection-failure evidence",
    )
    proof = _require_dict(
        postgres.get("measured_query_connection_failure"),
        "measured-query connection-failure proof",
    )
    _require_exact_keys(proof, {
        "schema",
        "version",
        "captured_status",
        "same_postmaster",
        "same_database",
        "changed_live_backend",
        "live_identity_query_succeeded",
        "measured_session",
        "recovered_session",
    }, "measured-query connection-failure proof")
    if (proof.get("schema") != MEASURED_QUERY_CONNECTION_FAILURE_SCHEMA
            or _require_int(
                proof.get("version"), "connection-failure proof version") != 1
            or proof.get("captured_status") != "connection_failure"):
        raise ContractError("measured-query connection-failure proof identity changed")

    identities: dict[str, dict[str, Any]] = {}
    for name in ("measured_session", "recovered_session"):
        session = _require_dict(proof.get(name), f"{name} identity")
        _require_exact_keys(
            session,
            {"postmaster_start", "backend_pid", "database_oid"},
            f"{name} identity",
        )
        identities[name] = {
            "postmaster_start": _require_string(
                session.get("postmaster_start"), f"{name}.postmaster_start"),
            "backend_pid": _require_int(
                session.get("backend_pid"), f"{name}.backend_pid", minimum=1),
            "database_oid": _require_int(
                session.get("database_oid"), f"{name}.database_oid", minimum=1),
        }
    measured = identities["measured_session"]
    recovered = identities["recovered_session"]
    same_postmaster = measured["postmaster_start"] == recovered["postmaster_start"]
    same_database = measured["database_oid"] == recovered["database_oid"]
    changed_backend = measured["backend_pid"] != recovered["backend_pid"]
    if (not _require_bool(proof.get("same_postmaster"), "same_postmaster")
            or not _require_bool(proof.get("same_database"), "same_database")
            or not _require_bool(
                proof.get("changed_live_backend"), "changed_live_backend")
            or not _require_bool(
                proof.get("live_identity_query_succeeded"),
                "live_identity_query_succeeded")
            or not same_postmaster or not same_database or not changed_backend):
        raise ContractError("measured-query connection continuity proof is false")
    if (record.get("alignment_snapshot") is not None
            or record.get("persisted_gpu_operation_stats") is not None):
        raise ContractError("measured connection failure retained execution evidence")
    _validate_unexecuted_measured_connection_attestation(record)


def _normalized_binary_counter(value: Any, label: str) -> int:
    if isinstance(value, bool):
        return int(value)
    parsed = _require_int(value, label)
    if parsed not in (0, 1):
        raise ContractError(f"{label} must be Boolean or integer 0/1")
    return parsed


def _normalized_block_aligner_stats(
        value: Any, label: str) -> dict[str, int]:
    stats = _require_dict(value, label)
    _require_exact_keys(stats, BLOCK_ALIGNER_EXECUTION_STATS_FIELDS, label)
    result: dict[str, int] = {}
    for field in BLOCK_ALIGNER_EXECUTION_STATS_FIELDS:
        field_label = f"{label}.{field}"
        if field in {"available", "consistent", "saturated"}:
            result[field] = _normalized_binary_counter(stats.get(field), field_label)
        else:
            result[field] = _require_int(stats.get(field), field_label)
    return result


_CLOSED_ATTESTATION_SIGNAL = re.compile(
    r"(?:fallback|declin|reject|unsupported|failure|invalid|mismatch|"
    r"conflict|saturat|forbidden|scalar_backend_branch)",
    re.IGNORECASE,
)


def _validate_closed_failure_signals(
        value: Any,
        label: str,
        *,
        allowed_positive_paths: frozenset[str] = frozenset(),
) -> None:
    """Reject every unclassified failure/fallback-like receipt signal.

    Receipt subobjects deliberately remain production-owned schemas. Walking
    them by field name closes the acceptance surface for counters that are not
    individually copied into the aggregation schema, while the one existing
    axis-specific fallback bit can be admitted only by its exact path and is
    checked for transition coherence by ``_validate_accepted_attestation``.
    """
    if isinstance(value, dict):
        for field, child in value.items():
            child_label = f"{label}.{field}"
            if _CLOSED_ATTESTATION_SIGNAL.search(field):
                if child_label in allowed_positive_paths:
                    if child is not True:
                        raise ContractError(
                            f"{child_label} must be the exact positive route proof")
                elif field == "failure_code":
                    if child != "none":
                        raise ContractError(
                            f"{child_label} must be the exact no-failure sentinel")
                elif isinstance(child, bool):
                    if child:
                        raise ContractError(
                            f"accepted attestation has unexplained signal {child_label}")
                elif type(child) is int:
                    if child != 0:
                        raise ContractError(
                            f"accepted attestation has unexplained counter {child_label}")
                elif child not in (None, "", "none"):
                    raise ContractError(
                        f"accepted attestation has unexplained signal {child_label}")
            _validate_closed_failure_signals(
                child,
                child_label,
                allowed_positive_paths=allowed_positive_paths,
            )
    elif isinstance(value, list):
        for ordinal, child in enumerate(value):
            _validate_closed_failure_signals(
                child,
                f"{label}[{ordinal}]",
                allowed_positive_paths=allowed_positive_paths,
            )


def _validated_zero_prefilter_stats(value: Any, label: str) -> dict[str, Any]:
    stats = _require_dict(value, label)
    _require_exact_keys(stats, PREFILTER_EXECUTION_STATS_FIELDS, label)
    if (_require_bool(stats.get("available"), f"{label}.available") is not False
            or _require_bool(stats.get("saturated"), f"{label}.saturated") is not False):
        raise ContractError(
            "accepted Bioseq prefilter stats must prove unavailable and unsaturated")
    for field in PREFILTER_EXECUTION_STATS_FIELDS - {"available", "saturated"}:
        if _require_int(stats.get(field), f"{label}.{field}") != 0:
            raise ContractError(
                f"accepted Bioseq prefilter activity must be zero: {field}")
    return stats


def _validated_alignment_snapshot(
        record: dict[str, Any],
        attestation: dict[str, Any],
        requested: dict[str, str],
        observed: dict[str, str],
        expected_backend: str,
        expected_source: str,
        prefilter_stats: dict[str, Any],
        block_stats: dict[str, int],
) -> dict[str, dict[str, Any]]:
    snapshot = _require_dict(
        record.get("alignment_snapshot"), "accepted alignment snapshot")
    _validate_closed_failure_signals(snapshot, "alignment snapshot")
    _require_exact_keys(snapshot, {
        "schema", "schema_revision", "expected_column_count",
        "expected_row_count", "actual_row_count", "columns", "rows",
    }, "accepted alignment snapshot")
    if (snapshot.get("schema") != "bioseq.last_alignment_execution_stats/raw-rowset"
            or _require_int(
                snapshot.get("schema_revision"), "alignment snapshot revision") != 2
            or _require_int(
                snapshot.get("expected_column_count"),
                "alignment snapshot expected column count") != 151
            or _require_int(
                snapshot.get("expected_row_count"),
                "alignment snapshot expected row count") != len(ALIGNMENT_SNAPSHOT_STAGES)
            or _require_int(
                snapshot.get("actual_row_count"),
                "alignment snapshot actual row count") != len(ALIGNMENT_SNAPSHOT_STAGES)):
        raise ContractError("accepted alignment snapshot is not the exact revision-2 rowset")
    columns = _require_list(snapshot.get("columns"), "alignment snapshot columns")
    rows = _require_list(snapshot.get("rows"), "alignment snapshot rows")
    if len(columns) != 151 or len(rows) != len(ALIGNMENT_SNAPSHOT_STAGES):
        raise ContractError("accepted alignment snapshot cardinality changed")
    column_names: list[str] = []
    for ordinal, raw_column in enumerate(columns):
        column = _require_dict(raw_column, f"alignment snapshot column[{ordinal}]")
        _require_exact_keys(
            column, {"name", "ordinal", "json_type"},
            f"alignment snapshot column[{ordinal}]")
        if _require_int(
                column.get("ordinal"), f"alignment snapshot column[{ordinal}].ordinal") \
                != ordinal:
            raise ContractError("alignment snapshot column ordinals are not canonical")
        column_names.append(_require_string(
            column.get("name"), f"alignment snapshot column[{ordinal}].name"))
        _require_string(
            column.get("json_type"), f"alignment snapshot column[{ordinal}].json_type")
    if len(set(column_names)) != len(column_names):
        raise ContractError("alignment snapshot column names are not unique")
    required_ordinals = {
        "requested_backend": 16,
        "actual_backend": 17,
        "requested_target_index_source": 18,
        "actual_target_index_source": 19,
        "stage": 23,
        "eligible_jobs": 24,
        "executed_host_io_jobs": 25,
        "executed_scalar_cpu_jobs": 26,
        "executed_simd_cpu_jobs": 27,
        "executed_vulkan_gpu_jobs": 28,
        "fallback_jobs": 29,
        "nthash_vector_batches": 94,
        "nthash_scalar_backend_branches": 95,
        "simd_chain_vector_batches": 98,
        "simd_chain_backtrack_vector_batches": 99,
        "simd_chain_scalar_backend_branches": 100,
        "simd_chain_backtrack_scalar_branches": 101,
        "cpu_seed_positions_rejected_outside_collection": 102,
        "backend_declines": 139,
        "backend_fallbacks": 140,
        "are_stats_saturated": 141,
        "simd_execution_stats": 150,
    }
    for name, ordinal in required_ordinals.items():
        if column_names[ordinal] != name:
            raise ContractError(
                f"alignment snapshot required column {ordinal} changed from {name}")

    by_stage: dict[str, dict[str, Any]] = {}
    for row_ordinal, raw_row in enumerate(rows):
        row = _require_list(raw_row, f"alignment snapshot row[{row_ordinal}]")
        if len(row) != len(columns):
            raise ContractError("alignment snapshot row width changed")
        decoded = dict(zip(column_names, row))
        stage = _require_string(
            decoded.get("stage"), f"alignment snapshot row[{row_ordinal}].stage")
        if stage != ALIGNMENT_SNAPSHOT_STAGES[row_ordinal] or stage in by_stage:
            raise ContractError("alignment snapshot stage order/membership changed")
        by_stage[stage] = decoded
        _validate_closed_failure_signals(
            decoded, f"alignment snapshot {stage}")
        for field in (
                "eligible_jobs", "executed_host_io_jobs",
                "executed_scalar_cpu_jobs", "executed_simd_cpu_jobs",
                "executed_vulkan_gpu_jobs", "fallback_jobs",
                "nthash_vector_batches", "nthash_scalar_backend_branches",
                "simd_chain_vector_batches",
                "simd_chain_backtrack_vector_batches",
                "simd_chain_scalar_backend_branches",
                "simd_chain_backtrack_scalar_branches",
                "cpu_seed_positions_rejected_outside_collection",
                "backend_declines", "backend_fallbacks"):
            _require_int(decoded.get(field), f"alignment snapshot {stage}.{field}")
        if decoded.get("requested_backend") != expected_backend \
                or decoded.get("actual_backend") != expected_backend:
            raise ContractError(
                "alignment snapshot backend identity differs from the canonical tuple")
        if decoded.get("requested_target_index_source") != expected_source \
                or decoded.get("actual_target_index_source") != expected_source:
            raise ContractError(
                "alignment snapshot index-source identity differs from the canonical tuple")
        if (decoded["fallback_jobs"] != 0
                or decoded["backend_declines"] != 0
                or decoded["backend_fallbacks"] != 0
                or decoded["nthash_scalar_backend_branches"] != 0
                or decoded["simd_chain_scalar_backend_branches"] != 0
                or decoded["simd_chain_backtrack_scalar_branches"] != 0
                or decoded["cpu_seed_positions_rejected_outside_collection"] != 0
                or _require_bool(
                    decoded.get("are_stats_saturated"),
                    f"alignment snapshot {stage}.are_stats_saturated")):
            raise ContractError(
                "accepted alignment snapshot has unexplained fallback, decline, "
                "rejection, scalar-fallback, or saturation activity")

        simd = _require_dict(
            decoded.get("simd_execution_stats"),
            f"alignment snapshot {stage}.simd_execution_stats")
        _require_exact_keys(
            simd, SIMD_EXECUTION_STATS_FIELDS,
            f"alignment snapshot {stage}.simd_execution_stats")
        if (simd.get("schema") != "bioseq.simd_execution_stats"
                or _require_int(
                    simd.get("revision"), f"alignment snapshot {stage} SIMD revision") != 6):
            raise ContractError("accepted SIMD execution stats are not revision 6")
        for axis in ("minimizer_algorithm", "chaining_algorithm", "base_alignment_algorithm"):
            if (simd.get(f"requested_{axis}") != requested[axis]
                    or simd.get(f"observed_{axis}") != observed[axis]):
                raise ContractError(
                    f"alignment snapshot {stage} {axis} identity contradicts attestation")
        if (simd.get("requested_policy")
                != attestation["requested_simd_chaining_policy"]
                or simd.get("observed_policy")
                != attestation["observed_simd_chaining_policy"]
                or simd.get("requested_alignment_algorithm")
                != attestation["requested_simd_alignment_algorithm"]
                or simd.get("observed_alignment_algorithm")
                != attestation["observed_simd_alignment_algorithm"]):
            raise ContractError(
                "alignment snapshot SIMD policy/alignment identity contradicts attestation")
        if _validated_zero_prefilter_stats(
                simd.get("prefilter_execution_stats"),
                f"alignment snapshot {stage}.prefilter_execution_stats") \
                != prefilter_stats:
            raise ContractError("alignment snapshot prefilter proof changed across stages")
        if _normalized_block_aligner_stats(
                simd.get("block_aligner_execution_stats"),
                f"alignment snapshot {stage}.block_aligner_execution_stats") \
                != block_stats:
            raise ContractError("alignment snapshot block-aligner proof changed across stages")
        for field in (
                "simd_chaining_policy_rejections",
                "simd_chaining_exact_policy_calls"):
            if _require_int(
                    simd.get(field), f"alignment snapshot {stage}.{field}") != 0:
                raise ContractError(
                    f"accepted alignment snapshot has unexplained {field}")
        if _require_bool(
                simd.get("are_simd_chaining_policy_stats_saturated"),
                f"alignment snapshot {stage}.SIMD policy saturation"):
            raise ContractError("accepted SIMD chaining-policy stats are saturated")
        if (simd.get("mapq_execution_available")
                != attestation["mapq_execution_available"]
                or simd.get("mapq_executed") != attestation["mapq_executed"]
                or simd.get("mapq_execution_conflict")
                != attestation["mapq_execution_conflict"]):
            raise ContractError("alignment snapshot MAPQ proof contradicts attestation")
    return by_stage


def _require_stage_route(
        rows: dict[str, dict[str, Any]],
        stage: str,
        engine: str,
        *,
        require_positive: bool = True,
) -> bool:
    row = rows[stage]
    engine_fields = {
        "host_io": "executed_host_io_jobs",
        "scalar_cpu": "executed_scalar_cpu_jobs",
        "simd_cpu": "executed_simd_cpu_jobs",
        "vulkan_gpu": "executed_vulkan_gpu_jobs",
    }
    selected_field = engine_fields[engine]
    eligible = row["eligible_jobs"]
    if row[selected_field] != eligible or any(
            row[field] != 0 for candidate, field in engine_fields.items()
            if candidate != engine):
        raise ContractError(
            f"accepted {stage} work does not exclusively prove observed engine {engine}")
    if require_positive and eligible <= 0:
        raise ContractError(
            f"accepted {stage} has no positive work for observed engine {engine}")
    return eligible > 0


def _validate_selected_vulkan_stage_receipts(
        rows: dict[str, dict[str, Any]],
        vulkan_seed: bool,
        vulkan_chain: bool,
) -> None:
    """Validate existing physical GPU/cache counters, without an all-GPU suffix."""
    if not (vulkan_seed or vulkan_chain):
        raise ContractError("hybrid Vulkan evidence has no selected GPU stage")
    shared_fields = (
        "gpu_device_uuid", "gpu_driver", "gpu_pipeline_cache_policy",
        "gpu_pipeline_cache_seed_sha256", "gpu_pipeline_cache_seed_size",
        "gpu_pipeline_cache_manifest_checksum", "gpu_pipeline_cache_hit",
        "gpu_pipeline_cache_loads", "gpu_pipeline_cache_seed_bytes",
        "gpu_pipeline_cache_load_ns", "gpu_pipeline_creation_ns",
        "gpu_pipeline_creates", "gpu_timestamp_query_failures",
        "gpu_seed_dispatches", "gpu_chain_dispatches",
        "gpu_chain_backtrack_dispatches", "gpu_extend_dispatches",
        "gpu_target_window_request_dispatches",
    )
    first = rows["query_sketch"]
    for field, digits in (
            ("gpu_device_uuid", 32), ("gpu_pipeline_cache_seed_sha256", 64),
            ("gpu_pipeline_cache_manifest_checksum", 16)):
        encoded = _require_hex(first.get(field), field, digits)
        if int(encoded, 16) == 0:
            raise ContractError(f"hybrid Vulkan evidence has an empty {field}")
    _require_string(first.get("gpu_driver"), "hybrid Vulkan driver")
    if first.get("gpu_pipeline_cache_policy") != "immutable_preseed_all_hit" \
            or _require_bool(first.get("gpu_pipeline_cache_hit"), "GPU cache hit") is not True:
        raise ContractError("hybrid Vulkan execution lacks immutable all-hit cache proof")
    seed_size = _require_int(
        first.get("gpu_pipeline_cache_seed_size"), "GPU cache seed size", minimum=1)
    if (_require_int(first.get("gpu_pipeline_cache_loads"), "GPU cache loads") != 1
            or _require_int(first.get("gpu_pipeline_cache_seed_bytes"), "GPU cache bytes")
                != seed_size):
        raise ContractError("hybrid Vulkan cache load was not accounted exactly once")
    for field in ("gpu_pipeline_cache_load_ns", "gpu_pipeline_creation_ns", "gpu_pipeline_creates"):
        _require_int(first.get(field), field, minimum=1)
    for field, selected in (("gpu_seed_dispatches", vulkan_seed),
                            ("gpu_chain_dispatches", vulkan_chain)):
        dispatches = _require_int(first.get(field), field)
        if (dispatches > 0) != selected:
            raise ContractError(f"selected Vulkan stage contradicts physical {field}")
    for field in ("gpu_timestamp_query_failures", "gpu_chain_backtrack_dispatches",
                  "gpu_extend_dispatches", "gpu_target_window_request_dispatches"):
        if _require_int(first.get(field), field) != 0:
            raise ContractError(f"hybrid Vulkan execution has forbidden {field}")
    for stage, row in rows.items():
        for field in shared_fields:
            if row.get(field) != first[field]:
                raise ContractError(f"hybrid Vulkan {field} changed across snapshot stages")
        device_ns = _require_int(row.get("stage_gpu_device_ns"), f"{stage}.stage_gpu_device_ns")
        if row["executed_vulkan_gpu_jobs"] == 0 and device_ns != 0:
            raise ContractError(f"host-only stage {stage} contains GPU device time")
        permitted_gpu_stage = (
            vulkan_seed and stage in {
                "query_sketch", "target_sketch", "seed_lookup", "device_index_upload"}
        ) or (vulkan_chain and stage == "chain_score")
        if not permitted_gpu_stage and row["executed_vulkan_gpu_jobs"] != 0:
            raise ContractError(f"unselected GPU stage {stage} executed")


def _validate_hybrid_vulkan_legacy_gpu_ledger(
        record: dict[str, Any],
        attestation: dict[str, Any],
) -> None:
    """Require the legacy all-GPU ledger to be exactly empty for a hybrid route."""
    if _require_int(
            attestation.get("persisted_gpu_operation_stats_accepted"),
            "attestation.persisted_gpu_operation_stats_accepted") != 0:
        raise ContractError(
            "accepted hybrid Vulkan row claims an accepted legacy all-GPU ledger")

    stats = _require_dict(
        record.get("persisted_gpu_operation_stats"),
        "hybrid Vulkan legacy GPU operation ledger")
    fields = {
        "schema", "revision", "execution_id", "backend_pid", "database_oid",
        "is_completed", "is_partial", "is_fully_drained", "is_consistent",
        "is_complete", "is_saturated", "dropped_operation_count",
        "operation_count", "accepted_operation_count", "complete_operation_count",
        "job_count", "accepted_job_count", "cache_validation", "operations",
    }
    _require_exact_keys(stats, fields, "hybrid Vulkan legacy GPU operation ledger")
    if (stats.get("schema") != "bioseq.persisted_gpu_operation_stats"
            or _require_int(
                stats.get("revision"),
                "hybrid Vulkan legacy GPU operation ledger revision") != 3):
        raise ContractError(
            "hybrid Vulkan legacy GPU operation ledger has the wrong schema revision")
    for field in ("execution_id", "backend_pid", "database_oid"):
        _require_int(
            stats.get(field),
            f"hybrid Vulkan legacy GPU operation ledger.{field}",
            minimum=1)
    lifecycle = {
        "is_completed": True,
        "is_partial": False,
        "is_fully_drained": True,
        "is_consistent": False,
        "is_complete": True,
        "is_saturated": False,
    }
    for field, expected in lifecycle.items():
        if _require_bool(
                stats.get(field),
                f"hybrid Vulkan legacy GPU operation ledger.{field}") is not expected:
            raise ContractError(
                "hybrid Vulkan legacy GPU operation ledger lifecycle is not exact")
    for field in (
            "dropped_operation_count", "operation_count",
            "accepted_operation_count", "complete_operation_count",
            "job_count", "accepted_job_count"):
        if _require_int(
                stats.get(field),
                f"hybrid Vulkan legacy GPU operation ledger.{field}") != 0:
            raise ContractError(
                "hybrid Vulkan route leaked legacy all-GPU operation work")
    if (stats.get("cache_validation") is not None
            or _require_list(
                stats.get("operations"),
                "hybrid Vulkan legacy GPU operation ledger.operations")):
        raise ContractError(
            "hybrid Vulkan route retained legacy all-GPU operation evidence")


def _validate_revision6_operation_origins(
        record: dict[str, Any],
        observed_base: str,
        expected_query_count: int,
        mapq_projection: bool,
        expect_mapq_execution: bool,
) -> None:
    receipt = _require_dict(record.get("interval_receipt"), "Bioseq interval receipt")
    origins = _require_list(receipt.get("origins"), "Bioseq interval origins")
    origin_fields = {
        "logical_target_row", "query_ordinal", "kind", "algorithm",
        "inclusive_work", "interval_count",
    }
    ordinals: dict[str, set[int]] = {"cigar": set(), "mapq": set()}
    counts: dict[str, int] = {"cigar": 0, "mapq": 0}
    logical_target_rows: set[int] = set()
    cigar_work = {
        "ksw2": (
            "alignment_dp_trace_storage_traceback_cigar_assembly_"
            "result_materialization_inclusive"),
        "block_aligner": (
            "block_dp_trace_storage_traceback_cigar_assembly_"
            "result_materialization_inclusive"),
        "vulkan_simple_cigar": (
            "host_packing_upload_gpu_alignment_traceback_wait_readback_decode_"
            "result_materialization_inclusive"),
    }
    for raw_origin in origins:
        origin = _require_dict(raw_origin, "Bioseq interval origin")
        _require_exact_keys(origin, origin_fields, "Bioseq interval origin")
        kind = _require_string(origin.get("kind"), "Bioseq interval origin kind")
        if kind not in ordinals:
            raise ContractError("Bioseq interval receipt has an unknown operation kind")
        ordinal = _require_int(
            origin.get("query_ordinal"), "Bioseq interval origin query ordinal")
        if ordinal >= expected_query_count or ordinal in ordinals[kind]:
            raise ContractError("Bioseq interval origin query membership is not exact")
        ordinals[kind].add(ordinal)
        logical_target_rows.add(_require_int(
            origin.get("logical_target_row"),
            "Bioseq interval origin logical target row", minimum=1))
        inclusive_work = _require_string(
            origin.get("inclusive_work"), "Bioseq interval origin work scope")
        counts[kind] += _require_int(
            origin.get("interval_count"),
            "Bioseq interval origin interval count", minimum=1)
        algorithm = _require_string(
            origin.get("algorithm"), "Bioseq interval origin algorithm")
        expected_algorithm = (
            observed_base if kind == "cigar" else "cpu_mapping_quality")
        expected_work = (
            cigar_work.get(observed_base)
            if kind == "cigar" else "compute_mapping_quality_only")
        if (algorithm != expected_algorithm or inclusive_work != expected_work
                or "ksw2_batched" in algorithm):
            raise ContractError(
                f"Bioseq {kind} origin does not prove the observed operation")
    expected_ordinals = set(range(expected_query_count))
    if len(logical_target_rows) != 1:
        raise ContractError("Bioseq operation origins span multiple logical target rows")
    if ordinals["cigar"] != expected_ordinals:
        raise ContractError("CIGAR origins do not cover every canonical query exactly once")
    expected_mapq_ordinals = expected_ordinals if expect_mapq_execution else set()
    if ordinals["mapq"] != expected_mapq_ordinals:
        raise ContractError("MAPQ origins contradict the attested execution classification")
    if (counts["cigar"] != receipt.get("cigar_interval_count")
            or counts["mapq"] != receipt.get("mapq_interval_count")
            or counts["cigar"] + counts["mapq"] != receipt.get("interval_count")):
        raise ContractError("Bioseq operation-origin interval counts are inconsistent")
    if not mapq_projection or not expect_mapq_execution and counts["mapq"] != 0:
        raise ContractError("Bioseq MAPQ origins contradict the sealed projection contract")


def _validate_ordered_query_oracles(
        record: dict[str, Any], expected_query_count: int,
) -> dict[str, Any] | None:
    """Recompute the existing embedded count/MAPQ digests; MAPQ is diagnostic."""
    count = record.get("ordered_query_oracle")
    mapq = record.get("mapq_diagnostic")
    if count is None and mapq is None:
        return None
    count = _require_dict(count, "ordered query oracle")
    mapq = _require_dict(mapq, "MAPQ diagnostic")
    common = {
        "schema", "version", "ordered_query_count", "fields", "digest_domain",
        "digest_algorithm", "digest", "digest_words", "diagnostic",
    }
    _require_exact_keys(count, common | {
        "ordered_observations", "raw_observations_embedded", "repeat_ordinal",
        "direct_comparison_available", "direct_comparison_exact", "validity_scope",
        "mapq_participates_in_validity", "pair_verdict_sealed",
    }, "ordered query oracle")
    _require_exact_keys(mapq, common | {
        "ordered_vector", "raw_vector_embedded", "numeric_vector_equal",
        "numeric_equality_is_validity_gate", "minimap2_mapq_semantics_claimed",
    }, "MAPQ diagnostic")
    vectors: list[tuple[int, ...]] = []
    for blob, field, vector_key, schema, version, domain, embedded in (
        (count, "primary_match_count", "ordered_observations",
         "bioseq.e2e.ordered-query-primary-count-oracle", 2,
         "bioseq.e2e.ordered-query-primary-count.v2", "raw_observations_embedded"),
        (mapq, "mapq", "ordered_vector",
         "bioseq.e2e.ordered-query-mapq-diagnostic", 1,
         "bioseq.e2e.ordered-query-mapq-diagnostic.v1", "raw_vector_embedded"),
    ):
        if (blob.get("schema") != schema
                or _require_int(blob.get("version"), "oracle version") != version
                or blob.get("fields") != ["query_id", field]
                or blob.get("digest_domain") != domain
                or blob.get("digest_algorithm") != "sha256"
                or blob.get(embedded) is not True
                or _require_int(blob.get("ordered_query_count"), "oracle count")
                    != expected_query_count):
            raise ContractError("ordered oracle schema/shape/domain is contradictory")
        observations = _require_list(blob.get(vector_key), "ordered oracle vector")
        if len(observations) != expected_query_count:
            raise ContractError("ordered oracle vector has the wrong query count")
        hasher = hashlib.sha256(domain.encode("ascii"))
        hasher.update(expected_query_count.to_bytes(8, "little"))
        values: list[int] = []
        for ordinal, value in enumerate(observations):
            observation = _require_dict(value, "ordered oracle observation")
            _require_exact_keys(observation, {"query_id", field}, "oracle observation")
            if _require_int(observation.get("query_id"), "oracle query id") != ordinal:
                raise ContractError("ordered oracle query ids are not canonical 0..N-1")
            numeric = _require_int(
                observation.get(field), field, minimum=-1 if field == "mapq" else 0)
            maximum = 255 if field == "mapq" else (1 << 64) - 1
            if numeric > maximum:
                raise ContractError("ordered oracle value exceeds its physical domain")
            values.append(numeric)
            hasher.update(ordinal.to_bytes(8, "little"))
            hasher.update((numeric if numeric >= 0 else (1 << 64) - 1).to_bytes(8, "little"))
        digest = hasher.hexdigest()
        if _require_hex(blob.get("digest"), "ordered oracle digest", 64) != digest:
            raise ContractError("ordered oracle digest does not authenticate its vector")
        words = _require_dict(blob.get("digest_words"), "ordered oracle digest words")
        _require_exact_keys(words, {f"word_{word}" for word in range(8)}, "oracle words")
        if any(_require_int(words[f"word_{word}"], "oracle digest word")
               != int(digest[word * 8:(word + 1) * 8], 16) for word in range(8)):
            raise ContractError("ordered oracle digest words use the wrong byte order/value")
        vectors.append(tuple(values))
    repeat = _require_int(record.get("repeat"), "oracle framework repeat")
    sealed = count.get("pair_verdict_sealed") is True
    if (count.get("validity_scope") != (
                "exact ordered query-id and primary-match-count equality "
                "plus exact total primary-match-count equality")
            or count.get("mapq_participates_in_validity") is not False
            or mapq.get("numeric_equality_is_validity_gate") is not False
            or mapq.get("minimap2_mapq_semantics_claimed") is not False):
        raise ContractError("ordered oracle changed its count-only validity scope")
    status = _require_dict(record.get("tuple_status"), "oracle tuple status")
    accepted = _require_int(status.get("accepted"), "oracle tuple acceptance")
    if accepted not in (0, 1):
        raise ContractError("oracle tuple acceptance must be integer 0 or 1")
    exact: bool | None
    equal_mapq: bool | None
    diagnostic: str | None
    if sealed:
        if (count.get("direct_comparison_available") is not True
                or _require_int(
                    count.get("repeat_ordinal"), "oracle repeat") != repeat):
            raise ContractError(
                "ordered oracle lacks its sealed same-repeat ownership")
        exact = _require_bool(
            count.get("direct_comparison_exact"), "paired count equality")
        equal_mapq = _require_bool(
            mapq.get("numeric_vector_equal"), "diagnostic MAPQ equality")
        if exact:
            if count.get("diagnostic") is not None or accepted != 1:
                raise ContractError(
                    "exact paired verdict contradicts tuple acceptance")
            if any(value < 0 for value in vectors[1]):
                raise ContractError("accepted paired row has missing MAPQ output")
        elif (accepted != 0 or status.get("disposition") != "invalid"
              or status.get("phase") != "paired_oracle"
              or not _require_string(
                  count.get("diagnostic"), "paired invalid diagnostic")
                  .startswith("**INVALID**: paired minimap2 oracle ")
              or count.get("diagnostic") != status.get("rejection_reason")):
            raise ContractError(
                "failed paired verdict is not an explicitly INVALID pair member")
        diagnostic = _require_string(
            mapq.get("diagnostic"), "MAPQ diagnostic text")
    else:
        if (count.get("direct_comparison_available") is not False
                or count.get("direct_comparison_exact") is not None
                or count.get("repeat_ordinal") is not None
                or count.get("diagnostic") is not None
                or mapq.get("numeric_vector_equal") is not None
                or mapq.get("diagnostic") is not None
                or any(value != -1 for value in vectors[1])):
            raise ContractError(
                "ordinary ordered vector falsely claims an in-process paired verdict")
        exact = None
        equal_mapq = None
        diagnostic = None
    correctness = _require_dict(record.get("correctness"), "oracle correctness")
    if sum(vectors[0]) != _require_int(correctness.get("match_count"), "oracle total matches"):
        raise ContractError("ordered count vector contradicts total match count")
    return {"counts": vectors[0], "mapq": vectors[1], "sealed": sealed,
            "exact": exact,
            "mapq_equal": equal_mapq, "mapq_comment": diagnostic,
            "count_digest": count["digest"], "mapq_digest": mapq["digest"],
            "invalid_reason": count["diagnostic"]}


def _validate_paired_repeat_oracles(
        records: list[dict[str, Any]], paired_cases: dict[str, bool],
) -> None:
    """Bind both sealed vectors to actual members of one report/group/repeat."""
    pairs: dict[tuple[str, int, int], list[dict[str, Any]]] = {}
    for item in records:
        if item["test_case"] not in paired_cases:
            raise ContractError("paired row refers to an unconfigured test case")
        paired = paired_cases[item["test_case"]]
        oracle = item["ordered_oracle"]
        if paired != (oracle is not None and oracle["sealed"]):
            raise ContractError(
                "sealed paired oracle ownership differs from the execution config")
        if (not paired
                and item.get("payload_schema_version",
                             LEGACY_RAW_PAYLOAD_SCHEMA_VERSION)
                    == RAW_PAYLOAD_SCHEMA_VERSION
                and item["accepted"]
                and (oracle is None or oracle["sealed"])):
            raise ContractError(
                "current-schema ordinary row lacks its unsealed ordered "
                "count vector")
        if paired:
            key = (item["test_case"], item["group_index"], item["repeat"])
            pairs.setdefault(key, []).append(item)
    for key, members in pairs.items():
        if len(members) != 2 or {item["identity"]["engine"] for item in members} != {
                "bioseq", "minimap2"}:
            raise ContractError("paired repeat lacks exactly one Bioseq and one minimap2 row")
        bioseq, minimap2 = sorted(members, key=lambda item: item["identity"]["engine"])
        if (bioseq["payload_schema_version"]
                != minimap2["payload_schema_version"]):
            raise ContractError("paired repeat members use different payload schemas")
        if (bioseq["payload_schema_version"] == RAW_PAYLOAD_SCHEMA_VERSION
                and ("pairing_evidence" in bioseq
                     or "pairing_evidence" in minimap2)):
            if (bioseq.get("pairing_evidence") is None
                    or bioseq.get("pairing_evidence")
                        != minimap2.get("pairing_evidence")):
                raise ContractError(
                    "paired repeat members carry different target/setup evidence")
        for field in ("campaign_id", "dataset", "index_mode"):
            if bioseq["identity"][field] != minimap2["identity"][field]:
                raise ContractError("paired repeat members refer to different inputs/modes")
        left, right = bioseq["ordered_oracle"], minimap2["ordered_oracle"]
        if (left["exact"] != right["exact"]
                or left["invalid_reason"] != right["invalid_reason"]):
            raise ContractError("paired repeat members carry different count verdicts")
        if left["exact"] and (left["counts"] != right["counts"]
                              or bioseq["match_count"] != minimap2["match_count"]):
            raise ContractError("**INVALID**: paired minimap2 ordered/total match counts differ")
        equal_mapq = left["mapq"] == right["mapq"]
        if left["mapq_equal"] != equal_mapq or right["mapq_equal"] != equal_mapq:
            raise ContractError("diagnostic MAPQ flag contradicts the actual paired vectors")
        repeat = key[2]
        if equal_mapq:
            expected_comment = (
                f"diagnostic MAPQ vectors happen to match at repeat={repeat}; this does "
                "not claim minimap2 MAPQ semantics")
        else:
            ordinal = next(index for index, values in enumerate(zip(left["mapq"], right["mapq"]))
                           if values[0] != values[1])
            expected_comment = (
                f"diagnostic MAPQ vector differs at repeat={repeat} ordered_index={ordinal} "
                f"bioseq=(query_id={ordinal},mapq={left['mapq'][ordinal]}) "
                f"minimap2=(query_id={ordinal},mapq={right['mapq'][ordinal]}) "
                f"bioseq_mapq_sha256={left['mapq_digest']} "
                f"minimap2_mapq_sha256={right['mapq_digest']}; numeric "
                "minimap2 MAPQ equivalence is not a validity requirement")
        if any(oracle["mapq_comment"] != expected_comment for oracle in (left, right)):
            raise ContractError("MAPQ comment does not describe the actual paired vectors")


def _validate_accepted_behavior(
        record: dict[str, Any], disposition: str, behavior_comment: str,
        documented_fallback: bool,
) -> None:
    """Keep a numerical MAPQ diagnostic separate from selected-algorithm fallback."""
    expected_comment = (
        "simd_minimap2_reference completed through the operation-observed "
        "scalar minimizer route" if documented_fallback else "")
    expected_disposition = "accepted_fallback" if documented_fallback else "accepted"
    mapq = record.get("mapq_diagnostic")
    if isinstance(mapq, dict) and mapq.get("numeric_vector_equal") is False:
        diagnostic = _require_string(mapq.get("diagnostic"), "MAPQ behavior diagnostic")
        expected_comment += ("; " if expected_comment else "") + diagnostic
        if not documented_fallback:
            expected_disposition = "accepted_behavior_difference"
    if disposition != expected_disposition or behavior_comment != expected_comment:
        raise ContractError(
            "accepted disposition/comment does not report its exact route classification")


def _validate_accepted_attestation(
        record: dict[str, Any],
        identity: dict[str, Any],
        disposition: str,
        behavior_comment: str,
        expected_query_count: int,
        cigar_projection: bool,
        mapq_projection: bool,
        prefilter_enabled: bool,
        payload_schema_version: int | None = None,
) -> tuple[dict[str, Any], dict[str, str]]:
    attestation = _require_dict(record.get("attestation"), "attestation")
    payload_schema_version = _validate_attestation_shape(
        record, attestation, payload_schema_version)
    if identity["engine"] == "minimap2":
        baseline_sentinel = {
            "actual_backend": "",
            "actual_target_index_source": "",
            "backend_declines": -1,
            "backend_fallbacks": -1,
            "block_aligner_execution_stats": {
                field: -1 for field in BLOCK_ALIGNER_EXECUTION_STATS_FIELDS
            },
            "documented_minimizer_fallback_selected": False,
            "hash_major_index_reader_stats": None,
            "mapq_executed": None,
            "mapq_execution_available": None,
            "mapq_execution_conflict": None,
            "minimizer_fragment_reader_stats": None,
            "nthash_vector_activity": False,
            "observed_base_alignment_algorithm": "",
            "observed_chaining_algorithm": "",
            "observed_minimizer_algorithm": "",
            "observed_simd_alignment_algorithm": "",
            "observed_simd_chaining_policy": "",
            "persisted_gpu_operation_stats_accepted": None,
            "prefilter_execution_stats": None,
            "requested_backend": "",
            "requested_base_alignment_algorithm": "",
            "requested_chaining_algorithm": "",
            "requested_minimizer_algorithm": "",
            "requested_simd_alignment_algorithm": "",
            "requested_simd_chaining_policy": "",
            "requested_target_index_source": "",
            "stage_engine_matrix_valid": -1,
            "stats_saturated": -1,
            "validation_accepted": 0,
            "validation_attached_to_measurement": 0,
            "validation_present": 0,
        }
        if payload_schema_version == LEGACY_RAW_PAYLOAD_SCHEMA_VERSION:
            baseline_sentinel["vulkan_synthetic_constant_mapq_not_executed"] = None
        else:
            baseline_sentinel.update({
                "numeric_mapq_matches_minimap2_semantics_claimed": False,
                "vulkan_physical_mapq_executed": None,
            })
        if attestation != baseline_sentinel or record.get("alignment_snapshot") is not None:
            raise ContractError(
                "accepted minimap2 baseline has non-baseline Bioseq attestation")
        _validate_accepted_behavior(record, disposition, behavior_comment, False)
        return {
            "observed_minimizer_algorithm": None,
            "observed_chaining_algorithm": None,
            "observed_base_alignment_algorithm": None,
        }, {
            "minimizer": "not_applicable",
            "chaining": "not_applicable",
            "base_alignment": "not_applicable",
        }

    if prefilter_enabled is not False:
        raise ContractError("accepted Bioseq records require sealed prefilter=false")
    if cigar_projection is not True or mapq_projection is not True:
        raise ContractError("algorithm-matrix accepted records require CIGAR+MAPQ projection")
    requested = {
        "minimizer_algorithm": identity["requested_minimizer_algorithm"],
        "chaining_algorithm": identity["requested_chaining_algorithm"],
        "base_alignment_algorithm": identity["requested_base_alignment_algorithm"],
    }
    observed = {
        "minimizer_algorithm": attestation.get("observed_minimizer_algorithm"),
        "chaining_algorithm": attestation.get("observed_chaining_algorithm"),
        "base_alignment_algorithm": attestation.get("observed_base_alignment_algorithm"),
    }
    for axis, requested_value in requested.items():
        if attestation.get(f"requested_{axis}") != requested_value:
            raise ContractError(
                f"attested requested {axis} differs from sealed canonical identity")
        _require_string(observed[axis], f"attested observed {axis}")
    relevant_algorithm_names = [*requested.values(), *observed.values()]
    if any("ksw2_batched" in value for value in relevant_algorithm_names):
        raise ContractError("ksw2_batched is excluded from algorithm-matrix evidence")

    if (payload_schema_version == RAW_PAYLOAD_SCHEMA_VERSION
            and observed["base_alignment_algorithm"]
                == "vulkan_simple_cigar"):
        interval_receipt = _require_dict(
            record.get("interval_receipt"), "Vulkan MAPQ interval receipt")
        expected_physical_mapq = (
            mapq_projection
            and _require_bool(
                attestation.get("mapq_execution_available"),
                "attestation.mapq_execution_available")
            and _require_bool(
                attestation.get("mapq_executed"),
                "attestation.mapq_executed")
            and not _require_bool(
                attestation.get("mapq_execution_conflict"),
                "attestation.mapq_execution_conflict")
            and _require_int(
                interval_receipt.get("mapq_interval_count"),
                "Vulkan MAPQ interval count") > 0
        )
        if attestation.get("vulkan_physical_mapq_executed") \
                is not expected_physical_mapq:
            raise ContractError(
                "Vulkan physical MAPQ attestation contradicts execution evidence")

    vulkan_seed = requested["minimizer_algorithm"] == "vulkan_minimap_integer"
    vulkan_chain = requested["chaining_algorithm"] == "vulkan_tiled_dynamic_programming"
    hybrid_vulkan = vulkan_seed or vulkan_chain
    if requested["base_alignment_algorithm"] not in {"block_aligner", "ksw2"}:
        raise ContractError("algorithm-matrix evidence requires the selected host aligner")
    all_scalar = requested == {
        "minimizer_algorithm": "scalar",
        "chaining_algorithm": "scalar_minimap2",
        "base_alignment_algorithm": "ksw2",
    }
    all_simd = (
        requested["minimizer_algorithm"]
            in {"two_stack_nthash", "bounded_nthash_simd"}
        and requested["chaining_algorithm"] == "simd_minimap2"
        and requested["base_alignment_algorithm"] == "block_aligner")
    expected_backend = (
        "hybrid_cpu_vulkan" if hybrid_vulkan else
        "scalar_cpu" if all_scalar else
        "simd_cpu" if all_simd else
        "mixed_cpu")
    expected_source = (
        "persisted" if identity["index_mode"] == "just_index" else "recomputed")
    if (attestation.get("requested_backend") != expected_backend
            or attestation.get("actual_backend") != expected_backend
            or attestation.get("requested_target_index_source") != expected_source
            or attestation.get("actual_target_index_source") != expected_source):
        raise ContractError(
            "accepted backend/index-source attestation contradicts canonical identity")
    if hybrid_vulkan:
        _validate_hybrid_vulkan_legacy_gpu_ledger(record, attestation)
    for field in (
            "validation_present", "validation_attached_to_measurement",
            "validation_accepted", "stage_engine_matrix_valid"):
        if _require_int(attestation.get(field), f"attestation.{field}") != 1:
            raise ContractError(f"accepted Bioseq attestation lacks positive {field}")
    if (_require_int(attestation.get("stats_saturated"), "attestation.stats_saturated") != 0
            or _require_int(
                attestation.get("backend_declines"), "attestation.backend_declines") != 0
            or _require_int(
                attestation.get("backend_fallbacks"), "attestation.backend_fallbacks") != 0):
        raise ContractError(
            "accepted Bioseq attestation has unexplained decline/fallback/saturation activity")

    prefilter_stats = _validated_zero_prefilter_stats(
        attestation.get("prefilter_execution_stats"),
        "attestation.prefilter_execution_stats")
    block_stats = _normalized_block_aligner_stats(
        attestation.get("block_aligner_execution_stats"),
        "attestation.block_aligner_execution_stats")
    documented_fallback = _require_bool(
        attestation.get("documented_minimizer_fallback_selected"),
        "attestation.documented_minimizer_fallback_selected")
    allowed_positive_paths = frozenset({
        "attestation.documented_minimizer_fallback_selected",
    }) if documented_fallback else frozenset()
    _validate_closed_failure_signals(
        attestation,
        "attestation",
        allowed_positive_paths=allowed_positive_paths,
    )
    for name, fields in (
        ("hash_major_index_reader_stats", (
            "backend_fallbacks", "generic_source_fallback_calls", "failures")),
        ("minimizer_fragment_reader_stats", (
            "fragment_native_compact_direct_fallbacks",
            "fragment_native_compact_direct_rejected_posting_reads",
            "fragment_native_rejected_control_validation_bytes",
            "fragment_native_rejected_control_validation_ns")),
    ):
        receipt_stats = _require_dict(attestation.get(name), f"attestation.{name}")
        for field in fields:
            if _require_int(
                    receipt_stats.get(field), f"attestation.{name}.{field}") != 0:
                raise ContractError(
                    f"accepted Bioseq source receipt has unexplained {name}.{field}")

    transitions = {
        "minimizer": "exact",
        "chaining": "exact",
        "base_alignment": "exact",
    }
    if observed["minimizer_algorithm"] != requested["minimizer_algorithm"]:
        if not (
            requested["minimizer_algorithm"] == "simd_minimap2_reference"
            and observed["minimizer_algorithm"] == "scalar"
            and documented_fallback
        ):
            raise ContractError(
                "minimizer requested-to-observed transition lacks an exact "
                "instrumented classification")
        transitions["minimizer"] = "documented_minimizer_fallback"
    elif documented_fallback:
        raise ContractError(
            "exact minimizer identity contradicts documented fallback classification")
    if observed["chaining_algorithm"] != requested["chaining_algorithm"]:
        raise ContractError(
            "chaining requested-to-observed transition is not representable by "
            "the current axis-specific receipt schema")
    if observed["base_alignment_algorithm"] != requested["base_alignment_algorithm"]:
        raise ContractError(
            "base-aligner requested-to-observed transition lacks exact route proof")

    _validate_accepted_behavior(
        record, disposition, behavior_comment, transitions["minimizer"] != "exact")

    block_selected = observed["base_alignment_algorithm"] == "block_aligner"
    if block_selected:
        if not (
            block_stats["available"] == 1
            and block_stats["consistent"] == 1
            and block_stats["executed_calls"] > 0
            and block_stats["nonempty_calls"] > 0
            and block_stats["nonempty_calls_with_simd"]
                == block_stats["nonempty_calls"]
            and block_stats["simd_dp_vector_batches"] > 0
            and block_stats["simd_dp_cells"] > 0
            and block_stats["forbidden_ksw2_calls"] == 0
            and block_stats["forbidden_scalar_biological_fallbacks"] == 0
            and block_stats["saturated"] == 0
        ):
            raise ContractError(
                "observed block_aligner lacks positive SIMD work/forbidden-path proof")
    elif any(block_stats.values()):
        raise ContractError(
            "non-block-aligner route has contradictory block-aligner execution proof")

    expect_simd_chaining = observed["chaining_algorithm"] == "simd_minimap2"
    expected_policy = "bounded_batch_approximate" if expect_simd_chaining else "none"
    if (attestation.get("requested_simd_chaining_policy") != expected_policy
            or attestation.get("observed_simd_chaining_policy") != expected_policy):
        raise ContractError("SIMD chaining policy does not match the selected route")
    expected_simd_alignment = "block_aligner" if block_selected else "none"
    if (attestation.get("requested_simd_alignment_algorithm")
            != expected_simd_alignment
            or attestation.get("observed_simd_alignment_algorithm")
            != expected_simd_alignment):
        raise ContractError("SIMD alignment identity does not match the observed route")

    rows = _validated_alignment_snapshot(
        record, attestation, requested, observed, expected_backend,
        expected_source, prefilter_stats, block_stats)
    if hybrid_vulkan:
        _validate_selected_vulkan_stage_receipts(rows, vulkan_seed, vulkan_chain)
    minimizer_engine = (
        "vulkan_gpu" if vulkan_seed else
        "scalar_cpu" if observed["minimizer_algorithm"] == "scalar" else
        "simd_cpu")
    _require_stage_route(rows, "query_sketch", minimizer_engine)
    _require_stage_route(
        rows, "seed_lookup", "vulkan_gpu" if vulkan_seed else "scalar_cpu")
    chaining_engine = (
        "vulkan_gpu" if vulkan_chain else
        "simd_cpu" if expect_simd_chaining else
        "scalar_cpu")
    _require_stage_route(rows, "chain_score", chaining_engine)
    _require_stage_route(
        rows, "chain_backtrack", "scalar_cpu" if vulkan_chain else chaining_engine)
    alignment_engine = (
        "simd_cpu" if block_selected else
        "scalar_cpu")
    extension_work = [
        _require_stage_route(
            rows, stage, alignment_engine, require_positive=False)
        for stage in ("left_extension", "gap_extension", "right_extension")
    ]
    if not any(extension_work):
        raise ContractError("observed base aligner has no positive extension-stage work")

    simd_stats = rows["query_sketch"]["simd_execution_stats"]
    nthash_active = _require_bool(
        attestation.get("nthash_vector_activity"),
        "attestation.nthash_vector_activity")
    nthash_work = (
        _require_int(simd_stats.get("nthash_vector_batches"), "nthash vector batches")
        + _require_int(
            simd_stats.get("nthash_resident_vector_stream_kernel_calls"),
            "nthash resident vector calls"))
    expects_nthash = observed["minimizer_algorithm"] in {
        "two_stack_nthash", "bounded_nthash_simd"}
    if nthash_active != (nthash_work > 0) or expects_nthash != nthash_active:
        raise ContractError("ntHash minimizer work proof contradicts observed minimizer")
    bounded_calls = _require_int(
        simd_stats.get("simd_chaining_bounded_policy_calls"),
        "SIMD bounded chaining calls")
    bounded_batches = _require_int(
        simd_stats.get("simd_chaining_bounded_score_batches"),
        "SIMD bounded chaining batches")
    if expect_simd_chaining != (bounded_calls > 0 and bounded_batches > 0):
        raise ContractError("SIMD chaining work proof contradicts observed chaining")
    if not expect_simd_chaining and (bounded_calls != 0 or bounded_batches != 0):
        raise ContractError("non-SIMD chaining route has contradictory SIMD work")

    mapq_available = _require_bool(
        attestation.get("mapq_execution_available"),
        "attestation.mapq_execution_available")
    mapq_executed = _require_bool(
        attestation.get("mapq_executed"), "attestation.mapq_executed")
    mapq_conflict = _require_bool(
        attestation.get("mapq_execution_conflict"),
        "attestation.mapq_execution_conflict")
    if not mapq_available or mapq_conflict or not mapq_executed:
        raise ContractError("MAPQ execution evidence contradicts the selected engine route")
    if (payload_schema_version == LEGACY_RAW_PAYLOAD_SCHEMA_VERSION
            and attestation.get("vulkan_synthetic_constant_mapq_not_executed")
                is not None):
        raise ContractError("Vulkan constant-MAPQ classification is contradictory")
    _validate_revision6_operation_origins(
        record,
        observed["base_alignment_algorithm"],
        expected_query_count,
        mapq_projection,
        mapq_executed,
    )
    return {
        "observed_minimizer_algorithm": observed["minimizer_algorithm"],
        "observed_chaining_algorithm": observed["chaining_algorithm"],
        "observed_base_alignment_algorithm": observed["base_alignment_algorithm"],
    }, transitions


def _expected_gate8_gucs(
        record: dict[str, Any], identity: dict[str, Any]) -> dict[str, str]:
    """Derive the complete accepted 22-GUC state from one sealed cell."""
    params = _require_dict(record.get("params"), "record.params")
    pipeline_mode = _require_string(
        params.get("pipeline_mode"), "Gate-8 pipeline mode")
    minimizer = identity["requested_minimizer_algorithm"]
    chaining = identity["requested_chaining_algorithm"]
    alignment = identity["requested_base_alignment_algorithm"]
    minimizer_index = identity["bioseq_index_name"]
    no_index = identity["index_mode"] == "no_index"
    vulkan_seed = minimizer == "vulkan_minimap_integer"
    return {
        "bioseq.planner.custom_scan.enabled": "on",
        "bioseq.planner.index_scan.enabled": "on",
        "bioseq.vectorization.enabled": "on",
        "bioseq.scheduler.pipeline.mode": pipeline_mode,
        "bioseq.aligner.minimizer.algorithm": minimizer,
        "bioseq.aligner.chaining.algorithm": chaining,
        "bioseq.aligner.base_alignment.algorithm": alignment,
        "bioseq.aligner.minimizer.kmer_size": str(identity["kmer_size"]),
        "bioseq.aligner.minimizer.window_size": str(identity["window_size"]),
        "bioseq.aligner.grouped_soa_generation.enabled": "on",
        "bioseq.aligner.vulkan.enabled": "on",
        "bioseq.aligner.vulkan.seed_lookup.enabled": (
            "on" if vulkan_seed else "off"),
        "bioseq.aligner.vulkan.index_cache.enabled": "on",
        "bioseq.aligner.vulkan.fused_frontend.enabled": "off",
        "bioseq.aligner.vulkan.alignment_extension.enabled": "off",
        "bioseq.aligner.vulkan.index_build.enabled": (
            "on" if vulkan_seed and no_index else "off"),
        "bioseq.aligner.fragment_minimizer.lookup_route": (
            "direct_compact_hash_lookup"
            if minimizer_index == "minimizers_per_fragment"
            else "control_block_scan"),
        "bioseq.column_cache.enabled": "on",
        "bioseq.index.async_persistence.enabled": "off",
        "bioseq.index.recompute_async_persistence.enabled": "off",
        "bioseq.index.insert_maintenance.enabled": "off",
        "bioseq.index.sequence_read.enabled": "off" if no_index else "on",
    }


def _validate_gate8_guc_snapshot(
        value: Any, expected: dict[str, str], label: str) -> dict[str, Any]:
    snapshot = _require_dict(value, label)
    _require_exact_keys(snapshot, {"sql", "json_text", "values"}, label)
    sql = _require_string(snapshot.get("sql"), f"{label}.sql")
    text = _require_string(snapshot.get("json_text"), f"{label}.json_text")
    values = _require_dict(snapshot.get("values"), f"{label}.values")
    if values != expected or len(values) != 22:
        raise ContractError(f"{label} differs from the complete selected 22-GUC state")
    try:
        parsed = _loads_json(text, f"{label}.json_text")
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise ContractError(f"{label} did not retain valid JSON bytes") from exc
    if parsed != values:
        raise ContractError(f"{label} JSON bytes differ from its parsed values")
    if (not sql.startswith("SELECT json_build_object(")
            or not sql.endswith(")::text;")
            or any(
                sql.count(f"current_setting('{name}')") != 1
                for name in expected)):
        raise ContractError(f"{label} SQL does not read every exact GUC once")
    return snapshot


def _validate_gate8_route(
        value: Any, attestation: dict[str, Any], label: str) -> dict[str, Any]:
    route = _require_dict(value, label)
    _require_exact_keys(route, {
        "requested_backend", "actual_backend",
        "requested_target_index_source", "actual_target_index_source",
        "requested_algorithms", "observed_algorithms",
        "requested_simd_chaining_policy", "observed_simd_chaining_policy",
        "requested_simd_alignment_algorithm", "observed_simd_alignment_algorithm",
        "prefilter_activity_count", "backend_declines", "backend_fallbacks",
    }, label)
    expected = {
        "requested_backend": attestation["requested_backend"],
        "actual_backend": attestation["actual_backend"],
        "requested_target_index_source":
            attestation["requested_target_index_source"],
        "actual_target_index_source": attestation["actual_target_index_source"],
        "requested_algorithms": {
            "minimizer": attestation["requested_minimizer_algorithm"],
            "chaining": attestation["requested_chaining_algorithm"],
            "base_alignment":
                attestation["requested_base_alignment_algorithm"],
        },
        "observed_algorithms": {
            "minimizer": attestation["observed_minimizer_algorithm"],
            "chaining": attestation["observed_chaining_algorithm"],
            "base_alignment":
                attestation["observed_base_alignment_algorithm"],
        },
        "requested_simd_chaining_policy":
            attestation["requested_simd_chaining_policy"],
        "observed_simd_chaining_policy":
            attestation["observed_simd_chaining_policy"],
        "requested_simd_alignment_algorithm":
            attestation["requested_simd_alignment_algorithm"],
        "observed_simd_alignment_algorithm":
            attestation["observed_simd_alignment_algorithm"],
        "prefilter_activity_count": 0,
        "backend_declines": attestation["backend_declines"],
        "backend_fallbacks": attestation["backend_fallbacks"],
    }
    if route != expected:
        raise ContractError(f"{label} differs from the measured route vector")
    return route


def _validate_gate8_absence(
        value: Any, query_count: int, label: str) -> None:
    absence = _require_dict(value, label)
    expected = {
        "target_rows": query_count,
        "target_cardinality_metadata_only": 1,
        "source_parent_rows_read": 0,
        "source_fragment_rows_read": 0,
        "source_decoded_bp": 0,
        "physical_index_relations": 0,
        "persisted_index_rows": 0,
        "generation_objects": 0,
    }
    _require_exact_keys(absence, set(expected), label)
    if absence != expected:
        raise ContractError(f"{label} does not prove physical no-index state")


def _validate_gate8_cache_evidence(
        value: Any, identity: dict[str, Any], label: str) -> dict[str, Any]:
    evidence = _require_dict(value, label)
    _require_exact_keys(evidence, {
        "applicability", "not_applicable_reason",
        "gpu_persisted_resident_uploads",
        "gpu_persisted_resident_cache_hits",
    }, label)
    applicable = (
        identity["index_mode"] == "just_index"
        and identity["requested_minimizer_algorithm"]
            == "vulkan_minimap_integer"
    )
    expected = {
        "applicability": (
            "persisted_vulkan_seed_statement_resident" if applicable
            else "not_applicable_to_selected_route"),
        "not_applicable_reason": (
            None if applicable
            else "no_index_uses_transient_target_construction"
            if identity["index_mode"] == "no_index"
            else "selected_minimizer_is_not_vulkan_seed"),
        "gpu_persisted_resident_uploads": 1 if applicable else 0,
        "gpu_persisted_resident_cache_hits": 0,
    }
    if evidence != expected:
        raise ContractError(
            f"{label} contradicts the existing selected-route resident counters")
    return evidence


def _gate8_mpf_reader_comment(
        record: dict[str, Any], identity: dict[str, Any]) -> str:
    """Explain effective MPF reading using already validated route counters."""
    if identity["bioseq_index_name"] != "minimizers_per_fragment":
        return ""
    if identity["index_mode"] == "no_index":
        return (
            "configured MPF direct_compact_hash_lookup is not applicable to "
            "no-index execution: transient target construction and querying "
            "ran without a persisted MPF reader")
    if identity["requested_minimizer_algorithm"] == "vulkan_minimap_integer":
        return (
            "configured MPF direct_compact_hash_lookup does not describe GPU "
            "seed lookup: persisted MPF host traversal feeds the validated "
            "statement-owned device resident; this is not a CPU seeding fallback")
    stats = record["attestation"]["minimizer_fragment_reader_stats"]
    control_bytes = _require_int(
        stats.get("fragment_native_control_count_bytes"),
        "MPF control-reader count bytes")
    direct_shards = _require_int(
        stats.get("fragment_native_compact_direct_selection_shards"),
        "MPF compact-direct selection shards")
    if control_bytes > 0 and direct_shards == 0:
        return (
            "configured MPF direct_compact_hash_lookup was not the effective "
            "reader: existing counters prove the persisted control reader "
            "ran; compact-direct applicability follows the generated hash "
            "profile, without changing seeding/chaining/alignment algorithms")
    if control_bytes == 0 and direct_shards > 0:
        return ""
    raise ContractError("MPF counters do not identify one effective reader")


def _gate8_explain_objects(value: Any) -> Iterable[dict[str, Any]]:
    """Yield every retained EXPLAIN object so summaries cannot spoof plan bytes."""
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from _gate8_explain_objects(child)
    elif isinstance(value, list):
        for child in value:
            yield from _gate8_explain_objects(child)


def _validate_gate8_explain_bytes(
        value: Any, pipeline: dict[str, Any]) -> None:
    """Prove the summarized provider and frozen pipeline from actual JSON bytes."""
    # EXPLAIN uses algorithm_identity_contract.cppm display names, while the
    # proof summary and requested/observed identities retain canonical IDs.
    chaining_display = {
        "scalar_minimap2": "Minimap2 Scalar",
        "simd_minimap2": "Minimap2 SIMD",
        "range_minimum_query": "Minimap2 RMQ",
        "vulkan_tiled_dynamic_programming": "Vulkan tiled DP",
    }.get(pipeline["chaining_algorithm"])
    base_display = {
        "ksw2": "KSW2",
        "block_aligner": "Block Aligner",
    }.get(pipeline["base_alignment_algorithm"])
    if chaining_display is None or base_display is None:
        raise ContractError("Gate-8 EXPLAIN has an unknown algorithm identity")
    explain_objects = list(_gate8_explain_objects(value))
    batch_join_nodes = [
        item for item in explain_objects
        if item.get("Node Type") == "Custom Scan"
        and item.get("Custom Plan Provider") == "BioseqBatchJoin"
    ]
    matching_pipeline_groups = []
    for item in batch_join_nodes:
        retained_pipeline = item.get("Bioseq Pipeline")
        if (isinstance(retained_pipeline, dict)
                and retained_pipeline.get("Chaining Algorithm")
                    == chaining_display
                and retained_pipeline.get("Base Alignment Algorithm")
                    == base_display
                and retained_pipeline.get("Pipeline Concurrency Mode")
                    == pipeline["pipeline_concurrency_mode"]
                and retained_pipeline.get("Max Concurrency")
                    == pipeline["max_concurrency"]):
            matching_pipeline_groups.append(retained_pipeline)
    if len(batch_join_nodes) != 1 or len(matching_pipeline_groups) != 1:
        raise ContractError(
            "Gate-8 EXPLAIN summary is not proven by the retained JSON plan bytes")


def _validate_gate8_sql_proof_v1(
        value: Any,
        record: dict[str, Any],
        identity: dict[str, Any],
        expected_query_count: int,
) -> dict[str, Any]:
    proof = _require_dict(value, "Gate-8 SQL proof")
    _require_exact_keys(proof, {
        "schema", "version", "scope", "measurement", "query_fixture",
        "detached",
    }, "Gate-8 SQL proof")
    if (proof.get("schema") != "bioseq.e2e.gate8-sql-proof"
            or proof.get("version") != 1
            or proof.get("scope")
                != "accepted_nonpaired_bioseq_algorithm_matrix"):
        raise ContractError("Gate-8 SQL proof identity changed")
    expected_gucs = _expected_gate8_gucs(record, identity)
    attestation = _require_dict(record.get("attestation"), "attestation")

    measurement = _require_dict(
        proof.get("measurement"), "Gate-8 measurement")
    _require_exact_keys(measurement, {
        "boundary", "operation", "statement_count", "sql", "target_table",
        "query_table", "index_read_requirement", "pre_statement_gucs",
        "ordered_primary_match_counts", "route", "cache_evidence",
    }, "Gate-8 measurement")
    expected_requirement = 2 if identity["index_mode"] == "just_index" else 1
    target_table = _require_string(
        measurement.get("target_table"), "Gate-8 target table")
    query_table = _require_string(
        measurement.get("query_table"), "Gate-8 query table")
    sql = _require_string(measurement.get("sql"), "Gate-8 measured SQL")
    if (measurement.get("boundary") != "framework_iteration_measurement_window"
            or measurement.get("operation")
                != "single_libpq_PQexec_mmsearch_statement"
            or measurement.get("statement_count") != 1
            or measurement.get("index_read_requirement") != expected_requirement
            or ";" in sql
            or sql.count("mmsearch(") != 1
            or sql.count(
                f"aligner_options(index_read_requirement => {expected_requirement})")
                != 1
            or f"FROM {target_table} t JOIN {query_table} q ON mmsearch(" not in sql):
        raise ContractError("Gate-8 A is not the one literal mmsearch statement")
    if (re.fullmatch(r"e2eq_(?:dna4|dna5)_k[0-9]+_w[0-9]+_[0-9a-f]{16}",
                    query_table) is None):
        raise ContractError("Gate-8 query fixture name is not deterministically bounded")
    _validate_gate8_guc_snapshot(
        measurement.get("pre_statement_gucs"), expected_gucs,
        "Gate-8 pre-statement GUCs")
    measured_route = _validate_gate8_route(
        measurement.get("route"), attestation, "Gate-8 measured route")
    measured_cache_evidence = _validate_gate8_cache_evidence(
        measurement.get("cache_evidence"), identity,
        "Gate-8 measured cache evidence")
    measured_counts = measurement.get("ordered_primary_match_counts")
    if (not isinstance(measured_counts, list)
            or len(measured_counts) != expected_query_count
            or any(isinstance(count, bool) or not isinstance(count, int)
                   or count < 0 or count > 1 for count in measured_counts)
            or sum(measured_counts) != expected_query_count):
        raise ContractError(
            "Gate-8 ordered query vector does not retain every query, including zeros")

    fixture = _require_dict(proof.get("query_fixture"), "Gate-8 query fixture")
    _require_exact_keys(fixture, {
        "table", "access_method", "row_count", "distinct_query_id_count",
        "minimum_query_id", "maximum_query_id", "policies",
        "physical_absence",
    }, "Gate-8 query fixture")
    if (fixture.get("table") != query_table
            or fixture.get("access_method") != "bioseq"
            or fixture.get("row_count") != expected_query_count
            or fixture.get("distinct_query_id_count") != expected_query_count
            or fixture.get("minimum_query_id") != 0
            or fixture.get("maximum_query_id") != expected_query_count - 1
            or fixture.get("policies") != {
                "minimizers_per_fragment": "UPDATE_MANUALLY",
                "minimizers_per_sequence": "UPDATE_MANUALLY",
            }):
        raise ContractError("Gate-8 durable USING bioseq query fixture changed")
    physical = _require_dict(
        fixture.get("physical_absence"), "Gate-8 query physical absence")
    _require_exact_keys(physical, {
        "minimizers_per_fragment", "minimizers_per_sequence",
    }, "Gate-8 query physical absence")
    for index_name in ("minimizers_per_fragment", "minimizers_per_sequence"):
        _validate_gate8_absence(
            physical.get(index_name), expected_query_count,
            f"Gate-8 query {index_name} absence")

    detached = _require_dict(proof.get("detached"), "Gate-8 detached proof")
    _require_exact_keys(detached, {
        "same_database", "same_postmaster", "different_backend_pid",
        "measured_backend_pid", "proof_backend_pid", "database_oid",
        "postmaster_start_time", "accepted_gucs", "explain_sql",
        "explain_json_text", "explain_contract", "explain_route", "controls",
    }, "Gate-8 detached proof")
    if (detached.get("same_database") is not True
            or detached.get("same_postmaster") is not True
            or detached.get("different_backend_pid") is not True
            or _require_int(detached.get("measured_backend_pid"),
                            "Gate-8 measured PID", minimum=1)
                == _require_int(detached.get("proof_backend_pid"),
                                "Gate-8 proof PID", minimum=1)
            or _require_int(detached.get("database_oid"),
                            "Gate-8 database OID", minimum=1) < 1
            or not _require_string(detached.get("postmaster_start_time"),
                                   "Gate-8 postmaster start")):
        raise ContractError("Gate-8 detached session identity is not independent")
    accepted_gucs = _validate_gate8_guc_snapshot(
        detached.get("accepted_gucs"), expected_gucs,
        "Gate-8 detached accepted GUCs")
    explain_sql = _require_string(
        detached.get("explain_sql"), "Gate-8 EXPLAIN SQL")
    explain_text = _require_string(
        detached.get("explain_json_text"), "Gate-8 EXPLAIN JSON")
    if explain_sql != "EXPLAIN (ANALYZE, FORMAT JSON) " + sql:
        raise ContractError("Gate-8 detached EXPLAIN is not the literal measured SQL")
    try:
        explain_json = _loads_gate8_explain_json(explain_text)
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise ContractError("Gate-8 EXPLAIN did not retain valid JSON bytes") from exc
    if (not isinstance(explain_json, list) or len(explain_json) != 1
            or not isinstance(explain_json[0], dict)
            or not isinstance(explain_json[0].get("Plan"), dict)):
        raise ContractError("Gate-8 EXPLAIN JSON has the wrong top-level shape")
    explain_contract = _require_dict(
        detached.get("explain_contract"), "Gate-8 EXPLAIN contract")
    _require_exact_keys(explain_contract, {
        "custom_scan_node_count", "node_type", "custom_plan_provider",
        "matching_pipeline_group_count", "pipeline",
    }, "Gate-8 EXPLAIN contract")
    pipeline = _require_dict(
        explain_contract.get("pipeline"), "Gate-8 EXPLAIN pipeline")
    _require_exact_keys(pipeline, {
        "chaining_algorithm", "base_alignment_algorithm",
        "pipeline_concurrency_mode", "max_concurrency",
    }, "Gate-8 EXPLAIN pipeline")
    params = _require_dict(record.get("params"), "Gate-8 framework params")
    if (explain_contract.get("custom_scan_node_count") != 1
            or explain_contract.get("node_type") != "Custom Scan"
            or explain_contract.get("custom_plan_provider")
                != "BioseqBatchJoin"
            or explain_contract.get("matching_pipeline_group_count") != 1
            or pipeline.get("chaining_algorithm")
                != identity["requested_chaining_algorithm"]
            or pipeline.get("base_alignment_algorithm")
                != identity["requested_base_alignment_algorithm"]
            or pipeline.get("pipeline_concurrency_mode")
                != params.get("pipeline_mode")
            or _require_int(
                pipeline.get("max_concurrency"),
                "Gate-8 EXPLAIN max concurrency", minimum=1) < 1):
        raise ContractError(
            "Gate-8 EXPLAIN does not prove BioseqBatchJoin and the frozen "
            "chaining/base/concurrency plan")
    _validate_gate8_explain_bytes(explain_json, pipeline)
    if _validate_gate8_route(
            detached.get("explain_route"), attestation,
            "Gate-8 EXPLAIN route") != measured_route:
        raise ContractError("Gate-8 EXPLAIN route differs from measured SQL")

    controls = _require_dict(detached.get("controls"), "Gate-8 controls")
    _require_exact_keys(
        controls, {"cache_off", "vectorization_off"}, "Gate-8 controls")
    expected_changes = {
        "cache_off": [
            "bioseq.aligner.vulkan.index_cache.enabled=off",
            "bioseq.column_cache.enabled=off",
        ],
        "vectorization_off": ["bioseq.vectorization.enabled=off"],
    }
    for control_name, changes in expected_changes.items():
        control = _require_dict(
            controls.get(control_name), f"Gate-8 {control_name} control")
        _require_exact_keys(control, {
            "identity", "publishable", "disposition", "changed_settings",
            "gucs", "result_status", "ordered_primary_match_counts", "route",
            "cache_evidence", "restored_gucs",
        }, f"Gate-8 {control_name} control")
        changed_gucs = copy.deepcopy(expected_gucs)
        if control_name == "cache_off":
            changed_gucs["bioseq.aligner.vulkan.index_cache.enabled"] = "off"
            changed_gucs["bioseq.column_cache.enabled"] = "off"
        else:
            changed_gucs["bioseq.vectorization.enabled"] = "off"
        if (control.get("identity") != control_name
                or control.get("publishable") is not False
                or control.get("disposition")
                    != "nonpublishable_control_config_changed"
                or control.get("changed_settings") != changes
                or control.get("result_status") != "PGRES_TUPLES_OK"
                or control.get("ordered_primary_match_counts") != measured_counts
                or _validate_gate8_cache_evidence(
                    control.get("cache_evidence"), identity,
                    f"Gate-8 {control_name} cache evidence")
                    != measured_cache_evidence
                or _validate_gate8_route(
                    control.get("route"), attestation,
                    f"Gate-8 {control_name} route") != measured_route):
            raise ContractError(
                f"Gate-8 {control_name} control changed output/route or became publishable")
        _validate_gate8_guc_snapshot(
            control.get("gucs"), changed_gucs,
            f"Gate-8 {control_name} GUCs")
        restored = _validate_gate8_guc_snapshot(
            control.get("restored_gucs"), expected_gucs,
            f"Gate-8 {control_name} restored GUCs")
        if restored["values"] != accepted_gucs["values"]:
            raise ContractError(f"Gate-8 {control_name} did not restore accepted GUCs")
    return proof


def _validate_gate8_postgres_identity(
        value: Any, label: str) -> dict[str, Any]:
    identity = _require_dict(value, label)
    _require_exact_keys(
        identity, {"postmaster_start_time", "backend_pid", "database_oid"},
        label)
    return {
        "postmaster_start_time": _require_string(
            identity.get("postmaster_start_time"),
            f"{label}.postmaster_start_time"),
        "backend_pid": _require_int(
            identity.get("backend_pid"), f"{label}.backend_pid", minimum=1),
        "database_oid": _require_int(
            identity.get("database_oid"), f"{label}.database_oid", minimum=1),
    }


def _validate_gate8_current_postgres(
        record: dict[str, Any]) -> dict[str, Any]:
    """Bind a Gate-8 v2 repeat to its actual cold timed SQL session."""
    postgres = _require_dict(record.get("postgres"), "Gate-8 PostgreSQL evidence")
    _require_exact_keys(postgres, {
        "postmaster_start", "backend_pid", "database_oid",
        "prior_execution_id", "actual_execution_id", "expected_generation",
        "expected_source_identity", "actual_generation",
        "actual_source_identity", "restart_identity", "persisted_policy",
        "persisted_layout", "restart_performed", "restart_identity_changed",
        "cold_no_index_lifecycle",
    }, "Gate-8 PostgreSQL evidence")

    def normalize(value: Any, label: str) -> dict[str, Any]:
        raw = _require_dict(value, label)
        _require_exact_keys(
            raw, {"postmaster_start", "backend_pid", "database_oid"}, label)
        return {
            "postmaster_start_time": _require_string(
                raw.get("postmaster_start"), f"{label}.postmaster_start"),
            "backend_pid": _require_int(
                raw.get("backend_pid"), f"{label}.backend_pid", minimum=1),
            "database_oid": _require_int(
                raw.get("database_oid"), f"{label}.database_oid", minimum=1),
        }

    restart = _require_dict(
        postgres.get("restart_identity"), "Gate-8 restart identity")
    _require_exact_keys(
        restart, {"before", "after", "timed_statement"},
        "Gate-8 restart identity")
    before = normalize(restart.get("before"), "Gate-8 restart before")
    after = normalize(restart.get("after"), "Gate-8 restart after")
    timed = normalize(
        restart.get("timed_statement"), "Gate-8 timed statement identity")
    current = {
        "postmaster_start_time": _require_string(
            postgres.get("postmaster_start"), "Gate-8 current postmaster start"),
        "backend_pid": _require_int(
            postgres.get("backend_pid"), "Gate-8 current backend PID", minimum=1),
        "database_oid": _require_int(
            postgres.get("database_oid"), "Gate-8 current database OID", minimum=1),
    }
    if (_require_int(
            postgres.get("restart_performed"), "Gate-8 restart performed") != 1
            or _require_int(
                postgres.get("restart_identity_changed"),
                "Gate-8 restart identity changed") != 1
            or current != after or current != timed
            or before["database_oid"] != current["database_oid"]
            or before["postmaster_start_time"]
                == current["postmaster_start_time"]
            or before["backend_pid"] == current["backend_pid"]):
        raise ContractError(
            "Gate-8 repeat is not bound to its actual post-restart timed session")
    return current


def _gate8_runtime_device(record: dict[str, Any]) -> dict[str, Any]:
    snapshot = _require_dict(
        record.get("alignment_snapshot"), "Gate-8 alignment snapshot")
    columns = _require_list(snapshot.get("columns"), "Gate-8 snapshot columns")
    rows = _require_list(snapshot.get("rows"), "Gate-8 snapshot rows")
    names = [
        _require_string(
            _require_dict(column, "Gate-8 snapshot column").get("name"),
            "Gate-8 snapshot column name")
        for column in columns
    ]
    if len(names) <= 5 or names[4:6] != ["gpu_device_uuid", "gpu_driver"]:
        raise ContractError("Gate-8 snapshot device columns changed")
    devices: set[tuple[str, str]] = set()
    for ordinal, raw_row in enumerate(rows):
        row = _require_list(raw_row, f"Gate-8 snapshot row[{ordinal}]")
        if len(row) != len(names):
            raise ContractError("Gate-8 snapshot device row width changed")
        devices.add((
            "" if row[4] is None else _require_text(
                row[4], f"Gate-8 snapshot row[{ordinal}] device"),
            "" if row[5] is None else _require_text(
                row[5], f"Gate-8 snapshot row[{ordinal}] driver"),
        ))
    if len(devices) != 1:
        raise ContractError("Gate-8 snapshot device identity changed across stages")
    device_uuid, driver = next(iter(devices))
    attestation = _require_dict(record.get("attestation"), "Gate-8 attestation")
    return {
        "execution_device": (
            "cpu_and_gpu"
            if attestation.get("actual_backend") == "hybrid_cpu_vulkan"
            else "cpu"),
        "gpu_device_uuid": device_uuid,
        "gpu_driver": driver,
    }


def _validate_gate8_runtime_cell(
        value: Any,
        record: dict[str, Any],
        identity: dict[str, Any],
        expected_query_count: int,
        measurement: dict[str, Any],
        detached: dict[str, Any],
        cigar_projection: bool,
        mapq_projection: bool,
        prefilter_enabled: bool,
) -> dict[str, Any]:
    runtime = _require_dict(value, "Gate-8 runtime cell")
    pipeline = _require_dict(
        _require_dict(
            detached.get("explain_contract"),
            "Gate-8 runtime EXPLAIN contract").get("pipeline"),
        "Gate-8 runtime EXPLAIN pipeline")
    postgres = _require_dict(record.get("postgres"), "Gate-8 PostgreSQL evidence")
    if identity["index_mode"] == "just_index":
        persisted = _require_dict(
            postgres.get("persisted_layout"), "Gate-8 persisted layout")
        target_state = {
            "kind": "persisted_generation",
            "layout": _require_string(
                persisted.get("layout"), "Gate-8 persisted layout identity"),
            "generation_id": _require_int(
                postgres.get("actual_generation"),
                "Gate-8 actual generation", minimum=1),
            "source_identity": _require_string(
                postgres.get("actual_source_identity"),
                "Gate-8 actual source identity"),
        }
    else:
        if postgres.get("persisted_layout") is not None:
            raise ContractError("Gate-8 no-index cell retained a persisted layout")
        target_state = {
            "kind": "transient_recomputed",
            "layout": None,
            "generation_id": None,
            "source_identity": None,
        }
    params = _require_dict(record.get("params"), "Gate-8 framework params")
    pairing = _require_dict(record.get("pairing"), "Gate-8 pairing")
    expected = {
        "algorithm": record.get("algorithm"),
        "dataset_key": identity["dataset"],
        "alphabet": identity["alphabet"],
        "target_size_bp": pairing.get("target_size_bp"),
        "kmer_size": identity["kmer_size"],
        "window_size": identity["window_size"],
        "query_count": expected_query_count,
        "mode": identity["index_mode"],
        "minimizer_index": identity["bioseq_index_name"],
        "requested_algorithms": {
            "minimizer": identity["requested_minimizer_algorithm"],
            "chaining": identity["requested_chaining_algorithm"],
            "base_alignment": identity["requested_base_alignment_algorithm"],
        },
        "pipeline_mode": params.get("pipeline_mode"),
        "max_concurrency": pipeline.get("max_concurrency"),
        "target_table": measurement.get("target_table"),
        "query_table": measurement.get("query_table"),
        "index_read_requirement": measurement.get("index_read_requirement"),
        "projections": {
            "cigar": cigar_projection,
            "mapq": mapq_projection,
            "prefilter": prefilter_enabled,
        },
        "route": measurement.get("route"),
        "target_state": target_state,
        "device": _gate8_runtime_device(record),
    }
    if runtime != expected:
        raise ContractError(
            "Gate-8 qualification runtime cell differs from the actual record")
    return runtime


def _validate_gate8_sql_proof(
        value: Any,
        record: dict[str, Any],
        identity: dict[str, Any],
        expected_query_count: int,
        cigar_projection: bool = True,
        mapq_projection: bool = True,
        prefilter_enabled: bool = False,
) -> dict[str, Any]:
    proof = _require_dict(value, "Gate-8 SQL proof")
    version = _require_int(proof.get("version"), "Gate-8 SQL proof version")
    if version == 1:
        return _validate_gate8_sql_proof_v1(
            proof, record, identity, expected_query_count)
    if version != 2:
        raise ContractError("Gate-8 SQL proof version is unsupported")
    _require_exact_keys(proof, {
        "schema", "version", "scope", "qualification_order",
        "measurement", "query_fixture", "binding", "qualification",
    }, "Gate-8 SQL proof v2")
    if (proof.get("schema") != "bioseq.e2e.gate8-sql-proof"
            or proof.get("scope")
                != "accepted_nonpaired_bioseq_algorithm_matrix"
            or proof.get("qualification_order") != "after_source_measurement"):
        raise ContractError("Gate-8 SQL proof v2 identity/order changed")
    qualification = _require_dict(
        proof.get("qualification"), "Gate-8 cell qualification")
    _require_exact_keys(qualification, {
        "runtime_cell", "source_repeat", "source_postgres",
        "source_measurement", "query_fixture", "detached",
    }, "Gate-8 cell qualification")
    source_repeat = _require_int(
        qualification.get("source_repeat"),
        "Gate-8 qualification source repeat")
    if source_repeat != 0:
        raise ContractError("Gate-8 qualification source must be repeat zero")
    source_postgres = _validate_gate8_postgres_identity(
        qualification.get("source_postgres"),
        "Gate-8 qualification source PostgreSQL")
    source_measurement = _require_dict(
        qualification.get("source_measurement"),
        "Gate-8 qualification source measurement")
    measurement = _require_dict(
        proof.get("measurement"), "Gate-8 current measurement")
    current_query_fixture = _require_dict(
        proof.get("query_fixture"), "Gate-8 current query fixture")
    source_query_fixture = _require_dict(
        qualification.get("query_fixture"),
        "Gate-8 qualification source query fixture")
    if measurement != source_measurement:
        raise ContractError(
            "Gate-8 current repeat measurement drifted from its source witness")
    if current_query_fixture != source_query_fixture:
        raise ContractError(
            "Gate-8 current repeat query fixture drifted from its source "
            "qualification")

    # Reuse the unchanged strict v1 SQL/EXPLAIN/control validator, but label
    # its detached material honestly as the one source qualification.
    legacy_projection = {
        "schema": "bioseq.e2e.gate8-sql-proof",
        "version": 1,
        "scope": "accepted_nonpaired_bioseq_algorithm_matrix",
        "measurement": source_measurement,
        "query_fixture": current_query_fixture,
        "detached": qualification.get("detached"),
    }
    _validate_gate8_sql_proof_v1(
        legacy_projection, record, identity, expected_query_count)
    detached = _require_dict(
        qualification.get("detached"), "Gate-8 source detached proof")
    if (detached.get("measured_backend_pid") != source_postgres["backend_pid"]
            or detached.get("database_oid") != source_postgres["database_oid"]
            or detached.get("postmaster_start_time")
                != source_postgres["postmaster_start_time"]):
        raise ContractError(
            "Gate-8 detached source does not bind its real measured session")
    _validate_gate8_runtime_cell(
        qualification.get("runtime_cell"), record, identity,
        expected_query_count, measurement, detached,
        cigar_projection, mapq_projection, prefilter_enabled)

    binding = _require_dict(proof.get("binding"), "Gate-8 repeat binding")
    _require_exact_keys(binding, {
        "repeat", "source_repeat", "role", "source_epoch_is_current",
        "current_postgres", "source_postgres",
    }, "Gate-8 repeat binding")
    repeat = _require_int(binding.get("repeat"), "Gate-8 binding repeat")
    role = _require_string(binding.get("role"), "Gate-8 binding role")
    source_is_current = _require_bool(
        binding.get("source_epoch_is_current"),
        "Gate-8 source-epoch current flag")
    current_postgres = _validate_gate8_postgres_identity(
        binding.get("current_postgres"), "Gate-8 current PostgreSQL")
    binding_source_postgres = _validate_gate8_postgres_identity(
        binding.get("source_postgres"), "Gate-8 binding source PostgreSQL")
    actual_postgres = _validate_gate8_current_postgres(record)
    expected_role = "source" if repeat == 0 else "reuse"
    if (repeat != record.get("repeat")
            or binding.get("source_repeat") != source_repeat
            or role != expected_role
            or source_is_current != (repeat == 0)
            or current_postgres != actual_postgres
            or binding_source_postgres != source_postgres
            or current_postgres["database_oid"]
                != source_postgres["database_oid"]
            or (repeat == 0 and current_postgres != source_postgres)
            or (repeat != 0 and current_postgres["postmaster_start_time"]
                == source_postgres["postmaster_start_time"])):
        raise ContractError(
            "Gate-8 repeat binding is not its actual source/current SQL epoch")
    return proof


def _validate_record(
        record: dict[str, Any],
        source: dict[str, Any],
        campaign_id: str,
        expected_query_count: int,
        mapq_projection: bool,
        cigar_projection: bool = True,
        prefilter_enabled: bool = False,
) -> dict[str, Any]:
    if (_require_int(record.get("schema"), "raw framework schema") != 1
            or record.get("benchmark") != "bench_e2e"):
        raise ContractError("raw record is not a bench_e2e framework schema-1 envelope")
    payload_schema_version = _require_int(
        record.get("payload_schema_version"), "raw payload schema version")
    if (record.get("payload_schema") != "bioseq.e2e.raw-sample"
            or payload_schema_version
                not in SUPPORTED_RAW_PAYLOAD_SCHEMA_VERSIONS):
        raise ContractError(
            "E2E raw payload schema must be current version "
            f"{RAW_PAYLOAD_SCHEMA_VERSION} or retained version "
            f"{LEGACY_RAW_PAYLOAD_SCHEMA_VERSION}")
    identity, stable_key, algorithm = _stable_identity(
        record, campaign_id, payload_schema_version)
    repeat = _require_int(record.get("repeat"), "framework repeat")
    if repeat >= MIN_ACCEPTED_REPEATS:
        raise ContractError("campaign repeat must be in the exact range 0..4")

    status = _require_dict(record.get("tuple_status"), "tuple status")
    _require_exact_keys(status, {
        "accepted",
        "disposition",
        "phase",
        "correctness_evaluated",
        "rejection_reason",
        "behavior_difference_comment",
        "requested_minimizer_algorithm",
        "requested_chaining_algorithm",
        "requested_base_alignment_algorithm",
        "sql_diagnostics",
    }, "tuple status")
    accepted_raw = _require_int(status.get("accepted"), "tuple_status.accepted")
    if accepted_raw not in (0, 1):
        raise ContractError("tuple_status.accepted must be integer 0 or 1")
    accepted = accepted_raw == 1
    disposition = _require_string(status.get("disposition"), "tuple disposition")
    phase = _require_string(status.get("phase"), "tuple phase")
    rejection_reason = _require_text(
        status.get("rejection_reason"), "tuple rejection reason")
    behavior_comment = _require_text(
        status.get("behavior_difference_comment"), "tuple behavior comment")
    status_evaluated = _require_int(
        status.get("correctness_evaluated"), "tuple_status.correctness_evaluated")
    if status_evaluated not in (0, 1):
        raise ContractError("tuple_status.correctness_evaluated must be integer 0 or 1")
    diagnostics = _require_dict(status.get("sql_diagnostics"), "SQL diagnostics")
    _require_exact_keys(
        diagnostics,
        {"result_status", "sqlstate", "primary", "detail"},
        "SQL diagnostics",
    )
    for key in diagnostics:
        _require_text(diagnostics.get(key), f"SQL diagnostics.{key}")

    skipped_after_prior_attestation_rejection = (
        disposition == "skipped_after_prior_attestation_rejection")
    prior_attestation_rejection = record.get("prior_attestation_rejection")
    if skipped_after_prior_attestation_rejection:
        if payload_schema_version != RAW_PAYLOAD_SCHEMA_VERSION:
            raise ContractError(
                "prior-attestation skip requires the current raw schema")
        provenance = _require_dict(
            prior_attestation_rejection,
            "prior attestation rejection provenance")
        _require_exact_keys(provenance, {
            "schema", "version", "source_repeat", "source_disposition",
            "source_rejection_reason",
        }, "prior attestation rejection provenance")
        source_repeat = _require_int(
            provenance.get("source_repeat"),
            "prior attestation rejection source repeat")
        source_reason = _require_string(
            provenance.get("source_rejection_reason"),
            "prior attestation rejection source reason")
        if (provenance.get("schema")
                != "bioseq.e2e.prior-attestation-rejection"
                or provenance.get("version") != 1
                or provenance.get("source_disposition")
                    != "rejected_attestation"
                or source_repeat < 0 or source_repeat >= repeat
                or not source_reason or source_reason != rejection_reason
                or status_evaluated != 0
                or any(diagnostics.get(key) for key in diagnostics)):
            raise ContractError(
                "prior-attestation skip provenance/chronology is contradictory")
        for field in (
                "alignment_snapshot", "attestation", "correctness",
                "gate8_sql", "interval_receipt", "logical_execution",
                "mapq_diagnostic", "ordered_query_oracle", "postgres",
                "persisted_gpu_operation_stats",
                "process_priority_attestation"):
            if record.get(field) is not None:
                raise ContractError(
                    "prior-attestation skip retained execution evidence "
                    f"in {field}")
    elif prior_attestation_rejection is not None:
        raise ContractError(
            "prior-attestation rejection provenance escaped its skip disposition")

    requested_values = (
        status.get("requested_minimizer_algorithm"),
        status.get("requested_chaining_algorithm"),
        status.get("requested_base_alignment_algorithm"),
    )
    if any(not isinstance(value, str) for value in requested_values):
        raise ContractError("tuple-status requested identities must be strings")
    if identity["engine"] == "bioseq":
        if requested_values != (
            identity["requested_minimizer_algorithm"],
            identity["requested_chaining_algorithm"],
            identity["requested_base_alignment_algorithm"],
        ):
            raise ContractError("tuple-status requested identities differ from the stable key")
    elif requested_values != ("", "", ""):
        raise ContractError("minimap2 baseline must not claim Bioseq requested algorithms")

    ordered_oracle = _validate_ordered_query_oracles(record, expected_query_count)
    pairing_evidence = _validate_pairing_evidence(
        record,
        payload_schema_version,
        ordered_oracle is not None and ordered_oracle["sealed"],
    )
    paired_oracle_invalid = (
        ordered_oracle is not None
        and ordered_oracle["sealed"]
        and not ordered_oracle["exact"]
    )
    measured_query_connection_failure_invalid = (
        not accepted
        and disposition == "invalid"
        and phase == "measured_statement"
    )
    if accepted:
        if disposition not in ACCEPTED_DISPOSITIONS or phase != "completed":
            raise ContractError("accepted flag contradicts tuple disposition/phase")
        if rejection_reason:
            raise ContractError("accepted tuple retains a rejection reason")
        if disposition == "accepted" and behavior_comment:
            raise ContractError("plain accepted disposition contradicts a behavior comment")
        if disposition != "accepted" and not behavior_comment:
            raise ContractError("accepted behavior/fallback disposition requires a comment")
        record_status = "accepted"
    else:
        if disposition in ACCEPTED_DISPOSITIONS:
            raise ContractError("rejected flag contradicts an accepted disposition")
        if disposition not in EXPECTED_REJECTION_DISPOSITIONS | INVALID_DISPOSITIONS:
            raise ContractError(f"unknown rejected tuple disposition {disposition!r}")
        expected_phase = (
            "paired_oracle" if paired_oracle_invalid else "measured_statement"
            if measured_query_connection_failure_invalid
            else {
                "rejected_setup": "setup",
                "rejected_sql": "measured_statement",
                "invalid": "correctness",
                "rejected_correctness": "correctness",
                "rejected_attestation": "attestation",
                "skipped_after_prior_attestation_rejection": "not_executed",
            }[disposition]
        )
        if phase != expected_phase or not rejection_reason or behavior_comment:
            raise ContractError("rejected tuple disposition/reason/comment/phase is contradictory")
        if disposition == "invalid" and not rejection_reason.startswith("**INVALID**:"):
            raise ContractError("invalid tuple reason must start with exact **INVALID**:")
        if measured_query_connection_failure_invalid:
            _validate_measured_query_connection_failure(
                record, status, identity, rejection_reason)
            record_status = "invalid"
        elif disposition in EXPECTED_REJECTION_DISPOSITIONS:
            _validate_expected_rejection(
                status, identity, disposition, rejection_reason,
                mapq_projection, cigar_projection, prefilter_enabled)
            record_status = "rejected"
        else:
            record_status = "invalid"
    postgres_evidence = record.get("postgres")
    if (not measured_query_connection_failure_invalid
            and isinstance(postgres_evidence, dict)
            and "measured_query_connection_failure" in postgres_evidence):
        raise ContractError(
            "connection-failure proof escaped its invalid measured-statement disposition")

    contract = _require_dict(
        record.get("e2e_interval_metric_contract"), "interval metric contract")
    if (
        contract.get("schema") != "bioseq.e2e.interval-metrics"
        or contract.get("version") != 2
        or contract.get("framework_finalize_adjusted_metric") is not True
    ):
        raise ContractError("framework-adjusted interval contract v2 is required")
    domains = _require_dict(contract.get("duration_clock_domains"), "duration clock domains")
    if (
        domains.get("absolute_timestamp_domain_equality_claimed") is not False
        or domains.get("same_repeat_proof")
            != "framework_program_order_and_raw_envelope"
    ):
        raise ContractError("timing contract makes an invalid absolute-clock claim")
    interval_domain = _require_dict(
        domains.get("interval_union"), "interval-union clock domain")
    framework_domain = _require_dict(
        domains.get("framework_outer"), "framework-outer clock domain")
    if framework_domain != {
        "clock": "bioseq.instrumentation.clock::now_ns",
        "unit": "ns",
        "value_kind": "duration",
    }:
        raise ContractError("framework outer duration clock contract is malformed")
    required_rate_contract = (
        "same_clock_implementation"
        if identity["engine"] == "bioseq"
        else "same_kernel_monotonic_nanosecond_rate"
    )
    required_interval_clock = (
        "bioseq.instrumentation.clock::now_ns"
        if identity["engine"] == "bioseq"
        else "helper-declared CLOCK_MONOTONIC"
    )
    if accepted and (
        interval_domain.get("clock") != required_interval_clock
        or interval_domain.get("unit") != "ns"
        or interval_domain.get("value_kind") != "duration"
        or interval_domain.get("rate_compatibility") != required_rate_contract
    ):
        raise ContractError(
            "interval and framework duration clock rates are not proven compatible"
        )
    if not accepted and interval_domain != {
            "clock": "not_available",
            "unit": "ns",
            "value_kind": "duration",
            "rate_compatibility": "not_available",
    }:
        raise ContractError("rejected/invalid record retained an interval clock authority")
    required_timing_fields = {
        "outer_e2e_ns",
        "index_and_map_ns",
        "cigar_or_mapq_interval_union_ns",
        "outer_e2e_minus_cigar_or_mapq_union_ns",
        "downstream_active_wall_ns",
        "downstream_work_ns",
        "interval_receipt",
    }
    missing_timing_fields = required_timing_fields - set(record)
    if missing_timing_fields:
        raise ContractError(
            "raw E2E record is missing timing-nullability fields: "
            + ", ".join(sorted(missing_timing_fields)))
    outer_value = record["outer_e2e_ns"]
    union_value = record["cigar_or_mapq_interval_union_ns"]
    adjusted_value = record["outer_e2e_minus_cigar_or_mapq_union_ns"]
    if accepted:
        outer = _require_int(outer_value, "accepted outer_e2e_ns")
        union_ns = _require_int(union_value, "accepted interval union")
        adjusted = _require_int(adjusted_value, "framework adjusted timing")
        if union_ns > outer or adjusted != outer - union_ns:
            raise ContractError("same-repeat adjusted timing does not equal outer minus union")
        _validate_receipt(record, union_ns, identity["engine"])
    else:
        retained_timing_paths = _non_null_numeric_timing_paths(record)
        if (outer_value is not None
                or union_value is not None or adjusted_value is not None
                or record["index_and_map_ns"] is not None
                or record["downstream_active_wall_ns"] is not None
                or record["downstream_work_ns"] is not None
                or record["interval_receipt"] is not None
                or retained_timing_paths):
            suffix = (f"; first retained field={retained_timing_paths[0]}"
                      if retained_timing_paths else "")
            raise ContractError(
                "rejected/invalid record retained forbidden numeric timing"
                + suffix)
        outer = None
        union_ns = None
        adjusted = None

    if skipped_after_prior_attestation_rejection:
        # Local sentinels let the existing invalid-cell aggregation remain
        # closed without inventing any raw observed-count evidence.
        correctness = {
            "evaluated": 0,
            "found": False,
            "match_count": 0,
            "expected_match_count": expected_query_count,
            "correct_match_count": 0,
            "exact_primary_count_accepted": 0,
            "strand_result_count": 0,
            "correct_strand_count": 0,
            "cigar_result_count": 0,
            "correct_cigar_count": 0,
            "cigar_replay_count": 0,
            "mapq_result_count": 0,
            "max_abs_offset_bp": None,
        }
    else:
        correctness = _require_dict(record.get("correctness"), "correctness")
    _require_exact_keys(correctness, {
        "evaluated",
        "found",
        "match_count",
        "expected_match_count",
        "correct_match_count",
        "exact_primary_count_accepted",
        "strand_result_count",
        "correct_strand_count",
        "cigar_result_count",
        "correct_cigar_count",
        "cigar_replay_count",
        "mapq_result_count",
        "max_abs_offset_bp",
    }, "correctness")
    evaluated_raw = correctness.get("evaluated")
    if isinstance(evaluated_raw, bool) or evaluated_raw not in (0, 1):
        raise ContractError("correctness.evaluated must be integer 0 or 1")
    if evaluated_raw != status_evaluated:
        raise ContractError("tuple-status and correctness evaluated flags contradict")
    found = correctness.get("found")
    if not isinstance(found, bool):
        raise ContractError("correctness.found must be a Boolean")
    match_count = _require_int(correctness.get("match_count"), "match_count")
    expected_count = _require_int(
        correctness.get("expected_match_count"), "expected_match_count")
    correct_count = _require_int(
        correctness.get("correct_match_count"), "correct_match_count")
    exact_primary = _require_int(
        correctness.get("exact_primary_count_accepted"),
        "exact_primary_count_accepted")
    strand_count = _require_int(
        correctness.get("strand_result_count"), "strand_result_count")
    correct_strand_count = _require_int(
        correctness.get("correct_strand_count"), "correct_strand_count")
    cigar_count = _require_int(
        correctness.get("cigar_result_count"), "cigar_result_count")
    correct_cigar_count = _require_int(
        correctness.get("correct_cigar_count"), "correct_cigar_count")
    cigar_replay_count = _require_int(
        correctness.get("cigar_replay_count"), "cigar_replay_count")
    mapq_count = _require_int(
        correctness.get("mapq_result_count"), "mapq_result_count")
    offset = correctness.get("max_abs_offset_bp")
    if offset is not None and (
            isinstance(offset, bool) or not isinstance(offset, (int, float))
            or not math.isfinite(offset) or offset < 0):
        raise ContractError("max_abs_offset_bp must be null or finite and nonnegative")
    if expected_count != expected_query_count:
        raise ContractError("raw expected_match_count differs from canonical query cardinality")
    measured_counts = (
        match_count, correct_count, exact_primary, strand_count,
        correct_strand_count, cigar_count, correct_cigar_count,
        cigar_replay_count, mapq_count,
    )
    if record_status == "rejected" and (
            evaluated_raw != 0 or found or any(measured_counts)
            or offset is not None):
        raise ContractError("expected capability rejection retained correctness evidence")
    if accepted and (
            evaluated_raw != 1 or not found or exact_primary != 1
            or any(count != expected_query_count for count in (
                match_count, correct_count, strand_count, correct_strand_count,
                cigar_count, correct_cigar_count, cigar_replay_count, mapq_count))
            or offset is None or offset >= 15):
        raise ContractError("accepted tuple contradicts the canonical correctness contract")
    if measured_query_connection_failure_invalid and (
            evaluated_raw != 0 or found or any(measured_counts)
            or offset is not None):
        raise ContractError(
            "measured connection failure retained correctness evidence")
    if (not measured_query_connection_failure_invalid and not paired_oracle_invalid
            and disposition in {"invalid", "rejected_correctness"}
            and evaluated_raw != 1):
        raise ContractError("correctness-invalid tuple lacks evaluated correctness evidence")

    shape = _require_dict(record.get("benchmark_shape"), "benchmark shape")
    _require_exact_keys(shape, {
        "expected_target_rows", "expected_query_rows", "expected_alignment_jobs",
        "actual_target_rows", "actual_alignment_jobs",
    }, "benchmark shape")
    expected_query_rows = _require_int(
        shape.get("expected_query_rows"), "benchmark expected_query_rows")
    expected_alignment_jobs = _require_int(
        shape.get("expected_alignment_jobs"),
        "benchmark expected_alignment_jobs")
    expected_target_rows = shape.get("expected_target_rows")
    required_target_rows = 1 if identity["engine"] == "bioseq" else None
    if expected_target_rows is not None:
        expected_target_rows = _require_int(
            expected_target_rows, "benchmark expected_target_rows")
    if (expected_query_rows != expected_query_count
            or expected_alignment_jobs != expected_query_count
            or expected_target_rows != required_target_rows):
        raise ContractError("benchmark shape differs from canonical query cardinality")
    for key in ("actual_target_rows", "actual_alignment_jobs"):
        value = shape.get(key)
        if value is not None:
            _require_int(value, f"benchmark {key}")
    if (measured_query_connection_failure_invalid
            or skipped_after_prior_attestation_rejection) and any(
            shape.get(key) is not None
            for key in ("actual_target_rows", "actual_alignment_jobs")):
        raise ContractError(
            "unexecuted record retained executed benchmark shape")

    if accepted:
        observed, transitions = _validate_accepted_attestation(
            record,
            identity,
            disposition,
            behavior_comment,
            expected_query_count,
            cigar_projection,
            mapq_projection,
            prefilter_enabled,
            payload_schema_version,
        )
    elif skipped_after_prior_attestation_rejection:
        observed = {
            "observed_minimizer_algorithm": None,
            "observed_chaining_algorithm": None,
            "observed_base_alignment_algorithm": None,
        }
        transitions = {
            "minimizer": "not_accepted",
            "chaining": "not_accepted",
            "base_alignment": "not_accepted",
        }
    else:
        attestation = _require_dict(record.get("attestation"), "attestation")
        _validate_attestation_shape(
            record, attestation, payload_schema_version)
        observed = {
            "observed_minimizer_algorithm":
                attestation.get("observed_minimizer_algorithm")
                if identity["engine"] == "bioseq" else None,
            "observed_chaining_algorithm":
                attestation.get("observed_chaining_algorithm")
                if identity["engine"] == "bioseq" else None,
            "observed_base_alignment_algorithm":
                attestation.get("observed_base_alignment_algorithm")
                if identity["engine"] == "bioseq" else None,
        }
        transitions = {
            "minimizer": "not_accepted",
            "chaining": "not_accepted",
            "base_alignment": "not_accepted",
        }
    gate8_required = (
        payload_schema_version == RAW_PAYLOAD_SCHEMA_VERSION
        and accepted
        and identity["engine"] == "bioseq"
        and ordered_oracle is not None
        and not ordered_oracle["sealed"]
    )
    gate8_sql = record.get("gate8_sql")
    if gate8_required:
        gate8_sql = _validate_gate8_sql_proof(
            gate8_sql, record, identity, expected_query_count,
            cigar_projection, mapq_projection, prefilter_enabled)
        # This is an extracted report comment, not a mutation of the validated
        # raw disposition or its exact algorithm-behavior comment contract.
        reader_comment = _gate8_mpf_reader_comment(record, identity)
        if reader_comment:
            behavior_comment += ("; " if behavior_comment else "") + reader_comment
    elif gate8_sql is not None:
        raise ContractError(
            "Gate-8 SQL proof escaped accepted current-schema ordinary "
            "Bioseq scope")
    return {
        "identity": identity,
        "stable_key": stable_key,
        "test_case": record["test_case"],
        "group_index": record["group_index"],
        "ordered_oracle": ordered_oracle,
        "pairing_evidence": pairing_evidence,
        "gate8_sql": gate8_sql,
        "payload_schema_version": payload_schema_version,
        "algorithm": algorithm,
        "repeat": repeat,
        "accepted": accepted,
        "record_status": record_status,
        "disposition": disposition,
        "rejection_reason": rejection_reason,
        "behavior_comment": behavior_comment,
        "correctness_evaluated": evaluated_raw == 1,
        "match_count": match_count,
        "expected_match_count": expected_count,
        "correct_match_count": correct_count,
        "outer_e2e_ns": outer,
        "adjusted_ns": adjusted,
        "observed": observed,
        "transition_classifications": transitions,
        "source": source,
        "record_checksum": record["record_checksum"],
    }


def _load_live_run_receipt_context(
        run_receipt_id: str,
        report_path: Path,
        config_path: Path,
        receipt_config_path: Path | None = None,
        label: str = "incoming q1024 run receipt",
) -> tuple[dict[str, Any], dict[str, Any]]:
    if re.fullmatch(r"rr_[0-9a-f]{32}", run_receipt_id) is None:
        raise ContractError("--run-receipt-id must be one exact rr_<32hex> id")
    raw = _read_external_regular(
        BUILD_RECEIPT_STORE_PATH, "build/run receipt store")
    try:
        document = _loads_json(raw.decode("utf-8"), "build/run receipt store")
    except UnicodeError as exc:
        raise ContractError("build/run receipt store is not UTF-8") from exc
    store = _require_dict(document, "build/run receipt store")
    run = store.get(run_receipt_id)
    if not isinstance(run, dict) or run.get("build_receipt_id") != run_receipt_id:
        raise ContractError("requested run receipt is absent from the receipt store")
    build_id = run.get("reused_build_receipt")
    build = store.get(build_id) if isinstance(build_id, str) else None
    if not isinstance(build, dict) or build.get("build_receipt_id") != build_id:
        raise ContractError("run receipt reused build receipt is absent")
    return _validate_run_receipt_snapshot(
        run, build, report_path,
        receipt_config_path if receipt_config_path is not None else config_path,
        label)


def _is_vulkan_hardware_setup_descriptor_override(
        item: dict[str, Any],
        expected_disposition: str,
) -> bool:
    """Narrow bridge for immutable descriptors generated before this boundary."""
    identity = item["identity"]
    expected_primary = "bioseq.update_index raised a C++ exception"
    return (
        expected_disposition == "rejected_sql"
        and item["record_status"] == "rejected"
        and item["disposition"] == "rejected_setup"
        and identity["dataset"] == "matrix_2gbp_dna4_k15_w10"
        and identity["alphabet"] == "dna4"
        and identity["target_size_gbp"] == 2
        and (identity["kmer_size"], identity["window_size"]) == (15, 10)
        and identity["index_mode"] == "just_index"
        and identity["engine"] == "bioseq"
        and identity["bioseq_index_name"] in {
            "minimizers_per_fragment",
            "minimizers_per_sequence",
        }
        and identity["requested_minimizer_algorithm"]
            == "vulkan_minimap_integer"
        and identity["requested_chaining_algorithm"] in {
            "scalar_minimap2",
            "simd_minimap2",
            "range_minimum_query",
            "vulkan_tiled_dynamic_programming",
        }
        and identity["requested_base_alignment_algorithm"] in {
            "ksw2",
            "block_aligner",
        }
        and item["rejection_reason"] == (
            "allowlisted untimed bioseq.update_index Vulkan hardware "
            "capability rejection "
            f"identity={identity['requested_minimizer_algorithm']}|"
            f"{identity['bioseq_index_name']} SQLSTATE=XX000 "
            f"primary={expected_primary}"))


def _validate_config_disposition_records(
        validated: list[dict[str, Any]], config_entry: dict[str, Any]) -> None:
    """Enforce only an explicitly sealed legacy all-rejected descriptor."""
    expected_disposition = config_entry["expected_all_records_disposition"]
    if expected_disposition is not None and any(
            item["record_status"] != "rejected"
            or (item["disposition"] != expected_disposition
                and not _is_vulkan_hardware_setup_descriptor_override(
                    item, expected_disposition))
            or item["outer_e2e_ns"] is not None
            or item["adjusted_ns"] is not None
            for item in validated):
        raise ContractError(
            "all-rejected canonical shard did not serialize only exact untimed "
            f"{expected_disposition} records")


def _validate_gate8_cell_qualifications(
        records: list[dict[str, Any]],
        source: dict[str, Any],
        build_receipt: dict[str, Any] | None,
) -> None:
    """Join one v2 source qualification to five actual records in this report."""
    gate8_versions = {
        _require_int(
            item["gate8_sql"].get("version"),
            "Gate-8 report proof version")
        for item in records
        if isinstance(item.get("gate8_sql"), dict)
    }
    if 2 in gate8_versions and gate8_versions != {2}:
        raise ContractError(
            "one report may not mix Gate-8 v1 and v2 required records")
    by_cell: dict[str, list[dict[str, Any]]] = {}
    for item in records:
        by_cell.setdefault(item["stable_key"], []).append(item)
    report_record_owners: set[tuple[str, str]] = set()
    report_source_owners: set[tuple[str, str]] = set()
    report_epoch_owners: set[tuple[str, int, str]] = set()
    for stable_key, members in by_cell.items():
        v2_members = [
            item for item in members
            if isinstance(item.get("gate8_sql"), dict)
            and item["gate8_sql"].get("version") == 2
        ]
        if not v2_members:
            continue
        if (len(members) != MIN_ACCEPTED_REPEATS
                or len(v2_members) != MIN_ACCEPTED_REPEATS
                or any(not item["accepted"] for item in members)):
            raise ContractError(
                "Gate-8 v2 cell must contain five accepted v2 bindings")
        ordered = sorted(members, key=lambda item: item["repeat"])
        if [item["repeat"] for item in ordered] != list(
                range(MIN_ACCEPTED_REPEATS)):
            raise ContractError("Gate-8 v2 cell lost repeat ordinals 0..4")
        if build_receipt is None:
            raise ContractError(
                "Gate-8 v2 report lacks its validated build/run receipt context")

        qualifications = [
            item["gate8_sql"]["qualification"] for item in ordered]
        qualification_bytes = _canonical_bytes(qualifications[0])
        if any(_canonical_bytes(value) != qualification_bytes
               for value in qualifications[1:]):
            raise ContractError(
                "Gate-8 v2 bindings do not share one exact source qualification")
        source_item = ordered[0]
        source_proof = source_item["gate8_sql"]
        source_binding = source_proof["binding"]
        qualification = qualifications[0]
        if (source_binding.get("role") != "source"
                or source_binding.get("source_epoch_is_current") is not True
                or qualification.get("source_repeat") != 0
                or qualification.get("source_measurement")
                    != source_proof.get("measurement")
                or qualification.get("source_postgres")
                    != source_binding.get("current_postgres")):
            raise ContractError(
                "Gate-8 qualification is not owned by the actual source record")

        source_record_checksum = _require_string(
            source_item.get("record_checksum"),
            "Gate-8 source record checksum")
        source_owner = (source["report_sha256"], source_record_checksum)
        if source_owner in report_source_owners:
            raise ContractError(
                "Gate-8 v2 source record was reused across cells")
        report_source_owners.add(source_owner)
        record_checksums: set[str] = set()
        current_epochs: set[str] = set()
        source_postgres = qualification["source_postgres"]
        for ordinal, item in enumerate(ordered):
            proof = item["gate8_sql"]
            binding = proof["binding"]
            checksum = _require_string(
                item.get("record_checksum"),
                f"Gate-8 repeat {ordinal} record checksum")
            current_postgres = binding["current_postgres"]
            epoch = current_postgres["postmaster_start_time"]
            if (checksum in record_checksums or epoch in current_epochs
                    or binding.get("repeat") != ordinal
                    or binding.get("source_repeat") != 0
                    or binding.get("source_postgres") != source_postgres
                    or binding.get("role")
                        != ("source" if ordinal == 0 else "reuse")
                    or binding.get("source_epoch_is_current")
                        != (ordinal == 0)
                    or current_postgres["database_oid"]
                        != source_postgres["database_oid"]
                    or (ordinal == 0 and current_postgres != source_postgres)
                    or (ordinal != 0 and epoch
                        == source_postgres["postmaster_start_time"])):
                raise ContractError(
                    "Gate-8 v2 report contains a replayed/cross-cell binding")
            record_checksums.add(checksum)
            current_epochs.add(epoch)
            record_owner = (source["report_sha256"], checksum)
            if record_owner in report_record_owners:
                raise ContractError(
                    "Gate-8 v2 current record was rebound across cells")
            report_record_owners.add(record_owner)
            epoch_owner = (
                source["report_sha256"],
                current_postgres["database_oid"],
                epoch,
            )
            if epoch_owner in report_epoch_owners:
                raise ContractError(
                    "Gate-8 v2 cold PostgreSQL epoch was rebound across cells")
            report_epoch_owners.add(epoch_owner)

        runtime_cell = qualification["runtime_cell"]
        qualification_sha256 = _sha256(qualification_bytes)
        fixture_qualification = {
            "dataset_key": runtime_cell["dataset_key"],
            "target_state": runtime_cell["target_state"],
            "target_table": runtime_cell["target_table"],
            "query_fixture": qualification["query_fixture"],
        }
        run_receipt_id = _require_string(
            source.get("run_receipt_id"), "Gate-8 run receipt id")
        run_receipt_sha256 = _require_hex(
            source.get("run_receipt_sha256"),
            "Gate-8 run receipt SHA-256", 64)
        qualification_key = {
            "stable_key": stable_key,
            "test_case": source_item["test_case"],
            "group_index": source_item["group_index"],
            "query_count": runtime_cell["query_count"],
            "campaign_id": source["campaign_id"],
            "campaign_digest": source["campaign_digest"],
            "config_sha256": source["config_sha256"],
            "report_sha256": source["report_sha256"],
            "raw_sidecar_sha256": source["raw_sidecar_sha256"],
            "run_id": _require_string(source.get("run_id"), "Gate-8 run id"),
            "run_receipt_id": run_receipt_id,
            "run_receipt_sha256": run_receipt_sha256,
            "build_receipt_id": _require_string(
                build_receipt.get("build_receipt_id"),
                "Gate-8 build receipt id"),
            "build_receipt_sha256": _require_hex(
                build_receipt.get("content_sha256"),
                "Gate-8 build receipt SHA-256", 64),
            "runtime_cell_sha256": _sha256(_canonical_bytes(runtime_cell)),
            "raw_qualification_sha256": qualification_sha256,
            "fixture_qualification_sha256": _sha256(
                _canonical_bytes(fixture_qualification)),
            "fixture_projection_sha256": _require_hex(
                source.get("fixture_projection_sha256"),
                "Gate-8 fixture projection SHA-256", 64),
            "runtime_device": runtime_cell["device"],
            "source_repeat": 0,
            "source_record_checksum": source_record_checksum,
        }
        qualification_key_sha256 = _sha256(
            _canonical_bytes(qualification_key))
        for item in ordered:
            # Private aggregation evidence only; public candidate/result rows
            # continue to be built from their existing fixed projections.
            item["gate8_qualification_binding"] = {
                "schema": "bioseq.e2e.gate8-report-binding",
                "version": 2,
                "qualification_key": copy.deepcopy(qualification_key),
                "qualification_key_sha256": qualification_key_sha256,
                "qualification_sha256": qualification_sha256,
                "record_checksum": item["record_checksum"],
                "current_query_fixture_sha256": _sha256(_canonical_bytes(
                    item["gate8_sql"]["query_fixture"])),
            }


def _project_approved_gate8_sql_v2(
        records: list[dict[str, Any]], timing_scope: str) -> dict[str, Any]:
    """Project five validated raw v2 records into the closed private proof.

    Structured evidence uses canonical JSON; retained SQL and EXPLAIN use
    their exact UTF-8 bytes.
    """
    if len(records) != MIN_ACCEPTED_REPEATS:
        raise ContractError(
            "Gate-8 approved projection requires exactly five records")
    scope = _require_string(timing_scope, "Gate-8 approved timing scope")
    ordered = sorted(
        records,
        key=lambda item: _require_int(
            item.get("repeat"), "Gate-8 projected repeat"))
    if ([item["repeat"] for item in ordered]
            != list(range(MIN_ACCEPTED_REPEATS))
            or any(item.get("accepted") is not True for item in ordered)):
        raise ContractError(
            "Gate-8 approved projection requires five accepted repeats 0..4")
    stable_key = _require_string(
        ordered[0].get("stable_key"), "Gate-8 projected stable key")
    if (any(item.get("stable_key") != stable_key for item in ordered)
            or any(item.get("payload_schema_version")
                != RAW_PAYLOAD_SCHEMA_VERSION for item in ordered)):
        raise ContractError(
            "Gate-8 approved projection crossed a cell or raw schema")

    proofs = [
        _require_dict(
            item.get("gate8_sql"), f"Gate-8 projected proof[{ordinal}]")
        for ordinal, item in enumerate(ordered)
    ]
    if any(proof.get("version") != 2 for proof in proofs):
        raise ContractError("Gate-8 approved projection requires only v2 proofs")
    qualification = _require_dict(
        proofs[0].get("qualification"),
        "Gate-8 projected source qualification")
    qualification_bytes = _canonical_bytes(qualification)
    if any(_canonical_bytes(proof.get("qualification"))
           != qualification_bytes for proof in proofs[1:]):
        raise ContractError(
            "Gate-8 approved projection crossed source qualifications")
    qualification_sha256 = _sha256(qualification_bytes)

    report_bindings: list[dict[str, Any]] = []
    for ordinal, item in enumerate(ordered):
        report_binding = _require_dict(
            item.get("gate8_qualification_binding"),
            f"Gate-8 report binding[{ordinal}]")
        _require_exact_keys(report_binding, {
            "schema", "version", "qualification_key",
            "qualification_key_sha256", "qualification_sha256",
            "record_checksum", "current_query_fixture_sha256",
        }, f"Gate-8 report binding[{ordinal}]")
        if (report_binding.get("schema")
                != "bioseq.e2e.gate8-report-binding"
                or report_binding.get("version") != 2):
            raise ContractError(
                "Gate-8 approved projection lacks a v2 report binding")
        report_bindings.append(report_binding)

    qualification_key = _require_dict(
        report_bindings[0].get("qualification_key"),
        "Gate-8 projected qualification key")
    qualification_key_bytes = _canonical_bytes(qualification_key)
    qualification_key_sha256 = _sha256(qualification_key_bytes)
    source_record_checksum = _require_hex(
        ordered[0].get("record_checksum"),
        "Gate-8 projected source record checksum", 16)
    runtime_cell = _require_dict(
        qualification.get("runtime_cell"),
        "Gate-8 projected runtime cell")
    source_measurement = _require_dict(
        qualification.get("source_measurement"),
        "Gate-8 projected source measurement")
    source_query_fixture = _require_dict(
        qualification.get("query_fixture"),
        "Gate-8 projected source query fixture")
    source_postgres = _validate_gate8_postgres_identity(
        qualification.get("source_postgres"),
        "Gate-8 projected source PostgreSQL")
    source_proof = proofs[0]
    source_binding = _require_dict(
        source_proof.get("binding"), "Gate-8 projected source binding")
    runtime_dataset_key = _require_string(
        runtime_cell.get("dataset_key"),
        "Gate-8 projected runtime dataset key")
    runtime_target_state = _require_dict(
        runtime_cell.get("target_state"),
        "Gate-8 projected runtime target state")
    runtime_target_table = _require_string(
        runtime_cell.get("target_table"),
        "Gate-8 projected runtime target table")
    fixture_qualification = {
        "dataset_key": runtime_dataset_key,
        "target_state": runtime_target_state,
        "target_table": runtime_target_table,
        "query_fixture": source_query_fixture,
    }
    fixture_qualification_sha256 = _sha256(
        _canonical_bytes(fixture_qualification))
    query_fixture_sha256 = _sha256(
        _canonical_bytes(source_query_fixture))
    if (qualification.get("source_repeat") != 0
            or source_proof.get("qualification_order")
                != "after_source_measurement"
            or source_proof.get("measurement") != source_measurement
            or source_proof.get("query_fixture") != source_query_fixture
            or source_binding.get("role") != "source"
            or source_binding.get("source_epoch_is_current") is not True
            or source_binding.get("current_postgres") != source_postgres
            or source_binding.get("source_postgres") != source_postgres
            or qualification_key.get("stable_key") != stable_key
            or qualification_key.get("source_repeat") != 0
            or qualification_key.get("source_record_checksum")
                != source_record_checksum
            or qualification_key.get("runtime_cell_sha256")
                != _sha256(_canonical_bytes(runtime_cell))
            or qualification_key.get("raw_qualification_sha256")
                != qualification_sha256
            or qualification_key.get("fixture_qualification_sha256")
                != fixture_qualification_sha256):
        raise ContractError(
            "Gate-8 approved projection source/key ownership changed")

    detached = _require_dict(
        qualification.get("detached"),
        "Gate-8 projected detached qualification")
    accepted_gucs = _require_dict(
        detached.get("accepted_gucs"),
        "Gate-8 projected accepted GUCs")
    accepted_guc_values = _require_dict(
        accepted_gucs.get("values"),
        "Gate-8 projected accepted GUC values")
    explain_contract = _require_dict(
        detached.get("explain_contract"),
        "Gate-8 projected EXPLAIN contract")
    pipeline = _require_dict(
        explain_contract.get("pipeline"),
        "Gate-8 projected EXPLAIN pipeline")
    controls = _require_dict(
        detached.get("controls"), "Gate-8 projected controls")
    cache_control = _require_dict(
        controls.get("cache_off"), "Gate-8 projected cache control")
    vectorization_control = _require_dict(
        controls.get("vectorization_off"),
        "Gate-8 projected vectorization control")
    source_route = _require_dict(
        source_measurement.get("route"), "Gate-8 projected source route")
    source_counts = _require_list(
        source_measurement.get("ordered_primary_match_counts"),
        "Gate-8 projected source counts")
    source_cache = _require_dict(
        source_measurement.get("cache_evidence"),
        "Gate-8 projected source cache evidence")
    physical = _require_dict(
        source_query_fixture.get("physical_absence"),
        "Gate-8 projected query physical absence")
    absence_fields = (
        "source_parent_rows_read", "source_fragment_rows_read",
        "source_decoded_bp", "physical_index_relations",
        "persisted_index_rows", "generation_objects",
    )
    query_fixture_physical_no_index = all(
        isinstance(value, dict)
        and all(value.get(field) == 0 for field in absence_fields)
        for value in physical.values()
    ) and bool(physical)
    route_and_counts_unchanged = (
        detached.get("explain_route") == source_route
        and cache_control.get("route") == source_route
        and vectorization_control.get("route") == source_route
        and cache_control.get("ordered_primary_match_counts") == source_counts
        and vectorization_control.get("ordered_primary_match_counts")
            == source_counts
    )
    projected_cache = {
        "applicability": source_cache.get("applicability"),
        "gpu_persisted_resident_uploads":
            source_cache.get("gpu_persisted_resident_uploads"),
        "gpu_persisted_resident_cache_hits":
            source_cache.get("gpu_persisted_resident_cache_hits"),
    }

    bindings: list[dict[str, Any]] = []
    for ordinal, (item, proof, report_binding) in enumerate(zip(
            ordered, proofs, report_bindings)):
        measurement = _require_dict(
            proof.get("measurement"),
            f"Gate-8 projected measurement[{ordinal}]")
        current_query_fixture = _require_dict(
            proof.get("query_fixture"),
            f"Gate-8 projected query fixture[{ordinal}]")
        raw_binding = _require_dict(
            proof.get("binding"),
            f"Gate-8 projected raw binding[{ordinal}]")
        record_checksum = _require_hex(
            item.get("record_checksum"),
            f"Gate-8 projected record checksum[{ordinal}]", 16)
        current_query_fixture_sha256 = _sha256(
            _canonical_bytes(current_query_fixture))
        if (proof.get("qualification_order")
                != "after_source_measurement"
                or measurement != source_measurement
                or current_query_fixture != source_query_fixture
                or raw_binding.get("repeat") != ordinal
                or raw_binding.get("source_repeat") != 0
                or raw_binding.get("role")
                    != ("source" if ordinal == 0 else "reuse")
                or raw_binding.get("source_epoch_is_current")
                    != (ordinal == 0)
                or raw_binding.get("source_postgres") != source_postgres
                or report_binding.get("qualification_key")
                    != qualification_key
                or report_binding.get("qualification_key_sha256")
                    != qualification_key_sha256
                or report_binding.get("qualification_sha256")
                    != qualification_sha256
                or report_binding.get("record_checksum") != record_checksum
                or report_binding.get("current_query_fixture_sha256")
                    != current_query_fixture_sha256):
            raise ContractError(
                "Gate-8 approved projection binding ownership changed")
        current_postgres = _validate_gate8_postgres_identity(
            raw_binding.get("current_postgres"),
            f"Gate-8 projected current PostgreSQL[{ordinal}]")
        measurement_route = _require_dict(
            measurement.get("route"),
            f"Gate-8 projected measurement route[{ordinal}]")
        measurement_counts = _require_list(
            measurement.get("ordered_primary_match_counts"),
            f"Gate-8 projected measurement counts[{ordinal}]")
        measurement_sql = _require_string(
            measurement.get("sql"),
            f"Gate-8 projected measurement SQL[{ordinal}]")
        pre_statement_gucs = _require_dict(
            measurement.get("pre_statement_gucs"),
            f"Gate-8 projected pre-statement GUCs[{ordinal}]")
        bindings.append({
            "repeat": ordinal,
            "repeat_identity": (
                f"{stable_key}:timing_scope:{scope}:repeat:{ordinal}"),
            "role": raw_binding["role"],
            "record_checksum": record_checksum,
            "current_measurement_sha256": _sha256(
                _canonical_bytes(measurement)),
            "measurement_sql_sha256": _sha256(
                measurement_sql.encode("utf-8")),
            "pre_statement_gucs_sha256": _sha256(
                _canonical_bytes(pre_statement_gucs)),
            "ordered_primary_counts_sha256": _sha256(
                _canonical_bytes(measurement_counts)),
            "current_query_fixture_sha256": current_query_fixture_sha256,
            "current_postgres": copy.deepcopy(current_postgres),
            "source_postgres": copy.deepcopy(source_postgres),
            "qualification_key_sha256": qualification_key_sha256,
            "source_record_checksum": source_record_checksum,
            "source_epoch_is_current": raw_binding[
                "source_epoch_is_current"],
            "prefilter_activity_count": measurement_route.get(
                "prefilter_activity_count"),
            "route_and_ordered_counts_unchanged": (
                measurement_route == source_route
                and measurement_counts == source_counts),
        })

    explain_json_text = _require_string(
        detached.get("explain_json_text"),
        "Gate-8 projected EXPLAIN JSON")
    return {
        "schema": "bioseq.e2e.gate8-approved-cell-qualification",
        "version": 2,
        "raw_payload_schema_version": ordered[0]["payload_schema_version"],
        "qualification_count": 1,
        "repeat_count": len(ordered),
        "qualification": {
            "qualification_order": source_proof["qualification_order"],
            "qualification_key": copy.deepcopy(qualification_key),
            "qualification_key_sha256": qualification_key_sha256,
            "qualification_sha256": qualification_sha256,
            "source_repeat": qualification["source_repeat"],
            "source_repeat_identity": (
                f"{stable_key}:timing_scope:{scope}:repeat:0"),
            "source_record_checksum": source_record_checksum,
            "source_postgres": copy.deepcopy(source_postgres),
            "source_measurement_sha256": _sha256(
                _canonical_bytes(source_measurement)),
            "query_fixture_sha256": query_fixture_sha256,
            "explain_json_sha256": _sha256(
                explain_json_text.encode("utf-8")),
            "complete_guc_count": len(accepted_guc_values),
            "custom_scan_node_count": explain_contract.get(
                "custom_scan_node_count"),
            "custom_plan_provider": explain_contract.get(
                "custom_plan_provider"),
            "chaining_algorithm": pipeline.get("chaining_algorithm"),
            "base_alignment_algorithm": pipeline.get(
                "base_alignment_algorithm"),
            "pipeline_concurrency_mode": pipeline.get(
                "pipeline_concurrency_mode"),
            "query_fixture_physical_no_index":
                query_fixture_physical_no_index,
            "prefilter_activity_count": source_route.get(
                "prefilter_activity_count"),
            "route_and_ordered_counts_unchanged":
                route_and_counts_unchanged,
            "cache_control_disposition": cache_control.get("disposition"),
            "vectorization_control_disposition":
                vectorization_control.get("disposition"),
            "cache_evidence": projected_cache,
        },
        "bindings": bindings,
    }


def _load_report(
        report_path: Path,
        config_path: Path,
        campaign_contract: dict[str, Any],
        receipt_context: tuple[dict[str, Any], dict[str, Any]] | None = None,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    expected_report_dir = REPOSITORY_ROOT / "reporting/bioseq_benchmark_e2e"
    if report_path.parent != expected_report_dir:
        raise ContractError(
            "campaign reports must come from reporting/bioseq_benchmark_e2e")
    config, config_digest, config_entry = _validate_config(
        config_path, campaign_contract)
    report, report_raw = _read_json_file(report_path, "framework report")
    _validate_framework_seal(report, config, config_path, config_entry)
    records, manifest, sidecar_path, sidecar_digest = _validate_raw_sidecar(
        report_path, report, config, config_entry)
    source = {
        "campaign_id": campaign_contract["campaign_id"],
        "campaign_digest": campaign_contract["campaign_digest"],
        "campaign_contract": campaign_contract["contract_path"],
        "campaign_contract_sha256": campaign_contract["contract_sha256"],
        "report": _relative(report_path),
        "report_sha256": _sha256(report_raw),
        "raw_sidecar": _relative(sidecar_path),
        "raw_sidecar_sha256": sidecar_digest,
        "run_id": manifest.get("run_id"),
        "schedule_sorted_checksum": manifest.get("schedule_sorted_checksum"),
        "config": _relative(config_path),
        "config_sha256": config_digest,
        "config_role": config_entry["role"],
        "expected_stable_keys_sha256": config_entry[
            "expected_stable_keys_sha256"],
    }
    validated_build: dict[str, Any] | None = None
    if ACTIVE_CAMPAIGN_PROFILE == "observed_rung":
        source["execution_contract_id"] = campaign_contract[
            "execution_contract_id"]
    if config_entry.get("execution_authority") == "q8_rescue":
        sealed = _require_dict(
            config_entry.get("sealed_evidence"), "q8 rescue child evidence")
        report_evidence = _require_dict(
            sealed.get("report"), "q8 rescue report evidence")
        raw_evidence = _require_dict(
            report_evidence.get("raw_sidecar"), "q8 rescue raw evidence")
        if (report_evidence.get("path") != source["report"]
                or report_evidence.get("sha256") != source["report_sha256"]
                or raw_evidence.get("path") != source["raw_sidecar"]
                or raw_evidence.get("sha256") != source["raw_sidecar_sha256"]
                or report_evidence.get("run_id") != source["run_id"]
                or report_evidence.get("schedule_sorted_checksum")
                    != source["schedule_sorted_checksum"]):
            raise ContractError(
                "q8 rescue ingestion is not the manifest-sealed report/raw run")
    if ACTIVE_CAMPAIGN_PROFILE == "q1024":
        authority = config_entry.get("execution_authority")
        authority_names = {
            "q1024_singleton": "q1024_singleton_fallback",
            "q1024_packed": "q1024_packed_partition",
            "q1024_deadline_v2_adoption": "q1024_deadline_v2_adoption",
        }
        if authority not in authority_names or receipt_context is None:
            raise ContractError(
                "q1024 hybrid ingestion requires its sealed run receipt")
        run_receipt, build_receipt = receipt_context
        adoption = None
        receipt_config_path = config_path
        if authority == "q1024_deadline_v2_adoption":
            adoption = _require_dict(
                config_entry.get("sealed_evidence"), "q1024 adoption evidence")
            receipt_config = _require_dict(
                adoption.get("run_receipt_config"),
                "q1024 adoption run-receipt config")
            receipt_config_path = _canonical_repository_file(
                _require_string(
                    receipt_config.get("path"),
                    "q1024 adoption run-receipt config path"),
                "q1024 adoption run-receipt config")
        validated_run, validated_build = _validate_run_receipt_snapshot(
            run_receipt, build_receipt, report_path, receipt_config_path,
            "q1024 hybrid run receipt")
        if (adoption is not None
                and adoption.get("run_receipt_id") is not None
                and validated_run.get("build_receipt_id")
                    != adoption.get("run_receipt_id")):
            raise ContractError(
                "q1024 adoption is not its manifest-sealed run receipt")
        parent = _require_dict(
            config_entry.get("canonical_parent"), "q1024 canonical parent")
        source.update({
            "execution_authority": authority_names[authority],
            "partition_manifest": campaign_contract["execution_authority"],
            "partition_manifest_sha256": campaign_contract[
                "execution_authority_sha256"],
            "canonical_config": parent["path"],
            "canonical_config_sha256": parent["config_sha256"],
            "canonical_stable_keys_sha256": config_entry[
                "expected_stable_keys_sha256"],
            "run_receipt_id": validated_run["build_receipt_id"],
            "run_receipt_sha256": validated_run["content_sha256"],
            "run_receipt": copy.deepcopy(validated_run),
        })
        if adoption is not None:
            artifacts = adoption.get("sealed_artifacts")
            if artifacts is not None:
                sealed = _require_dict(artifacts, "q1024 adoption artifacts")
                report_seal = _require_dict(
                    sealed.get("report"), "q1024 adoption report seal")
                raw_seal = _require_dict(
                    sealed.get("raw_sidecar"), "q1024 adoption raw seal")
                if (report_seal.get("path") != source["report"]
                        or report_seal.get("sha256") != source["report_sha256"]
                        or raw_seal.get("path") != source["raw_sidecar"]
                        or raw_seal.get("sha256")
                            != source["raw_sidecar_sha256"]):
                    raise ContractError(
                        "q1024 adoption is not its manifest-sealed report/raw run")
    elif ACTIVE_CAMPAIGN_PROFILE == "observed_rung":
        if (receipt_context is None
                or config_entry.get("execution_authority")
                    != "observed_rung_singleton"):
            raise ContractError(
                "observed rung report requires its exact singleton authority "
                "and MCP run receipt")
        run_receipt, build_receipt = receipt_context
        validated_run, validated_build = _validate_run_receipt_snapshot(
            run_receipt, build_receipt, report_path, config_path,
            "observed rung run receipt")
        parent = _require_dict(
            config_entry.get("canonical_parent"),
            "observed singleton canonical parent")
        sealed = _require_dict(
            config_entry.get("sealed_evidence"),
            "observed singleton sealed evidence")
        if (sealed.get("partition_manifest")
                != campaign_contract["execution_authority"]
                or sealed.get("partition_manifest_sha256")
                    != campaign_contract["execution_authority_sha256"]
                or sealed.get("stable_key_sha256")
                    != config_entry["expected_stable_keys_sha256"]):
            raise ContractError(
                "observed singleton report lost partition/stable-key authority")
        source.update({
            "execution_authority": "observed_rung_singleton",
            "partition_manifest": campaign_contract["execution_authority"],
            "partition_manifest_sha256": campaign_contract[
                "execution_authority_sha256"],
            "canonical_config": parent["path"],
            "canonical_config_sha256": parent["config_sha256"],
            "canonical_stable_keys_sha256": config_entry[
                "expected_stable_keys_sha256"],
            "fixture_projection_sha256": campaign_contract[
                "fixture_projection_sha256"],
            "run_receipt_id": validated_run["build_receipt_id"],
            "run_receipt_sha256": validated_run["content_sha256"],
            "build_receipt_id": validated_build["build_receipt_id"],
            "build_receipt_sha256": validated_build["content_sha256"],
        })
    query_counts: dict[str, int] = {}
    mapq_projections: dict[str, bool] = {}
    cigar_projections: dict[str, bool] = {}
    paired_cases: dict[str, bool] = {}
    for test_case, test_case_config in _require_dict(
            config.get("test_cases"), "config test_cases").items():
        data = _require_dict(
            _require_dict(test_case_config, f"test case {test_case}").get("data"),
            f"test case {test_case} data",
        )
        query_counts[test_case] = _require_int(
            data.get("fragment_count"), f"{test_case} fragment_count", minimum=1)
        mapq_projections[test_case] = _require_bool(
            data.get("mapq_projection"), f"{test_case} mapq_projection")
        cigar_projections[test_case] = _require_bool(
            data.get("cigar_projection"), f"{test_case} cigar_projection")
        paired_cases[test_case] = _require_bool(
            data.get("paired_minimap2_oracle", False), f"{test_case} paired_minimap2_oracle")
        if (mapq_projections[test_case] != campaign_contract["mapq_projection"]
                or cigar_projections[test_case]
                    != campaign_contract["cigar_projection"]):
            raise ContractError(
                "execution config projection flags differ from sealed campaign authority")
    validated: list[dict[str, Any]] = []
    seen: set[tuple[str, int]] = set()
    for record in records:
        test_case = _require_string(record.get("test_case"), "record test_case")
        if test_case not in query_counts:
            raise ContractError("raw record names a non-configured test case")
        item = _validate_record(
            record,
            source,
            campaign_contract["campaign_id"],
            query_counts[test_case],
            mapq_projections[test_case],
            cigar_projections[test_case],
            campaign_contract["prefilter_enabled"],
        )
        descriptor = config_entry["descriptors_by_key"].get(item["stable_key"])
        execution_descriptor = config_entry[
            "execution_descriptors_by_key"].get(item["stable_key"])
        if descriptor is None or execution_descriptor is None:
            raise ContractError("raw stable key is not permitted by the canonical config")
        if (descriptor["identity"] != item["identity"]
                or execution_descriptor["test_case"] != test_case
                or execution_descriptor["algorithm"] != item["algorithm"]
                or execution_descriptor["tuple_index"]
                    != record.get("tuple_index")
                or execution_descriptor["params"] != record.get("params")):
            raise ContractError("raw record differs from its canonical cell descriptor")
        key_repeat = (item["stable_key"], item["repeat"])
        if key_repeat in seen:
            raise ContractError("report duplicates a canonical stable key/repeat")
        seen.add(key_repeat)
        validated.append(item)
    expected = {
        (stable_key, repeat)
        for stable_key in config_entry["descriptors_by_key"]
        for repeat in range(MIN_ACCEPTED_REPEATS)
    }
    if seen != expected:
        raise ContractError("report stable-key/repeat membership is not the exact config set")
    gate8_source = source
    if ACTIVE_CAMPAIGN_PROFILE == "q1024":
        gate8_source = dict(source)
        gate8_source["fixture_projection_sha256"] = campaign_contract[
            "fixture_projection_sha256"]
    _validate_gate8_cell_qualifications(validated, gate8_source, validated_build)
    _validate_paired_repeat_oracles(validated, paired_cases)
    _validate_config_disposition_records(validated, config_entry)
    return validated, source


def _observed_rung_path_classification(item: dict[str, Any]) -> str:
    """Classify one already-validated result without reopening route proof."""
    identity = item["identity"]
    if identity["engine"] != "bioseq":
        return OBSERVED_RUNG_BASELINE_PATH

    observed = item["observed"]
    transitions = item["transition_classifications"]
    exact_transitions = {
        "minimizer": "exact",
        "chaining": "exact",
        "base_alignment": "exact",
    }
    requested_algorithms = (
        identity["requested_minimizer_algorithm"],
        identity["requested_chaining_algorithm"],
        identity["requested_base_alignment_algorithm"],
    )
    observed_algorithms = (
        observed["observed_minimizer_algorithm"],
        observed["observed_chaining_algorithm"],
        observed["observed_base_alignment_algorithm"],
    )
    if (requested_algorithms == observed_algorithms
            and transitions == exact_transitions
            and item["disposition"] != "accepted_fallback"):
        return OBSERVED_RUNG_REQUESTED_PATH_ELIGIBLE

    documented_dna5_reference_scalar = (
        identity["alphabet"] == "dna5"
        and identity["requested_minimizer_algorithm"]
            == "simd_minimap2_reference"
        and observed["observed_minimizer_algorithm"] == "scalar"
        and observed["observed_chaining_algorithm"]
            == identity["requested_chaining_algorithm"]
        and observed["observed_base_alignment_algorithm"]
            == identity["requested_base_alignment_algorithm"]
        and transitions == {
            "minimizer": "documented_minimizer_fallback",
            "chaining": "exact",
            "base_alignment": "exact",
        }
        and item["disposition"] == "accepted_fallback"
    )
    if documented_dna5_reference_scalar:
        return OBSERVED_RUNG_REQUESTED_PATH_INELIGIBLE
    raise ContractError(
        "observed rung report contains a fallback substitution outside the "
        "DNA5 reference-to-scalar requested-path-ineligible exception")


def _observed_rung_path_classifications(
        records: list[dict[str, Any]]) -> dict[str, str]:
    """Return the one stable path classification owned by each result cell."""
    result: dict[str, str] = {}
    for item in records:
        stable_key = item["stable_key"]
        classification = _observed_rung_path_classification(item)
        prior = result.setdefault(stable_key, classification)
        if prior != classification:
            raise ContractError(
                "observed rung fallback substitution changed across repeats")
    return result


def _validated_observed_rung_stable_keys(
        records: list[dict[str, Any]]) -> set[str]:
    """Require accepted status and exact repeat ownership for validated records."""
    if any(not item["accepted"] or item["record_status"] != "accepted"
           for item in records):
        raise ContractError(
            "observed rung report contains a nonpublishable cell status")
    path_classifications = _observed_rung_path_classifications(records)
    stable_keys = {item["stable_key"] for item in records}
    if set(path_classifications) != stable_keys:
        raise ContractError(
            "observed rung path classifications lost result-cell ownership")
    if (not stable_keys
            or len(records) != len(stable_keys) * MIN_ACCEPTED_REPEATS):
        raise ContractError(
            "observed rung report lost exact five-repeat cell ownership")
    for stable_key in stable_keys:
        repeats = sorted(
            item["repeat"] for item in records
            if item["stable_key"] == stable_key)
        if repeats != list(range(MIN_ACCEPTED_REPEATS)):
            raise ContractError(
                "observed rung report lost exact repeat ordinals 0..4")
    return stable_keys


def validate_observed_rung_report(
        manifest_path: Path,
        expected_campaign_digest: str,
        query_count: int,
        report_path: Path,
        config_path: Path,
        run_receipt_id: str,
) -> dict[str, Any]:
    """Validate one exact rung report without creating a results authority."""
    _activate_observed_rung_profile(
        manifest_path, expected_campaign_digest, query_count)
    campaign_contract = _load_campaign_contract()
    receipt_context = _load_live_run_receipt_context(
        run_receipt_id, report_path, config_path,
        label="incoming observed rung run receipt")
    records, source = _load_report(
        report_path, config_path, campaign_contract, receipt_context)
    stable_keys = _validated_observed_rung_stable_keys(records)
    if len(stable_keys) != 1:
        raise ContractError(
            "observed singleton receipt must own exactly one canonical stable key")
    stable_key = next(iter(stable_keys))
    repeat_count_evidence: list[dict[str, Any]] = []
    for item in sorted(records, key=lambda value: value["repeat"]):
        oracle = _require_dict(
            item.get("ordered_oracle"), "observed ordered-count oracle")
        if oracle.get("sealed") is not False:
            raise ContractError(
                "observed ordinary singleton must retain an unpaired ordered-count oracle")
        repeat_count_evidence.append({
            "repeat": item["repeat"],
            "ordered_count_sha256": _require_hex(
                oracle.get("count_digest"),
                "observed ordered-count digest", 64),
            "total_match_count": item["match_count"],
        })
    if ([entry["repeat"] for entry in repeat_count_evidence]
            != list(range(MIN_ACCEPTED_REPEATS))):
        raise ContractError("observed singleton count proof lost repeats 0..4")
    path_classifications = _observed_rung_path_classifications(records)
    path_classification_counts = {
        classification: sum(
            value == classification
            for value in path_classifications.values())
        for classification in (
            OBSERVED_RUNG_BASELINE_PATH,
            OBSERVED_RUNG_REQUESTED_PATH_ELIGIBLE,
            OBSERVED_RUNG_REQUESTED_PATH_INELIGIBLE,
        )
    }
    return {
        "schema": "bioseq.e2e.observed-rung-validation-receipt",
        "version": 1,
        "status": "accepted",
        "campaign_id": campaign_contract["campaign_id"],
        "campaign_digest": campaign_contract["campaign_digest"],
        "execution_contract_id": campaign_contract["execution_contract_id"],
        "result_status_contract": campaign_contract[
            "result_status_contract"],
        "query_count": campaign_contract["query_count"],
        "predecessor_execution_contract_id": campaign_contract[
            "predecessor_execution_contract_id"],
        "manifest": source["campaign_contract"],
        "manifest_sha256": source["campaign_contract_sha256"],
        "partition_manifest": source["partition_manifest"],
        "partition_manifest_sha256": source["partition_manifest_sha256"],
        "config": source["config"],
        "config_sha256": source["config_sha256"],
        "canonical_parent_config": source["canonical_config"],
        "canonical_parent_config_sha256": source[
            "canonical_config_sha256"],
        "stable_key": stable_key,
        "stable_key_sha256": _sha256(stable_key.encode("utf-8")),
        "fixture_projection_sha256": source[
            "fixture_projection_sha256"],
        "report": source["report"],
        "report_sha256": source["report_sha256"],
        "raw_sidecar": source["raw_sidecar"],
        "raw_sidecar_sha256": source["raw_sidecar_sha256"],
        "run_receipt_id": source["run_receipt_id"],
        "run_receipt_sha256": source["run_receipt_sha256"],
        "build_receipt_id": source["build_receipt_id"],
        "build_receipt_sha256": source["build_receipt_sha256"],
        "result_cell_count": len(stable_keys),
        "raw_record_count": len(records),
        "repeat_count_evidence": repeat_count_evidence,
        "path_classification_counts": path_classification_counts,
    }


def _nearest_rank_p50(values: list[int]) -> int:
    if len(values) < MIN_ACCEPTED_REPEATS:
        raise ContractError("P50 requires at least five accepted repeats")
    ordered = sorted(values)
    return ordered[(50 * len(ordered) + 99) // 100 - 1]


def _evidence_refs(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_report: dict[str, list[dict[str, Any]]] = {}
    for record in records:
        by_report.setdefault(record["source"]["report"], []).append(record)
    result: list[dict[str, Any]] = []
    for report in sorted(by_report):
        selected = sorted(by_report[report], key=lambda item: item["repeat"])
        source = selected[0]["source"]
        reference = {
            "report": report,
            "raw_sidecar": source["raw_sidecar"],
            "run_id": source["run_id"],
            "schedule_sorted_checksum": source["schedule_sorted_checksum"],
            "record_count": len(selected),
            "record_checksum_digest": hashlib.sha256(
                "".join(item["record_checksum"] for item in selected).encode("ascii")
            ).hexdigest(),
        }
        if ACTIVE_CAMPAIGN_PROFILE == "q1024":
            reference.update({
                "run_receipt_id": source["run_receipt_id"],
                "run_receipt_sha256": source["run_receipt_sha256"],
            })
        result.append(reference)
    return result


def _aggregate(records: list[dict[str, Any]], *, expected_repeats: int = MIN_ACCEPTED_REPEATS) -> list[dict[str, Any]]:
    if expected_repeats not in (1, MIN_ACCEPTED_REPEATS):
        raise ContractError("unsupported explicit repeat policy")
    cells: dict[str, list[dict[str, Any]]] = {}
    repeats_seen: set[tuple[str, int]] = set()
    baselines: dict[tuple[str, str, str, int], dict[str, Any]] = {}
    for record in records:
        duplicate_key = (record["stable_key"], record["repeat"])
        if duplicate_key in repeats_seen:
            raise ContractError(
                "duplicate/conflicting stable cell repeat across shard reports: "
                f"{duplicate_key[0]} repeat={duplicate_key[1]}"
            )
        repeats_seen.add(duplicate_key)
        cells.setdefault(record["stable_key"], []).append(record)
        if record["identity"]["engine"] == "minimap2":
            baseline_key = (
                record["identity"]["campaign_id"],
                record["identity"]["dataset"],
                record["identity"]["index_mode"],
                record["repeat"],
            )
            if baseline_key in baselines:
                raise ContractError(f"duplicate minimap2 baseline repeat {baseline_key}")
            baselines[baseline_key] = record

    rows: list[dict[str, Any]] = []
    for stable_key in sorted(cells):
        cell = sorted(cells[stable_key], key=lambda item: item["repeat"])
        identity = cell[0]["identity"]
        if any(item["identity"] != identity for item in cell):
            raise ContractError(f"stable key collision for {stable_key}")
        invalid: list[str] = []
        rejected: list[str] = []
        pending: list[str] = []
        behavior = sorted({item["behavior_comment"] for item in cell if item["behavior_comment"]})
        transition_comments: list[str] = []
        for item in cell:
            if not item["accepted"]:
                continue
            classifications = item["transition_classifications"]
            for axis, classification in classifications.items():
                if classification in {"exact", "not_applicable"}:
                    continue
                identity_axis = (
                    "requested_base_alignment_algorithm"
                    if axis == "base_alignment"
                    else f"requested_{axis}_algorithm")
                observed_axis = identity_axis.replace("requested_", "observed_", 1)
                prefix = "FALLBACK" if "fallback" in classification else "ROUTE"
                transition_comments.append(
                    f"{prefix}: requested {identity_axis}={identity[identity_axis]} "
                    f"-> observed {observed_axis}={item['observed'][observed_axis]} "
                    f"classification={classification}")
        terminal_kinds = {item["record_status"] for item in cell}
        if len(terminal_kinds) != 1:
            invalid.append(
                "**INVALID**: tuple disposition changed across canonical repeats")
        rejection_shapes = {
            (item["disposition"], item["rejection_reason"])
            for item in cell if item["record_status"] == "rejected"
        }
        if len(rejection_shapes) > 1:
            invalid.append(
                "**INVALID**: expected capability rejection changed across repeats")
        for item in cell:
            engine_label = "Bioseq" if identity["engine"] == "bioseq" else "minimap2"
            if item["accepted"] and not item["correctness_evaluated"]:
                invalid.append(
                    f"**INVALID**: accepted {engine_label} repeat lacks evaluated "
                    f"correctness evidence for dataset={identity['dataset']}, "
                    f"mode={identity['index_mode']}, repeat={item['repeat']}"
                )
            if item["correctness_evaluated"] and (
                item["match_count"] != item["expected_match_count"]
                or item["correct_match_count"] != item["expected_match_count"]
            ):
                invalid.append(
                    f"**INVALID**: observed {engine_label} match_count="
                    f"{item['match_count']} and correct_match_count="
                    f"{item['correct_match_count']} versus internal "
                    f"expected_match_count={item['expected_match_count']} for "
                    f"dataset={identity['dataset']}, mode={identity['index_mode']}, "
                    f"repeat={item['repeat']}"
                )
            if item["record_status"] == "invalid":
                reason = item["rejection_reason"]
                invalid.append(reason if reason.startswith("**INVALID**:") else
                               f"**INVALID**: {reason}")
            elif item["record_status"] == "rejected":
                rejected.append(item["rejection_reason"])
            if (identity["engine"] == "bioseq"
                    and item["record_status"] == "accepted"):
                baseline_key = (
                    identity["campaign_id"],
                    identity["dataset"],
                    identity["index_mode"],
                    item["repeat"],
                )
                baseline = baselines.get(baseline_key)
                if baseline is None:
                    pending.append(
                        "corresponding minimap2 baseline is missing for "
                        f"repeat={item['repeat']}"
                    )
                    continue
                if baseline["record_status"] != "accepted":
                    invalid.append(
                        "**INVALID**: corresponding minimap2 baseline is not "
                        f"accepted/valid for dataset={identity['dataset']}, "
                        f"mode={identity['index_mode']}, repeat={item['repeat']}"
                    )
                    continue
                if (
                    not baseline["correctness_evaluated"]
                    or baseline["match_count"] != baseline["expected_match_count"]
                    or baseline["correct_match_count"]
                        != baseline["expected_match_count"]
                ):
                    invalid.append(
                        "**INVALID**: corresponding minimap2 baseline lacks "
                        "accepted internal expected-count evidence for "
                        f"dataset={identity['dataset']}, mode={identity['index_mode']}, "
                        f"repeat={item['repeat']}"
                    )
                    continue
                if item["match_count"] != baseline["match_count"]:
                    invalid.append(
                        "**INVALID**: observed Bioseq match_count="
                        f"{item['match_count']} versus minimap2 match_count="
                        f"{baseline['match_count']} for dataset={identity['dataset']}, "
                        f"mode={identity['index_mode']}, repeat={item['repeat']}"
                    )
                    continue
                bioseq_oracle = item["ordered_oracle"]
                baseline_oracle = baseline["ordered_oracle"]
                if bioseq_oracle is not None and baseline_oracle is not None:
                    # JSON recovery caches encode validated tuples as lists.
                    # Compare the ordered values, not their Python container type.
                    if tuple(bioseq_oracle["counts"]) != tuple(baseline_oracle["counts"]):
                        invalid.append(
                            "**INVALID**: observed Bioseq ordered primary-count "
                            "vector differs from the same-repeat minimap2 baseline "
                            f"for dataset={identity['dataset']}, "
                            f"mode={identity['index_mode']}, repeat={item['repeat']}"
                        )
                elif (item["payload_schema_version"]
                          == RAW_PAYLOAD_SCHEMA_VERSION
                      or baseline["payload_schema_version"]
                          == RAW_PAYLOAD_SCHEMA_VERSION):
                    invalid.append(
                        "**INVALID**: current-schema Bioseq/minimap2 "
                        "same-repeat pair lacks independently consumed ordered "
                        "primary-count vectors "
                        f"for dataset={identity['dataset']}, "
                        f"mode={identity['index_mode']}, repeat={item['repeat']}"
                    )
        observed_values = {
            tuple(item["observed"].items()) for item in cell if item["accepted"]
        }
        transition_values = {
            tuple(sorted(item["transition_classifications"].items()))
            for item in cell if item["accepted"]
        }
        if len(observed_values) > 1:
            invalid.append("**INVALID**: observed algorithm identities changed across repeats")
        if len(transition_values) > 1:
            invalid.append(
                "**INVALID**: requested-to-observed route classification changed across repeats")
        if len(cell) != expected_repeats \
                or [item["repeat"] for item in cell] != list(range(expected_repeats)):
            invalid.append("**INVALID**: cell does not own the exact configured repeat ordinals")

        if invalid:
            status = "invalid"
            comments = "; ".join(dict.fromkeys(
                invalid + rejected + pending + behavior + transition_comments))
        elif rejected:
            status = "rejected"
            comments = "REJECTED: " + "; ".join(dict.fromkeys(
                rejected + pending + behavior + transition_comments))
        elif pending:
            status = "pending"
            comments = "PENDING: " + "; ".join(dict.fromkeys(
                pending + behavior + transition_comments))
        else:
            status = "accepted"
            accepted_comments = list(dict.fromkeys(
                behavior + transition_comments))
            comments = "; ".join(accepted_comments) if accepted_comments else None

        accepted_for_timing = status == "accepted" and all(item["accepted"] for item in cell)
        outer_p50 = (cell[0]["outer_e2e_ns"] if expected_repeats == 1 else _nearest_rank_p50(
            [item["outer_e2e_ns"] for item in cell])) if accepted_for_timing else None
        adjusted_p50 = (cell[0]["adjusted_ns"] if expected_repeats == 1 else _nearest_rank_p50(
            [item["adjusted_ns"] for item in cell])) if accepted_for_timing else None
        observed = dict(observed_values.pop()) if len(observed_values) == 1 else {
            "observed_minimizer_algorithm": None,
            "observed_chaining_algorithm": None,
            "observed_base_alignment_algorithm": None,
        }
        evidence_records = list(cell)
        if identity["engine"] == "bioseq":
            for item in cell:
                baseline = baselines.get((
                    identity["campaign_id"],
                    identity["dataset"],
                    identity["index_mode"],
                    item["repeat"],
                ))
                if baseline is not None:
                    evidence_records.append(baseline)
        rows.append({
            "stable_key": stable_key,
            **identity,
            **observed,
            "status": status,
            "run_count": len(cell),
            "outer_e2e_ns_p50": outer_p50,
            "outer_e2e_minus_cigar_or_mapq_union_ns_p50": adjusted_p50,
            "COMMENTS": comments,
            "evidence_refs": _evidence_refs(evidence_records),
        })
    return rows


def _expected_all_results_contract(
        campaign_contract: dict[str, Any]) -> dict[str, Any]:
    contract = {
        "schema": EXPECTED_LEDGER_CONTRACT_SCHEMA,
        "version": EXPECTED_LEDGER_CONTRACT_VERSION,
        "campaign_id": campaign_contract["campaign_id"],
        "campaign_digest": campaign_contract["campaign_digest"],
        "campaign_contract": campaign_contract["contract_path"],
        "campaign_contract_sha256": campaign_contract["contract_sha256"],
        "encoding": "JSON (YAML 1.2 subset)",
        "raw_framework_schema": 1,
        "raw_payload_schema": "bioseq.e2e.raw-sample",
        "raw_payload_schema_version": RAW_PAYLOAD_SCHEMA_VERSION,
        "old_or_mixed_schema_policy": RAW_PAYLOAD_SCHEMA_POLICY,
        "minimum_accepted_repeats": MIN_ACCEPTED_REPEATS,
        "stable_key_schema": "bioseq-e2e-cell-v2",
        "stable_key_fields": list(STABLE_KEY_FIELDS),
        "stable_key_encoding": (
            "bioseq-e2e-cell-v2 followed by the compact JSON array of the "
            "ordered field values"),
        "repeat_identity": "stable_key plus framework envelope repeat",
        "duplicate_policy": (
            "The same stable_key/repeat may occur once across all processed "
            "reports; a second occurrence is rejected as a duplicate or conflict."),
        "idempotency_policy": (
            "Reprocessing the same repository-relative report with unchanged "
            "report/config/raw-sidecar digests is a no-op. Changed evidence at a "
            "registered path and identical report bytes under another path are rejected."),
        "metric_a": {
            "name": "outer_e2e_ns_p50",
            "source": (
                "same accepted raw record framework envelope outer_e2e_ns"),
            "aggregation": "exact integer nearest-rank P50",
        },
        "metric_b": {
            "name": "outer_e2e_minus_cigar_or_mapq_union_ns_p50",
            "per_repeat_formula": "outer_e2e_ns - cigar_or_mapq_interval_union_ns",
            "aggregation": "exact integer nearest-rank P50 after per-repeat subtraction",
            "forbidden": [
                "subtract independent medians",
                "sum overlapping physical intervals",
                "subtract downstream_active_wall_ns",
            ],
        },
        "timing_acceptance": {
            "required": [
                "complete framework raw sidecar and matching embedded records",
                "rejected E2E raw records have null framework outer timing and are excluded from framework wall-clock, region timing, and duration summaries",
                "every rejected or invalid raw field named *_ns or *_ns_manual is null",
                "accepted tuple and correctness evidence",
                "one accepted interval receipt",
                "proven compatible monotonic duration clock rates",
                "cigar_or_mapq_interval_union_ns <= the same record outer_e2e_ns",
                "framework-derived adjusted integer equals outer minus union",
                "at least five accepted repeats",
                "exact per-repeat Bioseq/minimap2 match-count parity",
            ],
            "rejected_or_invalid_timing": None,
            "raw_timing_arrays_in_all_results": False,
        },
        "clock_semantics": {
            "framework_outer": "duration sampled by bioseq.instrumentation.clock::now_ns",
            "bioseq_intervals": (
                "monotonic nanosecond durations from revision-6 statement evidence"),
            "minimap2_intervals": (
                "monotonic nanosecond durations from helper-declared "
                "CLOCK_MONOTONIC receipt v2"),
            "same_repeat_proof": "framework program order and one finalized raw envelope",
            "minimap2_timestamp_containment": (
                "parent-observed child-lifetime envelope only"),
            "framework_outer_absolute_timestamp_containment_claimed": False,
            "duration_rate_gate": (
                "Bioseq intervals require the identical framework clock implementation. "
                "Local macOS minimap2 intervals require the framework mach_absolute_time "
                "and helper CLOCK_MONOTONIC durations to declare the same kernel monotonic "
                "nanosecond rate. Other rate pairs, including Linux CLOCK_MONOTONIC_RAW "
                "versus CLOCK_MONOTONIC, fail closed."),
            "subtraction_basis": (
                "nonnegative monotonic nanosecond durations with a proven equal rate; "
                "no absolute timestamps from different domains are subtracted or compared"),
        },
        "match_parity": {
            "join_keys": ["campaign_id", "dataset", "index_mode", "repeat"],
            "baseline_by_mode": {
                "no_index": "minimap2_no_index",
                "just_index": "minimap2_just_index",
            },
            "required_equality": "exact integer match_count",
            "internal_expected_count_mismatch": "invalid",
            "cross_engine_mismatch": "invalid",
            "invalid_comments_prefix": "**INVALID**:",
        },
        "result_row_timing_fields": [
            "run_count",
            "outer_e2e_ns_p50",
            "outer_e2e_minus_cigar_or_mapq_union_ns_p50",
        ],
        "non_timing_result_fields": [
            "stable identity",
            "observed algorithm identities",
            "status",
            "COMMENTS",
            "compact evidence_refs",
        ],
    }
    if ACTIVE_CAMPAIGN_PROFILE == "q1024":
        contract.update({
            "execution_partition": {
                "schema": (
                    "bioseq.e2e.algorithm-matrix-q1024-packed-partition"),
                "manifest": campaign_contract["execution_authority"],
                "manifest_sha256": campaign_contract[
                    "execution_authority_sha256"],
                "singleton_manifest": campaign_contract[
                    "singleton_execution_authority"],
                "singleton_manifest_sha256": campaign_contract[
                    "singleton_execution_authority_sha256"],
                "canonical_parent_configs": 88,
                "packed_configs": 25,
                "adopted_configs": 4,
                "singleton_fallback_configs": 1142,
                "execution_configs": 1171,
                "packed_cells": 150,
                "adopted_cells": 4,
                "singleton_fallback_cells": 1142,
                "records_per_cell": 5,
                "exact_disjoint_hybrid_union": True,
                "canonical_topology_and_digest_unchanged": True,
            },
            "run_receipt_policy": {
                "incoming_receipt_store": str(BUILD_RECEIPT_STORE_PATH),
                "required_kind": "bioseq-run-receipt",
                "required_schema_revision": 1,
                "required_mode": "benchmark-e2e",
                "required_filter": ["--filter", "benchmark=bench_e2e"],
                "required_debug": False,
                "required_preset": "bench-e2e-opt",
                "processed_reports_embed_full_run_receipt": True,
                "aggregation_state_deduplicates_full_build_receipts": True,
                "historical_receipt_revalidation": (
                    "embedded immutable content seals and artifact bindings; "
                    "no comparison with later unrelated source edits"),
                "new_run_freshness_authority": (
                    "build_mcp/build_worker.py revalidates the current build "
                    "receipt before sealing the run receipt"),
            },
            "legacy_deadline_v2_checkpoint_policy": {
                "optional_top_level_field": "deadline_v2_checkpoint",
                "schema": "bioseq.e2e.deadline-v2-audited-checkpoint",
                "version": 1,
                "canonical_json_sha256": (
                    Q1024_DEADLINE_V2_CHECKPOINT_SHA256),
                "updated_through": "s058",
                "literal_q1024_anchor_cells": 3,
                "publication_membership": False,
                "preservation": (
                    "retain byte-semantic content unchanged while adopted "
                    "q1024 reports are independently revalidated and merged"),
            },
        })
    if QUERY_PREFIX_PROOF_PATH is not None:
        contract["query_prefix_proof"] = campaign_contract[
            "query_prefix_proof"]
        contract["query_prefix_proof_sha256"] = campaign_contract[
            "query_prefix_proof_sha256"]
    return contract


def _current_all_results_contract_for_document(
        campaign_contract: dict[str, Any],
        document: dict[str, Any],
) -> dict[str, Any]:
    """Build the current contract while retaining its sealed checkpoint policy."""
    contract = _expected_all_results_contract(campaign_contract)
    if (ACTIVE_CAMPAIGN_PROFILE == "q1024"
            and _q1024_s059_preservation_checkpoint_is_exact(
                document.get("deadline_v2_checkpoint"))):
        contract["legacy_deadline_v2_checkpoint_policy"] = {
            "optional_top_level_field": "deadline_v2_checkpoint",
            "schema": "bioseq.e2e.deadline-v2-audited-checkpoint",
            "version": 1,
            "canonical_json_sha256": (
                Q1024_S059_PRESERVATION_CHECKPOINT_SHA256),
            "updated_through": "s059",
            "literal_q1024_anchor_cells": 4,
            "publication_membership": True,
            "preservation": (
                "The explicit user preservation rule admits audited s059 as "
                "one publication result outside aggregation_state.processed_reports: "
                "report/footer and five raw records are complete, two independent "
                "audits found the receipt-authorized executable, dylib, and all 43 "
                "authorized artifacts unchanged, and all post-launch source drift "
                "was confined to benchmark tooling. No run receipt is invented; "
                "the wrapper postflight failure remains disclosed."),
        }
    return contract


def _expected_all_results_contract_for_document(
        campaign_contract: dict[str, Any],
        document: dict[str, Any],
) -> dict[str, Any]:
    """Accept the exact raw-v11 contract until current evidence is ingested."""
    current = _current_all_results_contract_for_document(
        campaign_contract, document)
    legacy = _legacy_all_results_contract(current)
    return legacy if document.get("contract") == legacy else current


def _legacy_all_results_contract(
        current: dict[str, Any],
) -> dict[str, Any]:
    """Project the one retained predecessor without weakening any other field."""
    legacy = copy.deepcopy(current)
    legacy["raw_payload_schema_version"] = LEGACY_RAW_PAYLOAD_SCHEMA_VERSION
    legacy["old_or_mixed_schema_policy"] = "reject"
    return legacy


def _expected_matrix_contract() -> dict[str, Any]:
    return {
        "target_size_gbp": [2, 4],
        "alphabet": ["dna4", "dna5"],
        "window_kmer": [
            {"window_size": 10, "kmer_size": 15},
            {"window_size": 15, "kmer_size": 20},
        ],
        "query_count": EXPECTED_QUERY_COUNT,
        "cigar_projection": True,
        "mapq_projection": True,
        "prefilter_enabled": False,
        "index_mode": ["no_index", "just_index"],
        "bioseq_index_name": [
            "minimizers_per_fragment", "minimizers_per_sequence"],
        "repetitions_per_cell": MIN_ACCEPTED_REPEATS,
        "minimizer_algorithms": [
            "scalar",
            "simd_minimap2_reference",
            "two_stack_nthash",
            "vulkan_minimap_integer",
            "bounded_nthash_simd",
        ],
        "chaining_algorithms": [
            "scalar_minimap2",
            "simd_minimap2",
            "range_minimum_query",
            "vulkan_tiled_dynamic_programming",
        ],
        "base_alignment_algorithms": ["ksw2", "block_aligner"],
        "explicitly_excluded_base_alignment_algorithms": ["ksw2_batched"],
        "bioseq_result_cells": 1280,
        "minimap2_baseline_result_cells": 16,
        "expected_result_cells": EXPECTED_RESULT_CELLS,
        "expected_raw_records": EXPECTED_RESULT_CELLS * MIN_ACCEPTED_REPEATS,
    }


def _expected_wiring() -> dict[str, Any]:
    if ACTIVE_CAMPAIGN_PROFILE == "q8":
        return {
            "campaign_contract": (
                "bench/bench_e2e_algorithm_matrix_q8_campaign_manifest.json"),
            "query_prefix_proof": (
                "bench/bench_e2e_algorithm_matrix_q8_query_prefix_proof.json"),
            "shard_generator": "bench/generate_e2e_algorithm_matrix_shards.py",
            "serial_shard_directory": "bench/e2e_algorithm_matrix_q8_shards",
            "serial_shard_count": EXPECTED_SHARD_CONFIGS,
            "builder_config_override": (
                "Pass bench_e2e_config=bench/"
                "bench_e2e_algorithm_matrix_q8_campaign.json or one of the "
                "exact 256 q8-manifest-bound shard paths to bioseq-build "
                "start_build mode benchmark-e2e."),
            "mcp_run_only_example": {
                "mode": "benchmark-e2e",
                "run_only": True,
                "reuse_build_receipt":
                    "<fresh successful benchmark-e2e build receipt>",
                "bench_e2e_config": (
                    "bench/e2e_algorithm_matrix_q8_shards/"
                    "bench_e2e_algorithm_matrix_q8_campaign_shard_"
                    "m2_d4_k15_w10_isolated_minimap2_no_index.json"),
                "filter": {"benchmarks": ["bench_e2e"]},
            },
            "automatic_consumer": (
                "build_mcp/build_worker.py runs the q8-bound merger only after "
                "a successful explicit q8 manifest member and before marking "
                "the worker completed."),
            "manual_consumer_invocation": (
                "python3 bench/aggregate_e2e_algorithm_matrix.py --config "
                "bench/e2e_algorithm_matrix_q8_shards/<exact-q8-shard>.json "
                "--report reporting/bioseq_benchmark_e2e/report_<N>.json "
                "--all-results Q8_RESULTS.yaml"),
            "proof_policy": (
                "The exact q8 query-prefix proof and its SHA-256 are mandatory "
                "aggregation authority; q1024 reports/results are never merged."),
            "shard_resume": (
                "Each invocation revalidates and recomputes from every q8 "
                "processed report and external raw sidecar. Cells lacking "
                "repeats or their isolated minimap2 baseline remain pending "
                "with null timing until later disjoint q8 shards supply evidence."),
        }
    return {
        "campaign_contract": (
            "bench/bench_e2e_algorithm_matrix_campaign_contract.json"),
        "execution_partition_manifest": (
            "bench/bench_e2e_algorithm_matrix_q1024_packed_partition_manifest.json"),
        "singleton_partition_manifest": (
            "bench/bench_e2e_algorithm_matrix_q1024_singleton_partition_manifest.json"),
        "shard_generator": "bench/generate_e2e_algorithm_matrix_shards.py",
        "packed_partition_generator": (
            "bench/generate_e2e_algorithm_matrix_q1024_packed_partition.py"),
        "packed_shard_directory": (
            "bench/e2e_algorithm_matrix_q1024_packed_shards"),
        "singleton_shard_directory": "bench/e2e_algorithm_matrix_shards",
        "canonical_parent_shard_count": EXPECTED_SHARD_CONFIGS,
        "packed_execution_config_count": 25,
        "adopted_execution_config_count": 4,
        "singleton_fallback_config_count": 1142,
        "builder_config_override": (
            "Pass one exact hybrid-manifest packed or singleton-fallback path with "
            "run_only=true, a current debug=false bench-e2e-opt build receipt, and "
            "the exact bench_e2e filter. Only the fifteen explicitly selected canonical "
            "parents are executable as packs; all other parents remain topology-only."),
        "mcp_run_only_example": {
            "mode": "benchmark-e2e",
            "run_only": True,
            "reuse_build_receipt": (
                "<current successful debug=false bench-e2e-opt build receipt>"),
            "bench_e2e_config": (
                "bench/e2e_algorithm_matrix_shards/"
                "bench_e2e_algorithm_matrix_campaign_shard_singleton_"
                "0664_ce4a7113f77b3cdf.json"),
            "filter": {"benchmarks": ["bench_e2e"]},
        },
        "automatic_consumer": (
            "build_mcp/build_worker.py revalidates the build receipt, seals and "
            "persists the run receipt, invokes this merger with its exact id, and "
            "marks the worker complete only after successful ingestion."),
        "manual_consumer_invocation": (
            "python3 bench/aggregate_e2e_algorithm_matrix.py --config "
            "<exact-packed-adopted-or-singleton-config>.json --report "
            "reporting/bioseq_benchmark_e2e/report_<N>.json --run-receipt-id "
            "rr_<32hex> --all-results ALL_RESULTS.yaml"),
        "proof_policy": (
            "Only exact path/SHA owners in the pinned hybrid manifest are mergeable. "
            "The 154 replacement keys remove their singleton alternatives; the other "
            "1142 singleton paths remain fallback authority. q1/q8 artifacts are excluded."),
        "shard_resume": (
            "Each invocation revalidates and recomputes from every processed report "
            "and external raw sidecar, validates embedded historical receipts, and "
            "preserves completed disjoint packed/adopted/singleton cells across later "
            "failures/source edits. "
            "Cells lacking their minimap2 baseline remain pending with null timing."),
    }


RESULT_ROW_FIELDS = {
    "stable_key",
    *STABLE_KEY_FIELDS,
    "observed_minimizer_algorithm",
    "observed_chaining_algorithm",
    "observed_base_alignment_algorithm",
    "status",
    "run_count",
    "outer_e2e_ns_p50",
    "outer_e2e_minus_cigar_or_mapq_union_ns_p50",
    "COMMENTS",
    "evidence_refs",
}
EVIDENCE_REF_FIELDS = {
    "report",
    "raw_sidecar",
    "run_id",
    "schedule_sorted_checksum",
    "record_count",
    "record_checksum_digest",
}
Q1024_EVIDENCE_REF_FIELDS = EVIDENCE_REF_FIELDS | {
    "run_receipt_id", "run_receipt_sha256",
}
Q1024_PRESERVATION_EVIDENCE_REF_FIELDS = Q1024_EVIDENCE_REF_FIELDS | {
    "publication_authority",
}
PROCESSED_REPORT_FIELDS = {
    "campaign_id",
    "campaign_digest",
    "campaign_contract",
    "campaign_contract_sha256",
    "report",
    "report_sha256",
    "raw_sidecar",
    "raw_sidecar_sha256",
    "run_id",
    "schedule_sorted_checksum",
    "config",
    "config_sha256",
    "config_role",
    "expected_stable_keys_sha256",
}
Q1024_PROCESSED_REPORT_FIELDS = PROCESSED_REPORT_FIELDS | {
    "execution_authority", "partition_manifest",
    "partition_manifest_sha256", "canonical_config",
    "canonical_config_sha256", "canonical_stable_keys_sha256",
    "run_receipt_id", "run_receipt_sha256", "run_receipt",
}


def _active_evidence_ref_fields() -> set[str]:
    return (Q1024_EVIDENCE_REF_FIELDS
            if ACTIVE_CAMPAIGN_PROFILE == "q1024" else EVIDENCE_REF_FIELDS)


def _active_processed_report_fields() -> set[str]:
    return (Q1024_PROCESSED_REPORT_FIELDS
            if ACTIVE_CAMPAIGN_PROFILE == "q1024" else PROCESSED_REPORT_FIELDS)


def _validate_existing_result_rows_schema(rows: Any) -> None:
    for raw_row in _require_list(rows, "results"):
        row = _require_dict(raw_row, "result row")
        _require_exact_keys(row, RESULT_ROW_FIELDS, "result row")
        stable_key = _require_string(row.get("stable_key"), "result stable_key")
        evidence_values = _require_list(row.get("evidence_refs"), "evidence_refs")
        preservation_row = (
            ACTIVE_CAMPAIGN_PROFILE == "q1024"
            and any(isinstance(reference, dict)
                    and "publication_authority" in reference
                    for reference in evidence_values))
        if (preservation_row
                and stable_key != Q1024_S059_PRESERVATION_STABLE_KEY):
            raise ContractError("preservation authority moved to another stable key")
        for field in STABLE_KEY_FIELDS:
            value = row.get(field)
            if value is not None and isinstance(value, (dict, list)):
                raise ContractError(f"result identity {field} must be scalar or null")
        for field in (
                "observed_minimizer_algorithm",
                "observed_chaining_algorithm",
                "observed_base_alignment_algorithm"):
            if row.get(field) is not None:
                _require_string(row.get(field), f"result {field}")
        if row.get("status") not in {"accepted", "rejected", "invalid", "pending"}:
            raise ContractError("result status is not canonical")
        _require_int(row.get("run_count"), "result run_count")
        for field in (
                "outer_e2e_ns_p50",
                "outer_e2e_minus_cigar_or_mapq_union_ns_p50"):
            if row.get(field) is not None:
                _require_int(row.get(field), f"result {field}")
        if row.get("COMMENTS") is not None:
            _require_text(row.get("COMMENTS"), "result COMMENTS")
        for raw_reference in evidence_values:
            reference = _require_dict(raw_reference, "evidence reference")
            expected_reference_fields = (
                Q1024_PRESERVATION_EVIDENCE_REF_FIELDS
                if preservation_row else _active_evidence_ref_fields())
            _require_exact_keys(
                reference, expected_reference_fields, "evidence reference")
            for field in (
                    "report", "raw_sidecar", "run_id",
                    "schedule_sorted_checksum", "record_checksum_digest"):
                _require_string(reference.get(field), f"evidence reference {field}")
            _require_int(reference.get("record_count"), "evidence reference record_count")
            if ACTIVE_CAMPAIGN_PROFILE == "q1024":
                if preservation_row:
                    if (reference.get("run_receipt_id") is not None
                            or reference.get("run_receipt_sha256") is not None
                            or reference.get("publication_authority")
                                != Q1024_S059_PRESERVATION_AUTHORITY):
                        raise ContractError(
                            "s059 preservation evidence invented a run receipt "
                            "or changed authority")
                else:
                    _require_string(
                        reference.get("run_receipt_id"),
                        "evidence reference run_receipt_id")
                    _require_hex(
                        reference.get("run_receipt_sha256"),
                        "evidence reference run_receipt_sha256", 64)
        if preservation_row and not _q1024_s059_preservation_result_is_exact(row):
            raise ContractError("s059 preservation result differs from its audit seal")


def _validate_q1024_ledger_consistency(
        document: dict[str, Any],
        campaign_contract: dict[str, Any],
) -> None:
    """Recompute every partial-publication count and status from result rows."""
    rows = _require_list(document.get("results"), "results")
    state = _require_dict(document.get("aggregation_state"), "aggregation_state")
    campaign = _require_dict(document.get("campaign"), "campaign")
    canonical_cells = campaign_contract["canonical_cells"]
    stable_keys = [
        _require_string(row.get("stable_key"), "result stable_key")
        for row in rows]
    if len(stable_keys) != len(set(stable_keys)):
        raise ContractError("results repeat a canonical stable key")
    actual_keys = set(stable_keys)
    expected_keys = set(canonical_cells)
    if not actual_keys <= expected_keys:
        raise ContractError("results contain a noncanonical stable key")

    statuses: list[str] = []
    canonical_statuses = True
    for row in rows:
        descriptor = canonical_cells[row["stable_key"]]
        if any(row.get(field) != value
               for field, value in descriptor["identity"].items()):
            raise ContractError("result identity differs from its canonical cell")
        status = row["status"]
        statuses.append(status)
        if (status not in descriptor["allowed_statuses"]
                or row["run_count"] != MIN_ACCEPTED_REPEATS):
            canonical_statuses = False
        timings = (
            row["outer_e2e_ns_p50"],
            row["outer_e2e_minus_cigar_or_mapq_union_ns_p50"],
        )
        if status == "accepted":
            if any(isinstance(value, bool) or not isinstance(value, int)
                   for value in timings):
                raise ContractError(
                    "accepted result lacks one exact aggregate timing")
        elif timings != (None, None):
            raise ContractError(
                "nonaccepted result retained an aggregate timing")

    complete_coverage = (
        len(rows) == EXPECTED_RESULT_CELLS
        and actual_keys == expected_keys)
    publishable = (
        complete_coverage
        and canonical_statuses
        and not any(status in {"invalid", "pending"} for status in statuses))
    expected_state = {
        "canonical_membership_complete": complete_coverage,
        "canonical_statuses_valid": canonical_statuses,
        "missing_canonical_cell_count": len(expected_keys - actual_keys),
        "report_count": len(_require_list(
            state.get("processed_reports"), "processed_reports")),
        "raw_record_count": sum(row["run_count"] for row in rows),
        "result_cell_count": len(rows),
        "accepted_cell_count": statuses.count("accepted"),
        "rejected_cell_count": statuses.count("rejected"),
        "invalid_cell_count": statuses.count("invalid"),
        "pending_cell_count": statuses.count("pending"),
    }
    for field, expected in expected_state.items():
        if state.get(field) != expected:
            raise ContractError(
                f"aggregation_state.{field} does not recompute from results")

    if publishable:
        campaign_status = "complete"
        no_results_reason = None
    elif complete_coverage:
        campaign_status = "complete_nonpublishable"
        no_results_reason = (
            "The canonical cell set is complete, but one or more statuses "
            "are invalid, pending, or forbidden by the campaign contract.")
    else:
        campaign_status = "aggregation_in_progress"
        no_results_reason = (
            "Canonical campaign coverage is incomplete: "
            f"{len(expected_keys - actual_keys)} cells remain unmerged.")
    calibration_runs = sum(
        _require_int(campaign.get(field), f"campaign.{field}")
        for field in (
            "accepted_calibration_records", "excluded_calibration_records",
            "active_calibration_runs", "pending_calibration_runs"))
    if (campaign.get("status") != campaign_status
            or campaign.get("measurements_collected") != sum(
                row["run_count"] for row in rows
                if row["status"] == "accepted")
            or campaign.get("benchmark_runs_started")
                != expected_state["report_count"] + calibration_runs
            or campaign.get("results_are_publishable") is not publishable
            or campaign.get("no_results_reason") != no_results_reason):
        raise ContractError(
            "campaign partial-publication counts/status do not recompute")

    expected_publication_status = (
        "complete" if publishable
        else "complete_nonpublishable" if complete_coverage
        else "pending")
    if document.get("publication_state") != {
            "status": expected_publication_status,
            "query_count": EXPECTED_QUERY_COUNT,
            "result_collection": "results",
            "result_count": len(rows),
            "calibration_results_included": False,
    }:
        raise ContractError("publication_state does not recompute from results")
    methodology = _require_dict(
        document.get("publication_methodology"), "publication_methodology")
    if methodology.get("status") != (
            "complete" if complete_coverage else "pending"):
        raise ContractError("publication methodology status is inconsistent")
    if document.get("status") != campaign_status:
        raise ContractError("top-level status differs from campaign status")


def _q1024_s058_deadline_v2_checkpoint_is_exact(value: Any) -> bool:
    """Recognize the immutable s058 audit checkpoint without publishing it."""
    if not isinstance(value, dict):
        return False
    try:
        digest = _sha256(_canonical_bytes(value))
    except (TypeError, ValueError):
        return False
    authority = value.get("authority")
    deadline = (
        authority.get("deadline_v2_manifest")
        if isinstance(authority, dict) else None)
    coverage = value.get("coverage")
    deadline_coverage = (
        coverage.get("deadline_v2")
        if isinstance(coverage, dict) else None)
    literal_coverage = (
        coverage.get("literal_q1024")
        if isinstance(coverage, dict) else None)
    anchors = value.get("q1024_anchors")
    return (
        digest == Q1024_DEADLINE_V2_CHECKPOINT_SHA256
        and value.get("schema")
            == "bioseq.e2e.deadline-v2-audited-checkpoint"
        and value.get("version") == 1
        and value.get("status") == "INCOMPLETE_LITERAL_Q1024_COVERAGE"
        and value.get("updated_through") == "s058"
        and value.get("active_excluded") == "s059"
        and isinstance(deadline, dict)
        and deadline.get("path")
            == "bench/bench_e2e_algorithm_matrix_deadline_v2_manifest.json"
        and deadline.get("sha256") == Q1024_DEADLINE_V2_MANIFEST_SHA256
        and isinstance(deadline_coverage, dict)
        and deadline_coverage.get("terminal_audited_shards") == 59
        and deadline_coverage.get("total_durable_terminal_records") == 295
        and isinstance(literal_coverage, dict)
        and literal_coverage.get("expected_cells") == EXPECTED_RESULT_CELLS
        and literal_coverage.get("accepted_cells") == 3
        and literal_coverage.get("complete") is False
        and isinstance(anchors, list)
        and [anchor.get("shard") for anchor in anchors
             if isinstance(anchor, dict)] == ["s056", "s057", "s058"]
    )


def _q1024_s059_preservation_checkpoint_is_exact(value: Any) -> bool:
    """Recognize the explicit, immutable s059 preservation authority."""
    if not isinstance(value, dict):
        return False
    try:
        digest = _sha256(_canonical_bytes(value))
    except (TypeError, ValueError):
        return False
    coverage = value.get("coverage")
    deadline_coverage = (
        coverage.get("deadline_v2") if isinstance(coverage, dict) else None)
    literal_coverage = (
        coverage.get("literal_q1024") if isinstance(coverage, dict) else None)
    anchors = value.get("q1024_anchors")
    preservation_anchor = (
        anchors[-1] if isinstance(anchors, list) and anchors else None)
    preservation_audit = (
        preservation_anchor.get("preservation_audit")
        if isinstance(preservation_anchor, dict) else None)
    evidence = (
        preservation_anchor.get("evidence")
        if isinstance(preservation_anchor, dict) else None)
    report = evidence.get("report") if isinstance(evidence, dict) else None
    raw_sidecar = (
        evidence.get("raw_sidecar") if isinstance(evidence, dict) else None)
    return (
        digest == Q1024_S059_PRESERVATION_CHECKPOINT_SHA256
        and value.get("schema")
            == "bioseq.e2e.deadline-v2-audited-checkpoint"
        and value.get("version") == 1
        and value.get("status") == "INCOMPLETE_LITERAL_Q1024_COVERAGE"
        and value.get("updated_through") == "s059"
        and value.get("active_excluded") is None
        and isinstance(deadline_coverage, dict)
        and deadline_coverage.get("terminal_audited_shards") == 60
        and deadline_coverage.get("active_shards") == 0
        and deadline_coverage.get("total_durable_terminal_records") == 300
        and isinstance(literal_coverage, dict)
        and literal_coverage.get("expected_cells") == EXPECTED_RESULT_CELLS
        and literal_coverage.get("accepted_cells") == 4
        and literal_coverage.get("active_cells") == 0
        and literal_coverage.get("unscheduled_cells") == 1292
        and literal_coverage.get("complete") is False
        and isinstance(anchors, list)
        and [anchor.get("shard") for anchor in anchors
             if isinstance(anchor, dict)] == ["s056", "s057", "s058", "s059"]
        and isinstance(preservation_anchor, dict)
        and preservation_anchor.get("status") == "VALID_PRESERVED"
        and preservation_anchor.get("run_count") == MIN_ACCEPTED_REPEATS
        and isinstance(preservation_audit, dict)
        and preservation_audit.get("authority")
            == "explicit_user_preservation_rule"
        and preservation_audit.get("run_receipt_sealed") is False
        and isinstance(report, dict)
        and report.get("path")
            == "reporting/bioseq_benchmark_e2e/report_95.json"
        and report.get("sha256")
            == "923870e587b8b8ff687b08ede8b681707f909469369a93cf9b5c51111f01cbfa"
        and isinstance(raw_sidecar, dict)
        and raw_sidecar.get("path")
            == "reporting/bioseq_benchmark_e2e/report_95.raw.jsonl"
        and raw_sidecar.get("sha256")
            == "319163e8631389ecd2840c4e4f3eeabc6301a07a327d19bbfb69b0b296fab56f"
    )


def _q1024_deadline_v2_checkpoint_is_exact(value: Any) -> bool:
    """Recognize either immutable audited checkpoint revision."""
    return (_q1024_s058_deadline_v2_checkpoint_is_exact(value)
            or _q1024_s059_preservation_checkpoint_is_exact(value))


def _q1024_s059_preservation_result_is_exact(value: Any) -> bool:
    """Bind the sole run-receipt-free result to the user preservation seal."""
    if not isinstance(value, dict):
        return False
    try:
        digest = _sha256(_canonical_bytes(value))
    except (TypeError, ValueError):
        return False
    evidence_refs = value.get("evidence_refs")
    reference = (
        evidence_refs[0]
        if isinstance(evidence_refs, list) and len(evidence_refs) == 1 else None)
    return (
        digest == Q1024_S059_PRESERVATION_RESULT_SHA256
        and value.get("stable_key") == Q1024_S059_PRESERVATION_STABLE_KEY
        and value.get("status") == "accepted"
        and value.get("run_count") == MIN_ACCEPTED_REPEATS
        and value.get("outer_e2e_ns_p50") == 25767444160458
        and value.get("outer_e2e_minus_cigar_or_mapq_union_ns_p50")
            == 25764785262335
        and isinstance(reference, dict)
        and reference.get("report")
            == "reporting/bioseq_benchmark_e2e/report_95.json"
        and reference.get("raw_sidecar")
            == "reporting/bioseq_benchmark_e2e/report_95.raw.jsonl"
        and reference.get("record_count") == MIN_ACCEPTED_REPEATS
        and reference.get("run_receipt_id") is None
        and reference.get("run_receipt_sha256") is None
        and reference.get("publication_authority")
            == Q1024_S059_PRESERVATION_AUTHORITY
    )


def _q1024_s059_preservation_rows(
        document: dict[str, Any]) -> list[dict[str, Any]]:
    """Return the exact external-authority row, never a reconstructed variant."""
    rows = _require_list(document.get("results"), "results")
    candidates = [
        row for row in rows
        if isinstance(row, dict)
        and any(isinstance(reference, dict)
                and reference.get("publication_authority")
                    == Q1024_S059_PRESERVATION_AUTHORITY
                for reference in row.get("evidence_refs", []))]
    checkpoint_is_preservation = (
        _q1024_s059_preservation_checkpoint_is_exact(
            document.get("deadline_v2_checkpoint")))
    if not checkpoint_is_preservation:
        if candidates:
            raise ContractError(
                "s059 preservation result has no matching checkpoint authority")
        return []
    if (len(candidates) != 1
            or not _q1024_s059_preservation_result_is_exact(candidates[0])):
        raise ContractError(
            "s059 preservation checkpoint lacks its one exact result row")
    return [copy.deepcopy(candidates[0])]


def _merge_q1024_preservation_rows(
        aggregated_rows: list[dict[str, Any]],
        preservation_rows: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Add external-authority rows while rejecting any reconstructed collision."""
    rows_by_key = {row["stable_key"]: row for row in aggregated_rows}
    if len(rows_by_key) != len(aggregated_rows):
        raise ContractError("aggregated rows repeat a stable key")
    for preserved in preservation_rows:
        stable_key = preserved["stable_key"]
        if stable_key in rows_by_key:
            raise ContractError(
                "run-receipted evidence attempted to overwrite the sealed "
                "s059 preservation result")
        rows_by_key[stable_key] = copy.deepcopy(preserved)
    return [rows_by_key[key] for key in sorted(rows_by_key)]


def _legacy_q1024_ledger_is_exactly_empty(
        document: Any,
        campaign_id: str,
        campaign_digest: str,
) -> bool:
    """Recognize the one revision-4 state that has no q1024 publication data.

    Calibration is deliberately allowed to be populated, but its membership
    must stay confined to ``calibration`` and its four campaign counters must
    recompute from that section. Keep this predicate equivalent to the worker
    copy; contract tests exercise the same mutation corpus against both.
    """
    legacy_fields = {
        "schema_version", "contract", "campaign", "matrix_contract", "wiring",
        "aggregation_state", "results", "calibration", "execution_environment",
        "publication_methodology", "publication_state", "status",
    }
    if (not isinstance(document, dict)
            or set(document) not in {
                frozenset(legacy_fields),
                frozenset(legacy_fields | {"deadline_v2_checkpoint"}),
            }):
        return False
    if ("deadline_v2_checkpoint" in document
            and not _q1024_s058_deadline_v2_checkpoint_is_exact(
                document["deadline_v2_checkpoint"])):
        return False
    contract = document.get("contract")
    if (document.get("schema_version") != 4
            or not isinstance(contract, dict)
            or set(contract) != {
                "schema", "version", "campaign_id", "campaign_digest",
                "campaign_contract", "campaign_contract_sha256", "encoding",
                "raw_framework_schema", "raw_payload_schema",
                "raw_payload_schema_version", "old_or_mixed_schema_policy",
                "minimum_accepted_repeats", "stable_key_schema",
                "stable_key_fields", "stable_key_encoding", "repeat_identity",
                "duplicate_policy", "idempotency_policy", "metric_a", "metric_b",
                "timing_acceptance", "clock_semantics", "match_parity",
                "result_row_timing_fields", "non_timing_result_fields",
            }
            or contract.get("schema")
                != "bioseq.e2e.algorithm-matrix-all-results"
            or contract.get("version") != 2
            or contract.get("campaign_id") != campaign_id
            or contract.get("campaign_digest") != campaign_digest
            or contract.get("raw_framework_schema") != 1
            or contract.get("raw_payload_schema") != "bioseq.e2e.raw-sample"
            or contract.get("raw_payload_schema_version")
                != LEGACY_RAW_PAYLOAD_SCHEMA_VERSION
            or contract.get("minimum_accepted_repeats") != MIN_ACCEPTED_REPEATS
            or contract.get("stable_key_fields") != list(STABLE_KEY_FIELDS)
            or contract.get("result_row_timing_fields") != [
                "run_count", "outer_e2e_ns_p50",
                "outer_e2e_minus_cigar_or_mapq_union_ns_p50",
            ]):
        return False
    if document.get("results") != [] or document.get("status") != "in_progress":
        return False
    if document.get("matrix_contract") != _expected_matrix_contract():
        return False
    if (not isinstance(document.get("wiring"), dict)
            or set(document["wiring"]) != {
                "campaign_contract", "shard_generator", "serial_shard_directory",
                "serial_shard_count", "builder_config_override",
                "mcp_run_only_example", "automatic_consumer",
                "manual_consumer_invocation", "proof_policy", "shard_resume",
            }) \
            or not isinstance(document.get("execution_environment"), dict):
        return False

    state = document.get("aggregation_state")
    if not isinstance(state, dict) or set(state) != {
        "campaign_id", "campaign_digest", "canonical_result_cell_count",
        "canonical_membership_complete", "canonical_statuses_valid",
        "missing_canonical_cell_count", "processed_reports", "report_count",
        "raw_record_count", "result_cell_count", "accepted_cell_count",
        "rejected_cell_count", "invalid_cell_count", "pending_cell_count",
    }:
        return False
    if (state.get("campaign_id") != campaign_id
            or state.get("campaign_digest") != campaign_digest
            or state.get("canonical_membership_complete") is not False
            or state.get("canonical_statuses_valid") is not True
            or state.get("processed_reports") != []):
        return False
    exact_state_counts = {
        "canonical_result_cell_count": EXPECTED_RESULT_CELLS,
        "missing_canonical_cell_count": EXPECTED_RESULT_CELLS,
        "report_count": 0,
        "raw_record_count": 0,
        "result_cell_count": 0,
        "accepted_cell_count": 0,
        "rejected_cell_count": 0,
        "invalid_cell_count": 0,
        "pending_cell_count": 0,
    }
    if any(type(state.get(field)) is not int or state[field] != expected
           for field, expected in exact_state_counts.items()):
        return False

    calibration = document.get("calibration")
    if not isinstance(calibration, dict) or set(calibration) != {
        "schema_version", "publication_eligible", "separation_policy", "q1", "q8",
    } or calibration.get("schema_version") != 1 \
            or calibration.get("publication_eligible") is not False \
            or not isinstance(calibration.get("separation_policy"), str):
        return False
    q1 = calibration.get("q1")
    q8 = calibration.get("q8")
    if (not isinstance(q1, dict)
            or set(q1) != {
                "status", "query_count", "reason", "run_count",
                "outer_e2e_ns_p50",
                "outer_e2e_minus_cigar_or_mapq_union_ns_p50", "parity_comment",
            }
            or q1.get("status") != "excluded"
            or q1.get("query_count") != 1
            or q1.get("reason") != "query_count_not_significant"
            or type(q1.get("run_count")) is not int
            or q1.get("run_count") != MIN_ACCEPTED_REPEATS
            or type(q1.get("outer_e2e_ns_p50")) is not int
            or type(q1.get(
                "outer_e2e_minus_cigar_or_mapq_union_ns_p50")) is not int
            or not isinstance(q1.get("parity_comment"), str)
            or not isinstance(q8, dict)
            or set(q8) != {
                "status", "query_count", "accepted", "running", "pending_next",
            }
            or q8.get("status") not in {"in_progress", "complete"}
            or q8.get("query_count") != 8):
        return False
    calibration_lists: list[list[Any]] = []
    for field in ("accepted", "running", "pending_next"):
        value = q8.get(field)
        if not isinstance(value, list) or any(not isinstance(item, dict) for item in value):
            return False
        calibration_lists.append(value)
    calibration_ids = [
        item.get("id") for values in calibration_lists for item in values
    ]
    if (any(not isinstance(value, str) or not value for value in calibration_ids)
            or len(calibration_ids) != len(set(calibration_ids))):
        return False
    accepted_calibration, running_calibration, pending_calibration = (
        len(values) for values in calibration_lists)
    expected_q8_status = (
        "complete" if running_calibration == 0 and pending_calibration == 0
        else "in_progress")
    if q8.get("status") != expected_q8_status:
        return False

    campaign = document.get("campaign")
    if not isinstance(campaign, dict) or set(campaign) != {
        "name", "campaign_id", "campaign_digest", "status", "last_updated_at",
        "execution_scope", "remote_execution_invoked", "measurements_collected",
        "benchmark_runs_started", "accepted_calibration_records",
        "excluded_calibration_records", "active_calibration_runs",
        "pending_calibration_runs", "results_are_publishable", "no_results_reason",
    }:
        return False
    if (campaign.get("name") != "local_e2e_algorithm_matrix"
            or campaign.get("campaign_id") != campaign_id
            or campaign.get("campaign_digest") != campaign_digest
            or campaign.get("status") != "in_progress"
            or not isinstance(campaign.get("last_updated_at"), str)
            or not campaign["last_updated_at"]
            or campaign.get("execution_scope") != "local_campaign"
            or campaign.get("remote_execution_invoked") is not False
            or campaign.get("results_are_publishable") is not False
            or not isinstance(campaign.get("no_results_reason"), str)
            or not campaign["no_results_reason"]):
        return False
    exact_campaign_counts = {
        "measurements_collected": 0,
        "benchmark_runs_started": 1 + accepted_calibration + running_calibration,
        "accepted_calibration_records": accepted_calibration,
        "excluded_calibration_records": 1,
        "active_calibration_runs": running_calibration,
        "pending_calibration_runs": pending_calibration,
    }
    if any(type(campaign.get(field)) is not int or campaign[field] != expected
           for field, expected in exact_campaign_counts.items()):
        return False

    publication_state = document.get("publication_state")
    if publication_state != {
        "status": "pending",
        "query_count": EXPECTED_QUERY_COUNT,
        "result_collection": "results",
        "result_count": 0,
        "calibration_results_included": False,
    } or type(publication_state.get("result_count")) is not int \
            or publication_state.get("calibration_results_included") is not False:
        return False
    methodology = document.get("publication_methodology")
    if not isinstance(methodology, dict) or set(methodology) != {
        "status", "query_count", "target_size_gbp", "alphabet", "window_kmer",
        "index_mode", "cigar_projection", "mapq_projection", "prefilter_enabled",
        "engines", "minimizer_algorithms", "chaining_algorithms",
        "base_alignment_algorithms", "excluded_base_alignment_algorithms",
        "minimum_run_count", "reported_timing_fields_only", "metric_a", "metric_b",
        "correctness_requirement", "algorithm_audit_requirement",
        "calibration_inference_policy",
    }:
        return False
    if (methodology.get("status") != "pending"
            or methodology.get("query_count") != EXPECTED_QUERY_COUNT
            or methodology.get("cigar_projection") is not True
            or methodology.get("mapq_projection") is not True
            or methodology.get("prefilter_enabled") is not False
            or methodology.get("base_alignment_algorithms")
                != ["ksw2", "block_aligner"]
            or methodology.get("excluded_base_alignment_algorithms")
                != ["ksw2_batched"]
            or methodology.get("minimum_run_count") != MIN_ACCEPTED_REPEATS
            or methodology.get("reported_timing_fields_only") != [
                "run_count", "outer_e2e_ns_p50",
                "outer_e2e_minus_cigar_or_mapq_union_ns_p50",
            ]):
        return False
    return True


def _upgrade_q1024_ledger_contract(
        document: dict[str, Any], campaign_contract: dict[str, Any]) -> None:
    """Migrate the result-empty revision-4 q1024 ledger in memory.

    The optional audited deadline-v2 checkpoint is historical, nonpublication
    evidence. Its exact canonical digest is preserved; adopted q1024 anchors
    are still independently parsed and validated before entering ``results``.
    """
    if ACTIVE_CAMPAIGN_PROFILE != "q1024" \
            or document.get("schema_version") == ALL_RESULTS_SCHEMA_VERSION:
        return
    if not _legacy_q1024_ledger_is_exactly_empty(
            document,
            campaign_contract["campaign_id"],
            campaign_contract["campaign_digest"]):
        raise ContractError(
            "q1024 revision-4 ledger can be upgraded only while publication "
            "state is the exact legacy empty contract")
    state = document["aggregation_state"]
    document["schema_version"] = ALL_RESULTS_SCHEMA_VERSION
    document["contract"] = _expected_all_results_contract(campaign_contract)
    document["wiring"] = _expected_wiring()
    state["build_receipts"] = []
    document["campaign"]["status"] = "aggregation_in_progress"
    document["campaign"]["no_results_reason"] = (
        "Canonical campaign coverage is incomplete: 1296 cells remain unmerged.")
    document["publication_methodology"]["status"] = "pending"
    document["publication_state"] = {
        "status": "pending",
        "query_count": EXPECTED_QUERY_COUNT,
        "result_collection": "results",
        "result_count": 0,
        "calibration_results_included": False,
    }
    document["status"] = "aggregation_in_progress"


def _refresh_q1024_partition_binding(
        document: dict[str, Any], campaign_contract: dict[str, Any]) -> None:
    """Migrate only the sealed pre-queue partition overlay to current authority."""
    if ACTIVE_CAMPAIGN_PROFILE != "q1024":
        return
    contract = _require_dict(document.get("contract"), "ALL_RESULTS.yaml contract")
    expected = _expected_all_results_contract_for_document(
        campaign_contract, document)
    if contract == expected:
        return
    expected_wiring = _expected_wiring()
    wiring = _require_dict(document.get("wiring"), "ALL_RESULTS.yaml wiring")

    previous_metadata_manifest_sha256 = (
        "65d31b8b3aa49400598d80d50641b2a9a4b1f78c08c85c8a33afbef761fd891b")
    current_metadata_expected = _current_all_results_contract_for_document(
        campaign_contract, document)
    previous_metadata_expected = copy.deepcopy(current_metadata_expected)
    previous_metadata_expected["execution_partition"]["manifest_sha256"] = (
        previous_metadata_manifest_sha256)
    previous_metadata_legacy = _legacy_all_results_contract(
        previous_metadata_expected)
    if contract in (previous_metadata_expected, previous_metadata_legacy):
        if wiring != expected_wiring:
            raise ContractError(
                "previous metadata partition contract has noncanonical wiring")
        authority_names = {
            "q1024_singleton": "q1024_singleton_fallback",
            "q1024_packed": "q1024_packed_partition",
            "q1024_deadline_v2_adoption": "q1024_deadline_v2_adoption",
        }
        state = _require_dict(
            document.get("aggregation_state"), "aggregation_state")
        entries_to_rebind: list[dict[str, Any]] = []
        for raw_entry in _require_list(
                state.get("processed_reports"), "processed_reports"):
            entry = _require_dict(raw_entry, "processed report")
            if (entry.get("partition_manifest")
                    != campaign_contract["execution_authority"]
                    or entry.get("partition_manifest_sha256")
                        != previous_metadata_manifest_sha256):
                raise ContractError(
                    "previous metadata report binding is not uniformly sealed")
            config_path = _canonical_repository_file(
                _require_string(entry.get("config"), "processed config path"),
                "processed config path")
            current_owner = campaign_contract["configs_by_path"].get(config_path)
            if (current_owner is None
                    or authority_names.get(
                        current_owner.get("execution_authority"))
                        != entry.get("execution_authority")):
                raise ContractError(
                    "previous metadata report is not preserved by current ownership")
            canonical_parent = _require_dict(
                current_owner.get("canonical_parent"),
                "current q1024 owner canonical parent")
            if (entry.get("config_sha256")
                        != current_owner.get("config_sha256")
                    or entry.get("config_role") != current_owner.get("role")
                    or entry.get("expected_stable_keys_sha256")
                        != current_owner.get("expected_stable_keys_sha256")
                    or entry.get("canonical_config")
                        != canonical_parent.get("path")
                    or entry.get("canonical_config_sha256")
                        != canonical_parent.get("config_sha256")
                    or entry.get("canonical_stable_keys_sha256")
                        != current_owner.get("expected_stable_keys_sha256")):
                raise ContractError(
                    "previous metadata report configuration identity changed")
            entries_to_rebind.append(entry)
        for entry in entries_to_rebind:
            entry["partition_manifest_sha256"] = campaign_contract[
                "execution_authority_sha256"]
        document["contract"] = (
            _legacy_all_results_contract(current_metadata_expected)
            if contract == previous_metadata_legacy
            else current_metadata_expected)
        document["wiring"] = expected_wiring
        return

    previous_manifest_sha256 = (
        "adc97dcc133c2fb568745a91af023faa47ab3a636c915358d12f1395a35e984e")
    previous_expected = copy.deepcopy(expected)
    previous_expected["execution_partition"] = {
        "schema": "bioseq.e2e.algorithm-matrix-q1024-packed-partition",
        "manifest": (
            "bench/bench_e2e_algorithm_matrix_q1024_packed_partition_manifest.json"),
        "manifest_sha256": previous_manifest_sha256,
        "singleton_manifest": (
            "bench/bench_e2e_algorithm_matrix_q1024_singleton_partition_manifest.json"),
        "singleton_manifest_sha256": Q1024_SINGLETON_PARTITION_SHA256,
        "canonical_parent_configs": 88,
        "packed_configs": 23,
        "adopted_configs": 4,
        "singleton_fallback_configs": 1154,
        "execution_configs": 1181,
        "packed_cells": 138,
        "adopted_cells": 4,
        "singleton_fallback_cells": 1154,
        "records_per_cell": 5,
        "exact_disjoint_hybrid_union": True,
        "canonical_topology_and_digest_unchanged": True,
    }
    if contract == previous_expected:
        previous_wiring = copy.deepcopy(expected_wiring)
        previous_wiring.update({
            "builder_config_override": (
                "Pass one exact hybrid-manifest packed or singleton-fallback path "
                "with run_only=true, a current debug=false bench-e2e-opt build "
                "receipt, and the exact bench_e2e filter. Only the fourteen "
                "explicitly selected canonical parents are executable as packs; "
                "all other parents remain topology-only."),
            "packed_execution_config_count": 23,
            "singleton_fallback_config_count": 1154,
            "proof_policy": (
                "Only exact path/SHA owners in the pinned hybrid manifest are "
                "mergeable. The 142 replacement keys remove their singleton "
                "alternatives; the other 1154 singleton paths remain fallback "
                "authority. q1/q8 artifacts are excluded."),
        })
        if wiring != previous_wiring:
            raise ContractError(
                "previous partition contract has noncanonical wiring")
        authority_names = {
            "q1024_singleton": "q1024_singleton_fallback",
            "q1024_packed": "q1024_packed_partition",
            "q1024_deadline_v2_adoption": "q1024_deadline_v2_adoption",
        }
        state = _require_dict(
            document.get("aggregation_state"), "aggregation_state")
        for raw_entry in _require_list(
                state.get("processed_reports"), "processed_reports"):
            entry = _require_dict(raw_entry, "processed report")
            if (entry.get("partition_manifest")
                    != campaign_contract["execution_authority"]
                    or entry.get("partition_manifest_sha256")
                    != previous_manifest_sha256):
                raise ContractError(
                    "previous partition report binding is not uniformly sealed")
            config_path = _canonical_repository_file(
                _require_string(entry.get("config"), "processed config path"),
                "processed config path")
            current_owner = campaign_contract["configs_by_path"].get(config_path)
            if (current_owner is None
                    or authority_names.get(
                        current_owner.get("execution_authority"))
                        != entry.get("execution_authority")):
                raise ContractError(
                    "previous partition report is not preserved by current ownership")
            entry["partition_manifest_sha256"] = campaign_contract[
                "execution_authority_sha256"]
        document["contract"] = expected
        document["wiring"] = expected_wiring
        return

    legacy_expected = copy.deepcopy(expected)
    legacy_expected["execution_partition"] = {
        "schema": "bioseq.e2e.algorithm-matrix-q1024-packed-partition",
        "manifest": (
            "bench/bench_e2e_algorithm_matrix_q1024_packed_partition_manifest.json"),
        "manifest_sha256": (
            "78b8f9aaa26b1f7ad5a49a39cc4b47f7e7dc49320bb99cd3d1d0c253b4476d7d"),
        "singleton_manifest": (
            "bench/bench_e2e_algorithm_matrix_q1024_singleton_partition_manifest.json"),
        "singleton_manifest_sha256": Q1024_SINGLETON_PARTITION_SHA256,
        "canonical_parent_configs": 88,
        "packed_configs": 19,
        "adopted_configs": 4,
        "singleton_fallback_configs": 1186,
        "execution_configs": 1209,
        "packed_cells": 106,
        "adopted_cells": 4,
        "singleton_fallback_cells": 1186,
        "records_per_cell": 5,
        "exact_disjoint_hybrid_union": True,
        "canonical_topology_and_digest_unchanged": True,
    }
    if contract != legacy_expected:
        return

    legacy_wiring = copy.deepcopy(expected_wiring)
    legacy_wiring.update({
        "builder_config_override": (
            "Pass one exact hybrid-manifest packed or singleton-fallback path "
            "with run_only=true, a current debug=false bench-e2e-opt build "
            "receipt, and the exact bench_e2e filter. Only the ten explicitly "
            "selected canonical parents are executable as packs; all other "
            "parents remain topology-only."),
        "packed_execution_config_count": 19,
        "singleton_fallback_config_count": 1186,
        "proof_policy": (
            "Only exact path/SHA owners in the pinned hybrid manifest are "
            "mergeable. The 110 replacement keys remove their singleton "
            "alternatives; the other 1186 singleton paths remain fallback "
            "authority. q1/q8 artifacts are excluded."),
    })
    if wiring != legacy_wiring:
        raise ContractError(
            "legacy partition contract has noncanonical wiring")

    state = _require_dict(document.get("aggregation_state"), "aggregation_state")
    for raw_entry in _require_list(
            state.get("processed_reports"), "processed_reports"):
        entry = _require_dict(raw_entry, "processed report")
        if entry.get("partition_manifest_sha256") != (
                "78b8f9aaa26b1f7ad5a49a39cc4b47f7e7dc49320bb99cd3d1d0c253b4476d7d"):
            continue
        if (entry.get("execution_authority")
                != "q1024_deadline_v2_adoption"
                or entry.get("partition_manifest")
                    != campaign_contract["execution_authority"]):
            raise ContractError(
                "legacy partition binding appeared on non-adoption evidence")
        entry["partition_manifest_sha256"] = campaign_contract[
            "execution_authority_sha256"]
    document["contract"] = expected
    document["wiring"] = expected_wiring


def _refresh_q8_ledger_proof_binding(
        document: dict[str, Any], campaign_contract: dict[str, Any]) -> None:
    """Adapt the schema-1 working ledger to the regenerated source proof."""
    if ACTIVE_CAMPAIGN_PROFILE != "q8":
        return
    contract = document.get("contract")
    if not isinstance(contract, dict):
        raise ContractError("Q8_RESULTS.yaml contract must be an object")
    current = _current_all_results_contract_for_document(
        campaign_contract, document)
    legacy = _legacy_all_results_contract(current)
    if contract in (current, legacy):
        return
    adapted = copy.deepcopy(contract)
    adapted["query_prefix_proof"] = current["query_prefix_proof"]
    adapted["query_prefix_proof_sha256"] = current[
        "query_prefix_proof_sha256"]
    if adapted not in (current, legacy):
        raise ContractError(
            "Q8_RESULTS.yaml differs beyond the regenerated prefix-proof SHA")
    document["contract"] = adapted


def _validate_all_results(
        document: dict[str, Any],
        campaign_contract: dict[str, Any],
) -> None:
    expected_top_level = {
        "schema_version",
        "contract",
        "campaign",
        "matrix_contract",
        "wiring",
        "aggregation_state",
        "results",
    }
    if ACTIVE_CAMPAIGN_PROFILE == "q1024":
        expected_top_level.update({
            "calibration", "execution_environment", "publication_methodology",
            "publication_state", "status",
        })
        if "deadline_v2_checkpoint" in document:
            expected_top_level.add("deadline_v2_checkpoint")
    _require_exact_keys(document, expected_top_level, EXPECTED_LEDGER_PATH)
    if (ACTIVE_CAMPAIGN_PROFILE == "q1024"
            and "deadline_v2_checkpoint" in document
            and not _q1024_deadline_v2_checkpoint_is_exact(
                document["deadline_v2_checkpoint"])):
        raise ContractError(
            "deadline_v2_checkpoint differs from the audited s058 seal")
    if document.get("schema_version") != ALL_RESULTS_SCHEMA_VERSION:
        raise ContractError(
            f"{EXPECTED_LEDGER_PATH} must use schema_version "
            f"{ALL_RESULTS_SCHEMA_VERSION}")
    contract = _require_dict(
        document.get("contract"), f"{EXPECTED_LEDGER_PATH} contract")
    if contract != _expected_all_results_contract_for_document(
            campaign_contract, document):
        raise ContractError(
            f"{EXPECTED_LEDGER_PATH} contract keys or auxiliary "
            "metric/evidence semantics changed")
    if _require_dict(document.get("matrix_contract"), "matrix contract") \
            != _expected_matrix_contract():
        raise ContractError(
            f"{EXPECTED_LEDGER_PATH} exact {EXPECTED_SHARD_CONFIGS}/"
            "1296/6480 matrix contract changed")
    if _require_dict(document.get("wiring"), f"{EXPECTED_LEDGER_PATH} wiring") \
            != _expected_wiring():
        raise ContractError(
            f"{EXPECTED_LEDGER_PATH} campaign wiring/authority fields changed")
    campaign = _require_dict(
        document.get("campaign"), f"{EXPECTED_LEDGER_PATH} campaign")
    expected_campaign_fields = {
        "name",
        "campaign_id",
        "campaign_digest",
        "status",
        "last_updated_at",
        "execution_scope",
        "remote_execution_invoked",
        "measurements_collected",
        "benchmark_runs_started",
        "results_are_publishable",
        "no_results_reason",
    }
    if ACTIVE_CAMPAIGN_PROFILE == "q1024":
        expected_campaign_fields.update({
            "accepted_calibration_records", "excluded_calibration_records",
            "active_calibration_runs", "pending_calibration_runs",
        })
    _require_exact_keys(
        campaign, expected_campaign_fields, f"{EXPECTED_LEDGER_PATH} campaign")
    if (campaign.get("name") != EXPECTED_LEDGER_CAMPAIGN_NAME
            or campaign.get("campaign_id") != campaign_contract["campaign_id"]
            or campaign.get("campaign_digest") != campaign_contract["campaign_digest"]
            or campaign.get("execution_scope") != "local_campaign"
            or campaign.get("remote_execution_invoked") is not False):
        raise ContractError(
            f"{EXPECTED_LEDGER_PATH} campaign identity or local/remote "
            "machine authority changed")
    _require_string(campaign.get("status"), "campaign status")
    updated_at = _require_string(campaign.get("last_updated_at"), "last_updated_at")
    try:
        dt.datetime.fromisoformat(updated_at.replace("Z", "+00:00"))
    except ValueError as exc:
        raise ContractError("campaign last_updated_at is not an ISO timestamp/date") from exc
    _require_int(campaign.get("measurements_collected"), "measurements_collected")
    _require_int(campaign.get("benchmark_runs_started"), "benchmark_runs_started")
    if ACTIVE_CAMPAIGN_PROFILE == "q1024":
        for field in (
                "accepted_calibration_records", "excluded_calibration_records",
                "active_calibration_runs", "pending_calibration_runs"):
            _require_int(campaign.get(field), f"campaign.{field}")
    _require_bool(campaign.get("results_are_publishable"), "results_are_publishable")
    if campaign.get("no_results_reason") is not None:
        _require_text(campaign.get("no_results_reason"), "no_results_reason")
    state = _require_dict(document.get("aggregation_state"), "aggregation_state")
    expected_state_fields = {
        "campaign_id",
        "campaign_digest",
        "canonical_result_cell_count",
        "canonical_membership_complete",
        "canonical_statuses_valid",
        "missing_canonical_cell_count",
        "processed_reports",
        "report_count",
        "raw_record_count",
        "result_cell_count",
        "accepted_cell_count",
        "rejected_cell_count",
        "invalid_cell_count",
        "pending_cell_count",
    }
    if ACTIVE_CAMPAIGN_PROFILE == "q1024":
        expected_state_fields.add("build_receipts")
    _require_exact_keys(state, expected_state_fields, "aggregation_state")
    if (state.get("campaign_id") != campaign_contract["campaign_id"]
            or state.get("campaign_digest") != campaign_contract["campaign_digest"]
            or state.get("canonical_result_cell_count") != EXPECTED_RESULT_CELLS):
        raise ContractError(
            f"{EXPECTED_LEDGER_PATH} aggregation state has the wrong campaign identity")
    _require_list(state.get("processed_reports"), "processed_reports")
    for field in ("canonical_membership_complete", "canonical_statuses_valid"):
        _require_bool(state.get(field), f"aggregation_state.{field}")
    for field in (
            "missing_canonical_cell_count",
            "report_count",
            "raw_record_count",
            "result_cell_count",
            "accepted_cell_count",
            "rejected_cell_count",
            "invalid_cell_count",
            "pending_cell_count"):
        _require_int(state.get(field), f"aggregation_state.{field}")
    if ACTIVE_CAMPAIGN_PROFILE == "q1024":
        receipt_ids: set[str] = set()
        for index, receipt_value in enumerate(_require_list(
                state.get("build_receipts"), "aggregation_state.build_receipts")):
            receipt = _validate_build_receipt_snapshot(
                receipt_value, f"aggregation build receipt[{index}]")
            receipt_id = receipt["build_receipt_id"]
            if receipt_id in receipt_ids:
                raise ContractError("aggregation_state repeats a build receipt id")
            receipt_ids.add(receipt_id)
    _validate_existing_result_rows_schema(document.get("results"))
    if ACTIVE_CAMPAIGN_PROFILE == "q1024":
        _q1024_s059_preservation_rows(document)
        _validate_q1024_ledger_consistency(document, campaign_contract)


def _load_existing_reports(
        document: dict[str, Any],
        campaign_contract: dict[str, Any],
        build_receipts_by_id: dict[str, dict[str, Any]],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    entries = _require_list(
        _require_dict(document["aggregation_state"], "aggregation_state").get("processed_reports"),
        "processed_reports",
    )
    records: list[dict[str, Any]] = []
    validated_entries: list[dict[str, Any]] = []
    paths_seen: set[str] = set()
    digests_seen: set[str] = set()
    for entry_value in entries:
        entry = _require_dict(entry_value, "processed report entry")
        _require_exact_keys(
            entry, _active_processed_report_fields(), "processed report entry")
        report_rel = _require_string(entry.get("report"), "processed report path")
        config_rel = _require_string(entry.get("config"), "processed config path")
        _canonical_repository_relative(report_rel, "processed report path")
        _canonical_repository_relative(config_rel, "processed config path")
        _canonical_repository_relative(
            _require_string(entry.get("raw_sidecar"), "processed raw sidecar"),
            "processed raw sidecar")
        _canonical_repository_relative(
            _require_string(
                entry.get("campaign_contract"), "processed campaign contract"),
            "processed campaign contract")
        for field in (
                "campaign_contract", "report_sha256", "raw_sidecar_sha256",
                "config_sha256", "expected_stable_keys_sha256"):
            _require_string(entry.get(field), f"processed report {field}")
        for field in (
                "campaign_contract_sha256", "report_sha256", "raw_sidecar_sha256",
                "config_sha256", "expected_stable_keys_sha256"):
            _require_hex(entry.get(field), f"processed report {field}", 64)
        _require_string(entry.get("run_id"), "processed report run_id")
        _require_hex(
            entry.get("schedule_sorted_checksum"),
            "processed report schedule checksum", 16)
        if entry.get("config_role") not in {"monolithic", "shard"}:
            raise ContractError("processed report config_role is not canonical")
        receipt_context = None
        if ACTIVE_CAMPAIGN_PROFILE == "q1024":
            for field in (
                    "execution_authority", "partition_manifest",
                    "canonical_config", "run_receipt_id"):
                _require_string(entry.get(field), f"processed report {field}")
            for field in (
                    "partition_manifest_sha256", "canonical_config_sha256",
                    "canonical_stable_keys_sha256", "run_receipt_sha256"):
                _require_hex(entry.get(field), f"processed report {field}", 64)
            _canonical_repository_relative(
                entry["partition_manifest"], "processed partition manifest")
            _canonical_repository_relative(
                entry["canonical_config"], "processed canonical config")
            if (entry["execution_authority"] not in {
                    "q1024_singleton_fallback", "q1024_packed_partition",
                    "q1024_deadline_v2_adoption"}
                    or entry["partition_manifest"]
                        != campaign_contract["execution_authority"]
                    or entry["partition_manifest_sha256"]
                        != campaign_contract["execution_authority_sha256"]):
                raise ContractError(
                    "processed q1024 report lost hybrid-partition authority")
            run_receipt = _require_dict(
                entry.get("run_receipt"), "processed full run receipt")
            build_id = run_receipt.get("reused_build_receipt")
            build_receipt = build_receipts_by_id.get(build_id)
            if build_receipt is None:
                raise ContractError(
                    "processed q1024 run receipt has no deduplicated build snapshot")
            receipt_context = (run_receipt, build_receipt)
        if report_rel in paths_seen:
            raise ContractError(f"duplicate processed report entry {report_rel}")
        paths_seen.add(report_rel)
        loaded, source = _load_report(
            _canonical_repository_file(report_rel, "processed report"),
            _canonical_repository_file(config_rel, "processed config"),
            campaign_contract,
            receipt_context,
        )
        if source != entry:
            raise ContractError(f"processed report evidence changed: {report_rel}")
        if source["report_sha256"] in digests_seen:
            raise ContractError("same report bytes registered under multiple paths")
        digests_seen.add(source["report_sha256"])
        validated_entries.append(source)
        records.extend(loaded)
    return records, validated_entries


# ``ALL_RESULTS_TUPLES.yaml`` deliberately has no general-purpose YAML
# dependency.  Its writer accepts and emits only the closed block-YAML subset
# used by the publication authority: mappings, sequences, empty plain nulls,
# booleans, nonnegative integers, plain strings, and single-quoted strings.
# Keeping that surface here makes duplicate keys and representation aliases
# visible before any semantic validation or filesystem replacement.
_APPROVED_YAML_KEY = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
_APPROVED_YAML_INTEGER = re.compile(r"(?:0|[1-9][0-9]*)\Z")
_APPROVED_YAML_NUMBER_LIKE = re.compile(
    r"[-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?\Z")
_APPROVED_YAML_RESERVED = {
    "null", "Null", "NULL", "~", "true", "True", "TRUE",
    "false", "False", "FALSE",
}


def _approved_tuple_path(path: Path, label: str, *, must_exist: bool) -> Path:
    """Check an explicit publication input without resolving a symlink."""
    candidate = path if path.is_absolute() else Path.cwd() / path
    if any(component in {"", ".", ".."} for component in path.parts):
        raise ApprovedTupleIOError(f"{label} contains a lexical path alias")
    try:
        parent = candidate.parent.lstat()
    except OSError as exc:
        raise ApprovedTupleIOError(f"cannot inspect {label} parent: {exc}") from exc
    if not stat.S_ISDIR(parent.st_mode):
        raise ApprovedTupleIOError(f"{label} parent must be a directory")
    try:
        metadata = candidate.lstat()
    except FileNotFoundError:
        if must_exist:
            raise ApprovedTupleIOError(f"{label} does not exist: {candidate}")
        return candidate
    except OSError as exc:
        raise ApprovedTupleIOError(f"cannot inspect {label}: {exc}") from exc
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise ApprovedTupleIOError(
            f"{label} must be a regular non-symlink file: {candidate}")
    return candidate


def _read_approved_tuple_file(path: Path, label: str) -> bytes:
    candidate = _approved_tuple_path(path, label, must_exist=True)
    try:
        before = candidate.lstat()
        flags = os.O_RDONLY
        if hasattr(os, "O_CLOEXEC"):
            flags |= os.O_CLOEXEC
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        descriptor = os.open(candidate, flags)
    except OSError as exc:
        raise ApprovedTupleIOError(f"cannot open {label}: {exc}") from exc
    try:
        opened = os.fstat(descriptor)
        if (not stat.S_ISREG(opened.st_mode)
                or opened.st_dev != before.st_dev
                or opened.st_ino != before.st_ino
                or opened.st_nlink != 1):
            raise ApprovedTupleIOError(f"{label} changed while opening")
        chunks: list[bytes] = []
        while True:
            try:
                chunk = os.read(descriptor, 1024 * 1024)
            except InterruptedError:
                continue
            if not chunk:
                break
            chunks.append(chunk)
        return b"".join(chunks)
    except OSError as exc:
        raise ApprovedTupleIOError(f"cannot read {label}: {exc}") from exc
    finally:
        os.close(descriptor)


def _approved_yaml_scalar(text: str, label: str) -> Any:
    if text == "":
        return None
    if text.startswith("'"):
        if len(text) < 2 or not text.endswith("'"):
            raise ContractError(f"{label} has an unterminated quoted scalar")
        body = text[1:-1]
        index = 0
        decoded: list[str] = []
        while index < len(body):
            if body[index] == "'":
                if index + 1 >= len(body) or body[index + 1] != "'":
                    raise ContractError(f"{label} has a noncanonical quote escape")
                decoded.append("'")
                index += 2
            else:
                decoded.append(body[index])
                index += 1
        return "".join(decoded)
    if text[0] in "\"!&*|>@`{}[],:?#%":
        raise ContractError(f"{label} uses a forbidden YAML scalar form")
    if any(token in text for token in ("{", "}", "[", "]", " #")):
        raise ContractError(f"{label} uses flow syntax or an inline comment")
    if ": " in text or text.endswith(":"):
        raise ContractError(f"{label} has an ambiguous unquoted colon")
    if text in {"true", "false"}:
        return text == "true"
    if text in {"null", "Null", "NULL", "~", "True", "TRUE", "False", "FALSE"}:
        raise ContractError(f"{label} uses a forbidden YAML scalar alias")
    if _APPROVED_YAML_INTEGER.fullmatch(text):
        return int(text)
    if _APPROVED_YAML_NUMBER_LIKE.fullmatch(text):
        raise ContractError(f"{label} uses a forbidden numeric scalar form")
    if any(ord(character) < 0x20 or ord(character) == 0x7f
           for character in text):
        raise ContractError(f"{label} contains a control character")
    return text


def _approved_yaml_pair(text: str, label: str) -> tuple[str, str]:
    separator = text.find(":")
    if separator <= 0:
        raise ContractError(f"{label} is not one mapping entry")
    key = text[:separator]
    if not _APPROVED_YAML_KEY.fullmatch(key):
        raise ContractError(f"{label} has a noncanonical mapping key")
    remainder = text[separator + 1:]
    if remainder and not remainder.startswith(" "):
        raise ContractError(f"{label} requires one space after a nonempty colon")
    if remainder.startswith("  "):
        raise ContractError(f"{label} permits exactly one space after a colon")
    return key, remainder[1:] if remainder else ""


def _approved_yaml_lines(raw: bytes, label: str) -> list[tuple[int, str, int]]:
    try:
        text = raw.decode("utf-8")
    except UnicodeError as exc:
        raise ContractError(f"{label} is not UTF-8: {exc}") from exc
    if text.startswith("\ufeff"):
        raise ContractError(f"{label} may not contain a UTF-8 BOM")
    if "\r" in text or not text.endswith("\n") or text.endswith("\n\n"):
        raise ContractError(
            f"{label} must use LF and contain exactly one terminal LF")
    physical = text[:-1].split("\n")
    if not physical or physical[0] != "---":
        raise ContractError(f"{label} must begin with the YAML document marker")
    result: list[tuple[int, str, int]] = []
    for line_number, line in enumerate(physical[1:], 2):
        if not line or line.endswith(" ") or "\t" in line:
            raise ContractError(
                f"{label}:{line_number} has blank, tabbed, or trailing-space syntax")
        indent = len(line) - len(line.lstrip(" "))
        if indent % 2:
            raise ContractError(f"{label}:{line_number} has odd indentation")
        content = line[indent:]
        if content.startswith("#") or content in {"---", "..."}:
            raise ContractError(f"{label}:{line_number} has forbidden YAML syntax")
        result.append((indent, content, line_number))
    if not result:
        raise ContractError(f"{label} has no document body")
    return result


def _approved_yaml_mapping(
        lines: list[tuple[int, str, int]], index: int, indent: int,
        label: str, initial: tuple[str, str, int] | None = None,
) -> tuple[dict[str, Any], int]:
    result: dict[str, Any] = {}

    def insert(key: str, scalar: str, line_number: int, next_index: int) -> int:
        if key in result:
            raise ContractError(
                f"{label}:{line_number} duplicates mapping key {key!r}")
        if scalar:
            result[key] = _approved_yaml_scalar(
                scalar, f"{label}:{line_number} {key}")
            return next_index
        if next_index < len(lines):
            child_indent, child_text, _ = lines[next_index]
            if child_indent > indent:
                if child_indent != indent + 2:
                    raise ContractError(
                        f"{label}:{lines[next_index][2]} skips an indentation level")
                value, next_index = _approved_yaml_node(
                    lines, next_index, child_indent, label)
                result[key] = value
                return next_index
            if child_indent == indent and child_text.startswith("-"):
                value, next_index = _approved_yaml_node(
                    lines, next_index, indent, label)
                result[key] = value
                return next_index
        result[key] = None
        return next_index

    if initial is not None:
        key, scalar, line_number = initial
        index = insert(key, scalar, line_number, index)
    while index < len(lines):
        current_indent, text, line_number = lines[index]
        if current_indent < indent:
            break
        if current_indent > indent:
            raise ContractError(f"{label}:{line_number} has an indentation jump")
        if text.startswith("-"):
            break
        key, scalar = _approved_yaml_pair(text, f"{label}:{line_number}")
        index = insert(key, scalar, line_number, index + 1)
    return result, index


def _approved_yaml_sequence(
        lines: list[tuple[int, str, int]], index: int, indent: int, label: str,
) -> tuple[list[Any], int]:
    result: list[Any] = []
    while index < len(lines):
        current_indent, text, line_number = lines[index]
        if current_indent < indent:
            break
        if current_indent > indent:
            raise ContractError(f"{label}:{line_number} has an indentation jump")
        if not text.startswith("-"):
            break
        if len(text) > 1 and text[1] != " ":
            raise ContractError(f"{label}:{line_number} has a malformed sequence item")
        if text.startswith("-  "):
            raise ContractError(
                f"{label}:{line_number} has noncanonical sequence spacing")
        item = text[2:] if len(text) > 1 else ""
        index += 1
        if not item:
            if index >= len(lines) or lines[index][0] != indent + 2:
                raise ContractError(f"{label}:{line_number} has an empty sequence item")
            value, index = _approved_yaml_node(
                lines, index, lines[index][0], label)
        elif (":" in item
              and _APPROVED_YAML_KEY.fullmatch(item.split(":", 1)[0])):
            key, scalar = _approved_yaml_pair(item, f"{label}:{line_number}")
            value, index = _approved_yaml_mapping(
                lines, index, indent + 2, label,
                initial=(key, scalar, line_number))
        else:
            value = _approved_yaml_scalar(item, f"{label}:{line_number}")
            if index < len(lines) and lines[index][0] > indent:
                raise ContractError(
                    f"{label}:{lines[index][2]} nests under a scalar sequence item")
        result.append(value)
    if not result:
        raise ContractError(f"{label} contains an empty sequence")
    return result, index


def _approved_yaml_node(
        lines: list[tuple[int, str, int]], index: int, indent: int, label: str,
) -> tuple[Any, int]:
    if lines[index][0] != indent:
        raise ContractError(f"{label}:{lines[index][2]} has unexpected indentation")
    if lines[index][1].startswith("-"):
        return _approved_yaml_sequence(lines, index, indent, label)
    return _approved_yaml_mapping(lines, index, indent, label)


def _parse_approved_tuples_yaml(raw: bytes, label: str) -> dict[str, Any]:
    lines = _approved_yaml_lines(raw, label)
    value, index = _approved_yaml_node(lines, 0, lines[0][0], label)
    if index != len(lines) or not isinstance(value, dict) or lines[0][0] != 0:
        raise ContractError(f"{label} must contain one top-level mapping")
    return value


def _approved_yaml_mapping_order(value: dict[str, Any]) -> list[str]:
    keys = set(value)
    if keys == set(APPROVED_TUPLES_TOP_LEVEL_FIELDS):
        return list(APPROVED_TUPLES_TOP_LEVEL_FIELDS)
    if keys == set(APPROVED_TUPLES_REMOTE_TOP_LEVEL_FIELDS):
        return list(APPROVED_TUPLES_REMOTE_TOP_LEVEL_FIELDS)
    if keys == set(APPROVED_TUPLES_REMOTE_HOST_FIELDS):
        return list(APPROVED_TUPLES_REMOTE_HOST_FIELDS)
    if keys == set(APPROVED_TUPLES_REMOTE_AWS_IDENTITY_FIELDS):
        return list(APPROVED_TUPLES_REMOTE_AWS_IDENTITY_FIELDS)
    if keys == set(APPROVED_TUPLES_REMOTE_CPU_FIELDS):
        return list(APPROVED_TUPLES_REMOTE_CPU_FIELDS)
    if keys == set(APPROVED_TUPLES_REMOTE_MEMORY_FIELDS):
        return list(APPROVED_TUPLES_REMOTE_MEMORY_FIELDS)
    if keys == set(APPROVED_TUPLES_REMOTE_STORAGE_FIELDS):
        return list(APPROVED_TUPLES_REMOTE_STORAGE_FIELDS)
    if keys == set(APPROVED_TUPLES_REMOTE_GPU_FIELDS):
        return list(APPROVED_TUPLES_REMOTE_GPU_FIELDS)
    if keys == set(APPROVED_TUPLES_REMOTE_DISPOSITION_FIELDS):
        return list(APPROVED_TUPLES_REMOTE_DISPOSITION_FIELDS)
    if keys == set(APPROVED_TUPLES_ROW_FIELDS):
        return list(APPROVED_TUPLES_ROW_FIELDS)
    if keys == set(APPROVED_TUPLES_V2_ROW_FIELDS):
        return list(APPROVED_TUPLES_V2_ROW_FIELDS)
    if keys == set(APPROVED_TUPLES_ALGORITHM_FIELDS):
        return list(APPROVED_TUPLES_ALGORITHM_FIELDS)
    if keys == set(APPROVED_TUPLES_ELIGIBILITY_FIELDS):
        return list(APPROVED_TUPLES_ELIGIBILITY_FIELDS)
    if keys == set(APPROVED_TUPLES_METRIC_FIELDS):
        return list(APPROVED_TUPLES_METRIC_FIELDS)
    return sorted(value)


def _approved_yaml_emit_scalar(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int) and not isinstance(value, bool) and value >= 0:
        return str(value)
    if not isinstance(value, str) or "\n" in value or "\r" in value:
        raise ContractError("approved authority contains an unsupported scalar")
    plain = bool(value) and value not in _APPROVED_YAML_RESERVED
    plain = plain and not _APPROVED_YAML_NUMBER_LIKE.fullmatch(value)
    plain = plain and value[0] not in "-?:,[]{}#&*!|>'\"%@`"
    plain = plain and not any(token in value for token in (
        ": ", " #", "{", "}", "[", "]"))
    plain = plain and not value.endswith(":")
    plain = plain and all(ord(character) >= 0x20 and ord(character) != 0x7f
                          for character in value)
    if plain:
        return value
    return "'" + value.replace("'", "''") + "'"


def _approved_yaml_emit(value: Any, indent: int, output: list[str]) -> None:
    if isinstance(value, dict):
        if not value:
            raise ContractError("approved authority may not contain an empty mapping")
        for key in _approved_yaml_mapping_order(value):
            if not _APPROVED_YAML_KEY.fullmatch(key):
                raise ContractError("approved authority has a noncanonical mapping key")
            child = value[key]
            prefix = " " * indent + key + ":"
            if isinstance(child, dict):
                output.append(prefix)
                _approved_yaml_emit(child, indent + 2, output)
            elif isinstance(child, list):
                if not child:
                    if key == "approved_results":
                        # A newly provisioned worker has no measurements yet.
                        output.append(prefix + " []")
                        continue
                    raise ContractError("approved authority may not contain an empty sequence")
                output.append(prefix)
                _approved_yaml_emit(child, indent, output)
            else:
                scalar = _approved_yaml_emit_scalar(child)
                output.append(prefix + (" " + scalar if scalar else ""))
        return
    if isinstance(value, list):
        if not value:
            raise ContractError("approved authority may not contain an empty sequence")
        for child in value:
            prefix = " " * indent + "-"
            if isinstance(child, dict):
                if not child:
                    raise ContractError("approved authority may not contain an empty mapping")
                ordered = _approved_yaml_mapping_order(child)
                first = ordered[0]
                first_value = child[first]
                if isinstance(first_value, (dict, list)):
                    output.append(prefix)
                    _approved_yaml_emit(child, indent + 2, output)
                else:
                    scalar = _approved_yaml_emit_scalar(first_value)
                    output.append(prefix + " " + first + ":" +
                                  (" " + scalar if scalar else ""))
                    for key in ordered[1:]:
                        nested = child[key]
                        nested_prefix = " " * (indent + 2) + key + ":"
                        if isinstance(nested, dict):
                            output.append(nested_prefix)
                            _approved_yaml_emit(nested, indent + 4, output)
                        elif isinstance(nested, list):
                            output.append(nested_prefix)
                            _approved_yaml_emit(nested, indent + 2, output)
                        else:
                            nested_scalar = _approved_yaml_emit_scalar(nested)
                            output.append(nested_prefix + (
                                " " + nested_scalar if nested_scalar else ""))
            elif isinstance(child, list):
                output.append(prefix)
                _approved_yaml_emit(child, indent + 2, output)
            else:
                scalar = _approved_yaml_emit_scalar(child)
                if not scalar:
                    raise ContractError("a sequence null is not an authorized null form")
                output.append(prefix + " " + scalar)
        return
    raise ContractError("approved authority root must be a mapping")


def _canonical_approved_tuples_yaml_v2(document: dict[str, Any]) -> bytes:
    output = ["---"]
    _approved_yaml_emit(document, 0, output)
    return ("\n".join(output) + "\n").encode("utf-8")


def _canonical_approved_tuples_yaml_v3(document: dict[str, Any]) -> bytes:
    output = ["---"]
    _approved_yaml_emit(document, 0, output)
    return ("\n".join(output) + "\n").encode("utf-8")


def _approved_exact_order(
        value: dict[str, Any], fields: tuple[str, ...], label: str) -> None:
    _require_exact_keys(value, set(fields), label)
    if tuple(value) != fields:
        raise ContractError(f"{label} fields are not in fixed generator order")


def _approved_row_identity(row: dict[str, Any]) -> dict[str, Any]:
    return {
        "campaign_id": row["campaign_id"],
        "dataset": row["dataset"],
        "alphabet": row["alphabet"],
        "target_size_gbp": row["target_size_gbp"],
        "kmer_size": row["kmer_size"],
        "window_size": row["window_size"],
        "index_mode": row["mode"],
        "engine": row["pipeline"],
        "bioseq_index_name": (
            row["index_type"] if row["pipeline"] == "bioseq" else None),
        "requested_minimizer_algorithm": row["requested_algorithms"]["minimizer"],
        "requested_chaining_algorithm": row["requested_algorithms"]["chaining"],
        "requested_base_alignment_algorithm": (
            row["requested_algorithms"]["base_alignment"]),
    }


def _approved_observation_identity(row: dict[str, Any]) -> tuple[str, str]:
    return row["stable_key"], row["timing_scope"]


def _approved_observation_sort_key(row: dict[str, Any]) -> tuple[str, str]:
    return _approved_observation_identity(row)


def _approved_row_in_fixed_order(
        row: dict[str, Any], *, authority_version: int = APPROVED_TUPLES_VERSION,
) -> dict[str, Any]:
    result: dict[str, Any] = {}
    fields = (APPROVED_TUPLES_V2_ROW_FIELDS
              if authority_version == APPROVED_TUPLES_LEGACY_VERSION
              else APPROVED_TUPLES_ROW_FIELDS)
    for field in fields:
        value = row[field]
        if field in {"requested_algorithms", "observed_algorithms"}:
            value = {key: value[key] for key in APPROVED_TUPLES_ALGORITHM_FIELDS}
        elif field == "comparison_eligibility":
            value = {key: value[key] for key in APPROVED_TUPLES_ELIGIBILITY_FIELDS}
        elif field == "metrics":
            value = {key: value[key] for key in APPROVED_TUPLES_METRIC_FIELDS}
        result[field] = copy.deepcopy(value)
    return result


def _validate_approved_tuple_row(
        raw_row: Any,
        campaign_id: str,
        matrix_scope: dict[str, Any],
        label: str,
        *,
        allow_legacy_preservation: bool,
        require_field_order: bool,
        authority_version: int = APPROVED_TUPLES_VERSION,
        fast_local: bool = False,
) -> dict[str, Any]:
    row = _require_dict(raw_row, label)
    fields = (APPROVED_TUPLES_V2_ROW_FIELDS
              if authority_version == APPROVED_TUPLES_LEGACY_VERSION
              else APPROVED_TUPLES_ROW_FIELDS)
    if require_field_order:
        _approved_exact_order(row, fields, label)
    else:
        _require_exact_keys(row, set(fields), label)
    if authority_version == APPROVED_TUPLES_VERSION:
        timing_scope = _require_string(
            row.get("timing_scope"), f"{label}.timing_scope")
        allowed_scopes = (FAST_LOCAL_TIMING_SCOPE,) if fast_local else APPROVED_TUPLES_TIMING_SCOPES
        if timing_scope not in allowed_scopes:
            raise ContractError(f"{label}.timing_scope is not recognized")
    elif authority_version != APPROVED_TUPLES_LEGACY_VERSION:
        raise ContractError(f"{label} uses an unsupported authority version")
    if row.get("stable_key_schema") != "bioseq-e2e-cell-v2":
        raise ContractError(f"{label}.stable_key_schema is not canonical")
    if row.get("campaign_id") != campaign_id:
        raise ContractError(f"{label}.campaign_id differs from the authority")
    for field in ("dataset", "alphabet", "mode", "pipeline", "index_type",
                  "effective_path", "execution_conformance", "status"):
        _require_string(row.get(field), f"{label}.{field}")
    for field in ("target_size_gbp", "kmer_size", "window_size", "query_count"):
        _require_int(row.get(field), f"{label}.{field}", minimum=1)
    if row["query_count"] != matrix_scope.get("query_count"):
        raise ContractError(f"{label}.query_count is outside matrix_scope")
    if row["target_size_gbp"] not in matrix_scope.get("target_size_gbp", []):
        raise ContractError(f"{label}.target_size_gbp is outside matrix_scope")
    if row["alphabet"] not in matrix_scope.get("alphabets", []):
        raise ContractError(f"{label}.alphabet is outside matrix_scope")
    pairs = {
        (pair.get("kmer_size"), pair.get("window_size"))
        for pair in _require_list(
            matrix_scope.get("window_kmer_pairs"), "matrix_scope.window_kmer_pairs")
        if isinstance(pair, dict)
    }
    if (row["kmer_size"], row["window_size"]) not in pairs:
        raise ContractError(f"{label} k-mer/window pair is outside matrix_scope")
    expected_dataset = (
        f"matrix_{row['target_size_gbp']}gbp_{row['alphabet']}_"
        f"k{row['kmer_size']}_w{row['window_size']}")
    if row["dataset"] != expected_dataset:
        raise ContractError(f"{label}.dataset disagrees with its dimensions")
    if row["mode"] not in matrix_scope.get("modes", []):
        raise ContractError(f"{label}.mode is outside matrix_scope")
    if row["pipeline"] not in matrix_scope.get("pipelines", []):
        raise ContractError(f"{label}.pipeline is outside matrix_scope")

    requested = _require_dict(row.get("requested_algorithms"),
                              f"{label}.requested_algorithms")
    observed = _require_dict(row.get("observed_algorithms"),
                             f"{label}.observed_algorithms")
    eligibility = _require_dict(row.get("comparison_eligibility"),
                                f"{label}.comparison_eligibility")
    metrics = _require_dict(row.get("metrics"), f"{label}.metrics")
    for value, fields, nested_label in (
            (requested, APPROVED_TUPLES_ALGORITHM_FIELDS,
             f"{label}.requested_algorithms"),
            (observed, APPROVED_TUPLES_ALGORITHM_FIELDS,
             f"{label}.observed_algorithms"),
            (eligibility, APPROVED_TUPLES_ELIGIBILITY_FIELDS,
             f"{label}.comparison_eligibility"),
            (metrics, APPROVED_TUPLES_METRIC_FIELDS, f"{label}.metrics")):
        if require_field_order:
            _approved_exact_order(value, fields, nested_label)
        else:
            _require_exact_keys(value, set(fields), nested_label)
    for field in ("requested_path", "effective_path"):
        _require_bool(eligibility.get(field), f"{label}.comparison_eligibility.{field}")
    _require_string(eligibility.get("scope"),
                    f"{label}.comparison_eligibility.scope")
    if row.get("prefilter") is not False:
        raise ContractError(f"{label}.prefilter must be false")
    if row.get("exact_baseline_match_count_equal") is not True:
        raise ContractError(
            f"{label}.exact_baseline_match_count_equal must be true")
    if row.get("status") != "accepted":
        raise ContractError(f"{label}.status must be accepted")
    if _require_int(row.get("run_count"), f"{label}.run_count") != (1 if fast_local else 5):
        raise ContractError(f"{label}.run_count differs from the authority repeat policy")
    if metrics.get("unit") != "ns":
        raise ContractError(f"{label}.metrics.unit must be ns")
    p50_a = _require_int(metrics.get("p50_a"), f"{label}.metrics.p50_a", minimum=1)
    p50_b = _require_int(metrics.get("p50_b"), f"{label}.metrics.p50_b", minimum=1)
    if p50_b > p50_a:
        raise ContractError(f"{label}.metrics.p50_b exceeds p50_a")
    if row.get("comments") is not None:
        _require_string(row.get("comments"), f"{label}.comments")

    if row["pipeline"] == "minimap2":
        if (row["pipeline_mode"] is not None
                or row["index_type"] != "minimap2_internal"
                or any(value is not None for value in requested.values())
                or any(value is not None for value in observed.values())
                or row["effective_path"] != "minimap2"
                or row["execution_conformance"] != "external_minimap2_baseline"
                or eligibility != {
                    "requested_path": True,
                    "effective_path": True,
                    "scope": "external_minimap2_baseline",
                }
                ):
            raise ContractError(f"{label} is not a closed minimap2 baseline row")
    else:
        if row["pipeline_mode"] != matrix_scope.get("bioseq_pipeline_mode"):
            raise ContractError(f"{label}.pipeline_mode is not canonical")
        if row["index_type"] not in matrix_scope.get("bioseq_index_types", []):
            raise ContractError(f"{label}.index_type is outside matrix_scope")
        enum_fields = (
            ("minimizer", "bioseq_minimizer_algorithms"),
            ("chaining", "bioseq_chaining_algorithms"),
            ("base_alignment", "bioseq_base_alignment_algorithms"),
        )
        for field, scope_field in enum_fields:
            if requested[field] not in matrix_scope.get(scope_field, []):
                raise ContractError(f"{label} requests an out-of-scope {field}")
            if observed[field] not in matrix_scope.get(scope_field, []):
                raise ContractError(f"{label} observes an out-of-scope {field}")
        if requested["base_alignment"] in matrix_scope.get(
                "excluded_runtime_only_base_alignment_algorithms", []):
            raise ContractError(f"{label} uses an excluded base aligner")
        expected_path = " -> ".join(observed[field]
                                    for field in APPROVED_TUPLES_ALGORITHM_FIELDS)
        if row["effective_path"] != expected_path:
            raise ContractError(f"{label}.effective_path is not the observed path")
        exact_eligibility = {
            "requested_path": True,
            "effective_path": True,
            "scope": "requested_and_effective_path",
        }
        fallback_eligibility = {
            "requested_path": False,
            "effective_path": True,
            "scope": (
                "planned_dna5_reference_scalar_only"
                if row["alphabet"] == "dna5"
                else "historical_scalar_fallback_only"),
        }
        if row["execution_conformance"] == "documented_scalar_minimizer_fallback":
            if (requested["minimizer"] != "simd_minimap2_reference"
                    or observed["minimizer"] != "scalar"
                    or requested["chaining"] != observed["chaining"]
                    or requested["base_alignment"] != observed["base_alignment"]
                    or (row["alphabet"] != "dna5"
                        and (not allow_legacy_preservation
                             or (authority_version == APPROVED_TUPLES_VERSION
                                 and row["timing_scope"]
                                 != APPROVED_TUPLES_HISTORICAL_TIMING_SCOPE)))
                    or eligibility != fallback_eligibility
                    or not isinstance(row["comments"], str)
                    or "simd_minimap2_reference" not in row["comments"]
                    or "scalar" not in row["comments"]):
                raise ContractError(f"{label} has an unqualified scalar fallback")
        elif row["execution_conformance"] in {
                "exact_requested_path", "exact_requested_path_user_preserved"}:
            expected_eligibility = (
                {
                    "requested_path": True,
                    "effective_path": True,
                    "scope": "historical_user_preserved_requested_path",
                }
                if row["execution_conformance"] ==
                "exact_requested_path_user_preserved"
                else exact_eligibility)
            if requested != observed or eligibility != expected_eligibility:
                raise ContractError(
                    f"{label} is not exact requested-path execution")
            if row["execution_conformance"] == "exact_requested_path_user_preserved":
                if (not allow_legacy_preservation
                        or (authority_version == APPROVED_TUPLES_VERSION
                            and row["timing_scope"]
                            != APPROVED_TUPLES_HISTORICAL_TIMING_SCOPE)
                        or not isinstance(row["comments"], str)):
                    raise ContractError(
                        f"{label} cannot create a new user-preservation exception")
            # An exact selected path may still have a publication annotation,
            # e.g. a diagnostic numeric MAPQ difference. Path conformance is
            # proved by requested/observed identities and the sealed metrics,
            # not by requiring its human-readable comment to be empty.
        else:
            raise ContractError(f"{label}.execution_conformance is not approved")

    stable_key = _require_string(row.get("stable_key"), f"{label}.stable_key")
    identity = _approved_row_identity(row)
    expected_stable_key = _stable_key_for_identity(identity)
    if stable_key != expected_stable_key:
        raise ApprovedTupleDuplicateError(
            f"{label}.stable_key does not encode its exact row identity")
    prefix = "bioseq-e2e-cell-v2:"
    decoded = _loads_json(stable_key[len(prefix):], f"{label}.stable_key")
    if decoded != list(identity.values()):
        raise ApprovedTupleDuplicateError(
            f"{label}.stable_key has an identity collision")
    return (row if require_field_order else _approved_row_in_fixed_order(
        row, authority_version=authority_version))


def _is_remote_approved_authority(document: dict[str, Any]) -> bool:
    return (document.get("schema") == APPROVED_TUPLES_REMOTE_SCHEMA
            and document.get("version") == APPROVED_TUPLES_REMOTE_VERSION)


def _remote_host_descriptor_sha256(value: dict[str, Any]) -> str:
    payload = {field: copy.deepcopy(value[field])
               for field in APPROVED_TUPLES_REMOTE_HOST_FIELDS
               if field != "descriptor_sha256"}
    return _sha256(_canonical_bytes(payload))


def _validate_remote_host_descriptor(
        value: Any, label: str, *, require_field_order: bool) -> dict[str, Any]:
    descriptor = _require_dict(value, label)
    if require_field_order:
        _approved_exact_order(
            descriptor, APPROVED_TUPLES_REMOTE_HOST_FIELDS, label)
    else:
        _require_exact_keys(
            descriptor, set(APPROVED_TUPLES_REMOTE_HOST_FIELDS), label)
    if (descriptor.get("schema") != APPROVED_TUPLES_REMOTE_HOST_SCHEMA
            or descriptor.get("version") != APPROVED_TUPLES_REMOTE_HOST_VERSION
            or descriptor.get("cloud_provider") != "aws"
            or descriptor.get("preset_family") != "e2e"):
        raise ContractError(f"{label} schema/provider/preset family changed")
    machine_id = _require_string(
        descriptor.get("machine_id"), f"{label}.machine_id")
    policy = APPROVED_TUPLES_REMOTE_MACHINE_POLICIES.get(machine_id)
    if policy is None:
        raise ContractError(f"{label}.machine_id is not admitted")
    if any(descriptor.get(field) != policy[field] for field in (
            "instance_type", "architecture", "build_preset")):
        raise ContractError(f"{label} differs from the closed machine policy")
    aws_identity = _require_dict(
        descriptor.get("aws_identity"), f"{label}.aws_identity")
    if require_field_order:
        _approved_exact_order(
            aws_identity, APPROVED_TUPLES_REMOTE_AWS_IDENTITY_FIELDS,
            f"{label}.aws_identity")
    else:
        _require_exact_keys(
            aws_identity, set(APPROVED_TUPLES_REMOTE_AWS_IDENTITY_FIELDS),
            f"{label}.aws_identity")
    expected_aws_identity = {
        "account_id": policy["account_id"],
        "region": policy["region"],
        "availability_zone": policy["availability_zone"],
        "instance_id": policy["instance_id"],
        "ec2_architecture": policy["ec2_architecture"],
    }
    if aws_identity != expected_aws_identity:
        raise ContractError(
            f"{label}.aws_identity differs from the exact admitted instance")
    cpu = _require_dict(descriptor.get("cpu"), f"{label}.cpu")
    if require_field_order:
        _approved_exact_order(cpu, APPROVED_TUPLES_REMOTE_CPU_FIELDS,
                              f"{label}.cpu")
    else:
        _require_exact_keys(cpu, set(APPROVED_TUPLES_REMOTE_CPU_FIELDS),
                            f"{label}.cpu")
    model = _require_string(cpu.get("model"), f"{label}.cpu.model")
    isa = _require_string(cpu.get("isa"), f"{label}.cpu.isa")
    for field in ("vcpu_count", "core_count", "threads_per_core"):
        _require_int(cpu.get(field), f"{label}.cpu.{field}", minimum=1)
    if (model.casefold() in {"unknown", "not available", "not_available"}
            or isa.casefold() in {"unknown", "not available", "not_available"}
            or cpu != {
                "architecture": policy["architecture"],
                "model": model,
                "isa": isa,
                "vcpu_count": policy["vcpu_count"],
                "core_count": policy["core_count"],
                "threads_per_core": policy["threads_per_core"],
            }):
        raise ContractError(f"{label}.cpu is not the admitted host inventory")
    memory = _require_dict(descriptor.get("memory"), f"{label}.memory")
    if require_field_order:
        _approved_exact_order(memory, APPROVED_TUPLES_REMOTE_MEMORY_FIELDS,
                              f"{label}.memory")
    else:
        _require_exact_keys(memory, set(APPROVED_TUPLES_REMOTE_MEMORY_FIELDS),
                            f"{label}.memory")
    _require_int(memory.get("physical_mib"), f"{label}.memory.physical_mib",
                 minimum=1)
    if memory != {"physical_mib": policy["physical_memory_mib"]}:
        raise ContractError(
            f"{label}.memory differs from the admitted EC2 physical memory")
    storage = _require_dict(descriptor.get("storage"), f"{label}.storage")
    if require_field_order:
        _approved_exact_order(storage, APPROVED_TUPLES_REMOTE_STORAGE_FIELDS,
                              f"{label}.storage")
    else:
        _require_exact_keys(storage, set(APPROVED_TUPLES_REMOTE_STORAGE_FIELDS),
                            f"{label}.storage")
    for field in (
            "capacity_gib", "provisioned_iops",
            "provisioned_throughput_mib_per_s"):
        _require_int(storage.get(field), f"{label}.storage.{field}", minimum=1)
    expected_storage = {
        "kind": "ebs",
        "root_device": policy["root_device"],
        "volume_id": policy["volume_id"],
        "capacity_gib": policy["capacity_gib"],
        "volume_type": policy["volume_type"],
        "provisioned_iops": policy["provisioned_iops"],
        "provisioned_throughput_mib_per_s": policy[
            "provisioned_throughput_mib_per_s"],
        "encrypted": True,
        "instance_store_present": False,
    }
    if storage != expected_storage:
        raise ContractError(
            f"{label}.storage differs from the exact attached root volume")
    receipt_host = _require_dict(
        descriptor.get("receipt_host"), f"{label}.receipt_host")
    _require_exact_keys(
        receipt_host, {"node", "system", "machine", "release"},
        f"{label}.receipt_host")
    for field in ("node", "system", "machine", "release"):
        _require_string(receipt_host.get(field), f"{label}.receipt_host.{field}")
    if (receipt_host.get("node") != policy["hostname"]
            or receipt_host.get("system") != "Linux"
            or receipt_host.get("machine") != policy["architecture"]):
        raise ContractError(f"{label}.receipt_host is not the admitted Linux host")
    measurement_device = _require_dict(
        descriptor.get("measurement_device"), f"{label}.measurement_device")
    _require_exact_keys(
        measurement_device, {"identity", "version"},
        f"{label}.measurement_device")
    expected_device = {
        "identity": (
            f"cpu:{policy['architecture']}:{receipt_host['node']}"),
        "version": f"Linux:{receipt_host['release']}",
    }
    if measurement_device != expected_device:
        raise ContractError(
            f"{label}.measurement_device differs from the receipt host")
    gpu = _require_dict(descriptor.get("gpu"), f"{label}.gpu")
    if require_field_order:
        _approved_exact_order(gpu, APPROVED_TUPLES_REMOTE_GPU_FIELDS,
                              f"{label}.gpu")
    else:
        _require_exact_keys(gpu, set(APPROVED_TUPLES_REMOTE_GPU_FIELDS),
                            f"{label}.gpu")
    _require_int(gpu.get("device_count"), f"{label}.gpu.device_count")
    if gpu != {
            "available": False,
            "device_count": 0,
            "identity": None,
            "driver": None,
            "vulkan_available": False,
            "aws_gpu_info": None,
    }:
        raise ContractError(f"{label}.gpu is not the CPU-only disposition")
    claimed = _require_hex(
        descriptor.get("descriptor_sha256"), f"{label}.descriptor_sha256", 64)
    if claimed != _remote_host_descriptor_sha256(descriptor):
        raise ContractError(f"{label} seal does not recompute")
    normalized = {field: copy.deepcopy(descriptor[field])
                  for field in APPROVED_TUPLES_REMOTE_HOST_FIELDS}
    normalized["aws_identity"] = {
        field: copy.deepcopy(aws_identity[field])
        for field in APPROVED_TUPLES_REMOTE_AWS_IDENTITY_FIELDS}
    normalized["cpu"] = {
        field: copy.deepcopy(cpu[field])
        for field in APPROVED_TUPLES_REMOTE_CPU_FIELDS}
    normalized["memory"] = {
        field: copy.deepcopy(memory[field])
        for field in APPROVED_TUPLES_REMOTE_MEMORY_FIELDS}
    normalized["storage"] = {
        field: copy.deepcopy(storage[field])
        for field in APPROVED_TUPLES_REMOTE_STORAGE_FIELDS}
    normalized["gpu"] = {
        field: copy.deepcopy(gpu[field])
        for field in APPROVED_TUPLES_REMOTE_GPU_FIELDS}
    return normalized


def _load_remote_host_descriptor(path: Path) -> dict[str, Any]:
    raw = _read_approved_tuple_file(path, "remote host descriptor")
    try:
        value = _loads_json(raw.decode("utf-8"), "remote host descriptor")
    except UnicodeError as exc:
        raise ContractError(f"remote host descriptor is not UTF-8: {exc}") from exc
    return _validate_remote_host_descriptor(
        value, "remote host descriptor", require_field_order=False)


def _remote_hardware_disposition_from_identity(
        identity: dict[str, Any], *, query_count: int) -> dict[str, Any]:
    if not _remote_identity_requires_vulkan(identity):
        raise ContractError("hardware-unavailable disposition is not a Vulkan cell")
    return {
        "stable_key_schema": "bioseq-e2e-cell-v2",
        "stable_key": _stable_key_for_identity(identity),
        "campaign_id": identity["campaign_id"],
        "dataset": identity["dataset"],
        "target_size_gbp": identity["target_size_gbp"],
        "alphabet": identity["alphabet"],
        "kmer_size": identity["kmer_size"],
        "window_size": identity["window_size"],
        "query_count": query_count,
        "mode": identity["index_mode"],
        "pipeline": identity["engine"],
        "pipeline_mode": "task_parallel",
        "index_type": identity["bioseq_index_name"],
        "requested_algorithms": {
            "minimizer": identity["requested_minimizer_algorithm"],
            "chaining": identity["requested_chaining_algorithm"],
            "base_alignment": identity[
                "requested_base_alignment_algorithm"],
        },
        "execution_attempted": False,
        "observed_algorithms": {
            "minimizer": None,
            "chaining": None,
            "base_alignment": None,
        },
        "effective_path": None,
        "prefilter": False,
        "status": "hardware_unavailable",
        "reason": "vulkan_device_unavailable",
        "run_count": 0,
        "metrics": {"unit": "ns", "p50_a": None, "p50_b": None},
    }


def _remote_hardware_disposition_from_row(
        row: dict[str, Any]) -> dict[str, Any]:
    return _remote_hardware_disposition_from_identity(
        _approved_row_identity(row), query_count=row["query_count"])


def _validate_remote_hardware_disposition(
        value: Any, campaign_id: str, label: str) -> dict[str, Any]:
    row = _require_dict(value, label)
    _approved_exact_order(row, APPROVED_TUPLES_REMOTE_DISPOSITION_FIELDS, label)
    identity = _approved_identity_from_stable_key(
        row.get("stable_key"), f"{label}.stable_key")
    requested = _require_dict(
        row.get("requested_algorithms"), f"{label}.requested_algorithms")
    observed = _require_dict(
        row.get("observed_algorithms"), f"{label}.observed_algorithms")
    metrics = _require_dict(row.get("metrics"), f"{label}.metrics")
    _approved_exact_order(requested, APPROVED_TUPLES_ALGORITHM_FIELDS,
                          f"{label}.requested_algorithms")
    _approved_exact_order(observed, APPROVED_TUPLES_ALGORITHM_FIELDS,
                          f"{label}.observed_algorithms")
    _approved_exact_order(metrics, APPROVED_TUPLES_METRIC_FIELDS,
                          f"{label}.metrics")
    expected_identity_fields = {
        "campaign_id": identity["campaign_id"],
        "dataset": identity["dataset"],
        "target_size_gbp": identity["target_size_gbp"],
        "alphabet": identity["alphabet"],
        "kmer_size": identity["kmer_size"],
        "window_size": identity["window_size"],
        "mode": identity["index_mode"],
        "pipeline": identity["engine"],
        "index_type": identity["bioseq_index_name"],
    }
    if (row.get("stable_key_schema") != "bioseq-e2e-cell-v2"
            or identity["campaign_id"] != campaign_id
            or any(row.get(field) != expected
                   for field, expected in expected_identity_fields.items())
            or row.get("query_count") != 1024
            or row.get("pipeline") != "bioseq"
            or row.get("pipeline_mode") != "task_parallel"
            or requested != {
                "minimizer": identity["requested_minimizer_algorithm"],
                "chaining": identity["requested_chaining_algorithm"],
                "base_alignment": identity[
                    "requested_base_alignment_algorithm"],
            }
            or not _remote_identity_requires_vulkan(identity)
            or row.get("execution_attempted") is not False
            or observed != {
                "minimizer": None, "chaining": None, "base_alignment": None}
            or row.get("effective_path") is not None
            or row.get("prefilter") is not False
            or row.get("status") != "hardware_unavailable"
            or row.get("reason") != "vulkan_device_unavailable"
            or row.get("run_count") != 0
            or metrics != {"unit": "ns", "p50_a": None, "p50_b": None}):
        raise ContractError(f"{label} is not an exact unavailable-Vulkan cell")
    return copy.deepcopy(row)


def _remote_eligible_rows_from_local_authority(
        local_rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    # The locally sealed logical identity is the allowlist.  Eligibility is not
    # copied from one historical execution disposition: remote timings are new
    # host observations, while every requested Vulkan identity is separately
    # projected as non-executed hardware-unavailable on these CPU-only hosts.
    return sorted(_approved_logical_rows(local_rows),
                  key=lambda row: row["stable_key"])


def _remote_new_fleet_eligible_identities() -> list[dict[str, Any]]:
    if (ACTIVE_CAMPAIGN_PROFILE != "q1024"
            or CAMPAIGN_CONTRACT_PATH != Q1024_CONTRACT_PATH
            or EXPECTED_QUERY_COUNT != 1024):
        raise ContractError(
            "new-fleet bootstrap requires the canonical q1024 campaign profile")
    campaign = _load_campaign_contract()
    canonical_cells = campaign["canonical_cells"]
    eligible_keys = sorted(canonical_cells)
    if (campaign["campaign_id"] != Q1024_CAMPAIGN_ID
            or campaign["campaign_digest"] != Q1024_CAMPAIGN_DIGEST
            or campaign["contract_sha256"]
                != APPROVED_TUPLES_NEW_FLEET_CAMPAIGN_CONTRACT_SHA256
            or len(eligible_keys)
                != APPROVED_TUPLES_NEW_FLEET_ELIGIBLE_KEY_COUNT
            or _sha256(_canonical_bytes(eligible_keys))
                != APPROVED_TUPLES_NEW_FLEET_ELIGIBLE_KEYS_SHA256):
        raise ContractError(
            "new-fleet eligibility differs from the sealed q1024 campaign")
    return [copy.deepcopy(canonical_cells[key]["identity"])
            for key in eligible_keys]


def _remote_authority_complete(
        eligible_keys: list[str], unavailable_keys: set[str],
        approved_rows: list[dict[str, Any]]) -> bool:
    return set(eligible_keys) == unavailable_keys.union(
        row["stable_key"] for row in approved_rows)


def _regenerate_remote_approved_tuple_projections(
        document: dict[str, Any], rows: list[dict[str, Any]]) -> None:
    rows = sorted(rows, key=_approved_observation_sort_key)
    document["approved_results"] = rows
    eligible_keys = _require_list(
        document.get("eligible_stable_keys"), "eligible_stable_keys")
    dispositions = _require_list(
        document.get("hardware_unavailable_cells"),
        "hardware_unavailable_cells")
    unavailable_keys = {row["stable_key"] for row in dispositions}
    complete = _remote_authority_complete(
        eligible_keys, unavailable_keys, rows)
    authority = _require_dict(document.get("authority"), "authority")
    authority["full_eligible_set_complete"] = complete
    document["status"] = (
        "approved_remote_complete" if complete else "approved_remote_partial")
    provenance = _require_dict(
        document.get("projection_provenance"), "projection_provenance")
    provenance.update({
        "approved_results_projection_sha256":
            _approved_projection_sha256(rows),
        "approved_result_count": len(rows),
        "approved_run_count": sum(row["run_count"] for row in rows),
        "hardware_unavailable_count": len(dispositions),
        "remaining_eligible_count": (
            len(eligible_keys) - len(unavailable_keys) - len(rows)),
    })


def _new_remote_approved_tuple_authority(
        local_authority: dict[str, Any], local_authority_sha256: str,
        host_descriptor: dict[str, Any]) -> dict[str, Any]:
    if (local_authority.get("schema") != APPROVED_TUPLES_SCHEMA
            or local_authority.get("version") != APPROVED_TUPLES_VERSION):
        raise ContractError("remote bootstrap requires a validated local v3 authority")
    local_rows = _require_list(
        local_authority.get("approved_results"), "local approved_results")
    descriptor = _validate_remote_host_descriptor(
        host_descriptor, "remote host binding", require_field_order=False)
    if descriptor["machine_id"] in APPROVED_TUPLES_NEW_FLEET_MACHINE_IDS:
        eligible_identities = _remote_new_fleet_eligible_identities()
        eligible_keys = [
            _stable_key_for_identity(identity)
            for identity in eligible_identities]
        dispositions = [
            _remote_hardware_disposition_from_identity(
                identity, query_count=1024)
            for identity in eligible_identities
            if _remote_identity_requires_vulkan(identity)]
        purpose = APPROVED_TUPLES_NEW_FLEET_PURPOSE
    else:
        eligible_rows = _remote_eligible_rows_from_local_authority(local_rows)
        eligible_keys = [row["stable_key"] for row in eligible_rows]
        dispositions = [
            _remote_hardware_disposition_from_row(row) for row in eligible_rows
            if _remote_identity_requires_vulkan(_approved_row_identity(row))]
        purpose = (
            "Host-local approved E2E timings for one sealed AWS CPU machine. "
            "The authority embeds its exact EC2, CPU, RAM, root-volume, and "
            "no-GPU inventory. "
            "Linux E2E intervals use CLOCK_MONOTONIC under the existing strong "
            "same-kernel nanosecond-rate contract; unavailable Vulkan cells are "
            "non-executed dispositions, never zero-time results.")
    local_projection = _approved_projection_sha256(local_rows)
    document = {
        "schema": APPROVED_TUPLES_REMOTE_SCHEMA,
        "version": APPROVED_TUPLES_REMOTE_VERSION,
        "status": "approved_remote_partial",
        "purpose": purpose,
        "authority": {
            "profile": APPROVED_TUPLES_REMOTE_PROFILE,
            "normative_datasets": [
                "approved_results", "hardware_unavailable_cells"],
            "contains_only_approved_rows": True,
            "host_timings_only": True,
            "gpu_execution_prohibited": True,
            "external_lookup_required_to_interpret": False,
            "no_external_path_or_reference_dependency": True,
            "sealed_verification_manifest_required_for_append": True,
            "full_eligible_set_complete": False,
        },
        "projection_provenance": {
            "classification": "derived_non_authoritative",
            "generator": "bioseq-approved-results-projection",
            "generator_version": APPROVED_TUPLES_REMOTE_VERSION,
            "source_local_authority_sha256": local_authority_sha256,
            "source_local_approved_results_projection_sha256": local_projection,
            "eligible_stable_keys_projection_sha256":
                _sha256(_canonical_bytes(eligible_keys)),
            "hardware_unavailable_cells_projection_sha256":
                _sha256(_canonical_bytes(dispositions)),
            "approved_results_projection_sha256":
                _approved_projection_sha256([]),
            "approved_result_count": 0,
            "approved_run_count": 0,
            "hardware_unavailable_count": len(dispositions),
            "remaining_eligible_count": len(eligible_keys) - len(dispositions),
        },
        "campaign_identity": copy.deepcopy(
            local_authority["campaign_identity"]),
        "stable_key_contract": copy.deepcopy(
            local_authority["stable_key_contract"]),
        "matrix_scope": copy.deepcopy(local_authority["matrix_scope"]),
        "remote_host_binding": descriptor,
        "eligible_stable_keys": eligible_keys,
        "hardware_unavailable_cells": dispositions,
        "append_update_protocol": {
            "mode": "additive_audited_rows_only",
            "writer": {
                "name": "bioseq-approved-results-aggregator",
                "version": 3,
                "lock": {
                    "name": "bioseq-approved-results-publication",
                    "version": 1,
                },
            },
            "canonical_serializer": {
                "name": "bioseq-approved-results-yaml",
                "version": APPROVED_TUPLES_REMOTE_VERSION,
            },
            "sealed_append_candidate": {
                "name": APPROVED_TUPLES_CANDIDATE_SCHEMA,
                "version": APPROVED_TUPLES_REMOTE_CANDIDATE_VERSION,
                "verification_manifest": {
                    "name": APPROVED_TUPLES_MANIFEST_SCHEMA,
                    "version": APPROVED_TUPLES_MANIFEST_VERSION,
                    "proof_categories": list(
                        APPROVED_TUPLES_PROOF_CATEGORIES),
                },
            },
        },
        "approved_results": [],
    }
    return document


def _remote_bootstrap_seed_sha256(document: dict[str, Any]) -> str:
    if (not _is_remote_approved_authority(document)
            or document.get("approved_results") != []):
        raise ContractError(
            "remote bootstrap seed requires the generated empty in-memory authority")
    return _sha256(_canonical_bytes(document))


def _validate_remote_approved_tuple_authority_document(
        document: dict[str, Any], raw: bytes | None = None
) -> list[dict[str, Any]]:
    _approved_exact_order(
        document, APPROVED_TUPLES_REMOTE_TOP_LEVEL_FIELDS,
        "remote approved tuple authority")
    if (document.get("schema") != APPROVED_TUPLES_REMOTE_SCHEMA
            or document.get("version") != APPROVED_TUPLES_REMOTE_VERSION
            or document.get("status") not in {
                "approved_remote_partial", "approved_remote_complete"}):
        raise ContractError("remote approved tuple authority schema/status changed")
    _require_string(document.get("purpose"), "remote authority purpose")
    authority = _require_dict(document.get("authority"), "authority")
    _require_exact_keys(authority, {
        "profile", "normative_datasets", "contains_only_approved_rows",
        "host_timings_only", "gpu_execution_prohibited",
        "external_lookup_required_to_interpret",
        "no_external_path_or_reference_dependency",
        "sealed_verification_manifest_required_for_append",
        "full_eligible_set_complete",
    }, "remote authority flags")
    if (authority.get("profile") != APPROVED_TUPLES_REMOTE_PROFILE
            or authority.get("normative_datasets") != [
                "approved_results", "hardware_unavailable_cells"]
            or authority.get("contains_only_approved_rows") is not True
            or authority.get("host_timings_only") is not True
            or authority.get("gpu_execution_prohibited") is not True
            or authority.get("external_lookup_required_to_interpret") is not False
            or authority.get("no_external_path_or_reference_dependency") is not True
            or type(authority.get(
                "sealed_verification_manifest_required_for_append")) is not bool):
        raise ContractError("remote authority ownership flags changed")
    campaign = _require_dict(document.get("campaign_identity"),
                             "campaign_identity")
    if (campaign.get("campaign_id") != Q1024_CAMPAIGN_ID
            or campaign.get("campaign_digest") != Q1024_CAMPAIGN_DIGEST):
        raise ContractError("remote authority campaign identity changed")
    stable_contract = _require_dict(
        document.get("stable_key_contract"), "stable_key_contract")
    if (stable_contract.get("stable_key_schema") != "bioseq-e2e-cell-v2"
            or stable_contract.get("ordered_aggregator_fields")
                != list(STABLE_KEY_FIELDS)):
        raise ContractError("remote stable-key contract changed")
    matrix = _require_dict(document.get("matrix_scope"), "matrix_scope")
    if (matrix.get("total_expected_cells") != EXPECTED_RESULT_CELLS
            or matrix.get("repetitions_per_cell") != MIN_ACCEPTED_REPEATS
            or matrix.get("query_count") != 1024
            or matrix.get("prefilter") is not False
            or matrix.get("cigar_projection") is not True
            or matrix.get("mapq_projection") is not True):
        raise ContractError("remote matrix scope changed")
    host = _validate_remote_host_descriptor(
        document.get("remote_host_binding"), "remote_host_binding",
        require_field_order=True)
    eligible_keys = _require_list(
        document.get("eligible_stable_keys"), "eligible_stable_keys")
    if (not eligible_keys or eligible_keys != sorted(eligible_keys)
            or len(eligible_keys) != len(set(eligible_keys))):
        raise ContractError("eligible_stable_keys must be one sorted unique allowlist")
    if host["machine_id"] in APPROVED_TUPLES_NEW_FLEET_MACHINE_IDS:
        purpose_valid = document.get("purpose") == APPROVED_TUPLES_NEW_FLEET_PURPOSE
        if authority.get("sealed_verification_manifest_required_for_append") is False:
            purpose_valid = document.get("purpose", "").startswith(
                APPROVED_TUPLES_NEW_FLEET_PURPOSE + " Recovery policy authorized 2026-09-21:")
        if (not purpose_valid
                or len(eligible_keys)
                    != APPROVED_TUPLES_NEW_FLEET_ELIGIBLE_KEY_COUNT
                or _sha256(_canonical_bytes(eligible_keys))
                    != APPROVED_TUPLES_NEW_FLEET_ELIGIBLE_KEYS_SHA256):
            raise ContractError(
                "new-fleet eligibility/provenance differs from the sealed "
                "q1024 campaign")
    identities: dict[str, dict[str, Any]] = {}
    matrix_pairs = {
        (item.get("kmer_size"), item.get("window_size"))
        for item in _require_list(matrix.get("window_kmer_pairs"),
                                  "matrix_scope.window_kmer_pairs")
        if isinstance(item, dict)
    }
    for index, key in enumerate(eligible_keys):
        identity = _approved_identity_from_stable_key(
            key, f"eligible_stable_keys[{index}]")
        if (identity["campaign_id"] != Q1024_CAMPAIGN_ID
                or identity["dataset"] != (
                    f"matrix_{identity['target_size_gbp']}gbp_"
                    f"{identity['alphabet']}_k{identity['kmer_size']}_"
                    f"w{identity['window_size']}")
                or identity["target_size_gbp"] not in matrix.get(
                    "target_size_gbp", [])
                or identity["alphabet"] not in matrix.get("alphabets", [])
                or (identity["kmer_size"], identity["window_size"])
                    not in matrix_pairs
                or identity["index_mode"] not in {"just_index", "no_index"}
                or identity["engine"] not in matrix.get("pipelines", [])):
            raise ContractError(
                f"eligible_stable_keys[{index}] is outside the sealed matrix")
        if identity["engine"] == "minimap2":
            if (identity["bioseq_index_name"] is not None
                    or any(identity[field] is not None for field in (
                        "requested_minimizer_algorithm",
                        "requested_chaining_algorithm",
                        "requested_base_alignment_algorithm"))):
                raise ContractError("eligible minimap2 key has Bioseq fields")
        elif (identity["bioseq_index_name"] not in matrix.get(
                    "bioseq_index_types", [])
                or identity["requested_minimizer_algorithm"] not in matrix.get(
                    "bioseq_minimizer_algorithms", [])
                or identity["requested_chaining_algorithm"] not in matrix.get(
                    "bioseq_chaining_algorithms", [])
                or identity["requested_base_alignment_algorithm"] not in matrix.get(
                    "bioseq_base_alignment_algorithms", [])):
            raise ContractError("eligible Bioseq key is outside the sealed matrix")
        identities[key] = identity
    raw_dispositions = _require_list(
        document.get("hardware_unavailable_cells"),
        "hardware_unavailable_cells")
    dispositions = [
        _validate_remote_hardware_disposition(
            value, Q1024_CAMPAIGN_ID,
            f"hardware_unavailable_cells[{index}]")
        for index, value in enumerate(raw_dispositions)]
    unavailable_keys = [row["stable_key"] for row in dispositions]
    expected_unavailable = [key for key in eligible_keys
                            if _remote_identity_requires_vulkan(identities[key])]
    if unavailable_keys != expected_unavailable:
        raise ContractError(
            "hardware_unavailable_cells is not the exact Vulkan allowlist projection")
    raw_rows = _require_list(document.get("approved_results"), "approved_results")
    # Empty approved_results is an honest new-worker inventory, not a result.
    rows = [
        _validate_approved_tuple_row(
            raw_row, Q1024_CAMPAIGN_ID, matrix,
            f"approved_results[{index}]", allow_legacy_preservation=False,
            require_field_order=True,
            authority_version=APPROVED_TUPLES_VERSION)
        for index, raw_row in enumerate(raw_rows)]
    observation_identities = [
        _approved_observation_identity(row) for row in rows]
    if (observation_identities != sorted(observation_identities)
            or len(observation_identities) != len(set(observation_identities))):
        raise ApprovedTupleDuplicateError(
            "remote approved_results is not sorted and unique")
    unavailable_set = set(unavailable_keys)
    approved_keys = {row["stable_key"] for row in rows}
    for index, row in enumerate(rows):
        if (row["timing_scope"] != APPROVED_TUPLES_QUERY_TIMING_SCOPE
                or row["stable_key"] not in identities
                or row["stable_key"] in unavailable_set
                or row["execution_conformance"] not in {
                    "exact_requested_path", "external_minimap2_baseline"}):
            raise ContractError(
                f"approved_results[{index}] is not one host-local eligible result")
        if (row["pipeline"] == "bioseq"
                and _remote_baseline_stable_key(row) not in approved_keys):
            raise ContractError(
                f"approved_results[{index}] lacks its same-host minimap2 baseline")
    protocol = _require_dict(
        document.get("append_update_protocol"), "append_update_protocol")
    expected_protocol = {
        "mode": "additive_audited_rows_only",
        "writer": {
            "name": "bioseq-approved-results-aggregator",
            "version": 3,
            "lock": {
                "name": "bioseq-approved-results-publication", "version": 1},
        },
        "canonical_serializer": {
            "name": "bioseq-approved-results-yaml",
            "version": APPROVED_TUPLES_REMOTE_VERSION,
        },
        "sealed_append_candidate": {
            "name": APPROVED_TUPLES_CANDIDATE_SCHEMA,
            "version": APPROVED_TUPLES_REMOTE_CANDIDATE_VERSION,
            "verification_manifest": {
                "name": APPROVED_TUPLES_MANIFEST_SCHEMA,
                "version": APPROVED_TUPLES_MANIFEST_VERSION,
                "proof_categories": list(APPROVED_TUPLES_PROOF_CATEGORIES),
            },
        },
    }
    if protocol != expected_protocol:
        raise ContractError("remote append protocol projection changed")
    provenance = _require_dict(
        document.get("projection_provenance"), "projection_provenance")
    _require_exact_keys(provenance, {
        "classification", "generator", "generator_version",
        "source_local_authority_sha256",
        "source_local_approved_results_projection_sha256",
        "eligible_stable_keys_projection_sha256",
        "hardware_unavailable_cells_projection_sha256",
        "approved_results_projection_sha256", "approved_result_count",
        "approved_run_count", "hardware_unavailable_count",
        "remaining_eligible_count",
    }, "remote projection_provenance")
    _require_hex(provenance.get("source_local_authority_sha256"),
                 "source local authority SHA-256", 64)
    _require_hex(
        provenance.get("source_local_approved_results_projection_sha256"),
        "source local projection SHA-256", 64)
    expected_complete = _remote_authority_complete(
        eligible_keys, unavailable_set, rows)
    if (provenance.get("classification") != "derived_non_authoritative"
            or provenance.get("generator")
                != "bioseq-approved-results-projection"
            or provenance.get("generator_version")
                != APPROVED_TUPLES_REMOTE_VERSION
            or provenance.get("eligible_stable_keys_projection_sha256")
                != _sha256(_canonical_bytes(eligible_keys))
            or provenance.get("hardware_unavailable_cells_projection_sha256")
                != _sha256(_canonical_bytes(dispositions))
            or provenance.get("approved_results_projection_sha256")
                != _approved_projection_sha256(rows)
            or provenance.get("approved_result_count") != len(rows)
            or provenance.get("approved_run_count")
                != sum(row["run_count"] for row in rows)
            or provenance.get("hardware_unavailable_count")
                != len(dispositions)
            or provenance.get("remaining_eligible_count")
                != len(eligible_keys) - len(dispositions) - len(rows)
            or authority.get("full_eligible_set_complete") is not expected_complete
            or document.get("status") != (
                "approved_remote_complete" if expected_complete
                else "approved_remote_partial")):
        raise ContractError("remote authority projections are stale")
    if raw is not None and raw != _canonical_approved_tuples_yaml_v3(document):
        raise ContractError("remote approved tuple authority is not canonical YAML")
    # A descriptor is normative and intentionally contributes no path.  Its
    # digest is returned so callers can bind candidates without reproducing it.
    if host["descriptor_sha256"] != document[
            "remote_host_binding"]["descriptor_sha256"]:
        raise ContractError("remote host descriptor changed during validation")
    return rows


def _approved_projection_sha256(rows: list[dict[str, Any]]) -> str:
    return _sha256(_canonical_bytes(rows))


def _approved_logical_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Return one deterministic representative for each unchanged stable key."""
    by_key: dict[str, dict[str, Any]] = {}
    for row in rows:
        current = by_key.get(row["stable_key"])
        if (current is None
                or (row.get("timing_scope") == APPROVED_TUPLES_QUERY_TIMING_SCOPE
                    and current.get("timing_scope")
                    != APPROVED_TUPLES_QUERY_TIMING_SCOPE)):
            by_key[row["stable_key"]] = row
    return [by_key[key] for key in sorted(by_key)]


def _approved_rows_by_timing_scope(
        rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [{
        "timing_scope": timing_scope,
        "approved_rows": sum(
            row.get("timing_scope") == timing_scope for row in rows),
        "contributing_runs": sum(
            row["run_count"] for row in rows
            if row.get("timing_scope") == timing_scope),
    } for timing_scope in APPROVED_TUPLES_TIMING_SCOPES]


def _approved_historical_v2_rows(
        rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for row in rows:
        if row.get("timing_scope") != APPROVED_TUPLES_HISTORICAL_TIMING_SCOPE:
            continue
        legacy = {key: copy.deepcopy(value) for key, value in row.items()
                  if key != "timing_scope"}
        result.append(_approved_row_in_fixed_order(
            legacy, authority_version=APPROVED_TUPLES_LEGACY_VERSION))
    return sorted(result, key=lambda row: row["stable_key"])


def _validate_approved_historical_v2_preservation(
        document: dict[str, Any], rows: list[dict[str, Any]]) -> None:
    provenance = _require_dict(
        document.get("projection_provenance"), "projection_provenance")
    preservation = _require_dict(
        provenance.get("historical_v2_preservation"),
        "projection_provenance.historical_v2_preservation")
    _require_exact_keys(preservation, {
        "source_authority_sha256", "approved_results_projection_sha256",
        "observation_count", "timing_scope", "projection_rule",
    }, "projection_provenance.historical_v2_preservation")
    historical_rows = _approved_historical_v2_rows(rows)
    if (preservation.get("source_authority_sha256")
            != APPROVED_TUPLES_LEGACY_SHA256
            or preservation.get("approved_results_projection_sha256")
            != APPROVED_TUPLES_LEGACY_PROJECTION_SHA256
            or preservation.get("observation_count") != 66
            or preservation.get("timing_scope")
            != APPROVED_TUPLES_HISTORICAL_TIMING_SCOPE
            or preservation.get("projection_rule")
            != "strip timing_scope, restore v2 row order, sort by stable_key"
            or len(historical_rows) != 66
            or _approved_projection_sha256(historical_rows)
            != APPROVED_TUPLES_LEGACY_PROJECTION_SHA256):
        raise ContractError("historical v2 approved-row projection changed")


def _approved_count_rows(
        rows: list[dict[str, Any]], field: str, values: list[Any]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for value in values:
        count = sum(row[field] == value for row in rows)
        result.append({"key": value, "approved_rows": count,
                       "valid_runs": count * MIN_ACCEPTED_REPEATS})
    return result


def _approved_count_logical_rows(
        rows: list[dict[str, Any]], field: str,
        values: list[Any]) -> list[dict[str, Any]]:
    return [{
        "key": value,
        "approved_logical_cells": sum(row[field] == value for row in rows),
    } for value in values]


# Exact historical nonvalid identities from the frozen evidence snapshot
# f14f283215a444aa4b0ec1612923d572fbad9b2b875f93d340215ab4a08acc1f.
# These describe past attempts, never future route capability. A later approved
# row supersedes that cell's current disposition without erasing its attempts.
_APPROVED_HISTORICAL_NONVALID_CELLS = (
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "bounded_nthash_simd", "vulkan_tiled_dynamic_programming", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "bounded_nthash_simd", "vulkan_tiled_dynamic_programming", "ksw2", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "scalar", "vulkan_tiled_dynamic_programming", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "scalar", "vulkan_tiled_dynamic_programming", "ksw2", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "simd_minimap2_reference", "vulkan_tiled_dynamic_programming", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "simd_minimap2_reference", "vulkan_tiled_dynamic_programming", "ksw2", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "two_stack_nthash", "vulkan_tiled_dynamic_programming", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "two_stack_nthash", "vulkan_tiled_dynamic_programming", "ksw2", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "vulkan_minimap_integer", "range_minimum_query", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "vulkan_minimap_integer", "range_minimum_query", "ksw2", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "vulkan_minimap_integer", "scalar_minimap2", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "vulkan_minimap_integer", "scalar_minimap2", "ksw2", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "vulkan_minimap_integer", "simd_minimap2", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "vulkan_minimap_integer", "simd_minimap2", "ksw2", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "vulkan_minimap_integer", "vulkan_tiled_dynamic_programming", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "vulkan_minimap_integer", "vulkan_tiled_dynamic_programming", "ksw2", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_sequence", "vulkan_minimap_integer", "range_minimum_query", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_sequence", "vulkan_minimap_integer", "range_minimum_query", "ksw2", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_sequence", "vulkan_minimap_integer", "scalar_minimap2", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_sequence", "vulkan_minimap_integer", "scalar_minimap2", "ksw2", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_sequence", "vulkan_minimap_integer", "simd_minimap2", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_sequence", "vulkan_minimap_integer", "simd_minimap2", "ksw2", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_sequence", "vulkan_minimap_integer", "vulkan_tiled_dynamic_programming", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "just_index", "minimizers_per_sequence", "vulkan_minimap_integer", "vulkan_tiled_dynamic_programming", "ksw2", "rejected"),
    (2, "dna4", 15, 10, "no_index", "minimizers_per_fragment", "bounded_nthash_simd", "range_minimum_query", "block_aligner", "invalid"),
    (2, "dna4", 15, 10, "no_index", "minimizers_per_fragment", "vulkan_minimap_integer", "range_minimum_query", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "no_index", "minimizers_per_fragment", "vulkan_minimap_integer", "range_minimum_query", "ksw2", "rejected"),
    (2, "dna4", 15, 10, "no_index", "minimizers_per_fragment", "vulkan_minimap_integer", "scalar_minimap2", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "no_index", "minimizers_per_fragment", "vulkan_minimap_integer", "scalar_minimap2", "ksw2", "rejected"),
    (2, "dna4", 15, 10, "no_index", "minimizers_per_fragment", "vulkan_minimap_integer", "simd_minimap2", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "no_index", "minimizers_per_fragment", "vulkan_minimap_integer", "simd_minimap2", "ksw2", "rejected"),
    (2, "dna4", 15, 10, "no_index", "minimizers_per_fragment", "vulkan_minimap_integer", "vulkan_tiled_dynamic_programming", "block_aligner", "rejected"),
    (2, "dna4", 15, 10, "no_index", "minimizers_per_fragment", "vulkan_minimap_integer", "vulkan_tiled_dynamic_programming", "ksw2", "rejected"),
    (2, "dna4", 20, 15, "just_index", "minimizers_per_fragment", "scalar", "range_minimum_query", "ksw2", "rejected"),
    (2, "dna4", 20, 15, "just_index", "minimizers_per_fragment", "scalar", "scalar_minimap2", "ksw2", "rejected"),
    (2, "dna4", 20, 15, "just_index", "minimizers_per_fragment", "scalar", "simd_minimap2", "ksw2", "rejected"),
    (2, "dna4", 20, 15, "just_index", "minimizers_per_fragment", "scalar", "vulkan_tiled_dynamic_programming", "ksw2", "rejected"),
    (2, "dna5", 15, 10, "just_index", "minimizers_per_fragment", "bounded_nthash_simd", "simd_minimap2", "block_aligner", "rejected"),
    (2, "dna5", 20, 15, "just_index", "minimizers_per_sequence", "scalar", "scalar_minimap2", "block_aligner", "rejected"),
    (4, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "bounded_nthash_simd", "vulkan_tiled_dynamic_programming", "block_aligner", "rejected"),
    (4, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "bounded_nthash_simd", "vulkan_tiled_dynamic_programming", "ksw2", "rejected"),
    (4, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "scalar", "vulkan_tiled_dynamic_programming", "block_aligner", "rejected"),
    (4, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "scalar", "vulkan_tiled_dynamic_programming", "ksw2", "rejected"),
    (4, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "simd_minimap2_reference", "vulkan_tiled_dynamic_programming", "block_aligner", "rejected"),
    (4, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "simd_minimap2_reference", "vulkan_tiled_dynamic_programming", "ksw2", "rejected"),
    (4, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "two_stack_nthash", "vulkan_tiled_dynamic_programming", "block_aligner", "rejected"),
    (4, "dna4", 15, 10, "just_index", "minimizers_per_fragment", "two_stack_nthash", "vulkan_tiled_dynamic_programming", "ksw2", "rejected"),
    (4, "dna4", 15, 10, "no_index", "minimizers_per_fragment", "bounded_nthash_simd", "simd_minimap2", "block_aligner", "rejected"),
)


def _approved_historical_nonvalid_rows() -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for target, alphabet, kmer, window, mode, index, seed, chain, base, status in (
            _APPROVED_HISTORICAL_NONVALID_CELLS):
        row = {
            "campaign_id": Q1024_CAMPAIGN_ID,
            "dataset": f"matrix_{target}gbp_{alphabet}_k{kmer}_w{window}",
            "target_size_gbp": target, "alphabet": alphabet,
            "kmer_size": kmer, "window_size": window,
            "mode": mode, "pipeline": "bioseq", "index_type": index,
            "requested_algorithms": {
                "minimizer": seed, "chaining": chain, "base_alignment": base},
            "status": status, "run_count": MIN_ACCEPTED_REPEATS,
        }
        row["stable_key"] = _stable_key_for_identity(_approved_row_identity(row))
        rows.append(row)
    return rows


def _approved_corrected_summaries(
        document: dict[str, Any], rows: list[dict[str, Any]], *,
        legacy_layout: bool = False) -> dict[str, Any]:
    excluded = _require_dict(
        document.get("excluded_and_nonvalid_summary"),
        "excluded_and_nonvalid_summary")
    rejected = _require_dict(excluded.get("rejected"), "excluded rejected")
    invalid = _require_dict(excluded.get("invalid"), "excluded invalid")
    matrix = _require_dict(document.get("matrix_scope"), "matrix_scope")
    rejected_cells = _require_int(rejected.get("cells"), "rejected.cells")
    rejected_runs = _require_int(
        rejected.get("recorded_attempts"), "rejected.recorded_attempts")
    invalid_cells = _require_int(invalid.get("cells"), "invalid.cells")
    invalid_runs = _require_int(
        invalid.get("recorded_attempts"), "invalid.recorded_attempts")
    timing_scoped = bool(rows) and all("timing_scope" in row for row in rows)
    coverage_rows = _approved_logical_rows(rows) if timing_scoped else rows
    query_rows = ([row for row in rows if row.get("timing_scope")
                   == APPROVED_TUPLES_QUERY_TIMING_SCOPE]
                  if timing_scoped else rows)
    reason_groups = _require_list(
        rejected.get("reason_groups_by_cell_count"),
        "rejected.reason_groups_by_cell_count")
    reason_cell_total = 0
    for index, raw_group in enumerate(reason_groups):
        group = _require_dict(raw_group, f"rejected reason group[{index}]")
        _require_exact_keys(group, {"reason", "cells"},
                            f"rejected reason group[{index}]")
        _require_string(group.get("reason"), f"rejected reason group[{index}].reason")
        reason_cell_total += _require_int(
            group.get("cells"), f"rejected reason group[{index}].cells", minimum=1)
    if (reason_cell_total != rejected_cells
            or rejected_runs != rejected_cells * MIN_ACCEPTED_REPEATS
            or invalid_runs != invalid_cells * MIN_ACCEPTED_REPEATS):
        raise ContractError("frozen rejected/invalid disposition totals do not reconcile")
    historical = _approved_historical_nonvalid_rows()
    if (rejected_cells != sum(row["status"] == "rejected" for row in historical)
            or invalid_cells != sum(row["status"] == "invalid" for row in historical)):
        raise ContractError("frozen rejected/invalid identities do not reconcile")
    approved_keys = {row["stable_key"] for row in coverage_rows}
    remaining_rejected = sum(
        row["status"] == "rejected" and row["stable_key"] not in approved_keys
        for row in historical)
    remaining_invalid = sum(
        row["status"] == "invalid" and row["stable_key"] not in approved_keys
        for row in historical)
    repaired_cells = rejected_cells + invalid_cells - remaining_rejected - remaining_invalid
    total_cells = _require_int(matrix.get("total_expected_cells"),
                               "matrix_scope.total_expected_cells")
    total_runs = _require_int(matrix.get("total_expected_runs"),
                              "matrix_scope.total_expected_runs")
    approved_runs = sum(row["run_count"] for row in rows)
    if len(coverage_rows) + remaining_rejected + remaining_invalid > total_cells:
        raise ContractError("approved/rejected/invalid cells exceed the matrix")
    exact_rows = [row for row in rows
                  if row["pipeline"] == "bioseq"
                  and row["execution_conformance"] in {
                      "exact_requested_path", "exact_requested_path_user_preserved"}]
    preserved = sum(row["execution_conformance"] ==
                    "exact_requested_path_user_preserved" for row in rows)
    fallback = [row for row in rows if row["execution_conformance"] ==
                "documented_scalar_minimizer_fallback"]
    external = [row for row in rows if row["execution_conformance"] ==
                "external_minimap2_baseline"]
    eligible = [row for row in rows
                if row["comparison_eligibility"]["requested_path"]]

    def counts(selected: list[dict[str, Any]]) -> tuple[int, int]:
        return len(selected), sum(row["run_count"] for row in selected)

    exact_count, exact_runs = counts(exact_rows)
    fallback_count, fallback_runs = counts(fallback)
    external_count, external_runs = counts(external)
    eligible_count, eligible_runs = counts(eligible)
    by_window: list[dict[str, Any]] = []
    for pair in matrix["window_kmer_pairs"]:
        selected = [row for row in coverage_rows
                    if row["window_size"] == pair["window_size"]
                    and row["kmer_size"] == pair["kmer_size"]]
        if timing_scoped:
            by_window.append({
                "window_size": pair["window_size"],
                "kmer_size": pair["kmer_size"],
                "approved_logical_cells": len(selected),
            })
        else:
            count, valid_runs = counts(selected)
            by_window.append({
                "window_size": pair["window_size"],
                "kmer_size": pair["kmer_size"],
                "approved_rows": count,
                "valid_runs": valid_runs,
            })
    missing_cells = (
        total_cells - len(coverage_rows) - remaining_rejected - remaining_invalid)
    query_keys = {row["stable_key"] for row in query_rows}
    query_missing_cells = total_cells - len(query_keys)
    accepted_marginals = (
        {
            "by_pipeline": _approved_count_logical_rows(
                coverage_rows, "pipeline", list(matrix["pipelines"])),
            "by_mode": _approved_count_logical_rows(
                coverage_rows, "mode", ["just_index", "no_index"]),
            "by_index_type": _approved_count_logical_rows(
                coverage_rows, "index_type",
                list(matrix["bioseq_index_types"]) + ["minimap2_internal"]),
            "by_target_size_gbp": _approved_count_logical_rows(
                coverage_rows, "target_size_gbp", list(matrix["target_size_gbp"])),
            "by_alphabet": _approved_count_logical_rows(
                coverage_rows, "alphabet", list(matrix["alphabets"])),
            "by_window_kmer": by_window,
        }
        if timing_scoped else {
            "by_pipeline": _approved_count_rows(
                rows, "pipeline", list(matrix["pipelines"])),
            "by_mode": _approved_count_rows(rows, "mode", ["just_index", "no_index"]),
            "by_index_type": _approved_count_rows(
                rows, "index_type",
                list(matrix["bioseq_index_types"]) + ["minimap2_internal"]),
            "by_target_size_gbp": _approved_count_rows(
                rows, "target_size_gbp", list(matrix["target_size_gbp"])),
            "by_alphabet": _approved_count_rows(
                rows, "alphabet", list(matrix["alphabets"])),
            "by_window_kmer": by_window,
        })
    return {
        "classification": "derived_non_authoritative",
        "projection_description": (
            "Generated count projections recomputed from approved_results and "
            "frozen campaign disposition totals."
            if legacy_layout else
            ("Logical coverage uses unique stable_key values; approved_rows and "
             "approved_valid_runs count actual timing observations and their "
             "contributing runs. Query-only completion and each timing scope are "
             "reported separately from frozen historical observations."
             if timing_scoped else
            "Current cell coverage is the disjoint union of approved identities "
            "and historical nonvalid identities not yet approved. Recorded attempts "
            "retain historical failures, including cells later repaired; they are "
            "not a partition of the planned five valid runs per cell.")),
        "overall": {
            "total_matrix_cells": total_cells,
            "total_planned_runs": total_runs,
            "materialized_cells": (
                len(coverage_rows) + remaining_rejected + remaining_invalid),
            "recorded_attempts": approved_runs + rejected_runs + invalid_runs,
            "approved_rows": len(rows),
            "approved_valid_runs": approved_runs,
            "rejected_cells_excluded": remaining_rejected,
            "rejected_attempts_excluded": rejected_runs,
            "invalid_cells_excluded": remaining_invalid,
            "invalid_attempts_excluded": invalid_runs,
            "missing_cells": missing_cells,
            "unrecorded_planned_runs": missing_cells * MIN_ACCEPTED_REPEATS,
            **({} if legacy_layout else {
                "historical_rejected_cells": rejected_cells,
                "historical_invalid_cells": invalid_cells,
                "historically_nonvalid_now_approved_cells": repaired_cells,
                "planned_valid_runs_still_needed": (
                    query_missing_cells * MIN_ACCEPTED_REPEATS
                    if timing_scoped else total_runs - approved_runs),
                **({
                    "approved_logical_cells": len(coverage_rows),
                    "query_only_approved_cells": len(query_keys),
                    "query_only_missing_cells": query_missing_cells,
                    "query_only_contributing_runs": sum(
                        row["run_count"] for row in query_rows),
                    "approved_observations_by_timing_scope":
                        _approved_rows_by_timing_scope(rows),
                } if timing_scoped else {}),
            }),
        },
        "approved_execution_classes": [
            {"class": "exact_requested_path_bioseq",
             "approved_rows": exact_count, "valid_runs": exact_runs,
             "user_preserved_subset_rows": preserved},
            {"class": "documented_scalar_minimizer_fallback",
             "approved_rows": fallback_count, "valid_runs": fallback_runs,
             "requested_simd_performance_eligible": False},
            {"class": "external_minimap2_baseline",
             "approved_rows": external_count, "valid_runs": external_runs},
        ],
        "requested_path_performance_eligible": {
            "approved_rows": eligible_count, "valid_runs": eligible_runs},
        "accepted_marginals": accepted_marginals,
    }


_APPROVED_FAMILY_NONAPPROVED_CELLS = {
    ("corrected_user_intended", "bounded_simd_dp_block_mpf_just_index"): 1,
    ("corrected_user_intended", "vulkan_seed_vulkan_chain_block_mpf_just_index"): 1,
    ("corrected_user_intended", "bounded_simd_dp_block_mpf_no_index"): 1,
    ("corrected_user_intended", "vulkan_seed_vulkan_chain_block_mpf_no_index"): 1,
    ("corrected_user_intended", "minimap2_just_index"): 0,
    ("corrected_user_intended", "minimap2_no_index"): 0,
    ("historical_rmq_mislabeled", "bounded_rmq_block_mpf_just_index"): 0,
    ("historical_rmq_mislabeled", "vulkan_seed_vulkan_chain_block_mpf_just_index"): 1,
    ("historical_rmq_mislabeled", "bounded_rmq_block_mpf_no_index"): 1,
    ("historical_rmq_mislabeled", "vulkan_seed_vulkan_chain_block_mpf_no_index"): 1,
    ("historical_rmq_mislabeled", "minimap2_just_index"): 0,
    ("historical_rmq_mislabeled", "minimap2_no_index"): 0,
}


def _approved_family_matches(row: dict[str, Any], family: dict[str, Any]) -> bool:
    return (row["mode"] == family.get("mode")
            and row["pipeline"] == family.get("pipeline")
            and row["index_type"] == family.get("index_type")
            and row["requested_algorithms"] == family.get("requested_algorithms"))


def _regenerate_approved_priority_families(
        value: Any, rows: list[dict[str, Any]]) -> dict[str, Any]:
    result = copy.deepcopy(_require_dict(value, "high_priority_families"))
    timing_scoped = bool(rows) and all("timing_scope" in row for row in rows)
    _require_exact_keys(result, {
        "classification", "projection_description", "vector_metric",
        "family_order_is_normative", "corrected_user_intended",
        "historical_rmq_mislabeled",
    }, "high_priority_families")
    if (result.get("classification") != "derived_non_authoritative"
            or result.get("vector_metric") != "valid_runs"
            or result.get("family_order_is_normative") is not False):
        raise ContractError("high_priority_families metadata changed")
    for view_name in ("corrected_user_intended", "historical_rmq_mislabeled"):
        view = _require_dict(result.get(view_name), f"high_priority_families.{view_name}")
        expected_view_keys = {
            "valid_run_vector", "recorded_run_vector_for_exact_family_definitions",
            "families", "reconciliation",
        }
        if timing_scoped:
            expected_view_keys.add("valid_run_vectors_by_timing_scope")
        if view_name == "corrected_user_intended":
            expected_view_keys.add("simd_dp_key")
            if view.get("simd_dp_key") != "simd_minimap2":
                raise ContractError("corrected family SIMD-DP key changed")
        else:
            expected_view_keys.update({
                "historical_shorthand_that_was_wrong", "actual_chaining_key",
                "actual_human_label", "invariant",
            })
            if (view.get("historical_shorthand_that_was_wrong") != "SIMD-DP"
                    or view.get("actual_chaining_key") != "range_minimum_query"):
                raise ContractError("historical RMQ family identity changed")
        _require_exact_keys(view, expected_view_keys,
                            f"high_priority_families.{view_name}")
        families = _require_list(view.get("families"), f"{view_name}.families")
        if len(families) != 6:
            raise ContractError(f"{view_name} must retain six family positions")
        family_ids = [item.get("id") for item in families if isinstance(item, dict)]
        expected_family_ids = [
            family_id for (owner, family_id) in _APPROVED_FAMILY_NONAPPROVED_CELLS
            if owner == view_name
        ]
        if family_ids != expected_family_ids:
            raise ContractError(f"{view_name} family identity/order changed")
        seen: set[str] = set()
        valid_vector: list[int] = []
        recorded_vector: list[int] = []
        scope_vectors = {scope: [] for scope in APPROVED_TUPLES_TIMING_SCOPES}
        for index, raw_family in enumerate(families):
            family = _require_dict(raw_family, f"{view_name}.families[{index}]")
            expected_family_keys = {
                "id", "label", "mode", "pipeline", "index_type",
                "requested_algorithms", "materialized_cells", "recorded_runs",
                "approved_cells", "valid_runs",
            }
            if timing_scoped:
                expected_family_keys.add("approved_observations_by_timing_scope")
            _require_exact_keys(
                family, expected_family_keys, f"{view_name}.families[{index}]")
            family_id = _require_string(family.get("id"), f"{view_name} family id")
            if family_id in seen or (view_name, family_id) not in (
                    _APPROVED_FAMILY_NONAPPROVED_CELLS):
                raise ContractError(f"{view_name} family identity/order changed")
            seen.add(family_id)
            algorithms = _require_dict(
                family.get("requested_algorithms"), f"{view_name} requested algorithms")
            _approved_exact_order(
                algorithms, APPROVED_TUPLES_ALGORITHM_FIELDS,
                f"{view_name} requested algorithms")
            selected = [row for row in rows if _approved_family_matches(row, family)]
            approved_cells = len({row["stable_key"] for row in selected})
            valid_runs = sum(row["run_count"] for row in selected)
            historical = [row for row in _approved_historical_nonvalid_rows()
                          if _approved_family_matches(row, family)]
            approved_keys = {row["stable_key"] for row in selected}
            nonapproved = sum(row["stable_key"] not in approved_keys for row in historical)
            family["materialized_cells"] = approved_cells + nonapproved
            family["recorded_runs"] = valid_runs + sum(row["run_count"] for row in historical)
            family["approved_cells"] = approved_cells
            family["valid_runs"] = valid_runs
            if timing_scoped:
                family["approved_observations_by_timing_scope"] = [{
                    "timing_scope": scope,
                    "approved_rows": sum(
                        row["timing_scope"] == scope for row in selected),
                    "contributing_runs": sum(
                        row["run_count"] for row in selected
                        if row["timing_scope"] == scope),
                } for scope in APPROVED_TUPLES_TIMING_SCOPES]
                for scope in APPROVED_TUPLES_TIMING_SCOPES:
                    scope_vectors[scope].append(sum(
                        row["run_count"] for row in selected
                        if row["timing_scope"] == scope))
            valid_vector.append(valid_runs)
            recorded_vector.append(family["recorded_runs"])
        view["valid_run_vector"] = valid_vector
        view["recorded_run_vector_for_exact_family_definitions"] = recorded_vector
        if timing_scoped:
            view["valid_run_vectors_by_timing_scope"] = [{
                "timing_scope": scope,
                "valid_run_vector": scope_vectors[scope],
            } for scope in APPROVED_TUPLES_TIMING_SCOPES]
        rendered = "/".join(str(item) for item in valid_vector)
        if view_name == "corrected_user_intended":
            if valid_vector == [10, 0, 5, 0, 40, 40]:
                view["reconciliation"] = (
                    "The vector is 10/0/5/0/40/40. The third entry is the approved "
                    "2 Gbp dna4 k15/w10 bounded_nthash_simd + simd_minimap2 + "
                    "block_aligner MPF no-index row.")
            else:
                view["reconciliation"] = (
                    f"The vector is {rendered}. The third entry is the approved "
                    "bounded_nthash_simd + simd_minimap2 + block_aligner MPF "
                    "no-index family projection.")
        else:
            if valid_vector == [10, 0, 0, 0, 40, 40]:
                view["reconciliation"] = (
                    "The historical vector is 10/0/0/0/40/40. Its third family has "
                    "five recorded attempts but zero valid runs because the one "
                    "materialized cell is invalid and has no P50.")
            else:
                view["reconciliation"] = (
                    f"The historical vector is {rendered}. Its third family retains "
                    "the frozen nonvalid-cell accounting and never relabels RMQ as SIMD-DP.")
    return result


def _approved_invariant_checks(
        rows: list[dict[str, Any]], corrected_vector: list[int],
        historical_vector: list[int]) -> list[str]:
    timing_scoped = bool(rows) and all("timing_scope" in row for row in rows)
    logical_count = len({row["stable_key"] for row in rows})
    query_count = sum(
        row.get("timing_scope") == APPROVED_TUPLES_QUERY_TIMING_SCOPE
        for row in rows)
    run_total = sum(row["run_count"] for row in rows)
    fallback_count = sum(row["execution_conformance"] ==
                         "documented_scalar_minimizer_fallback" for row in rows)
    comment_count = sum(row["comments"] is not None for row in rows)
    preserved_count = sum(row["execution_conformance"] ==
                          "exact_requested_path_user_preserved" for row in rows)
    checks = [
        "approved_results contains accepted rows only.",
        ("Every row carries campaign_id bioseq-e2e-algorithm-matrix-local-v1, "
         "stable_key_schema bioseq-e2e-cell-v2, and the exact canonical stable_key "
         "recomputed through stable_key_contract."),
        (("Canonical stable keys remain unchanged logical identities; observation "
          "pairs (stable_key, timing_scope) are unique and approved_results is in "
          "ascending pair order.") if timing_scoped else
         ("Canonical stable keys are unique full identities and approved_results is "
          "in ascending stable_key order.")),
        "Every run_count is exactly five.",
        ("Every metrics.unit is ns and every p50_a and p50_b is a non-null positive "
         "integer with p50_b <= p50_a."),
        (f"Every P50, run_count, requested identity, and observed identity retains "
         f"the approved value. The {comment_count} non-null comments are publication "
         f"annotations; {fallback_count} qualify scalar fallback and {preserved_count} "
         "qualify legacy user preservation, including zero ksw2_batched activity."),
        ("Every exact_baseline_match_count_equal is true; no returned-count value "
         "is stored."),
        "Every row has prefilter=false.",
        "No approved row uses an excluded runtime-only base-alignment algorithm.",
        ("A documented_scalar_minimizer_fallback row requests "
         "simd_minimap2_reference, observes scalar, exposes the scalar effective "
         "path, and has requested_path=false."),
        "No other Bioseq row may have a requested/observed algorithm mismatch.",
        ("External minimap2 rows have null Bioseq requested and observed algorithm "
         "fields and index_type=minimap2_internal."),
        ((f"The current snapshot has exactly {len(rows)} timing observations, "
          f"{logical_count} logical cells, {query_count} query-only observations, "
          f"and {run_total} actual contributing runs, including exactly "
          f"{fallback_count} documented scalar-minimizer fallback observations.")
         if timing_scoped else
         (f"The current snapshot has exactly {len(rows)} approved rows and "
          f"{run_total} valid runs, including exactly {fallback_count} documented "
          "scalar-minimizer fallback rows.")),
        ("Approved plus currently unapproved historical rejected and invalid plus "
         "missing cell counts reconcile to 1296. Five planned valid runs per cell "
         "reconcile to 6480; historical failed attempts are additional and are "
         "never counted as approved runs or as additional distinct cells."),
        ("The corrected user-intended valid-run vector is "
         f"{'/'.join(map(str, corrected_vector))} and the historical RMQ-mislabeled "
         f"vector is {'/'.join(map(str, historical_vector))}."),
        (("No approved row stores receipt identity, proof, raw-repeat, path, URI, or "
          "log-location fields; historical_outer_e2e_v1 remains exactly the pinned "
          "66-row v2 projection and new observations use only "
          "query_only_framework_iteration_v1.") if timing_scoped else
         ("No approved row stores receipt identity, proof, raw-repeat, path, URI, or "
          "log-location fields; the immutable legacy_not_sealed scope remains limited "
          "to its pinned 66-row projection.")),
    ]
    return checks


def _approved_p50_audit_scope(rows: list[dict[str, Any]]) -> str:
    if rows and all("timing_scope" in row for row in rows):
        return (
            f"All {len(rows)} timing observations and "
            f"{sum(row['run_count'] for row in rows)} actual contributing runs were "
            "checked without combining P50 inputs across timing_scope values.")
    return (
        f"All {len(rows)} approved rows and {sum(row['run_count'] for row in rows)} "
        "contributing runs were checked before publication or sealed append.")


def _approved_fallback_rule(rows: list[dict[str, Any]]) -> str:
    fallback_count = sum(row["execution_conformance"] ==
                         "documented_scalar_minimizer_fallback" for row in rows)
    return (
        f"The {fallback_count} simd_minimap2_reference requests observed as scalar "
        "are approved only as scalar-fallback observations. Their requested_path "
        "comparison eligibility is false even though downstream validity and exact "
        "baseline equality passed.")


def _regenerate_approved_tuple_projections(
        document: dict[str, Any], rows: list[dict[str, Any]]) -> None:
    timing_scoped = document.get("version") == APPROVED_TUPLES_VERSION
    if timing_scoped:
        rows = sorted(rows, key=_approved_observation_sort_key)
    else:
        rows = sorted(rows, key=lambda row: row["stable_key"])
    document["approved_results"] = rows
    projection = _approved_projection_sha256(rows)
    provenance = _require_dict(
        document.get("projection_provenance"), "projection_provenance")
    provenance["approved_results_projection_sha256"] = projection
    p50_audit = _require_dict(provenance.get("current_p50_audit"),
                              "projection_provenance.current_p50_audit")
    p50_audit["scope"] = _approved_p50_audit_scope(rows)
    if timing_scoped:
        p50_audit["median_partition_rule"] = (
            "Each published row P50 is computed only from repeats carrying that "
            "row's timing_scope; observations from different scopes are never pooled.")
        p50_audit["by_timing_scope"] = _approved_rows_by_timing_scope(rows)
    authority = _require_dict(document.get("authority"), "authority")
    total_cells = _require_dict(document.get("matrix_scope"),
                                "matrix_scope")["total_expected_cells"]
    query_only_cells = len({
        row["stable_key"] for row in rows
        if row.get("timing_scope") == APPROVED_TUPLES_QUERY_TIMING_SCOPE})
    complete = (
        query_only_cells == total_cells if timing_scoped else len(rows) == total_cells)
    authority["full_matrix_complete"] = complete
    document["status"] = (
        "approved_complete_matrix" if complete
        else "approved_partial_matrix")
    run_counts = _require_dict(document.get("run_count_expectations"),
                               "run_count_expectations")
    run_counts["current_approved_rows_all_have_exactly_five"] = all(
        row["run_count"] == MIN_ACCEPTED_REPEATS for row in rows)
    run_counts["current_approved_run_total"] = sum(row["run_count"] for row in rows)
    if timing_scoped:
        run_counts["current_logical_tuple_count"] = len({
            row["stable_key"] for row in rows})
        run_counts["current_timing_observation_count"] = len(rows)
        run_counts["current_query_only_observation_count"] = query_only_cells
        run_counts["current_query_only_run_total"] = sum(
            row["run_count"] for row in rows
            if row["timing_scope"] == APPROVED_TUPLES_QUERY_TIMING_SCOPE)
    corrected = _approved_corrected_summaries(document, rows)
    document["corrected_summaries"] = corrected
    excluded = _require_dict(document.get("excluded_and_nonvalid_summary"),
                             "excluded_and_nonvalid_summary")
    excluded["projection_description"] = (
        "Rejected and invalid summaries describe the frozen historical attempts, "
        "including cells that may now be approved. Missing describes current "
        "unrecorded cell coverage. corrected_summaries separates those historical "
        "attempts from logical cell coverage and separately reports query-only "
        "campaign progress and actual observation runs."
        if timing_scoped else
        "Rejected and invalid summaries describe the frozen historical attempts, "
        "including cells that may now be approved. Missing describes current "
        "unrecorded cell coverage. corrected_summaries separates those historical "
        "attempts from the disjoint current cell partition and valid-run progress.")
    excluded_missing = _require_dict(excluded.get("missing"), "excluded missing")
    excluded_missing["cells"] = corrected["overall"]["missing_cells"]
    excluded_missing["planned_runs_not_recorded"] = corrected["overall"][
        "unrecorded_planned_runs"]
    priority = _regenerate_approved_priority_families(
        document.get("high_priority_families"), rows)
    document["high_priority_families"] = priority
    corrected_vector = priority["corrected_user_intended"]["valid_run_vector"]
    historical_vector = priority["historical_rmq_mislabeled"]["valid_run_vector"]
    invariants = _require_dict(document.get("invariants"), "invariants")
    invariants["checks"] = _approved_invariant_checks(
        rows, corrected_vector, historical_vector)
    validity = _require_dict(document.get("validity_rules"), "validity_rules")
    validity["documented_fallback_exception"] = _approved_fallback_rule(rows)
    if timing_scoped:
        _validate_approved_historical_v2_preservation(document, rows)


def _migrate_approved_tuple_authority_v3(
        document: dict[str, Any], source_sha256: str) -> dict[str, Any]:
    """Add observation scope without changing any frozen v2 row fact."""
    if document.get("version") == APPROVED_TUPLES_VERSION:
        return copy.deepcopy(document)
    if document.get("version") != APPROVED_TUPLES_LEGACY_VERSION:
        raise ContractError("approved tuple authority cannot migrate this version")
    legacy_rows = _require_list(document.get("approved_results"), "approved_results")
    if (source_sha256 != APPROVED_TUPLES_LEGACY_SHA256
            or len(legacy_rows) != 66
            or _approved_projection_sha256(legacy_rows)
                != APPROVED_TUPLES_LEGACY_PROJECTION_SHA256):
        raise ApprovedTupleProofError(
            "v2-to-v3 migration requires the exact frozen 66-row authority")

    migrated = copy.deepcopy(document)
    migrated["version"] = APPROVED_TUPLES_VERSION
    migrated_rows: list[dict[str, Any]] = []
    for legacy_row in legacy_rows:
        row = dict(legacy_row)
        row["timing_scope"] = APPROVED_TUPLES_HISTORICAL_TIMING_SCOPE
        migrated_rows.append(_approved_row_in_fixed_order(row))

    provenance = _require_dict(
        migrated.get("projection_provenance"), "projection_provenance")
    provenance["generator_version"] = APPROVED_TUPLES_VERSION
    provenance["historical_v2_preservation"] = {
        "source_authority_sha256": APPROVED_TUPLES_LEGACY_SHA256,
        "approved_results_projection_sha256":
            APPROVED_TUPLES_LEGACY_PROJECTION_SHA256,
        "observation_count": 66,
        "timing_scope": APPROVED_TUPLES_HISTORICAL_TIMING_SCOPE,
        "projection_rule": (
            "strip timing_scope, restore v2 row order, sort by stable_key"),
    }
    projection_encoding = provenance.get("approved_results_projection_encoding")
    if isinstance(projection_encoding, str):
        provenance["approved_results_projection_encoding"] = (
            projection_encoding.replace(
                "rows are sorted by stable_key",
                "rows are sorted by (stable_key, timing_scope)"))
    p50_audit = _require_dict(
        provenance.get("current_p50_audit"),
        "projection_provenance.current_p50_audit")
    p50_audit["median_partition_rule"] = "pending deterministic regeneration"
    p50_audit["by_timing_scope"] = [{
        "timing_scope": scope, "approved_rows": 0, "contributing_runs": 0,
    } for scope in APPROVED_TUPLES_TIMING_SCOPES]

    stable_contract = _require_dict(
        migrated.get("stable_key_contract"), "stable_key_contract")
    stable_contract["observation_identity_rule"] = (
        "stable_key remains the logical tuple identity; the publication observation "
        "identity is the ordered pair (stable_key, timing_scope).")
    if "validation_rule" in stable_contract:
        stable_contract["validation_rule"] = (
            "Decode each stable_key and compare its twelve values with the row field "
            "projection. Stable keys may repeat only across timing scopes; every "
            "(stable_key, timing_scope) observation pair is unique.")
    terminology = _require_dict(migrated.get("terminology"), "terminology")
    field_guide = terminology.get("approved_row_field_guide")
    if isinstance(field_guide, dict):
        field_guide["timing_scope"] = (
            "Non-metric classification of the timing boundary used by this row.")
    metric_definitions = _require_dict(
        migrated.get("metric_definitions"), "metric_definitions")
    metric_definitions["timing_scope"] = {
        "classification": "non_metric_observation_scope",
        "allowed_values": list(APPROVED_TUPLES_TIMING_SCOPES),
        "historical_outer_e2e_v1": (
            "Frozen historical outer E2E interval; retained without reinterpretation."),
        "query_only_framework_iteration_v1": (
            "Framework iteration interval containing only the measured mapping operation."),
        "p50_partition_rule": (
            "Compute every row P50 solely within its timing_scope; never pool scopes."),
    }
    metric_a = metric_definitions.get("a")
    if isinstance(metric_a, dict) and "per_repeat_definition" in metric_a:
        metric_a["per_repeat_definition"] = (
            "The exact interval classified by the row timing_scope: frozen outer E2E "
            "for historical_outer_e2e_v1 or the framework measured-iteration window "
            "for query_only_framework_iteration_v1.")
    clock_semantics = metric_definitions.get("clock_semantics")
    if isinstance(clock_semantics, dict) and "framework_outer" in clock_semantics:
        clock_semantics["framework_outer"] = (
            "Monotonic duration for the row's classified framework interval; the two "
            "timing scopes are retained and aggregated separately.")

    run_counts = _require_dict(
        migrated.get("run_count_expectations"), "run_count_expectations")
    run_counts.update({
        "current_logical_tuple_count": 0,
        "current_timing_observation_count": 0,
        "current_query_only_observation_count": 0,
        "current_query_only_run_total": 0,
    })
    priority = _require_dict(
        migrated.get("high_priority_families"), "high_priority_families")
    for view_name in ("corrected_user_intended", "historical_rmq_mislabeled"):
        view = _require_dict(priority.get(view_name), f"high_priority_families.{view_name}")
        view["valid_run_vectors_by_timing_scope"] = []
        for family in _require_list(view.get("families"), f"{view_name}.families"):
            _require_dict(family, f"{view_name} family")[
                "approved_observations_by_timing_scope"] = []

    ordering = _require_dict(
        migrated.get("deterministic_ordering"), "deterministic_ordering")
    ordering["approved_results_sort_key"] = (
        "Ascending lexicographic (stable_key, timing_scope) observation identity; "
        "stable_key itself remains the unchanged logical tuple identity.")
    protocol = _require_dict(
        migrated.get("append_update_protocol"), "append_update_protocol")
    writer = _require_dict(protocol.get("writer"), "append_update_protocol.writer")
    writer["version"] = 2
    serializer = _require_dict(
        protocol.get("canonical_serializer"),
        "append_update_protocol.canonical_serializer")
    serializer["version"] = APPROVED_TUPLES_VERSION
    if "sequence_order" in serializer:
        serializer["sequence_order"] = (
            "approved_results sorted by (stable_key, timing_scope); all other "
            "sequences retain generated semantic order.")
    digest_encoding = serializer.get("approved_results_digest_encoding")
    if isinstance(digest_encoding, str):
        serializer["approved_results_digest_encoding"] = digest_encoding.replace(
            "rows are sorted by stable_key",
            "rows are sorted by (stable_key, timing_scope)")
    sealed = _require_dict(
        protocol.get("sealed_append_candidate"),
        "append_update_protocol.sealed_append_candidate")
    sealed["version"] = APPROVED_TUPLES_CANDIDATE_VERSION
    if "required_identity" in sealed:
        sealed["required_identity"] = (
            "campaign_id, stable_key_schema, canonical stable_key, timing_scope, and "
            "every stable-key component must agree exactly.")
    if "candidate_shape" in sealed:
        sealed["candidate_shape"] = (
            "One nonempty sequence of complete query-only observation rows, sorted "
            "by (stable_key, timing_scope); an unscoped v1 candidate is forbidden.")
    if "approved_row_schema" in sealed:
        sealed["approved_row_schema"] = (
            "Exactly " + ", ".join(APPROVED_TUPLES_ROW_FIELDS)
            + ", with the nested fields described by approved_row_field_guide.")
    verification = sealed.get("verification_manifest")
    if isinstance(verification, dict):
        verification["version"] = APPROVED_TUPLES_MANIFEST_VERSION
        if "row_binding" in verification:
            verification["row_binding"] = (
                "Each candidate (stable_key, timing_scope) observation has exactly "
                "six internal identity/version bindings; bindings are never published.")
    duplicate_policy = protocol.get("duplicate_policy")
    if isinstance(duplicate_policy, dict):
        duplicate_policy["full_identity"] = (
            "stable_key is the logical cell tuple identity; timing_scope classifies "
            "a distinct timing observation.")
        duplicate_policy["rule"] = (
            "Reject a candidate only when its exact (stable_key, timing_scope) "
            "observation identity already exists.")
        if "collision_rule" in duplicate_policy:
            duplicate_policy["collision_rule"] = (
                "Reject when a stable_key fails to decode to its logical fields or "
                "when an observation pair is repeated.")
    receipt_semantics = protocol.get("current_rows_receipt_semantics")
    if isinstance(receipt_semantics, dict):
        receipt_semantics["timing_scope"] = (
            APPROVED_TUPLES_HISTORICAL_TIMING_SCOPE)
        receipt_semantics["source_authority_sha256"] = (
            APPROVED_TUPLES_LEGACY_SHA256)
    steps = protocol.get("steps")
    if isinstance(steps, list):
        protocol["steps"] = [
            ("Construct the canonical bioseq-e2e-cell-v2 stable_key, retain it as "
             "the logical identity, and reject duplicate (stable_key, timing_scope) "
             "observation pairs before adding anything.")
            if isinstance(step, str) and step.startswith(
                "Construct the canonical bioseq-e2e-cell-v2 stable_key")
            else step
            for step in steps]
    if "remeasurement_policy" in protocol:
        protocol["remeasurement_policy"] = (
            "A query-only observation may coexist with its frozen historical row; "
            "the exact observation pair can never be silently replaced or replayed.")

    _regenerate_approved_tuple_projections(migrated, migrated_rows)
    return migrated


def _validate_approved_tuple_authority_document(
        document: dict[str, Any], raw: bytes | None = None) -> list[dict[str, Any]]:
    if _is_remote_approved_authority(document):
        return _validate_remote_approved_tuple_authority_document(document, raw)
    _approved_exact_order(
        document, APPROVED_TUPLES_TOP_LEVEL_FIELDS, "approved tuple authority")
    authority_version = document.get("version")
    timing_scoped = authority_version == APPROVED_TUPLES_VERSION
    if (document.get("schema") != APPROVED_TUPLES_SCHEMA
            or authority_version not in {
                APPROVED_TUPLES_LEGACY_VERSION, APPROVED_TUPLES_VERSION}
            or document.get("status") not in {
                "approved_partial_matrix", "approved_complete_matrix"}):
        raise ContractError("approved tuple authority schema/version/status changed")
    _require_string(document.get("purpose"), "approved tuple authority purpose")
    authority = _require_dict(document.get("authority"), "authority")
    _require_exact_keys(authority, {
        "single_source_of_truth_for_approved_results", "normative_tuple_dataset",
        "all_other_sections", "contains_only_approved_rows",
        "approved_snapshot_complete", "full_matrix_complete",
        "external_lookup_required_to_interpret",
        "no_external_path_or_reference_dependency",
        "sealed_verification_manifest_required_for_append",
        "raw_repeat_timings_included", "standalone_forensic_evidence_archive",
        "frozen_evidence_archive_is_publication_authority",
        "identity_contract_rule", "enum_authority_rule",
    }, "approved tuple authority.authority")
    if (authority.get("single_source_of_truth_for_approved_results") is not True
            or authority.get("normative_tuple_dataset") != "approved_results"
            or authority.get("all_other_sections") != "derived_non_authoritative"
            or authority.get("contains_only_approved_rows") is not True
            or authority.get("approved_snapshot_complete") is not True
            or authority.get("external_lookup_required_to_interpret") is not False
            or authority.get("no_external_path_or_reference_dependency") is not True
            or type(authority.get("sealed_verification_manifest_required_for_append")) is not bool
            or authority.get("raw_repeat_timings_included") is not False):
        raise ContractError("approved tuple authority ownership flags changed")
    for section_name in APPROVED_TUPLES_TOP_LEVEL_FIELDS[5:-1]:
        section = _require_dict(document.get(section_name), section_name)
        if section.get("classification") != "derived_non_authoritative":
            raise ContractError(f"{section_name} is not marked derived_non_authoritative")
    campaign = _require_dict(document.get("campaign_identity"), "campaign_identity")
    campaign_id = _require_string(campaign.get("campaign_id"), "campaign_identity.campaign_id")
    _require_hex(campaign.get("campaign_digest"), "campaign_identity.campaign_digest", 64)
    if (campaign_id != Q1024_CAMPAIGN_ID
            or campaign.get("campaign_digest") != Q1024_CAMPAIGN_DIGEST):
        raise ContractError("approved tuple campaign identity changed")
    stable_contract = _require_dict(document.get("stable_key_contract"),
                                    "stable_key_contract")
    if (stable_contract.get("stable_key_schema") != "bioseq-e2e-cell-v2"
            or stable_contract.get("ordered_aggregator_fields")
                != list(STABLE_KEY_FIELDS)):
        raise ContractError("stable-key contract differs from the aggregator")
    if (timing_scoped and stable_contract.get("observation_identity_rule") != (
            "stable_key remains the logical tuple identity; the publication observation "
            "identity is the ordered pair (stable_key, timing_scope).")):
        raise ContractError("observation identity contract changed")
    append_protocol = _require_dict(document.get("append_update_protocol"),
                                    "append_update_protocol")
    append_writer = _require_dict(append_protocol.get("writer"),
                                  "append_update_protocol.writer")
    append_lock = _require_dict(append_writer.get("lock"),
                                "append_update_protocol.writer.lock")
    serializer = _require_dict(append_protocol.get("canonical_serializer"),
                               "append_update_protocol.canonical_serializer")
    sealed_candidate = _require_dict(append_protocol.get("sealed_append_candidate"),
                                     "append_update_protocol.sealed_append_candidate")
    protocol_manifest = _require_dict(
        sealed_candidate.get("verification_manifest"),
        "append_update_protocol.sealed_append_candidate.verification_manifest")
    expected_writer_version = 2 if timing_scoped else 1
    expected_serializer_version = (
        APPROVED_TUPLES_VERSION if timing_scoped
        else APPROVED_TUPLES_LEGACY_VERSION)
    expected_candidate_version = (
        APPROVED_TUPLES_CANDIDATE_VERSION if timing_scoped else 1)
    expected_manifest_version = (
        APPROVED_TUPLES_MANIFEST_VERSION if timing_scoped else 1)
    if (append_protocol.get("mode") != "additive_audited_rows_only"
            or append_writer.get("name") != "bioseq-approved-results-aggregator"
            or append_writer.get("version") != expected_writer_version
            or append_lock.get("name") != "bioseq-approved-results-publication"
            or append_lock.get("version") != 1
            or serializer.get("name") != "bioseq-approved-results-yaml"
            or serializer.get("version") != expected_serializer_version
            or sealed_candidate.get("name")
                != APPROVED_TUPLES_CANDIDATE_SCHEMA
            or sealed_candidate.get("version") != expected_candidate_version
            or protocol_manifest.get("name") != APPROVED_TUPLES_MANIFEST_SCHEMA
            or protocol_manifest.get("version") != expected_manifest_version):
        raise ContractError("approved tuple append protocol projection changed")
    matrix = _require_dict(document.get("matrix_scope"), "matrix_scope")
    if (matrix.get("total_expected_cells") != EXPECTED_RESULT_CELLS
            or matrix.get("repetitions_per_cell") != MIN_ACCEPTED_REPEATS
            or matrix.get("total_expected_runs")
                != EXPECTED_RESULT_CELLS * MIN_ACCEPTED_REPEATS
            or matrix.get("query_count") != 1024
            or matrix.get("prefilter") is not False
            or matrix.get("cigar_projection") is not True
            or matrix.get("mapq_projection") is not True):
        raise ContractError("approved tuple matrix scope changed")
    raw_rows = _require_list(document.get("approved_results"), "approved_results")
    if not raw_rows:
        raise ContractError("approved_results may not be empty")
    rows = [
        _validate_approved_tuple_row(
            raw_row, campaign_id, matrix, f"approved_results[{index}]",
            allow_legacy_preservation=True, require_field_order=True,
            authority_version=authority_version)
        for index, raw_row in enumerate(raw_rows)
    ]
    identities = ([_approved_observation_identity(row) for row in rows]
                  if timing_scoped else [row["stable_key"] for row in rows])
    if identities != sorted(identities):
        raise ApprovedTupleDuplicateError(
            "approved_results is not sorted by observation identity"
            if timing_scoped else
            "approved_results is not sorted by stable_key")
    if len(identities) != len(set(identities)):
        raise ApprovedTupleDuplicateError(
            "approved_results contains a duplicate observation identity"
            if timing_scoped else
            "approved_results contains a duplicate stable_key")
    projection = _approved_projection_sha256(rows)
    provenance = _require_dict(document.get("projection_provenance"),
                               "projection_provenance")
    if provenance.get("approved_results_projection_sha256") != projection:
        raise ContractError("approved_results projection seal does not recompute")
    p50_audit = _require_dict(provenance.get("current_p50_audit"),
                              "projection_provenance.current_p50_audit")
    expected_p50_fields = {
        "independent_recomputation_completed", "scope",
        "raw_repeat_observations_embedded",
        "interpretation_or_comparison_dependency",
    }
    if timing_scoped:
        expected_p50_fields.update({"median_partition_rule", "by_timing_scope"})
    _require_exact_keys(
        p50_audit, expected_p50_fields,
        "projection_provenance.current_p50_audit")
    legacy_bytes = raw is not None and _sha256(raw) == APPROVED_TUPLES_LEGACY_SHA256
    if (provenance.get("generator") != "bioseq-approved-results-projection"
            or provenance.get("generator_version")
                != (APPROVED_TUPLES_VERSION if timing_scoped else 2)
            or p50_audit.get("independent_recomputation_completed") is not True
            or p50_audit.get("raw_repeat_observations_embedded") is not False
            or p50_audit.get("interpretation_or_comparison_dependency") is not False
            or (not legacy_bytes and p50_audit.get("scope")
                != _approved_p50_audit_scope(rows))):
        raise ContractError("approved P50 audit projection is stale")
    if timing_scoped:
        if (p50_audit.get("median_partition_rule") != (
                "Each published row P50 is computed only from repeats carrying that "
                "row's timing_scope; observations from different scopes are never pooled.")
                or p50_audit.get("by_timing_scope")
                    != _approved_rows_by_timing_scope(rows)):
            raise ContractError("approved timing-scope P50 partition is stale")
        _validate_approved_historical_v2_preservation(document, rows)
        metric_definitions = _require_dict(
            document.get("metric_definitions"), "metric_definitions")
        timing_definition = _require_dict(
            metric_definitions.get("timing_scope"),
            "metric_definitions.timing_scope")
        if (timing_definition.get("classification")
                != "non_metric_observation_scope"
                or timing_definition.get("allowed_values")
                    != list(APPROVED_TUPLES_TIMING_SCOPES)):
            raise ContractError("timing_scope classification metadata changed")
    validity = _require_dict(document.get("validity_rules"), "validity_rules")
    if (not legacy_bytes and validity.get("documented_fallback_exception")
            != _approved_fallback_rule(rows)):
        raise ContractError("documented fallback projection is stale")
    run_counts = _require_dict(document.get("run_count_expectations"),
                               "run_count_expectations")
    expected_run_count_fields = {
        "classification", "projection_description", "minimum_for_approval",
        "campaign_target_per_cell",
        "current_approved_rows_all_have_exactly_five",
        "current_approved_run_total",
        "recorded_attempts_are_not_synonymous_with_valid_runs",
    }
    if timing_scoped:
        expected_run_count_fields.update({
            "current_logical_tuple_count", "current_timing_observation_count",
            "current_query_only_observation_count", "current_query_only_run_total",
        })
    _require_exact_keys(
        run_counts, expected_run_count_fields, "run_count_expectations")
    if (run_counts.get("current_approved_rows_all_have_exactly_five") is not True
            or run_counts.get("current_approved_run_total")
                != sum(row["run_count"] for row in rows)
            or run_counts.get("minimum_for_approval") != MIN_ACCEPTED_REPEATS
            or run_counts.get("campaign_target_per_cell") != MIN_ACCEPTED_REPEATS
            or run_counts.get(
                "recorded_attempts_are_not_synonymous_with_valid_runs") is not True):
        raise ContractError("approved run-count projection does not recompute")
    if timing_scoped:
        query_rows = [
            row for row in rows if row["timing_scope"]
            == APPROVED_TUPLES_QUERY_TIMING_SCOPE]
        if (run_counts.get("current_logical_tuple_count")
                != len({row["stable_key"] for row in rows})
                or run_counts.get("current_timing_observation_count") != len(rows)
                or run_counts.get("current_query_only_observation_count")
                    != len(query_rows)
                or run_counts.get("current_query_only_run_total")
                    != sum(row["run_count"] for row in query_rows)):
            raise ContractError("approved timing-scope run counts do not recompute")
    expected_corrected = _approved_corrected_summaries(
        document, rows, legacy_layout=legacy_bytes)
    if document.get("corrected_summaries") != expected_corrected:
        raise ContractError("corrected_summaries does not recompute exactly")
    excluded = _require_dict(document.get("excluded_and_nonvalid_summary"),
                             "excluded_and_nonvalid_summary")
    _require_exact_keys(excluded, {
        "classification", "projection_description",
        "approved_results_contains_nonvalid_rows", "rejected", "invalid",
        "missing", "calibration_partial_and_timeout_diagnostics_approved",
    }, "excluded_and_nonvalid_summary")
    rejected = _require_dict(excluded.get("rejected"), "excluded rejected")
    invalid = _require_dict(excluded.get("invalid"), "excluded invalid")
    missing = _require_dict(excluded.get("missing"), "excluded missing")
    _require_exact_keys(rejected, {
        "cells", "recorded_attempts", "p50_published",
        "reason_groups_by_cell_count",
    }, "excluded rejected")
    _require_exact_keys(invalid, {
        "cells", "recorded_attempts", "p50_published", "summary",
    }, "excluded invalid")
    _require_exact_keys(missing, {
        "cells", "planned_runs_not_recorded", "p50_published",
    }, "excluded missing")
    if (missing.get("cells") != expected_corrected["overall"]["missing_cells"]
            or missing.get("planned_runs_not_recorded")
                != expected_corrected["overall"]["unrecorded_planned_runs"]
            or rejected.get("p50_published") is not False
            or invalid.get("p50_published") is not False
            or missing.get("p50_published") is not False
            or excluded.get("approved_results_contains_nonvalid_rows") is not False
            or excluded.get(
                "calibration_partial_and_timeout_diagnostics_approved") is not False):
        raise ContractError("excluded/missing projection does not reconcile")
    expected_priority = _regenerate_approved_priority_families(
        document.get("high_priority_families"), rows)
    if document.get("high_priority_families") != expected_priority:
        raise ContractError("high-priority vectors do not recompute exactly")
    invariants = _require_dict(document.get("invariants"), "invariants")
    expected_checks = _approved_invariant_checks(
        rows,
        expected_priority["corrected_user_intended"]["valid_run_vector"],
        expected_priority["historical_rmq_mislabeled"]["valid_run_vector"],
    )
    if invariants.get("checks") != expected_checks:
        # The one pinned pre-writer authority carries the equivalent legacy
        # prose.  It is admitted only under its whole-file SHA below.
        if not legacy_bytes:
            raise ContractError("derived invariant checks do not recompute exactly")
    complete = (
        len({row["stable_key"] for row in rows
             if row.get("timing_scope") == APPROVED_TUPLES_QUERY_TIMING_SCOPE})
        == EXPECTED_RESULT_CELLS if timing_scoped
        else len(rows) == EXPECTED_RESULT_CELLS)
    if (authority.get("full_matrix_complete") is not complete
            or document.get("status") != (
                "approved_complete_matrix" if complete else "approved_partial_matrix")):
        raise ContractError("approved authority completion projection is stale")
    if raw is not None and not legacy_bytes:
        canonical = (_canonical_approved_tuples_yaml_v3(document)
                     if timing_scoped else
                     _canonical_approved_tuples_yaml_v2(document))
        if raw != canonical:
            raise ContractError(
                f"approved tuple authority is not canonical YAML v{authority_version}")
    return rows


def _load_approved_tuple_authority(
        path: Path, expected_sha256: str) -> tuple[dict[str, Any], bytes]:
    try:
        _require_hex(expected_sha256, "authority expected SHA-256", 64)
    except ContractError as exc:
        raise ApprovedTupleProofError(str(exc)) from exc
    raw = _read_approved_tuple_file(path, "approved tuple authority")
    observed = _sha256(raw)
    if observed != expected_sha256:
        raise ApprovedTupleProofError(
            f"authority seal changed: expected={expected_sha256} observed={observed}")
    document = _parse_approved_tuples_yaml(raw, "approved tuple authority")
    _validate_approved_tuple_authority_document(document, raw)
    return document, raw


def _load_approved_append_candidate(path: Path) -> tuple[dict[str, Any], bytes]:
    raw = _read_approved_tuple_file(path, "approved tuple append candidate")
    try:
        text = raw.decode("utf-8")
    except UnicodeError as exc:
        raise ContractError(f"append candidate is not UTF-8: {exc}") from exc
    value = _loads_json(text, "approved tuple append candidate")
    if not isinstance(value, dict):
        raise ContractError("append candidate root must be an object")
    return value, raw


def _approved_proof_has_external_reference(value: Any) -> bool:
    if isinstance(value, dict):
        for key, child in value.items():
            normalized = key.lower()
            if (normalized in {"path", "uri", "url", "log", "location"}
                    or normalized.endswith(("_path", "_uri", "_url", "_log",
                                             "_location"))) \
                    and normalized != "effective_path":
                return True
            if _approved_proof_has_external_reference(child):
                return True
    elif isinstance(value, list):
        return any(_approved_proof_has_external_reference(child) for child in value)
    elif isinstance(value, str):
        lowered = value.lower()
        if (lowered in {"not_sealed", "inferred"}
                or "://" in value or "\\" in value
                or value.startswith(("/", "./", "../"))):
            return True
    return False


def _approved_manifest_rows(
        payload: dict[str, Any], field: str, expected_rows: list[dict[str, Any]],
        label: str) -> list[dict[str, Any]]:
    rows = _require_list(payload.get(field), f"{label}.{field}")
    observed = [
        (item.get("stable_key"), item.get("timing_scope"))
        for item in rows if isinstance(item, dict)]
    expected = [_approved_observation_identity(row) for row in expected_rows]
    if observed != expected:
        raise ApprovedTupleProofError(
            f"{label}.{field} must cover candidate observations in exact order")
    return [_require_dict(item, f"{label}.{field}[{index}]")
            for index, item in enumerate(rows)]


# DOC0433 closes the six proof objects but does not enumerate their payload
# leaves.  These validators are the deliberately narrow candidate-v2 leaf schema: source
# and build bind the same immutable graph/artifact hashes; fixture/config
# observations bind sorted candidate identities, and config embeds the exact
# generator-rendered observed manifest/full config plus the sealed canonical
# q1024 source chain; measurement owns device, driver, interval, and
# physical-stage facts; chunk_plan alone owns repeats.
# None of these audit-only objects is copied into the publication authority.
def _validate_approved_source_proof(
        payload: dict[str, Any], timing_scope: str) -> None:
    _require_exact_keys(payload, {
        "timing_scope", "source_commit", "source_graph_fingerprint",
        "authorized_artifacts_sha256",
    }, "source proof payload")
    if payload.get("timing_scope") != timing_scope:
        raise ApprovedTupleProofError("source proof timing_scope changed")
    source_commit = _require_string(payload.get("source_commit"),
                                    "source proof source_commit")
    if not re.fullmatch(r"[0-9a-f]{40,64}", source_commit):
        raise ApprovedTupleProofError("source proof commit is not a concrete hash")
    _require_hex(payload.get("source_graph_fingerprint"),
                 "source proof graph fingerprint", 64)
    _require_hex(payload.get("authorized_artifacts_sha256"),
                 "source proof authorized artifacts", 64)


def _validate_approved_build_proof(
        payload: dict[str, Any], source_payload: dict[str, Any],
        timing_scope: str, *,
        remote_host: dict[str, Any] | None = None) -> None:
    expected_fields = {
        "timing_scope", "build_receipt_id", "build_receipt_sha256", "mode",
        "preset", "debug", "status", "source_commit", "source_graph_fingerprint",
        "authorized_artifacts_sha256",
    }
    if remote_host is not None:
        expected_fields.update({
            "remote_host_descriptor_sha256", "receipt_host_sha256"})
    _require_exact_keys(payload, expected_fields, "build proof payload")
    receipt_id = _require_string(payload.get("build_receipt_id"),
                                 "build proof receipt id")
    if not re.fullmatch(r"br_[0-9a-f]{32}", receipt_id):
        raise ApprovedTupleProofError("build proof receipt id is not canonical")
    _require_hex(payload.get("build_receipt_sha256"),
                 "build proof receipt SHA-256", 64)
    if (payload.get("timing_scope") != timing_scope
            or payload.get("mode") != "benchmark-e2e"
            or payload.get("debug") is not False
            or payload.get("status") != "success"):
        raise ApprovedTupleProofError("build proof is not a successful production E2E build")
    _require_string(payload.get("preset"), "build proof preset")
    for field in (
            "source_commit", "source_graph_fingerprint",
            "authorized_artifacts_sha256"):
        if payload.get(field) != source_payload.get(field):
            raise ApprovedTupleProofError(
                f"build proof does not bind source proof field {field}")
    if remote_host is not None:
        receipt_host_sha256 = _sha256(_canonical_bytes(
            remote_host["receipt_host"]))
        if (payload.get("preset") != remote_host["build_preset"]
                or payload.get("remote_host_descriptor_sha256")
                    != remote_host["descriptor_sha256"]
                or payload.get("receipt_host_sha256")
                    != receipt_host_sha256):
            raise ApprovedTupleProofError(
                "build proof does not bind the remote authority host")


def _validate_approved_fixture_proof(
        payload: dict[str, Any], candidate_rows: list[dict[str, Any]],
        timing_scope: str) -> None:
    _require_exact_keys(
        payload, {"timing_scope", "fixtures"}, "fixture proof payload")
    if payload.get("timing_scope") != timing_scope:
        raise ApprovedTupleProofError("fixture proof timing_scope changed")
    fixtures = _require_list(payload.get("fixtures"), "fixture proof fixtures")
    expected_by_dataset: dict[str, dict[str, Any]] = {}
    for row in candidate_rows:
        expected_by_dataset.setdefault(row["dataset"], row)
    if len(fixtures) != len(expected_by_dataset):
        raise ApprovedTupleProofError("fixture proof does not cover each candidate dataset")
    observed_datasets: list[str] = []
    fixture_ids: set[str] = set()
    for index, raw_fixture in enumerate(fixtures):
        fixture = _require_dict(raw_fixture, f"fixture proof fixtures[{index}]")
        _require_exact_keys(fixture, {
            "fixture_id", "fixture_sha256", "dataset", "target_size_gbp",
            "alphabet", "kmer_size", "window_size", "query_count",
        }, f"fixture proof fixtures[{index}]")
        dataset = _require_string(fixture.get("dataset"), "fixture proof dataset")
        observed_datasets.append(dataset)
        expected = expected_by_dataset.get(dataset)
        if expected is None or any(fixture.get(field) != expected[field] for field in (
                "target_size_gbp", "alphabet", "kmer_size", "window_size",
                "query_count")):
            raise ApprovedTupleProofError("fixture proof dimensions differ from candidate row")
        fixture_id = _require_string(
            fixture.get("fixture_id"), "fixture proof fixture id")
        if fixture_id in fixture_ids:
            raise ApprovedTupleProofError("fixture proof replays a fixture identity")
        fixture_ids.add(fixture_id)
        _require_hex(fixture.get("fixture_sha256"), "fixture proof SHA-256", 64)
    if observed_datasets != sorted(observed_datasets) or len(set(observed_datasets)) != len(
            observed_datasets):
        raise ApprovedTupleProofError("fixture proof datasets are not sorted and unique")


def _approved_config_projection(row: dict[str, Any]) -> dict[str, Any]:
    return {
        "stable_key": row["stable_key"],
        "timing_scope": row["timing_scope"],
        "mode": row["mode"],
        "pipeline": row["pipeline"],
        "pipeline_mode": row["pipeline_mode"],
        "index_type": row["index_type"],
        "requested_algorithms": row["requested_algorithms"],
    }


def _decode_approved_sealed_json(
        value: Any, label: str) -> tuple[dict[str, Any], bytes]:
    """Decode embedded strict JSON without opening a path."""
    encoded = _require_string(value, f"{label}_base64")
    try:
        raw = base64.b64decode(encoded.encode("ascii"), altchars=b"-_", validate=True)
    except (UnicodeEncodeError, binascii.Error, ValueError) as exc:
        raise ApprovedTupleProofError(
            f"{label} is not canonical base64: {exc}") from exc
    if base64.urlsafe_b64encode(raw).decode("ascii") != encoded:
        raise ApprovedTupleProofError(f"{label} base64 is not canonical")
    try:
        text = raw.decode("utf-8")
    except UnicodeError as exc:
        raise ApprovedTupleProofError(f"{label} is not UTF-8: {exc}") from exc
    document = _loads_json(text, label)
    if not isinstance(document, dict):
        raise ApprovedTupleProofError(f"{label} root must be an object")
    return document, raw


def _decode_approved_generator_json(
        value: Any, label: str) -> tuple[dict[str, Any], bytes]:
    """Decode embedded generator-rendered JSON without opening a path."""
    document, raw = _decode_approved_sealed_json(value, label)
    rendered = (json.dumps(
        document, indent=2, ensure_ascii=True, allow_nan=False) + "\n").encode("utf-8")
    if raw != rendered:
        raise ApprovedTupleProofError(
            f"{label} is not canonical generator JSON")
    return document, raw


def _validate_approved_execution_rung(
        value: Any, projected_config: dict[str, Any],
        canonical_manifest: dict[str, Any], canonical_manifest_raw: bytes,
        canonical_config: dict[str, Any], canonical_config_raw: bytes) -> None:
    """Validate the self-contained q1024 observed-rung projection."""
    rung = _require_dict(value, "config proof observed rung")
    _require_exact_keys(rung, {
        "schema", "version", "query_count", "ladder_query_counts",
        "predecessor_query_count", "predecessor_execution_contract_id",
        "query_ordinal_interval", "projection", "fixture_projection_sha256",
        "matrix_contract", "canonical_campaign_binding",
    }, "config proof observed rung")
    for field in ("version", "query_count", "predecessor_query_count"):
        _require_int(rung.get(field), f"config proof observed rung {field}")
    for index, count in enumerate(_require_list(
            rung.get("ladder_query_counts"), "config proof rung ladder")):
        _require_int(count, f"config proof rung ladder[{index}]")
    interval = _require_dict(
        rung.get("query_ordinal_interval"), "config proof rung ordinal interval")
    for field in ("begin", "end_exclusive"):
        _require_int(interval.get(field), f"config proof rung interval {field}")
    matrix_contract = _require_dict(
        rung.get("matrix_contract"), "config proof rung matrix")
    _require_int(matrix_contract.get("repetitions_per_cell"),
                 "config proof rung matrix repetitions_per_cell")
    if (rung.get("schema") != "bioseq.e2e.algorithm-matrix-observed-rung"
            or rung.get("version") != 1
            or rung.get("query_count") != 1024
            or rung.get("ladder_query_counts") != list(RUNG_QUERY_COUNTS)
            or rung.get("predecessor_query_count") != 512
            or rung.get("predecessor_execution_contract_id")
                != _observed_rung_execution_contract_id(512)
            or rung.get("query_ordinal_interval") != {
                "begin": 0, "end_exclusive": 1024}
            or rung.get("projection")
                != "exact_fixed_seed_ordered_prefix_of_q1024"):
        raise ApprovedTupleProofError(
            "config proof execution manifest is not the exact q1024 rung")
    fixture_projection_sha256 = _require_hex(
        rung.get("fixture_projection_sha256"),
        "config proof rung fixture projection SHA-256", 64)
    _require_exact_keys(
        projected_config, {"params", "test_cases", "bench_e2e", "regression_rules"},
        "config proof observed full config")
    if fixture_projection_sha256 != _rung_fixture_projection_sha256(
            projected_config, 1024):
        raise ApprovedTupleProofError(
            "config proof rung fixture projection does not match supplied config")
    if _require_dict(rung.get("matrix_contract"), "config proof rung matrix") != {
            "all_axes_preserved": True,
            "repetitions_per_cell": MIN_ACCEPTED_REPEATS,
            "prefilter_enabled": False,
            "base_alignment_algorithms": ["ksw2", "block_aligner"],
            "excluded_base_alignment_algorithms": ["ksw2_batched"],
    }:
        raise ApprovedTupleProofError(
            "config proof observed rung changed the matrix contract")
    binding = _require_dict(
        rung.get("canonical_campaign_binding"),
        "config proof canonical q1024 binding")
    _require_exact_keys(binding, {
        "campaign_id", "campaign_digest", "manifest", "manifest_sha256",
        "config", "config_sha256",
    }, "config proof canonical q1024 binding")
    expected_manifest = Q1024_CONTRACT_PATH.relative_to(REPOSITORY_ROOT).as_posix()
    expected_config = Q1024_CONFIG_PATH.relative_to(REPOSITORY_ROOT).as_posix()
    _validate_canonical_q1024_source_binding(
        canonical_manifest, canonical_config_raw)
    if (binding.get("campaign_id") != Q1024_CAMPAIGN_ID
            or binding.get("campaign_digest") != Q1024_CAMPAIGN_DIGEST
            or _canonical_repository_relative(
                binding.get("manifest"), "config proof canonical manifest")
                != expected_manifest
            or _canonical_repository_relative(
                binding.get("config"), "config proof canonical config")
                != expected_config
            or _require_hex(
                binding.get("manifest_sha256"),
                "config proof canonical manifest SHA-256", 64)
                != _sha256(canonical_manifest_raw)
            or _require_hex(
                binding.get("config_sha256"),
                "config proof canonical config SHA-256", 64)
                != _sha256(canonical_config_raw)):
        raise ApprovedTupleProofError(
            "config proof observed rung changed its canonical campaign binding")
    if projected_config != _expected_observed_rung_config(canonical_config, 1024):
        raise ApprovedTupleProofError(
            "config proof supplied config is not the canonical q1024 projection")


def _validate_approved_execution_manifest_cell(
        raw_cell: Any, campaign_id: str, label: str) -> dict[str, Any]:
    """Validate one cell descriptor embedded in an observed campaign manifest."""
    cell = _require_dict(raw_cell, label)
    _require_exact_keys(cell, {
        "stable_key", "identity", "test_case", "test_case_index", "algorithm",
        "tuple_index", "params", "allowed_statuses",
    }, label)
    identity = _require_dict(cell.get("identity"), f"{label}.identity")
    if tuple(identity) != STABLE_KEY_FIELDS or identity.get("campaign_id") != campaign_id:
        raise ApprovedTupleProofError(
            f"{label} identity fields/campaign are not canonical")
    stable_key = _require_string(cell.get("stable_key"), f"{label}.stable_key")
    if _stable_key_for_identity(identity) != stable_key:
        raise ApprovedTupleProofError(f"{label} stable key does not recompute")
    _require_string(cell.get("test_case"), f"{label}.test_case")
    _require_int(cell.get("test_case_index"), f"{label}.test_case_index")
    _require_string(cell.get("algorithm"), f"{label}.algorithm")
    _require_int(cell.get("tuple_index"), f"{label}.tuple_index")
    params = _require_dict(cell.get("params"), f"{label}.params")
    if any(isinstance(item, (dict, list)) or item is None for item in params.values()):
        raise ApprovedTupleProofError(f"{label} params are not scalar")
    if cell.get("allowed_statuses") != _allowed_cell_statuses(
            identity.get("engine"), OBSERVED_RESULT_STATUS_CONTRACT):
        raise ApprovedTupleProofError(
            f"{label} does not use the observed accepted-only status contract")
    return cell


def _validate_approved_execution_manifest(
        manifest: dict[str, Any], candidate_rows: list[dict[str, Any]],
        config_sha256: str, projected_config: dict[str, Any]) -> None:
    """Validate one embedded observed manifest and bind candidate configurations."""
    _require_exact_keys(manifest, {
        "schema", "version", "campaign", "source", "configs", "shard_ownership",
        "rung",
    }, "config proof execution manifest")
    if (manifest.get("schema") != CAMPAIGN_CONTRACT_SCHEMA
            or manifest.get("version") != OBSERVED_RUNG_CAMPAIGN_CONTRACT_VERSION):
        raise ApprovedTupleProofError(
            "config proof execution manifest schema/version changed")
    campaign = _require_dict(
        manifest.get("campaign"), "config proof execution campaign")
    _require_exact_keys(campaign, {
        "campaign_id", "campaign_digest_algorithm", "stable_key_schema",
        "stable_key_fields", "repetitions_per_cell", "expected_result_cells",
        "expected_raw_records", "required_shard_configs",
        "publication_status_contract", "query_count", "result_status_contract",
        "execution_contract_id", "campaign_digest",
    }, "config proof execution campaign")
    required_shards = _require_int(
        campaign.get("required_shard_configs"),
        "config proof required shard configs", minimum=1)
    if (campaign.get("campaign_id") != Q1024_CAMPAIGN_ID
            or campaign.get("campaign_digest_algorithm")
                != "sha256-canonical-json-without-campaign_digest"
            or campaign.get("stable_key_schema") != "bioseq-e2e-cell-v2"
            or campaign.get("stable_key_fields") != list(STABLE_KEY_FIELDS)
            or campaign.get("repetitions_per_cell") != MIN_ACCEPTED_REPEATS
            or campaign.get("expected_result_cells") != EXPECTED_RESULT_CELLS
            or campaign.get("expected_raw_records")
                != EXPECTED_RESULT_CELLS * MIN_ACCEPTED_REPEATS
            or required_shards not in {88, 256}
            or campaign.get("query_count") != 1024
            or campaign.get("result_status_contract")
                != OBSERVED_RESULT_STATUS_CONTRACT
            or campaign.get("execution_contract_id")
                != _observed_rung_execution_contract_id(1024)
            or campaign.get("publication_status_contract") != {
                "status_source": "observed",
                "minimap2": ["accepted"],
                "bioseq": ["accepted"],
                "rejected_dispositions": ["rejected_setup", "rejected_sql"],
                "nonpublishable_statuses": ["rejected", "invalid", "pending"],
            }):
        raise ApprovedTupleProofError(
            "config proof execution campaign is not the observed q1024 contract")
    campaign_digest = _require_hex(
        campaign.get("campaign_digest"), "config proof campaign digest", 64)
    digest_payload = copy.deepcopy(manifest)
    del digest_payload["campaign"]["campaign_digest"]
    if _sha256(_canonical_bytes(digest_payload)) != campaign_digest:
        raise ApprovedTupleProofError(
            "config proof execution campaign digest does not recompute")

    source = _require_dict(manifest.get("source"), "config proof execution source")
    _require_exact_keys(
        source, {"full_config", "full_config_sha256", "generator"},
        "config proof execution source")
    full_config = _canonical_repository_relative(
        source.get("full_config"), "config proof observed full config")
    if (source.get("generator") != "bench/generate_e2e_algorithm_matrix_shards.py"
            or _require_hex(
                source.get("full_config_sha256"),
                "config proof observed full-config SHA-256", 64) != config_sha256):
        raise ApprovedTupleProofError(
            "config proof execution manifest does not bind its supplied config")
    # The rung is validated after the embedded canonical source chain is decoded.

    expected_topology = (
        {"no_index_baselines": (8, 82), "persisted_index": (80, 8)}
        if required_shards == 88 else
        {"no_index_micro": (160, 4), "minimap2_baseline": (16, 1),
         "persisted_index": (80, 8)})
    entries = _require_list(manifest.get("configs"), "config proof manifest configs")
    if len(entries) != required_shards + 1:
        raise ApprovedTupleProofError(
            "config proof manifest does not contain one monolith plus every shard")
    configs_by_path: dict[str, dict[str, Any]] = {}
    descriptors_by_path: dict[str, dict[str, dict[str, Any]]] = {}
    shards: list[dict[str, Any]] = []
    monolithic: dict[str, Any] | None = None
    topology: dict[str, list[int]] = {}
    for entry_index, raw_entry in enumerate(entries):
        label = f"config proof manifest configs[{entry_index}]"
        entry = _require_dict(raw_entry, label)
        _require_exact_keys(entry, {
            "path", "config_sha256", "role", "shard_kind",
            "expected_result_cells", "expected_raw_records",
            "expected_stable_keys_sha256", "allowed_nonblocking_rule_names",
            "declared_metric_names", "expected_all_records_disposition", "cells",
        }, label)
        path = _canonical_repository_relative(entry.get("path"), f"{label}.path")
        if path in configs_by_path:
            raise ApprovedTupleProofError(
                "config proof execution manifest repeats a config path")
        _require_hex(entry.get("config_sha256"), f"{label}.config_sha256", 64)
        role = _require_string(entry.get("role"), f"{label}.role")
        shard_kind = entry.get("shard_kind")
        if role == "monolithic":
            if monolithic is not None or shard_kind is not None:
                raise ApprovedTupleProofError(
                    "config proof execution manifest has an invalid monolith")
            monolithic = entry
            expected_metrics = list(FULL_DECLARED_METRIC_NAMES)
        elif (role == "shard" and isinstance(shard_kind, str)
                and shard_kind in expected_topology):
            shards.append(entry)
            topology.setdefault(shard_kind, []).append(entry["expected_result_cells"])
            expected_metrics = list(
                MIXED_PERSISTED_METRIC_NAMES
                if shard_kind in {"persisted_index", "no_index_micro"}
                else MINIMAP2_BASELINE_METRIC_NAMES
                if shard_kind == "minimap2_baseline"
                else FULL_DECLARED_METRIC_NAMES)
        else:
            raise ApprovedTupleProofError(
                "config proof execution manifest has a foreign role/shard kind")
        expected_count = _require_int(
            entry.get("expected_result_cells"), f"{label}.expected cells", minimum=1)
        if (entry.get("expected_raw_records")
                != expected_count * MIN_ACCEPTED_REPEATS
                or entry.get("expected_all_records_disposition") is not None):
            raise ApprovedTupleProofError(
                "config proof observed manifest retained a predicted disposition")
        rules = _require_list(
            entry.get("allowed_nonblocking_rule_names"), f"{label}.rules")
        if (any(not isinstance(rule, str) or not rule for rule in rules)
                or len(rules) != len(set(rules))):
            raise ApprovedTupleProofError(
                "config proof manifest allowed-rule names are malformed")
        metrics = _require_list(entry.get("declared_metric_names"), f"{label}.metrics")
        if metrics != expected_metrics:
            raise ApprovedTupleProofError(
                "config proof manifest metric applicability changed")
        cells = _require_list(entry.get("cells"), f"{label}.cells")
        if len(cells) != expected_count:
            raise ApprovedTupleProofError(
                "config proof manifest cell count changed")
        descriptors: dict[str, dict[str, Any]] = {}
        for cell_index, raw_cell in enumerate(cells):
            cell = _validate_approved_execution_manifest_cell(
                raw_cell, campaign["campaign_id"], f"{label}.cells[{cell_index}]")
            stable_key = cell["stable_key"]
            if stable_key in descriptors:
                raise ApprovedTupleProofError(
                    "config proof manifest config repeats a stable key")
            descriptors[stable_key] = cell
        if (_require_hex(
                entry.get("expected_stable_keys_sha256"),
                f"{label}.stable-key SHA-256", 64)
                != _sha256("\n".join(sorted(descriptors)).encode("utf-8"))):
            raise ApprovedTupleProofError(
                "config proof manifest config stable-key digest does not recompute")
        configs_by_path[path] = entry
        descriptors_by_path[path] = descriptors

    if monolithic is None or len(shards) != required_shards:
        raise ApprovedTupleProofError(
            "config proof manifest monolith/shard count changed")
    for kind, (expected_count, expected_cells) in expected_topology.items():
        cell_counts = topology.get(kind, [])
        if (len(cell_counts) != expected_count
                or any(count != expected_cells for count in cell_counts)):
            raise ApprovedTupleProofError(
                f"config proof manifest {kind} topology changed")
    canonical_cells = descriptors_by_path[monolithic["path"]]
    if (len(canonical_cells) != EXPECTED_RESULT_CELLS
            or monolithic.get("path") != full_config
            or monolithic.get("config_sha256") != config_sha256):
        raise ApprovedTupleProofError(
            "config proof manifest monolith does not bind the supplied full config")
    benchmark = _require_dict(
        projected_config.get("bench_e2e"), "config proof full config bench_e2e")
    if (benchmark.get("fast") is not False
            or benchmark.get("concurrency") != {
                "bench_lock": True, "concurrency": False}
            or benchmark.get("iteration_order") != {
                "mode": "interleaved_seeded", "seed": 333}
            or projected_config.get("regression_rules") != []):
        raise ApprovedTupleProofError(
            "config proof full config scheduling contract changed")
    matrix = _require_dict(
        benchmark.get("matrix"), "config proof full config matrix")
    metrics = _require_dict(
        matrix.get("metrics"), "config proof full config metrics")
    if (list(metrics) != monolithic["declared_metric_names"]
            or _config_nonblocking_rule_names(projected_config)
                != monolithic["allowed_nonblocking_rule_names"]):
        raise ApprovedTupleProofError(
            "config proof full config metric/rule projection changed")
    execution_cells = _execution_descriptors_from_config(
        projected_config, campaign["campaign_id"],
        OBSERVED_RESULT_STATUS_CONTRACT, query_count=1024)
    if execution_cells != monolithic["cells"]:
        raise ApprovedTupleProofError(
            "config proof full config expansion differs from manifest monolith")
    shard_owner: dict[str, str] = {}
    for shard in shards:
        for stable_key in descriptors_by_path[shard["path"]]:
            if stable_key in shard_owner or stable_key not in canonical_cells:
                raise ApprovedTupleProofError(
                    "config proof manifest shard ownership overlaps or is foreign")
            shard_owner[stable_key] = shard["path"]
    if set(shard_owner) != set(canonical_cells):
        raise ApprovedTupleProofError(
            "config proof manifest shard ownership is incomplete")
    ownership = _require_dict(
        manifest.get("shard_ownership"), "config proof shard ownership")
    _require_exact_keys(ownership, {
        "exact_nonoverlapping", "shard_paths", "stable_keys_sha256",
        "resume_semantics",
    }, "config proof shard ownership")
    if (ownership.get("exact_nonoverlapping") is not True
            or ownership.get("shard_paths") != [shard["path"] for shard in shards]
            or _require_hex(
                ownership.get("stable_keys_sha256"),
                "config proof shard ownership SHA-256", 64)
                != _sha256("\n".join(sorted(shard_owner)).encode("utf-8"))):
        raise ApprovedTupleProofError(
            "config proof manifest shard ownership seal changed")
    _require_string(
        ownership.get("resume_semantics"), "config proof shard resume semantics")

    for row in candidate_rows:
        cell = canonical_cells.get(row["stable_key"])
        if (cell is None or cell["identity"] != _approved_row_identity(row)
                or row["query_count"] != campaign["query_count"]):
            raise ApprovedTupleProofError(
                "candidate row is not owned by the supplied q1024 manifest")
        if row["pipeline"] == "bioseq":
            expected_algorithm = f"bioseq_algorithm_matrix_{row['mode']}"
            expected_params = {
                "_": 0,
                "base_alignment_algorithm": row["requested_algorithms"]["base_alignment"],
                "chaining_algorithm": row["requested_algorithms"]["chaining"],
                "minimizer_algorithm": row["requested_algorithms"]["minimizer"],
                "minimizer_index": row["index_type"],
                "pipeline_mode": row["pipeline_mode"],
            }
            if row["mode"] == "no_index":
                expected_params["recompute_async_persistence"] = "off"
        else:
            expected_algorithm = f"minimap2_{row['mode']}"
            expected_params = {"_": 0, "threads": 0}
        if cell["algorithm"] != expected_algorithm or cell["params"] != expected_params:
            raise ApprovedTupleProofError(
                "candidate row differs from its supplied manifest configuration")


def _validate_approved_config_proof(
        payload: dict[str, Any], candidate_rows: list[dict[str, Any]],
        campaign: dict[str, Any], timing_scope: str) -> None:
    _require_exact_keys(payload, {
        "timing_scope", "campaign_id", "campaign_digest", "execution_contract",
        "config_sha256", "config_base64", "canonical_config_base64", "rows",
    }, "config proof payload")
    if (payload.get("timing_scope") != timing_scope
            or payload.get("campaign_id") != campaign.get("campaign_id")
            or payload.get("campaign_digest") != campaign.get("campaign_digest")):
        raise ApprovedTupleProofError("config proof campaign binding changed")
    execution = _require_dict(payload.get("execution_contract"),
                              "config proof execution contract")
    _require_exact_keys(execution, {
        "identity", "version", "sha256", "result_status_contract",
        "manifest_base64", "canonical_manifest_base64",
    }, "config proof execution contract")
    if (execution.get("identity") != _observed_rung_execution_contract_id(1024)
            or execution.get("version") != OBSERVED_RUNG_CAMPAIGN_CONTRACT_VERSION
            or execution.get("result_status_contract") != OBSERVED_RESULT_STATUS_CONTRACT):
        raise ApprovedTupleProofError(
            "append proof requires the repaired q1024 observed execution contract")
    execution_digest = _require_hex(
        execution.get("sha256"), "config proof execution-contract SHA-256", 64)
    manifest, manifest_raw = _decode_approved_generator_json(
        execution.get("manifest_base64"), "config proof execution manifest")
    if execution_digest == campaign.get("campaign_digest"):
        raise ApprovedTupleProofError(
            "new execution contract may not reuse the historical campaign digest")
    if execution_digest != _sha256(manifest_raw):
        raise ApprovedTupleProofError(
            "config proof execution-contract digest does not match manifest bytes")
    config_sha256 = _require_hex(
        payload.get("config_sha256"), "config proof SHA-256", 64)
    projected_config, config_raw = _decode_approved_generator_json(
        payload.get("config_base64"), "config proof observed full config")
    if _sha256(config_raw) != config_sha256:
        raise ApprovedTupleProofError(
            "config proof digest does not match supplied config bytes")
    canonical_manifest, canonical_manifest_raw = _decode_approved_generator_json(
        execution.get("canonical_manifest_base64"),
        "config proof canonical q1024 manifest")
    canonical_config, canonical_config_raw = _decode_approved_sealed_json(
        payload.get("canonical_config_base64"),
        "config proof canonical q1024 config")
    _validate_approved_execution_rung(
        manifest.get("rung"), projected_config,
        canonical_manifest, canonical_manifest_raw,
        canonical_config, canonical_config_raw)
    _validate_approved_execution_manifest(
        manifest, candidate_rows, config_sha256, projected_config)
    rows = _approved_manifest_rows(
        payload, "rows", candidate_rows,
        "config proof")
    for index, (proof_row, candidate_row) in enumerate(zip(rows, candidate_rows)):
        _require_exact_keys(proof_row, {
            "stable_key", "timing_scope", "mode", "pipeline", "pipeline_mode",
            "index_type", "requested_algorithms",
        }, f"config proof rows[{index}]")
        if proof_row != _approved_config_projection(candidate_row):
            raise ApprovedTupleProofError(
                f"config proof rows[{index}] differs from candidate configuration")


def _approved_measurement_projection(row: dict[str, Any]) -> dict[str, Any]:
    return {
        "stable_key": row["stable_key"],
        "timing_scope": row["timing_scope"],
        "requested_algorithms": row["requested_algorithms"],
        "observed_algorithms": row["observed_algorithms"],
        "effective_path": row["effective_path"],
        "execution_conformance": row["execution_conformance"],
        "backend_declines": 0,
        "backend_fallbacks": 0,
        "prefilter_activity_count": 0,
        "physical_stage_conformance": True,
    }


def _validate_approved_gate8_sql_v1(
        value: Any, row: dict[str, Any], label: str) -> None:
    if row["pipeline"] != "bioseq":
        if value is not None:
            raise ApprovedTupleProofError(
                f"{label} gives isolated minimap2 a PostgreSQL Gate-8 proof")
        return
    proof = _require_dict(value, label)
    _require_exact_keys(proof, {
        "schema", "version", "raw_payload_schema_version", "repeat_count",
        "repeats",
    }, label)
    for field in ("version", "raw_payload_schema_version", "repeat_count"):
        _require_int(proof.get(field), f"{label}.{field}")
    if (proof.get("schema") != "bioseq.e2e.gate8-approved-repeats"
            or proof.get("version") != 1
            or proof.get("raw_payload_schema_version")
                != RAW_PAYLOAD_SCHEMA_VERSION
            or proof.get("repeat_count") != MIN_ACCEPTED_REPEATS):
        raise ApprovedTupleProofError(f"{label} identity/version/count changed")
    repeats = _require_list(proof.get("repeats"), f"{label}.repeats")
    if len(repeats) != MIN_ACCEPTED_REPEATS:
        raise ApprovedTupleProofError(f"{label} must bind all five raw repeats")
    applicable_cache = (
        row["mode"] == "just_index"
        and row["requested_algorithms"]["minimizer"]
            == "vulkan_minimap_integer"
    )
    expected_cache = {
        "applicability": (
            "persisted_vulkan_seed_statement_resident" if applicable_cache
            else "not_applicable_to_selected_route"),
        "gpu_persisted_resident_uploads": 1 if applicable_cache else 0,
        "gpu_persisted_resident_cache_hits": 0,
    }
    for ordinal, raw_repeat in enumerate(repeats):
        repeat = _require_dict(raw_repeat, f"{label}.repeats[{ordinal}]")
        _require_exact_keys(repeat, {
            "repeat", "repeat_identity", "measurement_sql_sha256",
            "pre_statement_gucs_sha256", "explain_json_sha256",
            "literal_single_statement", "complete_guc_count",
            "explain_analyze_json", "custom_scan_node_count",
            "custom_plan_provider", "chaining_algorithm",
            "base_alignment_algorithm", "pipeline_concurrency_mode",
            "query_fixture_physical_no_index", "prefilter_activity_count",
            "route_and_ordered_counts_unchanged",
            "cache_control_disposition",
            "vectorization_control_disposition", "cache_evidence",
        }, f"{label}.repeats[{ordinal}]")
        for field in ("repeat", "complete_guc_count", "custom_scan_node_count",
                      "prefilter_activity_count"):
            _require_int(repeat.get(field), f"{label}.repeats[{ordinal}].{field}")
        cache = _require_dict(
            repeat.get("cache_evidence"), f"{label}.repeats[{ordinal}].cache_evidence")
        for field in ("gpu_persisted_resident_uploads",
                      "gpu_persisted_resident_cache_hits"):
            _require_int(cache.get(field),
                         f"{label}.repeats[{ordinal}].cache_evidence.{field}")
        if (repeat.get("repeat") != ordinal
                or repeat.get("repeat_identity")
                    != (f"{row['stable_key']}:timing_scope:"
                        f"{row['timing_scope']}:repeat:{ordinal}")
                or repeat.get("literal_single_statement") is not True
                or repeat.get("complete_guc_count") != 22
                or repeat.get("explain_analyze_json") is not True
                or repeat.get("custom_scan_node_count") != 1
                or repeat.get("custom_plan_provider") != "BioseqBatchJoin"
                or repeat.get("chaining_algorithm")
                    != row["requested_algorithms"]["chaining"]
                or repeat.get("base_alignment_algorithm")
                    != row["requested_algorithms"]["base_alignment"]
                or repeat.get("pipeline_concurrency_mode")
                    != row["pipeline_mode"]
                or repeat.get("query_fixture_physical_no_index") is not True
                or repeat.get("prefilter_activity_count") != 0
                or repeat.get("route_and_ordered_counts_unchanged") is not True
                or repeat.get("cache_control_disposition")
                    != "nonpublishable_control_config_changed"
                or repeat.get("vectorization_control_disposition")
                    != "nonpublishable_control_config_changed"
                or repeat.get("cache_evidence") != expected_cache):
            raise ApprovedTupleProofError(
                f"{label}.repeats[{ordinal}] does not retain the exact Gate-8 contract")
        for digest_field in (
                "measurement_sql_sha256", "pre_statement_gucs_sha256",
                "explain_json_sha256"):
            _require_hex(
                repeat.get(digest_field),
                f"{label}.repeats[{ordinal}].{digest_field}", 64)


def _validate_approved_gate8_sql_v2(
        value: Any,
        row: dict[str, Any],
        label: str,
        build_payload: dict[str, Any],
        fixture_payload: dict[str, Any],
        config_payload: dict[str, Any],
        measurement_payload: dict[str, Any],
) -> None:
    proof = _require_dict(value, label)
    _require_exact_keys(proof, {
        "schema", "version", "raw_payload_schema_version",
        "qualification_count", "repeat_count", "qualification", "bindings",
    }, label)
    for field in (
            "version", "raw_payload_schema_version", "qualification_count",
            "repeat_count"):
        _require_int(proof.get(field), f"{label}.{field}")
    if (proof.get("schema")
            != "bioseq.e2e.gate8-approved-cell-qualification"
            or proof.get("version") != 2
            or proof.get("raw_payload_schema_version")
                != RAW_PAYLOAD_SCHEMA_VERSION
            or proof.get("qualification_count") != 1
            or proof.get("repeat_count") != MIN_ACCEPTED_REPEATS):
        raise ApprovedTupleProofError(
            f"{label} v2 identity/version/count changed")

    qualification = _require_dict(
        proof.get("qualification"), f"{label}.qualification")
    _require_exact_keys(qualification, {
        "qualification_order", "qualification_key",
        "qualification_key_sha256", "qualification_sha256",
        "source_repeat", "source_repeat_identity", "source_record_checksum",
        "source_postgres", "source_measurement_sha256",
        "query_fixture_sha256", "explain_json_sha256", "complete_guc_count",
        "custom_scan_node_count", "custom_plan_provider",
        "chaining_algorithm", "base_alignment_algorithm",
        "pipeline_concurrency_mode", "query_fixture_physical_no_index",
        "prefilter_activity_count", "route_and_ordered_counts_unchanged",
        "cache_control_disposition", "vectorization_control_disposition",
        "cache_evidence",
    }, f"{label}.qualification")
    key = _require_dict(
        qualification.get("qualification_key"),
        f"{label}.qualification.qualification_key")
    _require_exact_keys(key, {
        "stable_key", "test_case", "group_index", "query_count",
        "campaign_id", "campaign_digest", "config_sha256", "report_sha256",
        "raw_sidecar_sha256", "run_id", "run_receipt_id",
        "run_receipt_sha256", "build_receipt_id", "build_receipt_sha256",
        "runtime_cell_sha256", "raw_qualification_sha256",
        "fixture_qualification_sha256", "fixture_projection_sha256",
        "runtime_device", "source_repeat",
        "source_record_checksum",
    }, f"{label}.qualification.qualification_key")
    key_sha256 = _require_hex(
        qualification.get("qualification_key_sha256"),
        f"{label} qualification-key SHA-256", 64)
    if _sha256(_canonical_bytes(key)) != key_sha256:
        raise ApprovedTupleProofError(
            f"{label} qualification key seal does not recompute")
    for field in (
            "qualification_sha256", "source_measurement_sha256",
            "query_fixture_sha256", "explain_json_sha256"):
        _require_hex(
            qualification.get(field), f"{label}.qualification.{field}", 64)
    source_record_checksum = _require_hex(
        qualification.get("source_record_checksum"),
        f"{label} source record checksum", 16)
    source_repeat_identity = (
        f"{row['stable_key']}:timing_scope:{row['timing_scope']}:repeat:0")
    fixture = next((
        item for item in _require_list(
            fixture_payload.get("fixtures"), "fixture proof fixtures")
        if isinstance(item, dict) and item.get("dataset") == row["dataset"]
    ), None)
    if fixture is None:
        raise ApprovedTupleProofError(
            f"{label} has no matching fixture-proof row")
    execution_contract = _require_dict(
        config_payload.get("execution_contract"),
        f"{label} config execution contract")
    execution_manifest, _ = _decode_approved_generator_json(
        execution_contract.get("manifest_base64"),
        f"{label} execution manifest")
    rung = _require_dict(
        execution_manifest.get("rung"), f"{label} execution rung")
    runtime_device = _require_dict(
        key.get("runtime_device"), f"{label} runtime device")
    _require_exact_keys(runtime_device, {
        "execution_device", "gpu_device_uuid", "gpu_driver",
    }, f"{label} runtime device")
    gpu_selected = (
        row["requested_algorithms"]["minimizer"] == "vulkan_minimap_integer"
        or row["requested_algorithms"]["chaining"]
            == "vulkan_tiled_dynamic_programming")
    expected_device = "cpu_and_gpu" if gpu_selected else "cpu"
    gpu_uuid = _require_text(
        runtime_device.get("gpu_device_uuid"), f"{label} GPU UUID")
    gpu_driver = _require_text(
        runtime_device.get("gpu_driver"), f"{label} GPU driver")
    measurement_device = _require_dict(
        measurement_payload.get("device"), "measurement-contract device")
    measurement_driver = _require_dict(
        measurement_payload.get("driver"), "measurement-contract driver")
    for identity_value, identity_label in (
            (measurement_device, "measurement-contract device"),
            (measurement_driver, "measurement-contract driver")):
        _require_string(identity_value.get("identity"), f"{identity_label} identity")
        _require_string(identity_value.get("version"), f"{identity_label} version")
    if (key.get("stable_key") != row["stable_key"]
            or key.get("query_count") != row["query_count"]
            or key.get("campaign_id") != row["campaign_id"]
            or key.get("campaign_id") != config_payload.get("campaign_id")
            or key.get("campaign_digest") != config_payload.get("campaign_digest")
            or key.get("config_sha256") != config_payload.get("config_sha256")
            or key.get("fixture_projection_sha256")
                != rung.get("fixture_projection_sha256")
            or key.get("build_receipt_id")
                != build_payload.get("build_receipt_id")
            or key.get("build_receipt_sha256")
                != build_payload.get("build_receipt_sha256")
            or key.get("source_repeat") != 0
            or key.get("source_record_checksum") != source_record_checksum
            or qualification.get("qualification_order")
                != "after_source_measurement"
            or qualification.get("source_repeat") != 0
            or qualification.get("source_repeat_identity")
                != source_repeat_identity
            or qualification.get("qualification_sha256")
                != key.get("raw_qualification_sha256")
            or runtime_device.get("execution_device") != expected_device
            or (gpu_selected and (not gpu_uuid or not gpu_driver))):
        raise ApprovedTupleProofError(
            f"{label} qualification does not bind its cell/config/build/source")
    _require_string(key.get("test_case"), f"{label} test case")
    _require_int(key.get("group_index"), f"{label} group index")
    _require_string(key.get("run_id"), f"{label} run id")
    if re.fullmatch(r"rr_[0-9a-f]{32}", _require_string(
            key.get("run_receipt_id"), f"{label} run receipt id")) is None:
        raise ApprovedTupleProofError(f"{label} run receipt id is not canonical")
    for field in (
            "report_sha256", "raw_sidecar_sha256", "run_receipt_sha256",
            "build_receipt_sha256", "runtime_cell_sha256",
            "raw_qualification_sha256", "fixture_qualification_sha256",
            "fixture_projection_sha256"):
        _require_hex(key.get(field), f"{label} key.{field}", 64)
    if (key.get("build_receipt_id") != build_payload.get("build_receipt_id")
            or fixture.get("fixture_id") is None
            or fixture.get("fixture_sha256") is None):
        raise ApprovedTupleProofError(
            f"{label} build/fixture ownership is incomplete")

    source_postgres = _validate_gate8_postgres_identity(
        qualification.get("source_postgres"),
        f"{label} qualification source PostgreSQL")
    expected_cache = {
        "applicability": (
            "persisted_vulkan_seed_statement_resident"
            if row["mode"] == "just_index"
            and row["requested_algorithms"]["minimizer"]
                == "vulkan_minimap_integer"
            else "not_applicable_to_selected_route"),
        "gpu_persisted_resident_uploads": (
            1 if row["mode"] == "just_index"
            and row["requested_algorithms"]["minimizer"]
                == "vulkan_minimap_integer" else 0),
        "gpu_persisted_resident_cache_hits": 0,
    }
    if (_require_int(
            qualification.get("complete_guc_count"),
            f"{label} complete GUC count") != 22
            or _require_int(
                qualification.get("custom_scan_node_count"),
                f"{label} custom scan count") != 1
            or qualification.get("custom_plan_provider") != "BioseqBatchJoin"
            or qualification.get("chaining_algorithm")
                != row["requested_algorithms"]["chaining"]
            or qualification.get("base_alignment_algorithm")
                != row["requested_algorithms"]["base_alignment"]
            or qualification.get("pipeline_concurrency_mode")
                != row["pipeline_mode"]
            or qualification.get("query_fixture_physical_no_index") is not True
            or _require_int(
                qualification.get("prefilter_activity_count"),
                f"{label} qualification prefilter count") != 0
            or qualification.get("route_and_ordered_counts_unchanged") is not True
            or qualification.get("cache_control_disposition")
                != "nonpublishable_control_config_changed"
            or qualification.get("vectorization_control_disposition")
                != "nonpublishable_control_config_changed"
            or qualification.get("cache_evidence") != expected_cache):
        raise ApprovedTupleProofError(
            f"{label} source qualification does not retain Gate-8 controls")

    bindings = _require_list(proof.get("bindings"), f"{label}.bindings")
    if len(bindings) != MIN_ACCEPTED_REPEATS:
        raise ApprovedTupleProofError(f"{label} must bind all five raw repeats")
    record_checksums: set[str] = set()
    postgres_epochs: set[str] = set()
    for ordinal, raw_binding in enumerate(bindings):
        binding = _require_dict(raw_binding, f"{label}.bindings[{ordinal}]")
        _require_exact_keys(binding, {
            "repeat", "repeat_identity", "role", "record_checksum",
            "current_measurement_sha256", "measurement_sql_sha256",
            "pre_statement_gucs_sha256", "ordered_primary_counts_sha256",
            "current_query_fixture_sha256",
            "current_postgres", "source_postgres",
            "qualification_key_sha256", "source_record_checksum",
            "source_epoch_is_current", "prefilter_activity_count",
            "route_and_ordered_counts_unchanged",
        }, f"{label}.bindings[{ordinal}]")
        repeat = _require_int(
            binding.get("repeat"), f"{label}.bindings[{ordinal}].repeat")
        expected_repeat_identity = (
            f"{row['stable_key']}:timing_scope:{row['timing_scope']}:"
            f"repeat:{ordinal}")
        checksum = _require_hex(
            binding.get("record_checksum"),
            f"{label}.bindings[{ordinal}].record_checksum", 16)
        current_postgres = _validate_gate8_postgres_identity(
            binding.get("current_postgres"),
            f"{label}.bindings[{ordinal}].current_postgres")
        binding_source = _validate_gate8_postgres_identity(
            binding.get("source_postgres"),
            f"{label}.bindings[{ordinal}].source_postgres")
        epoch = current_postgres["postmaster_start_time"]
        if (repeat != ordinal
                or binding.get("repeat_identity") != expected_repeat_identity
                or binding.get("role")
                    != ("source" if ordinal == 0 else "reuse")
                or _require_bool(
                    binding.get("source_epoch_is_current"),
                    f"{label}.bindings[{ordinal}].source_epoch_is_current")
                    != (ordinal == 0)
                or binding.get("qualification_key_sha256") != key_sha256
                or binding.get("source_record_checksum")
                    != source_record_checksum
                or binding_source != source_postgres
                or current_postgres["database_oid"]
                    != source_postgres["database_oid"]
                or (ordinal == 0 and (
                    checksum != source_record_checksum
                    or current_postgres != source_postgres))
                or (ordinal != 0 and epoch
                    == source_postgres["postmaster_start_time"])
                or binding.get("current_measurement_sha256")
                    != qualification.get("source_measurement_sha256")
                or binding.get("current_query_fixture_sha256")
                    != qualification.get("query_fixture_sha256")
                or _require_int(
                    binding.get("prefilter_activity_count"),
                    f"{label}.bindings[{ordinal}].prefilter_activity_count") != 0
                or binding.get("route_and_ordered_counts_unchanged") is not True
                or checksum in record_checksums or epoch in postgres_epochs):
            raise ApprovedTupleProofError(
                f"{label}.bindings[{ordinal}] is replayed or cross-qualified")
        for field in (
                "current_measurement_sha256", "measurement_sql_sha256",
                "pre_statement_gucs_sha256", "ordered_primary_counts_sha256",
                "current_query_fixture_sha256"):
            _require_hex(
                binding.get(field), f"{label}.bindings[{ordinal}].{field}", 64)
        record_checksums.add(checksum)
        postgres_epochs.add(epoch)


def _validate_approved_gate8_sql(
        value: Any,
        row: dict[str, Any],
        label: str,
        *,
        build_payload: dict[str, Any],
        fixture_payload: dict[str, Any],
        config_payload: dict[str, Any],
        measurement_payload: dict[str, Any],
) -> None:
    if row["pipeline"] != "bioseq":
        if value is not None:
            raise ApprovedTupleProofError(
                f"{label} gives isolated minimap2 a PostgreSQL Gate-8 proof")
        return
    proof = _require_dict(value, label)
    version = _require_int(proof.get("version"), f"{label}.version")
    if version == 1:
        _validate_approved_gate8_sql_v1(proof, row, label)
        return
    if version == 2:
        _validate_approved_gate8_sql_v2(
            proof, row, label, build_payload, fixture_payload,
            config_payload, measurement_payload)
        return
    raise ApprovedTupleProofError(f"{label} has an unsupported version")


def _validate_approved_measurement_proof(
        payload: dict[str, Any], candidate_rows: list[dict[str, Any]],
        timing_scope: str, *, build_payload: dict[str, Any],
        fixture_payload: dict[str, Any], config_payload: dict[str, Any],
        remote_host: dict[str, Any] | None = None) -> None:
    expected_fields = {
        "timing_scope", "clock", "unit", "cigar_projection", "mapq_projection",
        "prefilter", "interval_receipt_complete", "equal_nanosecond_rate",
        "device", "driver", "rows",
    }
    if remote_host is not None:
        expected_fields.add("remote_host_descriptor_sha256")
    _require_exact_keys(
        payload, expected_fields, "measurement-contract proof payload")
    if (payload.get("timing_scope") != timing_scope
            or payload.get("clock") != "monotonic"
            or payload.get("unit") != "ns"
            or payload.get("cigar_projection") is not True
            or payload.get("mapq_projection") is not True
            or payload.get("prefilter") is not False
            or payload.get("interval_receipt_complete") is not True
            or payload.get("equal_nanosecond_rate") is not True):
        raise ApprovedTupleProofError("measurement contract is not the approved A/union/B contract")
    for field in ("device", "driver"):
        identity = _require_dict(payload.get(field), f"measurement contract {field}")
        _require_exact_keys(identity, {"identity", "version"},
                            f"measurement contract {field}")
        _require_string(identity.get("identity"), f"measurement contract {field} identity")
        _require_string(identity.get("version"), f"measurement contract {field} version")
    if remote_host is not None and (
            payload.get("remote_host_descriptor_sha256")
                != remote_host["descriptor_sha256"]
            or payload.get("device") != remote_host["measurement_device"]):
        raise ApprovedTupleProofError(
            "measurement contract does not bind the remote authority host")
    rows = _approved_manifest_rows(
        payload, "rows", candidate_rows,
        "measurement-contract proof")
    gate8_record_owners: set[tuple[str, str]] = set()
    gate8_source_owners: set[tuple[str, str]] = set()
    gate8_epoch_owners: set[tuple[str, int, str]] = set()
    for index, (proof_row, candidate_row) in enumerate(zip(rows, candidate_rows)):
        _require_exact_keys(proof_row, {
            "stable_key", "timing_scope", "requested_algorithms", "observed_algorithms",
            "effective_path", "execution_conformance", "backend_declines",
            "backend_fallbacks", "prefilter_activity_count",
            "physical_stage_conformance", "gate8_sql",
        }, f"measurement-contract proof rows[{index}]")
        base_projection = dict(proof_row)
        gate8_sql = base_projection.pop("gate8_sql")
        if base_projection != _approved_measurement_projection(candidate_row):
            raise ApprovedTupleProofError(
                f"measurement-contract proof rows[{index}] differs from physical path")
        _validate_approved_gate8_sql(
            gate8_sql, candidate_row,
            f"measurement-contract proof rows[{index}].gate8_sql",
            build_payload=build_payload,
            fixture_payload=fixture_payload,
            config_payload=config_payload,
            measurement_payload=payload)
        if (isinstance(gate8_sql, dict)
                and gate8_sql.get("version") == 2):
            qualification = gate8_sql["qualification"]
            key = qualification["qualification_key"]
            report_sha256 = key["report_sha256"]
            source_owner = (
                report_sha256, qualification["source_record_checksum"])
            if source_owner in gate8_source_owners:
                raise ApprovedTupleProofError(
                    "Gate-8 v2 source record is replayed across candidate cells")
            gate8_source_owners.add(source_owner)
            for binding in gate8_sql["bindings"]:
                owner = (report_sha256, binding["record_checksum"])
                if owner in gate8_record_owners:
                    raise ApprovedTupleProofError(
                        "Gate-8 v2 current record is replayed across candidate cells")
                gate8_record_owners.add(owner)
                current_postgres = binding["current_postgres"]
                epoch_owner = (
                    report_sha256,
                    current_postgres["database_oid"],
                    current_postgres["postmaster_start_time"])
                if epoch_owner in gate8_epoch_owners:
                    raise ApprovedTupleProofError(
                        "Gate-8 v2 cold PostgreSQL epoch is replayed across cells")
                gate8_epoch_owners.add(epoch_owner)


def _validate_approved_chunk_proof(
        payload: dict[str, Any], candidate_rows: list[dict[str, Any]],
        timing_scope: str, *,
        remote_baselines: dict[str, str] | None = None) -> None:
    _require_exact_keys(
        payload, {"timing_scope", "rows"}, "chunk-plan proof payload")
    if payload.get("timing_scope") != timing_scope:
        raise ApprovedTupleProofError("chunk-plan proof timing_scope changed")
    rows = _approved_manifest_rows(
        payload, "rows", candidate_rows,
        "chunk-plan proof")
    replay_keys: set[str] = set()
    for row_index, (proof_row, candidate_row) in enumerate(zip(rows, candidate_rows)):
        label = f"chunk-plan proof rows[{row_index}]"
        expected_fields = {
            "stable_key", "timing_scope", "prior_disposition", "repeats",
        }
        if remote_baselines is not None:
            expected_fields.add("baseline_stable_key")
        _require_exact_keys(proof_row, expected_fields, label)
        if (remote_baselines is not None
                and proof_row.get("baseline_stable_key")
                    != remote_baselines.get(candidate_row["stable_key"])):
            raise ApprovedTupleProofError(
                f"{label} count vectors do not bind the same-host baseline")
        prior_disposition = proof_row.get("prior_disposition")
        if prior_disposition not in {"missing", "rejected", "invalid"}:
            raise ApprovedTupleProofError(
                f"{label} must identify a previously nonapproved matrix cell")
        expected_prior = "missing"
        if prior_disposition != expected_prior:
            raise ApprovedTupleProofError(
                f"{label} prior disposition disagrees with observation-pair absence")
        repeats = _require_list(proof_row.get("repeats"), f"{label}.repeats")
        if len(repeats) != MIN_ACCEPTED_REPEATS:
            raise ApprovedTupleProofError(f"{label} must contain exactly five repeats")
        a_values: list[int] = []
        b_values: list[int] = []
        query_partition_signature: tuple[str, str, int] | None = None
        for repeat_index, raw_repeat in enumerate(repeats):
            repeat = _require_dict(raw_repeat, f"{label}.repeats[{repeat_index}]")
            _require_exact_keys(repeat, {
                "repeat", "repeat_identity", "query_partition", "A_ns",
                "union_ns", "B_ns",
                "candidate_match_vector_sha256", "baseline_match_vector_sha256",
                "candidate_match_vector_count", "baseline_match_vector_count",
                "candidate_match_total", "baseline_match_total", "valid",
            }, f"{label}.repeats[{repeat_index}]")
            ordinal = _require_int(repeat.get("repeat"),
                                   f"{label}.repeats[{repeat_index}].repeat")
            repeat_identity = _require_string(
                repeat.get("repeat_identity"),
                f"{label}.repeats[{repeat_index}].repeat_identity")
            expected_repeat_identity = (
                f"{candidate_row['stable_key']}:timing_scope:"
                f"{candidate_row['timing_scope']}:repeat:{ordinal}")
            if (ordinal != repeat_index
                    or repeat_identity != expected_repeat_identity
                    or repeat_identity in replay_keys):
                raise ApprovedTupleProofError(
                    f"{label} repeat order or in-envelope replay is invalid")
            replay_keys.add(repeat_identity)
            partition = _require_dict(
                repeat.get("query_partition"),
                f"{label}.repeats[{repeat_index}].query_partition")
            _require_exact_keys(partition, {"identity", "sha256", "query_count"},
                                f"{label} query partition")
            _require_string(partition.get("identity"), f"{label} query partition identity")
            _require_hex(partition.get("sha256"), f"{label} query partition SHA-256", 64)
            if partition.get("query_count") != candidate_row["query_count"]:
                raise ApprovedTupleProofError(
                    f"{label} query partition count differs from candidate row")
            current_partition = (
                partition["identity"], partition["sha256"], partition["query_count"])
            if query_partition_signature is None:
                query_partition_signature = current_partition
            elif current_partition != query_partition_signature:
                raise ApprovedTupleProofError(
                    f"{label} repeats do not share one immutable query partition")
            a_ns = _require_int(repeat.get("A_ns"), f"{label} A_ns", minimum=1)
            union_ns = _require_int(
                repeat.get("union_ns"), f"{label} union_ns")
            b_ns = _require_int(repeat.get("B_ns"), f"{label} B_ns")
            if union_ns > a_ns or b_ns != a_ns - union_ns:
                raise ApprovedTupleProofError(f"{label} does not satisfy B=A-union")
            candidate_digest = _require_hex(
                repeat.get("candidate_match_vector_sha256"),
                f"{label} candidate match-vector SHA-256", 64)
            baseline_digest = _require_hex(
                repeat.get("baseline_match_vector_sha256"),
                f"{label} baseline match-vector SHA-256", 64)
            candidate_vector_count = _require_int(
                repeat.get("candidate_match_vector_count"),
                f"{label} candidate match-vector count")
            baseline_vector_count = _require_int(
                repeat.get("baseline_match_vector_count"),
                f"{label} baseline match-vector count")
            candidate_total = _require_int(
                repeat.get("candidate_match_total"), f"{label} candidate match total")
            baseline_total = _require_int(
                repeat.get("baseline_match_total"), f"{label} baseline match total")
            if (candidate_digest != baseline_digest
                    or candidate_vector_count != candidate_row["query_count"]
                    or baseline_vector_count != candidate_row["query_count"]
                    or candidate_total != baseline_total
                    or repeat.get("valid") is not True):
                raise ApprovedTupleProofError(f"{label} lacks complete match-count parity")
            a_values.append(a_ns)
            b_values.append(b_ns)
        if (_nearest_rank_p50(a_values) != candidate_row["metrics"]["p50_a"]
                or _nearest_rank_p50(b_values) != candidate_row["metrics"]["p50_b"]):
            raise ApprovedTupleProofError(
                f"{label} repeat observations do not reproduce published P50s")


def _validate_approved_verification_manifest(
        raw_manifest: Any, candidate_rows: list[dict[str, Any]],
        campaign: dict[str, Any], timing_scope: str, *,
        remote_host: dict[str, Any] | None = None,
        remote_baselines: dict[str, str] | None = None) -> None:
    manifest = _require_dict(raw_manifest, "append verification_manifest")
    _require_exact_keys(
        manifest, {"schema", "version", "timing_scope", "proofs", "row_bindings"},
        "append verification_manifest")
    if (manifest.get("schema") != APPROVED_TUPLES_MANIFEST_SCHEMA
            or manifest.get("version") != APPROVED_TUPLES_MANIFEST_VERSION
            or manifest.get("timing_scope") != timing_scope):
        raise ContractError("append verification_manifest schema/version changed")
    proofs = _require_dict(manifest.get("proofs"), "append verification_manifest.proofs")
    _require_exact_keys(proofs, set(APPROVED_TUPLES_PROOF_CATEGORIES),
                        "append verification_manifest.proofs")
    resolved: dict[str, tuple[str, int]] = {}
    identities: set[tuple[str, int]] = set()
    payloads: dict[str, dict[str, Any]] = {}
    try:
        for category in APPROVED_TUPLES_PROOF_CATEGORIES:
            proof = _require_dict(proofs.get(category), f"{category} proof")
            _require_exact_keys(proof, {"identity", "version", "sha256", "payload"},
                                f"{category} proof")
            identity = _require_string(proof.get("identity"), f"{category} proof identity")
            if _approved_proof_has_external_reference(identity):
                raise ApprovedTupleProofError(
                    f"{category} proof identity is an external reference")
            version = _require_int(proof.get("version"), f"{category} proof version", minimum=1)
            digest = _require_hex(proof.get("sha256"), f"{category} proof SHA-256", 64)
            payload = _require_dict(proof.get("payload"), f"{category} proof payload")
            if _approved_proof_has_external_reference(payload):
                raise ApprovedTupleProofError(
                    f"{category} proof contains an external or unsealed reference")
            if _sha256(_canonical_bytes(payload)) != digest:
                raise ApprovedTupleProofError(f"{category} proof seal does not recompute")
            if (identity, version) in identities:
                raise ApprovedTupleProofError(
                    "proof identity/version is ambiguous across categories")
            identities.add((identity, version))
            resolved[category] = (identity, version)
            payloads[category] = payload
        _validate_approved_source_proof(payloads["source"], timing_scope)
        _validate_approved_build_proof(
            payloads["build"], payloads["source"], timing_scope,
            remote_host=remote_host)
        _validate_approved_fixture_proof(
            payloads["fixture"], candidate_rows, timing_scope)
        _validate_approved_config_proof(
            payloads["config"], candidate_rows, campaign, timing_scope)
        _validate_approved_measurement_proof(
            payloads["measurement_contract"], candidate_rows, timing_scope,
            build_payload=payloads["build"],
            fixture_payload=payloads["fixture"],
            config_payload=payloads["config"], remote_host=remote_host)
        _validate_approved_chunk_proof(
            payloads["chunk_plan"], candidate_rows, timing_scope,
            remote_baselines=remote_baselines)
    except ApprovedTupleProofError:
        raise
    except ContractError as exc:
        raise ApprovedTupleProofError(str(exc)) from exc
    try:
        bindings = _require_list(manifest.get("row_bindings"),
                                 "append verification_manifest.row_bindings")
        if [
                (item.get("stable_key"), item.get("timing_scope"))
                for item in bindings if isinstance(item, dict)] != [
                    _approved_observation_identity(row) for row in candidate_rows]:
            raise ApprovedTupleProofError(
                "row bindings are not sorted one-for-one with candidates")
        for index, raw_binding in enumerate(bindings):
            binding = _require_dict(raw_binding, f"row_bindings[{index}]")
            _require_exact_keys(binding, {"stable_key", "timing_scope", "proofs"},
                                f"row_bindings[{index}]")
            proof_bindings = _require_dict(binding.get("proofs"),
                                           f"row_bindings[{index}].proofs")
            _require_exact_keys(proof_bindings, set(APPROVED_TUPLES_PROOF_CATEGORIES),
                                f"row_bindings[{index}].proofs")
            for category in APPROVED_TUPLES_PROOF_CATEGORIES:
                reference = _require_dict(
                    proof_bindings.get(category),
                    f"row_bindings[{index}].proofs.{category}")
                _require_exact_keys(reference, {"identity", "version"},
                                    f"row_bindings[{index}].proofs.{category}")
                if ((reference.get("identity"), reference.get("version"))
                        != resolved[category]):
                    raise ApprovedTupleProofError(
                        f"row_bindings[{index}] does not resolve {category} exactly")
    except ApprovedTupleProofError:
        raise
    except ContractError as exc:
        raise ApprovedTupleProofError(str(exc)) from exc


def _validate_approved_append_candidate_document(
        candidate: dict[str, Any], authority: dict[str, Any],
        authority_sha256: str) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    remote_authority = _is_remote_approved_authority(authority)
    expected_candidate_fields = {
        "schema", "version", "authority_before_sha256", "stable_key_schema",
        "timing_scope", "candidate_rows", "verification_manifest",
        "candidate_projection_sha256", "candidate_envelope_sha256",
    }
    if remote_authority:
        expected_candidate_fields.update({
            "remote_host_binding", "same_host_baselines"})
    _require_exact_keys(
        candidate, expected_candidate_fields, "approved tuple append candidate")
    timing_scope = candidate.get("timing_scope")
    if (candidate.get("schema") != APPROVED_TUPLES_CANDIDATE_SCHEMA
            or candidate.get("version") != (
                APPROVED_TUPLES_REMOTE_CANDIDATE_VERSION
                if remote_authority else APPROVED_TUPLES_CANDIDATE_VERSION)
            or candidate.get("stable_key_schema") != "bioseq-e2e-cell-v2"
            or timing_scope != APPROVED_TUPLES_QUERY_TIMING_SCOPE):
        raise ContractError("append candidate schema/version/stable-key schema changed")
    try:
        before = _require_hex(candidate.get("authority_before_sha256"),
                              "candidate authority_before_sha256", 64)
        envelope = _require_hex(candidate.get("candidate_envelope_sha256"),
                                "candidate envelope SHA-256", 64)
    except ContractError as exc:
        raise ApprovedTupleProofError(str(exc)) from exc
    if before != authority_sha256:
        raise ApprovedTupleProofError("candidate is bound to a different authority seal")
    envelope_value = dict(candidate)
    del envelope_value["candidate_envelope_sha256"]
    if _sha256(_canonical_bytes(envelope_value)) != envelope:
        raise ApprovedTupleProofError("candidate envelope seal does not recompute")
    merge_base = (copy.deepcopy(authority) if remote_authority else
                  _migrate_approved_tuple_authority_v3(
                      authority, authority_sha256))
    campaign = _require_dict(
        merge_base.get("campaign_identity"), "campaign_identity")
    matrix = _require_dict(merge_base.get("matrix_scope"), "matrix_scope")
    raw_candidate_rows = _require_list(candidate.get("candidate_rows"), "candidate_rows")
    if not raw_candidate_rows:
        raise ContractError("candidate_rows must be one nonempty sequence")
    candidate_rows = [
        _validate_approved_tuple_row(
            raw_row, campaign["campaign_id"], matrix, f"candidate_rows[{index}]",
            allow_legacy_preservation=False, require_field_order=False,
            authority_version=APPROVED_TUPLES_VERSION)
        for index, raw_row in enumerate(raw_candidate_rows)
    ]
    remote_host: dict[str, Any] | None = None
    remote_baselines: dict[str, str] | None = None
    if remote_authority:
        remote_host = _validate_remote_host_descriptor(
            merge_base.get("remote_host_binding"),
            "remote authority host binding", require_field_order=True)
        raw_remote_binding = _require_dict(
            candidate.get("remote_host_binding"),
            "candidate remote_host_binding")
        _require_exact_keys(
            raw_remote_binding, set(APPROVED_TUPLES_REMOTE_BINDING_FIELDS),
            "candidate remote_host_binding")
        expected_receipt_host_sha256 = _sha256(
            _canonical_bytes(remote_host["receipt_host"]))
        if raw_remote_binding != {
                "machine_id": remote_host["machine_id"],
                "host_descriptor_sha256": remote_host["descriptor_sha256"],
                "receipt_host_sha256": expected_receipt_host_sha256,
        }:
            raise ApprovedTupleProofError(
                "candidate is bound to a different remote host")
        eligible_keys = set(_require_list(
            merge_base.get("eligible_stable_keys"), "eligible_stable_keys"))
        unavailable_keys = {
            item["stable_key"] for item in _require_list(
                merge_base.get("hardware_unavailable_cells"),
                "hardware_unavailable_cells")}
        for index, row in enumerate(candidate_rows):
            if (row["stable_key"] not in eligible_keys
                    or row["stable_key"] in unavailable_keys
                    or row["timing_scope"]
                        != APPROVED_TUPLES_QUERY_TIMING_SCOPE
                    or row["execution_conformance"] not in {
                        "exact_requested_path", "external_minimap2_baseline"}):
                raise ApprovedTupleProofError(
                    f"candidate_rows[{index}] is not admitted by the remote allowlist")
    if any(row["timing_scope"] != timing_scope for row in candidate_rows):
        raise ContractError("candidate_rows timing_scope differs from its envelope")
    candidate_identities = [
        _approved_observation_identity(row) for row in candidate_rows]
    if candidate_identities != sorted(candidate_identities):
        raise ApprovedTupleDuplicateError(
            "candidate_rows is not sorted by observation identity")
    if len(candidate_identities) != len(set(candidate_identities)):
        raise ApprovedTupleDuplicateError(
            "candidate_rows repeats an observation identity")
    existing_rows = _require_list(
        merge_base.get("approved_results"), "approved_results")
    existing_identities = {
        _approved_observation_identity(row) for row in existing_rows}
    duplicate = sorted(existing_identities.intersection(candidate_identities))
    if duplicate:
        raise ApprovedTupleDuplicateError(
            f"candidate observation identity already exists: {duplicate[0]}")
    if remote_authority:
        raw_baselines = _require_list(
            candidate.get("same_host_baselines"), "same_host_baselines")
        if len(raw_baselines) != len(candidate_rows):
            raise ApprovedTupleProofError(
                "same_host_baselines must bind every candidate row")
        existing_keys = {row["stable_key"] for row in existing_rows}
        observed_bindings: list[tuple[str, str]] = []
        remote_baselines = {}
        for index, (raw_binding, row) in enumerate(
                zip(raw_baselines, candidate_rows)):
            binding = _require_dict(
                raw_binding, f"same_host_baselines[{index}]")
            _require_exact_keys(
                binding,
                set(APPROVED_TUPLES_REMOTE_BASELINE_BINDING_FIELDS),
                f"same_host_baselines[{index}]")
            expected_baseline = _remote_baseline_stable_key(row)
            if (binding.get("candidate_stable_key") != row["stable_key"]
                    or binding.get("baseline_stable_key") != expected_baseline
                    or binding.get("host_descriptor_sha256")
                        != remote_host["descriptor_sha256"]):
                raise ApprovedTupleProofError(
                    f"same_host_baselines[{index}] does not bind the exact control")
            if (row["pipeline"] == "minimap2"
                    and expected_baseline != row["stable_key"]):
                raise ApprovedTupleProofError(
                    "external minimap2 baseline does not self-bind")
            if (row["pipeline"] == "bioseq"
                    and expected_baseline not in existing_keys):
                raise ApprovedTupleProofError(
                    f"candidate_rows[{index}] requires its previously approved "
                    "same-host minimap2 baseline")
            observed_bindings.append(
                (binding["candidate_stable_key"], binding["baseline_stable_key"]))
            remote_baselines[row["stable_key"]] = binding[
                "baseline_stable_key"]
        if observed_bindings != sorted(observed_bindings):
            raise ApprovedTupleProofError(
                "same_host_baselines is not in candidate-row order")
    merged_rows = sorted(
        [copy.deepcopy(row) for row in existing_rows]
        + [copy.deepcopy(row) for row in candidate_rows],
        key=_approved_observation_sort_key,
    )
    try:
        projection = _require_hex(candidate.get("candidate_projection_sha256"),
                                  "candidate projection SHA-256", 64)
    except ContractError as exc:
        raise ApprovedTupleProofError(str(exc)) from exc
    if _approved_projection_sha256(merged_rows) != projection:
        raise ApprovedTupleProofError("candidate post-merge projection seal does not recompute")
    _validate_approved_verification_manifest(
        candidate.get("verification_manifest"), candidate_rows, campaign,
        timing_scope, remote_host=remote_host,
        remote_baselines=remote_baselines)
    return candidate_rows, merged_rows


@contextlib.contextmanager
def _exclusive_results_lock(
        path: Path, *, approved_tuples: bool = False,
        approved_target_must_exist: bool = True) -> Iterable[int]:
    """Serialize the complete read/validate/merge/replace state transaction."""
    if approved_tuples:
        _approved_tuple_path(
            path, "approved tuple lock target",
            must_exist=approved_target_must_exist)
        error_type: type[ContractError] = ApprovedTupleIOError
    else:
        _verified_repository_file(path, f"{EXPECTED_LEDGER_PATH} lock target")
        error_type = ContractError
    lock_path = path.parent / f".{path.name}.lock"
    if approved_tuples:
        try:
            lock_metadata = lock_path.lstat()
        except FileNotFoundError:
            pass
        except OSError as exc:
            raise ApprovedTupleIOError(
                f"cannot inspect aggregation lock {lock_path}: {exc}") from exc
        else:
            if stat.S_ISLNK(lock_metadata.st_mode):
                raise ApprovedTupleIOError(
                    f"aggregation lock may not be a symlink: {lock_path}")
    flags = os.O_RDWR | os.O_CREAT
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(lock_path, flags, 0o600)
    except OSError as exc:
        raise error_type(f"cannot open aggregation lock {lock_path}: {exc}") from exc
    locked = False
    try:
        metadata = os.fstat(descriptor)
        if (not stat.S_ISREG(metadata.st_mode)
                or metadata.st_nlink != 1
                or metadata.st_uid != os.geteuid()
                or (stat.S_IMODE(metadata.st_mode) != 0o600
                    if approved_tuples else bool(metadata.st_mode & 0o077))):
            raise error_type(
                "aggregation lock must be one owner-only regular file")
        while True:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX)
                locked = True
                break
            except InterruptedError:
                continue
            except OSError as exc:
                raise error_type(f"cannot acquire aggregation lock: {exc}") from exc
        if approved_tuples and os.fstat(descriptor).st_size != 0:
            raise ApprovedTupleIOError(
                "approved tuple publication requires recovery: the locked "
                "publication marker is nonempty; no append was attempted")
        yield descriptor
    finally:
        try:
            if locked:
                fcntl.flock(descriptor, fcntl.LOCK_UN)
        finally:
            os.close(descriptor)


def _approved_tuple_receipt(raw: bytes, document: dict[str, Any]) -> dict[str, Any]:
    rows = _require_list(document.get("approved_results"), "approved_results")
    if _is_remote_approved_authority(document):
        host = _require_dict(
            document.get("remote_host_binding"), "remote_host_binding")
        unavailable = _require_list(
            document.get("hardware_unavailable_cells"),
            "hardware_unavailable_cells")
        return {
            "byte_count": len(raw),
            "lf_terminated_line_count": raw.count(b"\n"),
            "sha256": _sha256(raw),
            "approved_results_projection_sha256":
                _approved_projection_sha256(rows),
            "approved_result_count": len(rows),
            "approved_run_count": sum(row["run_count"] for row in rows),
            "hardware_unavailable_count": len(unavailable),
            "machine_id": host["machine_id"],
            "host_descriptor_sha256": host["descriptor_sha256"],
        }
    timing_scoped = document.get("version") == APPROVED_TUPLES_VERSION
    return {
        "byte_count": len(raw),
        "lf_terminated_line_count": raw.count(b"\n"),
        "sha256": _sha256(raw),
        "approved_results_projection_sha256": _approved_projection_sha256(
            rows),
        **({
            "logical_tuple_count": len({row["stable_key"] for row in rows}),
            "timing_observation_count": len(rows),
            "query_only_observation_count": sum(
                row["timing_scope"] == APPROVED_TUPLES_QUERY_TIMING_SCOPE
                for row in rows),
            "historical_v2_projection_sha256": _approved_projection_sha256(
                _approved_historical_v2_rows(rows)),
        } if timing_scoped else {}),
    }


def _write_approved_publication_marker(
        descriptor: int, payload: bytes) -> None:
    """Durably update the already-held lock inode; never create another registry."""
    try:
        offset = 0
        while offset < len(payload):
            written = os.pwrite(descriptor, payload[offset:], offset)
            if written <= 0:
                raise OSError("publication marker write made no progress")
            offset += written
        os.ftruncate(descriptor, len(payload))
        os.fsync(descriptor)
    except OSError as exc:
        raise ApprovedTupleIOError(
            f"cannot persist approved publication recovery marker: {exc}") from exc


def _fsync_approved_publication_directory(path: Path) -> None:
    flags = os.O_RDONLY
    for option in ("O_DIRECTORY", "O_CLOEXEC", "O_NOFOLLOW"):
        flags |= getattr(os, option, 0)
    descriptor = os.open(path.parent, flags)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _atomic_bootstrap_remote_approved_tuples(
        path: Path, document: dict[str, Any], *,
        lock_descriptor: int) -> dict[str, Any]:
    """Create one remote authority without an overwrite-capable operation."""
    payload = _canonical_approved_tuples_yaml_v3(document)
    parsed = _parse_approved_tuples_yaml(payload, "generated remote authority")
    _validate_remote_approved_tuple_authority_document(parsed, payload)
    path = _approved_tuple_path(
        path, "remote approved tuple authority", must_exist=False)
    temporary: Path | None = None
    linked = False
    marker_started = False
    try:
        if path.exists():
            raise ApprovedTupleIOError(
                "remote approved tuple authority already exists")
        descriptor, name = tempfile.mkstemp(
            prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
        temporary = Path(name)
        try:
            os.fchmod(descriptor, 0o600)
            with os.fdopen(descriptor, "wb") as handle:
                descriptor = -1
                handle.write(payload)
                handle.flush()
                os.fsync(handle.fileno())
        finally:
            if descriptor >= 0:
                os.close(descriptor)
        temporary_raw = _read_approved_tuple_file(
            temporary, "remote authority bootstrap temporary")
        if temporary_raw != payload:
            raise ApprovedTupleIOError(
                "remote authority bootstrap temporary bytes changed")
        temporary_document = _parse_approved_tuples_yaml(
            temporary_raw, "remote authority bootstrap temporary")
        _validate_remote_approved_tuple_authority_document(
            temporary_document, temporary_raw)
        marker_started = True
        _write_approved_publication_marker(lock_descriptor, _canonical_bytes({
            "state": "remote_authority_bootstrap_pending_final_validation",
            "intended_after_sha256": _sha256(payload),
        }) + b"\n")
        _fsync_approved_publication_directory(path)
        try:
            os.link(temporary, path, follow_symlinks=False)
        except FileExistsError as exc:
            raise ApprovedTupleIOError(
                "remote authority appeared during bootstrap; no overwrite attempted") \
                from exc
        linked = True
        temporary.unlink()
        temporary = None
        _fsync_approved_publication_directory(path)
        final_raw = _read_approved_tuple_file(
            path, "final remote approved tuple authority")
        final_document = _parse_approved_tuples_yaml(
            final_raw, "final remote approved tuple authority")
        _validate_remote_approved_tuple_authority_document(
            final_document, final_raw)
        if final_raw != payload:
            raise ApprovedTupleIOError(
                "final remote authority differs from validated bootstrap bytes")
        receipt = _approved_tuple_receipt(final_raw, final_document)
        _write_approved_publication_marker(lock_descriptor, b"")
        return receipt
    except Exception as exc:
        if linked:
            observed = "unreadable"
            try:
                observed = _sha256(_read_approved_tuple_file(
                    path, "post-bootstrap remote authority"))
            except Exception:
                pass
            try:
                _write_approved_publication_marker(
                    lock_descriptor, _canonical_bytes({
                        "state": "fatal_remote_bootstrap_validation_failure",
                        "intended_after_sha256": _sha256(payload),
                        "observed_sha256": observed,
                    }) + b"\n")
            except ApprovedTupleIOError:
                pass
            if isinstance(exc, ApprovedTupleIOError):
                raise
            raise ApprovedTupleIOError(
                "remote authority was created but final validation failed; "
                f"observed_sha256={observed}") from exc
        if marker_started:
            _write_approved_publication_marker(lock_descriptor, b"")
        raise
    finally:
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


def _atomic_write_approved_tuples(
        path: Path, expected_before_sha256: str,
        document: dict[str, Any], *, candidate: dict[str, Any],
        prior_authority: dict[str, Any], lock_descriptor: int) -> dict[str, Any]:
    """Validate exact temporary bytes before one durable atomic replacement."""
    payload = _canonical_approved_tuples_yaml_v3(document)
    parsed_payload = _parse_approved_tuples_yaml(payload, "generated authority bytes")
    _validate_approved_tuple_authority_document(parsed_payload, payload)
    path = _approved_tuple_path(path, "approved tuple authority", must_exist=True)
    temporary: Path | None = None
    replaced = False
    marker_started = False
    try:
        try:
            descriptor, name = tempfile.mkstemp(
                prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
            temporary = Path(name)
            os.fchmod(descriptor, 0o600)
            metadata = os.fstat(descriptor)
            if (not stat.S_ISREG(metadata.st_mode)
                    or metadata.st_nlink != 1
                    or stat.S_IMODE(metadata.st_mode) != 0o600):
                raise ApprovedTupleIOError(
                    "approved tuple temporary is not one owner-only regular file")
            with os.fdopen(descriptor, "wb") as handle:
                descriptor = -1
                handle.write(payload)
                handle.flush()
                os.fsync(handle.fileno())
        except ApprovedTupleIOError:
            raise
        except OSError as exc:
            raise ApprovedTupleIOError(
                f"cannot create or fsync approved tuple temporary: {exc}") from exc
        finally:
            if 'descriptor' in locals() and descriptor >= 0:
                os.close(descriptor)

        if temporary is None:
            raise ApprovedTupleIOError(
                "approved tuple temporary creation returned no path")
        temporary_raw = _read_approved_tuple_file(
            temporary, "approved tuple temporary")
        if temporary_raw != payload:
            raise ContractError("approved tuple temporary bytes changed after fsync")
        temporary_document = _parse_approved_tuples_yaml(
            temporary_raw, "approved tuple temporary")
        _validate_approved_tuple_authority_document(
            temporary_document, temporary_raw)
        _, repeated_merge = _validate_approved_append_candidate_document(
            candidate, prior_authority, expected_before_sha256)
        if temporary_document.get("approved_results") != repeated_merge:
            raise ContractError(
                "approved tuple temporary differs from the revalidated candidate merge")
        current_raw = _read_approved_tuple_file(
            path, "approved tuple authority before atomic replace")
        current_sha256 = _sha256(current_raw)
        if current_sha256 != expected_before_sha256:
            raise ApprovedTupleProofError(
                "authority seal changed before replace: "
                f"expected={expected_before_sha256} observed={current_sha256}")
        try:
            # Persist intent before replace, including the directory entry of a
            # newly created lock. A crash in the replace/final-validation window
            # therefore also prevents a later process from blindly appending.
            marker_started = True
            _write_approved_publication_marker(lock_descriptor, _canonical_bytes({
                "state": "publication_pending_final_validation",
                "authority_before_sha256": expected_before_sha256,
                "intended_after_sha256": _sha256(payload),
            }) + b"\n")
            _fsync_approved_publication_directory(path)
            os.replace(temporary, path)
            replaced = True
            temporary = None
            _fsync_approved_publication_directory(path)
        except OSError as exc:
            raise ApprovedTupleIOError(
                f"approved tuple atomic replace or directory fsync failed: {exc}") from exc
        final_raw = _read_approved_tuple_file(path, "final approved tuple authority")
        final_document = _parse_approved_tuples_yaml(
            final_raw, "final approved tuple authority")
        _validate_approved_tuple_authority_document(final_document, final_raw)
        if final_raw != payload:
            raise ApprovedTupleIOError(
                "final approved tuple authority differs from validated temporary")
        receipt = _approved_tuple_receipt(final_raw, final_document)
        _write_approved_publication_marker(lock_descriptor, b"")
        return receipt
    except Exception as exc:
        if replaced:
            observed = "unreadable"
            try:
                observed = _sha256(_read_approved_tuple_file(
                    path, "post-replace approved tuple authority"))
            except Exception:
                pass
            try:
                _write_approved_publication_marker(lock_descriptor, _canonical_bytes({
                    "state": "fatal_post_replace_validation_failure",
                    "authority_before_sha256": expected_before_sha256,
                    "intended_after_sha256": _sha256(payload),
                    "observed_sha256": observed,
                }) + b"\n")
            except ApprovedTupleIOError as marker_error:
                # The pre-replace intent remains the fail-closed latch even
                # when recording the more precise observed seal fails.
                print(f"FATAL: cannot update recovery marker: {marker_error}",
                      file=sys.stderr)
            print(
                "FATAL: approved tuple replacement completed but final validation "
                f"failed; observed_sha256={observed}; no second rewrite attempted: {exc}",
                file=sys.stderr,
            )
            if isinstance(exc, ApprovedTupleIOError):
                raise
            raise ApprovedTupleIOError(
                f"post-replace validation failed; observed_sha256={observed}") from exc
        if marker_started:
            # Only a positively unchanged authority permits clearing intent
            # after a pre-replace I/O failure. Otherwise retain it for recovery.
            current = _read_approved_tuple_file(path, "failed pre-replace authority")
            if _sha256(current) == expected_before_sha256:
                _write_approved_publication_marker(lock_descriptor, b"")
        raise
    finally:
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass
            except OSError as exc:
                raise ApprovedTupleIOError(
                    f"cannot remove failed approved tuple temporary: {exc}") from exc


def bootstrap_remote_approved_tuples(
        authority_path: Path, source_authority_path: Path,
        expected_source_sha256: str,
        host_descriptor_path: Path, candidate_path: Path) -> dict[str, Any]:
    authority_path = _approved_tuple_path(
        authority_path, "remote approved tuple authority", must_exist=False)
    if authority_path.exists():
        raise ApprovedTupleIOError(
            "remote approved tuple authority already exists")
    source, source_raw = _load_approved_tuple_authority(
        source_authority_path, expected_source_sha256)
    if _sha256(source_raw) != expected_source_sha256:
        raise ApprovedTupleProofError("source authority seal changed")
    descriptor = _load_remote_host_descriptor(host_descriptor_path)
    policy = APPROVED_TUPLES_REMOTE_MACHINE_POLICIES[
        descriptor["machine_id"]]
    if authority_path.name != policy["authority_filename"]:
        raise ApprovedTupleIOError(
            "remote authority filename differs from the machine policy")
    seed_document = _new_remote_approved_tuple_authority(
        source, expected_source_sha256, descriptor)
    bootstrap_seed_sha256 = _remote_bootstrap_seed_sha256(seed_document)
    candidate, _ = _load_approved_append_candidate(candidate_path)
    candidate_rows, merged_rows = _validate_approved_append_candidate_document(
        candidate, seed_document, bootstrap_seed_sha256)
    if (len(candidate_rows) != 1
            or candidate_rows[0]["pipeline"] != "minimap2"
            or candidate_rows[0]["execution_conformance"]
                != "external_minimap2_baseline"):
        raise ApprovedTupleProofError(
            "remote bootstrap candidate must contain exactly one host-local "
            "external minimap2 baseline")
    document = copy.deepcopy(seed_document)
    _regenerate_remote_approved_tuple_projections(document, merged_rows)
    _validate_remote_approved_tuple_authority_document(document)
    with _exclusive_results_lock(
            authority_path, approved_tuples=True,
            approved_target_must_exist=False) as lock_descriptor:
        return _atomic_bootstrap_remote_approved_tuples(
            authority_path, document, lock_descriptor=lock_descriptor)


def validate_approved_tuples(
        authority_path: Path, expected_sha256: str) -> dict[str, Any]:
    document, raw = _load_approved_tuple_authority(
        authority_path, expected_sha256)
    return _approved_tuple_receipt(raw, document)


def validate_approved_append_candidate(
        authority_path: Path, expected_sha256: str,
        candidate_path: Path) -> dict[str, Any]:
    if authority_path.absolute() == candidate_path.absolute():
        raise ApprovedTupleIOError("candidate path may not alias the authority path")
    authority, authority_raw = _load_approved_tuple_authority(
        authority_path, expected_sha256)
    candidate, candidate_raw = _load_approved_append_candidate(candidate_path)
    candidate_rows, merged_rows = _validate_approved_append_candidate_document(
        candidate, authority, _sha256(authority_raw))
    return {
        "authority_sha256": _sha256(authority_raw),
        "candidate_sha256": _sha256(candidate_raw),
        "candidate_envelope_sha256": candidate["candidate_envelope_sha256"],
        "candidate_row_count": len(candidate_rows),
        "post_merge_row_count": len(merged_rows),
        "post_merge_logical_tuple_count": len({
            row["stable_key"] for row in merged_rows}),
        "post_merge_query_only_observation_count": sum(
            row["timing_scope"] == APPROVED_TUPLES_QUERY_TIMING_SCOPE
            for row in merged_rows),
        "post_merge_projection_sha256": _approved_projection_sha256(merged_rows),
    }


def append_approved_tuples(
        authority_path: Path, expected_sha256: str,
        candidate_path: Path) -> dict[str, Any]:
    authority_path = _approved_tuple_path(
        authority_path, "approved tuple authority", must_exist=True)
    candidate_path = _approved_tuple_path(
        candidate_path, "approved tuple append candidate", must_exist=True)
    if authority_path == candidate_path:
        raise ApprovedTupleIOError("candidate path may not alias the authority path")
    with _exclusive_results_lock(authority_path, approved_tuples=True) as lock_descriptor:
        authority, authority_raw = _load_approved_tuple_authority(
            authority_path, expected_sha256)
        candidate, _ = _load_approved_append_candidate(candidate_path)
        _, merged_rows = _validate_approved_append_candidate_document(
            candidate, authority, _sha256(authority_raw))
        if _is_remote_approved_authority(authority):
            merged = copy.deepcopy(authority)
            _regenerate_remote_approved_tuple_projections(merged, merged_rows)
        else:
            merged = _migrate_approved_tuple_authority_v3(
                authority, _sha256(authority_raw))
            _regenerate_approved_tuple_projections(merged, merged_rows)
        _validate_approved_tuple_authority_document(merged)
        return _atomic_write_approved_tuples(
            authority_path, _sha256(authority_raw), merged,
            candidate=candidate, prior_authority=authority,
            lock_descriptor=lock_descriptor)


def _atomic_write(path: Path, document: dict[str, Any]) -> None:
    _verified_repository_file(
        path, f"{EXPECTED_LEDGER_PATH} before atomic rewrite")
    payload = (json.dumps(document, indent=2, ensure_ascii=True, allow_nan=False)
               + "\n").encode("utf-8")
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        _verified_repository_file(
            path, f"{EXPECTED_LEDGER_PATH} before atomic replace")
        os.replace(temporary, path)
        directory_fd = os.open(path.parent, os.O_RDONLY)
        try:
            try:
                os.fsync(directory_fd)
            except OSError as exc:
                if exc.errno not in {
                        errno.EINVAL,
                        getattr(errno, "ENOTSUP", errno.EINVAL),
                        errno.EBADF,
                }:
                    raise
        finally:
            os.close(directory_fd)
    finally:
        if temporary.exists():
            temporary.unlink()


def merge(
        report_path: Path,
        config_path: Path,
        all_results_path: Path,
        run_receipt_id: str | None = None,
) -> bool:
    if all_results_path != REPOSITORY_ROOT / EXPECTED_LEDGER_PATH:
        raise ContractError(
            f"active campaign requires exact ledger {EXPECTED_LEDGER_PATH}")
    with _exclusive_results_lock(all_results_path):
        campaign_contract = _load_campaign_contract()
        document, _ = _read_json_file(all_results_path, EXPECTED_LEDGER_PATH)
        original_document = copy.deepcopy(document)
        _upgrade_q1024_ledger_contract(document, campaign_contract)
        _refresh_q1024_partition_binding(document, campaign_contract)
        _refresh_q8_ledger_proof_binding(document, campaign_contract)
        _validate_all_results(document, campaign_contract)
        preservation_rows = (
            _q1024_s059_preservation_rows(document)
            if ACTIVE_CAMPAIGN_PROFILE == "q1024" else [])
        state = _require_dict(document["aggregation_state"], "aggregation_state")
        build_receipts = (
            _require_list(
                state.get("build_receipts"), "aggregation_state.build_receipts")
            if ACTIVE_CAMPAIGN_PROFILE == "q1024" else [])
        build_receipts_by_id = {
            receipt["build_receipt_id"]: receipt
            for receipt in build_receipts
        }
        existing_records, processed = _load_existing_reports(
            document, campaign_contract, build_receipts_by_id)

        receipt_context = None
        if ACTIVE_CAMPAIGN_PROFILE == "q1024":
            if (not processed
                    and _relative(config_path)
                        != campaign_contract["deterministic_first_ingestion"]):
                raise ContractError(
                    "the empty q1024 ledger must start with deterministic "
                    "adopted baseline "
                    f"{campaign_contract['deterministic_first_ingestion']}")
            if run_receipt_id is None:
                raise ContractError(
                    "q1024 hybrid ingestion requires --run-receipt-id")
            receipt_config_path = config_path
            selected_entry = campaign_contract["configs_by_path"].get(config_path)
            if (isinstance(selected_entry, dict)
                    and selected_entry.get("execution_authority")
                        == "q1024_deadline_v2_adoption"):
                adoption = _require_dict(
                    selected_entry.get("sealed_evidence"),
                    "q1024 adoption evidence")
                receipt_config = _require_dict(
                    adoption.get("run_receipt_config"),
                    "q1024 adoption run-receipt config")
                receipt_config_path = _canonical_repository_file(
                    _require_string(
                        receipt_config.get("path"),
                        "q1024 adoption run-receipt config path"),
                    "q1024 adoption run-receipt config")
            receipt_context = _load_live_run_receipt_context(
                run_receipt_id, report_path, config_path,
                receipt_config_path=receipt_config_path)
        elif run_receipt_id is not None:
            raise ContractError("--run-receipt-id is reserved for q1024 runs")
        incoming_records, incoming_source = _load_report(
            report_path, config_path, campaign_contract, receipt_context)
        if receipt_context is not None:
            incoming_build = receipt_context[1]
            build_id = incoming_build["build_receipt_id"]
            prior_build = build_receipts_by_id.get(build_id)
            if prior_build is not None and prior_build != incoming_build:
                raise ContractError(
                    "incoming run receipt conflicts with a sealed build snapshot")
            if prior_build is None:
                build_receipts.append(copy.deepcopy(incoming_build))
                build_receipts_by_id[build_id] = incoming_build
        incoming_already_processed = False
        for source in processed:
            if source["report"] == incoming_source["report"]:
                if source != incoming_source:
                    raise ContractError("registered report path now has conflicting evidence")
                incoming_already_processed = True
                continue
            if source["report_sha256"] == incoming_source["report_sha256"]:
                raise ContractError(
                    "incoming report duplicates existing bytes under another path")

        merged_records = list(existing_records)
        if not incoming_already_processed:
            merged_records.extend(incoming_records)
            processed.append(incoming_source)
        if any(item["payload_schema_version"] == RAW_PAYLOAD_SCHEMA_VERSION
               for item in merged_records):
            document["contract"] = _current_all_results_contract_for_document(
                campaign_contract, document)
        merged_rows = _aggregate(merged_records)
        if ACTIVE_CAMPAIGN_PROFILE == "q1024":
            merged_rows = _merge_q1024_preservation_rows(
                merged_rows, preservation_rows)
        statuses = [row["status"] for row in merged_rows]
        expected_cells = campaign_contract["canonical_cells"]
        actual_keys = {row["stable_key"] for row in merged_rows}
        expected_keys = set(expected_cells)
        complete_coverage = (
            len(merged_rows) == EXPECTED_RESULT_CELLS
            and actual_keys == expected_keys)
        canonical_statuses = True
        for row in merged_rows:
            descriptor = expected_cells.get(row["stable_key"])
            if descriptor is None or any(
                    row.get(field) != value
                    for field, value in descriptor["identity"].items()):
                raise ContractError("aggregated row identity is not canonical")
            if row["status"] not in descriptor["allowed_statuses"]:
                canonical_statuses = False
            if row["run_count"] != MIN_ACCEPTED_REPEATS:
                canonical_statuses = False
            timings = (
                row["outer_e2e_ns_p50"],
                row["outer_e2e_minus_cigar_or_mapq_union_ns_p50"],
            )
            if row["status"] == "accepted":
                if any(isinstance(value, bool) or not isinstance(value, int)
                       for value in timings):
                    raise ContractError(
                        "accepted canonical row lost one of its exact timing fields")
            elif timings != (None, None):
                raise ContractError(
                    "rejected/invalid/pending canonical row retained numeric timing")
        publishable = (
            complete_coverage
            and canonical_statuses
            and len(statuses) == EXPECTED_RESULT_CELLS
            and not any(status in {"invalid", "pending"} for status in statuses)
        )
        if publishable:
            campaign_status = "complete"
            no_results_reason = None
        elif complete_coverage:
            campaign_status = "complete_nonpublishable"
            no_results_reason = (
                "The canonical cell set is complete, but one or more statuses "
                "are invalid, pending, or forbidden by the campaign contract.")
        else:
            campaign_status = "aggregation_in_progress"
            no_results_reason = (
                f"Canonical campaign coverage is incomplete: "
                f"{len(expected_keys - actual_keys)} cells remain unmerged.")
        campaign = _require_dict(document.get("campaign"), "campaign")
        benchmark_runs_started = len(processed)
        if ACTIVE_CAMPAIGN_PROFILE == "q1024":
            benchmark_runs_started += sum(
                _require_int(campaign.get(field), f"campaign.{field}")
                for field in (
                    "accepted_calibration_records",
                    "excluded_calibration_records",
                    "active_calibration_runs",
                    "pending_calibration_runs",
                ))
        campaign.update({
            "status": campaign_status,
            "measurements_collected": sum(
                row["run_count"] for row in merged_rows
                if row["status"] == "accepted"),
            "benchmark_runs_started": benchmark_runs_started,
            "results_are_publishable": publishable,
            "no_results_reason": no_results_reason,
        })
        state.update({
            "processed_reports": processed,
            "report_count": len(processed),
            "raw_record_count": sum(row["run_count"] for row in merged_rows),
            "result_cell_count": len(merged_rows),
            "accepted_cell_count": statuses.count("accepted"),
            "rejected_cell_count": statuses.count("rejected"),
            "invalid_cell_count": statuses.count("invalid"),
            "pending_cell_count": statuses.count("pending"),
            "canonical_membership_complete": complete_coverage,
            "canonical_statuses_valid": canonical_statuses,
            "missing_canonical_cell_count": len(expected_keys - actual_keys),
        })
        if ACTIVE_CAMPAIGN_PROFILE == "q1024":
            state["build_receipts"] = build_receipts
            publication_status = (
                "complete" if publishable
                else "complete_nonpublishable" if complete_coverage
                else "pending")
            _require_dict(
                document.get("publication_methodology"),
                "publication_methodology")["status"] = (
                    "complete" if complete_coverage else "pending")
            document["publication_state"] = {
                "status": publication_status,
                "query_count": EXPECTED_QUERY_COUNT,
                "result_collection": "results",
                "result_count": len(merged_rows),
                "calibration_results_included": False,
            }
            document["status"] = campaign_status
        document["results"] = merged_rows
        changed = document != original_document
        if changed:
            campaign["last_updated_at"] = dt.datetime.now(
                dt.timezone.utc).isoformat()
        # Even an unchanged/idempotent ingestion rewrites the evidence-derived
        # document under this same flock.  There is no path that returns before
        # recomputing publication flags, counts, comments, and result rows.
        _atomic_write(all_results_path, document)
        return changed


APPROVED_TUPLE_COMMANDS = {
    "bootstrap-remote-approved-tuples",
    "validate-approved-tuples",
    "validate-append-candidate",
    "append-approved-tuples",
}
OBSERVED_RUNG_COMMAND = "validate-observed-rung-report"


def _arguments(argv: list[str] | None = None) -> argparse.Namespace:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments and arguments[0] == OBSERVED_RUNG_COMMAND:
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument(
            "observed_rung_command", choices=(OBSERVED_RUNG_COMMAND,))
        parser.add_argument("--manifest", required=True)
        parser.add_argument("--expected-campaign-digest", required=True)
        parser.add_argument("--query-count", required=True, type=int)
        parser.add_argument("--report", required=True)
        parser.add_argument("--config", required=True)
        parser.add_argument("--run-receipt-id", required=True)
        args = parser.parse_args(arguments)
        args.approved_command = None
        return args
    if arguments and arguments[0] in APPROVED_TUPLE_COMMANDS:
        parser = argparse.ArgumentParser(description=__doc__)
        subparsers = parser.add_subparsers(dest="approved_command", required=True)
        for command in sorted(APPROVED_TUPLE_COMMANDS):
            subparser = subparsers.add_parser(command)
            subparser.add_argument("--authority", required=True)
            if command == "bootstrap-remote-approved-tuples":
                subparser.add_argument("--source-authority", required=True)
                subparser.add_argument("--expected-source-sha256", required=True)
                subparser.add_argument("--host-descriptor", required=True)
                subparser.add_argument("--candidate", required=True)
            else:
                subparser.add_argument("--expected-sha256", required=True)
            if command not in {
                    "bootstrap-remote-approved-tuples",
                    "validate-approved-tuples"}:
                subparser.add_argument("--candidate", required=True)
        args = parser.parse_args(arguments)
        args.observed_rung_command = None
        return args
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--all-results", required=True)
    parser.add_argument("--run-receipt-id")
    args = parser.parse_args(arguments)
    args.approved_command = None
    args.observed_rung_command = None
    return args


def main(argv: list[str] | None = None) -> int:
    args = _arguments(argv)
    try:
        if args.observed_rung_command == OBSERVED_RUNG_COMMAND:
            receipt = validate_observed_rung_report(
                _canonical_repository_file(args.manifest, "rung manifest"),
                args.expected_campaign_digest,
                args.query_count,
                _canonical_repository_file(args.report, "rung report"),
                _canonical_repository_file(args.config, "rung config"),
                args.run_receipt_id,
            )
            print(json.dumps(receipt, ensure_ascii=True, sort_keys=True,
                             separators=(",", ":")))
            return 0
        if args.approved_command == "bootstrap-remote-approved-tuples":
            receipt = bootstrap_remote_approved_tuples(
                Path(args.authority), Path(args.source_authority),
                args.expected_source_sha256, Path(args.host_descriptor),
                Path(args.candidate))
            print(json.dumps(receipt, ensure_ascii=True, sort_keys=True,
                             separators=(",", ":")))
            return 0
        if args.approved_command == "validate-approved-tuples":
            receipt = validate_approved_tuples(
                Path(args.authority), args.expected_sha256)
            print(json.dumps(receipt, ensure_ascii=True, sort_keys=True,
                             separators=(",", ":")))
            return 0
        if args.approved_command == "validate-append-candidate":
            receipt = validate_approved_append_candidate(
                Path(args.authority), args.expected_sha256, Path(args.candidate))
            print(json.dumps(receipt, ensure_ascii=True, sort_keys=True,
                             separators=(",", ":")))
            return 0
        if args.approved_command == "append-approved-tuples":
            receipt = append_approved_tuples(
                Path(args.authority), args.expected_sha256, Path(args.candidate))
            print(json.dumps(receipt, ensure_ascii=True, sort_keys=True,
                             separators=(",", ":")))
            return 0
        _activate_campaign_profile(args.all_results)
        changed = merge(
            _canonical_repository_file(args.report, "report"),
            _canonical_repository_file(args.config, "config"),
            _canonical_repository_file(args.all_results, EXPECTED_LEDGER_PATH),
            args.run_receipt_id,
        )
    except ApprovedTupleDuplicateError as exc:
        print(f"approved tuple operation rejected: {exc}", file=sys.stderr)
        return 3
    except ApprovedTupleProofError as exc:
        print(f"approved tuple operation rejected: {exc}", file=sys.stderr)
        return 4
    except ApprovedTupleIOError as exc:
        print(f"approved tuple operation rejected: {exc}", file=sys.stderr)
        return 5
    except ContractError as exc:
        operation = (
            "observed rung validation"
            if args.observed_rung_command is not None else
            "approved tuple operation"
            if args.approved_command is not None else
            "algorithm-matrix aggregation")
        print(f"{operation} rejected: {exc}", file=sys.stderr)
        return 2
    if args.approved_command is not None or args.observed_rung_command is not None:
        raise AssertionError("validation command returned without a receipt")
    print(f"{EXPECTED_LEDGER_PATH} aggregation merged" if changed else
          f"{EXPECTED_LEDGER_PATH} aggregation already current")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
