#!/usr/bin/env python3
"""Small TCP/UDP fault proxy for WiVRn connection-lifecycle testing.

The proxy listens on the same port for TCP and UDP and forwards both to a
server. Commands are read from stdin while it runs:

  drop tcp|udp|both on|off
  delay tcp|udp|both MILLISECONDS
  close tcp
  status
  quit

Use --upstream-bind with two addresses on consecutive runs to exercise resume
from a different source IP. --close-tcp-after closes during a handshake after
the requested number of client-to-server bytes.
"""

from __future__ import annotations

import argparse
import dataclasses
import select
import socket
import sys
import threading
import time
from typing import Optional


@dataclasses.dataclass
class FaultState:
    drop_tcp: bool = False
    drop_udp: bool = False
    tcp_delay_ms: int = 0
    udp_delay_ms: int = 0
    close_tcp_after: int = 0
    tcp_bytes_from_client: int = 0
    tcp_connections: int = 0
    udp_from_client: int = 0
    udp_from_server: int = 0


class FaultProxy:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.state = FaultState(close_tcp_after=args.close_tcp_after)
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.tcp_pair: Optional[tuple[socket.socket, socket.socket]] = None

    def snapshot(self) -> FaultState:
        with self.lock:
            return dataclasses.replace(self.state)

    def close_tcp(self) -> None:
        with self.lock:
            pair = self.tcp_pair
            self.tcp_pair = None
        if pair:
            for sock in pair:
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                sock.close()

    def relay(self, source: socket.socket, destination: socket.socket, from_client: bool) -> None:
        try:
            while not self.stop.is_set():
                data = source.recv(64 * 1024)
                if not data:
                    break
                with self.lock:
                    state = dataclasses.replace(self.state)
                    if from_client:
                        self.state.tcp_bytes_from_client += len(data)
                        byte_count = self.state.tcp_bytes_from_client
                    else:
                        byte_count = 0
                if state.close_tcp_after and from_client and byte_count >= state.close_tcp_after:
                    self.close_tcp()
                    return
                if state.drop_tcp:
                    continue
                if state.tcp_delay_ms:
                    time.sleep(state.tcp_delay_ms / 1000)
                destination.sendall(data)
        except OSError:
            pass
        finally:
            self.close_tcp()

    def tcp_loop(self) -> None:
        family = socket.AF_INET6 if ":" in self.args.listen_address else socket.AF_INET
        with socket.socket(family, socket.SOCK_STREAM) as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((self.args.listen_address, self.args.listen_port))
            listener.listen()
            listener.settimeout(0.25)
            while not self.stop.is_set():
                try:
                    client, _ = listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                try:
                    source = (self.args.upstream_bind, 0) if self.args.upstream_bind else None
                    upstream = socket.create_connection(
                        (self.args.server_host, self.args.server_port), timeout=5, source_address=source
                    )
                except OSError as error:
                    print(f"upstream TCP connection failed: {error}", file=sys.stderr, flush=True)
                    client.close()
                    continue
                with self.lock:
                    previous = self.tcp_pair
                    self.tcp_pair = (client, upstream)
                    self.state.tcp_connections += 1
                    self.state.tcp_bytes_from_client = 0
                if previous:
                    for sock in previous:
                        sock.close()
                threading.Thread(target=self.relay, args=(client, upstream, True), daemon=True).start()
                threading.Thread(target=self.relay, args=(upstream, client, False), daemon=True).start()

    def udp_loop(self) -> None:
        listen_family = socket.AF_INET6 if ":" in self.args.listen_address else socket.AF_INET
        upstream_family = socket.AF_INET6 if ":" in self.args.server_host else socket.AF_INET
        with socket.socket(listen_family, socket.SOCK_DGRAM) as client_socket, \
                socket.socket(upstream_family, socket.SOCK_DGRAM) as upstream_socket:
            client_socket.bind((self.args.listen_address, self.args.listen_port))
            if self.args.upstream_bind:
                upstream_socket.bind((self.args.upstream_bind, 0))
            upstream_socket.connect((self.args.server_host, self.args.server_port))
            last_client = None
            while not self.stop.is_set():
                readable, _, _ = select.select([client_socket, upstream_socket], [], [], 0.25)
                for current in readable:
                    try:
                        if current is client_socket:
                            data, last_client = client_socket.recvfrom(65535)
                            with self.lock:
                                self.state.udp_from_client += 1
                                state = dataclasses.replace(self.state)
                            destination = upstream_socket
                            address = None
                        else:
                            data = upstream_socket.recv(65535)
                            with self.lock:
                                self.state.udp_from_server += 1
                                state = dataclasses.replace(self.state)
                            destination = client_socket
                            address = last_client
                        if state.drop_udp or (address is None and current is upstream_socket):
                            continue
                        if state.udp_delay_ms:
                            time.sleep(state.udp_delay_ms / 1000)
                        if address is None:
                            destination.send(data)
                        else:
                            destination.sendto(data, address)
                    except OSError:
                        if not self.stop.is_set():
                            raise

    def command_loop(self) -> None:
        for line in sys.stdin:
            words = line.strip().lower().split()
            if not words:
                continue
            try:
                if words == ["quit"]:
                    self.stop.set()
                    self.close_tcp()
                    return
                if words == ["close", "tcp"]:
                    self.close_tcp()
                    continue
                if words == ["status"]:
                    print(self.snapshot(), flush=True)
                    continue
                if len(words) == 3 and words[0] == "drop" and words[1] in {"tcp", "udp", "both"}:
                    enabled = {"on": True, "off": False}[words[2]]
                    with self.lock:
                        if words[1] in {"tcp", "both"}:
                            self.state.drop_tcp = enabled
                        if words[1] in {"udp", "both"}:
                            self.state.drop_udp = enabled
                    continue
                if len(words) == 3 and words[0] == "delay" and words[1] in {"tcp", "udp", "both"}:
                    delay = max(0, int(words[2]))
                    with self.lock:
                        if words[1] in {"tcp", "both"}:
                            self.state.tcp_delay_ms = delay
                        if words[1] in {"udp", "both"}:
                            self.state.udp_delay_ms = delay
                    continue
            except (KeyError, ValueError):
                pass
            print("invalid command", file=sys.stderr, flush=True)

    def run(self) -> None:
        threads = [
            threading.Thread(target=self.tcp_loop, name="fault-proxy-tcp", daemon=True),
            threading.Thread(target=self.udp_loop, name="fault-proxy-udp", daemon=True),
            threading.Thread(target=self.command_loop, name="fault-proxy-control", daemon=True),
        ]
        for thread in threads:
            thread.start()
        print(
            f"forwarding TCP/UDP {self.args.listen_address}:{self.args.listen_port} "
            f"to {self.args.server_host}:{self.args.server_port}",
            flush=True,
        )
        try:
            while not self.stop.wait(0.25):
                pass
        except KeyboardInterrupt:
            self.stop.set()
        self.close_tcp()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server_host")
    parser.add_argument("--server-port", type=int, default=9757)
    parser.add_argument("--listen-address", default="0.0.0.0")
    parser.add_argument("--listen-port", type=int, default=9758)
    parser.add_argument("--upstream-bind", help="source address used toward the server")
    parser.add_argument("--close-tcp-after", type=int, default=0, metavar="BYTES")
    return parser.parse_args()


if __name__ == "__main__":
    FaultProxy(parse_args()).run()
