#!/usr/bin/env python3
"""Run a command on the phone through cmdd (phone/tools-surya/cmdd.c) and the USB file server.

  surya-run.py [-u UID] [-t SECS] [-e KEY=VALUE]... PROGRAM [ARG...]

PROGRAM must be an absolute path (no shell, no PATH search). Prints the command's output and exits with
its status. The command file goes to /var/local/cmd/NAME.cmd; cmdd writes NAME.out and NAME.rc.
"""
import os, subprocess, sys, tempfile, time

FILES = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'surya-files.py')]


def files(*args, check=True):
    r = subprocess.run(FILES + list(args), capture_output=True, text=True)
    if check and r.returncode:
        raise SystemExit(r.stderr.strip() or r.stdout.strip())
    return r.returncode == 0


def main(argv):
    lines, timeout = [], 300
    while argv and argv[0].startswith('-'):
        opt = argv.pop(0)
        if opt == '-u':
            lines.append('UID ' + argv.pop(0))
        elif opt == '-t':
            timeout = int(argv.pop(0))
            lines.append(f'TIMEOUT {timeout}')
        elif opt == '-e':
            lines.append('ENV ' + argv.pop(0))
        else:
            raise SystemExit(__doc__)
    if not argv or not argv[0].startswith('/'):
        raise SystemExit(__doc__)
    lines += argv
    name = f'c{int(time.time() * 1000)}'
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, 'cmd')
        open(src, 'w').write('\n'.join(lines) + '\n')
        # written under another name first, then linked, so cmdd never sees half a file
        files('put', src, f'/var/local/cmd/{name}.tmp', '600')
        # the file server's LINK makes a symlink: cmdd reads through it, so the .tmp stays until the answer
        files('link', f'/var/local/cmd/{name}.tmp', f'/var/local/cmd/{name}.cmd')
        rc_local = os.path.join(d, 'rc')
        deadline = time.time() + timeout + 30
        while time.time() < deadline:
            if files('get', f'/var/local/cmd/{name}.rc', rc_local, check=False):
                break
            time.sleep(1)
        else:
            raise SystemExit('no answer from cmdd (is it running?)')
        out_local = os.path.join(d, 'out')
        if files('get', f'/var/local/cmd/{name}.out', out_local, check=False):
            sys.stdout.write(open(out_local, errors='replace').read())
        for ext in ('rc', 'out', 'tmp'):
            files('rm', f'/var/local/cmd/{name}.{ext}', check=False)
        return int(open(rc_local).read().strip() or 1)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
