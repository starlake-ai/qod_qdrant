#!/usr/bin/env python3
"""A TCP listener that accepts connections and never answers, for the connector's tests only: a TLS client
connecting to it waits for a ServerHello that never comes.

usage: silent_stub.py --port-file F
"""
import argparse
import os
import socket


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port-file", required=True)
    args = parser.parse_args()
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind(("127.0.0.1", 0))
    server.listen(16)
    with open(args.port_file + ".tmp", "w") as f:
        f.write(str(server.getsockname()[1]))
    os.replace(args.port_file + ".tmp", args.port_file)
    held = []
    while True:
        conn, _ = server.accept()
        held.append(conn)


if __name__ == "__main__":
    main()
