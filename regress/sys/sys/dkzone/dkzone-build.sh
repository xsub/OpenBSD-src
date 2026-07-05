#!/bin/sh
set -eu

cd "$(dirname "$0")"

make clean
make obj
make regress
