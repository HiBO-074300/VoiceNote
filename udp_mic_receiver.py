# ESP32 mic UDP receiver (subscribe + unicast mode)
#
# Usage:
#   pip install pyaudio
#   python udp_mic_receiver.py --list       # list audio devices
#   python udp_mic_receiver.py              # play to system default output
#   python udp_mic_receiver.py 5            # play to device index 5
#
# How it works:
#   Every 5s this script sends a "SUB" broadcast to port 9000.
#   ESP32 replies with UNICAST audio (reliable) instead of broadcast
#   (WiFi broadcast loses 30~70% of packets).

import socket
import sys
import time

try:
    import pyaudio
except ImportError:
    print("pyaudio not installed. Run:  pip install pyaudio")
    sys.exit(1)

PORT = 9000
RATE = 16000


def list_devices(pa):
    print("---- audio devices ----")
    for i in range(pa.get_device_count()):
        info = pa.get_device_info_by_index(i)
        kind = []
        if info['maxOutputChannels'] > 0:
            kind.append('OUT')
        if info['maxInputChannels'] > 0:
            kind.append('IN')
        print("[%2d] %-8s %s" % (i, '/'.join(kind), info['name']))
    print("-----------------------")


def local_ips():
    ips = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if ip not in ips and not ip.startswith('127.'):
                ips.append(ip)
    except Exception:
        pass
    return ips


def main():
    dev_index = None
    if '--list' in sys.argv:
        list_devices(pyaudio.PyAudio())
        return
    for a in sys.argv[1:]:
        if a.isdigit():
            dev_index = int(a)

    ips = local_ips()
    print("local IPs: %s" % ', '.join(ips or ['(unknown)']))

    pa = pyaudio.PyAudio()
    if dev_index is None:
        dev_index = pa.get_default_output_device_info()['index']
        print("using default output device [%d]" % dev_index)
    else:
        name = pa.get_device_info_by_index(dev_index)['name']
        print("output device [%d]: %s" % (dev_index, name))

    out = pa.open(format=pyaudio.paInt16,
                  channels=1,
                  rate=RATE,
                  output=True,
                  output_device_index=dev_index)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(('0.0.0.0', PORT))
    sock.settimeout(1)

    # directed broadcast targets derived from local subnets (/24 assumed)
    subs = ['.'.join(ip.split('.')[:3]) + '.255'
            for ip in ips if ip.startswith('192.168.') or ip.startswith('10.')]
    print("listening on UDP 0.0.0.0:%d, subscribing to: %s" % (PORT, ', '.join(subs)))

    n_pkts = 0
    last_src = None
    last_beat = time.time()
    last_sub = 0.0
    got = False
    try:
        while True:
            # re-subscribe until connected, then refresh every 15s
            now = time.time()
            if (not got and now - last_sub >= 2) or (got and now - last_sub >= 15):
                last_sub = now
                for t in subs:
                    try:
                        sock.sendto(b'SUB', (t, PORT))
                    except Exception:
                        pass

            try:
                data, addr = sock.recvfrom(2048)
                if data == b'SUB':
                    continue  # our own subscription echoed back
                n_pkts += 1
                last_src = addr[0]
                if not got:
                    got = True
                    print(">>> STREAM CONNECTED (unicast) from %s, playing <<<" % addr[0])
            except socket.timeout:
                pass

            now = time.time()
            if now - last_beat >= 5:
                last_beat = now
                if n_pkts > 0:
                    # 250 packets / 5s = realtime; report loss ratio
                    loss = max(0.0, 1.0 - n_pkts / 250.0) * 100.0
                    print("stats: %d pkts/5s (~%.0f%% loss), last src %s"
                          % (n_pkts, loss, last_src))
                    n_pkts = 0
                else:
                    print("waiting... no packets yet (check firewall / ESP32 log)")
    except KeyboardInterrupt:
        print("\nbye")
    finally:
        out.close()
        pa.terminate()
        sock.close()


if __name__ == '__main__':
    main()
