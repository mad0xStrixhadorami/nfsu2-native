#!/usr/bin/env python3
"""Progress window for long NFSU2 port jobs.

Follows a ninja build log ("[n/m] ..." lines) and shows a progress bar, the current step,
elapsed time and an estimate. Switches to "Linking..." once only the link step remains,
and shows the final result when the watched process exits.

Usage: progress_window.py LOG [--title T] [--pid PID]
  LOG    build log to follow (ninja output)
  --pid  process to watch for completion (default: wait for a "build_exit=" line)
"""
import argparse
import os
import re
import time
import tkinter as tk
from tkinter import ttk

STEP = re.compile(r'\[(\d+)/(\d+)\]\s*(.*)')


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


class Window:
    def __init__(self, log, title, pid):
        self.log, self.pid = log, pid
        self.start = time.time()
        self.first = None  # (time, step) of the first step we saw, for the estimate
        self.done = False
        self.root = tk.Tk()
        self.root.title(title)
        self.root.geometry('560x170')
        self.root.resizable(False, False)
        self.head = ttk.Label(self.root, text=title, font=('TkDefaultFont', 12, 'bold'))
        self.head.pack(pady=(14, 6))
        self.bar = ttk.Progressbar(self.root, length=520, maximum=100)
        self.bar.pack(padx=20)
        self.status = ttk.Label(self.root, text='Waiting for build output...')
        self.status.pack(pady=(8, 2))
        self.detail = ttk.Label(self.root, text='', foreground='gray')
        self.detail.pack()
        self.close = ttk.Button(self.root, text='Close', command=self.root.destroy)
        self.close.pack(pady=8)
        self.tick()

    def read_state(self):
        try:
            with open(self.log, 'rb') as f:
                f.seek(max(0, os.path.getsize(self.log) - 65536))
                tail = f.read().decode(errors='replace')
        except OSError:
            return None, None, ''
        n = m = None
        what = ''
        for line in tail.splitlines():
            s = STEP.search(line)
            if s:
                n, m, what = int(s.group(1)), int(s.group(2)), s.group(3)
        exit_line = re.findall(r'build_exit=(\d+)', tail)
        errors = len(re.findall(r'error:', tail))
        return (n, m, what), (exit_line[-1] if exit_line else None), errors

    def tick(self):
        state, exit_code, errors = self.read_state()
        elapsed = time.time() - self.start
        if state and state[0] is not None:
            n, m, what = state
            if self.first is None:
                self.first = (time.time(), n)
            self.bar['value'] = 100.0 * n / m
            short = re.sub(r'.*/', '', what)[:60]
            if n >= m - 1 and 'Linking' not in what:
                self.bar.config(mode='indeterminate')
                self.bar.start(15)
                self.status.config(text=f'Linking...  step {n}/{m}')
            else:
                self.status.config(text=f'Step {n} of {m} ({100 * n // m}%)  {short}')
            eta = ''
            t0, n0 = self.first
            if n > n0 and n < m - 1:
                rate = (time.time() - t0) / (n - n0)
                eta = f'  ·  about {int(rate * (m - 1 - n) / 60) + 1} min left (before linking)'
            self.detail.config(text=f'Elapsed {int(elapsed // 60)}m{int(elapsed % 60):02d}s{eta}' + (f'  ·  {errors} errors' if errors else ''))
        finished = exit_code is not None or (self.pid and not pid_alive(self.pid))
        if finished and not self.done:
            self.done = True
            self.bar.stop()
            self.bar.config(mode='determinate')
            ok = exit_code == '0' or (exit_code is None and errors == 0)
            self.bar['value'] = 100 if ok else self.bar['value']
            self.head.config(text='Build finished' if ok else 'Build FAILED', foreground='green' if ok else 'red')
            self.status.config(text='Done. Next: first test run.' if ok else 'See the build log for errors.')
            self.root.bell()
        self.root.after(1000, self.tick)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--title', default='NFSU2 native build')
    ap.add_argument('--pid', type=int, default=0)
    a = ap.parse_args()
    Window(a.log, a.title, a.pid).root.mainloop()


if __name__ == '__main__':
    main()
