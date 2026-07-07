#!/usr/bin/env python3
"""Drive a sicha notcurses demo inside a pty, headless.

notcurses probes the terminal at init (CPR, primary/secondary DA,
XTWINOPS size reports, kitty keyboard queries) and blocks until the
primary-DA answer arrives — a bare pty never answers, so this harness
plays terminal: it watches the output stream for the probe burst,
answers it, optionally types scripted keys, captures everything the
demo renders, and asserts expected substrings.

Usage:
  pty_demo.py [--keys "text\\n"] [--timeout N] [--expect STR]...
              -- <cmd> [args...]

Exit code: the demo's own exit code, or 1 on missing expectations /
timeout (the transcript tail is printed for diagnosis).
"""

import argparse
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time

# Answers for the notcurses init probe burst.  DA1 is the terminator
# notcurses waits for; CPR + size reports keep its geometry sane.
PROBE_ANSWERS = (
    b"\x1b[1;1R"          # CPR (cursor position report)
    b"\x1b[?62;22c"       # primary DA: VT220-ish, ANSI color
    b"\x1b[>1;10;0c"      # secondary DA
    b"\x1b[8;24;80t"      # text area: 24x80
    b"\x1b[4;384;640t"    # pixel area
)

# Kitty-keyboard-protocol advertisement: answering the CSI ? u query
# makes notcurses keep the protocol enabled, so --keys may then inject
# CSI-u sequences (e.g. Shift+A as b"\x1b[97:65;2;65u") to test
# modifier-aware input paths.
KITTY_ANSWER = b"\x1b[?0u"


def run(cmd, keys, timeout, expects, kitty=False):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        try:
            os.execvp(cmd[0], cmd)
        finally:
            os._exit(127)

    # a fresh pty reports a 0x0 winsize: notcurses cannot establish
    # geometry and refuses to init.  Give it a real one.
    fcntl.ioctl(fd, termios.TIOCSWINSZ,
        struct.pack("HHHH", 24, 80, 640, 384))

    out = bytearray()
    answered = False
    keys_sent = keys is None
    deadline = time.time() + timeout
    key_time = None

    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                break  # child closed the pty
            if not chunk:
                break
            out.extend(chunk)
        # answer the probe burst once the DA1 query has been seen
        if not answered and b"\x1b[c" in out:
            os.write(fd, (KITTY_ANSWER if kitty else b"")
                + PROBE_ANSWERS)
            answered = True
            key_time = time.time() + 1.0  # let the UI settle
        if answered and not keys_sent and time.time() >= key_time:
            os.write(fd, keys)
            keys_sent = True

    # collect the exit status (bounded)
    code = None
    for _ in range(50):
        wpid, status = os.waitpid(pid, os.WNOHANG)
        if wpid == pid:
            if os.WIFEXITED(status):
                code = os.WEXITSTATUS(status)
            else:
                code = 128 + os.WTERMSIG(status)
            break
        time.sleep(0.1)
    if code is None:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        print("pty_demo: TIMEOUT (child killed)", file=sys.stderr)
        code = 1
    os.close(fd)

    text = out.decode("utf-8", "replace")
    plain = re.sub(r"\x1b\[[0-9;:?<>=]*[a-zA-Z]|\x1b[P_\]^X].*?"
        r"(\x1b\\|\x07)|\x1b[()][0-9A-B]|[\x00-\x08\x0b-\x1f]", "",
        text)
    failed = [e for e in expects if e not in plain]
    if failed:
        print("pty_demo: missing expected output:", file=sys.stderr)
        for e in failed:
            print(f"  !! {e!r}", file=sys.stderr)
        print("--- transcript tail ---", file=sys.stderr)
        print(plain[-2000:], file=sys.stderr)
        return 1
    for e in expects:
        print(f"pty_demo: saw {e!r}")
    return code


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keys", default=None,
        help="bytes to type once the UI is up (\\n etc. escaped)")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--kitty", action="store_true",
        help="advertise the kitty keyboard protocol (then inject "
        "CSI-u sequences via --keys)")
    ap.add_argument("--expect", action="append", default=[],
        help="substring that must appear in the rendered output")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    cmd = args.cmd
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if not cmd:
        ap.error("no command given")
    keys = None
    if args.keys is not None:
        keys = args.keys.encode().decode("unicode_escape").encode()
    code = run(cmd, keys, args.timeout, args.expect, kitty=args.kitty)
    print(f"pty_demo: exit {code}")
    return code


if __name__ == "__main__":
    sys.exit(main())
