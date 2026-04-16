#!/usr/bin/env python3
"""
Sample TCP client for ScummVM Monkey MCP.
Connects to server, sends initialize and tools/list, prints responses.

Usage: python3 client_tcp.py --host 127.0.0.1 --port 12345
"""
import socket
import argparse
import sys
import json


def recv_line(sock):
    buf = b''
    while True:
        c = sock.recv(1)
        if not c:
            return None
        if c == b'\n':
            break
        buf += c
    return buf.decode('utf-8')


def send_request(sock, req):
    s = json.dumps(req)
    sock.sendall((s + '\n').encode('utf-8'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=12345)
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((args.host, args.port))
    print('Connected to %s:%d' % (args.host, args.port))

    # initialize
    req = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}
    send_request(sock, req)
    line = recv_line(sock)
    print('initialize ->', line)

    # list tools
    req = {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}
    send_request(sock, req)
    line = recv_line(sock)
    print('tools/list ->', line)

    # close
    sock.close()

if __name__ == '__main__':
    main()
