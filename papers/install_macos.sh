#!/bin/bash
set -e

brew install pandoc
#brew tap homebrew/cask
brew install --cask basictex
eval "$(/usr/libexec/path_helper)"
# Update $PATH to include `/usr/local/texlive/2022basic/bin/universal-darwin`
sudo tlmgr update --self
sudo tlmgr install texliveonfly
sudo tlmgr install xelatex
sudo tlmgr install adjustbox
sudo tlmgr install tcolorbox
sudo tlmgr install collectbox
sudo tlmgr install ucs
sudo tlmgr install environ
sudo tlmgr install trimspaces
sudo tlmgr install titling
sudo tlmgr install enumitem
sudo tlmgr install rsfs