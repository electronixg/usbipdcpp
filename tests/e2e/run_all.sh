#!/usr/bin/env bash
# Top-level e2e script: run each device test script under tests/e2e/ in turn; continue after failures (see full picture),
# then exit nonzero according to failure count. Each script cleans its own server/device
set -u

E2E_DIR="$(cd "$(dirname "$0")" && pwd)"
FAILED=""

# Run test_*.sh sequentially (shell sort determines execution order)
for t in "$E2E_DIR"/test_*.sh; do
    [ -e "$t" ] || continue
    name=$(basename "$t" .sh)
    echo ""
    echo "==================== $name ===================="
    if bash "$t"; then
        echo "[$name] OK"
    else
        echo "[$name] FAILED"
        FAILED="$FAILED $name"
    fi
done

echo ""
echo "=============================================="
if [ -n "$FAILED" ]; then
    echo "FAILED:$FAILED"
    exit 1
fi
echo "All e2e tests passed"
