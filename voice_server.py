# VoiceNote PC host (voice_server.py)
#
# Receives audio files uploaded by the ESP32 recorder over the LAN.
#
# Flow:
#   1) UDP :9001 - answers ESP32 broadcast "VNOTE_DISCOVER" with "VNOTE_SERVER"
#   2) TCP :9001 - ESP32 connects and uploads files, one frame per file:
#        [4B magic "VNF1"][2B name-len][name][4B size LE][data] -> waits "OK"/"NG"
#   3) Files are saved as <name>.part first, renamed after full+verified receive
#   4) ESP32 deletes its local copy after "OK"
#
# Usage:
#   python voice_server.py                 # save to this script's folder
#   python voice_server.py D:\Recordings   # save to a specific folder
#
# Requires: Python 3.8+, no third-party packages.

import os
import socket
import sys
import threading

PORT = 9001
MAGIC = b"VNF1"
DISC_REQ = b"VNOTE_DISCOVER"
DISC_RSP = b"VNOTE_SERVER"


def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def handle_conn(conn, addr, save_dir):
    peer = "%s:%d" % (addr[0], addr[1])
    print("[tcp] connected: %s" % peer)
    try:
        while True:
            head = recv_exact(conn, 4)
            if head is None:
                break
            if head != MAGIC:
                print("[tcp] %s bad magic, closing" % peer)
                break
            nl = recv_exact(conn, 2)
            if nl is None:
                break
            name_len = nl[0] | (nl[1] << 8)
            name_b = recv_exact(conn, name_len)
            sz_b = recv_exact(conn, 4)
            if name_b is None or sz_b is None:
                break
            name = name_b.decode("ascii", errors="replace")
            # basic sanitizing: keep plain file name only
            name = os.path.basename(name.replace("\\", "/"))
            size = sz_b[0] | (sz_b[1] << 8) | (sz_b[2] << 16) | (sz_b[3] << 24)
            print("[tcp] %s -> %s (%d bytes)" % (peer, name, size))

            final = os.path.join(save_dir, name)
            tmp = final + ".part"
            got = 0
            ok = True
            with open(tmp, "wb") as f:
                while got < size:
                    chunk = conn.recv(min(4096, size - got))
                    if not chunk:
                        ok = False
                        break
                    f.write(chunk)
                    got += len(chunk)
            if ok and got == size:
                os.replace(tmp, final)
                conn.sendall(b"OK")
                print("[tcp] saved: %s (%d bytes)" % (final, size))
            else:
                try:
                    os.remove(tmp)
                except OSError:
                    pass
                conn.sendall(b"NG")
                print("[tcp] incomplete: %s (%d/%d), asked retry" % (name, got, size))
    except (ConnectionResetError, socket.timeout):
        pass
    finally:
        conn.close()
        print("[tcp] disconnected: %s" % peer)


def udp_discovery():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", PORT))
    print("[udp] discovery listener on 0.0.0.0:%d" % PORT)
    while True:
        data, addr = s.recvfrom(128)
        if data == DISC_REQ:
            s.sendto(DISC_RSP, addr)
            print("[udp] discovery from %s -> replied" % addr[0])


def main():
    if len(sys.argv) > 1:
        save_dir = sys.argv[1]
    else:
        save_dir = os.path.dirname(os.path.abspath(__file__))
    save_dir = os.path.abspath(save_dir)
    if not os.path.isdir(save_dir):
        os.makedirs(save_dir)
    print("save folder: %s" % save_dir)

    t = threading.Thread(target=udp_discovery, daemon=True)
    t.start()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("", PORT))
    srv.listen(2)
    print("[tcp] file server on 0.0.0.0:%d, waiting for ESP32 ..." % PORT)
    try:
        while True:
            conn, addr = srv.accept()
            conn.settimeout(15)
            threading.Thread(target=handle_conn, args=(conn, addr, save_dir),
                             daemon=True).start()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
