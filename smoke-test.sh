#!/bin/sh
# Smoke test for nullfs.  Run it against an already mounted instance:
#
#     ./nullfs -f /mnt/sink &
#     ./smoke-test.sh /mnt/sink
#
# It only uses ordinary POSIX tools; exit status is the number of failures.

set -u
MNT=${1:?usage: $0 <mountpoint>}
MNT=${MNT%/}
fails=0
pass() { printf 'ok   %s\n' "$1"; }
fail() { printf 'FAIL %s\n' "$1"; fails=$((fails + 1)); }
check() { if [ "$2" = "$3" ]; then pass "$1"; else fail "$1 (got '$2', want '$3')"; fi; }

echo "== basic tree =="
if mkdir -p "$MNT/a/b/c/d" 2>/dev/null; then pass "mkdir -p"; else fail "mkdir -p"; fi
if [ -d "$MNT/a/b/c/d" ]; then pass "directory is visible"; else fail "directory is visible"; fi

echo "== writes are accepted and sized =="
head -c 1048576 /dev/zero > "$MNT/a/b/c/d/f1.bin" 2>/dev/null
check "1 MiB file size" "$(stat -c %s "$MNT/a/b/c/d/f1.bin")" "1048576"
check "no blocks allocated" "$(stat -c %b "$MNT/a/b/c/d/f1.bin")" "0"

if dd if=/dev/zero of="$MNT/big.bin" bs=1M count=64 status=none 2>/dev/null; then
	pass "dd 64 MiB"
else
	fail "dd 64 MiB"
fi
check "truncate grows size" "$(truncate -s 32M "$MNT/sparse.bin" 2>/dev/null; stat -c %s "$MNT/sparse.bin" 2>/dev/null)" "33554432"

echo "== reads =="
# Reads may be served from the page cache right after a write; once they do
# reach nullfs they return zeroes.  Either way reading must never fail.
if head -c 4096 "$MNT/a/b/c/d/f1.bin" >/dev/null 2>&1; then
	pass "read succeeds"
else
	fail "read succeeds"
fi

echo "== reported free space =="
avail=$(df -P "$MNT" | awk 'NR==2 {print $4}')
if [ "${avail:-0}" -gt 100000000 ]; then pass "df reports a huge volume ($avail KiB)"; else fail "df reports a huge volume ($avail)"; fi

echo "== namespace bookkeeping =="
n=0
while [ $n -lt 200 ]; do : > "$MNT/f$n"; n=$((n + 1)); done
check "200 files enumerated" "$(ls "$MNT" | grep -c '^f[0-9]')" "200"
rm -f "$MNT"/f[0-9]*
check "files removed" "$(ls "$MNT" | grep -c '^f[0-9]')" "0"

echo "== unlink while open (gcc/sqlite pattern) =="
tmpf="$MNT/orphan.bin"
if ( exec 9>"$tmpf" && rm -f "$tmpf" && printf 'still writable' >&9 ); then
	pass "write after unlink"
else
	fail "write after unlink"
fi
check "name is gone" "$([ -e "$tmpf" ] && echo yes || echo no)" "no"

echo "== rename =="
: > "$MNT/r1"
mv "$MNT/r1" "$MNT/r2" 2>/dev/null
check "rename moves the name" "$([ -e "$MNT/r2" ] && [ ! -e "$MNT/r1" ] && echo yes || echo no)" "yes"
rm -f "$MNT/r2"

echo "== cleanup =="
if rm -rf "$MNT/a" "$MNT/big.bin" "$MNT/sparse.bin" 2>/dev/null; then pass "rm -rf"; else fail "rm -rf"; fi

echo
if [ "$fails" -eq 0 ]; then
	echo "all smoke tests passed"
	exit 0
fi
echo "$fails test(s) failed"
exit "$fails"
