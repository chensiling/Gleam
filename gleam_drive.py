#!/usr/bin/env python3
"""Gleam session driver: batch-per-pause command transport.

Since commit 72eb8c1 ("命令队列状态过滤") Gleam discards any command that
arrives while the debuggee is running (Defense #1, pushCommand) and clears the
queue whenever a resume command executes (Defense #2, commandLoop). A script
piped straight into Gleam's stdin is drained by the REPL thread before the
first pause and thrown away, so the transport the old suite relied on -- write
everything up front, let it execute across pauses -- no longer exists.

This driver speaks the protocol the debugger now requires:

  1. wait for a stop that actually enters the command loop
  2. write commands up to and including the next resume command
  3. repeat from 1

Two details drive the design:

* Defense #2 discards post-resume commands SILENTLY (no "(command ignored)"
  line), so batch boundaries must come from the command names. RESUME below is
  the full set, taken from every `return CmdResult::Resume` in
  GleamCommands.Control.cpp.
* `mIsPaused` is set at the top of commandLoop, which runs AFTER emitStop has
  printed the stop line -- with rebindPendingBreakpoints() (symbol resolution)
  in between. Seeing a stop line therefore does not mean a write will be
  accepted yet, so the first command of each batch is retried while Defense #1
  rejects it. Rejections are printed synchronously by the REPL thread as it
  reads each line, so their absence within a short window means acceptance.

Usage:
  gleam_drive.py [-t SEC] [-p SEC] [--attach] -- GLEAM TARGET [ARGS...]

Commands are read from stdin, one per line. Blank lines are skipped. A line
starting with '#' is a driver directive, currently only "#sleep N". Gleam's
transcript is reproduced verbatim on stdout so existing assertions keep
matching; driver diagnostics go to stderr prefixed with "[drive]".
"""

import argparse
import os
import subprocess
import sys
import threading
import time

POLL = 0.005      # shared-state poll granularity
CONFIRM = 0.25    # window to observe a synchronous Defense #1 rejection
RETRIES = 40      # first-command retries before giving up on a batch

# Commands returning CmdResult::Resume (GleamCommands.Control.cpp). A batch
# must end at one of these: anything after it is dropped by Defense #2.
RESUME = {
    "g", "continue", "until", "step", "tgo", "stepover", "next",
    "ret", "stepout", "stepn", "restart", "quit", "detach",
}

# Handled by the REPL thread without touching the command queue, so they are
# never rejected and must NOT wait for a pause.
#
# "pause" only means anything while the debuggee RUNS: gating it on a stop would
# deadlock (nothing else is coming) and, if a stop did arrive first, the request
# would be swallowed. So it is sent the moment the script reaches it.
#
# quit/detach are deliberately NOT here. The REPL tries pushCommand first and
# only falls back to the request*() flag when that is rejected, and the flag is
# consumed after the *next debug event* -- which never arrives when the target
# sits frozen in commandLoop. Gating them on a stop keeps them on the queue
# path, where executeCommand ends the session synchronously.
REPL_IMMEDIATE = {"pause", "help"}


def is_resume(cmd):
    parts = cmd.split()
    if not parts:
        return False
    # "exception pass|handle" resumes; a bare or malformed "exception" does not.
    if parts[0] == "exception":
        return len(parts) == 2 and parts[1] in ("pass", "handle")
    return parts[0] in RESUME


def is_repl_immediate(cmd):
    parts = cmd.split()
    return bool(parts) and parts[0] in REPL_IMMEDIATE


# These read the raw pipe, so they match bytes. The prefixes are pure ASCII and
# so mean the same thing in every code page the debuggee might print in.
#
# A stop line means commandLoop was entered -- except reason=exit, where the
# process is gone and the mProcess && mThread gate fails.
def is_pause(line):
    return line.startswith(b"stop reason=") and not line.startswith(b"stop reason=exit")


def is_session_over(line):
    # ONLY the session-finished line is terminal. "stop reason=exit" means the
    # *process* died, which usually ends the session but does not on `restart`:
    # there the old process exits, then a new one is created and stops at its
    # own system breakpoint. Treating exit as terminal made the driver stop
    # sending right there and abandon the rest of the script.
    # Gleam exiting without this line (a crash, or a launch that never started)
    # is caught by the reader's EOF instead.
    return line.startswith(b"[gleam] session finished")


def is_rejection(line):
    return line.startswith(b"(command ignored:")

class Session:
    """Gleam subprocess; a reader thread turns stdout into monotonic counters.

    Counters rather than a queue: every waiter cares only about "has a new
    pause/rejection happened since I last looked", never about consuming a
    specific line, and the transcript is echoed by the reader itself.
    """

    def __init__(self, argv, out):
        self.out = out
        self.lock = threading.Lock()
        self.pauses = 0
        self.rejections = 0
        self.over = False
        self.eof = False
        # Byte mode, deliberately. The debuggee shares this pipe and writes in
        # the ANSI code page (ArgvTarget echoes its argv), so decoding as UTF-8
        # would turn those bytes into U+FFFD and the suite's byte-exact greps
        # would never match. The driver only needs ASCII line prefixes, which
        # are unambiguous in every code page it sees.
        self.proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        # Echo as lines arrive: the transcript must stay identical to a plain
        # pipe run, and must survive a crash mid-session.
        #
        # readline(), not "for line in stdout": iterating a pipe reads ahead by
        # a block, so a stop line would sit unseen in Python's buffer while the
        # driver waited for the pause it announced and the debugger waited for
        # the command that pause was supposed to unblock. readline() returns at
        # the newline.
        for line in iter(self.proc.stdout.readline, b""):
            line = line.rstrip(b"\r\n")
            # Straight to the byte layer, so the debuggee's own bytes reach the
            # suite unchanged.
            self.out.write(line + b"\n")
            self.out.flush()
            with self.lock:
                if is_rejection(line):
                    self.rejections += 1
                elif is_pause(line):
                    self.pauses += 1
                elif is_session_over(line):
                    self.over = True
        with self.lock:
            self.eof = True

    def state(self):
        with self.lock:
            return self.pauses, self.rejections, self.over or self.eof

    def send(self, cmd):
        try:
            # stdin is a byte pipe now (see __init__). Commands themselves are
            # ASCII, but encode via UTF-8 so a non-ASCII argument still goes out
            # as bytes rather than raising.
            self.proc.stdin.write(cmd.encode("utf-8", "replace") + b"\n")
            self.proc.stdin.flush()
            return True
        except (OSError, ValueError):
            return False  # Gleam exited

    def close_stdin(self):
        try:
            self.proc.stdin.close()
        except (OSError, ValueError):
            pass

    def wait_pause(self, base, budget, deadline):
        """Block until the pause counter passes `base`.

        Returns (status, pauses) with status in paused/over/timeout/deadline.
        A timeout is not fatal: a command that looked like a resume may have
        failed its own argument check and returned Handled instead, leaving us
        still paused. The caller logs it and carries on.
        """
        limit = min(time.monotonic() + budget, deadline)
        while True:
            pauses, _, over = self.state()
            if pauses > base:
                return "paused", pauses
            if over:
                return "over", pauses
            now = time.monotonic()
            if now >= limit:
                return ("deadline" if now >= deadline else "timeout"), pauses
            time.sleep(POLL)

    def send_gated(self, cmd, base, deadline):
        """Send the first command of a batch, retrying while Defense #1 rejects.

        Returns (ok, pauses). Rejections here are expected (the pause is still
        being set up) and are not protocol faults.
        """
        for _ in range(RETRIES):
            _, rej_before, over = self.state()
            if over:
                return False, base
            if not self.send(cmd):
                return False, base
            # A rejection is printed synchronously as the REPL reads the line,
            # so no rejection within CONFIRM means the command was queued.
            limit = time.monotonic() + CONFIRM
            while time.monotonic() < limit:
                _, rej_now, _ = self.state()
                if rej_now > rej_before:
                    break
                time.sleep(POLL)
            else:
                return True, base
            status, base = self.wait_pause(base, deadline - time.monotonic(), deadline)
            if status in ("over", "deadline"):
                return False, base
        return False, base

def run(session, cmds, args, deadline):
    """Feed cmds to the session. Returns (hard_faults, notes).

    Only hard faults fail the scenario. A session that ends with commands left
    over is NOT a fault: scripts legitimately over-send resume commands (e.g.
    W14b sends fifteen `g`s to drain a stepout) and the target may exit first.
    Under the old transport the surplus was simply ignored, so treating it as
    an error would fail scenarios that actually passed.
    """
    faults, notes = [], []
    pause_base = 0

    # A launched session stops at the system breakpoint on its own. An attached
    # one does not: cbAttachBreakpoint deliberately omits mWantsPause, so the
    # debuggee keeps running and pushCommand would reject the whole script.
    #
    # So take control first, the way a human would at the REPL: attach, then hit
    # pause. Only inject it when the script does not already open with its own
    # "pause" (some attach scenarios do), otherwise the session would stop twice
    # and every scenario counting "stop reason=pause" would see one too many.
    if args.attach and not (cmds and cmds[0].split()[:1] == ["pause"]):
        if session.send("pause"):
            notes.append("attach: injected an opening pause to take control")
        else:
            faults.append("attach: could not send the opening pause")

    need_pause = True
    if not args.attach:
        status, pause_base = session.wait_pause(pause_base, args.pause_wait, deadline)
        if status == "over":
            # The target can exit before the system breakpoint (V4d: a launch
            # that fails outright). The exit code carries the verdict.
            notes.append("session ended before the first pause")
        elif status != "paused":
            faults.append("no initial pause (%s)" % status)
        else:
            # This wait already consumed the system breakpoint; waiting again
            # for the first command would block on a second pause that nothing
            # has asked for yet.
            need_pause = False
    for cmd in cmds:
        if cmd.startswith("#"):
            parts = cmd.split()
            if len(parts) == 2 and parts[0] == "#sleep":
                time.sleep(min(float(parts[1]), max(0.0, deadline - time.monotonic())))
            continue

        _, _, over = session.state()
        if over:
            notes.append("session ended with %d command(s) unsent, from: %s"
                         % (len(cmds) - cmds.index(cmd), cmd))
            break

        if is_repl_immediate(cmd):
            # Bypasses the command queue: accepted whatever the run state is.
            if not session.send(cmd):
                notes.append("session gone before: %s" % cmd)
                break
            # "pause" produces a stop, so the next command has to wait for it.
            # "help" just prints and leaves the run state alone.
            if cmd.split()[0] == "pause":
                need_pause = True
            continue

        if need_pause:
            status, pause_base = session.wait_pause(pause_base, args.pause_wait, deadline)
            if status == "over":
                notes.append("session ended with %d command(s) unsent, from: %s"
                             % (len(cmds) - cmds.index(cmd), cmd))
                break
            if status == "deadline":
                faults.append("deadline reached before: %s" % cmd)
                break
            if status == "timeout":
                # Probably still paused: the previous "resume" may have failed
                # its own argument check and returned Handled. Not a fault by
                # itself -- send_gated decides.
                notes.append("pause wait timed out before: %s" % cmd)
            ok, pause_base = session.send_gated(cmd, pause_base, deadline)
            if not ok:
                _, _, over = session.state()
                if over:
                    notes.append("session ended while sending: %s" % cmd)
                else:
                    faults.append("command never accepted: %s" % cmd)
                break
            need_pause = False
        elif not session.send(cmd):
            notes.append("session gone before: %s" % cmd)
            break

        if is_resume(cmd):
            need_pause = True

    return faults, notes

def main():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("-t", "--timeout", type=float, default=60.0,
                    help="whole-session budget in seconds")
    ap.add_argument("-p", "--pause-wait", type=float, default=20.0,
                    help="per-batch wait for the next pause")
    ap.add_argument("--attach", action="store_true",
                    help="attach session: no initial pause is expected")
    ap.add_argument("cmd", nargs=argparse.REMAINDER,
                    help="-- GLEAM TARGET [ARGS...]")
    args = ap.parse_args()

    argv = args.cmd[1:] if args.cmd and args.cmd[0] == "--" else args.cmd
    if not argv:
        sys.stderr.write("[drive] usage: gleam_drive.py [-t SEC] -- GLEAM TARGET [ARGS...]\n")
        return 2

    # An attach session never stops on its own (cbAttachBreakpoint omits
    # mWantsPause), so detect "-a <pid>" and skip the initial pause wait
    # instead of making all ~50 call sites pass the flag.
    if not args.attach and len(argv) > 1 and argv[1] in ("-a", "attach"):
        args.attach = True

    # CreateProcess does not search the current directory, so a bare relative
    # program path ("bin/Debug/x64/Gleam.exe") raises FileNotFoundError while
    # "./bin/..." works. Absolutise it so callers need not know that.
    if os.path.exists(argv[0]):
        argv[0] = os.path.abspath(argv[0])

    cmds = [l.rstrip("\r\n").strip() for l in sys.stdin]
    cmds = [c for c in cmds if c]

    deadline = time.monotonic() + args.timeout
    # Byte sink: the reader writes the debuggee's bytes through unchanged.
    session = Session(argv, sys.stdout.buffer)
    faults, notes = run(session, cmds, args, deadline)

    # Closing stdin does NOT end the session: Gleam deliberately ignores EOF
    # (main.cpp replThread), so a script without quit/detach runs to the
    # target's own exit. Wait out the remaining budget, then kill.
    session.close_stdin()
    try:
        code = session.proc.wait(timeout=max(0.0, deadline - time.monotonic()))
    except subprocess.TimeoutExpired:
        faults.append("session budget exhausted; killed")
        session.proc.kill()
        code = session.proc.wait()

    _, rejections, _ = session.state()
    if rejections:
        # Retries while a pause is being set up are expected; informational.
        sys.stderr.write("[drive] %d command rejection(s) observed\n" % rejections)
    for n in notes:
        sys.stderr.write("[drive] note: %s\n" % n)
    for f in faults:
        sys.stderr.write("[drive] fault: %s\n" % f)
    # Hard faults must fail the scenario: run() in run_tests.sh checks the exit
    # status, so a silently truncated transcript cannot pass. Gleam's own
    # non-zero code takes precedence (scenarios assert on specific values).
    return code if not faults else (code or 90)


if __name__ == "__main__":
    sys.exit(main())

