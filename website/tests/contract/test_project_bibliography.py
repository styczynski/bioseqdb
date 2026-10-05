"""Regression checks for authored names and notes lost by CSL name parsing."""
from pathlib import Path
import importlib.util
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "project_importer", ROOT / "website/tools/import_project_chapters.py")
IMPORTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(IMPORTER)


class BibliographySourceFidelity(unittest.TestCase):
    def test_original_author_names_and_notes_survive_csl_parsing(self):
        # These are the actual source patterns that CSL interpreted as one
        # person's given/family/suffix fields, or split away a family particle.
        authors = (
            "Sadasivan H, Maric M, Dawson E et all",
            "Saurabh Kalikar, Chirag Jain, Md Vasimuddin et all",
            "Juliusz K, Mateusz C, Szymon D and Damian W",
            "NBIS - National Bioinformatics Infrastructure Sweden",
            "de Miranda, Antonio B. and de Vasconcelos, Ana Tereza R.",
        )
        note = "Function mm_set_mapq2; commit 3c28777e7e2dcc90f825de1b9f17a89cca7d4452"
        raw = "\n".join(
            "@online{source" + str(index) + ",\n"
            "  author = {" + author + "},\n"
            "  title = {Source reference},\n"
            "  note = {" + note + "}\n}"
            for index, author in enumerate(authors))
        with mock.patch.object(IMPORTER, "read_source", return_value=raw):
            output = IMPORTER.bibliography()
        for index, author in enumerate(authors, 1):
            punctuation = "" if author.endswith(".") else "."
            self.assertIn(f"**[{index}]** {author}{punctuation} ", output)
        self.assertEqual(output.count(note), len(authors))


if __name__ == "__main__":
    unittest.main()
