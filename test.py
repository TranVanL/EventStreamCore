import socket
import time
import struct
import random

SERVER_ADDR = ("127.0.0.1", 9000)

def make_frame(topic, payload: bytes, priority: int = 0) -> bytes:
    """
    Frame format:
    [4 bytes frame_len][1 byte priority][2 bytes topic_len][topic][payload]
    """
    t = topic.encode()
    topic_len = len(t)
    body = struct.pack("!H", topic_len) + t + payload
    frame_len = len(body) + 1  # cộng thêm 1 byte cho priority
    return struct.pack("!I", frame_len) + struct.pack("!B", priority) + body

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

            # tạo frame với priority khác nhau
            f1 = make_frame("sensor/1", payload1, priority=1)
            f2 = make_frame("sensor/2", payload2, priority=2)

            # sticky packet
            s.sendall(f1 + f2)

            # fragmented
            frag = f1 + f2
            cut = random.randint(4, len(frag) - 1)
            s.sendall(frag[:cut])
            s.sendall(frag[cut:])

            counter += 1
            # giảm delay để tăng tốc
            time.sleep(0.001)
        except Exception as e:
            print("Connection lost:", e)
            s.close()
            s = connect_socket()

if __name__ == "__main__":
    stress_sender()
