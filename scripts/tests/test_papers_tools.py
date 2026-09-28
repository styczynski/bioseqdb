"""Focused tests for the thesis build and PDF-inspection MCP tools."""

from pathlib import Path
import json
import tempfile
import time
import unittest
from unittest import mock

import build_worker
import process_state
import protocol


_MINIMAL_PNG_HEADER = (
    b"\x89PNG\r\n\x1a\n"
    b"\x00\x00\x00\rIHDR"
    b"\x00\x00\x00\x02"
    b"\x00\x00\x00\x03"
)


class PapersBuildTests(unittest.TestCase):
    def test_tool_definitions_expose_both_papers_tools(self):
        definitions = {
            definition["name"]: definition
            for definition in protocol.TOOL_DEFINITIONS
        }

        self.assertIn("papers-build", definitions)
        self.assertIn("papers-inspect", definitions)
        self.assertEqual(
            definitions["papers-build"]["inputSchema"]["properties"][
                "action"]["enum"],
            ["start", "status"],
        )
        self.assertEqual(
            definitions["papers-inspect"]["inputSchema"]["properties"][
                "pages"]["maxItems"],
            20,
        )

    def test_worker_invokes_cmake_configure_and_thesis_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            papers_dir = root / "papers"
            papers_dir.mkdir()
            (papers_dir / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.30)\n",
                encoding="utf-8",
            )
            build_dir = papers_dir / "build"
            pdf_path = papers_dir / "pdf" / "bioseqdb_thesis.pdf"
            log_path = root / "papers.log"
            state_path = root / "state.json"
            state_path.write_text(json.dumps({
                build_worker.PAPERS_BUILD_STATE_KEY: {
                    "status": "running",
                    "step": "spawning",
                    "started_at": time.time(),
                    "pid": 123,
                    "log_file": str(log_path),
                },
            }), encoding="utf-8")
            commands = []

            def run_command(command, log_handle, deadline, cwd=None, env=None):
                commands.append((command, cwd))
                if "--build" in command:
                    pdf_path.parent.mkdir(parents=True)
                    pdf_path.write_bytes(b"%PDF-test")
                return 0

            patches = (
                mock.patch.object(build_worker, "PAPERS_DIR", papers_dir),
                mock.patch.object(build_worker, "PAPERS_BUILD_DIR", build_dir),
                mock.patch.object(build_worker, "PAPERS_PDF_PATH", pdf_path),
                mock.patch.object(process_state, "STATE_FILE", state_path),
                mock.patch.object(
                    build_worker, "_refuse_root_build_identity",
                    return_value=None,
                ),
                mock.patch.object(
                    build_worker, "_ensure_build_worker_timesharing_policy"),
                mock.patch.object(
                    build_worker.shutil, "which", return_value="/usr/bin/cmake"),
                mock.patch.object(
                    build_worker, "_stream_subprocess", side_effect=run_command),
                mock.patch.object(process_state, "LOCK_FILE", root / "state.lock"),
            )
            with patches[0], patches[1], patches[2], patches[3], \
                    patches[4], patches[5], patches[6], patches[7], patches[8]:
                result = build_worker._run_papers_build_worker()

            self.assertEqual(result, 0)
            self.assertEqual(commands[0][0], [
                "/usr/bin/cmake",
                "-S", str(papers_dir),
                "-B", str(build_dir),
            ])
            self.assertEqual(
                commands[1][0][
                    commands[1][0].index("--target") + 1],
                "bioseqdb_thesis",
            )
            state = json.loads(state_path.read_text(encoding="utf-8"))
            self.assertEqual(
                state[build_worker.PAPERS_BUILD_STATE_KEY]["status"],
                "completed",
            )

    def test_dispatcher_preserves_rich_inspection_content(self):
        expected = {
            "content": [
                {"type": "text", "text": "{}"},
                {
                    "type": "image",
                    "data": "AA==",
                    "mimeType": "image/png",
                },
            ],
            "isError": False,
        }
        with mock.patch.object(
                protocol, "tool_papers_inspect", return_value=expected):
            actual = protocol._handle_tools_call({
                "name": "papers-inspect",
                "arguments": {},
            })

        self.assertIs(actual, expected)


class PapersInspectTests(unittest.TestCase):
    def test_latex_log_extracts_layout_and_reference_problems(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_path = Path(temporary) / "main.log"
            log_path.write_text(
                r"Overfull \hbox (12.5pt too wide) in paragraph" "\n"
                "[] context\n"
                r"Underfull \vbox (badness 10000)" "\n"
                "LaTeX Warning: Citation `missing' undefined.\n"
                "! LaTeX Error: Broken test.\n",
                encoding="utf-8",
            )

            diagnostics = build_worker._papers_log_diagnostics(log_path)

        self.assertEqual(
            diagnostics["overfull_boxes"][0]["overflow_pt"], 12.5)
        self.assertEqual(diagnostics["underfull_box_count"], 1)
        self.assertEqual(diagnostics["undefined_reference_count"], 1)
        self.assertEqual(len(diagnostics["latex_errors"]), 1)

    def test_cached_text_bounds_find_words_outside_media_box(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pdf_path = root / "paper.pdf"
            pdf_path.write_bytes(b"%PDF-test")
            output_dir = root / "inspect"
            output_dir.mkdir()
            bbox_path = output_dir / "text-bounds.html"
            bbox_path.write_text(
                "<html><body><doc>"
                "<page width='100' height='200'>"
                "<word xMin='10' yMin='10' xMax='30' yMax='20'>ok</word>"
                "</page>"
                "<page width='100' height='200'>"
                "<word xMin='95' yMin='10' xMax='105' yMax='20'>clip</word>"
                "</page>"
                "</doc></body></html>",
                encoding="utf-8",
            )

            with mock.patch.object(
                    build_worker.shutil, "which",
                    return_value="/usr/bin/pdftotext"):
                pages, command = build_worker._papers_text_bounds(
                    pdf_path, output_dir)

        self.assertTrue(command["cached"])
        self.assertEqual(pages[0]["outside_text_box_count"], 0)
        self.assertEqual(pages[1]["outside_text_box_count"], 1)
        self.assertEqual(
            pages[1]["outside_text_boxes"][0]["text"], "clip")

    def test_render_manifest_has_page_paths_and_png_dimensions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pdf_path = root / "paper.pdf"
            pdf_path.write_bytes(b"%PDF-test")
            output_dir = root / "inspect"

            def render(command, timeout=60.0):
                Path(command[-1]).with_suffix(".png").write_bytes(
                    _MINIMAL_PNG_HEADER)
                return {
                    "command": command,
                    "exit_code": 0,
                    "stdout": "",
                    "stderr": "",
                }

            with mock.patch.object(
                    build_worker.shutil, "which",
                    return_value="/usr/bin/pdftoppm"), mock.patch.object(
                        build_worker, "_run_capture", side_effect=render):
                manifest, errors = build_worker._render_papers_pages(
                    pdf_path, [2, 4], 160, output_dir)

        self.assertEqual(errors, [])
        self.assertEqual(
            [item["page"] for item in manifest], [2, 4])
        self.assertEqual(manifest[0]["width_px"], 2)
        self.assertEqual(manifest[0]["height_px"], 3)

    def test_inspection_returns_json_manifest_then_page_images(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pdf_path = root / "bioseqdb_thesis.pdf"
            pdf_path.write_bytes(b"%PDF-test")
            image_path = root / "page-0001.png"
            image_path.write_bytes(_MINIMAL_PNG_HEADER)
            log_path = root / "main.log"
            log_path.write_text("", encoding="utf-8")
            manifest = [{
                "page": 1,
                "path": str(image_path),
                "bytes": image_path.stat().st_size,
                "width_px": 2,
                "height_px": 3,
                "dpi": 48,
            }]
            pdfinfo_result = {
                "command": ["pdfinfo", str(pdf_path)],
                "exit_code": 0,
                "stdout": "Pages: 1\n",
                "stderr": "",
            }

            with mock.patch.object(
                    build_worker, "PAPERS_PDF_PATH", pdf_path), mock.patch.object(
                        build_worker, "PAPERS_LATEX_LOG_PATH", log_path), \
                    mock.patch.object(
                        build_worker, "PAPERS_INSPECT_ROOT", root / "inspect"), \
                    mock.patch.object(
                        build_worker, "_papers_pdf_info",
                        return_value=({"Pages": "1"}, pdfinfo_result)), \
                    mock.patch.object(
                        build_worker, "_papers_text_bounds",
                        return_value=([{
                            "page": 1,
                            "width_pt": 100.0,
                            "height_pt": 200.0,
                            "word_count": 1,
                            "outside_text_box_count": 0,
                            "outside_text_boxes": [],
                        }], {"exit_code": 0})), \
                    mock.patch.object(
                        build_worker, "_render_papers_pages",
                        return_value=(manifest, [])), \
                    mock.patch.object(
                        build_worker, "_papers_source_freshness",
                        return_value={
                            "stale": False,
                            "newer_sources": [],
                        }):
                result = build_worker.tool_papers_inspect({})

        self.assertFalse(result["isError"])
        self.assertEqual(
            [item["type"] for item in result["content"]],
            ["text", "image"],
        )
        payload = json.loads(result["content"][0]["text"])
        self.assertEqual(payload["render"]["embedded_pages"], [1])
        self.assertTrue(payload["visual_review_required"])


if __name__ == "__main__":
    unittest.main()
