#!/bin/bash

# Check library
if [ ! -f "../../build/libwfa.a" ]; then
  echo "Library libwfa.a not found. Please compile WFA library from top folder first"
  exit 
fi

# Compile examples
gcc -O3 -I../.. -L../../build wfa_basic.c -o wfa_basic -lwfa -lm
gcc -O3 -I../.. -L../../build wfa_repeated.c -o wfa_repeated -lwfa -lm
gcc -O3 -I../.. -L../../build wfa_adapt.c -o wfa_adapt -lwfa -lm
