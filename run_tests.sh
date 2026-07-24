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
  timeout 60 "$GLEAM" $TARGET $args > /tmp/gleam_$name.txt 2>&1
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
chk "A: bp hit at marker"        /tmp/gleam_A.txt "stop reason=breakpoint type=software address=0x140070EC9"
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
chk "A: exit code 0"             /tmp/gleam_A.txt "stop reason=exit code=0x00000000"

# --- B: one-shot breakpoint ---
run B "" <<'EOF'
bp 140070EC9 once
g
bl
g
EOF
chkcount "B: hit exactly once"   /tmp/gleam_B.txt "stop reason=breakpoint" 1
chk "B: bp auto-deleted"         /tmp/gleam_B.txt "no breakpoints"
chk "B: both calls ran"          /tmp/gleam_B.txt "MARKER_RESULT_2=13"

# --- C: ignore count ---
run C "" <<'EOF'
bp 140070EC9
ignore 140070EC9 1
g
g
EOF
chk "C: first hit ignored"       /tmp/gleam_C.txt "event ignored address=0x140070EC9 left=0"
chkcount "C: bp stop once"       /tmp/gleam_C.txt "stop reason=breakpoint" 1
chk "C: exit 0"                  /tmp/gleam_C.txt "stop reason=exit code=0x00000000"

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
chkcount "D: hbp hit once"       /tmp/gleam_D.txt "stop reason=breakpoint type=hardware" 1
chk "D: self write done"         /tmp/gleam_D.txt "GDATA_AFTER=584C45414D2D544553542D4441544121"

# --- E: memory write breakpoint ---
run E "" <<'EOF'
mbp 140190000 10 w
g
g
EOF
chk "E: mbp set"                 /tmp/gleam_E.txt "memory breakpoint set at 0x140190000"
chkcount "E: mbp hit once"       /tmp/gleam_E.txt "stop reason=breakpoint type=memory" 1

# --- F: unhandled exception + exinfo ---
run F "exc" <<'EOF'
g
exinfo
quit
EOF
chk "F: unhandled exception"     /tmp/gleam_F.txt "stop reason=exception code=0xE0DEAD00"
chk "F: exinfo code"             /tmp/gleam_F.txt "code=0xE0DEAD00"

# --- G: exception filter ---
run G "exc" <<'EOF'
ignoreexc E0DEAD00
g
EOF
chk "G: exception passed"        /tmp/gleam_G.txt "action=passed-to-debuggee"
chk "G: survived"                /tmp/gleam_G.txt "SURVIVED_EXCEPTION"
chk "G: exit 0"                  /tmp/gleam_G.txt "stop reason=exit code=0x00000000"

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
chk "H: bp inner hit"            /tmp/gleam_H.txt "stop reason=breakpoint type=software address=0x1400708AC"
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
chk "I: stepped over the call"   /tmp/gleam_I.txt "stop reason=step rip=0x140076B8F"
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

# --- L: symbol breakpoint + breakon switches ---
run L "" <<'EOF'
breakon
breakon entry on
breakon thread on
breakon dll on
bp TestTarget!marker
g
g
g
g
g
g
quit
EOF
chk "L: default exception on"    /tmp/gleam_L.txt "breakon exception=on"
chk "L: OEP entry stop"          /tmp/gleam_L.txt "stop reason=entry address=0x1400720EE"
chk "L: thread create stop"      /tmp/gleam_L.txt "stop reason=thread op=create"
chk "L: thread start named"      /tmp/gleam_L.txt "name=worker"
chk "L: dll load stop"           /tmp/gleam_L.txt "stop reason=dll op=load"
chk "L: symbol bp hit (real body)" /tmp/gleam_L.txt "stop reason=breakpoint type=software address=0x140076B70"

# --- M: breakon exception off ---
run M "exc" <<'EOF'
breakon exception off
g
EOF
chk "M: no exception stop"       /tmp/gleam_M.txt "SURVIVED_EXCEPTION"
chk "M: exit 0"                  /tmp/gleam_M.txt "stop reason=exit code=0x00000000"
chkcount "M: only 2 stops"       /tmp/gleam_M.txt "stop reason=" 2

# --- N1: hide (anti-anti-debug) ---
run N1 "" <<'EOF'
hide
g
EOF
chk "N1: hidden from IsDebuggerPresent" /tmp/gleam_N1.txt "ISDEBUGGERPRESENT=0"

# --- N2: tracepoint ---
run N2 "" <<'EOF'
trace 140070EC9
g
EOF
chkcount "N2: two trace lines"   /tmp/gleam_N2.txt "trace address=0x140070EC9" 2
chkcount "N2: no bp pause"       /tmp/gleam_N2.txt "stop reason=breakpoint" 0

# --- N3: conditional breakpoint ---
run N3 "" <<'EOF'
bp 140070EC9 if rcx==7
g
g
EOF
chkcount "N3: only rcx==7 pauses" /tmp/gleam_N3.txt "stop reason=breakpoint" 1

# --- N4: sym / stackscan / find-ascii / patch / until ---
run N4 "" <<'EOF'
bp 140070EC9
g
sym 140076B70
stackscan 10
find 140190000 100 ascii GLEAM
patch 140190000 AA BB
patches
restore 140190000
patches
until 140076B70
g
g
EOF
chk "N4: sym resolves marker"    /tmp/gleam_N4.txt "marker"
chk "N4: stackscan finds main"   /tmp/gleam_N4.txt "main"
chk "N4: find ascii"             /tmp/gleam_N4.txt "found at 0x140190000"
chk "N4: patched"                /tmp/gleam_N4.txt "patched 0x140190000 (2 bytes)"
chk "N4: restored"               /tmp/gleam_N4.txt "restored 0x140190000"
chk "N4: list empty after restore" /tmp/gleam_N4.txt "no patches"
chk "N4: until hits"             /tmp/gleam_N4.txt "stop reason=breakpoint type=software address=0x140076B70"
chk "N4: restore kept data"      /tmp/gleam_N4.txt "GDATA_AFTER=584C45414D2D544553542D4441544121"

# --- O1: alloc/protect ---
run O1 "" <<'EOF'
alloc 1000
protect 140190000 100 rw
g
EOF
chk "O1: allocated"              /tmp/gleam_O1.txt "allocated 0x"
chk "O1: protected"              /tmp/gleam_O1.txt "protected 0x140190000"

# --- O2: xref / findasm ---
run O2 "" <<'EOF'
xref 1400708AC
findasm call
g
EOF
chk "O2: xref finds call site"   /tmp/gleam_O2.txt "0x0000000140076B9F  call 0x00000001400708AC"
chk "O2: xref count"             /tmp/gleam_O2.txt "1 references to 0x1400708AC"
chk "O2: findasm indirect call"  /tmp/gleam_O2.txt "call [0x000000014019F008]"

# --- O3: thread suspend/resume (event thread, no tid needed) ---
run O3 "" <<'EOF'
bp 140070EC9
g
thread suspend
thread resume
g
g
EOF
chkre "O3: suspend ok"           /tmp/gleam_O3.txt "suspend thread [0-9]+: ok"
chkre "O3: resume ok"            /tmp/gleam_O3.txt "resume thread [0-9]+: ok"

# --- P1: conditional tracing ---
run P1 "" <<'EOF'
bp 140070EC9
g
tgo rcx==29 100
regs
g
g
EOF
chk "P1: trace stops on condition" /tmp/gleam_P1.txt "stop reason=trace condition steps=1"
chk "P1: rip at real body"       /tmp/gleam_P1.txt "RIP=0000000140076B70"

# --- P2: bp do <command> (non-resume) ---
run P2 "" <<'EOF'
bp 140070EC9 do regs
g
g
g
EOF
chkcount "P2: regs dumped twice" /tmp/gleam_P2.txt "RAX=" 2

# --- P3: bp do g (resume-type, no pause) ---
run P3 "" <<'EOF'
bp 1400708AC do g
g
EOF
chkcount "P3: no bp pause"       /tmp/gleam_P3.txt "stop reason=breakpoint" 0
chk "P3: results correct"        /tmp/gleam_P3.txt "MARKER_RESULT_2=13"

# --- Q1: one-shot bp rule not inherited (re-arm at same addr refires once, expected) ---
run Q1 "" <<'EOF'
bp 1400708AC once do regs
g
bp 1400708AC
g
g
g
EOF
chkcount "Q1: 1 one-shot + 1 refire + 1 call-2" /tmp/gleam_Q1.txt "stop reason=breakpoint" 3
chk "Q1: results correct"        /tmp/gleam_Q1.txt "MARKER_RESULT_2=13"

# --- Q2: breakon entry off disarms OEP breakpoint ---
run Q2 "" <<'EOF'
breakon entry on
breakon entry off
g
EOF
chkcount "Q2: no entry stop"     /tmp/gleam_Q2.txt "stop reason=entry" 0

# --- Q3: double patch restores the true original ---
run Q3 "" <<'EOF'
bp 140070EC9
g
patch 140190000 AA BB
patch 140190000 CC DD
restore 140190000
read 140190000 4
g
g
EOF
chk "Q3: restored to original"   /tmp/gleam_Q3.txt "47 4C 45 41"

# --- Q4: ret via frame pointer ---
run Q4 "" <<'EOF'
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
ret
g
g
EOF
chk "Q4: frame-based return"     /tmp/gleam_Q4.txt "(frame)"

# --- R1: one-shot 'do g' rule fully cleaned ---
run R1 "" <<'EOF'
bp 140070EC9
bp 1400708AC once do g
g
g
bp 1400708AC
g
g
EOF
chkcount "R1: 2 marker + 1 inner stops" /tmp/gleam_R1.txt "stop reason=breakpoint" 3
chk "R1: results correct"        /tmp/gleam_R1.txt "MARKER_RESULT_2=13"

# --- R2: overlapping patches restore fully ---
run R2 "" <<'EOF'
bp 140070EC9
g
patch 140190000 AA
patch 140190000 BB CC
restore 140190000
read 140190000 4
g
g
EOF
chk "R2: both bytes restored"    /tmp/gleam_R2.txt "47 4C 45 41"

# --- R3: double hide then off restores detection ---
run R3 "" <<'EOF'
hide
hide
hide off
g
EOF
chk "R3: detection restored"     /tmp/gleam_R3.txt "ISDEBUGGERPRESENT=1"
chk "R3: skip re-apply"          /tmp/gleam_R3.txt "hide already applied"

# --- R4: ret in Release/FPO function (guarded: needs Release binaries) ---
REL=bin/Release/x64
if [ -f "$REL/Gleam.exe" ] && [ -f "$REL/TestTarget.exe" ]; then
  echo "== R4 =="
  timeout 30 "$REL/Gleam.exe" "$REL/TestTarget.exe" > /tmp/gleam_R4.txt 2>&1 <<'EOF'
bp TestTarget!marker
g
step
stepover
stepover
ret
g
g
EOF
  chk "R4: stack-scan ret"       /tmp/gleam_R4.txt "(stack scan)"
  chk "R4: caller resume"        /tmp/gleam_R4.txt "MARKER_RESULT_1=47"
else
  echo "== R4 == (skipped: no Release binaries)"
fi

# --- R5: new patch bridging two old records ---
run R5 "" <<'EOF'
bp 140070EC9
g
patch 140190000 AA
patch 140190002 CC
patch 140190000 EE EE EE
restore 140190000
read 140190000 4
g
g
EOF
chk "R5: bridged originals kept" /tmp/gleam_R5.txt "47 4C 45 41"

# --- S1: pause injection stress (25 fresh sessions, each exactly one stop) ---
echo "== S1 =="
S1OK=0
for i in $(seq 1 25); do
  out=$(printf 'g\npause\ndetach\n' | timeout 20 ./bin/Debug/x64/Gleam.exe 'C:\Windows\notepad.exe' 2>&1)
  n=$(printf '%s' "$out" | grep -c 'stop reason=pause')
  [ "$n" -eq 1 ] && S1OK=$((S1OK+1))
  powershell -NoProfile -Command "Stop-Process -Name notepad -Force -ErrorAction SilentlyContinue" > /dev/null 2>&1
done
if [ "$S1OK" -eq 25 ]; then ok "S1: 25/25 pause injections"; else bad "S1: $S1OK/25 pause injections"; fi

# --- T1: argv quoting matrix ---
echo "== T1 =="
timeout 20 ./bin/Debug/x64/Gleam.exe bin/Debug/x64/ArgvTarget.exe "" "a b" "$(printf 'x\ty')" "quote\"in" 'trail\' > /tmp/gleam_T1.txt 2>&1 <<'EOF'
g
EOF
chk "T1: argc"                   /tmp/gleam_T1.txt "ARGC=6"
chk "T1: empty arg kept"         /tmp/gleam_T1.txt "ARGV[1]=[]"
chk "T1: space arg"              /tmp/gleam_T1.txt "ARGV[2]=[a b]"
chk "T1: tab arg"                /tmp/gleam_T1.txt "$(printf 'ARGV[3]=[x\ty]')"
chk "T1: embedded quote"         /tmp/gleam_T1.txt 'ARGV[4]=[quote"in]'
chk "T1: trailing backslash"     /tmp/gleam_T1.txt 'ARGV[5]=[trail\]'

# --- T2: cross-chunk instruction scan ---
echo "== T2 =="
timeout 30 ./bin/Debug/x64/Gleam.exe bin/Debug/x64/BoundaryTarget.exe > /tmp/gleam_T2.txt 2>&1 <<'EOF'
g
xref 60000000
quit
EOF
chk "T2: straddler found"        /tmp/gleam_T2.txt "0x00000000600FFFFD  call 0x0000000060000000"
chk "T2: both calls found"       /tmp/gleam_T2.txt "2 references to 0x60000000"

# --- T3: patch matrix (containment / adjacent / extend-right / left-overlap) ---
run T3 "" <<'EOF'
bp 140070EC9
g
patch 140190000 AA AA AA AA
patch 140190002 BB
restore 140190000
read 140190000 4
patch 140190000 CC
patch 140190002 DD
restore 140190002
restore 140190000
read 140190000 4
patch 140190004 EE EE
patch 140190004 FF FF FF FF
restore 140190004
read 140190004 6
patch 140190006 11 22
patch 140190004 33 33 33 33
restore 140190004
read 140190004 6
g
g
EOF
chk "T3: containment restored"   /tmp/gleam_T3.txt "47 4C 45 41"
chkcount "T3: gdata restored 2x" /tmp/gleam_T3.txt "47 4C 45 41" 2
chkcount "T3: tail restored 2x"  /tmp/gleam_T3.txt "4D 2D 54 45" 2

# --- T4a: one-shot + ignore (hit ignored, bp still deleted) ---
run T4a "" <<'EOF'
bp 1400708AC once
ignore 1400708AC 1
g
g
EOF
chk "T4a: one-shot ignored once" /tmp/gleam_T4a.txt "event ignored address=0x1400708AC left=0"
chkcount "T4a: no bp pause"      /tmp/gleam_T4a.txt "stop reason=breakpoint" 0
chk "T4a: results correct"       /tmp/gleam_T4a.txt "MARKER_RESULT_2=13"

# --- T4b: one-shot + do g (hit auto-continues, bp deleted, second call no hit) ---
run T4b "" <<'EOF'
bp 1400708AC once do g
g
g
EOF
chkcount "T4b: no bp pause"      /tmp/gleam_T4b.txt "stop reason=breakpoint" 0
chk "T4b: results correct"       /tmp/gleam_T4b.txt "MARKER_RESULT_2=13"

# --- T5: ret at function entry and at the ret instruction ---
run T5 "" <<'EOF'
bp 140070EC9
g
ret
regs
g
g
EOF
chk "T5: ret at entry"           /tmp/gleam_T5.txt "stepping out to 0x140076E94 (stack scan)"
run T5b "" <<'EOF'
bp 140070EC9
g
until 140076BAE
ret
regs
g
g
EOF
chk "T5: ret at ret insn"        /tmp/gleam_T5b.txt "stepping out to 0x140076E94 (stack scan)"

# --- S2: 100 pause injections (reviewer-standard stress) ---
echo "== S2 =="
S2OK=0
for i in $(seq 1 100); do
  out=$(printf 'g\npause\ndetach\n' | timeout 30 ./bin/Debug/x64/Gleam.exe 'C:\Windows\notepad.exe' 2>&1)
  n=$(printf '%s' "$out" | grep -c 'stop reason=pause')
  [ "$n" -eq 1 ] && S2OK=$((S2OK+1))
  powershell -NoProfile -Command "Stop-Process -Name notepad -Force -ErrorAction SilentlyContinue" > /dev/null 2>&1
  sleep 0.3  # let the previous notepad fully exit before the next launch
done
if [ "$S2OK" -eq 100 ]; then ok "S2: 100/100 pause injections"; else bad "S2: $S2OK/100 pause injections"; fi

# --- S3: 100 quick sessions (REPL lifecycle) ---
echo "== S3 =="
S3OK=0
for i in $(seq 1 100); do
  printf 'quit\n' | timeout 10 ./bin/Debug/x64/Gleam.exe bin/Debug/x64/TestTarget.exe > /dev/null 2>&1
  [ $? -eq 0 ] && S3OK=$((S3OK+1))
done
if [ "$S3OK" -eq 100 ]; then ok "S3: 100/100 sessions exited"; else bad "S3: $S3OK/100 sessions exited"; fi

echo
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
