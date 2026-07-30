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
OUT=$(printf 'eval TestTarget!marker\neval TestTarget!looper\nquit\n' | timeout 30 "$GLEAM" $TARGET 2>&1)
MBODY=$(printf '%s\n' "$OUT" | sed -n 's/^= 0x\([0-9A-F]*\).*/\1/p' | head -1)
LADDR=$(printf '%s\n' "$OUT" | sed -n 's/^= 0x\([0-9A-F]*\).*/\1/p' | sed -n '2p')
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
# inner's slow-loop body (main thread only in mt/mtl mode): the target of
# the first backward jump inside inner, located by disassembly. Used by W16
# to make the main thread raise a constant stream of breakpoint exceptions.
#
# The probe must NOT use gleam's "!inner" symbol resolution: under
# incremental linking the PDB can point at a ZOMBIE body (an outdated copy
# the linker replaced), and a breakpoint computed from it lands
# mid-instruction in the REAL body - it corrupted "sub rsp, imm" once and
# sent main into an access violation. Instead follow the target's own
# printed INNER (the ILT thunk) through its jmp to the real body, and
# require the slow loop's cmp against 10000000 (0x989680) in the listing as
# proof the right function was found.
OUT3=$(printf 'disasm 0x%s 2\nquit\n' "$INNER" | timeout 30 "$GLEAM" $TARGET 2>&1)
IBODY=$(printf '%s\n' "$OUT3" | sed -n 's/^0000000[0-9A-F]*  jmp 0x0000000\([0-9A-F]*\).*$/\1/p' | head -1)
[ -z "$IBODY" ] && IBODY=$INNER
OUT4=$(printf 'disasm 0x%s 60\nquit\n' "$IBODY" | timeout 30 "$GLEAM" $TARGET 2>&1)
ILOOP=$(printf '%s\n' "$OUT4" | awk '$1 ~ /^0000000[0-9A-F]+$/ && $2 ~ /^j/ && $3 ~ /^0x0000000[0-9A-F]+$/ { a = strtonum("0x" $1); t = strtonum("0x" substr($3, 3)); if (t < a) { printf "%X", t; exit } }')
printf '%s\n' "$OUT4" | grep -q '989680' || { echo "FATAL: ILOOP probe lost the slow loop (IBODY=$IBODY)"; exit 1; }
for v in MARKER INNER GDATA MBODY LADDR OEP MRET MCALLNEXT GD2 GD4 GD6 GD8 ILOOP; do
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
# The pause MAY legitimately inject BEFORE quitting starts (it was issued
# while not quitting - since the stub no longer depends on resolving
# ExitThread, that early injection now succeeds). What must not happen is
# an injection once detach is in flight: forceBreakIn refuses then, and the
# deferred detach cleanup reuses the existing stub instead of injecting.
run R6 "" <<EOF
bp $MARKER
g
pause
detach
EOF
chk "R6: detaching"              ${TDIR}/gleam_R6.txt "detaching..."
read -r r6Inj < <(awk '/detach deferred/ { f=1 } f && /event breakin injected/ { n++ } END { print n+0 }' ${TDIR}/gleam_R6.txt)
if [ "$r6Inj" = "0" ]; then
  ok "R6: no injection after detach"
else
  bad "R6: $r6Inj injection(s) after detach (see ${TDIR}/gleam_R6.txt)"
fi
# The detached target inherits gleam's stdout, so its completion output
# lands in this log AFTER gleam has already exited - give it a moment.
sleep 2
chk "R6: target ran to completion" ${TDIR}/gleam_R6.txt "MARKER_RESULT_2=13"

# --- R7: pause->quit must NOT re-inject ---
run R7 "" <<EOF
bp $MARKER
g
pause
quit
EOF
read -r r7Inj < <(awk '/stop reason=pause/ { f=1 } f && /event breakin injected/ { n++ } END { print n+0 }' ${TDIR}/gleam_R7.txt)
if [ "$r7Inj" = "0" ]; then
  ok "R7: no injection after quit"
else
  bad "R7: $r7Inj injection(s) after quit (see ${TDIR}/gleam_R7.txt)"
fi

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
# V4a: baseline handle count (pure measurement; runtime pause->detach is
# proven by V4e). The two g's leave gleam paused at the second marker hit
# until the delayed detach, so the sample window is deterministic.
( printf "bp $MARKER\ng\ng\n"; sleep 2; printf 'pause\n'; sleep 7; printf 'detach\n' ) | timeout 60 "$GLEAM" "$TARGET" > "${TDIR}/gleam_V4a.txt" 2>&1 &
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
  printf '%s\n' "$out" > ${TDIR}/gleam_V4c_$i.txt
  echo "iter=$i pid=$NPID ec=$ec hits=$n" >> "${TDIR}/pressure.log"
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

# --- V4e: runtime pause -> detach must not leak suspended threads ---
echo "== V4e =="
V4EOK=0
for i in $(seq 1 30); do
  out=$(( printf 'bp TestTarget!looper\ng\nret 100000\n'; sleep 1; printf 'pause\n'; sleep 1; printf 'detach\n' ) | timeout 20 "$GLEAM" "$TARGET" 2>&1)
  ec=$?
  n=$(printf '%s' "$out" | grep -c 'stop reason=pause')
  TPID=$(printf '%s' "$out" | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | head -1)
  # gleam has exited; the detached target must COMPLETE on its own (a
  # thread left suspended would keep it alive forever).
  done_ok=0
  for w in 1 2 3 4 5; do
    alive=$(powershell -NoProfile -Command "if(Get-Process -Id $TPID -ErrorAction SilentlyContinue) { 1 }" 2>/dev/null)
    [ -z "$alive" ] && { done_ok=1; break; }
    sleep 1
  done
  if [ $ec -eq 0 ] && [ "$n" -eq 1 ] && [ "$done_ok" -eq 1 ]; then
    V4EOK=$((V4EOK+1))
  else
    printf '%s' "$out" > ${TDIR}/gleam_V4e_fail_$i.txt
  fi
  echo "iter=$i ec=$ec pause=$n done=$done_ok" >> "${TDIR}/pressure.log"
done
if [ "$V4EOK" -eq 30 ]; then
  ok "V4e: 30/30 runtime pause->detach, target completes"
else
  bad "V4e: $V4EOK/30 runtime pause->detach"
fi

# --- W10: new ret after abort leaves no stale internal bp ---
( printf 'bp TestTarget!looper\ng\nret 100000\n'; sleep 1; printf 'pause\n'; sleep 1; printf 'bl\ng\nret 1000000\ng\nquit\n' ) | timeout 60 "$GLEAM" "$TARGET" > ${TDIR}/gleam_W10.txt 2>&1
ec=$?
echo "== W10 =="
if [ $ec -ne 0 ]; then bad "W10: abnormal exit (code $ec)"; fi
chk "W10: aborted on pause"        ${TDIR}/gleam_W10.txt "stepout aborted (pause)"
chkcount "W10: no stale once bp"   ${TDIR}/gleam_W10.txt " once" 0
chk "W10: new ret completes"       ${TDIR}/gleam_W10.txt "stop reason=stepout return"

# --- W11: delayed bp unbind/re-bind across unload/reload ---
run W11 "dll3" <<EOF
bp Late!LateInternal
g
g
g
quit
EOF
chkcount "W11: bound twice"        ${TDIR}/gleam_W11.txt "event bp bound module=late" 2
chk "W11: unbound on unload"       ${TDIR}/gleam_W11.txt "event bp unbound module=late"
chkcount "W11: hit both instances" ${TDIR}/gleam_W11.txt "stop reason=breakpoint type=software" 2
run W11b "dll3" <<EOF
bp Late!NoSuchSymbol
g
quit
EOF
chkcount "W11b: bogus never binds" ${TDIR}/gleam_W11b.txt "event bp bound module=late" 0
chk "W11b: session still clean"    ${TDIR}/gleam_W11b.txt "stop reason=exit"

# --- W13: UTF-16 boundary matrix (P0-5) ---
# Single block: the full matrix. An earlier, shorter duplicate wrote the same
# log file and was overwritten by this one - assertions must count once.
timeout 30 "$GLEAM" "$BTARGET" > ${TDIR}/gleam_W13.txt 2>&1 <<EOF
g
read utf16 602FFFFF 2
read utf16 602FFFFE 2
read utf16 60000001 2
write 60000000 3D D8 00 DE 00 00
read utf16 60000000 4
write 6000003E 3D D8 00 DE 00 00
read utf16 6000003E 2
protect 60001000 1000 n
read utf16 60000FFF 1
protect 60001000 1000 rwx
read utf16 60000FFF 1
quit
EOF
ec=$?
echo "== W13 =="
if [ $ec -ne 0 ]; then bad "W13: abnormal exit (code $ec)"; fi
chk "W13: unreadable next page -> error" ${TDIR}/gleam_W13.txt "cannot read string at 0x602FFFFF"
chk "W13: prefix then partial"           ${TDIR}/gleam_W13.txt "(partial: read failed at 0x60300000)"
chk "W13: odd address reads"             ${TDIR}/gleam_W13.txt "string at 0x60000001"
chk "W13: surrogate pair"                ${TDIR}/gleam_W13.txt "😀"
chk "W13: surrogate across chunk"        ${TDIR}/gleam_W13.txt "string at 0x6000003E = \"😀\""
chk "W13: noaccess -> error"             ${TDIR}/gleam_W13.txt "cannot read string at 0x60000FFF"
chkcount "W13: restored -> readable"     ${TDIR}/gleam_W13.txt "string at 0x60000FFF = " 1

# --- W14: stepout internal bp hit by a non-owner thread (S0-1) ---
# Per-iteration verdict: every run must show ec=0 AND at least one non-owner
# hit AND exactly one stepout return AND exactly one normal exit. Counting the
# two kinds of evidence separately across runs would pass even if no single
# run ever both saw a non-owner hit and completed.
echo "== W14 =="
W14OK=0
for i in $(seq 1 10); do
  timeout 30 "$GLEAM" $TARGET mt > ${TDIR}/gleam_W14_$i.txt 2>&1 <<EOF
bp TestTarget!marker once
g
ret
g
g
g
g
g
g
g
g
g
g
g
g
g
g
g
g
quit
EOF
  ec=$?
  n=$(grep -cF "hit by non-owner" ${TDIR}/gleam_W14_$i.txt)
  r=$(grep -cF "stop reason=stepout return" ${TDIR}/gleam_W14_$i.txt)
  x=$(grep -cF "stop reason=exit code=0x00000000" ${TDIR}/gleam_W14_$i.txt)
  if [ $ec -eq 0 ] && [ "$n" -ge 1 ] && [ "$r" -eq 1 ] && [ "$x" -eq 1 ]; then
    W14OK=$((W14OK+1))
  else
    cp ${TDIR}/gleam_W14_$i.txt ${TDIR}/gleam_W14_fail_$i.txt
  fi
  echo "W14 iter=$i ec=$ec nonowner=$n stepoutret=$r exit=$x" >> "${TDIR}/pressure.log"
done
if [ "$W14OK" -eq 10 ]; then
  ok "W14: 10/10 runs ec=0 + non-owner>=1 + exactly 1 return + clean exit"
else
  bad "W14: $W14OK/10 runs fully clean (see ${TDIR}/gleam_W14_fail_*.txt)"
fi
if grep -qE "stop reason=step " ${TDIR}/gleam_W14_*.txt; then
  bad "W14: spurious stop reason=step after abort"
else
  ok "W14: no spurious step stops"
fi

# --- W14b: what may follow a non-owner stop (execution-control matrix) ---
# At a non-owner stop the old stepout is still active with a deferred re-arm.
# Each follow-up command must either complete it or abort it through the one
# entry point - never leave a stale internal bp or emit a phantom step stop.
# ec=0 for every case: a hang or crash here must not count as a pass.
w14b() { # w14b <tag> <expected-pattern> <follow-up commands...>
  local tag=$1 expect=$2; shift 2
  local f=${TDIR}/gleam_W14b_$tag.txt
  { printf 'bp TestTarget!marker once\ng\nret\n'
    printf '%s\n' "$@"
    printf 'bl\nquit\n'; } | timeout 40 "$GLEAM" $TARGET mt > $f 2>&1
  local ec=$?
  if [ $ec -ne 0 ]; then bad "W14b/$tag: abnormal exit (code $ec; see $f)"; return; fi
  if grep -qF "hit by non-owner" $f; then
    ok "W14b/$tag: reached a non-owner stop"
  else
    bad "W14b/$tag: no non-owner stop reached (see $f)"
    return
  fi
  # Each follow-up command must prove ITS OWN semantics, not just "no crash".
  if grep -qF "$expect" $f; then
    ok "W14b/$tag: follow-up output '$expect'"
  else
    bad "W14b/$tag: expected '$expect' (see $f)"
  fi
  # No internal one-shot may outlive the operation: bl prints "once" for
  # single-shot breakpoints, and the user bp in this scenario is "once" too -
  # it is consumed at the first hit, well before these follow-ups.
  if grep -qF " once" $f; then
    bad "W14b/$tag: leftover one-shot breakpoint in bl (see $f)"
  else
    ok "W14b/$tag: no leftover internal bp"
  fi
}
echo "== W14b =="
w14b g        "stop reason=stepout return" g g g g g g g g g g g g g g g
w14b step     "stop reason=step rip=" "step"
w14b stepover "stop reason=step rip=" "stepover"
w14b tgo      "stop reason=trace " "tgo rax!=0 100"
w14b until    "stop reason=breakpoint type=software address=0x$LADDR" "until TestTarget!looper"
w14b newret   "stop reason=stepout return" "ret"
# Aborting the old stepout must not delete a USER breakpoint - not the normal
# one that happens to sit at the internal one-shot's own address, and not an
# unrelated one-shot.
#
# The bp address matters. At a non-owner stop rip IS the call-skip address, so
# a one-shot placed there is legitimately consumed by the following step's own
# hit - "gone from bl" would then be correct behaviour and the assertion could
# not tell that apart from a wrong delete. A one-shot is therefore placed at an
# address the single step cannot reach (inner, called later), while the shared-
# address case is covered by the normal bp, which no hit can consume.
w14b_user() { # w14b_user <tag> <bp-addr> <bp-suffix>
  local tag=$1; local addr=$2; local suffix=$3
  local f=${TDIR}/gleam_W14c_$tag.txt
  { printf 'bp TestTarget!marker once\ng\nret\n'
    printf 'bp %s %s\nbl\nstep\nbl\nquit\n' "$addr" "$suffix"; } |
    timeout 40 "$GLEAM" $TARGET mt > $f 2>&1
  local ec=$?
  if [ $ec -ne 0 ]; then bad "W14c/$tag: abnormal exit (code $ec; see $f)"; return; fi
  if ! grep -qF "hit by non-owner" $f; then
    bad "W14c/$tag: no non-owner stop reached (see $f)"
    return
  fi
  # The abort happens between the two bl listings, so the user bp must appear
  # in both. And it must not have been consumed by a hit in between either -
  # that would make "listed twice" unreachable for the one-shot case anyway.
  local n=$(grep -cF "0x$addr  software" $f)
  if [ "$n" -eq 2 ]; then
    ok "W14c/$tag: user bp survives the stepout abort"
  else
    bad "W14c/$tag: user bp listings=$n, want 2 (see $f)"
  fi
}
echo "== W14c =="
w14b_user shared "$MCALLNEXT" ""
w14b_user once   "$INNER"     "once"

# Same address as the internal one-shot: after the non-owner consumed it,
# physical ownership is gone (mStepOutBpOurs=false), so a user "once" there
# must hit as a NORMAL user breakpoint - never claimed as internal (C2).
timeout 40 "$GLEAM" $TARGET mt > ${TDIR}/gleam_W14c_sameaddr.txt 2>&1 <<EOF
bp TestTarget!marker once
g
ret
bp $MCALLNEXT once
g
g
g
g
g
g
g
g
g
g
g
g
g
g
g
g
bl
quit
EOF
ec=$?
echo "== W14c/sameaddr =="
if [ $ec -ne 0 ]; then bad "W14c/sameaddr: abnormal exit (code $ec)"; fi
# The user once must fire as a NORMAL user breakpoint, EXACTLY ONCE, and must
# never be claimed by the stepout bookkeeping. A non-owner stop at the same
# address prints the identical stop line AND an internal-claim event, so the
# whole-file match proves nothing. The only window in which a stop at
# 0x$MCALLNEXT can ONLY be the user once is between "one-shot breakpoint set"
# (the user once became the physical bp) and the next "re-armed" (the internal
# int3 is written back and later hits are claimed as internal again).
read -r wStop wInt < <(awk -v set="one-shot breakpoint set at 0x$MCALLNEXT" \
       -v hit="stop reason=breakpoint type=software address=0x$MCALLNEXT" \
       'index($0, set)          { f=1; next }
        f && /re-armed/         { exit }
        f && index($0, hit)     { s++ }
        f && /hit by non-owner/ { i++ }
        END                     { print s+0, i+0 }' ${TDIR}/gleam_W14c_sameaddr.txt)
if [ "$wStop" = "1" ] && [ "$wInt" = "0" ]; then
  ok "W14c/sameaddr: user once fires once, no internal claim"
else
  bad "W14c/sameaddr: window stops=$wStop (want 1), internal claims=$wInt (want 0) (see ${TDIR}/gleam_W14c_sameaddr.txt)"
fi
chk "W14c/sameaddr: stepout completes" ${TDIR}/gleam_W14c_sameaddr.txt "stop reason=stepout return"
# After the operation completes the last bl must be clean: neither the user
# once (consumed by its own hit) nor the internal one (consumed by owner).
chkcount "W14c/sameaddr: no leftover once" ${TDIR}/gleam_W14c_sameaddr.txt " once" 0

# Abort path with a user once at the internal address: abortStepOut must NOT
# delete it (ownership was lost at the non-owner hit), it must still fire.
timeout 40 "$GLEAM" $TARGET mt > ${TDIR}/gleam_W14c_abort.txt 2>&1 <<EOF
bp TestTarget!marker once
g
ret
bp $MCALLNEXT once
bl
ret
g
bl
quit
EOF
ec=$?
echo "== W14c/abort =="
if [ $ec -ne 0 ]; then bad "W14c/abort: abnormal exit (code $ec)"; fi
chk "W14c/abort: user once listed"   ${TDIR}/gleam_W14c_abort.txt "0x$MCALLNEXT  software int3"
chk "W14c/abort: unified abort"        ${TDIR}/gleam_W14c_abort.txt "stepout aborted (new ret)"
# The user once fires AFTER the abort, EXACTLY ONCE, with no internal claim,
# and the hit must come from the OWNER thread - the thread stopped at the
# non-owner stop is the one that issues the new `ret`, so it IS the new
# operation's owner. This is the deterministic owner-hit counterpart of
# W14c/sameaddr (whose hit comes from the non-owner busyWorker): setting a bp
# while the internal one is armed is unreachable in the command model (any
# command window means the internal bp was already consumed), so owner vs
# non-owner can only be distinguished AFTER ownership was lost.
read -r wStop wInt wOwner < <(awk -v set="one-shot breakpoint set at 0x$MCALLNEXT" \
       -v hit="stop reason=breakpoint type=software address=0x$MCALLNEXT" \
       '!f && index($0, hit) { if(owner == "" && match($0, /tid=[0-9]+/)) owner = substr($0, RSTART + 4, RLENGTH - 4) }
        index($0, set)       { f = 1; next }
        f && index($0, hit)  { s++
                               if(owner != "" && match($0, /tid=[0-9]+/) && substr($0, RSTART + 4, RLENGTH - 4) == owner) o++ }
        f && /hit by non-owner/ { i++ }
        END                  { print s + 0, i + 0, o + 0 }' ${TDIR}/gleam_W14c_abort.txt)
if [ "$wStop" = "1" ] && [ "$wInt" = "0" ] && [ "$wOwner" = "1" ]; then
  ok "W14c/abort: owner hits the user once exactly once, no internal claim"
else
  bad "W14c/abort: post-set stops=$wStop (want 1), internal claims=$wInt (want 0), owner hits=$wOwner (want 1) (see ${TDIR}/gleam_W14c_abort.txt)"
fi
# The fired once is consumed: the second bl (after the hit) must not list it.
read -r wOnce < <(awk '/stepout aborted \(new ret\)/ { f=1 }
                       f && / once/                  { n++ }
                       END                            { print n+0 }' ${TDIR}/gleam_W14c_abort.txt)
if [ "$wOnce" = "0" ]; then
  ok "W14c/abort: fired once gone from bl"
else
  bad "W14c/abort: $wOnce leftover once listing(s) after abort (see ${TDIR}/gleam_W14c_abort.txt)"
fi

# --- W14d: teardown paths at a non-owner stop (quit / restart / owner exit) ---
# quit and restart must go through the SAME abort entry point as pause and
# detach; the old operation may not re-arm, print or delete anything after it.
echo "== W14d =="
printf 'bp TestTarget!marker once\ng\nret\nquit\n' |
  timeout 40 "$GLEAM" $TARGET mt > ${TDIR}/gleam_W14d_quit.txt 2>&1
ec=$?
if [ $ec -ne 0 ]; then bad "W14d/quit: abnormal exit (code $ec)"; fi
chk      "W14d/quit: non-owner stop reached" ${TDIR}/gleam_W14d_quit.txt "hit by non-owner"
chkcount "W14d/quit: single unified abort"   ${TDIR}/gleam_W14d_quit.txt "stepout aborted (quit)" 1
chkcount "W14d/quit: no re-arm after abort"  ${TDIR}/gleam_W14d_quit.txt "stepout internal bp re-armed" 0

printf 'bp TestTarget!marker once\ng\nret\nrestart\nbl\ng\nquit\n' |
  timeout 60 "$GLEAM" $TARGET mt > ${TDIR}/gleam_W14d_restart.txt 2>&1
ec=$?
if [ $ec -ne 0 ]; then bad "W14d/restart: abnormal exit (code $ec)"; fi
chk      "W14d/restart: non-owner stop reached" ${TDIR}/gleam_W14d_restart.txt "hit by non-owner"
chkcount "W14d/restart: single unified abort"   ${TDIR}/gleam_W14d_restart.txt "stepout aborted (restart)" 1
# The new session starts from clean per-operation state: no stepout output, no
# leftover breakpoint, and the target completes normally.
chk      "W14d/restart: new session clean bl"   ${TDIR}/gleam_W14d_restart.txt "no breakpoints"
chkcount "W14d/restart: no stale stepout stop"  ${TDIR}/gleam_W14d_restart.txt "stop reason=stepout" 0
chk      "W14d/restart: target completes"       ${TDIR}/gleam_W14d_restart.txt "MARKER_RESULT_2=13"

# Owner thread dies inside the callee while the internal bp is armed (mtx).
printf 'bp TestTarget!exiter once\ng\nret\ng\nquit\n' |
  timeout 40 "$GLEAM" $TARGET mtx > ${TDIR}/gleam_W14d_ownerexit.txt 2>&1
ec=$?
if [ $ec -ne 0 ]; then bad "W14d/ownerexit: abnormal exit (code $ec)"; fi
chkcount "W14d/ownerexit: aborted on thread exit" ${TDIR}/gleam_W14d_ownerexit.txt "stepout aborted (thread exit)" 1
chkcount "W14d/ownerexit: no phantom step stop"   ${TDIR}/gleam_W14d_ownerexit.txt "stop reason=step " 0
chk      "W14d/ownerexit: target completes"       ${TDIR}/gleam_W14d_ownerexit.txt "EXITER_DONE=1"
chk      "W14d/ownerexit: clean exit"             ${TDIR}/gleam_W14d_ownerexit.txt "stop reason=exit code=0x00000000"

# --- W14e: ignore count layered on the stepout internal bp (S0-1, HIGH) ---
# The HIGH finding: cbBreakpoint() applied the user ignore count BEFORE the
# stepout bookkeeping and returned early, while the engine deleted the one-shot
# regardless - so the operation stayed "active" with no physical breakpoint and
# the target ran to exit. "ignore" is a bare address->count map that needs no
# user breakpoint at the address, which is exactly how it reaches the internal
# one-shot. Both the owner and the non-owner hit must survive it.
echo "== W14e =="
printf 'bp TestTarget!marker once\ng\nignore %s 1\nret\nbl\ng\ng\n' "$MCALLNEXT" |
  timeout 40 "$GLEAM" $TARGET > ${TDIR}/gleam_W14e_owner.txt 2>&1
ec=$?
if [ $ec -ne 0 ]; then bad "W14e/owner: abnormal exit (code $ec)"; fi
chkcount "W14e/owner: stepout still returns"    ${TDIR}/gleam_W14e_owner.txt "stop reason=stepout return" 1
# The internal one-shot is not a user breakpoint: its hit must be consumed by
# the stepout bookkeeping, never by the user's ignore counter.
chkcount "W14e/owner: internal hit not ignored" ${TDIR}/gleam_W14e_owner.txt "event ignored address=0x$MCALLNEXT" 0
chk      "W14e/owner: no leftover internal bp"  ${TDIR}/gleam_W14e_owner.txt "no breakpoints"
chk      "W14e/owner: target completes"         ${TDIR}/gleam_W14e_owner.txt "MARKER_RESULT_2=13"
chk      "W14e/owner: clean exit"               ${TDIR}/gleam_W14e_owner.txt "stop reason=exit code=0x00000000"

# Non-owner path: a worker thread may take the ignore instead. Whichever thread
# it lands on, the internal bookkeeping for that hit must have printed BEFORE
# "event ignored" (that ordering IS the fix), and stepout must still return
# exactly once. An ignored hit with no bookkeeping line ahead of it is the
# regression.
W14EOK=0
for i in $(seq 1 5); do
  { printf 'bp TestTarget!marker once\ng\nignore %s 1\nret\n' "$MCALLNEXT"
    for k in $(seq 1 16); do printf 'g\n'; done
    printf 'quit\n'; } |
    timeout 40 "$GLEAM" $TARGET mt > ${TDIR}/gleam_W14e_mt_$i.txt 2>&1
  ec=$?
  f=${TDIR}/gleam_W14e_mt_$i.txt
  r=$(grep -cF "stop reason=stepout return" $f)
  no=$(grep -cF "hit by non-owner" $f)
  ig=$(grep -cF "event ignored address=0x$MCALLNEXT" $f)
  ex=$(grep -cF "stop reason=exit code=0x00000000" $f)
  # Every object must EXIST before the order is judged - an absent non-owner
  # or ignore line must fail, never pass vacuously.
  ord=0
  if [ "$no" -ge 1 ] && [ "$ig" -ge 1 ]; then
    igo=$(grep -nF "event ignored address=0x$MCALLNEXT" $f | head -1 | cut -d: -f1)
    noo=$(grep -nF "hit by non-owner" $f | head -1 | cut -d: -f1)
    [ "$noo" -le "$igo" ] && ord=1
  fi
  if [ $ec -eq 0 ] && [ "$r" -eq 1 ] && [ "$ord" -eq 1 ] && [ "$ex" -eq 1 ]; then
    W14EOK=$((W14EOK+1))
  else
    cp $f ${TDIR}/gleam_W14e_fail_$i.txt
  fi
  echo "W14e iter=$i ec=$ec stepoutret=$r nonowner=$no ignored=$ig order_ok=$ord exit=$ex" >> "${TDIR}/pressure.log"
done
if [ "$W14EOK" -eq 5 ]; then
  ok "W14e: 5/5 runs ec=0 + exactly 1 return + bookkeeping before ignore + exit"
else
  bad "W14e: $W14EOK/5 runs clean (see ${TDIR}/gleam_W14e_fail_*.txt)"
fi

# --- W15: module identity via CodeView GUID (P0-4) ---
# The bind log always prints the LOGICAL module name ("late"), so grepping for
# "module=noexp" can never catch a mis-bind. Nor is an address-range test alone
# enough: the decoy REUSES the base Late#1 just freed, so Late#1's legitimate
# bind address also lies inside NoExp's range. The assertion must therefore be
# ordered - a bind prints during the load event that produced it, immediately
# before that event's stop line, so each bind pairs with exactly one load:
#   load Late#1 -> bind | unload Late#1 -> unbind | load NoExp -> MUST NOT bind
#   | load Late#2 -> bind
# We record the real bases/ranges of all three images and prove: 2 binds total,
# one paired with each Late load, ZERO paired with the decoy load, the two Late
# bases differ, and no bind/hit that exists while the decoy is loaded falls in
# the decoy's range.
W15F=${TDIR}/gleam_W15.txt
timeout 60 "$GLEAM" $TARGET dll4 > $W15F 2>&1 <<EOF
bp Late!LateInternal
breakon dll on
g
modules
g
modules
g
modules
g
modules
g
modules
g
modules
g
modules
g
modules
selftest
quit
EOF
ec=$?
echo "== W15 =="
if [ $ec -ne 0 ]; then bad "W15: abnormal exit (code $ec; see $W15F)"; fi

# Single pass over the log. A bind prints during the load event that produced
# it (immediately before that event's stop line), so pending-bind -> next load
# is an exact pairing. Image names come from the modules snapshots: the loader
# list lags one event, and a base is named by the FIRST snapshot after its own
# load - which is what disambiguates the decoy reusing Late#1's address.
W15_EV=$(awk '
  function h2d(s, i, c, n, d) {
    s = toupper(s); n = 0
    for (i = 1; i <= length(s); i++) {
      c = substr(s, i, 1); d = index("0123456789ABCDEF", c) - 1
      if (d < 0) return -1
      n = n * 16 + d
    }
    return n
  }
  function hx(s) { sub(/^0x/, "", s); sub(/^0+/, "", s); if (s == "") s = "0"; return toupper(s) }
  /^event bp bound module=late address=0x/ {
    a = $0; sub(/.*address=/, "", a); sub(/ .*/, "", a)
    binds++; bindAddr[binds] = hx(a); pending = binds; next
  }
  /^stop reason=dll op=load base=0x/ {
    b = $0; sub(/.*base=/, "", b); sub(/ .*/, "", b)
    loads++; loadBase[loads] = hx(b); loadName[loads] = ""; loadBind[loads] = 0
    if (pending) { loadBind[loads] = pending; bindLoad[pending] = loads; pending = 0 }
    next
  }
  NF == 3 && $1 ~ /^[0-9A-F]+$/ && $2 ~ /^[0-9A-F]+$/ {
    b = hx($1)
    for (i = 1; i <= loads; i++)
      if (loadName[i] == "" && loadBase[i] == b) { loadName[i] = $3; loadSize[i] = hx($2) }
    next
  }
  /^stop reason=breakpoint type=software address=0x/ {
    a = $0; sub(/.*address=/, "", a); sub(/ .*/, "", a)
    hits++; hitAddr[hits] = hx(a); hitLoad[hits] = loads; next
  }
  END {
    latePairs = 0; decoyPairs = 0; decoyIdx = 0
    for (i = 1; i <= loads; i++) {
      if (loadName[i] == "Late.dll") {
        if (loadBind[i]) latePairs++
        if (!(loadBase[i] in lateBase)) { lateBase[loadBase[i]] = 1; lateBases++ }
      }
      if (loadName[i] == "NoExp.dll") {
        if (loadBind[i]) decoyPairs++
        if (!decoyIdx) { decoyIdx = i; dBase = h2d(loadBase[i]); dSize = h2d(loadSize[i]) }
      }
    }
    # Binds/hits that exist while the decoy is loaded must not fall in its range.
    bindInDecoy = 0; hitInDecoy = 0
    if (decoyIdx) {
      for (i = 1; i <= binds; i++) {
        a = h2d(bindAddr[i])
        if (bindLoad[i] >= decoyIdx && a >= dBase && a < dBase + dSize) bindInDecoy++
      }
      for (i = 1; i <= hits; i++) {
        a = h2d(hitAddr[i])
        if (hitLoad[i] >= decoyIdx && a >= dBase && a < dBase + dSize) hitInDecoy++
      }
    }
    # Every hit must be at one of the bound addresses, and every bind hit once.
    match_ok = (binds == hits) ? 1 : 0
    for (i = 1; i <= binds; i++) {
      n = 0
      for (j = 1; j <= hits; j++) if (hitAddr[j] == bindAddr[i]) n++
      if (n != 1) match_ok = 0
    }
    # Was the decoy image actually identified (base AND size)? Without this the
    # two "zero inside the decoy range" checks would pass vacuously.
    decoyFound = (decoyIdx && dSize > 0) ? 1 : 0
    printf "%d %d %d %d %d %d %d %d %d\n",
           binds, latePairs, decoyPairs, lateBases, bindInDecoy, hitInDecoy, hits,
           match_ok, decoyFound
    for (i = 1; i <= loads; i++)
      printf "W15 load#%d base=%s name=%s size=%s bind=%s\n", i, loadBase[i],
             loadName[i] == "" ? "?" : loadName[i],
             loadSize[i] == "" ? "?" : loadSize[i],
             loadBind[i] ? bindAddr[loadBind[i]] : "-" > "/dev/stderr"
  }
' "$W15F" 2>>"${TDIR}/pressure.log")
read W15_BIND_N W15_LATEPAIRS W15_DECOYPAIRS W15_LATE_N W15_BINDINDECOY \
     W15_HITINDECOY W15_HIT_N W15_HITSMATCH W15_DECOYFOUND <<EOT
$W15_EV
EOT

# The decoy must have been identified by name AND size, otherwise the two
# "nothing inside the decoy range" checks below would pass without testing
# anything, and "zero binds at the NoExp load event" would be vacuous too.
if [ "${W15_DECOYFOUND:-0}" -eq 1 ]; then
  ok "W15: decoy image identified in the loader snapshots"
else
  bad "W15: NoExp.dll not identified by the parser (see ${TDIR}/pressure.log)"
fi

# Two Late loads at two DIFFERENT bases: the reload scenario only proves
# something if the second load did not land back on the first base.
if [ "${W15_LATE_N:-0}" -eq 2 ]; then
  ok "W15: two distinct Late image bases observed"
else
  bad "W15: distinct Late bases=${W15_LATE_N:-0}, want 2 (see $W15F)"
fi
# Exactly two binds, no more: an extra bind means a wrong image was accepted.
if [ "${W15_BIND_N:-0}" -eq 2 ]; then
  ok "W15: exactly 2 binds"
else
  bad "W15: binds=${W15_BIND_N:-0}, want 2 (see $W15F)"
fi
# Both binds paired with a Late.dll LOAD event - the pairing is by event order,
# not by address, so the decoy reusing Late#1's freed base cannot mask a
# mis-bind (and cannot frame a correct one either).
if [ "${W15_LATEPAIRS:-0}" -eq 2 ]; then
  ok "W15: both binds paired with a Late load event"
else
  bad "W15: binds paired with Late loads=${W15_LATEPAIRS:-0}, want 2 (see $W15F)"
fi
# The decisive one: no bind may be attributed to the decoy's load event.
if [ "${W15_DECOYPAIRS:-0}" -eq 0 ]; then
  ok "W15: zero binds at the NoExp load event"
else
  bad "W15: ${W15_DECOYPAIRS} bind(s) at the NoExp load (see $W15F)"
fi
# Nothing bound or hit inside the decoy's live range while it is loaded.
if [ "${W15_BINDINDECOY:-1}" -eq 0 ]; then
  ok "W15: zero binds inside the live NoExp range"
else
  bad "W15: ${W15_BINDINDECOY} bind(s) inside the live NoExp range (see $W15F)"
fi
if [ "${W15_HITINDECOY:-1}" -eq 0 ]; then
  ok "W15: zero hits inside the live NoExp range"
else
  bad "W15: ${W15_HITINDECOY} hit(s) inside the live NoExp range (see $W15F)"
fi
# Two hits, one per bound address: a hit at an address that was never bound (or
# a bound address never hit) means the int3 went somewhere unintended.
if [ "${W15_HIT_N:-0}" -eq 2 ] && [ "${W15_HITSMATCH:-0}" -eq 1 ]; then
  ok "W15: 2 hits, one per bound address"
else
  bad "W15: hits=${W15_HIT_N:-0} match=${W15_HITSMATCH:-0}, want 2/1 (see $W15F)"
fi
chk "W15: decoy actually loaded"   $W15F "DECOY_LOADED=1"
chk "W15: late reloaded"           $W15F "LATE2=1"
# Supplementary only: the identity function's own unit check. It cannot stand
# in for the event-level base verification above.
chk "W15: identity selftest (aux)" $W15F "selftest modid 2/2 ok"

# --- W16: engine API fault injection (selftest failapi) ---
# The loop's wait/continue/reply-later/resume failure paths cannot be forced
# from outside, so the engine exposes dormant test hooks
# (GleeBug::Debugger::mTestHook*) that "selftest failapi" arms. Hooks fail
# ONCE and disarm themselves (the "always" variant fails persistently until
# "selftest failapi off"); session init clears any armed-but-unfired hook.
# Every log is held to an EXACT internal-error count, so an extra
# cleanup/handle/resume error can never hide behind the expected one - this
# is also why the suite-wide sweep may keep excluding these logs.
echo "== W16 =="

# chk_pid_gone <desc> <file>: the target of a LAUNCH session must be gone
# once gleam has exited (the OS terminates a debuggee whose debugger died).
#
# pid_state <pid>: alive|gone|error. tasklist exits 0 with an INFO line when
# the pid does not exist, so "no pid in output" means GONE - but a nonzero
# exit (query itself broken) must surface as ERROR, never as a false "gone".
pid_state() {
  local out ec
  out=$(${TASKLIST:-tasklist} //FI "PID eq $1" 2>&1); ec=$?
  if [ $ec -ne 0 ]; then echo error; return; fi
  if printf '%s\n' "$out" | grep -q " $1 "; then echo alive; else echo gone; fi
}
# pid_state self-checks: a forced query failure must read as "error" (never
# a false "gone"), a certainly-dead pid as "gone" - otherwise the PID gate
# below could pass on a broken tasklist.
[ "$(TASKLIST=false pid_state 12345)" = "error" ] && ok "pid_state: query failure distinguished" || bad "pid_state: query failure not distinguished"
[ "$(pid_state 39999999)" = "gone" ] && ok "pid_state: dead pid reads gone" || bad "pid_state: dead pid misread"
chk_pid_gone() {
  local pid=$(sed -n 's/^event process op=create pid=\([0-9]*\).*/\1/p' "$2" | head -1)
  sleep 1
  local st=$(pid_state "$pid")
  if [ -n "$pid" ] && [ "$st" = "gone" ]; then
    ok "$1"
  else
    bad "$1 (pid=$pid state=$st; see $2)"
  fi
}

# VirtualQueryEx helper for the W16/attach MEM_FREE proof: prints STATE=<n>
# for (pid, address); 0x10000 (65536) = MEM_FREE. Kept as a file so bash
# quoting cannot mangle the C# source.
cat > ${TDIR}/vq.ps1 <<'PSEOF'
param([uint32]$TargetPid, [uint64]$Addr)
$src = @'
using System;
using System.Runtime.InteropServices;
public static class VQ
{
    [StructLayout(LayoutKind.Sequential)]
    public struct MBI
    {
        public IntPtr BaseAddress;
        public IntPtr AllocationBase;
        public uint AllocationProtect;
        public IntPtr RegionSize;
        public uint State;
        public uint Protect;
        public uint Type;
    }
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern UIntPtr VirtualQueryEx(IntPtr h, IntPtr addr, out MBI mbi, UIntPtr len);
    public static uint StateOf(uint pid, ulong addr)
    {
        IntPtr h = OpenProcess(0x0400, false, pid); // PROCESS_QUERY_INFORMATION
        if(h == IntPtr.Zero) return 0xFFFFFFF1;
        MBI m;
        UIntPtr r = VirtualQueryEx(h, new IntPtr((long)addr), out m, (UIntPtr)Marshal.SizeOf(typeof(MBI)));
        CloseHandle(h);
        if(r == UIntPtr.Zero) return 0xFFFFFFF2;
        return m.State;
    }
}
'@
Add-Type -TypeDefinition $src
"STATE=$([VQ]::StateOf($TargetPid, $Addr))"
PSEOF

# wait: the first WaitForDebugEvent after arming fails -> error + loop break.
run W16_wait "" <<EOF
selftest failapi wait
bp TestTarget!marker once
g
EOF
chkre     "W16/wait: precise error"    ${TDIR}/gleam_W16_wait.txt '^event error msg="Debugger::WaitForDebugEvent failed \(error 5\)"'
chkcount  "W16/wait: exactly 1 error"  ${TDIR}/gleam_W16_wait.txt 'event error msg=' 1
chk       "W16/wait: controlled exit"  ${TDIR}/gleam_W16_wait.txt "[gleam] session finished"
chk_pid_gone "W16/wait: no target residue" ${TDIR}/gleam_W16_wait.txt

# continue: the normal ContinueDebugEvent of the next resume fails.
run W16_continue "" <<EOF
bp TestTarget!marker once
g
selftest failapi continue
g
EOF
chkre     "W16/continue: precise error"   ${TDIR}/gleam_W16_continue.txt '^event error msg="Debugger::ContinueDebugEvent failed \(error 5, pid=[0-9]+, tid=[0-9]+\)"'
chkcount  "W16/continue: exactly 1 error" ${TDIR}/gleam_W16_continue.txt 'event error msg=' 1
chk       "W16/continue: controlled exit" ${TDIR}/gleam_W16_continue.txt "[gleam] session finished"
chk_pid_gone "W16/continue: no target residue" ${TDIR}/gleam_W16_continue.txt

# resume: stepping in mt mode suspends the busy worker; resuming it after the
# step fails ONCE. The failure must be reported, the loop must carry on (the
# step stop still surfaces) and - because the engine keeps failed suspension
# entries and retries at the next resume site - the detach cleanup must
# resume the frozen thread for real. Proof is thread-level: the busy worker
# only prints BUSYWORKER_DONE=1 if it actually ran to its end (process exit
# alone cannot prove it - ExitProcess also kills frozen threads). The target
# inherits gleam's stdout, so its output keeps landing in the log after the
# debugger is gone; give it a moment before checking.
run W16_resume "mt" <<EOF
bp TestTarget!marker once
g
selftest failapi resume
step
detach
EOF
WPID=$(sed -n 's/^event process op=create pid=\([0-9]*\).*/\1/p' ${TDIR}/gleam_W16_resume.txt | head -1)
sleep 2
chkre    "W16/resume: precise error"     ${TDIR}/gleam_W16_resume.txt '^event error msg="Debugger: ResumeThread failed for tid [0-9]+ \(error 5\)"'
chkcount "W16/resume: exactly 1 error"   ${TDIR}/gleam_W16_resume.txt 'event error msg=' 1
chk      "W16/resume: loop continues"    ${TDIR}/gleam_W16_resume.txt "stop reason=step rip="
chk      "W16/resume: detached"          ${TDIR}/gleam_W16_resume.txt "detaching..."
chk      "W16/resume: frozen thread resumed and completed" ${TDIR}/gleam_W16_resume.txt "BUSYWORKER_DONE=1"
WPSTATE=$(pid_state "$WPID")
if [ -n "$WPID" ] && [ "$WPSTATE" = "gone" ]; then
  ok "W16/resume: target process exited on its own"
else
  bad "W16/resume: target pid=$WPID state=$WPSTATE (see ${TDIR}/gleam_W16_resume.txt)"
fi
# A broken tasklist must FAIL this scenario, not pass it: with the query
# forced to fail, the same pid check yields "error", never "gone".
if [ -n "$WPID" ] && [ "$(TASKLIST=false pid_state "$WPID")" = "error" ]; then
  ok "W16/resume: pid query failure would fail the gate"
else
  bad "W16/resume: pid query failure not surfaced (pid=$WPID)"
fi

# reply-later: inject a DBG_REPLY_LATER continue failure. Both threads hammer
# their own auto-continued breakpoint (ignore): main inside inner's slow loop
# (ILOOP), the worker at inner's entry - using "mtl" mode, whose partner
# thread hammers for 5000 marker calls. The partner's LIFETIME is what
# matters: a deferral needs one thread's exception queued while the other's
# bp-hit internal step runs, and a partner that exits after 12 calls (~1ms
# with an ignored bp) leaves main hammering alone - no deferral can ever
# happen afterwards (Release gate proved it: ~1M solo hits, zero deferrals).
#
# HONEST LIMITATION: even with a long-lived partner the exact overlap is a
# kernel scheduling artifact, so the scenario still retries bounded times.
# Each attempt either injects (the handling is asserted on that log) or
# times out (~10s, a plain miss); all attempts missing = FAIL. What the gate
# proves is the HANDLING of the failure, never the scheduling itself.
rlLog=""
for rlTry in 1 2 3; do
  printf "selftest failapi replylater\nbp 0x$ILOOP\nignore 0x$ILOOP 200000000\nbp 0x$INNER\nignore 0x$INNER 200000000\ng\n" |
    timeout 10 "$GLEAM" $TARGET mtl > ${TDIR}/gleam_W16_replylater.txt 2>&1
  if grep -qE 'ContinueDebugEvent\(DBG_REPLY_LATER\) failed' ${TDIR}/gleam_W16_replylater.txt; then
    rlLog="yes"
    break
  fi
done
echo "== W16/replylater =="
if [ -z "$rlLog" ]; then
  bad "W16/replylater: no deferral in 3 attempts (see ${TDIR}/gleam_W16_replylater.txt)"
else
  ok "W16/replylater: injected (attempt $rlTry)"
fi
chkre     "W16/replylater: main hammered"      ${TDIR}/gleam_W16_replylater.txt "event ignored address=0x$ILOOP"
chkre     "W16/replylater: worker hammered"    ${TDIR}/gleam_W16_replylater.txt "event ignored address=0x$INNER"
chkre     "W16/replylater: precise error"     ${TDIR}/gleam_W16_replylater.txt '^event error msg="Debugger::ContinueDebugEvent\(DBG_REPLY_LATER\) failed \(error 5, pid=[0-9]+, tid=[0-9]+\)"'
chkcount  "W16/replylater: exactly 1 error"   ${TDIR}/gleam_W16_replylater.txt 'event error msg=' 1
chk       "W16/replylater: controlled exit"   ${TDIR}/gleam_W16_replylater.txt "[gleam] session finished"
chk_pid_gone "W16/replylater: no target residue" ${TDIR}/gleam_W16_replylater.txt

# restart: an armed-but-never-fired hook must not leak into the restarted
# session (session init clears all hooks). The new session's step does real
# safe-step suspend/resume work, so a leaked resume hook would print an
# error here.
run W16_restart "" <<EOF
selftest failapi resume
restart
bp TestTarget!marker once
g
step
g
g
quit
EOF
chk      "W16/restart: hooked session armed" ${TDIR}/gleam_W16_restart.txt "selftest failapi armed resume"
chkcount "W16/restart: zero injected errors" ${TDIR}/gleam_W16_restart.txt 'event error msg=' 0
chk      "W16/restart: step works in new session" ${TDIR}/gleam_W16_restart.txt "stop reason=step rip="
chk      "W16/restart: new session completes"     ${TDIR}/gleam_W16_restart.txt "MARKER_RESULT_2=13"

# attach + PERMANENT resume failure: the detach must be REFUSED while any
# debugger-owned suspension cannot be restored - an attached target keeps
# running after we leave, so frozen threads would stay frozen forever. The
# engine retries with a bound, then aborts the detach and reports it through
# cbDetachRefused SYNCHRONOUSLY; gleam re-arms the command loop immediately
# and forceBreakIn manufactures the event that re-enters it.
#
# The breakpoint is ONE-SHOT on purpose: after its single hit the target is
# completely quiet (main spins, the worker is frozen), so the off + second
# detach can only be processed if the refusal itself woke the command loop -
# a persistent breakpoint would mask a missing synchronous report by
# generating follow-up events. The suspension comes from the breakpoint
# re-execution machinery (internal step suspends the other threads); the
# "step" follow-up lets the post-event suspend fire (a "detach" at the
# first pause would skip suspension entirely).
#
# Error assertions: every internal error in the log must match the whitelist
# (injected resume failures with error 5, or the single expected refusal) -
# anything else fails the gate even though the suite-wide sweep skips W16.
"$TARGET" wait > ${TDIR}/gleam_W16_attach_target.txt 2>&1 &
ATT_BG=$!
sleep 1
ATT_PID=$(cat /proc/$ATT_BG/winpid 2>/dev/null)
if [ -z "$ATT_PID" ]; then
  bad "W16/attach: cannot resolve target pid"
else
  printf 'selftest failapi resume always\nbp kernel32!GetTickCount once\ng\nstep\ndetach\nselftest failapi off\ndetach\n' |
    timeout 60 "$GLEAM" -a "$ATT_PID" > ${TDIR}/gleam_W16_attach.txt 2>&1
  ec=$?
  echo "== W16/attach =="
  if [ $ec -ne 0 ]; then bad "W16/attach: abnormal exit (code $ec; see ${TDIR}/gleam_W16_attach.txt)"; fi
  chk      "W16/attach: persistent hook armed"  ${TDIR}/gleam_W16_attach.txt "selftest failapi armed resume always"
  chkre    "W16/attach: resume failure (error 5)" ${TDIR}/gleam_W16_attach.txt '^event error msg="Debugger: ResumeThread failed for tid [0-9]+ \(error 5\)"'
  chk      "W16/attach: engine refuses detach"  ${TDIR}/gleam_W16_attach.txt "Detach refused: threads still suspended by us"
  chkcount "W16/attach: exactly one refusal"    ${TDIR}/gleam_W16_attach.txt "Detach refused: threads still suspended by us" 1
  chk      "W16/attach: command loop re-armed"  ${TDIR}/gleam_W16_attach.txt "detach refused: target left attached"
  # The stub cleanup must never hit its timeout path: the deferred detach
  # waits for the stub thread's EXIT_THREAD event, frees the page, and only
  # then lets go.
  chkcount "W16/attach: no stub cleanup timeout" ${TDIR}/gleam_W16_attach.txt "wait=0x102" 0
  chk      "W16/attach: detach deferred for stub" ${TDIR}/gleam_W16_attach.txt "detach deferred: waiting for break-in stub thread to exit"
  chkre    "W16/attach: stub page freed"        ${TDIR}/gleam_W16_attach.txt "^event breakin stub freed page=0x[0-9A-Fa-f]+"
  # Whitelist: every internal error must FULL-LINE match one of the two
  # expected shapes (anchored, CRLF-aware): an injected resume failure with
  # error 5, or the single refusal with a STRICT tid list (at least one
  # number; multiple tids separated by exactly one space) and its fixed tail.
  WL_RE='^event error msg="Debugger: ResumeThread failed for tid [0-9]+ \(error 5\)"\r?$|^event error msg="Debugger::Detach refused: threads still suspended by us \(tid [0-9]+( [0-9]+)*\) - detach aborted, target left attached"\r?$'
  wlBad=$(grep 'event error msg=' ${TDIR}/gleam_W16_attach.txt | grep -cvE "$WL_RE")
  if [ "$wlBad" -eq 0 ]; then
    ok "W16/attach: all errors within whitelist"
  else
    bad "W16/attach: $wlBad unexpected internal error(s) (see ${TDIR}/gleam_W16_attach.txt)"
  fi
  # Whitelist meta-test: a legal prefix with junk appended must be REJECTED,
  # clean specimens must be ACCEPTED (proves the patterns really anchor).
  printf 'event error msg="Debugger: ResumeThread failed for tid 123 (error 5); cleanup failed"\r\n' > ${TDIR}/wl_neg.txt
  printf 'event error msg="Debugger::Detach refused: threads still suspended by us (tid 123 456) - detach aborted, target left attached"\r\n' > ${TDIR}/wl_pos.txt
  if [ "$(grep -cvE "$WL_RE" ${TDIR}/wl_neg.txt)" -eq 1 ] && [ "$(grep -cvE "$WL_RE" ${TDIR}/wl_pos.txt)" -eq 0 ]; then
    ok "W16/attach: whitelist anchors verified"
  else
    bad "W16/attach: whitelist anchoring broken"
  fi
  # Strict tid-list meta-test: empty, space-only, double-space and trailing-
  # space lists must ALL be rejected by the whitelist pattern.
  printf 'event error msg="Debugger::Detach refused: threads still suspended by us (tid ) - detach aborted, target left attached"\r\n' > ${TDIR}/wl_tid.txt
  printf 'event error msg="Debugger::Detach refused: threads still suspended by us (tid  ) - detach aborted, target left attached"\r\n' >> ${TDIR}/wl_tid.txt
  printf 'event error msg="Debugger::Detach refused: threads still suspended by us (tid 123  456) - detach aborted, target left attached"\r\n' >> ${TDIR}/wl_tid.txt
  printf 'event error msg="Debugger::Detach refused: threads still suspended by us (tid 123 ) - detach aborted, target left attached"\r\n' >> ${TDIR}/wl_tid.txt
  wlTidBad=$(grep -cvE "$WL_RE" ${TDIR}/wl_tid.txt)
  if [ "$wlTidBad" -eq 4 ]; then
    ok "W16/attach: strict tid list rejects malformed forms"
  else
    bad "W16/attach: strict tid list accepted $((4 - wlTidBad)) malformed form(s)"
  fi
  chkcount "W16/attach: two detach completions" ${TDIR}/gleam_W16_attach.txt "detaching..." 2
  chkcount "W16/attach: single session end"     ${TDIR}/gleam_W16_attach.txt "[gleam] session finished" 1
  # The refusal must precede the successful detach.
  rLine=$(grep -nF "Detach refused" ${TDIR}/gleam_W16_attach.txt | head -1 | cut -d: -f1)
  dLine=$(grep -nF "detaching..." ${TDIR}/gleam_W16_attach.txt | tail -1 | cut -d: -f1)
  if [ -n "$rLine" ] && [ -n "$dLine" ] && [ "$rLine" -gt 0 ] && [ "$rLine" -lt "$dLine" ]; then
    ok "W16/attach: refusal before successful detach"
  else
    bad "W16/attach: ordering wrong (refusal=$rLine, last detach=$dLine)"
  fi
  # Independent proof the remote page is gone: VirtualQueryEx on the stub
  # address (from the injection log line) must report MEM_FREE (0x10000),
  # and the target must still be alive afterwards.
  stubPage=$(sed -n 's/^event breakin injected page=0x\([0-9A-Fa-f]*\).*/\1/p' ${TDIR}/gleam_W16_attach.txt | head -1)
  sleep 1
  vqState=""
  if [ -n "$stubPage" ]; then
    vqState=$(powershell -NoProfile -ExecutionPolicy Bypass -File ${TDIR}/vq.ps1 "$ATT_PID" "$(printf '%d' 0x$stubPage)" 2>/dev/null | sed -n 's/^STATE=\([0-9]*\).*/\1/p')
  fi
  if [ "$vqState" = "65536" ]; then
    ok "W16/attach: stub page MEM_FREE after detach"
  else
    bad "W16/attach: stub page state=$vqState (want 65536=MEM_FREE, page=0x$stubPage)"
  fi
  # A properly detached target is still running; kill it ourselves. The
  # pid_state self-checks above prove "alive" is not a broken-query artifact.
  if [ "$(pid_state "$ATT_PID")" = "alive" ]; then
    ok "W16/attach: target alive after detach"
  else
    bad "W16/attach: target pid=$ATT_PID state=$(pid_state "$ATT_PID") (killed instead of detached?)"
  fi
  powershell -NoProfile -Command "Stop-Process -Id $ATT_PID -Force -ErrorAction SilentlyContinue" > /dev/null 2>&1
  kill $ATT_BG 2>/dev/null
fi

# --- W17: break-in stub fault paths (gleam-side failapi) ---
# The pause break-in stub is pure int3s; the thread is terminated at its
# int3 stop and the page is freed only after the thread's EXIT_THREAD event
# confirms death. These scenarios inject the three gleam-side stub API
# failures. Logs are named gleam_W16_* on purpose: like W16 they inject
# failures intentionally, so the suite-wide internal-error sweep skips
# them - and each log is held to an exact error count here instead.
echo "== W17 =="

w17_target() { # start a fresh "wait" target; sets ATT_PID / ATT_BG
  "$TARGET" wait > ${TDIR}/gleam_W17_target.txt 2>&1 &
  ATT_BG=$!
  sleep 1
  ATT_PID=$(cat /proc/$ATT_BG/winpid 2>/dev/null)
}
w17_cleanup() {
  [ -n "$ATT_PID" ] && powershell -NoProfile -Command "Stop-Process -Id $ATT_PID -Force -ErrorAction SilentlyContinue" > /dev/null 2>&1
  kill $ATT_BG 2>/dev/null
}

# stubresume: ResumeThread of the fresh stub thread fails once. The fallback
# (DebugBreakProcess) must still deliver the pause, the never-ran thread
# must be terminated and confirmed at cleanup, and the page freed.
w17_target
if [ -z "$ATT_PID" ]; then
  bad "W17/stubresume: cannot resolve target pid"
else
  ( printf 'selftest failapi stubresume\ng\n'; sleep 1; printf 'pause\n'; sleep 1; printf 'detach\n' ) |
    timeout 60 "$GLEAM" -a "$ATT_PID" > ${TDIR}/gleam_W16_stubresume.txt 2>&1
  ec=$?
  echo "== W17/stubresume =="
  if [ $ec -ne 0 ]; then bad "W17/stubresume: abnormal exit (code $ec)"; fi
  chk      "W17/stubresume: armed"              ${TDIR}/gleam_W16_stubresume.txt "selftest failapi armed stubresume"
  chk      "W17/stubresume: resume failure"     ${TDIR}/gleam_W16_stubresume.txt "event breakin fail=resume err=5"
  chk      "W17/stubresume: fallback pauses"    ${TDIR}/gleam_W16_stubresume.txt "stop reason=pause"
  chkcount "W17/stubresume: zero internal errors" ${TDIR}/gleam_W16_stubresume.txt 'event error msg=' 0
  chk      "W17/stubresume: detached"           ${TDIR}/gleam_W16_stubresume.txt "detaching..."
  chk      "W17/stubresume: controlled exit"    ${TDIR}/gleam_W16_stubresume.txt "[gleam] session finished"
  sleep 1
  [ "$(pid_state "$ATT_PID")" = "alive" ] && ok "W17/stubresume: target alive after detach" ||
    bad "W17/stubresume: target pid=$ATT_PID not alive"
  w17_cleanup
fi

# terminate: TerminateThread on the stub thread fails once at its int3. The
# failure must be reported; the thread survives to the NEXT stub int3 (the
# page is pure int3s and the identification covers the range), where the
# retry terminates it; the deferred detach then completes normally.
w17_target
if [ -z "$ATT_PID" ]; then
  bad "W17/terminate: cannot resolve target pid"
else
  ( printf 'selftest failapi terminate\ng\n'; sleep 1; printf 'pause\n'; sleep 1; printf 'g\ndetach\n' ) |
    timeout 60 "$GLEAM" -a "$ATT_PID" > ${TDIR}/gleam_W16_terminate.txt 2>&1
  ec=$?
  echo "== W17/terminate =="
  if [ $ec -ne 0 ]; then bad "W17/terminate: abnormal exit (code $ec)"; fi
  chk      "W17/terminate: armed"               ${TDIR}/gleam_W16_terminate.txt "selftest failapi armed terminate"
  chkre    "W17/terminate: precise error"       ${TDIR}/gleam_W16_terminate.txt '^event error msg="Gleam: TerminateThread failed for break-in stub tid [0-9]+ \(error 5\)"'
  chkcount "W17/terminate: exactly 1 error"     ${TDIR}/gleam_W16_terminate.txt 'event error msg=' 1
  chkcount "W17/terminate: retried at next int3" ${TDIR}/gleam_W16_terminate.txt "stop reason=pause" 2
  chk      "W17/terminate: stub freed"          ${TDIR}/gleam_W16_terminate.txt "event breakin stub freed"
  chk      "W17/terminate: detached"            ${TDIR}/gleam_W16_terminate.txt "detaching..."
  chk      "W17/terminate: controlled exit"     ${TDIR}/gleam_W16_terminate.txt "[gleam] session finished"
  sleep 1
  [ "$(pid_state "$ATT_PID")" = "alive" ] && ok "W17/terminate: target alive after detach" ||
    bad "W17/terminate: target pid=$ATT_PID not alive"
  w17_cleanup
fi

# vfree: VirtualFreeEx of the stub page fails once at the deferred detach.
# The detach must be REFUSED with the page address KEPT; the immediate retry
# (the hook is one-shot) frees it and the second detach completes. External
# VirtualQueryEx must confirm MEM_FREE in the surviving target.
w17_target
if [ -z "$ATT_PID" ]; then
  bad "W17/vfree: cannot resolve target pid"
else
  ( printf 'g\n'; sleep 1; printf 'pause\n'; sleep 1; printf 'selftest failapi vfree\ndetach\ndetach\n' ) |
    timeout 60 "$GLEAM" -a "$ATT_PID" > ${TDIR}/gleam_W16_vfree.txt 2>&1
  ec=$?
  echo "== W17/vfree =="
  if [ $ec -ne 0 ]; then bad "W17/vfree: abnormal exit (code $ec)"; fi
  chk      "W17/vfree: stub injected"           ${TDIR}/gleam_W16_vfree.txt "event breakin injected"
  chkre    "W17/vfree: precise error"           ${TDIR}/gleam_W16_vfree.txt '^event error msg="Gleam: break-in stub page free failed for 0x[0-9A-Fa-f]+ \(error 5\)"'
  chkcount "W17/vfree: exactly 1 error"         ${TDIR}/gleam_W16_vfree.txt 'event error msg=' 1
  chk      "W17/vfree: detach refused"          ${TDIR}/gleam_W16_vfree.txt "detach refused: break-in stub page still held (free failed)"
  chk      "W17/vfree: retry freed the page"    ${TDIR}/gleam_W16_vfree.txt "event breakin stub freed"
  chkcount "W17/vfree: single successful detach" ${TDIR}/gleam_W16_vfree.txt "detaching..." 1
  chk      "W17/vfree: controlled exit"         ${TDIR}/gleam_W16_vfree.txt "[gleam] session finished"
  vPage=$(sed -n 's/^event breakin injected page=0x\([0-9A-Fa-f]*\).*/\1/p' ${TDIR}/gleam_W16_vfree.txt | head -1)
  sleep 1
  vState=""
  if [ -n "$vPage" ]; then
    vState=$(powershell -NoProfile -ExecutionPolicy Bypass -File ${TDIR}/vq.ps1 "$ATT_PID" "$(printf '%d' 0x$vPage)" 2>/dev/null | sed -n 's/^STATE=\([0-9]*\).*/\1/p')
  fi
  if [ "$vState" = "65536" ]; then
    ok "W17/vfree: stub page MEM_FREE after retry"
  else
    bad "W17/vfree: stub page state=$vState (want 65536=MEM_FREE, page=0x$vPage)"
  fi
  [ "$(pid_state "$ATT_PID")" = "alive" ] && ok "W17/vfree: target alive after detach" ||
    bad "W17/vfree: target pid=$ATT_PID not alive"
  w17_cleanup
fi

# --- SYM: ambiguous symbol handling (incremental-link zombie shape) ---
# ZombieTarget is a PINNED fixture (ZombieTarget/fixtures/): two translation
# units each define a file-static "inner", so the PDB carries two records
# for the plain name - the same shape an incremental-link zombie record
# creates. The debugger must REFUSE the ambiguous name outright (a pending
# breakpoint would bind to a guess later or spam the error at every event),
# while unambiguous symbols keep working end to end.
echo "== SYM =="
ZTARGET=ZombieTarget/fixtures/ZombieTarget.exe
ZAADDR=$(printf 'eval ZombieTarget!innerA\nquit\n' | timeout 30 "$GLEAM" $ZTARGET 2>&1 | sed -n 's/^= 0x\([0-9A-F]*\).*/\1/p' | head -1)
if [ -z "$ZAADDR" ]; then
  bad "SYM: cannot resolve innerA in the fixture"
else
  timeout 40 "$GLEAM" $ZTARGET > ${TDIR}/gleam_SYM.txt 2>&1 <<EOF
eval ZombieTarget!inner
bp ZombieTarget!inner
bl
bp ZombieTarget!innerA
g
bl
rbp 0x$ZAADDR
disasm 0x$ZAADDR 1
g
g
quit
EOF
  ec=$?
  if [ $ec -ne 0 ]; then bad "SYM: abnormal exit (code $ec; see ${TDIR}/gleam_SYM.txt)"; fi
  chk      "SYM: ambiguity refused"        ${TDIR}/gleam_SYM.txt "ambiguous symbol 'ZombieTarget!inner' (2 records"
  chk      "SYM: bp refused, not pending"  ${TDIR}/gleam_SYM.txt "breakpoint refused (ambiguous symbol)"
  chk      "SYM: no pending registered"    ${TDIR}/gleam_SYM.txt "no breakpoints"
  chkcount "SYM: refusal printed once per use" ${TDIR}/gleam_SYM.txt "ambiguous symbol 'ZombieTarget!inner'" 2
  chk      "SYM: unambiguous bp hits"      ${TDIR}/gleam_SYM.txt "stop reason=breakpoint type=software address=0x$ZAADDR"
  chk      "SYM: bp removed"               ${TDIR}/gleam_SYM.txt "breakpoint removed at 0x$ZAADDR"
  # The byte under the removed breakpoint must be restored (no int3 left).
  if grep -E "^0000000[0-9A-F]+ " ${TDIR}/gleam_SYM.txt | grep -q "int3"; then
    bad "SYM: int3 left after breakpoint removal"
  else
    ok "SYM: original byte restored"
  fi
  chkcount "SYM: no exceptions in target"  ${TDIR}/gleam_SYM.txt "stop reason=exception" 0
  chk      "SYM: target completes"         ${TDIR}/gleam_SYM.txt "R2=13"
  chk      "SYM: clean exit"               ${TDIR}/gleam_SYM.txt "stop reason=exit code=0x00000000"
fi

# --- SYM-2: cross-command pollution regression ---
# An ambiguous symbol eval must not cause a subsequent unrelated breakpoint
# to be refused. mAddrError is per-parse; ambiguity is per-symbol.
echo "== SYM-2 (cross-command pollution) =="
timeout 30 "$GLEAM" $ZTARGET > ${TDIR}/gleam_SYM2.txt 2>&1 <<EOF
eval ZombieTarget!inner
bp definitely_missing_module+123
bp ZombieTarget!innerA
bp 0x1000
g
quit
EOF
ec=$?
if [ $ec -ne 0 ]; then bad "SYM-2: abnormal exit (code $ec)"; fi
chk "SYM-2: ambiguous eval refused"     ${TDIR}/gleam_SYM2.txt "ambiguous symbol 'ZombieTarget!inner'"
chk "SYM-2: module+rva stays pending"   ${TDIR}/gleam_SYM2.txt "breakpoint pending module=definitely_missing_module rva=0x123"
chk "SYM-2: unambiguous symbol works"   ${TDIR}/gleam_SYM2.txt "stop reason=breakpoint type=software address=0x$ZAADDR"
chk "SYM-2: numeric bp works"           ${TDIR}/gleam_SYM2.txt "breakpoint set at 0x1000"
# The module+rva bp must NOT be refused as ambiguous.
if grep -q "breakpoint refused (ambiguous symbol)" ${TDIR}/gleam_SYM2.txt; then
  bad "SYM-2: cross-command ambiguous pollution"
else
  ok "SYM-2: no cross-command pollution"
fi

# --- SYM-1: pending DLL with ambiguous PDB-only symbol must refuse at bind time ---
# Late.dll now has an ambiguous "ambig" (two static functions across TUs).
# A pending "bp Late!ambig" registered BEFORE the DLL loads must be refused
# when the DLL loads (in bindModuleBreakpoints), not silently pick a record.
echo "== SYM-1 (pending DLL ambiguous PDB fallback) =="
timeout 30 "$GLEAM" $TARGET dll2 > ${TDIR}/gleam_SYM1.txt 2>&1 <<EOF
bp Late!ambig
bp Late!LateInternal
g
bl
quit
EOF
ec=$?
if [ $ec -ne 0 ]; then bad "SYM-1: abnormal exit (code $ec)"; fi
chk "SYM-1: pending registered"       ${TDIR}/gleam_SYM1.txt "breakpoint pending module=late symbol=ambig"
chk "SYM-1: ambiguous bind refused"   ${TDIR}/gleam_SYM1.txt "event bp rejected module=late symbol=ambig (ambiguous)"
chk "SYM-1: unambiguous bind success" ${TDIR}/gleam_SYM1.txt "event bp bound module=late address=0x"
chk "SYM-1: unambiguous bp hits"      ${TDIR}/gleam_SYM1.txt "stop reason=breakpoint type=software"
# The ambiguous pending must NOT remain in the list after rejection.
if grep -q "module=late symbol=ambig" ${TDIR}/gleam_SYM1.txt | tail -1 | grep -q "pending"; then
  bad "SYM-1: ambiguous pending not removed after rejection"
else
  ok "SYM-1: ambiguous pending removed after rejection"
fi
chk "SYM-1: target completes"         ${TDIR}/gleam_SYM1.txt "LATE_LOADED=1"


# --- selftest: rangeInImage unit boundaries ---
run ST "" <<EOF
selftest
g
EOF
chk "selftest: rangeInImage"     ${TDIR}/gleam_ST.txt "selftest rangeInImage 12/12 ok"
chk "selftest: excpolicy"        ${TDIR}/gleam_ST.txt "selftest excpolicy 16/16 ok"
chk "selftest: symdis"           ${TDIR}/gleam_ST.txt "selftest symdis 4/4 ok"

# --- suite-wide: no engine internal error in ANY scenario ---
# cbInternalError is the engine's only channel for "a Windows API we depend on
# failed" (ResumeThread, ContinueDebugEvent, Detach, WaitForDebugEvent lookup).
# No scenario in this suite is supposed to produce one, and nothing else asserts
# on them - so a new internal error could otherwise appear in every single log
# and the suite would still be green. This sweep is what makes such a
# regression fail the gate: it caught a bogus ERROR_INVALID_HANDLE reported on
# every normal teardown by the checked-resume path.
#
# W16 is EXCLUDED: those scenarios inject API failures on purpose
# (selftest failapi) and assert the precise error lines themselves.
EV_ERR=$(grep -l 'event error msg=' ${TDIR}/gleam_*.txt 2>/dev/null | grep -v 'gleam_W16' | tr '\n' ' ')
if [ -z "$EV_ERR" ]; then
  ok "suite: no engine internal error in any scenario"
else
  bad "suite: engine internal error reported in: $EV_ERR"
  grep -h 'event error msg=' ${TDIR}/gleam_*.txt 2>/dev/null | sort -u |
    sed 's/^/    /' >> "${TDIR}/pressure.log"
fi

echo
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
