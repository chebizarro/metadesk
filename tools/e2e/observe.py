#!/usr/bin/env python3
"""Timestamp a native process's output; receipts stay in ignored output/.

SIGTERM/INT forward to the owned process group. A deadline is a failure bound,
never success or relay/capture completion. Exit 124 means deadline/forced kill.
"""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import signal
import subprocess
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--pid-file', type=Path, required=True)
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parent / 'output')
    parser.add_argument('--deadline', type=float, default=600)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command or args.deadline <= 0 or Path(args.name).name != args.name:
        parser.error('provide a command, a positive failure deadline and a simple name')
    args.out.mkdir(parents=True, exist_ok=True)
    path = args.out / (args.name + '.jsonl')
    with path.open('x') as log:
        started = time.monotonic()
        expired = threading.Event()
        forced = threading.Event()
        stopping = threading.Event()
        child = None
        claimed_pid = False
        kill_timer = None
        lock = threading.Lock()

        def emit(**data):
            data.update(utc=dt.datetime.now(dt.timezone.utc).isoformat(),
                        elapsed=time.monotonic() - started)
            line = json.dumps(data, ensure_ascii=False)
            with lock:
                log.write(line + '\n')
                log.flush()
            print(line, flush=True)

        def send(sig):
            if child is not None:
                try:
                    os.killpg(child.pid, sig)
                except ProcessLookupError:
                    pass

        def kill():
            forced.set()
            send(signal.SIGKILL)

        def stop(signum=signal.SIGTERM, _frame=None):
            nonlocal kill_timer
            stopping.set()
            send(signal.SIGTERM)
            if kill_timer is None:
                kill_timer = threading.Timer(5, kill)
                kill_timer.daemon = True
                kill_timer.start()

        def deadline():
            expired.set()
            stop()

        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        timer = threading.Timer(args.deadline, deadline)
        timer.daemon = True
        try:
            with args.pid_file.open('x') as pid:
                claimed_pid = True
                pid.write(str(os.getpid()) + '\n')
            child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, errors='replace', start_new_session=True)
            emit(event='spawn', child_pid=child.pid, scope='native observation, not automatic acceptance')
            timer.start()
            if stopping.is_set():
                send(signal.SIGTERM)
            for line in child.stdout:
                emit(event='line', line=line.rstrip('\n'))
            code = child.wait()
            emit(event='exit', exit_code=code, deadline=expired.is_set(), forced=forced.is_set())
            return 124 if expired.is_set() or forced.is_set() else (128 - code if code < 0 else code)
        except BaseException:
            send(signal.SIGKILL)
            if child is not None:
                child.wait()
            raise
        finally:
            timer.cancel()
            if kill_timer is not None:
                kill_timer.cancel()
            # Reap any descendants retaining the owned group's stdout or resources.
            send(signal.SIGKILL)
            if claimed_pid:
                args.pid_file.unlink(missing_ok=True)


if __name__ == '__main__':
    raise SystemExit(main())
