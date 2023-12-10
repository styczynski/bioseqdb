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
    missing = False
    try:
        rc = subprocess.call(['poetry', '--version'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        missing = (rc != 0)
    except:
        missing = True
    return missing

def call_poetry(*args):
    os.system(" ".join(["poetry", *args]))

is_missing = install_poetry()
if is_missing:
    print(" -- OH NOES! --")
    print("| There's a problem with the Poetry installation.")
    print("| Please restart the command line or install Poetry manually")
    print("| You can do it by going to: https://python-poetry.org/docs/#installing-with-pipx")
    print("| Sorry for the inconvenience")
    print("")
    sys.exit(1)
call_poetry("install")
call_poetry("run", "python3", "aadg_genomics_class/cli.py", *sys.argv[1:])
