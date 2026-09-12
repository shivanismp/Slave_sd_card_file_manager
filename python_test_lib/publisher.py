import paho.mqtt.client as mqtt
import time

broker = "broker.hivemq.com"
port = 1883
topic = "shivani/test1"

client = mqtt.Client()

client.connect(broker, port)

while True:
    message = "Hello from Python MQTT"
    client.publish(topic, message)
    print("Message Sent:", message)
    time.sleep(5)