#!/bin/python3
#
#  This is the entrypoin to the applciation.
#  The netrypoint should install poetry if required, execute "poetry install" and then execute "poetry run aadg_genomics_class/cli.py" to run the entrypoint application.
#
#  @Piotr Styczyński 2023 <piotr@styczynski.in>
#  MIT LICENSE
#  Algorithms for genomic data analysis | AADG | MIM UW | Bioinformatyka
#

import os
import sys
import subprocess

def install_poetry():
    missing = False
    try:
        rc = subprocess.call(['poetry', '--version'])
        missing = (rc != 0)
    except:
        missing = True
    if missing:
        print("Installing poetry (detected it's missing on the host system)")
        os.system("curl -sSL https://install.python-poetry.org | python3 -")

def call_poetry(*args):
    os.system(" ".join(["poetry", *args]))

install_poetry()
call_poetry("install")
call_poetry("run", "python3", "aadg_genomics_class/cli.py", *sys.argv[1:])
