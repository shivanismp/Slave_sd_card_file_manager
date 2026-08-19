#!/usr/bin/env python3
"""Reliable 2 MB MQTT transfer using a machine/server handshake.

Run this file in two terminals:
    python mqtt_test.py server
    python mqtt_test.py machine

AWS IoT Core accepts MQTT PUBLISH payloads up to 128 KB, so the 2 MB payload
is split into 120 KB binary chunks. QoS 1 can redeliver messages; the server
therefore stores chunks by index and safely ignores duplicates.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import logging
import os
import ssl
import struct
import threading
import uuid
from pathlib import Path

import paho.mqtt.client as mqtt

#AWS IoT Core connection details.
ENDPOINT = "a3958f6g61vd15-ats.iot.ap-south-1.amazonaws.com"
PORT = 8883
CA_CERT = "littlefs/mqtt/certs/AmazonRootCA1.pem"
CLIENT_CERT = "littlefs/mqtt/certs/device.crt"
PRIVATE_KEY = "littlefs/mqtt/certs/private.key"

# MQTT Topics for handshake and data transfer.
STATUS_TOPIC = "machine/status"
ACK_TOPIC = "server/ack"
DATA_TOPIC = "machine/data"
DATA_ACK_TOPIC = "server/data-ack"

QOS = 1  #AWS IoT Core connection details.
PAYLOAD_SIZE = 2 * 1024 * 1024 #Payload split into chunks.
CHUNK_SIZE = 120 * 1024

# Chunk packet: 4-byte marker, 16-byte transfer UUID, chunk index, chunk count.
CHUNK_HEADER = struct.Struct("!4s16sII")
CHUNK_MARKER = b"MDAT" #identifies valid data chunks.

LOG = logging.getLogger("mqtt-transfer")


def json_bytes(value: dict) -> bytes:
    return json.dumps(value, separators=(",", ":")).encode("utf-8")


def parse_json(payload: bytes) -> dict:
    return json.loads(payload.decode("utf-8"))


def wait_for_publish(info: mqtt.MQTTMessageInfo, timeout: float = 30.0) -> None:
    """Wait until the QoS acknowledgement is received from the broker."""
    info.wait_for_publish(timeout=timeout)
    if not info.is_published():
        raise TimeoutError(f"MQTT publish was not acknowledged (mid={info.mid})")


def make_client(client_id: str) -> mqtt.Client:
    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=client_id,
        protocol=mqtt.MQTTv311,
        clean_session=True,
    )
    client.tls_set(
        ca_certs=CA_CERT,
        certfile=CLIENT_CERT,
        keyfile=PRIVATE_KEY,
        tls_version=ssl.PROTOCOL_TLSv1_2,
    )
    client.reconnect_delay_set(min_delay=1, max_delay=30)
    return client


class Machine:
    def __init__(self, source_file: str | None) -> None:
        self.client = make_client(f"python-machine-{uuid.uuid4().hex[:8]}")
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self.client.on_message = self.on_message

        if source_file:
            self.payload = Path(source_file).read_bytes()
        else:
            self.payload = os.urandom(PAYLOAD_SIZE)

        self.transfer_id = uuid.uuid4()
        self.digest = hashlib.sha256(self.payload).hexdigest()
        self.chunk_count = (len(self.payload) + CHUNK_SIZE - 1) // CHUNK_SIZE
        self.transfer_started = False
        self.completed = threading.Event()

    def on_connect(self, client, userdata, flags, reason_code, properties) -> None:
        if reason_code != 0:
            LOG.error("Machine connection failed: %s", reason_code)
            return

        LOG.info("Machine connected to %s:%d", ENDPOINT, PORT)
        # Subscribe before announcing readiness, so a fast ACK cannot be missed.
        result, _ = client.subscribe(
            [(ACK_TOPIC, QOS), (DATA_ACK_TOPIC, QOS)]
        )
        if result != mqtt.MQTT_ERR_SUCCESS:
            LOG.error("Machine subscription failed: %s", mqtt.error_string(result))
            return

        status = {
            "message": "ready to transfer",
            "transfer_id": str(self.transfer_id),
            "size": len(self.payload),
            "chunk_size": CHUNK_SIZE,
            "chunk_count": self.chunk_count,
            "sha256": self.digest,
        }
        info = client.publish(STATUS_TOPIC, json_bytes(status), qos=QOS)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            LOG.error("Ready status publish failed: %s", mqtt.error_string(info.rc))
            return
        LOG.info(
            "Published ready status: transfer=%s, size=%d bytes, chunks=%d",
            self.transfer_id,
            len(self.payload),
            self.chunk_count,
        )

    def on_disconnect(
        self, client, userdata, disconnect_flags, reason_code, properties
    ) -> None:
        if reason_code != 0:
            LOG.warning("Machine disconnected unexpectedly: %s", reason_code)

    def on_message(self, client, userdata, message) -> None:
        try:
            body = parse_json(message.payload)
        except (UnicodeDecodeError, json.JSONDecodeError):
            LOG.warning("Machine received invalid JSON on %s", message.topic)
            return

        if body.get("transfer_id") != str(self.transfer_id):
            return

        if (
            message.topic == ACK_TOPIC
            and body.get("message") == "ready to receive data"
            and not self.transfer_started
        ):
            self.transfer_started = True
            LOG.info("Received server acknowledgement; starting data transfer")
            # Do not block Paho's network callback thread while publishing.
            threading.Thread(target=self.publish_chunks, daemon=True).start()
        elif message.topic == DATA_ACK_TOPIC:
            if body.get("status") == "complete":
                LOG.info(
                    "Transfer complete and SHA-256 verified by server: %s",
                    body.get("sha256"),
                )
                self.completed.set()
            elif body.get("status") == "error":
                LOG.error("Server rejected transfer: %s", body.get("error"))
                self.completed.set()

    def publish_chunks(self) -> None:
        try:
            for index in range(self.chunk_count):
                start = index * CHUNK_SIZE
                chunk = self.payload[start : start + CHUNK_SIZE]
                packet = CHUNK_HEADER.pack(
                    CHUNK_MARKER,
                    self.transfer_id.bytes,
                    index,
                    self.chunk_count,
                ) + chunk
                wait_for_publish(
                    self.client.publish(DATA_TOPIC, packet, qos=QOS),
                    timeout=60,
                )
                LOG.info(
                    "Published chunk %d/%d (%d bytes)",
                    index + 1,
                    self.chunk_count,
                    len(chunk),
                )
        except Exception:
            LOG.exception("Data publishing failed")
            self.completed.set()

    def run(self, timeout: float) -> None:
        self.client.connect(ENDPOINT, PORT, keepalive=60)
        self.client.loop_start()
        try:
            if not self.completed.wait(timeout):
                raise TimeoutError(f"Transfer did not finish within {timeout:g}s")
        finally:
            self.client.disconnect()
            self.client.loop_stop()


class Server:
    def __init__(self, output_dir: str) -> None:
        self.client = make_client(f"python-server-{uuid.uuid4().hex[:8]}")
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self.client.on_message = self.on_message
        self.output_dir = Path(output_dir)
        self.transfers: dict[str, dict] = {}
        self.lock = threading.Lock()

    def on_connect(self, client, userdata, flags, reason_code, properties) -> None:
        if reason_code != 0:
            LOG.error("Server connection failed: %s", reason_code)
            return
        LOG.info("Server connected to %s:%d", ENDPOINT, PORT)
        result, _ = client.subscribe([(STATUS_TOPIC, QOS), (DATA_TOPIC, QOS)])
        if result == mqtt.MQTT_ERR_SUCCESS:
            LOG.info("Listening on %s and %s", STATUS_TOPIC, DATA_TOPIC)
        else:
            LOG.error("Server subscription failed: %s", mqtt.error_string(result))

    def on_disconnect(
        self, client, userdata, disconnect_flags, reason_code, properties
    ) -> None:
        if reason_code != 0:
            LOG.warning("Server disconnected unexpectedly: %s", reason_code)

    def on_message(self, client, userdata, message) -> None:
        try:
            if message.topic == STATUS_TOPIC:
                self.handle_status(message.payload)
            elif message.topic == DATA_TOPIC:
                self.handle_chunk(message.payload)
        except Exception:
            LOG.exception("Failed to handle message on %s", message.topic)

    def handle_status(self, payload: bytes) -> None:
        body = parse_json(payload)
        if body.get("message") != "ready to transfer":
            return

        transfer_id = str(uuid.UUID(body["transfer_id"]))
        size = int(body["size"])
        chunk_count = int(body["chunk_count"])
        if size <= 0 or chunk_count <= 0:
            raise ValueError("Invalid transfer metadata")

        with self.lock:
            self.transfers[transfer_id] = {
                "size": size,
                "chunk_count": chunk_count,
                "sha256": body["sha256"],
                "chunks": {},
            }

        ack = {
            "message": "ready to receive data",
            "transfer_id": transfer_id,
        }
        info = self.client.publish(ACK_TOPIC, json_bytes(ack), qos=QOS)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError(f"ACK publish failed: {mqtt.error_string(info.rc)}")
        LOG.info(
            "Ready status received; acknowledgement sent for transfer %s",
            transfer_id,
        )

    def handle_chunk(self, packet: bytes) -> None:
        if len(packet) < CHUNK_HEADER.size:
            raise ValueError("Chunk is shorter than its header")
        marker, transfer_bytes, index, packet_chunk_count = CHUNK_HEADER.unpack_from(
            packet
        )
        if marker != CHUNK_MARKER:
            raise ValueError("Invalid chunk marker")

        transfer_id = str(uuid.UUID(bytes=transfer_bytes))
        chunk = packet[CHUNK_HEADER.size :]
        with self.lock:
            state = self.transfers.get(transfer_id)
            if state is None:
                LOG.warning("Ignoring chunk for unknown transfer %s", transfer_id)
                return
            if packet_chunk_count != state["chunk_count"]:
                raise ValueError("Chunk count does not match transfer metadata")
            if index >= state["chunk_count"]:
                raise ValueError("Chunk index is out of range")

            state["chunks"].setdefault(index, chunk)
            received = len(state["chunks"])
            LOG.info(
                "Received chunk %d/%d (%d unique chunks received)",
                index + 1,
                state["chunk_count"],
                received,
            )
            if received != state["chunk_count"]:
                return

            assembled = b"".join(
                state["chunks"][i] for i in range(state["chunk_count"])
            )
            expected_size = state["size"]
            expected_digest = state["sha256"]
            del self.transfers[transfer_id]

        actual_digest = hashlib.sha256(assembled).hexdigest()
        if len(assembled) != expected_size or actual_digest != expected_digest:
            result = {
                "status": "error",
                "transfer_id": transfer_id,
                "error": "size or SHA-256 mismatch",
            }
            info = self.client.publish(DATA_ACK_TOPIC, json_bytes(result), qos=QOS)
            if info.rc != mqtt.MQTT_ERR_SUCCESS:
                raise RuntimeError(
                    f"Error ACK publish failed: {mqtt.error_string(info.rc)}"
                )
            LOG.error("Integrity verification failed for transfer %s", transfer_id)
            return

        self.output_dir.mkdir(parents=True, exist_ok=True)
        destination = self.output_dir / f"{transfer_id}.bin"
        destination.write_bytes(assembled)
        result = {
            "status": "complete",
            "transfer_id": transfer_id,
            "size": len(assembled),
            "sha256": actual_digest,
        }
        info = self.client.publish(DATA_ACK_TOPIC, json_bytes(result), qos=QOS)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError(
                f"Completion ACK publish failed: {mqtt.error_string(info.rc)}"
            )
        LOG.info("Saved verified transfer to %s", destination)

    def run(self) -> None:
        self.client.connect(ENDPOINT, PORT, keepalive=60)
        try:
            self.client.loop_forever()
        finally:
            self.client.disconnect()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=("machine", "server"))
    parser.add_argument(
        "--file",
        help="Machine only: send this file instead of generated 2 MB test data",
    )
    parser.add_argument(
        "--output-dir",
        default="received_data",
        help="Server only: directory for verified received files",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=300,
        help="Machine only: overall transfer timeout in seconds",
    )
    return parser.parse_args()


def main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s | %(levelname)s | %(message)s",
    )
    args = parse_args()
    if args.role == "machine":
        Machine(args.file).run(args.timeout)
    else:
        Server(args.output_dir).run()


if __name__ == "__main__":
    main()
