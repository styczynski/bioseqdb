#!/bin/python3

import os
import sys

def call_poetry(*args):
    os.system(" ".join(["poetry", *args]))

call_poetry("install")
call_poetry("run", "python3", "aadg_genomics_class/cli.py", *sys.argv[1:])
