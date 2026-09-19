from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock


BUILD_MCP_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BUILD_MCP_DIR))

import subagent as subject  # noqa: E402


DESCRIPTION_200 = "D" * 200
INSTRUCTIONS = "Complete the assigned test task and report the concrete result."


class PersistentSubagentContractTest(unittest.TestCase):
    """Exercise the public command surface without touching paid runtimes."""

    def setUp(self) -> None:
        self.real_reconcile_agents = subject._reconcile_agents
        self.real_refresh_quotas = subject._refresh_quotas
        self.real_launch_runtime = subject._launch_runtime
        self.real_identity_is_live = subject._identity_is_live
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        path_values = {
            "SUBAGENT_STATE_DIR": root,
            "SUBAGENT_STATE_FILE": root / "state.json",
            "SUBAGENT_STATE_BACKUP_FILE": root / "state.backup.json",
            "SUBAGENT_BUS_FILE": root / "bus.json",
            "SUBAGENT_LOCK_FILE": root / "registry.lock",
            "SUBAGENT_LOG_DIR": root / "logs",
        }
        for name, value in path_values.items():
            patcher = mock.patch.object(subject, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

        environment = mock.patch.dict(os.environ, {}, clear=False)
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop(subject.AGENT_ID_ENV, None)
        os.environ.pop(subject.AGENT_CAPABILITY_ENV, None)

        self.root_identity = subject._ProcessIdentity(
            pid=41001,
            ppid=41000,
            pgid=41001,
            session=41001,
            started="Sat Aug 15 12:00:00 2026",
            command="codex root orchestrator",
        )
        self._patch("_process_anchor", return_value=self.root_identity)
        self._patch("_identity_is_live", return_value=True)
        self._patch("_reconcile_agents", return_value=None)

        self.quotas = {"admin": 80.0, "gmail": 60.0}
        self._patch("_refresh_quotas", side_effect=self._fake_refresh_quotas)
        self._patch("_probe_codex_quota", side_effect=AssertionError(
            "a hermetic test attempted a real quota probe"
        ))
        self._patch("_discover_codex_profiles", side_effect=AssertionError(
            "a hermetic test attempted profile discovery"
        ))

        self.launches: list[dict[str, object]] = []
        self.capabilities: dict[int, str] = {}
        self._patch("_launch_runtime", side_effect=self._fake_launch_runtime)
        self.interrupt_runtime = self._patch("_interrupt_runtime")
        self.wait_for_runtime_exit = self._patch(
            "_wait_for_owned_runtime_exit", return_value=False,
        )
        self.kill_runtime = self._patch("_kill_runtime")
        self.idle_runtime = self._patch(
            "_start_idle_runtime",
            side_effect=lambda agent: {
                "tmux_pane": agent.get("tmux_pane"),
                "pane_pid": 52000 + int(agent["id"]),
                "runtime_identity": {
                    "pid": 52000 + int(agent["id"]),
                    "started": f"idle-{agent['id']}",
                },
            },
        )
        self.send_literal = self._patch("_send_literal")
        self._patch("_wait_for_codex_ready", return_value="›")
        self._patch("_observe_codex_assignment", return_value="WORKING")
        self._patch("_tmux", side_effect=AssertionError(
            "a hermetic test attempted to invoke tmux"
        ))
        self._patch("_capture_pane", return_value="")
        self._patch("_tail_file", return_value="")
        self._patch("_parse_vendor_session_from_log", return_value=None)
        self._patch("_read_codex_telemetry", return_value={})

    def _patch(self, name: str, *args, **kwargs):
        patcher = mock.patch.object(subject, name, *args, **kwargs)
        value = patcher.start()
        self.addCleanup(patcher.stop)
        return value

    def _fake_refresh_quotas(self, *, force: bool = False) -> list[str]:
        del force
        now = time.time()
        with subject._registry_lock():
            state = subject._load_state_locked()
            for name, remaining in self.quotas.items():
                account = state["accounts"].setdefault(name, {})
                account.update({
                    "provider": "codex",
                    "profile": name,
                    "available": True,
                    "remaining_percent": remaining,
                    "used_percent": 100.0 - remaining,
                    "quota_sampled_at": now,
                    "quota_error": None,
                    "resets_at": now + 7 * 24 * 3600,
                })
                account.setdefault("quota_history", [])
            state["accounts"].setdefault("cursor", {
                "provider": "cursor",
                "profile": None,
                "remaining_percent": None,
                "quota_sampled_at": None,
                "quota_error": "Cursor exposes no quota endpoint.",
                "quota_history": [],
            })
            subject._save_state_locked(state)
        return []

    def _fake_launch_runtime(
        self,
        agent,
        capability: str,
        prompt: str,
        *,
        resume: bool,
        operation_context=None,
    ) -> dict[str, object]:
        del operation_context
        agent_id = int(agent["id"])
        self.capabilities[agent_id] = capability
        self.launches.append({
            "agent": dict(agent),
            "capability": capability,
            "prompt": prompt,
            "resume": resume,
        })
        return {
            "tmux_session": agent["tmux_session"],
            "tmux_pane": f"%{agent_id}",
            "pane_pid": 42000 + agent_id,
            "runtime_identity": {
                "pid": 42000 + agent_id,
                "started": f"agent-{agent_id}-generation-{len(self.launches)}",
            },
            "vendor_session_id": (
                agent.get("vendor_session_id") or f"vendor-session-{agent_id}"
            ),
        }

    def _call(self, **arguments):
        return json.loads(subject.tool_subagent(arguments))

    def _state(self) -> dict[str, object]:
        with subject._registry_lock():
            return subject._load_state_locked()

    def _mutate_state(self, mutate) -> None:
        with subject._registry_lock():
            state = subject._load_state_locked()
            mutate(state)
            subject._save_state_locked(state)

    def _initialize_root(self) -> None:
        response = self._call(command="status")
        self.assertTrue(response["ok"], response)
        self.assertEqual(response["data"]["caller_id"], 0)

    def _create(
        self,
        tier: str = "intern",
        *,
        description: str = DESCRIPTION_200,
        instructions: str = INSTRUCTIONS,
        **extra,
    ) -> tuple[int, dict[str, object]]:
        before = {
            int(agent_id) for agent_id in self._state()["agents"]
            if int(agent_id) != 0
        } if subject.SUBAGENT_STATE_FILE.exists() else set()
        response = self._call(
            command="create",
            reasoning_effort=tier,
            instructions=instructions,
            description=description,
            **extra,
        )
        if not response["ok"]:
            return -1, response
        after = {
            int(agent_id) for agent_id in self._state()["agents"]
            if int(agent_id) != 0
        }
        created = sorted(after - before)
        self.assertEqual(len(created), 1, response)
        return created[0], response

    def _agent_record(
        self,
        agent_id: int,
        *,
        parent_id: int = 0,
        tier: str = "intern",
        account: str = "admin",
        status: str = "WORKING",
        cost: int | None = None,
        tokens: int | None = 0,
        interruptions: int = 0,
    ) -> dict[str, object]:
        config = subject.TIER_CONFIG[tier]
        now = time.time()
        return {
            "id": agent_id,
            "parent_id": parent_id,
            "account": account,
            "provider": config["provider"],
            "tier": tier,
            "model": config["model"],
            "model_display": config["model_display"],
            "reasoning": config["reasoning"],
            "cost": config["cost"] if cost is None else cost,
            "label": "general-purpose",
            "description": DESCRIPTION_200,
            "highest_spawn_tier": tier,
            "status": status,
            "operation_state": "RUNNING" if status in subject.LIVE_STATES else "STOPPED",
            "created_at": now,
            "state_changed_at": now,
            "last_interrupted_at": None,
            "latest_log_at": None,
            "latest_message_at": None,
            "interruption_count": interruptions,
            "tokens": tokens,
            "tokens_known": tokens is not None,
            "capability_sha256": subject._capability_digest(f"cap-{agent_id}"),
            "pending_capability_sha256": None,
            "tmux_session": f"test-agent-{agent_id}",
            "tmux_pane": f"%{agent_id}",
            "log_file": str(subject.SUBAGENT_LOG_DIR / f"agent-{agent_id}.log"),
            "vendor_session_id": f"vendor-session-{agent_id}",
        }

    @staticmethod
    def _messages(response: dict[str, object]) -> list[dict[str, object]]:
        data = response["data"]
        if isinstance(data, list):
            return data
        return data["messages"]

    def test_tier_mapping_costs_and_runtime_arguments_are_exact(self) -> None:
        self.assertEqual(subject.SUBAGENT_BUDGET, 4000)
        self.assertEqual(
            subject.TIER_ORDER,
            (
                "contractor-intern", "intern", "contractor", "mid",
                "senior", "specialist",
            ),
        )
        self.assertEqual(
            {
                tier: (
                    config["provider"], config["model"],
                    config["reasoning"], config["cost"],
                )
                for tier, config in subject.TIER_CONFIG.items()
            },
            {
                "contractor-intern": ("cursor", "composer-2.5", None, 1),
                "intern": ("codex", "gpt-5.6-terra", "medium", 22),
                "contractor": ("cursor", "gpt-5.5", None, 45),
                "mid": ("codex", "gpt-5.6-terra", "high", 50),
                "senior": ("codex", "gpt-5.6-sol", "high", 70),
                "specialist": ("codex", "gpt-5.6-sol", "max", 100),
            },
        )

        codex = {
            "provider": "codex", "account": "gmail", "model": "gpt-5.6-sol",
            "reasoning": "max", "vendor_session_id": "session-123",
        }
        fresh = subject._runtime_arguments(codex, resume=False)
        self.assertEqual(fresh[:3], ["codex-profile", "cli", "gmail"])
        self.assertIn("--yolo", fresh)
        self.assertIn("gpt-5.6-sol", fresh)
        self.assertIn('model_reasoning_effort="max"', fresh)
        self.assertNotIn("resume", fresh)
        resumed = subject._runtime_arguments(codex, resume=True)
        self.assertEqual(resumed[:4], ["codex-profile", "cli", "gmail", "resume"])
        self.assertEqual(resumed[-1], "session-123")

        cursor = {
            "provider": "cursor", "model": "composer-2.5",
            "vendor_session_id": "chat-456",
        }
        cursor_args = subject._runtime_arguments(cursor, resume=True)
        self.assertEqual(cursor_args[0], "agent")
        self.assertIn("--yolo", cursor_args)
        self.assertIn("disabled", cursor_args)
        self.assertIn("composer-2.5", cursor_args)
        self.assertEqual(cursor_args[-2:], ["--resume", "chat-456"])

        contractor = {
            "provider": "cursor", "model": "gpt-5.5", "reasoning": None,
            "vendor_session_id": "chat-789",
        }
        contractor_args = subject._runtime_arguments(contractor, resume=True)
        self.assertIn("gpt-5.5", contractor_args)
        self.assertEqual(contractor_args[-2:], ["--resume", "chat-789"])
        self.assertFalse(any("reasoning" in argument for argument in contractor_args))

    def test_terminal_runtime_cleanup_flushes_logs_and_destroys_session(self) -> None:
        agent = self._agent_record(1, status="KILLED")
        agent["runtime_identity"] = {"pid": 42001, "started": "runtime-1"}
        pane = {
            "pane_target": "%1",
            "pane_pid": 42001,
            "pane_dead": True,
            "pane_dead_status": 0,
            "repository_marker": subject._REPOSITORY_KEY,
            "agent_marker": "1",
            "pane_command": "true",
        }
        dead_pipe = subprocess.CompletedProcess([], 1, "", "pane is dead")
        completed = subprocess.CompletedProcess([], 0, "", "")
        with mock.patch.object(
            subject, "_tmux_session_observation", side_effect=[True, False],
        ), mock.patch.object(
            subject, "_tmux_pane_info", return_value=pane,
        ), mock.patch.object(
            subject, "_identity_is_live", return_value=False,
        ), mock.patch.object(
            subject, "_tmux", side_effect=[dead_pipe, completed],
        ) as tmux:
            subject._destroy_owned_tmux_session(agent)
        self.assertEqual(
            [call.args[0] for call in tmux.call_args_list],
            [
                ["pipe-pane", "-t", "%1"],
                ["kill-session", "-t", "=test-agent-1"],
            ],
        )

    def test_runtime_identity_rejects_reused_pid_in_another_session(self) -> None:
        expected = {
            "pid": 42001,
            "started": "Mon Aug 17 00:00:00 2026",
            "session": 42001,
        }
        reused = subject._ProcessIdentity(
            pid=42001,
            ppid=1,
            pgid=42001,
            session=0,
            started="Mon Aug 17 00:00:00 2026",
            command="aslmanager",
        )
        with mock.patch.object(
            subject, "_capture_process_identity", return_value=reused,
        ):
            self.assertFalse(self.real_identity_is_live(expected))
        self.assertFalse(subject._same_identity_dict(
            expected,
            {**expected, "session": 0},
        ))

    def test_reconcile_reaps_and_skips_stable_killed_agents(self) -> None:
        self._initialize_root()

        def populate(state) -> None:
            state["agents"]["1"] = self._agent_record(1, status="KILLED")
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        with mock.patch.object(
            subject, "_destroy_owned_tmux_session",
        ) as destroy, mock.patch.object(
            subject, "_tmux_session_observation",
            side_effect=AssertionError("stable KILLED agent was reconciled twice"),
        ):
            self.real_reconcile_agents()
        destroy.assert_called_once()
        self.assertEqual(destroy.call_args.args[0]["id"], 1)
        self.assertEqual(self._state()["agents"]["1"]["status"], "KILLED")

    def test_reconcile_never_captures_a_dead_pane_reused_pid(self) -> None:
        self._initialize_root()
        original_identity = {
            "pid": 42001,
            "started": "Sun Aug 16 23:59:59 2026",
            "session": 42001,
        }

        def populate(state) -> None:
            record = self._agent_record(1)
            record["runtime_identity"] = original_identity
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        dead_pane = {
            "pane_target": "%1",
            "pane_pid": 42001,
            "pane_dead": True,
            "pane_dead_status": 0,
            "repository_marker": subject._REPOSITORY_KEY,
            "agent_marker": "1",
            "pane_command": "codex",
        }
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=True,
        ), mock.patch.object(
            subject, "_tmux_pane_info", return_value=dead_pane,
        ), mock.patch.object(
            subject, "_capture_process_identity",
            side_effect=AssertionError("dead pane PID must not be captured"),
        ):
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "KILLED")
        self.assertEqual(record["runtime_identity"], original_identity)

    def test_cursor_stream_init_result_and_usage_are_parsed_exactly(self) -> None:
        log_path = Path(self.temporary.name) / "cursor.jsonl"
        events = [
            {"type": "system", "subtype": "init", "session_id": "chat-exact"},
            {"type": "assistant", "message": {"content": "done"}},
            {
                "type": "result",
                "subtype": "success",
                "is_error": False,
                "usage": {"input_tokens": 17, "output_tokens": 5},
            },
        ]
        log_path.write_text(
            "".join(json.dumps(event) + "\n" for event in events),
            encoding="utf-8",
        )
        snapshot = subject._cursor_log_snapshot({"log_file": str(log_path)})
        self.assertTrue(snapshot["initialized"])
        self.assertEqual(snapshot["session_id"], "chat-exact")
        self.assertTrue(snapshot["terminal_result"])
        self.assertFalse(snapshot["result_error"])
        self.assertEqual(snapshot["usage_tokens"], 22)
        self.assertIsInstance(snapshot["latest_message_at"], (int, float))
        self.assertTrue(snapshot["scan_complete"])

    def test_cursor_latest_init_resets_prior_terminal_result(self) -> None:
        log_path = Path(self.temporary.name) / "cursor-multi-turn.jsonl"
        events = [
            {"type": "system", "subtype": "init", "session_id": "chat-exact"},
            {"type": "result", "subtype": "error", "is_error": True},
            {"type": "system", "subtype": "init", "session_id": "chat-exact"},
            {
                "type": "assistant",
                "timestamp": "2026-08-15T14:00:00Z",
                "message": {"content": "new turn is active"},
            },
        ]
        log_path.write_text(
            "".join(json.dumps(event) + "\n" for event in events),
            encoding="utf-8",
        )
        snapshot = subject._cursor_log_snapshot({"log_file": str(log_path)})
        self.assertEqual(snapshot["session_id"], "chat-exact")
        self.assertFalse(snapshot["terminal_result"])
        self.assertIsNone(snapshot["result_error"])
        self.assertEqual(snapshot["latest_message_at"], "2026-08-15T14:00:00Z")

    def test_fast_cursor_terminal_result_is_committed_killed_not_working(self) -> None:
        agent = self._agent_record(
            1, tier="contractor-intern", account="cursor",
        )
        agent["log_file"] = str(Path(self.temporary.name) / "cursor-fast.jsonl")
        alive = {
            "pane_target": "%1", "pane_pid": None, "pane_dead": False,
            "pane_dead_status": None,
        }
        dead = {**alive, "pane_dead": True, "pane_dead_status": 0}
        completed = mock.Mock(returncode=0, stdout="", stderr="")
        with mock.patch.object(
            subject, "_prepare_tmux_session", return_value=("session", "%1"),
        ), mock.patch.object(
            subject, "_tmux", return_value=completed,
        ), mock.patch.object(
            subject, "_tmux_pane_info", side_effect=[alive, dead],
        ), mock.patch.object(
            subject,
            "_wait_for_cursor_init",
            return_value={
                "initialized": True,
                "session_id": "chat-fast",
                "terminal_result": True,
                "usage_tokens": 9,
                "scan_complete": True,
            },
        ), mock.patch.object(
            subject,
            "_cursor_log_snapshot",
            return_value={
                "initialized": True,
                "session_id": "chat-fast",
                "terminal_result": True,
                "usage_tokens": 9,
                "scan_complete": True,
            },
        ):
            runtime = self.real_launch_runtime(
                agent, "capability", "simple prompt", resume=False,
            )
        self.assertEqual(runtime["_acknowledged_status"], "KILLED")
        self.assertEqual(runtime["vendor_session_id"], "chat-fast")
        self.assertEqual(runtime["tokens"], 9)

    def test_dead_cursor_pane_waits_for_terminal_log_flush(self) -> None:
        agent = self._agent_record(
            1, tier="contractor-intern", account="cursor",
        )
        agent["log_file"] = str(Path(self.temporary.name) / "cursor-flush.jsonl")
        alive = {
            "pane_target": "%1", "pane_pid": None, "pane_dead": False,
            "pane_dead_status": None,
        }
        dead = {**alive, "pane_dead": True, "pane_dead_status": 0}
        completed = mock.Mock(returncode=0, stdout="", stderr="")
        not_flushed = {
            "initialized": True,
            "session_id": "chat-flush",
            "terminal_result": False,
            "scan_complete": True,
        }
        flushed = {
            "initialized": True,
            "session_id": "chat-flush",
            "terminal_result": True,
            "result_error": False,
            "usage_tokens": 4,
            "scan_complete": True,
        }
        with mock.patch.object(
            subject, "_prepare_tmux_session", return_value=("session", "%1"),
        ), mock.patch.object(
            subject, "_tmux", return_value=completed,
        ), mock.patch.object(
            subject, "_tmux_pane_info", side_effect=[alive, dead],
        ), mock.patch.object(
            subject, "_wait_for_cursor_init", return_value=not_flushed,
        ), mock.patch.object(
            subject, "_cursor_log_snapshot", side_effect=[not_flushed, flushed],
        ) as snapshot_mock:
            runtime = self.real_launch_runtime(
                agent, "capability", "simple prompt", resume=False,
            )
        self.assertEqual(runtime["_acknowledged_status"], "KILLED")
        self.assertGreaterEqual(snapshot_mock.call_count, 2)

    def test_cursor_init_wait_settles_after_pane_dies_before_pipe_flush(self) -> None:
        agent = self._agent_record(
            1, tier="contractor-intern", account="cursor",
        )
        unflushed = {
            "initialized": False,
            "session_id": None,
            "terminal_result": False,
        }
        flushed = {
            "initialized": True,
            "session_id": "chat-after-exit",
            "terminal_result": True,
            "result_error": False,
        }
        dead = {"pane_dead": True}
        with mock.patch.object(
            subject, "_cursor_log_snapshot", side_effect=[unflushed, flushed],
        ) as snapshot_mock, mock.patch.object(
            subject, "_tmux_pane_info", return_value=dead,
        ):
            snapshot = subject._wait_for_cursor_init(
                agent, "%1", start_offset=0, timeout=0.3,
            )
        self.assertEqual(snapshot["session_id"], "chat-after-exit")
        self.assertEqual(snapshot_mock.call_count, 2)

    def test_cursor_resume_rejects_mismatched_chat_id(self) -> None:
        agent = self._agent_record(
            1, tier="contractor-intern", account="cursor",
        )
        agent["log_file"] = str(Path(self.temporary.name) / "cursor-mismatch.jsonl")
        pane = {
            "pane_target": "%1", "pane_pid": None, "pane_dead": False,
            "pane_dead_status": None,
        }
        completed = mock.Mock(returncode=0, stdout="", stderr="")
        with mock.patch.object(
            subject, "_prepare_tmux_session", return_value=("session", "%1"),
        ), mock.patch.object(
            subject, "_tmux", return_value=completed,
        ), mock.patch.object(
            subject, "_tmux_pane_info", return_value=pane,
        ), mock.patch.object(
            subject,
            "_wait_for_cursor_init",
            return_value={
                "initialized": True,
                "session_id": "different-chat",
                "terminal_result": False,
                "scan_complete": True,
            },
        ):
            with self.assertRaises(subject.SubagentError) as caught:
                self.real_launch_runtime(
                    agent, "capability", "revised prompt", resume=True,
                )
        self.assertEqual(caught.exception.code, "RESUME_CONTEXT_MISMATCH")
        self.assertEqual(agent["vendor_session_id"], "vendor-session-1")
        self.kill_runtime.assert_called_once()

    def test_cursor_terminal_error_is_not_reported_as_success(self) -> None:
        agent = self._agent_record(
            1, tier="contractor-intern", account="cursor",
        )
        agent["log_file"] = str(Path(self.temporary.name) / "cursor-error.jsonl")
        pane = {
            "pane_target": "%1", "pane_pid": None, "pane_dead": False,
            "pane_dead_status": None,
        }
        completed = mock.Mock(returncode=0, stdout="", stderr="")
        with mock.patch.object(
            subject, "_prepare_tmux_session", return_value=("session", "%1"),
        ), mock.patch.object(
            subject, "_tmux", return_value=completed,
        ), mock.patch.object(
            subject, "_tmux_pane_info", return_value=pane,
        ), mock.patch.object(
            subject,
            "_wait_for_cursor_init",
            return_value={
                "initialized": True,
                "session_id": "vendor-session-1",
                "terminal_result": True,
                "result_error": True,
                "result_diagnostic": "composer failed",
                "scan_complete": True,
            },
        ):
            with self.assertRaises(subject.SubagentError) as caught:
                self.real_launch_runtime(
                    agent, "capability", "simple prompt", resume=False,
                )
        self.assertEqual(caught.exception.code, "CURSOR_RESULT_ERROR")
        self.assertEqual(caught.exception.details, "composer failed")
        self.kill_runtime.assert_called_once()

    def test_prompt_prefix_is_short_and_encodes_delegation_policy(self) -> None:
        senior = subject._prompt_prefix(7, "senior", "mid", 13)
        self.assertIn("subagent 7", senior)
        self.assertIn("tier senior", senior)
        self.assertIn("13 messages", senior)
        self.assertIn("mid, contractor, intern, contractor-intern", senior)
        self.assertNotIn("senior,", senior)
        self.assertIn("never wait passively", senior)
        self.assertLess(len(senior), 600)

        intern = subject._prompt_prefix(8, "intern", "intern", 0)
        self.assertIn("critical situations", intern)
        self.assertIn("do the work yourself", intern)

    def test_contractor_create_status_authority_and_exact_context_resume(self) -> None:
        agent_id, created = self._create("contractor")
        self.assertTrue(created["ok"], created)
        self.assertEqual(created["data"]["account"], "cursor")
        self.assertEqual(created["data"]["provider"], "cursor")
        self.assertEqual(created["data"]["model"], "gpt-5.5")
        self.assertEqual(created["data"]["model_display"], "gpt-5.5")
        self.assertIsNone(created["data"]["vendor_reasoning"])
        self.assertEqual(created["data"]["cost"], 45)
        self.assertTrue(any(
            "separate Cursor account quota" in warning
            and "dynamically selected reasoning effort" in warning
            for warning in created["warnings"]
        ))

        status = self._call(command="status")
        projected = next(
            agent for agent in status["data"]["agents"]
            if agent["id"] == agent_id
        )
        self.assertEqual(projected["reasoning_effort"], "contractor")
        self.assertEqual(projected["model"], "gpt-5.5")
        self.assertEqual(projected["model_display"], "gpt-5.5")
        self.assertIsNone(projected["vendor_reasoning"])
        self.assertEqual(
            projected["allowed_tiers"],
            ["contractor", "intern", "contractor-intern"],
        )

        os.environ[subject.AGENT_ID_ENV] = str(agent_id)
        os.environ[subject.AGENT_CAPABILITY_ENV] = self.capabilities[agent_id]
        _, forbidden = self._create("mid")
        self.assertFalse(forbidden["ok"])
        self.assertEqual(forbidden["error_code"], "UNAUTHORIZED_TIER")
        os.environ.pop(subject.AGENT_ID_ENV)
        os.environ.pop(subject.AGENT_CAPABILITY_ENV)

        killed = self._call(command="kill", agent_id=agent_id)
        self.assertTrue(killed["ok"], killed)
        repurposed = self._call(
            command="repurpose",
            agent_id=agent_id,
            instructions="Resume the exact contractor chat for another bounded task.",
            description="C" * 200,
        )
        self.assertTrue(repurposed["ok"], repurposed)
        self.assertTrue(repurposed["data"]["resumed_process"])
        self.assertTrue(self.launches[-1]["resume"])
        self.assertEqual(self.launches[-1]["agent"]["model"], "gpt-5.5")
        self.assertIsNone(self.launches[-1]["agent"]["reasoning"])

    def test_public_dispatch_strictly_rejects_cross_command_arguments_and_bools(self) -> None:
        cases = (
            ({"command": "status", "agent_id": 1}, "INVALID_ARGUMENT"),
            ({"command": "status", "refresh_quotas": True}, "INVALID_ARGUMENT"),
            ({"command": "bus-read", "message_count": True}, "INVALID_ARGUMENT"),
            ({"command": "logs", "agent_id": True}, "INVALID_ARGUMENT"),
            ({"command": "interrupt", "agent_id": 1, "instructions": ""}, "INVALID_ARGUMENT"),
            ({"command": "unknown"}, "INVALID_COMMAND"),
        )
        for arguments, error_code in cases:
            with self.subTest(arguments=arguments):
                response = json.loads(subject.tool_subagent(arguments))
                self.assertFalse(response["ok"])
                self.assertEqual(response["error_code"], error_code)
        response = json.loads(subject.tool_subagent([]))
        self.assertFalse(response["ok"])
        self.assertEqual(response["error_code"], "INVALID_ARGUMENT")

    def test_description_boundary_is_measured_after_trimming(self) -> None:
        for description in ("x" * 199, " " + "x" * 199 + " "):
            response = self._call(
                command="create",
                reasoning_effort="intern",
                instructions=INSTRUCTIONS,
                description=description,
            )
            self.assertFalse(response["ok"])
            self.assertEqual(response["error_code"], "INVALID_ARGUMENT")
        agent_id, response = self._create(description="x" * 200)
        self.assertTrue(response["ok"], response)
        self.assertEqual(agent_id, 1)
        self.assertEqual(self._state()["agents"]["1"]["description"], "x" * 200)

    def test_live_budget_accepts_exact_4000_and_rejects_any_overage(self) -> None:
        self._initialize_root()
        costs_and_tiers = (
            [(100, "specialist")] * 38
            + [(70, "senior")]
            + [(22, "intern")] * 4
            + [(1, "contractor-intern")] * 20
        )
        self.assertEqual(sum(cost for cost, _ in costs_and_tiers), 3978)

        def populate(state) -> None:
            for agent_id, (cost, tier) in enumerate(costs_and_tiers, 1):
                account = "cursor" if tier == "contractor-intern" else "admin"
                state["agents"][str(agent_id)] = self._agent_record(
                    agent_id, tier=tier, account=account, cost=cost,
                )
            state["next_agent_id"] = len(costs_and_tiers) + 1

        self._mutate_state(populate)
        agent_id, accepted = self._create("intern")
        self.assertTrue(accepted["ok"], accepted)
        self.assertGreater(agent_id, 0)
        self.assertEqual(subject._live_budget(self._state()), 4000)

        _, rejected = self._create("contractor-intern")
        self.assertFalse(rejected["ok"])
        self.assertEqual(rejected["error_code"], "BUDGET_EXCEEDED")
        self.assertIn("4000", rejected["summary"])
        self.assertIn("must not", rejected["summary"].lower())

    def test_create_rejects_codex_accounts_below_three_percent(self) -> None:
        self.quotas = {"admin": 2.99, "gmail": 0.0}
        _, response = self._create("intern")
        self.assertFalse(response["ok"])
        self.assertEqual(response["error_code"], "NO_ACCOUNT_QUOTA")
        self.assertIn("3%", response["summary"])
        self.assertEqual(self.launches, [])

    def test_load_balancing_prefers_fewer_agents_then_more_quota(self) -> None:
        self.quotas = {"admin": 90.0, "gmail": 10.0}
        self._initialize_root()

        def populate(state) -> None:
            state["agents"]["1"] = self._agent_record(1, account="admin")
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        first_id, first = self._create("intern")
        self.assertTrue(first["ok"], first)
        self.assertEqual(self._state()["agents"][str(first_id)]["account"], "gmail")

        second_id, second = self._create("intern")
        self.assertTrue(second["ok"], second)
        self.assertEqual(self._state()["agents"][str(second_id)]["account"], "admin")

    def test_capability_auth_propagates_parent_and_enforces_ceiling(self) -> None:
        parent_id, parent = self._create("mid", highest_spawn_tier="intern")
        self.assertTrue(parent["ok"], parent)
        capability = self.capabilities[parent_id]

        os.environ[subject.AGENT_ID_ENV] = str(parent_id)
        os.environ[subject.AGENT_CAPABILITY_ENV] = "not-the-capability"
        _, unauthorized = self._create("intern")
        self.assertFalse(unauthorized["ok"])
        self.assertEqual(unauthorized["error_code"], "CALLER_UNAUTHORIZED")

        os.environ[subject.AGENT_CAPABILITY_ENV] = capability
        child_id, child = self._create("intern")
        self.assertTrue(child["ok"], child)
        self.assertEqual(self._state()["agents"][str(child_id)]["parent_id"], parent_id)

        _, over_ceiling = self._create("mid")
        self.assertFalse(over_ceiling["ok"])
        self.assertEqual(over_ceiling["error_code"], "UNAUTHORIZED_TIER")

    def test_managed_ancestry_cannot_re_elect_itself_as_root(self) -> None:
        agent_id, created = self._create("intern")
        self.assertTrue(created["ok"], created)
        os.environ.pop(subject.AGENT_ID_ENV, None)
        os.environ.pop(subject.AGENT_CAPABILITY_ENV, None)
        impostor = subject._ProcessIdentity(
            pid=43001,
            ppid=43000,
            pgid=43001,
            session=43001,
            started="Sat Aug 15 12:05:00 2026",
            command="codex managed descendant with stripped environment",
        )
        with mock.patch.object(
            subject, "_process_anchor", return_value=impostor,
        ), mock.patch.object(
            subject, "_unmarked_caller_is_managed", return_value=True,
        ):
            response = self._call(command="bus-read")
        self.assertFalse(response["ok"])
        self.assertEqual(response["error_code"], "CALLER_UNAUTHORIZED")
        self.assertEqual(self._state()["agents"][str(agent_id)]["parent_id"], 0)

    def test_lower_tier_can_view_but_cannot_mutate_higher_tier(self) -> None:
        specialist_id, specialist = self._create("specialist")
        self.assertTrue(specialist["ok"], specialist)
        intern_id, intern = self._create("intern")
        self.assertTrue(intern["ok"], intern)
        os.environ[subject.AGENT_ID_ENV] = str(intern_id)
        os.environ[subject.AGENT_CAPABILITY_ENV] = self.capabilities[intern_id]

        logs = self._call(command="logs", agent_id=specialist_id, lines=10)
        self.assertTrue(logs["ok"], logs)
        mutations = (
            {"command": "interrupt", "agent_id": specialist_id},
            {"command": "kill", "agent_id": specialist_id},
            {
                "command": "repurpose",
                "agent_id": specialist_id,
                "instructions": INSTRUCTIONS,
                "description": "H" * 200,
            },
        )
        for arguments in mutations:
            with self.subTest(command=arguments["command"]):
                response = self._call(**arguments)
                self.assertFalse(response["ok"])
                self.assertEqual(response["error_code"], "UNAUTHORIZED_TIER")

    def test_interrupt_kill_and_repurpose_preserve_id_and_resume_context(self) -> None:
        agent_id, created = self._create("intern")
        self.assertTrue(created["ok"], created)

        interrupted = self._call(command="interrupt", agent_id=agent_id)
        self.assertTrue(interrupted["ok"], interrupted)
        record = self._state()["agents"][str(agent_id)]
        self.assertEqual(record["status"], "INTERRUPTED")
        self.assertEqual(record["operation_state"], "USER_INTERRUPTED")
        self.assertEqual(record["interruption_count"], 1)

        redirected = self._call(
            command="interrupt",
            agent_id=agent_id,
            instructions="Pivot to the revised assignment and continue now.",
        )
        self.assertTrue(redirected["ok"], redirected)
        record = self._state()["agents"][str(agent_id)]
        self.assertEqual(record["status"], "WORKING")
        self.assertEqual(record["interruption_count"], 1)

        killed = self._call(command="kill", agent_id=agent_id)
        self.assertTrue(killed["ok"], killed)
        self.assertEqual(self._state()["agents"][str(agent_id)]["status"], "KILLED")

        new_description = "R" * 200
        repurposed = self._call(
            command="repurpose",
            agent_id=agent_id,
            instructions="Begin the new assignment while retaining prior useful context.",
            description=new_description,
            label="review",
            highest_spawn_tier="contractor-intern",
        )
        self.assertTrue(repurposed["ok"], repurposed)
        record = self._state()["agents"][str(agent_id)]
        self.assertEqual(record["id"], agent_id)
        self.assertEqual(record["status"], "WORKING")
        self.assertEqual(record["description"], new_description)
        self.assertEqual(record["label"], "review")
        self.assertEqual(record["highest_spawn_tier"], "contractor-intern")
        self.assertTrue(self.launches[-1]["resume"])
        self.assertEqual(
            self.launches[-1]["agent"]["vendor_session_id"],
            "vendor-session-1",
        )
        self.interrupt_runtime.assert_called_once()
        self.send_literal.assert_called_once()
        self.kill_runtime.assert_called_once()

    def test_launch_failure_keeps_record_but_releases_confirmed_budget(self) -> None:
        with mock.patch.object(
            subject,
            "_launch_runtime",
            side_effect=subject.SubagentError("RUNTIME_ERROR", "launch failed"),
        ):
            agent_id, response = self._create("intern")
        self.assertEqual(agent_id, -1)
        self.assertFalse(response["ok"])
        self.assertEqual(response["error_code"], "RUNTIME_ERROR")
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "KILLED")
        self.assertEqual(record["operation_state"], "LAUNCH_FAILED")
        self.assertEqual(subject._live_budget(self._state()), 0)

    def test_bus_defaults_to_last_25_and_enforces_read_write_limits(self) -> None:
        for number in range(30):
            response = self._call(command="bus-write", message=f"message-{number}")
            self.assertTrue(response["ok"], response)
        default_messages = self._messages(self._call(command="bus-read"))
        self.assertEqual(len(default_messages), 25)
        self.assertEqual(default_messages[0]["message"], "message-5")
        self.assertEqual(default_messages[-1]["message"], "message-29")
        last_three = self._messages(self._call(
            command="bus-read", message_count=3,
        ))
        self.assertEqual(
            [message["message"] for message in last_three],
            ["message-27", "message-28", "message-29"],
        )
        for count in (0, 201, True):
            response = self._call(command="bus-read", message_count=count)
            self.assertFalse(response["ok"])
            self.assertEqual(response["error_code"], "INVALID_ARGUMENT")

    def test_concurrent_bus_writes_and_budget_reservations_are_lossless(self) -> None:
        self._initialize_root()
        with ThreadPoolExecutor(max_workers=8) as executor:
            responses = list(executor.map(
                lambda number: self._call(
                    command="bus-write", message=f"concurrent-{number}",
                ),
                range(40),
            ))
        self.assertTrue(all(response["ok"] for response in responses), responses)
        messages = self._messages(self._call(command="bus-read", message_count=40))
        self.assertEqual(len(messages), 40)
        self.assertEqual(len({message["id"] for message in messages}), 40)

        def populate(state) -> None:
            for agent_id in range(1, 39):
                state["agents"][str(agent_id)] = self._agent_record(
                    agent_id, tier="specialist", cost=100,
                )
            state["agents"]["39"] = self._agent_record(
                39, tier="senior", cost=70,
            )
            for agent_id in range(40, 44):
                state["agents"][str(agent_id)] = self._agent_record(
                    agent_id, tier="intern", cost=22,
                )
            for agent_id in range(44, 64):
                state["agents"][str(agent_id)] = self._agent_record(
                    agent_id, tier="contractor-intern", account="cursor", cost=1,
                )
            state["next_agent_id"] = 64

        self._mutate_state(populate)
        create_arguments = {
            "command": "create",
            "reasoning_effort": "intern",
            "instructions": INSTRUCTIONS,
            "description": DESCRIPTION_200,
        }
        with ThreadPoolExecutor(max_workers=2) as executor:
            creates = list(executor.map(
                lambda _: json.loads(subject.tool_subagent(create_arguments)),
                range(2),
            ))
        self.assertEqual(sum(bool(response["ok"]) for response in creates), 1)
        rejected = next(response for response in creates if not response["ok"])
        self.assertEqual(rejected["error_code"], "BUDGET_EXCEEDED")
        self.assertEqual(subject._live_budget(self._state()), 4000)
        for message in ("", "x" * 20_001):
            response = self._call(command="bus-write", message=message)
            self.assertFalse(response["ok"])
            self.assertEqual(response["error_code"], "INVALID_ARGUMENT")

    def test_registry_lock_serializes_independent_processes(self) -> None:
        def cleanup_process(process: subprocess.Popen[str]) -> None:
            if process.poll() is None:
                process.kill()
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    pass
            if process.stdout is not None:
                process.stdout.close()
            if process.stderr is not None:
                process.stderr.close()

        script = """
import sys
import time
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import subagent
root = Path(sys.argv[2])
subagent.SUBAGENT_STATE_DIR = root
subagent.SUBAGENT_STATE_FILE = root / 'state.json'
subagent.SUBAGENT_BUS_FILE = root / 'bus.json'
subagent.SUBAGENT_LOCK_FILE = root / 'registry.lock'
subagent.SUBAGENT_LOG_DIR = root / 'logs'
with subagent._registry_lock():
    print('locked', flush=True)
    time.sleep(float(sys.argv[3]))
"""
        command = [
            sys.executable,
            "-c",
            script,
            str(BUILD_MCP_DIR),
            self.temporary.name,
        ]
        first = subprocess.Popen(
            [*command, "0.45"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.addCleanup(cleanup_process, first)
        self.assertEqual(first.stdout.readline().strip(), "locked")
        started = time.monotonic()
        second = subprocess.Popen(
            [*command, "0"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.addCleanup(cleanup_process, second)
        second_line = second.stdout.readline().strip()
        elapsed = time.monotonic() - started
        first.wait(timeout=3)
        second.wait(timeout=3)
        self.assertEqual(second_line, "locked")
        self.assertGreaterEqual(elapsed, 0.30)

    def test_status_projects_hierarchy_token_totals_and_interruption_warning(self) -> None:
        self._initialize_root()

        def populate(state) -> None:
            state["agents"].update({
                "1": self._agent_record(1, tokens=100, interruptions=11),
                "2": self._agent_record(2, parent_id=1, tokens=50),
                "3": self._agent_record(3, parent_id=2, tokens=None),
            })
            state["next_agent_id"] = 4

        self._mutate_state(populate)
        response = self._call(command="status")
        self.assertTrue(response["ok"], response)
        projected = {agent["id"]: agent for agent in response["data"]["agents"]}
        parent = projected[1]
        self.assertEqual(parent["direct_subagents_ever"], 1)
        self.assertEqual(parent["recursive_subagents_ever"], 2)
        self.assertEqual(parent["tokens_self"]["known_total"], 100)
        self.assertEqual(
            parent["tokens_self_plus_direct_children"]["known_total"], 150,
        )
        self.assertTrue(parent["tokens_self_plus_direct_children"]["complete"])
        self.assertEqual(parent["tokens_recursive_subtree"]["known_total"], 150)
        self.assertFalse(parent["tokens_recursive_subtree"]["complete"])
        self.assertEqual(parent["tokens_recursive_subtree"]["unknown_agent_ids"], [3])
        self.assertTrue(any(
            "11 times" in warning and "context" in warning
            for warning in response["warnings"]
        ))

    def test_quota_trend_requires_six_samples(self) -> None:
        samples = [
            {"sampled_at": index * 300.0, "remaining_percent": 100.0 - index}
            for index in range(6)
        ]
        insufficient = subject._quota_trend({"quota_history": samples[:5]})
        self.assertEqual(insufficient["sample_count"], 5)
        self.assertIsNone(insufficient["average_used_percent_per_hour"])
        self.assertIsNone(insufficient["estimated_exhaustion_at"])
        trend = subject._quota_trend({"quota_history": samples})
        self.assertEqual(trend["sample_count"], 6)
        self.assertAlmostEqual(trend["average_used_percent_per_hour"], 12.0)
        self.assertGreater(trend["estimated_exhaustion_at"], samples[-1]["sampled_at"])

    def test_aggregate_quota_eta_ignores_unavailable_profiles(self) -> None:
        self._initialize_root()
        now = time.time()
        samples = [
            {
                "sampled_at": now - (5 - index) * 300,
                "remaining_percent": 80.0 - index,
            }
            for index in range(6)
        ]

        def populate(state) -> None:
            state["accounts"]["admin"].update({
                "available": True,
                "quota_history": samples,
            })
            state["accounts"]["gmail"].update({
                "available": False,
                "quota_history": [],
            })

        self._mutate_state(populate)
        state = self._state()
        payload, _ = subject._status_payload(state, 0)
        admin_eta = subject._quota_trend(state["accounts"]["admin"])[
            "estimated_exhaustion_at"
        ]
        self.assertEqual(
            payload["quota_estimate"]["all_codex_accounts_exhausted_at"],
            admin_eta,
        )
        self._mutate_state(
            lambda value: value["accounts"]["admin"].update(
                quota_error="probe failed",
            )
        )
        payload, _ = subject._status_payload(self._state(), 0)
        self.assertIsNone(
            payload["quota_estimate"]["all_codex_accounts_exhausted_at"]
        )
        self._mutate_state(
            lambda value: value["accounts"]["admin"].update(quota_error=None)
        )
        self._mutate_state(
            lambda value: value["accounts"]["gmail"].update(available=True)
        )
        payload, _ = subject._status_payload(self._state(), 0)
        self.assertIsNone(
            payload["quota_estimate"]["all_codex_accounts_exhausted_at"]
        )

    def test_interrupt_always_reports_fresh_low_quota_warning(self) -> None:
        agent_id, created = self._create("intern")
        self.assertTrue(created["ok"], created)
        account = created["data"]["account"]
        self.quotas[str(account)] = 2.5

        interrupted = self._call(command="interrupt", agent_id=agent_id)
        self.assertTrue(interrupted["ok"], interrupted)
        self.assertTrue(any(
            "2.5%" in warning and "Avoid further interruptions" in warning
            for warning in interrupted["warnings"]
        ))

        already_idle = self._call(command="interrupt", agent_id=agent_id)
        self.assertTrue(already_idle["ok"], already_idle)
        self.assertTrue(any("2.5%" in warning for warning in already_idle["warnings"]))

        redirected = self._call(
            command="interrupt",
            agent_id=agent_id,
            instructions="Continue with a small revised assignment.",
        )
        self.assertTrue(redirected["ok"], redirected)
        self.assertTrue(any("2.5%" in warning for warning in redirected["warnings"]))

    def test_cursor_interrupt_uses_idle_supervisor_and_exact_chat_resume(self) -> None:
        agent_id, created = self._create("contractor-intern")
        self.assertTrue(created["ok"], created)

        interrupted = self._call(command="interrupt", agent_id=agent_id)
        self.assertTrue(interrupted["ok"], interrupted)
        record = self._state()["agents"][str(agent_id)]
        self.assertEqual(record["status"], "INTERRUPTED")
        self.assertEqual(record["operation_state"], "FORCED_STOP_IDLE")
        self.assertIsNone(record["capability_sha256"])
        self.assertEqual(record["interruption_count"], 1)
        self.assertTrue(interrupted["data"]["forced_stop"])
        self.interrupt_runtime.assert_called_once()
        self.kill_runtime.assert_not_called()
        self.idle_runtime.assert_called_once()

        redirected = self._call(
            command="interrupt",
            agent_id=agent_id,
            instructions="Resume the exact Cursor chat and perform the revised simple task.",
        )
        self.assertTrue(redirected["ok"], redirected)
        record = self._state()["agents"][str(agent_id)]
        self.assertEqual(record["status"], "WORKING")
        self.assertEqual(record["interruption_count"], 1)
        self.assertIsNotNone(record["capability_sha256"])
        self.assertTrue(self.launches[-1]["resume"])
        self.assertEqual(
            self.launches[-1]["agent"]["vendor_session_id"],
            f"vendor-session-{agent_id}",
        )
        self.kill_runtime.assert_not_called()

    def test_cursor_instruction_interrupt_without_chat_id_is_non_destructive(self) -> None:
        agent_id, created = self._create("contractor-intern")
        self.assertTrue(created["ok"], created)
        self._mutate_state(
            lambda state: state["agents"][str(agent_id)].update(
                vendor_session_id=None,
            )
        )
        response = self._call(
            command="interrupt",
            agent_id=agent_id,
            instructions="Replace the current work but retain its context.",
        )
        self.assertFalse(response["ok"])
        self.assertEqual(response["error_code"], "RESUME_CONTEXT_MISSING")
        self.assertEqual(self._state()["agents"][str(agent_id)]["status"], "WORKING")
        self.kill_runtime.assert_not_called()
        bare = self._call(command="interrupt", agent_id=agent_id)
        self.assertFalse(bare["ok"])
        self.assertEqual(bare["error_code"], "RESUME_CONTEXT_MISSING")
        self.kill_runtime.assert_not_called()

    def test_cursor_interrupt_failure_is_immediately_cleanup_killable(self) -> None:
        agent_id, created = self._create("contractor-intern")
        self.assertTrue(created["ok"], created)
        with mock.patch.object(
            subject,
            "_launch_runtime",
            side_effect=subject.SubagentError("RUNTIME_ERROR", "resume failed"),
        ), mock.patch.object(
            subject, "_owned_runtime_observation", return_value=(None, None),
        ):
            failed = self._call(
                command="interrupt",
                agent_id=agent_id,
                instructions="Try revised work in the retained chat.",
            )
        self.assertFalse(failed["ok"])
        record = self._state()["agents"][str(agent_id)]
        self.assertTrue(record["operation_cleanup_required"])
        killed = self._call(command="kill", agent_id=agent_id)
        self.assertTrue(killed["ok"], killed)
        self.assertEqual(self._state()["agents"][str(agent_id)]["status"], "KILLED")

    def test_cursor_repurpose_failure_is_immediately_cleanup_killable(self) -> None:
        agent_id, created = self._create("contractor-intern")
        self.assertTrue(created["ok"], created)
        self.assertTrue(self._call(command="kill", agent_id=agent_id)["ok"])
        with mock.patch.object(
            subject,
            "_launch_runtime",
            side_effect=subject.SubagentError("RUNTIME_ERROR", "resume failed"),
        ), mock.patch.object(
            subject, "_owned_runtime_observation", return_value=(None, None),
        ):
            failed = self._call(
                command="repurpose",
                agent_id=agent_id,
                instructions="Perform a replacement simple task.",
                description="R" * 200,
            )
        self.assertFalse(failed["ok"])
        record = self._state()["agents"][str(agent_id)]
        self.assertTrue(record["operation_cleanup_required"])
        killed = self._call(command="kill", agent_id=agent_id)
        self.assertTrue(killed["ok"], killed)
        self.assertEqual(self._state()["agents"][str(agent_id)]["status"], "KILLED")

    def test_failed_quota_probe_is_throttled_by_attempt_timestamp(self) -> None:
        discovery = mock.patch.object(
            subject, "_discover_codex_profiles", side_effect=lambda: (["admin"], []),
        )
        probe = mock.patch.object(
            subject,
            "_probe_codex_quota",
            side_effect=subject.SubagentError("QUOTA_PROBE_FAILED", "offline"),
        )
        with discovery, probe as probe_mock:
            first = self.real_refresh_quotas()
            second = self.real_refresh_quotas()
        self.assertEqual(probe_mock.call_count, 1)
        self.assertTrue(any("offline" in warning for warning in first))
        self.assertEqual(second, [])
        account = self._state()["accounts"]["admin"]
        self.assertIsInstance(account.get("quota_attempted_at"), (int, float))

    def test_operation_token_freezes_preoperation_runtime_identity(self) -> None:
        record = self._agent_record(1)
        original = {"pid": 50001, "started": "runtime-a"}
        record["runtime_identity"] = original
        subject._operation_token(record, "RESUMING", intended_status="WORKING")
        original["pid"] = 99999
        record["runtime_identity"] = {"pid": 50002, "started": "runtime-b"}
        self.assertEqual(
            record["operation_previous_runtime_identity"],
            {"pid": 50001, "started": "runtime-a"},
        )

    def test_stale_killed_resume_never_rolls_back_after_second_reconcile(self) -> None:
        self._initialize_root()
        instruction_at = time.time() - subject.OPERATION_LEASE_SECONDS - 5
        started_at = time.strftime(
            "%Y-%m-%dT%H:%M:%SZ", time.gmtime(instruction_at + 1),
        )

        def populate(state) -> None:
            record = self._agent_record(1)
            record.update({
                "status": "WORKING",
                "generation": 4,
                "operation_state": "RESUMING",
                "operation_phase": "ASSIGNMENT_ACKNOWLEDGED",
                "operation_started_at": instruction_at,
                "operation_instruction_sent_at": instruction_at,
                "operation_nonce": "stale-resume",
                "operation_previous_status": "KILLED",
                "operation_previous_runtime_identity": None,
                "operation_intended_status": "WORKING",
                "capability_sha256": None,
                "pending_capability_sha256": "new-capability-digest",
                "runtime_identity": None,
            })
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        pane = {
            "pane_target": "%1", "pane_pid": 50002, "pane_dead": False,
            "pane_dead_status": None,
            "repository_marker": subject._REPOSITORY_KEY,
            "agent_marker": "1", "pane_command": "codex",
        }
        telemetry = {
            "turn_active": True,
            "turn_started_at": started_at,
            "turn_state_at": started_at,
        }
        identity = subject._ProcessIdentity(
            pid=50002, ppid=1, pgid=50002, session=50002,
            started="Sat Aug 15 13:00:00 2026", command="codex",
        )
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=True,
        ), mock.patch.object(
            subject, "_tmux_pane_info", return_value=pane,
        ), mock.patch.object(
            subject, "_capture_process_identity", return_value=identity,
        ), mock.patch.object(
            subject, "_read_codex_telemetry", return_value=telemetry,
        ):
            self.real_reconcile_agents()
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "WORKING")
        self.assertEqual(record["capability_sha256"], "new-capability-digest")
        self.assertIsNone(record["pending_capability_sha256"])
        self.assertIsNone(record["operation_nonce"])
        self.assertEqual(subject._live_budget(self._state()), 22)

    def test_ambiguous_stale_delivery_remains_budgeted_but_public_kill_works(self) -> None:
        self._initialize_root()

        def populate(state) -> None:
            record = self._agent_record(1)
            record.update({
                "generation": 8,
                "operation_state": "REPURPOSING",
                "operation_phase": "INSTRUCTION_SENT",
                "operation_started_at": time.time() - subject.OPERATION_LEASE_SECONDS - 1,
                "operation_instruction_sent_at": time.time() - 10,
                "operation_nonce": "ambiguous-delivery",
                "operation_previous_status": "WORKING",
                "operation_previous_runtime_identity": None,
                "operation_intended_status": "WORKING",
                "pending_capability_sha256": "pending-capability",
                "pending_description": "P" * 200,
            })
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        pane = {
            "pane_target": "%1", "pane_pid": None, "pane_dead": False,
            "pane_dead_status": None,
            "repository_marker": subject._REPOSITORY_KEY,
            "agent_marker": "1", "pane_command": "codex",
        }
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=True,
        ), mock.patch.object(
            subject, "_tmux_pane_info", return_value=pane,
        ), mock.patch.object(
            subject, "_read_codex_telemetry", return_value={},
        ):
            self.real_reconcile_agents()
        retained = self._state()["agents"]["1"]
        self.assertTrue(retained["operation_cleanup_required"])
        self.assertEqual(subject._live_budget(self._state()), 22)
        killed = self._call(command="kill", agent_id=1)
        self.assertTrue(killed["ok"], killed)
        self.assertEqual(self._state()["agents"]["1"]["status"], "KILLED")
        self.assertEqual(subject._live_budget(self._state()), 0)

    def test_crashed_cleanup_kill_restores_cleanup_gate_when_runtime_is_live(self) -> None:
        self._initialize_root()

        def populate(state) -> None:
            record = self._agent_record(
                1, tier="contractor-intern", account="cursor",
            )
            record.update({
                "operation_state": "CLEANUP_REQUIRED",
                "operation_cleanup_required": True,
                "pending_capability_sha256": "indeterminate-capability",
            })
            subject._operation_token(record, "KILLING", intended_status="KILLED")
            record["operation_started_at"] = (
                time.time() - subject.OPERATION_LEASE_SECONDS - 1
            )
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        pane = {
            "pane_target": "%1", "pane_pid": None, "pane_dead": False,
            "pane_dead_status": None,
            "repository_marker": subject._REPOSITORY_KEY,
            "agent_marker": "1", "pane_command": "agent",
        }
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=True,
        ), mock.patch.object(
            subject, "_tmux_pane_info", return_value=pane,
        ):
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["operation_state"], "RECOVERED_KILL_NOT_APPLIED")
        self.assertTrue(record["operation_cleanup_required"])
        blocked = self._call(
            command="interrupt",
            agent_id=1,
            instructions="This must remain blocked until cleanup.",
        )
        self.assertFalse(blocked["ok"])
        self.assertEqual(blocked["error_code"], "AGENT_CLEANUP_REQUIRED")

    def test_session_owned_placeholder_is_never_reported_working(self) -> None:
        self._initialize_root()

        def populate(state) -> None:
            record = self._agent_record(1)
            record.update({
                "generation": 2,
                "operation_state": "STARTING",
                "operation_phase": "SESSION_OWNED",
                "operation_started_at": time.time() - subject.OPERATION_LEASE_SECONDS - 1,
                "operation_nonce": "stale-start",
                "operation_previous_status": None,
                "operation_previous_runtime_identity": None,
                "operation_intended_status": "WORKING",
            })
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        pane = {
            "pane_target": "%1", "pane_pid": None, "pane_dead": False,
            "pane_dead_status": None,
            "repository_marker": subject._REPOSITORY_KEY,
            "agent_marker": "1", "pane_command": "zsh",
        }
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=True,
        ), mock.patch.object(
            subject, "_tmux_pane_info", return_value=pane,
        ):
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "INTERRUPTED")
        self.assertEqual(record["operation_state"], "RECOVERED_SESSION_ONLY")
        self.assertIsNone(record["capability_sha256"])
        self.assertEqual(subject._live_budget(self._state()), 22)

    def test_cursor_pre_respawn_instruction_phase_recovers_as_session_only(self) -> None:
        self._initialize_root()

        def populate(state) -> None:
            record = self._agent_record(
                1, tier="contractor-intern", account="cursor",
            )
            record.update({
                "generation": 2,
                "operation_state": "RESUMING",
                "operation_phase": "INSTRUCTION_SENDING",
                "operation_started_at": time.time() - subject.OPERATION_LEASE_SECONDS - 1,
                "operation_nonce": "before-respawn",
                "operation_previous_status": "KILLED",
                "operation_previous_runtime_identity": None,
                "operation_intended_status": "WORKING",
                "pending_capability_sha256": "new-capability",
            })
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        pane = {
            "pane_target": "%1", "pane_pid": None, "pane_dead": False,
            "pane_dead_status": None,
            "repository_marker": subject._REPOSITORY_KEY,
            "agent_marker": "1", "pane_command": "zsh",
        }
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=True,
        ), mock.patch.object(
            subject, "_tmux_pane_info", return_value=pane,
        ):
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "INTERRUPTED")
        self.assertEqual(record["operation_state"], "RECOVERED_SESSION_ONLY")
        self.assertFalse(record.get("operation_cleanup_required"))
        self.assertEqual(subject._live_budget(self._state()), 1)

    def test_codex_killed_resume_vendor_starting_is_bounded_and_killable(self) -> None:
        self._initialize_root()

        def populate(state) -> None:
            record = self._agent_record(1)
            record.update({
                "status": "WORKING",
                "generation": 6,
                "operation_state": "RESUMING",
                "operation_phase": "VENDOR_STARTING",
                "operation_started_at": time.time() - subject.OPERATION_LEASE_SECONDS - 1,
                "operation_nonce": "vendor-starting",
                "operation_previous_status": "KILLED",
                "operation_previous_runtime_identity": None,
                "operation_intended_status": "WORKING",
                "capability_sha256": None,
                "pending_capability_sha256": "resumed-capability",
                "runtime_identity": None,
            })
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        pane = {
            "pane_target": "%1", "pane_pid": 50123, "pane_dead": False,
            "pane_dead_status": None,
            "repository_marker": subject._REPOSITORY_KEY,
            "agent_marker": "1", "pane_command": "codex",
        }
        identity = subject._ProcessIdentity(
            pid=50123, ppid=1, pgid=50123, session=50123,
            started="Sat Aug 15 14:00:00 2026", command="codex",
        )
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=True,
        ), mock.patch.object(
            subject, "_tmux_pane_info", return_value=pane,
        ), mock.patch.object(
            subject, "_capture_process_identity", return_value=identity,
        ), mock.patch.object(
            subject, "_read_codex_telemetry", return_value={},
        ):
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "INTERRUPTED")
        self.assertEqual(record["operation_state"], "RECOVERED_VENDOR_IDLE")
        self.assertEqual(record["capability_sha256"], "resumed-capability")
        self.assertIsNone(record["operation_nonce"])

    def test_stale_completed_codex_repurpose_commits_pending_metadata(self) -> None:
        self._initialize_root()
        instruction_at = time.time() - subject.OPERATION_LEASE_SECONDS - 5
        started_at = time.strftime(
            "%Y-%m-%dT%H:%M:%SZ", time.gmtime(instruction_at + 1),
        )
        completed_at = time.strftime(
            "%Y-%m-%dT%H:%M:%SZ", time.gmtime(instruction_at + 2),
        )
        new_description = "N" * 200

        def populate(state) -> None:
            record = self._agent_record(1)
            record.update({
                "generation": 3,
                "operation_state": "REPURPOSING",
                "operation_phase": "INSTRUCTION_SENT",
                "operation_started_at": instruction_at,
                "operation_instruction_sent_at": instruction_at,
                "operation_nonce": "stale-pivot",
                "operation_previous_status": "WORKING",
                "operation_previous_runtime_identity": None,
                "operation_intended_status": "WORKING",
                "pending_description": new_description,
                "pending_label": "audit",
                "pending_highest_spawn_tier": "intern",
            })
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        pane = {
            "pane_target": "%1", "pane_pid": None, "pane_dead": False,
            "pane_dead_status": None,
            "repository_marker": subject._REPOSITORY_KEY,
            "agent_marker": "1", "pane_command": "codex",
        }
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=True,
        ), mock.patch.object(
            subject, "_tmux_pane_info", return_value=pane,
        ), mock.patch.object(
            subject,
            "_read_codex_telemetry",
            return_value={
                "turn_active": False,
                "turn_started_at": started_at,
                "turn_state_at": completed_at,
            },
        ):
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "INTERRUPTED")
        self.assertEqual(record["operation_state"], "RECOVERED_TURN_COMPLETED")
        self.assertEqual(record["description"], new_description)
        self.assertEqual(record["label"], "audit")
        self.assertIsNone(record["pending_description"])

    def test_stale_confirmed_codex_repurpose_commits_metadata_after_exit(self) -> None:
        self._initialize_root()
        instruction_at = time.time() - subject.OPERATION_LEASE_SECONDS - 5
        started_at = time.strftime(
            "%Y-%m-%dT%H:%M:%SZ", time.gmtime(instruction_at + 1),
        )
        completed_at = time.strftime(
            "%Y-%m-%dT%H:%M:%SZ", time.gmtime(instruction_at + 2),
        )
        new_description = "X" * 200

        def populate(state) -> None:
            record = self._agent_record(1)
            record.update({
                "generation": 9,
                "operation_state": "REPURPOSING",
                "operation_phase": "ASSIGNMENT_ACKNOWLEDGED",
                "operation_started_at": instruction_at,
                "operation_instruction_sent_at": instruction_at,
                "operation_assignment_acknowledged_at": instruction_at + 1,
                "operation_nonce": "confirmed-then-exited",
                "operation_previous_status": "WORKING",
                "operation_previous_runtime_identity": None,
                "operation_intended_status": "WORKING",
                "pending_description": new_description,
                "pending_label": "review",
                "pending_highest_spawn_tier": "intern",
                "runtime_identity": None,
            })
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=False,
        ), mock.patch.object(
            subject, "_identity_is_live", return_value=False,
        ), mock.patch.object(
            subject, "_read_codex_telemetry", return_value={},
        ):
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "KILLED")
        self.assertEqual(record["operation_state"], "RECOVERED_OPERATION_STOPPED")
        self.assertEqual(record["description"], new_description)
        self.assertEqual(record["label"], "review")
        self.assertIsNone(record["pending_description"])

    def test_stale_completed_cursor_resume_commits_pending_metadata(self) -> None:
        self._initialize_root()
        log_path = Path(self.temporary.name) / "cursor-stale-success.jsonl"
        log_path.write_text(
            "".join(json.dumps(event) + "\n" for event in [
                {"type": "system", "subtype": "init", "session_id": "vendor-session-1"},
                {"type": "assistant", "message": {"content": "done"}},
                {"type": "result", "subtype": "success", "is_error": False},
            ]),
            encoding="utf-8",
        )
        new_description = "C" * 200

        def populate(state) -> None:
            record = self._agent_record(
                1, tier="contractor-intern", account="cursor",
            )
            record.update({
                "log_file": str(log_path),
                "generation": 5,
                "operation_state": "RESUMING",
                "operation_phase": "INSTRUCTION_SENT",
                "operation_started_at": time.time() - subject.OPERATION_LEASE_SECONDS - 1,
                "operation_nonce": "stale-cursor",
                "operation_previous_status": "KILLED",
                "operation_previous_runtime_identity": None,
                "operation_intended_status": "WORKING",
                "operation_cursor_log_start": 0,
                "operation_expected_session_id": "vendor-session-1",
                "pending_description": new_description,
                "pending_label": "implementation",
                "pending_highest_spawn_tier": "contractor-intern",
            })
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=False,
        ), mock.patch.object(
            subject, "_identity_is_live", return_value=False,
        ):
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "KILLED")
        self.assertEqual(record["operation_state"], "TURN_COMPLETED")
        self.assertEqual(record["description"], new_description)
        self.assertEqual(record["label"], "implementation")
        self.assertIsNotNone(record["latest_message_at"])
        self.assertIsNone(record["pending_description"])

    def test_natural_cursor_error_is_killed_with_diagnostic(self) -> None:
        self._initialize_root()
        log_path = Path(self.temporary.name) / "cursor-natural-error.jsonl"
        log_path.write_text(
            "".join(json.dumps(event) + "\n" for event in [
                {"type": "system", "subtype": "init", "session_id": "vendor-session-1"},
                {
                    "type": "result", "subtype": "error", "is_error": True,
                    "error": "composer task failed",
                },
            ]),
            encoding="utf-8",
        )

        def populate(state) -> None:
            record = self._agent_record(
                1, tier="contractor-intern", account="cursor",
            )
            record["log_file"] = str(log_path)
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=False,
        ), mock.patch.object(
            subject, "_identity_is_live", return_value=False,
        ):
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "KILLED")
        self.assertEqual(record["operation_state"], "CURSOR_RESULT_ERROR")
        self.assertIn("composer task failed", record["last_error"])

    def test_terminal_cursor_success_is_not_left_working_while_pane_settles(self) -> None:
        self._initialize_root()
        log_path = Path(self.temporary.name) / "cursor-live-success.jsonl"
        log_path.write_text(
            "".join(json.dumps(event) + "\n" for event in [
                {"type": "system", "subtype": "init", "session_id": "vendor-session-1"},
                {"type": "result", "subtype": "success", "is_error": False},
            ]),
            encoding="utf-8",
        )

        def populate(state) -> None:
            record = self._agent_record(
                1, tier="contractor-intern", account="cursor",
            )
            record.update({
                "log_file": str(log_path),
                "runtime_log_file": str(log_path),
            })
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        pane = {
            "pane_target": "%1", "pane_pid": None, "pane_dead": False,
            "pane_dead_status": None,
            "repository_marker": subject._REPOSITORY_KEY,
            "agent_marker": "1", "pane_command": "agent",
        }
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=True,
        ), mock.patch.object(
            subject, "_tmux_pane_info", return_value=pane,
        ):
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "INTERRUPTED")
        self.assertEqual(record["operation_state"], "TURN_COMPLETED")
        self.assertEqual(subject._live_budget(self._state()), 1)

    def test_reconcile_recovers_stale_cursor_interrupt_as_idle_not_working(self) -> None:
        self._initialize_root()

        def populate(state) -> None:
            record = self._agent_record(
                1, tier="contractor-intern", account="cursor",
            )
            record.update({
                "generation": 3,
                "operation_state": "INTERRUPTING",
                "operation_started_at": time.time() - subject.OPERATION_LEASE_SECONDS - 1,
                "operation_nonce": "stale",
                "operation_previous_status": "WORKING",
                "operation_intended_status": "INTERRUPTED",
            })
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        pane = {
            "pane_target": "%1",
            "pane_pid": None,
            "pane_dead": False,
            "pane_dead_status": None,
            "repository_marker": subject._REPOSITORY_KEY,
            "agent_marker": "1",
            "pane_command": "tail",
        }
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=True,
        ), mock.patch.object(
            subject, "_tmux_pane_info", return_value=pane,
        ):
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "INTERRUPTED")
        self.assertEqual(record["operation_state"], "RECOVERED_INTERRUPTED_RUNTIME")
        self.assertIsNone(record["operation_nonce"])

    def test_reconcile_marks_natural_codex_turn_completion_interrupted(self) -> None:
        self._initialize_root()

        def populate(state) -> None:
            record = self._agent_record(1)
            record.update({
                "generation": 1,
                "last_instruction_at": time.time() - 10,
            })
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        pane = {
            "pane_target": "%1",
            "pane_pid": None,
            "pane_dead": False,
            "pane_dead_status": None,
            "repository_marker": subject._REPOSITORY_KEY,
            "agent_marker": "1",
            "pane_command": "codex",
        }
        completed_at = time.time()
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=True,
        ), mock.patch.object(
            subject, "_tmux_pane_info", return_value=pane,
        ), mock.patch.object(
            subject,
            "_read_codex_telemetry",
            return_value={
                "turn_active": False,
                "turn_state_at": time.strftime(
                    "%Y-%m-%dT%H:%M:%SZ", time.gmtime(completed_at),
                ),
                "tokens": 123,
                "tokens_known": True,
            },
        ):
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "INTERRUPTED")
        self.assertEqual(record["operation_state"], "TURN_COMPLETED")
        self.assertEqual(record["tokens"], 123)

    def test_reconcile_promotes_pending_capability_for_live_cursor_resume(self) -> None:
        self._initialize_root()

        def populate(state) -> None:
            record = self._agent_record(
                1, tier="contractor-intern", account="cursor",
            )
            record.update({
                "generation": 4,
                "operation_state": "RESUMING",
                "operation_phase": "ASSIGNMENT_ACKNOWLEDGED",
                "operation_started_at": time.time() - subject.OPERATION_LEASE_SECONDS - 1,
                "operation_nonce": "stale-resume",
                "operation_previous_status": "INTERRUPTED",
                "operation_previous_runtime_identity": {
                    "pid": 50001, "started": "old-runtime",
                },
                "operation_intended_status": "WORKING",
                "capability_sha256": None,
                "pending_capability_sha256": "new-capability-digest",
                "runtime_identity": {"pid": 50001, "started": "old-runtime"},
            })
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        pane = {
            "pane_target": "%1",
            "pane_pid": 50002,
            "pane_dead": False,
            "pane_dead_status": None,
            "repository_marker": subject._REPOSITORY_KEY,
            "agent_marker": "1",
            "pane_command": "agent",
        }
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=True,
        ), mock.patch.object(
            subject, "_tmux_pane_info", return_value=pane,
        ), mock.patch.object(
            subject,
            "_capture_process_identity",
            return_value=subject._ProcessIdentity(
                pid=50002,
                ppid=1,
                pgid=50002,
                session=50002,
                started="Sat Aug 15 13:00:00 2026",
                command="agent --resume chat",
            ),
        ):
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "WORKING")
        self.assertEqual(record["capability_sha256"], "new-capability-digest")
        self.assertIsNone(record["pending_capability_sha256"])

    def test_reconcile_stale_killing_dead_is_recovered_requested_kill(self) -> None:
        self._initialize_root()

        def populate(state) -> None:
            record = self._agent_record(1)
            record.update({
                "generation": 2,
                "operation_state": "KILLING",
                "operation_started_at": time.time() - subject.OPERATION_LEASE_SECONDS - 1,
                "operation_nonce": "stale-kill",
                "operation_previous_status": "WORKING",
                "operation_intended_status": "KILLED",
                "runtime_identity": None,
            })
            state["agents"]["1"] = record
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=False,
        ), mock.patch.object(
            subject, "_identity_is_live", return_value=False,
        ):
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "KILLED")
        self.assertEqual(record["operation_state"], "RECOVERED_REQUESTED_KILL")

    def test_indeterminate_tmux_state_never_releases_live_budget(self) -> None:
        self._initialize_root()

        def populate(state) -> None:
            state["agents"]["1"] = self._agent_record(1)
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=False,
        ):
            self.real_reconcile_agents()
        record = self._state()["agents"]["1"]
        self.assertEqual(record["status"], "WORKING")
        self.assertEqual(subject._live_budget(self._state()), 22)

    def test_tmux_return_code_one_is_absent_only_for_explicit_diagnostic(self) -> None:
        permission_error = mock.Mock(
            returncode=1, stdout="", stderr="permission denied opening socket",
        )
        missing = mock.Mock(
            returncode=1, stdout="", stderr="can't find session: bioseq-agent",
        )
        missing_private_socket = mock.Mock(
            returncode=1,
            stdout="",
            stderr=(
                "error connecting to /tmp/bioseq/tmux.sock "
                "(No such file or directory)"
            ),
        )
        with mock.patch.object(subject, "_tmux", return_value=permission_error):
            self.assertIsNone(subject._tmux_session_observation("agent"))
        with mock.patch.object(subject, "_tmux", return_value=missing):
            self.assertIs(subject._tmux_session_observation("agent"), False)
        with mock.patch.object(subject, "_tmux", return_value=missing_private_socket):
            self.assertIs(subject._tmux_session_observation("agent"), False)

        existing_sessions = {"bioseq-agent-20"}
        observed_targets: list[str] = []

        def prefix_sensitive_tmux(arguments, *, timeout=10.0, input_text=None):
            del timeout, input_text
            target = arguments[arguments.index("-t") + 1]
            observed_targets.append(target)
            if target.startswith("="):
                found = target[1:] in existing_sessions
            else:
                found = any(name.startswith(target) for name in existing_sessions)
            return mock.Mock(
                returncode=0 if found else 1,
                stdout="",
                stderr="" if found else f"can't find session: {target}",
            )

        with mock.patch.object(subject, "_tmux", side_effect=prefix_sensitive_tmux):
            self.assertIs(
                subject._tmux_session_observation("bioseq-agent-2"), False,
            )
        self.assertEqual(observed_targets, ["=bioseq-agent-2"])

    def test_prepare_tmux_session_is_index_independent_and_atomic(self) -> None:
        agent = self._agent_record(7)
        calls: list[list[str]] = []

        def fake_tmux(arguments, *, timeout=10.0, input_text=None):
            del timeout, input_text
            calls.append(list(arguments))
            if arguments[0] == "new-session":
                return mock.Mock(returncode=0, stdout="$17\n", stderr="")
            return mock.Mock(returncode=0, stdout="", stderr="")

        pane = {
            "pane_target": "%17",
            "pane_pid": 42017,
            "pane_dead": False,
            "pane_dead_status": None,
            "repository_marker": subject._REPOSITORY_KEY,
            "agent_marker": "7",
            "pane_command": "zsh",
        }
        with (
            mock.patch.object(
                subject, "_tmux_session_observation", return_value=False,
            ),
            mock.patch.object(subject, "_tmux", side_effect=fake_tmux),
            mock.patch.object(subject, "_tmux_pane_info", return_value=pane),
        ):
            session, target = subject._prepare_tmux_session(agent, "unused-secret")

        self.assertEqual(session, agent["tmux_session"])
        self.assertEqual(target, "%17")
        create_calls = [call for call in calls if call[0] == "new-session"]
        self.assertEqual(len(create_calls), 1)
        create_call = create_calls[0]
        exact_active_target = f"={agent['tmux_session']}:"
        self.assertEqual(create_call.count(exact_active_target), 2)
        self.assertFalse(any(":0.0" in argument for argument in create_call))
        option_calls = [call for call in calls if call[0] == "set-option"]
        self.assertEqual(len(option_calls), 2)
        self.assertTrue(all("%17" in call for call in option_calls))

    def test_prepare_tmux_session_rolls_back_partial_compound_failure(self) -> None:
        agent = self._agent_record(8)
        calls: list[list[str]] = []

        def fake_tmux(arguments, *, timeout=10.0, input_text=None):
            del timeout, input_text
            calls.append(list(arguments))
            if arguments[0] == "new-session":
                return mock.Mock(
                    returncode=1, stdout="$42\n", stderr="simulated marker failure",
                )
            return mock.Mock(returncode=0, stdout="", stderr="")

        with (
            mock.patch.object(
                subject, "_tmux_session_observation", return_value=False,
            ),
            mock.patch.object(subject, "_tmux", side_effect=fake_tmux),
        ):
            with self.assertRaises(subject.SubagentError) as raised:
                subject._prepare_tmux_session(agent, "unused-secret")

        self.assertEqual(raised.exception.code, "RUNTIME_ERROR")
        self.assertIn(
            ["kill-session", "-t", "$42"], calls,
        )

    def test_prepare_tmux_session_does_not_kill_duplicate_name_race(self) -> None:
        agent = self._agent_record(10)
        calls: list[list[str]] = []

        def fake_tmux(arguments, *, timeout=10.0, input_text=None):
            del timeout, input_text
            calls.append(list(arguments))
            return mock.Mock(
                returncode=1,
                stdout="",
                stderr="duplicate session: test-agent-10",
            )

        with (
            mock.patch.object(
                subject, "_tmux_session_observation", return_value=False,
            ),
            mock.patch.object(subject, "_tmux", side_effect=fake_tmux),
        ):
            with self.assertRaises(subject.SubagentError) as raised:
                subject._prepare_tmux_session(agent, "unused-secret")

        self.assertEqual(raised.exception.code, "RUNTIME_ERROR")
        self.assertEqual([call[0] for call in calls], ["new-session"])

    def test_prepare_tmux_session_refuses_indeterminate_existing_state(self) -> None:
        agent = self._agent_record(9)
        with mock.patch.object(
            subject, "_tmux_session_observation", return_value=None,
        ):
            with self.assertRaises(subject.SubagentError) as raised:
                subject._prepare_tmux_session(agent, "unused-secret")
        self.assertEqual(raised.exception.code, "RUNTIME_UNKNOWN")

    def test_live_codex_repurpose_at_zero_quota_is_rejected_before_escape(self) -> None:
        agent_id, created = self._create("intern")
        self.assertTrue(created["ok"], created)
        self.quotas = {"admin": 0.0, "gmail": 0.0}
        response = self._call(
            command="repurpose",
            agent_id=agent_id,
            instructions="Pivot to a replacement task without losing the current turn.",
            description="Q" * 200,
        )
        self.assertFalse(response["ok"])
        self.assertEqual(response["error_code"], "ACCOUNT_QUOTA_EXHAUSTED")
        self.interrupt_runtime.assert_not_called()

    def test_noncanonical_tier_metadata_is_repaired_before_budget_math(self) -> None:
        self._initialize_root()

        def populate(state) -> None:
            agent = self._agent_record(1, cost=-100)
            agent.update({
                "provider": "obsolete-provider",
                "model": "obsolete-model",
                "model_display": "Obsolete model",
                "reasoning": "obsolete-reasoning",
            })
            state["agents"]["1"] = agent
            state["next_agent_id"] = 2

        self._mutate_state(populate)
        response = self._call(command="status")
        self.assertTrue(response["ok"], response)
        state = self._state()
        agent = state["agents"]["1"]
        config = subject.TIER_CONFIG[str(agent["tier"])]
        for field in ("provider", "model", "model_display", "reasoning", "cost"):
            self.assertEqual(agent[field], config[field])
        self.assertEqual(
            state["recovery_history"][-1]["kind"],
            "canonical_tier_metadata",
        )

    def test_unreadable_primary_is_quarantined_and_restored_from_backup(self) -> None:
        self._initialize_root()
        expected = subject.SUBAGENT_STATE_BACKUP_FILE.read_bytes()
        corrupt = b'{"schema_version": 1, definitely-not-json'
        subject.SUBAGENT_STATE_FILE.write_bytes(corrupt)

        response = self._call(command="status")

        self.assertTrue(response["ok"], response)
        restored = json.loads(subject.SUBAGENT_STATE_FILE.read_text(encoding="utf-8"))
        self.assertEqual(restored["recovery_history"][-1]["kind"], "backup_restore")
        quarantined = list(subject.SUBAGENT_STATE_DIR.glob("state.corrupt.*.json"))
        self.assertEqual(len(quarantined), 1)
        self.assertEqual(quarantined[0].read_bytes(), corrupt)
        self.assertNotEqual(subject.SUBAGENT_STATE_FILE.read_bytes(), expected)

    def test_corrupt_state_fails_closed_without_overwriting_evidence(self) -> None:
        subject.SUBAGENT_STATE_DIR.mkdir(parents=True, exist_ok=True)
        original = b'{"schema_version": 1, definitely-not-json'
        subject.SUBAGENT_STATE_FILE.write_bytes(original)
        response = self._call(command="status")
        self.assertFalse(response["ok"])
        self.assertEqual(response["error_code"], "STATE_CORRUPT")
        self.assertEqual(subject.SUBAGENT_STATE_FILE.read_bytes(), original)
        self.assertEqual(self.launches, [])


if __name__ == "__main__":
    unittest.main()
