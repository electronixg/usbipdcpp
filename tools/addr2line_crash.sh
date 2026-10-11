#!/usr/bin/env bash
# Symbolize the address stack in a crash log as file:line number.
#
# Usage:
#   cat crash.log | tools/addr2line_crash.sh
#   tools/addr2line_crash.sh crash.log
#
# Supports two frame formats (the backtrace printed by the tests' crash handler):
#   [/path/module(+0x1d0ef) [0x...]      <- Without symbols: offset is relative to the module; use addr2line directly
#   [/path/module(symbol+0x20) [0x...]   <- With symbols: offset is relative to the symbol; use nm to convert to an absolute VMA
#
# Parse module paths from the log (works on runners and locally), provided the module binaries are still on
# the machine that produced the input log.

set -u

log_file="${1:-/dev/stdin}"

# Cache already parsed module lines to avoid repeated lookups of the same address
declare -A seen

resolve() {
    local frame="$1"
    local module inner
    module=$(echo "$frame" | sed -E 's/\(.*//')
    inner=$(echo "$frame" | sed -E 's/^[^(]*\(([^)]*)\).*/\1/')

    [ "$inner" = "nil" ] && return 0
    [ -f "$module" ] || { echo "  Module does not exist: $module"; return 1; }

    if [[ "$inner" == +0x* ]]; then
        # Without symbols: +offset is relative to the module; use it directly with addr2line
        addr2line -f -C -e "$module" "0x${inner#+}" 2>/dev/null || echo "  addr2line failed"
        return
    fi

    # With symbols: inner = symbol+0xoffset; use nm to find the symbol VMA, then calculate the absolute address
    local sym="${inner%%+*}"
    local off="${inner#*+}"
    # Exported symbols from nm have version suffixes (such as pthread_detach@@GLIBC_2.34); match by the undecorated name;
    # Add the 0x address prefix (nm outputs no prefix; bash arithmetic treats leading zeros as octal)
    local sym_addr
    sym_addr=$(nm -D "$module" 2>/dev/null | awk -v s="$sym" '
        index($3, s) == 1 && substr($3, length(s) + 1, 2) == "@@" { print "0x" $1; exit }')
    if [ -z "$sym_addr" ]; then
        sym_addr=$(nm "$module" 2>/dev/null | awk -v s="$sym" '$3 == s { print "0x" $1; exit }')
    fi
    if [ -z "$sym_addr" ]; then
        echo "  Symbol not found: $sym"
        return 1
    fi
    local abs_addr
    abs_addr=$(printf "0x%x" "$((sym_addr + off))")
    addr2line -f -C -e "$module" "$abs_addr" 2>/dev/null || echo "  addr2line failed"
}

# Match frames line by line; frame format:  [n] /path/module(...) [0x...] or  [n] (nil)
while IFS= read -r line; do
    frame=$(echo "$line" | grep -oE '\[[0-9]+\][[:space:]]*[^[:space:]]+\([^)]*\)' | sed -E 's/^\[[0-9]+\][[:space:]]*//')
    [ -n "$frame" ] || continue
    [ -n "${seen[$frame]:-}" ] && continue
    seen[$frame]=1
    echo "=== $frame ==="
    resolve "$frame"
done < "$log_file"
