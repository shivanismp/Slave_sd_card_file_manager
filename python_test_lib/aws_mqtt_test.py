import ssl
import paho.mqtt.client as mqtt
import time

endpoint = "a3958f6g61vd15-ats.iot.ap-south-1.amazonaws.com"
port = 8883

ca = "AmazonRootCA1.pem"
cert = "device-certificate.pem.crt"
key = "private.pem.key"

def on_connect(client, userdata, flags, rc):
    print("Connected to AWS IoT! Result code:", rc)

    topics = [
        # ("shivani/test", 0),
        # ("temperature1", 0),
        # ("modbus/response", 0),
        # ("0Z5U4JVMVF/MBM/RES", 0)
        ("0Z5U4JVMVF/MBM/STATUS", 0)

    ]
    client.subscribe(topics)

# def on_message(client, userdata, msg):
#     print(f"Received from {msg.topic}: {msg.payload.decode()}")

def on_message(client, userdata, msg):
    # print(f"Received from {msg.topic}: {msg.payload}")

    # Print in HEX (best for debugging)
    hex_data = ' '.join(f'{b:02X}' for b in msg.payload)
    print("HEX:", hex_data)

client = mqtt.Client()

client.on_connect = on_connect
client.on_message = on_message

client.tls_set(
    ca_certs=ca,
    certfile=cert,
    keyfile=key,
    tls_version=ssl.PROTOCOL_TLSv1_2
)

client.connect(endpoint, port)
client.loop_start()

while True:
    # message = "Hello from Python Secure MQTT"
    # client.publish("shivani/test", message)
    # print("Published:", message)

    # modbus_frame = bytes([0x5F, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC9, 0x75])

    modbus_frame = bytes([0x5F, 0x04, 0x00, 0x25, 0x00, 0x02, 0x6D, 0x7E])


    # modbus_frame = bytes([0x5F, 0x03, 0x00, 0x00, 0x00, 0x16, 0xC9, 0x7A])  # Example Modbus frame (replace with your actual frame)

    

    # client.publish("0Z5U4JVMVF/MBM/REQ", modbus_frame)

    # print("Published Modbus Frame:", modbus_frame)
    time.sleep(1)