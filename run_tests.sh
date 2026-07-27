#!/bin/bash
# Gleam atomic-feature verification suite.
# Requires fixed addresses from TestTarget (no ASLR).
set -u
cd "$(dirname "$0")"

GLEAM=${GLEAM:-./bin/Debug/x64/Gleam.exe}
TARGET=${TARGET:-bin/Debug/x64/TestTarget.exe}
ATARGET=${ATARGET:-bin/Debug/x64/ArgvTarget.exe}
BTARGET=${BTARGET:-bin/Debug/x64/BoundaryTarget.exe}
TDIR=${TDIR:-/tmp}
mkdir -p "$TDIR"
# Target addresses are resolved at runtime: clean rebuilds shift the layout,
# so fixed RVAs are forbidden (review gate). TestTarget prints the three base
# addresses itself; the marker body and OEP come from a gleam probe session.
PROBE=$(timeout 30 "$TARGET" | grep -E '^(MARKER|INNER|GDATA)=')
MARKER=$(printf '%s\n' "$PROBE" | sed -n 's/^MARKER=0*\([0-9A-Fa-f]*\).*/\1/p' | tr 'a-f' 'A-F')
INNER=$(printf '%s\n' "$PROBE" | sed -n 's/^INNER=0*\([0-9A-Fa-f]*\).*/\1/p' | tr 'a-f' 'A-F')
GDATA=$(printf '%s\n' "$PROBE" | sed -n 's/^GDATA=0*\([0-9A-Fa-f]*\).*/\1/p' | tr 'a-f' 'A-F')
OUT=$(printf 'eval TestTarget!marker\nquit\n' | timeout 30 "$GLEAM" $TARGET 2>&1)
MBODY=$(printf '%s\n' "$OUT" | sed -n 's/^= 0x\([0-9A-F]*\).*/\1/p' | head -1)
OEP=$(printf '%s\n' "$OUT" | sed -n 's/^event process.*start=0x0*\([0-9A-F]*\).*/\1/p' | head -1)
# The ret instruction and the instruction after "call inner" inside marker,
# located by disassembly (never by fixed offsets).
OUT2=$(printf 'disasm TestTarget!marker 40\nquit\n' | timeout 30 "$GLEAM" $TARGET 2>&1)
MRET=$(printf '%s\n' "$OUT2" | sed -n 's/^0000000\([0-9A-F]*\)  ret.*$/\1/p' | head -1)
CALLA=$(printf '%s\n' "$OUT2" | sed -n 's/^0000000\([0-9A-F]*\)  call 0x0000000'$INNER'$/\1/p' | head -1)
MCALLNEXT=$(printf '%X' $((0x$CALLA + 5)))
GD2=$(printf '%X' $((0x$GDATA + 2)))
GD4=$(printf '%X' $((0x$GDATA + 4)))
GD6=$(printf '%X' $((0x$GDATA + 6)))
GD8=$(printf '%X' $((0x$GDATA + 8)))
for v in MARKER INNER GDATA MBODY OEP MRET MCALLNEXT GD2 GD4 GD6 GD8; do
  eval "test -n \"\$$v\"" || { echo "FATAL: cannot resolve $v - suite cannot run"; exit 1; }
done

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
  timeout 60 "$GLEAM" $TARGET $args > ${TDIR}/gleam_$name.txt 2>&1
  local ec=$?
  echo "== $name =="
  # A scenario that hangs or crashes after printing expected text must NOT
  # count as passed: check the exit status first.
  if [ $ec -ne 0 ]; then
    bad "$name: abnormal exit (code $ec; see ${TDIR}/gleam_$name.txt)"
  fi
}

# --- A: inspection commands + regs/read/write/setreg/step ---
run A "" <<EOF
bp $MARKER
g
disasm
maps
modules
bl
threads
thread
bt
find $GDATA 100 47 4C 45 41
regs
read $GDATA 10
write $GDATA DE AD BE EF 01 02 03 04 08 09 0A 0B 0C 0D 0E 0F
setreg rcx 64
step
regs
g
g
EOF
chk "A: bp hit at marker"        ${TDIR}/gleam_A.txt "stop reason=breakpoint type=software address=0x$MARKER"
chk "A: disasm at rip"           ${TDIR}/gleam_A.txt "0000000$MARKER"
chkre "A: maps regions"          ${TDIR}/gleam_A.txt "[0-9]+ committed regions"
chk "A: modules has TestTarget"  ${TDIR}/gleam_A.txt "TestTarget.exe"
chk "A: bl lists bp"             ${TDIR}/gleam_A.txt "0x$MARKER  software"
chk "A: two threads"             ${TDIR}/gleam_A.txt "[event]"
chk "A: bt frame0 rip"           ${TDIR}/gleam_A.txt "#0"
chk "A: find pattern"            ${TDIR}/gleam_A.txt "found at 0x$GDATA"
chk "A: rcx=arg at entry"        ${TDIR}/gleam_A.txt "RCX=0000000000000029"
chk "A: write ok"                ${TDIR}/gleam_A.txt "wrote 16 bytes"
chk "A: result1 modified by rcx" ${TDIR}/gleam_A.txt "MARKER_RESULT_1=106"
chk "A: result2 normal"          ${TDIR}/gleam_A.txt "MARKER_RESULT_2=13"
chk "A: gdata self-write wins b0" ${TDIR}/gleam_A.txt "GDATA_AFTER=58ADBEEF0102030408090A0B0C0D0E0F"
chk "A: exit code 0"             ${TDIR}/gleam_A.txt "stop reason=exit code=0x00000000"

# --- B: one-shot breakpoint ---
run B "" <<EOF
bp $MARKER once
g
bl
g
EOF
chkcount "B: hit exactly once"   ${TDIR}/gleam_B.txt "stop reason=breakpoint" 1
chk "B: bp auto-deleted"         ${TDIR}/gleam_B.txt "no breakpoints"
chk "B: both calls ran"          ${TDIR}/gleam_B.txt "MARKER_RESULT_2=13"

# --- C: ignore count ---
run C "" <<EOF
bp $MARKER
ignore $MARKER 1
g
g
EOF
chk "C: first hit ignored"       ${TDIR}/gleam_C.txt "event ignored address=0x$MARKER left=0"
chkcount "C: bp stop once"       ${TDIR}/gleam_C.txt "stop reason=breakpoint" 1
chk "C: exit 0"                  ${TDIR}/gleam_C.txt "stop reason=exit code=0x00000000"

# --- D: hardware write breakpoint ---
run D "" <<EOF
bp $MARKER
g
rbp $MARKER
hbp $GDATA w
g
g
EOF
chk "D: hbp set"                 ${TDIR}/gleam_D.txt "hardware breakpoint set at 0x$GDATA (w)"
chkcount "D: hbp hit once"       ${TDIR}/gleam_D.txt "stop reason=breakpoint type=hardware" 1
chk "D: self write done"         ${TDIR}/gleam_D.txt "GDATA_AFTER=584C45414D2D544553542D4441544121"

# --- E: memory write breakpoint ---
run E "" <<EOF
mbp $GDATA 10 w
g
g
EOF
chk "E: mbp set"                 ${TDIR}/gleam_E.txt "memory breakpoint set at 0x$GDATA"
chkcount "E: mbp hit once"       ${TDIR}/gleam_E.txt "stop reason=breakpoint type=memory" 1

# --- F: unhandled exception + exinfo ---
run F "exc" <<EOF
g
exinfo
quit
EOF
chk "F: unhandled exception"     ${TDIR}/gleam_F.txt "stop reason=exception code=0xE0DEAD00"
chk "F: exinfo code"             ${TDIR}/gleam_F.txt "code=0xE0DEAD00"

# --- G: exception filter ---
run G "exc" <<EOF
ignoreexc E0DEAD00
g
EOF
chk "G: exception passed"        ${TDIR}/gleam_G.txt "action=passed-to-debuggee"
chk "G: survived"                ${TDIR}/gleam_G.txt "SURVIVED_EXCEPTION"
chk "G: exit 0"                  ${TDIR}/gleam_G.txt "stop reason=exit code=0x00000000"

# --- H: stepout (stepping loop) ---
run H "" <<EOF
bp $INNER
g
ret
regs
g
g
EOF
chk "H: bp inner hit"            ${TDIR}/gleam_H.txt "stop reason=breakpoint type=software address=0x$INNER"
chk "H: stepout stop"            ${TDIR}/gleam_H.txt "stop reason=stepout return"
chk "H: result1 normal"          ${TDIR}/gleam_H.txt "MARKER_RESULT_1=47"

# --- I: stepover ---
run I "" <<EOF
bp $MARKER
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
chkre "I: stepped over the call" ${TDIR}/gleam_I.txt "stop reason=step rip=0x[0-9A-F]+"
chk "I: result1 normal"          ${TDIR}/gleam_I.txt "MARKER_RESULT_1=47"

# --- J: detach ---
run J "" <<EOF
bp $MARKER
g
detach
EOF
chk "J: detaching"               ${TDIR}/gleam_J.txt "detaching..."
chk "J: target ran free"         ${TDIR}/gleam_J.txt "MARKER_RESULT_1=47"
chk "J: session finished"        ${TDIR}/gleam_J.txt "session finished"

# --- K: symbols (imports/exports) ---
run K "" <<EOF
imports
exports kernel32 CreateFile*
g
EOF
chk "K: imports kernel32 group"  ${TDIR}/gleam_K.txt "KERNEL32.dll:"
chk "K: import names resolved"   ${TDIR}/gleam_K.txt "CreateThread"
chk "K: exports wildcard filter" ${TDIR}/gleam_K.txt "CreateFileW"
chk "K: exports summary"         ${TDIR}/gleam_K.txt "symbols"

# --- L: symbol breakpoint + breakon switches ---
run L "" <<EOF
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
chk "L: default exception on"    ${TDIR}/gleam_L.txt "breakon exception=on"
chk "L: OEP entry stop"          ${TDIR}/gleam_L.txt "stop reason=entry address=0x$OEP"
chk "L: thread create stop"      ${TDIR}/gleam_L.txt "stop reason=thread op=create"
chk "L: thread start named"      ${TDIR}/gleam_L.txt "name=worker"
chk "L: dll load stop"           ${TDIR}/gleam_L.txt "stop reason=dll op=load"
chk "L: symbol bp hit (real body)" ${TDIR}/gleam_L.txt "stop reason=breakpoint type=software address=0x$MBODY"

# --- M: breakon exception off ---
run M "exc" <<EOF
breakon exception off
g
EOF
chk "M: no exception stop"       ${TDIR}/gleam_M.txt "SURVIVED_EXCEPTION"
chk "M: exit 0"                  ${TDIR}/gleam_M.txt "stop reason=exit code=0x00000000"
chkcount "M: only 2 stops"       ${TDIR}/gleam_M.txt "stop reason=" 2

# --- N1: hide (anti-anti-debug) ---
run N1 "" <<EOF
hide
g
EOF
chk "N1: hidden from IsDebuggerPresent" ${TDIR}/gleam_N1.txt "ISDEBUGGERPRESENT=0"

# --- N2: tracepoint ---
run N2 "" <<EOF
trace $MARKER
g
EOF
chkcount "N2: two trace lines"   ${TDIR}/gleam_N2.txt "trace address=0x$MARKER" 2
chkcount "N2: no bp pause"       ${TDIR}/gleam_N2.txt "stop reason=breakpoint" 0

# --- N3: conditional breakpoint ---
run N3 "" <<EOF
bp $MARKER if rcx==7
g
g
EOF
chkcount "N3: only rcx==7 pauses" ${TDIR}/gleam_N3.txt "stop reason=breakpoint" 1

# --- N4: sym / stackscan / find-ascii / patch / until ---
run N4 "" <<EOF
bp $MARKER
g
sym $MBODY
stackscan 10
find $GDATA 100 ascii GLEAM
patch $GDATA AA BB
patches
restore $GDATA
patches
until $MBODY
g
g
EOF
chk "N4: sym resolves marker"    ${TDIR}/gleam_N4.txt "marker"
chk "N4: stackscan finds main"   ${TDIR}/gleam_N4.txt "main"
chk "N4: find ascii"             ${TDIR}/gleam_N4.txt "found at 0x$GDATA"
chk "N4: patched"                ${TDIR}/gleam_N4.txt "patched 0x$GDATA (2 bytes)"
chk "N4: restored"               ${TDIR}/gleam_N4.txt "restored 0x$GDATA"
chk "N4: list empty after restore" ${TDIR}/gleam_N4.txt "no patches"
chk "N4: until hits"             ${TDIR}/gleam_N4.txt "stop reason=breakpoint type=software address=0x$MBODY"
chk "N4: restore kept data"      ${TDIR}/gleam_N4.txt "GDATA_AFTER=584C45414D2D544553542D4441544121"

# --- O1: alloc/protect ---
run O1 "" <<EOF
alloc 1000
protect $GDATA 100 rw
g
EOF
chk "O1: allocated"              ${TDIR}/gleam_O1.txt "allocated 0x"
chk "O1: protected"              ${TDIR}/gleam_O1.txt "protected 0x$GDATA"

# --- O2: xref / findasm ---
run O2 "" <<EOF
xref $INNER
findasm call
g
EOF
chkre "O2: xref finds call site"  ${TDIR}/gleam_O2.txt "call 0x0000000$INNER"
chk "O2: xref count"             ${TDIR}/gleam_O2.txt "1 references to 0x$INNER"
chkre "O2: findasm indirect call" ${TDIR}/gleam_O2.txt "call \[0x[0-9A-F]{16}\]"

# --- O3: thread suspend/resume (event thread, no tid needed) ---
run O3 "" <<EOF
bp $MARKER
g
thread suspend
thread resume
g
g
EOF
chkre "O3: suspend ok"           ${TDIR}/gleam_O3.txt "suspend thread [0-9]+: ok"
chkre "O3: resume ok"            ${TDIR}/gleam_O3.txt "resume thread [0-9]+: ok"

# --- P1: conditional tracing ---
run P1 "" <<EOF
bp $MARKER
g
tgo rcx==29 100
regs
g
g
EOF
chk "P1: trace stops on condition" ${TDIR}/gleam_P1.txt "stop reason=trace condition steps=1"
chkre "P1: rip in marker code"    ${TDIR}/gleam_P1.txt "RIP=000000014[0-9A-F]+"

# --- P2: bp do <command> (non-resume) ---
run P2 "" <<EOF
bp $MARKER do regs
g
g
g
EOF
chkcount "P2: regs dumped twice" ${TDIR}/gleam_P2.txt "RAX=" 2

# --- P3: bp do g (resume-type, no pause) ---
run P3 "" <<EOF
bp $INNER do g
g
EOF
chkcount "P3: no bp pause"       ${TDIR}/gleam_P3.txt "stop reason=breakpoint" 0
chk "P3: results correct"        ${TDIR}/gleam_P3.txt "MARKER_RESULT_2=13"

# --- Q1: one-shot bp rule not inherited (re-arm at same addr refires once, expected) ---
run Q1 "" <<EOF
bp $INNER once do regs
g
bp $INNER
g
g
g
EOF
chkcount "Q1: 1 one-shot + 1 refire + 1 call-2" ${TDIR}/gleam_Q1.txt "stop reason=breakpoint" 3
chk "Q1: results correct"        ${TDIR}/gleam_Q1.txt "MARKER_RESULT_2=13"

# --- Q2: breakon entry off disarms OEP breakpoint ---
run Q2 "" <<EOF
breakon entry on
breakon entry off
g
EOF
chkcount "Q2: no entry stop"     ${TDIR}/gleam_Q2.txt "stop reason=entry" 0

# --- Q3: double patch restores the true original ---
run Q3 "" <<EOF
bp $MARKER
g
patch $GDATA AA BB
patch $GDATA CC DD
restore $GDATA
read $GDATA 4
g
g
EOF
chk "Q3: restored to original"   ${TDIR}/gleam_Q3.txt "47 4C 45 41"

# --- Q4: stepout mid-function (framed) ---
run Q4 "" <<EOF
bp $MARKER
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
chk "Q4: stepout stop"           ${TDIR}/gleam_Q4.txt "stop reason=stepout return"

# --- R1: one-shot 'do g' rule fully cleaned ---
run R1 "" <<EOF
bp $MARKER
bp $INNER once do g
g
g
bp $INNER
g
g
EOF
chkcount "R1: 2 marker + 1 inner stops" ${TDIR}/gleam_R1.txt "stop reason=breakpoint" 3
chk "R1: results correct"        ${TDIR}/gleam_R1.txt "MARKER_RESULT_2=13"

# --- R2: overlapping patches restore fully ---
run R2 "" <<EOF
bp $MARKER
g
patch $GDATA AA
patch $GDATA BB CC
restore $GDATA
read $GDATA 4
g
g
EOF
chk "R2: both bytes restored"    ${TDIR}/gleam_R2.txt "47 4C 45 41"

# --- R3: double hide then off restores detection ---
run R3 "" <<EOF
hide
hide
hide off
g
EOF
chk "R3: detection restored"     ${TDIR}/gleam_R3.txt "ISDEBUGGERPRESENT=1"
chk "R3: skip re-apply"          ${TDIR}/gleam_R3.txt "hide already applied"

# Direct scenarios must check the exit status too (no false pass after a hang).
check_ec() { # check_ec <desc> <code> <outfile>
  if [ "$2" -ne 0 ]; then bad "$1: abnormal exit (code $2; see $3)"; fi
}

# --- R4: stepout in Release/FPO function (guarded: needs Release binaries) ---
REL=bin/Release/x64
if [ -f "$REL/Gleam.exe" ] && [ -f "$REL/TestTarget.exe" ]; then
  echo "== R4 =="
  timeout 30 "$REL/Gleam.exe" "$REL/TestTarget.exe" > ${TDIR}/gleam_R4.txt 2>&1 <<EOF
bp TestTarget!marker
g
step
stepover
stepover
ret
g
g
EOF
  check_ec R4 $? ${TDIR}/gleam_R4.txt
  chk "R4: stepout stop (FPO)"   ${TDIR}/gleam_R4.txt "stop reason=stepout return"
  chk "R4: caller resume"        ${TDIR}/gleam_R4.txt "MARKER_RESULT_1=47"
else
  echo "== R4 == (skipped: no Release binaries)"
fi

# --- R5: new patch bridging two old records ---
run R5 "" <<EOF
bp $MARKER
g
patch $GDATA AA
patch $GD2 CC
patch $GDATA EE EE EE
restore $GDATA
read $GDATA 4
g
g
EOF
chk "R5: bridged originals kept" ${TDIR}/gleam_R5.txt "47 4C 45 41"

# --- S1: pause injection stress (25 fresh sessions, each exactly one stop) ---
echo "== S1 =="
S1OK=0
for i in $(seq 1 25); do
  out=$(printf 'g\npause\ndetach\n' | timeout 20 "$GLEAM" 'C:\Windows\notepad.exe' 2>&1)
  ec=$?
  n=$(printf '%s' "$out" | grep -c 'stop reason=pause')
  if [ "$n" -eq 1 ] && [ $ec -eq 0 ]; then S1OK=$((S1OK+1)); else printf '%s' "$out" > ${TDIR}/gleam_S1_fail_$i.txt; fi
  # Kill by exact PID: killing by name races with the next iteration.
  S1PID=$(printf '%s' "$out" | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | head -1)
  if [ -n "$S1PID" ]; then powershell -NoProfile -Command "Stop-Process -Id $S1PID -Force -ErrorAction SilentlyContinue" > /dev/null 2>&1; fi
  echo "iter=$i ec=$ec pid=$S1PID hits=$n" >> "${TDIR}/pressure.log"
done
if [ "$S1OK" -eq 25 ]; then ok "S1: 25/25 pause injections"; else bad "S1: $S1OK/25 pause injections (artifacts: ${TDIR}/gleam_S1_fail_*.txt)"; fi

# --- T1: argv quoting matrix ---
echo "== T1 =="
timeout 20 "$GLEAM" "$ATARGET" "" "a b" "$(printf 'x\ty')" "quote\"in" 'trail\' > ${TDIR}/gleam_T1.txt 2>&1 <<EOF
g
EOF
check_ec T1 $? ${TDIR}/gleam_T1.txt
chk "T1: argc"                   ${TDIR}/gleam_T1.txt "ARGC=6"
chk "T1: empty arg kept"         ${TDIR}/gleam_T1.txt "ARGV[1]=[]"
chk "T1: space arg"              ${TDIR}/gleam_T1.txt "ARGV[2]=[a b]"
chk "T1: tab arg"                ${TDIR}/gleam_T1.txt "$(printf 'ARGV[3]=[x\ty]')"
chk "T1: embedded quote"         ${TDIR}/gleam_T1.txt 'ARGV[4]=[quote"in]'
chk "T1: trailing backslash"     ${TDIR}/gleam_T1.txt 'ARGV[5]=[trail\]'

# --- T1c: argv backslash parity ---
echo "== T1c =="
timeout 20 "$GLEAM" "$ATARGET" 'a\\' 'x\\"y' > ${TDIR}/gleam_T1c.txt 2>&1 <<EOF
g
EOF
check_ec T1c $? ${TDIR}/gleam_T1c.txt
chk "T1c: trailing double backslash" ${TDIR}/gleam_T1c.txt 'ARGV[1]=[a\\]'
chk "T1c: 2x backslash + quote"  ${TDIR}/gleam_T1c.txt 'ARGV[2]=[x\\"y]'

# --- T1b: argv unicode + backslash-before-quote ---
echo "== T1b =="
timeout 20 "$GLEAM" "$ATARGET" "中文路径" 'a\"b' > ${TDIR}/gleam_T1b.txt 2>&1 <<EOF
g
EOF
check_ec T1b $? ${TDIR}/gleam_T1b.txt
# The target's CRT converts narrow argv using the system ANSI codepage (GBK),
# so the log contains GBK bytes - compare against those, not UTF-8.
T1B_UNICODE=$(printf '中文路径' | iconv -f UTF-8 -t GBK)
chk "T1b: unicode arg (gbk bytes)" ${TDIR}/gleam_T1b.txt "$T1B_UNICODE"
chk "T1b: backslash before quote" ${TDIR}/gleam_T1b.txt 'ARGV[2]=[a\"b]'

# --- T2: cross-chunk instruction scan ---
echo "== T2 =="
timeout 30 "$GLEAM" "$BTARGET" > ${TDIR}/gleam_T2.txt 2>&1 <<EOF
g
xref 60000000
xref 60100040
xref 60200042
quit
EOF
check_ec T2 $? ${TDIR}/gleam_T2.txt
chk "T2: straddler found"        ${TDIR}/gleam_T2.txt "0x00000000600FFFFD  call 0x0000000060000000"
chk "T2: direct+indirect calls"  ${TDIR}/gleam_T2.txt "3 references to 0x60000000"
chk "T2: rel8 jmp form"          ${TDIR}/gleam_T2.txt "0x0000000060100010  jmp 0x0000000060100040"
chk "T2: second-boundary straddler" ${TDIR}/gleam_T2.txt "0x00000000601FFFFF  jmp 0x0000000060200042"

# --- T3: patch matrix (containment / adjacent / extend-right / left-overlap) ---
run T3 "" <<EOF
bp $MARKER
g
patch $GDATA AA AA AA AA
patch $GD2 BB
restore $GDATA
read $GDATA 4
patch $GDATA CC
patch $GD2 DD
restore $GD2
restore $GDATA
read $GDATA 4
patch $GD4 EE EE
patch $GD4 FF FF FF FF
restore $GD4
read $GD4 6
patch $GD6 11 22
patch $GD4 33 33 33 33
restore $GD4
read $GD4 6
g
g
EOF
chk "T3: containment restored"   ${TDIR}/gleam_T3.txt "47 4C 45 41"
chkcount "T3: gdata restored 2x" ${TDIR}/gleam_T3.txt "47 4C 45 41" 2
chkcount "T3: tail restored 2x"  ${TDIR}/gleam_T3.txt "4D 2D 54 45" 2

# --- T4a: one-shot + ignore (hit ignored, bp still deleted) ---
run T4a "" <<EOF
bp $INNER once
ignore $INNER 1
g
g
EOF
chk "T4a: one-shot ignored once" ${TDIR}/gleam_T4a.txt "event ignored address=0x$INNER left=0"
chkcount "T4a: no bp pause"      ${TDIR}/gleam_T4a.txt "stop reason=breakpoint" 0
chk "T4a: results correct"       ${TDIR}/gleam_T4a.txt "MARKER_RESULT_2=13"

# --- T4b: one-shot + do g (hit auto-continues, bp deleted, second call no hit) ---
run T4b "" <<EOF
bp $INNER once do g
g
g
EOF
chkcount "T4b: no bp pause"      ${TDIR}/gleam_T4b.txt "stop reason=breakpoint" 0
chk "T4b: results correct"       ${TDIR}/gleam_T4b.txt "MARKER_RESULT_2=13"

# --- T5: stepout at function entry and at the ret instruction ---
run T5 "" <<EOF
bp $MARKER
g
ret
regs
g
g
EOF
chk "T5: stepout at entry"       ${TDIR}/gleam_T5.txt "stop reason=stepout return"
run T5b "" <<EOF
bp $MARKER
g
until $MRET
ret
regs
g
g
EOF
chk "T5b: stepout at ret insn"   ${TDIR}/gleam_T5b.txt "stop reason=stepout return"

# --- T5c: long loop hits maxsteps (fast-forward removed per S0-2) ---
run T5c "" <<EOF
bp TestTarget!looper
g
ret 1000
g
EOF
chk "T5c: maxreached at limit"   ${TDIR}/gleam_T5c.txt "stop reason=stepout maxreached steps=4096"
chk "T5c: loop result correct"   ${TDIR}/gleam_T5c.txt "LOOP_RESULT=4999950000"

# --- S2: 100 pause injections (reviewer-standard stress) ---
echo "== S2 =="
S2OK=0
for i in $(seq 1 100); do
  out=$(printf 'g\npause\ndetach\n' | timeout 30 "$GLEAM" 'C:\Windows\notepad.exe' 2>&1)
  ec=$?
  n=$(printf '%s' "$out" | grep -c 'stop reason=pause')
  if [ "$n" -eq 1 ] && [ $ec -eq 0 ]; then S2OK=$((S2OK+1)); else printf '%s' "$out" > ${TDIR}/gleam_S2_fail_$i.txt; fi
  # Kill by exact PID: killing by name races with the next iteration.
  S2PID=$(printf '%s' "$out" | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | head -1)
  if [ -n "$S2PID" ]; then powershell -NoProfile -Command "Stop-Process -Id $S2PID -Force -ErrorAction SilentlyContinue" > /dev/null 2>&1; fi
  echo "iter=$i ec=$ec pid=$S2PID hits=$n" >> "${TDIR}/pressure.log"
  sleep 0.3  # let the previous notepad fully exit before the next launch
done
if [ "$S2OK" -eq 100 ]; then ok "S2: 100/100 pause injections"; else bad "S2: $S2OK/100 pause injections (artifacts: ${TDIR}/gleam_S2_fail_*.txt)"; fi

# --- S3: 100 quick sessions (REPL lifecycle) ---
echo "== S3 =="
S3OK=0
for i in $(seq 1 100); do
  printf 'quit\n' | timeout 10 "$GLEAM" "$TARGET" > /dev/null 2>&1
  [ $? -eq 0 ] && S3OK=$((S3OK+1))
done
if [ "$S3OK" -eq 100 ]; then ok "S3: 100/100 sessions exited"; else bad "S3: $S3OK/100 sessions exited"; fi

# --- S3b: REPL stdin-open lifecycle (natural exit / detach / quit, with artifacts) ---
echo "== S3b =="
out=$({ printf 'g\n'; sleep 2; } | timeout 15 "$GLEAM" "$TARGET" 2>&1); ec=$?
[ $ec -eq 0 ] && ok "S3b: natural exit with stdin open" || { printf '%s' "$out" > ${TDIR}/gleam_S3b_exit.txt; bad "S3b: natural exit (code $ec)"; }
out=$({ printf 'bp $MARKER\ng\ndetach\n'; sleep 2; } | timeout 15 "$GLEAM" "$TARGET" 2>&1); ec=$?
[ $ec -eq 0 ] && ok "S3b: detach with stdin open" || { printf '%s' "$out" > ${TDIR}/gleam_S3b_detach.txt; bad "S3b: detach (code $ec)"; }
out=$({ printf 'quit\n'; sleep 2; } | timeout 15 "$GLEAM" "$TARGET" 2>&1); ec=$?
[ $ec -eq 0 ] && ok "S3b: quit with stdin open" || { printf '%s' "$out" > ${TDIR}/gleam_S3b_quit.txt; bad "S3b: quit (code $ec)"; }

# --- S4: runtime pause (delayed writer; pause sent while target RUNS free) ---
echo "== S4 =="
S4OK=0
for i in $(seq 1 100); do
  out=$(( printf 'g\n'; sleep 1; printf 'pause\n'; sleep 1; printf 'detach\n' ) | timeout 20 "$GLEAM" 'C:\Windows\notepad.exe' 2>&1)
  ec=$?
  n=$(printf '%s' "$out" | grep -c 'stop reason=pause')
  if [ "$n" -eq 1 ] && [ $ec -eq 0 ]; then S4OK=$((S4OK+1)); else printf '%s' "$out" > ${TDIR}/gleam_S4_fail_$i.txt; fi
  # Kill by exact PID: "Stop-Process -Name notepad" races with the next
  # iteration (slow powershell startup can kill the NEW notepad).
  S4PID=$(printf '%s' "$out" | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | head -1)
  if [ -n "$S4PID" ]; then powershell -NoProfile -Command "Stop-Process -Id $S4PID -Force -ErrorAction SilentlyContinue" > /dev/null 2>&1; fi
  echo "iter=$i ec=$ec pid=$S4PID hits=$n" >> "${TDIR}/pressure.log"
  sleep 0.3
done
if [ "$S4OK" -eq 100 ]; then ok "S4: 100/100 runtime pauses"; else bad "S4: $S4OK/100 runtime pauses (artifacts: ${TDIR}/gleam_S4_fail_*.txt)"; fi

# --- S5: runtime pause with hide applied ---
echo "== S5 =="
S5OK=0
for i in $(seq 1 100); do
  out=$(( printf 'hide\ng\n'; sleep 1; printf 'pause\n'; sleep 1; printf 'detach\n' ) | timeout 20 "$GLEAM" 'C:\Windows\notepad.exe' 2>&1)
  ec=$?
  n=$(printf '%s' "$out" | grep -c 'stop reason=pause')
  f=$(printf '%s' "$out" | grep -c 'breakin fail=')
  if [ "$n" -eq 1 ] && [ "$f" -eq 0 ] && [ $ec -eq 0 ]; then S5OK=$((S5OK+1)); else printf '%s' "$out" > ${TDIR}/gleam_S5_fail_$i.txt; fi
  # Kill by exact PID: killing by name races with the next iteration.
  S5PID=$(printf '%s' "$out" | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | head -1)
  if [ -n "$S5PID" ]; then powershell -NoProfile -Command "Stop-Process -Id $S5PID -Force -ErrorAction SilentlyContinue" > /dev/null 2>&1; fi
  echo "iter=$i ec=$ec pid=$S5PID hits=$n" >> "${TDIR}/pressure.log"
  sleep 0.3
done
if [ "$S5OK" -eq 100 ]; then ok "S5: 100/100 hidden runtime pauses"; else bad "S5: $S5OK/100 hidden runtime pauses (artifacts: ${TDIR}/gleam_S5_fail_*.txt)"; fi

# --- T7: one-shot + failing condition ---
run T7 "" <<EOF
bp $INNER once if rcx==0
g
g
EOF
chkcount "T7: cond never met, no pause" ${TDIR}/gleam_T7.txt "stop reason=breakpoint" 0
chk "T7: results correct"        ${TDIR}/gleam_T7.txt "MARKER_RESULT_2=13"

# --- T8: one-shot tracepoint ---
run T8 "" <<EOF
trace $INNER once
g
g
EOF
chkcount "T8: one trace line"    ${TDIR}/gleam_T8.txt "trace address=0x$INNER" 1
chk "T8: results correct"        ${TDIR}/gleam_T8.txt "MARKER_RESULT_2=13"

# --- R6: pause->detach must NOT re-inject after quitting ---
run R6 "" <<EOF
bp $MARKER
g
pause
detach
EOF
chk "R6: detaching"              ${TDIR}/gleam_R6.txt "detaching..."
chkcount "R6: no injection after detach" ${TDIR}/gleam_R6.txt "event breakin injected" 0
chk "R6: target ran to completion" ${TDIR}/gleam_R6.txt "MARKER_RESULT_2=13"

# --- R7: pause->quit must NOT re-inject ---
run R7 "" <<EOF
bp $MARKER
g
pause
quit
EOF
chkcount "R7: no injection after quit" ${TDIR}/gleam_R7.txt "event breakin injected" 0

# --- U1: address expressions (eval) ---
run U1 "" <<EOF
eval $GDATA+8
eval ($GDATA+10)-10
eval dead+beef
eval [$GDATA]
eval TestTarget!marker
eval rsp
eval kernel32
eval 1+
eval foo
read $GDATA+4 4
g
EOF
chk "U1: hex add"                ${TDIR}/gleam_U1.txt "= 0x$GD8"
chk "U1: parens and sub"         ${TDIR}/gleam_U1.txt "= 0x$GDATA"
chk "U1: bare hex names"         ${TDIR}/gleam_U1.txt "= 0x19D9C"
chk "U1: deref g_data"           ${TDIR}/gleam_U1.txt "= 0x45542D4D41454C47"
chkre "U1: module!symbol"        ${TDIR}/gleam_U1.txt "= 0x140[0-9A-F]+"
chkre "U1: register"             ${TDIR}/gleam_U1.txt "= 0x[0-9A-F]+"
chkre "U1: module base"          ${TDIR}/gleam_U1.txt "= 0x7FF[0-9A-F]+"
chk "U1: syntax error reported"  ${TDIR}/gleam_U1.txt "error: expected a value"
chk "U1: unknown name reported"  ${TDIR}/gleam_U1.txt "error: unknown name 'foo'"
chk "U1: expression in read"     ${TDIR}/gleam_U1.txt "4D 2D 54 45"

# --- U2: module-relative delayed breakpoints ---
run U2 "dll" <<EOF
bp version!GetFileVersionInfoSizeW
bl
g
g
EOF
chk "U2: pending at set"         ${TDIR}/gleam_U2.txt "breakpoint pending module=version symbol=GetFileVersionInfoSizeW"
chk "U2: bl shows pending"       ${TDIR}/gleam_U2.txt "logical module=version symbol=GetFileVersionInfoSizeW pending"
chk "U2: bound on dll load"      ${TDIR}/gleam_U2.txt "event bp bound module=version address=0x"
chk "U2: breakpoint hit"         ${TDIR}/gleam_U2.txt "stop reason=breakpoint"
chk "U2: dll call ran"           ${TDIR}/gleam_U2.txt "DLLCALL_RESULT="

# --- U3: real stack frame enumeration (frames) ---
run U3 "" <<EOF
bp TestTarget!marker
g
frames
frames 999999
g
g
EOF
chk "U3: marker frame"           ${TDIR}/gleam_U3.txt "sym=marker+0x"
chk "U3: main frame"             ${TDIR}/gleam_U3.txt "sym=main+0x"
chkre "U3: frame format"         ${TDIR}/gleam_U3.txt "frame #[0-9]+ rip=0x[0-9A-F]+ rsp=0x[0-9A-F]+ module=TestTarget"
chk "U3: unwind provenance"      ${TDIR}/gleam_U3.txt "source=unwind"
chk "U3: bad tid reported"       ${TDIR}/gleam_U3.txt "thread 999999 not found"

# --- U4: exception filters and disposition ---
run U4 "exc" <<EOF
excfilter
excfilter add E0DEAD00 never pass
excfilter
g
EOF
chk "U4: initially empty"        ${TDIR}/gleam_U4.txt "no exception filters"
chk "U4: filter listed"          ${TDIR}/gleam_U4.txt "code=0xE0DEAD00 break=never handledby=pass"
chk "U4: passed to debuggee"     ${TDIR}/gleam_U4.txt "action=passed-to-debuggee"
chk "U4: survived"               ${TDIR}/gleam_U4.txt "SURVIVED_EXCEPTION"

run U4b "exc" <<EOF
g
exception pass
EOF
chk "U4b: exception stop"        ${TDIR}/gleam_U4b.txt "stop reason=exception code=0xE0DEAD00"
chk "U4b: pass line"             ${TDIR}/gleam_U4b.txt "passing exception 0xE0DEAD00"
chk "U4b: survived"              ${TDIR}/gleam_U4b.txt "SURVIVED_EXCEPTION"

run U4c "exc" <<EOF
excfilter add E0DEAD00 never pass
excfilter del E0DEAD00
excfilter
g
quit
EOF
chk "U4c: filter removed"        ${TDIR}/gleam_U4c.txt "exception filter removed code=0xE0DEAD00"
chk "U4c: empty again"           ${TDIR}/gleam_U4c.txt "no exception filters"
chk "U4c: pauses again"          ${TDIR}/gleam_U4c.txt "stop reason=exception code=0xE0DEAD00"

run U4d "exc" <<EOF
breakon exception off
excfilter add E0DEAD00 first pass
g
exception pass
EOF
chk "U4d: filter overrides breakon" ${TDIR}/gleam_U4d.txt "stop reason=exception code=0xE0DEAD00"
chk "U4d: survived"              ${TDIR}/gleam_U4d.txt "SURVIVED_EXCEPTION"

# --- U5: typed memory read + savemem ---
run U5 "" <<EOF
read ansi $GDATA
read u8 $GDATA
read u16 $GDATA
read u32 $GDATA
read u64 $GDATA
read u64 1
savemem $GDATA 10 gleam_savemem_test.bin
g
EOF
chk "U5: ansi string"            ${TDIR}/gleam_U5.txt "string at 0x$GDATA = \"GLEAM-TEST-DATA!\""
chk "U5: u8"                     ${TDIR}/gleam_U5.txt "= 0x47"
chk "U5: u16"                    ${TDIR}/gleam_U5.txt "= 0x4C47"
chk "U5: u32"                    ${TDIR}/gleam_U5.txt "= 0x41454C47"
chk "U5: u64"                    ${TDIR}/gleam_U5.txt "= 0x45542D4D41454C47"
chk "U5: bad address reported"   ${TDIR}/gleam_U5.txt "read failed at 0x1"
chk "U5: savemem line"           ${TDIR}/gleam_U5.txt "saved 0x10 bytes to gleam_savemem_test.bin holes=0"
SAVED=$(od -An -v -tx1 gleam_savemem_test.bin 2>/dev/null | tr -d ' \n')
rm -f gleam_savemem_test.bin
if [ "$SAVED" = "474c45414d2d544553542d4441544121" ]; then ok "U5: savemem bytes"; else bad "U5: savemem bytes"; fi

# --- U6: full thread context (eflags/dr/xmm/mxcsr) ---
run U6 "" <<EOF
setreg xmm0 00112233445566778899AABBCCDDEEFF
setreg eflags 2D5
setreg dr7 0
regs
g
EOF
chk "U6: xmm0 roundtrip"         ${TDIR}/gleam_U6.txt "XMM0 =00112233445566778899AABBCCDDEEFF"
chk "U6: eflags roundtrip"       ${TDIR}/gleam_U6.txt "EFLAGS=000002D5"
chk "U6: dr line present"        ${TDIR}/gleam_U6.txt "DR7=0000000000000000"
chk "U6: mxcsr present"          ${TDIR}/gleam_U6.txt "MXCSR="

# --- U7: session restart ---
run U7 "" <<EOF
bp TestTarget!marker
patch $GDATA 90 90
restart
bl
g
quit
EOF
chk "U7: restart requested"      ${TDIR}/gleam_U7.txt "restart requested"
chk "U7: patches cleared"        ${TDIR}/gleam_U7.txt "patches cleared on restart"
chkcount "U7: two sessions"      ${TDIR}/gleam_U7.txt "stop reason=system" 2
chk "U7: logical bp rebound"     ${TDIR}/gleam_U7.txt "logical module=testtarget symbol=marker bound=0x$MBODY"
chk "U7: bp hits after restart"  ${TDIR}/gleam_U7.txt "stop reason=breakpoint"

# --- V1: expression error propagation (P0-1) ---
run V1 "" <<EOF
read u8 [1]
eval FFFFFFFFFFFFFFFFF
eval FFFFFFFFFFFFFFFF+1
eval 0-1
disasm zzz
g
EOF
chk "V1: deref failure reported" ${TDIR}/gleam_V1.txt "error: cannot read memory at 0x1"
chk "V1: literal out of range"   ${TDIR}/gleam_V1.txt "error: literal out of range: 'FFFFFFFFFFFFFFFFF'"
chk "V1: wrap arithmetic"        ${TDIR}/gleam_V1.txt "= 0x0"
chk "V1: wrap negative"          ${TDIR}/gleam_V1.txt "= 0xFFFFFFFFFFFFFFFF"
chk "V1: unknown name in disasm" ${TDIR}/gleam_V1.txt "error: unknown name 'zzz'"

# --- V2: sub-registers + single-register read (P0-6) ---
run V2 "" <<EOF
setreg rax 1122334455667788
setreg eax AABBCCDD
regs rax
regs eax
setreg ax BEEF
regs rax
regs al
regs ah
regs xmm1
g
EOF
chk "V2: eax rmw keeps high"     ${TDIR}/gleam_V2.txt "rax = 0x11223344AABBCCDD"
chk "V2: single read eax"        ${TDIR}/gleam_V2.txt "eax = 0xAABBCCDD"
chk "V2: ax rmw keeps high"      ${TDIR}/gleam_V2.txt "rax = 0x11223344AABBBEEF"
chk "V2: al read"                ${TDIR}/gleam_V2.txt "al = 0xEF"
chk "V2: ah read"                ${TDIR}/gleam_V2.txt "ah = 0xBE"
chkre "V2: xmm1 read"            ${TDIR}/gleam_V2.txt "xmm1 = [0-9A-F]{32}"

# --- V3: raw DR bidirectional conflict + DR6 hit report (P0-6) ---
# NOTE: DR writes only stick when made at a user-code stop - the kernel wipes
# debug registers on the initial system-breakpoint continue path.
run V3 "" <<EOF
bp $MARKER
g
hbp $GDATA w 1
setreg dr0 $GDATA
hbpd $GDATA
setreg dr7 10001
setreg dr0 $GDATA
hbp $GDATA w 1
g
g
g
EOF
chk "V3: dr write rejected"      ${TDIR}/gleam_V3.txt "dr write rejected: engine hardware breakpoint active"
chk "V3: raw dr written"         ${TDIR}/gleam_V3.txt "dr0 = 0x$GDATA"
chk "V3: hbp rejected after raw" ${TDIR}/gleam_V3.txt "engine hardware breakpoints unavailable after raw dr write"
chk "V3: raw hw hit"             ${TDIR}/gleam_V3.txt "stop reason=hardware"
chk "V3: slot decoded"           ${TDIR}/gleam_V3.txt "raw-hardware slot=0"

# --- V4: restart handle-count idempotence (P0-7) ---
# V4a: baseline handle count. The pause is sent DELAYED (S4-style), while
# the target runs free after the marker breakpoint - a deferred pause sent
# at the system stop can be lost while break-in symbols are unresolved.
( printf "bp $MARKER\ng\n"; sleep 2; printf 'pause\n'; sleep 7; printf 'detach\n' ) | timeout 60 "$GLEAM" "$TARGET" > "${TDIR}/gleam_V4a.txt" 2>&1 &
V4APID=$!
sleep 6
HC0=$(powershell -NoProfile -Command "(Get-Process -Name gleam -ErrorAction SilentlyContinue).HandleCount")
wait $V4APID
V4AEC=$?
if [ "$V4AEC" -ne 0 ]; then bad "V4a: abnormal exit (code $V4AEC)"; fi
# V4b: 20 restarts. HandleCount is sampled mid-loop (gleam is always alive
# while restart commands flow); detach ends it after the sample.
( for i in $(seq 1 20); do printf 'restart\n'; done; sleep 14; printf 'detach\n' ) | timeout 90 "$GLEAM" $TARGET > ${TDIR}/gleam_V4b.txt 2>&1 &
V4BPID=$!
sleep 12
HC1=$(powershell -NoProfile -Command "(Get-Process -Name gleam -ErrorAction SilentlyContinue).HandleCount")
wait $V4BPID
V4BEC=$?
if [ $V4BEC -ne 0 ]; then bad "V4b: abnormal exit (code $V4BEC)"; fi
if [ -n "$HC0" ] && [ -n "$HC1" ] && [ $((HC1 - HC0)) -le 2 ]; then
  ok "V4: handles stable across restarts ($HC0 -> $HC1)"
else
  bad "V4: handle growth ($HC0 -> $HC1)"
fi

# --- V4c: attach/detach cycles + Init failure (handle ownership, P0-7) ---
# PassThru gives us THIS run's exact pid - never touch another notepad.
NPID=$(powershell -NoProfile -Command "(Start-Process 'C:\Windows\notepad.exe' -PassThru).Id")
V4COK=0
for i in $(seq 1 5); do
  out=$(printf 'pause\ndetach\n' | timeout 15 "$GLEAM" -a $NPID 2>&1)
  ec=$?
  n=$(printf '%s' "$out" | grep -c 'stop reason=attach\|stop reason=pause')
  [ $ec -eq 0 ] && [ "$n" -ge 1 ] && V4COK=$((V4COK+1))
done
# Cleanup: kill only if the pid still maps to the expected image.
powershell -NoProfile -Command "\$p = Get-Process -Id $NPID -ErrorAction SilentlyContinue; if(\$p -and \$p.Path -eq 'C:\Windows\notepad.exe') { Stop-Process -Id $NPID -Force }" > /dev/null 2>&1
if [ "$V4COK" -eq 5 ]; then ok "V4c: 5/5 attach-detach cycles"; else bad "V4c: $V4COK/5 attach-detach cycles"; fi
timeout 10 "$GLEAM" C:\definitely\missing\target.exe > ${TDIR}/gleam_V4d.txt 2>&1
ec=$?
if [ $ec -eq 1 ]; then ok "V4d: Init failure exits 1"; else bad "V4d: Init failure exit code $ec"; fi

# --- V5: once logical bp consumed + RVA bounds (P0-4) ---
run V5 "" <<EOF
bp TestTarget!marker once
g
bl
bp TestTarget+FFFFFF
g
EOF
chkcount "V5: once logical gone" ${TDIR}/gleam_V5.txt "logical module=testtarget" 0
chk "V5: rva out of image"       ${TDIR}/gleam_V5.txt "out of image for module testtarget"

# --- V6: stepout abort on pause (S0-1) ---
( printf 'bp TestTarget!looper\ng\nret 100000\n'; sleep 1; printf 'pause\n'; sleep 1; printf 'detach\n' ) | timeout 30 "$GLEAM" $TARGET > ${TDIR}/gleam_V6.txt 2>&1
ec=$?
echo "== V6 =="
if [ $ec -ne 0 ]; then bad "V6: abnormal exit (code $ec)"; fi
chk "V6: stepout aborted"        ${TDIR}/gleam_V6.txt "stepout aborted (pause)"
chk "V6: pause stop"             ${TDIR}/gleam_V6.txt "stop reason=pause"

# --- W1: delayed bp on no-export DLL + PDB-only symbol (P0-4) ---
run W1 "dll2" <<EOF
bp NoExp+1000
bp Late!LateInternal
bl
bp TestTarget!marker
g
bl
g
g
quit
EOF
chk "W1: noexp pending"            ${TDIR}/gleam_W1.txt "breakpoint pending module=noexp rva=0x1000"
chk "W1: noexp bound via path"     ${TDIR}/gleam_W1.txt "event bp bound module=noexp address=0x"
chk "W1: noexp in-image rva kept"  ${TDIR}/gleam_W1.txt "logical module=noexp rva=0x1000 bound=0x"
chk "W1: late bound via pdb retry" ${TDIR}/gleam_W1.txt "event bp bound module=late address=0x"
chk "W1: noexp loaded"             ${TDIR}/gleam_W1.txt "NOEXP_LOADED=1"
chk "W1: late loaded"              ${TDIR}/gleam_W1.txt "LATE_LOADED=1"

# --- W2: second chance pauses; explicit pass is the only escape (P0-3) ---
run W2 "av" <<EOF
g
exception pass
exception pass
EOF
chk "W2: first chance"             ${TDIR}/gleam_W2.txt "chance=first"
chk "W2: second chance pauses"     ${TDIR}/gleam_W2.txt "chance=second"
chk "W2: dies after pass"          ${TDIR}/gleam_W2.txt "stop reason=exit code=0xC0000005"

# --- W3: address errors belong to their own command (P0-1) ---
run W3 "dll" <<EOF
bp version!GetFileVersionInfoSizeW
free zzz
until zzz
g
quit
EOF
chk "W3: pending clears probe err" ${TDIR}/gleam_W3.txt "error: unknown name 'zzz'"
chkcount "W3: two distinct errors" ${TDIR}/gleam_W3.txt "error: unknown name 'zzz'" 2

# --- W4: stepout arg validation + user-bp conflict (S0-1) ---
run W4 "" <<EOF
ret 1 extra
bp $MCALLNEXT
bp TestTarget!marker
g
ret
rbp $MCALLNEXT
ret
g
g
EOF
chk "W4: extra arg rejected"       ${TDIR}/gleam_W4.txt "usage: ret [maxsteps-hex]"
chk "W4: conflict reported"        ${TDIR}/gleam_W4.txt "stepout error: user breakpoint at 0x"
chk "W4: stepout works after rbp"  ${TDIR}/gleam_W4.txt "stop reason=stepout return"

# --- W5: read ansi at address 0 (P0-5) ---
run W5 "" <<EOF
read ansi 0
read utf16 0
g
EOF
chk "W5: ansi at 0 is error"       ${TDIR}/gleam_W5.txt "cannot read string at 0x0"

# --- W6: extended sub-registers (P0-6) ---
run W6 "" <<EOF
setreg r8 1122334455667788
setreg r8d AABBCCDD
regs r8
setreg r8w BEEF
regs r8
regs r8b
regs r8w
setreg sil AA
regs sil
setreg r9 0
setreg r9b 5A
regs r9
g
EOF
chk "W6: r8d rmw"                  ${TDIR}/gleam_W6.txt "r8 = 0x11223344AABBCCDD"
chk "W6: r8w rmw"                  ${TDIR}/gleam_W6.txt "r8 = 0x11223344AABBBEEF"
chk "W6: r8b read"                 ${TDIR}/gleam_W6.txt "r8b = 0xEF"
chk "W6: r8w read"                 ${TDIR}/gleam_W6.txt "r8w = 0xBEEF"
chk "W6: sil rmw"                  ${TDIR}/gleam_W6.txt "sil = 0xAA"
chk "W6: r9b rmw"                  ${TDIR}/gleam_W6.txt "r9 = 0x000000000000005A"

# --- W7: PDB-only symbol binds at the load event itself (P0-4) ---
run W7 "dll2" <<EOF
bp Late!LateInternal
g
g
g
EOF
chk "W7: bound at load event"    ${TDIR}/gleam_W7.txt "event bp bound module=late address=0x"
chk "W7: first call hit"         ${TDIR}/gleam_W7.txt "stop reason=breakpoint"

# --- W9: frames source labels (P0-2) ---
timeout 30 "$GLEAM" "$BTARGET" > ${TDIR}/gleam_W9.txt 2>&1 <<EOF
g
write 60000000 10 00 00 60 00 00 00 00
setreg rsp 60000000
setreg rip 60000000
frames 0 4
quit
EOF
ec=$?
echo "== W9 =="
if [ $ec -ne 0 ]; then bad "W9: abnormal exit (code $ec)"; fi
chk "W9: untrusted label"        ${TDIR}/gleam_W9.txt "source=untrusted"
chk "W9: non-module marked"      ${TDIR}/gleam_W9.txt "module=?"

# --- W8: UTF-16 code unit across a readable page boundary (P0-5) ---
# The SAME page-tail address must read consistently via read u16 and read
# utf16: next page is committed in BoundaryTarget, so the code unit is
# assembled across the boundary. No false "cannot read" error.
timeout 30 "$GLEAM" "$BTARGET" > ${TDIR}/gleam_W8.txt 2>&1 <<EOF
g
read u16 60000FFF
read utf16 60000FFF 1
quit
EOF
ec=$?
echo "== W8 =="
if [ $ec -ne 0 ]; then bad "W8: abnormal exit (code $ec)"; fi
chk "W8: u16 cross-page"        ${TDIR}/gleam_W8.txt "= 0x9090"
chk "W8: utf16 reads cross-page" ${TDIR}/gleam_W8.txt "string at 0x60000FFF = "
chkcount "W8: no false error"   ${TDIR}/gleam_W8.txt "cannot read string at 0x60000FFF" 0

# --- W7b: PDB-only binding survives restart (P0-4 lifecycle) ---
run W7b "dll2" <<EOF
bp Late!LateInternal
g
restart
g
g
g
EOF
chkcount "W7b: bound in both sessions" ${TDIR}/gleam_W7b.txt "event bp bound module=late" 2
chkcount "W7b: hit in both sessions"   ${TDIR}/gleam_W7b.txt "stop reason=breakpoint type=software" 2
B1=$(grep -n "event bp bound module=late" ${TDIR}/gleam_W7b.txt | tail -1 | cut -d: -f1)
L1=$(grep -n "LATE_LOADED=1" ${TDIR}/gleam_W7b.txt | tail -1 | cut -d: -f1)
if [ -n "$B1" ] && [ -n "$L1" ] && [ "$B1" -lt "$L1" ]; then
  ok "W7b: session2 binds before dllmain ($B1 < $L1)"
else
  bad "W7b: session2 bind order ($B1 vs $L1)"
fi

# --- selftest: rangeInImage unit boundaries ---
run ST "" <<EOF
selftest
g
EOF
chk "selftest: rangeInImage"     ${TDIR}/gleam_ST.txt "selftest rangeInImage 12/12 ok"
chk "selftest: excpolicy"        ${TDIR}/gleam_ST.txt "selftest excpolicy 16/16 ok"

echo
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
