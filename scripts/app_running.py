#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Whether a program of an app bundle is running: package-app.sh's check before it replaces the app.

    python3 -I scripts/app_running.py <path to the .app>

Prints the PID and executable of each process whose executable file is in the app's Contents/MacOS/
(the game, a --compile-shaders run, the bundle's tools) and exits 0 when there is one, 1 when there
is none. It goes by the kernel's record of each process's executable (proc_pidpath), not its command
line: `pgrep -f "<App>.app/Contents/MacOS/"` also matched any process whose arguments merely named the
path (a shell loop waiting for the game, an editor, a log tail), and refused a rebuild with "is
running: quit it first" while nothing of the app was. Another copy of the app elsewhere doesn't
count: only this one is replaced."""
import ctypes
import os
import sys

PROC_PIDPATHINFO_MAXSIZE = 4096


def running(app):
    """(pid, executable) of each process whose executable is in `app`/Contents/MacOS/."""
    folder = os.path.join(os.path.realpath(app), 'Contents', 'MacOS') + os.sep
    libproc = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True)
    count = libproc.proc_listallpids(None, 0)
    if count <= 0:
        raise OSError(ctypes.get_errno(), 'proc_listallpids failed')
    pids = (ctypes.c_int * (count + 64))()   # room for processes started since the count
    count = libproc.proc_listallpids(pids, ctypes.sizeof(pids))
    if count <= 0:
        raise OSError(ctypes.get_errno(), 'proc_listallpids failed')
    found = []
    path = ctypes.create_string_buffer(PROC_PIDPATHINFO_MAXSIZE)
    for pid in pids[:count]:
        if pid <= 0 or libproc.proc_pidpath(pid, path, PROC_PIDPATHINFO_MAXSIZE) <= 0:
            continue   # gone, or not ours to read (a process of another user is no program of this app)
        executable = path.value.decode('utf-8', 'surrogateescape')
        if executable.startswith(folder):
            found.append((pid, executable))
    return found


def main(argv):
    if len(argv) != 2:
        print('Usage: python3 -I scripts/app_running.py <path to the .app>', file=sys.stderr)
        return 2
    try:
        found = running(argv[1])
    except OSError as error:
        print(f'app_running: {error}', file=sys.stderr)
        return 2
    for pid, executable in found:
        print(f'{pid}\t{executable}')
    return 0 if found else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
