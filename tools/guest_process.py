"""Bounded cleanup for guest processes and their heavy emulator wrappers."""
import os
import shutil
import signal
import subprocess
import threading


def _exit_on_signal(number, frame):
    # SystemExit lets the finally blocks stop the cores before the test dies.
    raise SystemExit(128 + number)


# Any test that starts guests dies by TERM or HUP on a timeout; cleanup has to run then.
if threading.current_thread() is threading.main_thread():
    signal.signal(signal.SIGTERM, _exit_on_signal)
    signal.signal(signal.SIGHUP, _exit_on_signal)


def stop_guest(process, grace=3):
    # Each caller owns a fresh process group, including any queued wrapper and its descendants.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        pass
    # A wrapper can exit before a guest that ignores TERM. Its group still needs cleaning up.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=grace)


def run_guest(args, *, timeout, capture_output=False, **kwargs):
    # heavy only where it's installed, as on a build host without emulator slots.
    command = [*(['heavy', 'emulator'] if shutil.which('heavy') else []), *map(str, args)]
    if capture_output:
        kwargs.update(stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    process = subprocess.Popen(command, start_new_session=True, **kwargs)
    cleaned = False
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        stop_guest(process)
        cleaned = True
        stdout, stderr = process.communicate(timeout=3)
        raise subprocess.TimeoutExpired(command, timeout, output=stdout, stderr=stderr) from None
    finally:
        if not cleaned:
            stop_guest(process)
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
