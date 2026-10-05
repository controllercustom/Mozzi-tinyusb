#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 controllercustom@myyahoo.com
# Part of Mozzi-tinyusb. See LICENSE at the repository root.

# run_tests.sh — offline tests. No hardware needed.
#
# Two groups:
#
#   1. This repo's own check: the SPDX licence sweep. Nothing else in the build
#      notices a missing header, so without something invoking it the MIT sweep
#      rots silently. tools/add_license_headers.py --check exits non-zero and
#      names the files. scripts/avenv.sh is excluded by the script itself, being
#      vendored code of unstated licence.
#
#   2. The pinned Uac2Bridge's own suite, run from libs/. Uac2Bridge was
#      extracted from this repo into its own project so other USB-audio authors
#      can use the rate converter on its own, which means its tests and its
#      licence check live there now. Delegating rather than forking them keeps
#      one copy, and it means bumping the pin picks up the new tests.
#
# Its sweep_design.sh is not run: that regenerates the filter table and is a
# design tool, not a check.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD

fail=0

echo "== checking SPDX licence headers =="
if python3 tools/add_license_headers.py --check; then
  echo "PASS license headers"
else
  echo "FAIL license headers (run: python3 tools/add_license_headers.py)"
  fail=1
fi

echo
if [[ ! -f libs/Uac2Bridge/run_tests.sh ]]; then
  echo "FAIL libs/Uac2Bridge is missing or incomplete -- run ./scripts/setup_libs.sh" >&2
  exit 1
fi
echo "== running the pinned Uac2Bridge suite =="
# Invoked via bash rather than executed, so a lost exec bit (zip extract, a
# filesystem that does not carry permissions) cannot turn into a confusing
# "permission denied" instead of a test result.
if bash libs/Uac2Bridge/run_tests.sh; then
  echo "PASS Uac2Bridge suite"
else
  echo "FAIL Uac2Bridge suite"
  fail=1
fi

echo
if [[ $fail -eq 0 ]]; then
  echo "ALL OFFLINE TESTS PASS"
else
  echo "SOME OFFLINE TESTS FAILED"
fi
exit $fail