import socket
import time
import struct
import random

SERVER_ADDR = ("127.0.0.1", 9000)

def make_frame(topic, payload: bytes) -> bytes:
    t = topic.encode()
    topic_len = len(t)
    body = struct.pack("!H", topic_len) + t + payload
    frame_len = len(body)
    return struct.pack("!I", frame_len) + body

def connect_socket():
    while True:
        try:
            s = socket.socket()
            s.connect(SERVER_ADDR)
            print("Connected to server", SERVER_ADDR)
            return s
        except Exception as e:
            print("Reconnect failed:", e)
            time.sleep(1)

def stress_sender():
    s = connect_socket()
    counter = 0
    while True:
        try:
            payload1 = f"helloWorld-{counter}".encode()
            payload2 = ("world" + str(counter) +
                        "".join(random.choice("0123456789ABCDEF") for _ in range(40))).encode()

            f1 = make_frame("sensor/1", payload1)
            f2 = make_frame("sensor/2", payload2)

            # sticky packet
            s.sendall(f1 + f2)

            # fragmented
            frag = f1 + f2
            cut = random.randint(4, len(frag) - 1)
            s.sendall(frag[:cut])
            s.sendall(frag[cut:])

            counter += 1
            # giảm delay để tăng tốc
            # time.sleep(0.1)  # bỏ hoặc giảm xuống 0.001
        except Exception as e:
            print("Connection lost:", e)
            s.close()
            s = connect_socket()


if __name__ == "__main__":
    stress_sender()
