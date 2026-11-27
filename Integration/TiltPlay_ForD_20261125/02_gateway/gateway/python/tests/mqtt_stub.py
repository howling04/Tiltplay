"""最小 MQTT 3.1.1 broker，仅用于本机自测与无 broker 环境下的演示。

支持: CONNECT / PUBLISH(QoS0/1) / SUBSCRIBE / UNSUBSCRIBE / PINGREQ / DISCONNECT。
不支持: 保留消息、遗嘱、TLS、持久会话——这些由真实 Mosquitto 承担。
"""
from __future__ import annotations

import socket
import struct
import threading


def _decode_remaining_length(read_byte) -> tuple[int, int]:
    multiplier = 1
    value = 0
    count = 0
    while True:
        byte = read_byte()
        if byte is None:
            raise ConnectionError("eof")
        value += (byte & 0x7F) * multiplier
        count += 1
        if not byte & 0x80:
            return value, count
        multiplier *= 128
        if count > 4:
            raise ValueError("bad remaining length")


def _encode_remaining_length(length: int) -> bytes:
    out = bytearray()
    while True:
        byte = length % 128
        length //= 128
        if length:
            byte |= 0x80
        out.append(byte)
        if not length:
            return bytes(out)


def topic_matches(pattern: str, topic: str) -> bool:
    p = pattern.split("/")
    t = topic.split("/")
    for i, part in enumerate(p):
        if part == "#":
            return True
        if i >= len(t):
            return False
        if part != "+" and part != t[i]:
            return False
    return len(p) == len(t)


class _Client:
    def __init__(self, sock: socket.socket, addr):
        self.sock = sock
        self.addr = addr
        self.subs: list[str] = []
        self.write_lock = threading.Lock()
        self.client_id = "?"

    def send(self, data: bytes) -> None:
        with self.write_lock:
            try:
                self.sock.sendall(data)
            except OSError:
                pass

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


class MqttStubBroker:
    def __init__(self, host: str = "127.0.0.1", port: int = 0):
        self.host = host
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind((host, port))
        self.port = self.server.getsockname()[1]
        self.server.listen(16)
        self.clients: list[_Client] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []
        self.published = 0

    # -- 生命周期 -----------------------------------------------------------
    def start(self) -> "MqttStubBroker":
        thread = threading.Thread(target=self._accept_loop, daemon=True)
        thread.start()
        self._threads.append(thread)
        return self

    def stop(self) -> None:
        self._stop.set()
        try:
            self.server.close()
        except OSError:
            pass
        with self._lock:
            for client in self.clients:
                client.close()
            self.clients.clear()

    # -- 连接 ---------------------------------------------------------------
    def _accept_loop(self) -> None:
        while not self._stop.is_set():
            try:
                sock, addr = self.server.accept()
            except OSError:
                return
            client = _Client(sock, addr)
            with self._lock:
                self.clients.append(client)
            thread = threading.Thread(target=self._serve, args=(client,), daemon=True)
            thread.start()

    def _read_exact(self, sock: socket.socket, n: int) -> bytes | None:
        buf = b""
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def _serve(self, client: _Client) -> None:
        sock = client.sock
        try:
            while not self._stop.is_set():
                header = self._read_exact(sock, 1)
                if header is None:
                    return
                first = header[0]

                def read_byte():
                    data = self._read_exact(sock, 1)
                    return None if data is None else data[0]

                remaining, _ = _decode_remaining_length(read_byte)
                body = self._read_exact(sock, remaining) if remaining else b""
                if body is None:
                    return
                self._handle(client, first, body)
        except (ConnectionError, OSError, ValueError):
            return
        finally:
            with self._lock:
                if client in self.clients:
                    self.clients.remove(client)
            client.close()

    # -- 报文 ---------------------------------------------------------------
    def _handle(self, client: _Client, first: int, body: bytes) -> None:
        packet_type = first >> 4
        if packet_type == 1:  # CONNECT
            client.send(b"\x20\x02\x00\x00")
        elif packet_type == 3:  # PUBLISH
            qos = (first >> 1) & 0x03
            topic_len = struct.unpack("!H", body[:2])[0]
            topic = body[2:2 + topic_len].decode("utf-8")
            offset = 2 + topic_len
            packet_id = None
            if qos > 0:
                packet_id = struct.unpack("!H", body[offset:offset + 2])[0]
                offset += 2
            payload = body[offset:]
            self.published += 1
            if qos == 1 and packet_id is not None:
                client.send(b"\x40\x02" + struct.pack("!H", packet_id))
            self._route(topic, payload)
        elif packet_type == 8:  # SUBSCRIBE
            packet_id = struct.unpack("!H", body[:2])[0]
            offset = 2
            granted = []
            while offset < len(body):
                topic_len = struct.unpack("!H", body[offset:offset + 2])[0]
                topic = body[offset + 2:offset + 2 + topic_len].decode("utf-8")
                offset += 2 + topic_len + 1
                client.subs.append(topic)
                granted.append(0)
            client.send(b"\x90" + _encode_remaining_length(2 + len(granted)) +
                        struct.pack("!H", packet_id) + bytes(granted))
        elif packet_type == 10:  # UNSUBSCRIBE
            packet_id = struct.unpack("!H", body[:2])[0]
            offset = 2
            while offset < len(body):
                topic_len = struct.unpack("!H", body[offset:offset + 2])[0]
                topic = body[offset + 2:offset + 2 + topic_len].decode("utf-8")
                offset += 2 + topic_len
                if topic in client.subs:
                    client.subs.remove(topic)
            client.send(b"\xb0\x02" + struct.pack("!H", packet_id))
        elif packet_type == 12:  # PINGREQ
            client.send(b"\xd0\x00")
        elif packet_type == 14:  # DISCONNECT
            client.close()

    def _route(self, topic: str, payload: bytes) -> None:
        topic_bytes = topic.encode("utf-8")
        packet = b"\x30" + _encode_remaining_length(2 + len(topic_bytes) + len(payload)) + \
            struct.pack("!H", len(topic_bytes)) + topic_bytes + payload
        with self._lock:
            targets = [c for c in self.clients if any(topic_matches(p, topic) for p in c.subs)]
        for client in targets:
            client.send(packet)
