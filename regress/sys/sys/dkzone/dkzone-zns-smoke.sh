#!/bin/sh
set -eu

cd "$(dirname "$0")"
exec ./dkzone-zone-management.sh "$@"
