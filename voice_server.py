# -*- coding: gbk -*-
# ============================================================
#  声记 VoiceNote —— 电脑端上位机
#  接收 ESP32 自动上传的录音文件（UDP 发现 + TCP 收文件）
#
#  使用：
#    双击 voice_server.exe 或运行 python voice_server.py
#
#  配置：程序同目录的 voice_server.ini（首次运行自动生成）
#    [server]
#    port   = 9001        ← 和 ESP32 固件 uploader.c 的 UP_PORT 一致
#    folder =              ← 留空 = 程序所在目录；也可写 D:\Recordings
#
#  命令行参数可临时覆盖配置：
#    python voice_server.py D:\Recordings     （指定文件夹）
#    python voice_server.py --port 9002       （指定端口）
# ============================================================

import configparser
import os
import socket
import sys
import threading
import time

MAGIC = b"VNF1"
DISC_REQ = b"VNOTE_DISCOVER"
DISC_RSP = b"VNOTE_SERVER"

# ---- 运行基准目录：exe 所在目录（打包后）或脚本所在目录 ----
def base_dir():
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


def load_config():
    """读取 voice_server.ini；不存在则生成带注释的默认配置。返回 (port, folder)"""
    path = os.path.join(base_dir(), "voice_server.ini")
    cfg = configparser.ConfigParser()
    if os.path.isfile(path):
        cfg.read(path, encoding="gbk")
    else:
        with open(path, "w", encoding="gbk") as f:
            f.write("# 声记上位机配置文件\n")
            f.write("[server]\n")
            f.write("# 监听/上传端口：必须与 ESP32 固件 main/uploader.c 的 UP_PORT 一致\n")
            f.write("port = 9001\n")
            f.write("# 录音保存文件夹：留空 = 本程序所在目录；示例：D:\\Recordings\n")
            f.write("folder =\n")
        print("已生成默认配置文件: %s" % path)

    port = int(cfg.get("server", "port", fallback="9001"))
    folder = cfg.get("server", "folder", fallback="").strip().strip('"')
    if not folder:
        folder = base_dir()

    # 命令行参数覆盖配置：--port N / 直接给文件夹路径
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--port" and i + 1 < len(args):
            port = int(args[i + 1])
            i += 2
        elif a.isdigit():
            port = int(a)
            i += 1
        else:
            folder = a.strip('"')
            i += 1
    return port, folder


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
            name = os.path.basename(name.replace("\\", "/"))
            size = sz_b[0] | (sz_b[1] << 8) | (sz_b[2] << 16) | (sz_b[3] << 24)
            print("[tcp] %s -> %s (%d bytes)" % (peer, name, size))

            # 以"接收时间"命名，杜绝卡上序号循环导致的覆盖：
            # 20261111_153012_REC001.WAV（时间+原文件名，既唯一又可追溯）
            stamp = time.strftime("%Y%m%d_%H%M%S")
            final = os.path.join(save_dir, "%s_%s" % (stamp, name))
            tmp = final + ".part"
            got = 0
            ok = True
            last_pct = -1
            try:
                with open(tmp, "wb") as f:
                    while got < size:
                        chunk = conn.recv(min(4096, size - got))
                        if not chunk:
                            ok = False
                            break
                        f.write(chunk)
                        got += len(chunk)
                        pct = got * 100 // size
                        if pct != last_pct:
                            sys.stdout.write("\r[tcp] %s %3d%% (%d/%d bytes)"
                                             % (name, pct, got, size))
                            sys.stdout.flush()
                            last_pct = pct
                sys.stdout.write("\n")
                sys.stdout.flush()
                if ok and got == size:
                    os.replace(tmp, final)
                    conn.sendall(b"OK")
                    print("[tcp] saved: %s" % final)
                else:
                    conn.sendall(b"NG")
                    print("[tcp] incomplete: %s (%d/%d), asked retry" % (name, got, size))
            finally:
                # 无论哪条路径退出（成功改名后 tmp 不存在），都保证不留 .part
                if os.path.exists(tmp):
                    try:
                        os.remove(tmp)
                        print("[tcp] cleaned partial: %s" % tmp)
                    except OSError:
                        pass
    except (ConnectionResetError, socket.timeout, OSError):
        pass
    finally:
        conn.close()
        print("[tcp] disconnected: %s" % peer)


def udp_discovery(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", port))
    print("[udp] discovery listener on 0.0.0.0:%d" % port)
    while True:
        data, addr = s.recvfrom(128)
        if data == DISC_REQ:
            s.sendto(DISC_RSP, addr)
            print("[udp] discovery from %s -> replied" % addr[0])


def main():
    port, save_dir = load_config()
    save_dir = os.path.abspath(save_dir)
    if not os.path.isdir(save_dir):
        os.makedirs(save_dir)
    # 启动清理：上次程序退出/断电遗留的 .part 半截文件全部删除
    leftovers = [fn for fn in os.listdir(save_dir) if fn.endswith(".part")]
    for fn in leftovers:
        try:
            os.remove(os.path.join(save_dir, fn))
            print("[clean] removed leftover partial file: %s" % fn)
        except OSError:
            pass
    if leftovers:
        print("[clean] %d leftover .part file(s) removed" % len(leftovers))
    print("==============================================")
    print("  声记上位机 —— 录音自动接收服务")
    print("  保存文件夹 : %s" % save_dir)
    print("  端口       : %d" % port)
    print("==============================================")

    t = threading.Thread(target=udp_discovery, args=(port,), daemon=True)
    t.start()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("", port))
    srv.listen(2)
    print("[tcp] file server on 0.0.0.0:%d, waiting for ESP32 ... (Ctrl+C 退出)" % port)
    try:
        while True:
            conn, addr = srv.accept()
            conn.settimeout(15)
            threading.Thread(target=handle_conn, args=(conn, addr, save_dir),
                             daemon=True).start()
    except KeyboardInterrupt:
        print("\nbye")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
