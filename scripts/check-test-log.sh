#!/usr/bin/env bash
# Fail when a captured test log contains a sanitizer report.
#
# Why: forked test children and halt_on_error=0 style settings can let a
# sanitizer report scroll by while ctest still exits 0. Scanning the log makes
# any report a hard CI failure. TSan reports silenced through test/tsan.supp
# are never printed, so they cannot trip this check.
#
# Usage: scripts/check-test-log.sh <logfile>...
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <logfile>..." >&2
    exit 2
fi

# One extended regex per sanitizer; keep in sync with docs/ci.md.
patterns='ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|WARNING: ThreadSanitizer|SUMMARY: .*Sanitizer'

status=0
for log in "$@"; do
    if [ ! -r "$log" ]; then
        # A missing log means the capture step broke: do not pass silently.
        echo "check-test-log: cannot read $log" >&2
        status=2
        continue
    fi
    if hits="$(grep -nE -- "$patterns" "$log")"; then
        echo "check-test-log: sanitizer report(s) in $log:" >&2
        printf '%s\n' "$hits" | head -20 >&2
        status=1
    else
        echo "check-test-log: $log clean"
    fi
done
exit "$status"
