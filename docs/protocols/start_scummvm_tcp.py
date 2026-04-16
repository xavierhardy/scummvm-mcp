#!/usr/bin/env python3
"""
Simple TCP -> process stdio proxy for ScummVM MCP bridge.
Usage: start_scummvm_tcp.py [--host HOST] [--port PORT] --scummvm /path/to/scummvm --ini /path/to/scummvm.ini

This script listens on HOST:PORT, accepts a single client connection, then
spawns scummvm with stdin/stdout connected to the TCP socket. It proxies
data in both directions until the process exits or the client disconnects.

Note: This is an external helper. Do NOT compile or link this into ScummVM.
"""

import argparse
import socket
import subprocess
import threading
import sys


def forward(src, dst):
    try:
        while True:
            data = src.recv(4096)
            if not data:
                break
            dst.write(data)
            dst.flush()
    except Exception:
        pass


def forward_stdout(proc, conn):
    try:
        while True:
            data = proc.stdout.read(4096)
            if not data:
                break
            conn.sendall(data)
    except Exception:
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=12345)
    parser.add_argument('--scummvm', required=True, help='Path to scummvm binary')
    parser.add_argument('--ini', required=True, help='Path to scummvm.ini')
    parser.add_argument('--debugflags', default='monkey_mcp,scumm')
    parser.add_argument('--debuglevel', default='11')
    args = parser.parse_args()

    listen = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listen.bind((args.host, args.port))
    listen.listen(1)
    print('Listening for MCP client on %s:%d' % (args.host, args.port), file=sys.stderr)

    conn, addr = listen.accept()
    print('Accepted connection from %s' % (addr,), file=sys.stderr)

    # Start scummvm process with pipes
    cmd = [args.scummvm, '-c', args.ini, '--debugflags', args.debugflags, '--debuglevel', args.debuglevel]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    # Start threads to proxy
    t1 = threading.Thread(target=forward, args=(conn, proc.stdin))
    t2 = threading.Thread(target=forward_stdout, args=(proc, conn))
    t1.daemon = True
    t2.daemon = True
    t1.start()
    t2.start()

    try:
        proc.wait()
    except KeyboardInterrupt:
        proc.terminate()
    finally:
        try:
            conn.shutdown(socket.SHUT_RDWR)
            conn.close()
        except Exception:
            pass

    print('ScummVM exited, proxy shutting down', file=sys.stderr)

if __name__ == '__main__':
    main()
