#!/usr/bin/env python3
"""
Sample HTTP MCP client for ScummVM.
Connects to the built-in Streamable HTTP MCP server, sends initialize and
tools/list, then prints the responses.

Usage: python3 client_tcp.py [--host 127.0.0.1] [--port 23456]
"""
import socket
import argparse
import json
import sys


def http_post(host, port, path, body_obj):
    body = json.dumps(body_obj).encode('utf-8')
    request = (
        'POST %s HTTP/1.1\r\n'
        'Host: %s:%d\r\n'
        'Content-Type: application/json\r\n'
        'Content-Length: %d\r\n'
        'Connection: close\r\n'
        '\r\n' % (path, host, port, len(body))
    ).encode('utf-8') + body

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    sock.sendall(request)

    response = b''
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        response += chunk
    sock.close()

    # Split off headers
    headers, _, body_bytes = response.partition(b'\r\n\r\n')
    return json.loads(body_bytes.decode('utf-8'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=23456)
    args = parser.parse_args()

    print('Connecting to %s:%d' % (args.host, args.port))

    # initialize
    resp = http_post(args.host, args.port, '/mcp', {
        'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
        'params': {'protocolVersion': '2025-03-26', 'clientInfo': {'name': 'client_tcp.py', 'version': '1.0'}}
    })
    print('initialize ->', json.dumps(resp, indent=2))

    # tools/list
    resp = http_post(args.host, args.port, '/mcp', {
        'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list', 'params': {}
    })
    print('tools/list ->', json.dumps(resp, indent=2))

    # state
    resp = http_post(args.host, args.port, '/mcp', {
        'jsonrpc': '2.0', 'id': 3, 'method': 'tools/call',
        'params': {'name': 'state', 'arguments': {}}
    })
    print('state ->', json.dumps(resp, indent=2))


if __name__ == '__main__':
    main()
