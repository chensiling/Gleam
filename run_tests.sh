#!/bin/bash
# Gleam atomic-feature verification suite.
# Requires fixed addresses from TestTarget (no ASLR).
set -u
cd "$(dirname "$0")"

GLEAM=./bin/Debug/x64/Gleam.exe
TARGET=bin/Debug/x64/TestTarget.exe
MARKER=140070EC9
INNER=1400708AC
GDATA=140190000

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS: $1"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL: $1"; }
chk() { # chk <desc> <file> <pattern>
  if grep -qF "$3" "$2"; then ok "$1"; else bad "$1"; fi
}
chkre() { # chkre <desc> <file> <regex>
  if grep -qE "$3" "$2"; then ok "$1"; else bad "$1"; fi
}
chkcount() { # chkcount <desc> <file> <pattern> <expected-count>
  local n=$(grep -cF "$3" "$2")
  if [ "$n" -eq "$4" ]; then ok "$1"; else bad "$1 (count=$n, want $4)"; fi
}

run() { # run <name> <target-args> < commands
  local name=$1; shift
  local args=$1; shift
  "$GLEAM" $TARGET $args > /tmp/gleam_$name.txt 2>&1
  echo "== $name =="
}

# --- A: inspection commands + regs/read/write/setreg/step ---
run A "" <<'EOF'
bp 140070EC9
g
disasm
maps
modules
bl
threads
thread
bt
find 140190000 100 47 4C 45 41
regs
read 140190000 10
write 140190000 DE AD BE EF 01 02 03 04 08 09 0A 0B 0C 0D 0E 0F
setreg rcx 64
step
regs
g
g
EOF
chk "A: bp hit at marker"        /tmp/gleam_A.txt "breakpoint hit at 0x0000000140070EC9"
chk "A: disasm at rip"           /tmp/gleam_A.txt "0000000140070EC9"
chkre "A: maps regions"          /tmp/gleam_A.txt "[0-9]+ committed regions"
chk "A: modules has TestTarget"  /tmp/gleam_A.txt "TestTarget.exe"
chk "A: bl lists bp"             /tmp/gleam_A.txt "0x140070EC9  software"
chk "A: two threads"             /tmp/gleam_A.txt "[event]"
chk "A: bt frame0 rip"           /tmp/gleam_A.txt "#0"
chk "A: find pattern"            /tmp/gleam_A.txt "found at 0x140190000"
chk "A: rcx=arg at entry"        /tmp/gleam_A.txt "RCX=0000000000000029"
chk "A: write ok"                /tmp/gleam_A.txt "wrote 16 bytes"
chk "A: result1 modified by rcx" /tmp/gleam_A.txt "MARKER_RESULT_1=106"
chk "A: result2 normal"          /tmp/gleam_A.txt "MARKER_RESULT_2=13"
chk "A: gdata self-write wins b0" /tmp/gleam_A.txt "GDATA_AFTER=58ADBEEF0102030408090A0B0C0D0E0F"
chk "A: exit code 0"             /tmp/gleam_A.txt "exited, code=0x00000000"

# --- B: one-shot breakpoint ---
run B "" <<'EOF'
bp 140070EC9 once
g
bl
g
EOF
chkcount "B: hit exactly once"   /tmp/gleam_B.txt "breakpoint hit" 1
chk "B: bp auto-deleted"         /tmp/gleam_B.txt "no breakpoints"
chk "B: both calls ran"          /tmp/gleam_B.txt "MARKER_RESULT_2=13"

# --- C: ignore count ---
run C "" <<'EOF'
bp 140070EC9
ignore 140070EC9 1
g
g
EOF
chk "C: first hit ignored"       /tmp/gleam_C.txt "ignored (0 left)"
chkcount "C: paused once"        /tmp/gleam_C.txt "[gleam] paused" 2
chk "C: exit 0"                  /tmp/gleam_C.txt "exited, code=0x00000000"

# --- D: hardware write breakpoint ---
run D "" <<'EOF'
bp 140070EC9
g
rbp 140070EC9
hbp 140190000 w
g
g
EOF
chk "D: hbp set"                 /tmp/gleam_D.txt "hardware breakpoint set at 0x140190000 (w)"
chkcount "D: hbp hit once"       /tmp/gleam_D.txt "hardware breakpoint hit" 1
chk "D: self write done"         /tmp/gleam_D.txt "GDATA_AFTER=584C45414D2D544553542D4441544121"

# --- E: memory write breakpoint ---
run E "" <<'EOF'
mbp 140190000 10 w
g
g
EOF
chk "E: mbp set"                 /tmp/gleam_E.txt "memory breakpoint set at 0x140190000"
chkcount "E: mbp hit once"       /tmp/gleam_E.txt "memory breakpoint hit" 1

# --- F: unhandled exception + exinfo ---
run F "exc" <<'EOF'
g
exinfo
quit
EOF
chk "F: unhandled exception"     /tmp/gleam_F.txt "unhandled exception (first chance) code=0xE0DEAD00"
chk "F: exinfo code"             /tmp/gleam_F.txt "code=0xE0DEAD00"

# --- G: exception filter ---
run G "exc" <<'EOF'
ignoreexc E0DEAD00
g
EOF
chk "G: exception ignored"       /tmp/gleam_G.txt "exception 0xE0DEAD00"
chk "G: survived"                /tmp/gleam_G.txt "SURVIVED_EXCEPTION"
chk "G: exit 0"                  /tmp/gleam_G.txt "exited, code=0x00000000"

# --- H: ret (step out) ---
run H "" <<EOF
bp $INNER
g
step
ret
regs
g
g
EOF
chk "H: bp inner hit"            /tmp/gleam_H.txt "breakpoint hit at 0x00000001400708AC"
chk "H: stepping out"            /tmp/gleam_H.txt "stepping out to 0x"
chk "H: result1 normal"          /tmp/gleam_H.txt "MARKER_RESULT_1=47"

# --- I: stepover ---
run I "" <<'EOF'
bp 140070EC9
g
step
stepover
stepover
stepover
stepover
stepover
stepover
stepover
regs
g
g
EOF
chk "I: stepped over the call"   /tmp/gleam_I.txt "stepped over to 0x0000000140076B8F"
chk "I: result1 normal"          /tmp/gleam_I.txt "MARKER_RESULT_1=47"

# --- J: detach ---
run J "" <<'EOF'
bp 140070EC9
g
detach
EOF
chk "J: detaching"               /tmp/gleam_J.txt "detaching..."
chk "J: target ran free"         /tmp/gleam_J.txt "MARKER_RESULT_1=47"
chk "J: session finished"        /tmp/gleam_J.txt "session finished"

# --- K: symbols (imports/exports) ---
run K "" <<'EOF'
imports
exports kernel32 CreateFile*
g
EOF
chk "K: imports kernel32 group"  /tmp/gleam_K.txt "KERNEL32.dll:"
chk "K: import names resolved"   /tmp/gleam_K.txt "CreateThread"
chk "K: exports wildcard filter" /tmp/gleam_K.txt "CreateFileW"
chk "K: exports summary"         /tmp/gleam_K.txt "symbols"

echo
echo "PASS=$PASS FAIL=$FAIL"
