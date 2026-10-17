#!/bin/sh
set -eu

saihang-bms read-all --timeout "${1:-8}"
