"""Debug-console commands to a running conker.exe (--debug) through its
command pipe (to-do 174, SPEC 10.3); the replies printed.

    .venv\\Scripts\\python scripts\\conker_cmd.py [--pipe NAME] <command> [<command> ...]

Each argument is one command line, run in order; each waits for its reply
(wait, waitload, exec and shot answer when done). Exit code 0 = all "ok",
1 = some printed an error, 2 = no game / pipe. Also importable:
run(cmd, pipe=...) -> (ok, [lines]).

Script runs give the game a pipe name of their own (RECOMP_CMD_PIPE) so
commands never reach the user's game; the default is conker-console.
"""
import argparse
import sys
import time

DEFAULT_PIPE = 'conker-console'


class Pipe:
    def __init__(self, name=DEFAULT_PIPE, connect_s=3.0):
        path = r'\\.\pipe\%s' % name
        deadline = time.time() + connect_s
        while True:
            try:
                self.f = open(path, 'r+b', buffering=0)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        self.buf = b''

    def _line(self):
        while b'\n' not in self.buf:
            chunk = self.f.read(65536)
            if not chunk:
                raise EOFError('the game closed the pipe')
            self.buf += chunk
        line, self.buf = self.buf.split(b'\n', 1)
        return line.decode('utf-8', 'replace')

    def run(self, cmd):
        """(ok, lines) for one command line."""
        self.f.write(cmd.encode('utf-8') + b'\n')
        lines = []
        while True:
            line = self._line()
            if line == '= ok':
                return True, lines
            if line == '= error':
                return False, lines
            lines.append(line[1:] if line.startswith(' =') else line)

    def close(self):
        self.f.close()


def run(cmd, pipe=DEFAULT_PIPE):
    p = Pipe(pipe)
    try:
        return p.run(cmd)
    finally:
        p.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--pipe', default=DEFAULT_PIPE)
    ap.add_argument('commands', nargs='+')
    a = ap.parse_args()
    try:
        p = Pipe(a.pipe)
    except OSError:
        print(r'No game on \\.\pipe\%s (is conker.exe running with --debug?)' % a.pipe)
        return 2
    code = 0
    try:
        for c in a.commands:
            ok, lines = p.run(c)
            for line in lines:
                print(line)
            if not ok:
                code = 1
    except EOFError as e:
        print(e)
        return 2
    finally:
        p.close()
    return code


if __name__ == '__main__':
    sys.exit(main())
