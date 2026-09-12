#!/usr/bin/env python3
"""Control the machine through MQTT-backed Modbus using keyboard input.

Keyboard command 1 writes holding register 0 = 2 (machine ON).
Keyboard command 0 writes holding register 0 = 1 (machine OFF).
Keyboard command q exits the program.
"""

from __future__ import annotations

import ssl
import threading

import paho.mqtt.client as mqtt


AWS_ENDPOINT = "a3958f6g61vd15-ats.iot.ap-south-1.amazonaws.com"
AWS_PORT = 8883

CA_CERTIFICATE = "AmazonRootCA1.pem"
DEVICE_CERTIFICATE = "device-certificate.pem.crt"
PRIVATE_KEY = "private.pem.key"

MACHINE_SERIAL = "CSLD94MTZ2"
MQTT_REQUEST_TOPIC = f"{MACHINE_SERIAL}/MBM/REQ"
MQTT_RESPONSE_TOPIC = f"{MACHINE_SERIAL}/MBM/RES"

MODBUS_SLAVE_ID = 0x5F
MODBUS_FC_WRITE_SINGLE_REGISTER = 0x06
MACHINE_TRIGGER_HOLDING_REGISTER = 0
MACHINE_ON_VALUE = 2
MACHINE_OFF_VALUE = 1

connected_event = threading.Event()


def modbus_crc16(data: bytes) -> int:
    """Return the standard Modbus RTU CRC16 for data."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def build_write_single_register(register_address: int, value: int) -> bytes:
    """Build an FC06 Modbus RTU request with CRC low byte first."""
    if not 0 <= register_address <= 0xFFFF:
        raise ValueError("register address must be between 0 and 65535")
    if not 0 <= value <= 0xFFFF:
        raise ValueError("register value must be between 0 and 65535")

    request_without_crc = bytes(
        [
            MODBUS_SLAVE_ID,
            MODBUS_FC_WRITE_SINGLE_REGISTER,
            (register_address >> 8) & 0xFF,
            register_address & 0xFF,
            (value >> 8) & 0xFF,
            value & 0xFF,
        ]
    )
    crc = modbus_crc16(request_without_crc)
    return request_without_crc + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def frame_hex(frame: bytes) -> str:
    return " ".join(f"{byte:02X}" for byte in frame)


def valid_modbus_frame(frame: bytes) -> bool:
    if len(frame) < 4:
        return False
    received_crc = frame[-2] | (frame[-1] << 8)
    return received_crc == modbus_crc16(frame[:-2])


def on_connect(client, userdata, flags, result_code):
    del userdata, flags

    if result_code != 0:
        print(f"AWS IoT connection failed. Result code: {result_code}")
        return

    print("Connected to AWS IoT")
    subscribe_result, _ = client.subscribe(MQTT_RESPONSE_TOPIC, qos=1)
    if subscribe_result != mqtt.MQTT_ERR_SUCCESS:
        print(f"Failed to subscribe to {MQTT_RESPONSE_TOPIC}")
        return

    print(f"Subscribed: {MQTT_RESPONSE_TOPIC}")
    connected_event.set()


def on_disconnect(client, userdata, result_code):
    del client, userdata
    connected_event.clear()
    if result_code != 0:
        print(f"Disconnected unexpectedly. Result code: {result_code}")


def on_message(client, userdata, message):
    del client, userdata

    frame = bytes(message.payload)
    # print(f"\nReceived from {message.topic}")
    # print(f"RX HEX: {frame_hex(frame)}")

    if not valid_modbus_frame(frame):
        print("Response rejected: invalid Modbus CRC or frame length")
        return

    if frame[0] != MODBUS_SLAVE_ID:
        print(f"Response is for another slave: {frame[0]}")
        return

    if frame[1] & 0x80:
        exception_code = frame[2] if len(frame) >= 5 else -1
        print(f"Modbus exception response: code {exception_code}")
        return

    if len(frame) == 8 and frame[1] == MODBUS_FC_WRITE_SINGLE_REGISTER:
        register_address = (frame[2] << 8) | frame[3]
        value = (frame[4] << 8) | frame[5]

        if register_address == MACHINE_TRIGGER_HOLDING_REGISTER:
            if value == MACHINE_ON_VALUE:
                print("Machine ON command acknowledged")
            elif value == MACHINE_OFF_VALUE:
                print("Machine OFF command acknowledged")
            else:
                print(f"Holding register 0 acknowledged with value {value}")


def publish_machine_command(client: mqtt.Client, turn_on: bool) -> None:
    value = MACHINE_ON_VALUE if turn_on else MACHINE_OFF_VALUE
    state_text = "ON" if turn_on else "OFF"
    frame = build_write_single_register(
        MACHINE_TRIGGER_HOLDING_REGISTER,
        value,
    )

    publish_info = client.publish(
        MQTT_REQUEST_TOPIC,
        payload=frame,
        qos=1,
        retain=False,
    )

    if publish_info.rc != mqtt.MQTT_ERR_SUCCESS:
        print(f"Failed to publish {state_text}: MQTT error {publish_info.rc}")
        return

    publish_info.wait_for_publish(timeout=5.0)
    if not publish_info.is_published():
        print(f"{state_text} command was not acknowledged by the MQTT broker")
        return

    print(f"Published {state_text}: holding register 0 = {value}")
    print(f"TX HEX: {frame_hex(frame)}")


def main() -> int:
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    client.tls_set(
        ca_certs=CA_CERTIFICATE,
        certfile=DEVICE_CERTIFICATE,
        keyfile=PRIVATE_KEY,
        tls_version=ssl.PROTOCOL_TLSv1_2,
    )
    client.reconnect_delay_set(min_delay=1, max_delay=30)

    try:
        client.connect(AWS_ENDPOINT, AWS_PORT, keepalive=30)
        client.loop_start()

        print("Connecting to AWS IoT...")
        if not connected_event.wait(timeout=15.0):
            print("Connection or subscription timed out")
            return 1

        print("\nKeyboard controls:")
        print("  1 = Machine ON  (holding register 0 = 2)")
        print("  0 = Machine OFF (holding register 0 = 1)")
        print("  q = Quit")

        while True:
            command = input("\nEnter command [1/0/q]: ").strip().lower()

            if command == "q":
                break
            if command not in {"0", "1"}:
                print("Invalid command. Enter 1, 0, or q.")
                continue
            if not connected_event.is_set():
                print("MQTT is disconnected. Wait for reconnection and try again.")
                continue

            publish_machine_command(client, turn_on=(command == "1"))

        return 0
    except KeyboardInterrupt:
        print("\nStopped by user")
        return 0
    except (OSError, mqtt.MQTTException) as error:
        print(f"MQTT error: {error}")
        return 1
    finally:
        client.disconnect()
        client.loop_stop()


if __name__ == "__main__":
    raise SystemExit(main())
