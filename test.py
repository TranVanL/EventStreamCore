#!/usr/bin/env python3
import asyncio
import argparse
import os
import signal
import time
from typing import Optional

class Stats:
    def __init__(self):
        self.bytes_sent = 0
        self.messages_sent = 0
        self.errors = 0
        self.start_time = time.time()

    def snapshot(self):
        dur = max(1e-6, time.time() - self.start_time)
        return {
            "bytes_sent": self.bytes_sent,
            "messages_sent": self.messages_sent,
            "errors": self.errors,
            "duration_s": dur,
            "throughput_mbps": (self.bytes_sent * 8) / (dur * 1_000_000),
            "msg_rate_per_s": self.messages_sent / dur,
        }

async def sender_task(host: str, port: int, frame: bytes, msg_rate: Optional[float], stats: Stats, keepalive: bool, reconnect_delay: float):
    while True:
        try:
            reader, writer = await asyncio.open_connection(host, port)
            sock = writer.get_extra_info('socket')
            if keepalive and sock is not None:
                sock.setsockopt(asyncio.socket.SOL_SOCKET, asyncio.socket.SO_KEEPALIVE, 1)

            interval = (1.0 / msg_rate) if msg_rate and msg_rate > 0 else 0.0
            next_send = time.perf_counter()

            while True:
                writer.write(frame)
                await writer.drain()
                stats.bytes_sent += len(frame)
                stats.messages_sent += 1

                if interval > 0:
                    next_send += interval
                    delay = next_send - time.perf_counter()
                    if delay > 0:
                        await asyncio.sleep(delay)
                else:
                    await asyncio.sleep(0)
        except Exception:
            stats.errors += 1
            await asyncio.sleep(reconnect_delay)
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass

async def stats_printer(stats: Stats, every: float):
    while True:
        await asyncio.sleep(every)
        s = stats.snapshot()
        print(f"[{time.strftime('%H:%M:%S')}] "
              f"bytes={s['bytes_sent']} "
              f"msgs={s['messages_sent']} "
              f"errors={s['errors']} "
              f"dur={s['duration_s']:.1f}s "
              f"throughput={s['throughput_mbps']:.2f} Mbps "
              f"msg_rate={s['msg_rate_per_s']:.2f}/s")

async def main():
    parser = argparse.ArgumentParser(description="TCP stress sender")
    parser.add_argument("--host", required=True, help="Target host/IP")
    parser.add_argument("--port", required=True, type=int, help="Target TCP port")
    parser.add_argument("--conns", type=int, default=1, help="Number of concurrent connections")
    parser.add_argument("--size", type=int, default=1024, help="Payload size in bytes")
    parser.add_argument("--rate", type=float, default=0.0, help="Messages per second per connection (0 = max)")
    parser.add_argument("--text", type=str, default="", help="Optional text payload (overrides size)")
    parser.add_argument("--topic", type=str, required=True, help="Topic string (UTF-8)")
    parser.add_argument("--stats", type=float, default=1.0, help="Stats print interval seconds")
    parser.add_argument("--ka", action="store_true", help="Enable TCP keepalive")
    parser.add_argument("--reconnect", type=float, default=0.2, help="Reconnect delay seconds on error")
    args = parser.parse_args()

    payload = args.text.encode() if args.text else os.urandom(args.size)

    # build frame: [topic_len 2 bytes][topic utf8][payload]
    topic_bytes = args.topic.encode("utf-8")
    topic_len = len(topic_bytes)
    if topic_len > 0xFFFF:
        raise ValueError("Topic too long (max 65535 bytes)")
    topic_len_bytes = topic_len.to_bytes(2, byteorder="big")  # network order
    frame = topic_len_bytes + topic_bytes + payload

    stats = Stats()
    loop = asyncio.get_running_loop()
    stop_event = asyncio.Event()

    def handle_sig(*_):
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, handle_sig)
        except NotImplementedError:
            pass

    tasks = []
    for _ in range(args.conns):
        tasks.append(asyncio.create_task(sender_task(
            args.host, args.port, frame, args.rate if args.rate > 0 else None,
            stats, args.ka, args.reconnect
        )))
    tasks.append(asyncio.create_task(stats_printer(stats, args.stats)))

    await stop_event.wait()
    for t in tasks:
        t.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
