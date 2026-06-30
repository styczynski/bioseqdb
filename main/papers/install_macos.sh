#!/bin/bash
set -e

# macOS setup for building the LaTeX papers (the `latex-all` target / the
# build_mcp `papers-build` tool). Installs BasicTeX plus every TeX package the
# documents under src/content/**/main.tex actually use. The previous version of
# this script installed only ~10 packages and left a fresh machine unable to
# build; this list was derived empirically by building all documents until no
# package was missing.
#
# Python dependencies (matplotlib/numpy/pandas/pympler/Pygments/latexminted for
# the embedded robust-externalize + minted figures) are NOT installed here — the
# build creates its own venv from requirements.txt. You only need a Python >=3.10
# on PATH (papers-build auto-selects the newest of python3.10..python3.13).

brew install pandoc
brew install --cask basictex

# Make the freshly-installed TeX binaries visible on PATH in this shell.
eval "$(/usr/libexec/path_helper)"

sudo tlmgr update --self

# One install for the whole set (tlmgr pulls transitive dependencies itself).
sudo tlmgr install \
  texliveonfly xelatex \
  luacode \
  biblatex biber \
  collection-langcyrillic \
  adjustbox collectbox ucs trimspaces titling enumitem rsfs environ \
  algorithms algorithmicx algpseudocodex \
  forest smartdiagram tkz-berge bytefield robust-externalize \
  tcolorbox tabularray verbatimbox \
  minted \
  ifmtarg lipsum nowidow relsize soul stmaryrd hyperxmp filecontents \
  fourier heuristica emoji \
  texshade

# Refresh the filename database so newly-installed styles/encodings (e.g. the
# T2A cyrillic encoding pulled in by babel) are found by kpsewhich.
sudo mktexlsr

# Bulletproof alternative: if a document later pulls in a package not listed
# above, install everything in one shot instead of chasing individual packages:
#   sudo tlmgr install scheme-full
