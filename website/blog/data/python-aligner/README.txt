Python aligner chart sources

local-profiles.csv is a lossless row-oriented transcription of the project JSON, with explicit metric units.
course-runtime.csv and course-ps-memory.csv are APPROXIMATE digitizations of raster figures, not original timing logs.
course-runtime.csv covers ten submissions the author considered valid general-purpose implementations under the course rules; it is a selected comparison, not a transcription of every series in the original PDF.
course-ps-quality.csv contains the readable integer PS counts from the plotted markers.
See sources.json for units, assumptions, file hashes, report pages and limitations.

Regenerate figures from the repository root with:
python3 website/tools/generate_python_aligner_figures.py
Requires matplotlib; no network or benchmark execution is needed.
