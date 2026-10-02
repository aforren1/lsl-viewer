#!/usr/bin/env python3
"""Start a detached process and print its PID, or kill a PID.

MATLAB and Octave have no common way to start a background process and learn its PID.
system() also waits for the child's stdout to close, thus the child gets DEVNULL.

  python spawn.py start [NAME=VALUE ...] -- PROGRAM [ARGS ...]
  python spawn.py kill PID
"""
import os
import signal
import subprocess
import sys


def start(argv):
    sep = argv.index("--")
    env = dict(os.environ)
    for kv in argv[:sep]:
        k, _, v = kv.partition("=")
        env[k] = v
    kw = {}
    if os.name == "nt":
        kw["creationflags"] = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        kw["start_new_session"] = True
    p = subprocess.Popen(argv[sep + 1:], env=env, stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True, **kw)
    print(p.pid)


def kill(pid):
    try:
        os.kill(pid, signal.SIGTERM)  # TerminateProcess on Windows
    except OSError:
        pass


if __name__ == "__main__":
    if sys.argv[1] == "start":
        start(sys.argv[2:])
    elif sys.argv[1] == "kill":
        kill(int(sys.argv[2]))
    else:
        sys.exit("usage: spawn.py start|kill ...")
