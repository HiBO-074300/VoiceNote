# -*- coding: gbk -*-
# ============================================================
#  声记 VoiceNote —— 电脑端上位机（静默后台版）
#  与 voice_server.py 功能完全相同（UDP 发现 + TCP 收文件 +
#  接收时间戳命名 + .part 清理），区别：
#    - 无任何窗口/控制台（打包参数 --noconsole）
#    - 所有日志写入同目录 voice_server_bg.log（超过 256KB 自动清空重写）
#
#  配置：voice_server.ini（与 exe 同目录，首次运行自动生成）
#    [server]  port = 9001   folder = （留空=程序所在目录）
#
#  使用：
#    - 启动：双击 exe（无窗口出现 = 已在后台工作，任务管理器可见）
#    - 停止：任务管理器结束 voice_server_bg.exe 进程
#    - 改配置：编辑 voice_server.ini 后重启进程
#    - 排查：看 voice_server_bg.log
#    - 开机自启（可选）：Win+R 输入 shell:startup，把 exe 快捷方式放进去
#
#  打包：pyinstaller --onefile --noconsole --name voice_server_bg voice_server_bg.py
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
LOG_MAX = 256 * 1024
LOG_PATH = None      # main() 里按 exe/脚本所在目录赋值

_sockets = []          # 当前打开的服务 socket（换配置重启时关闭）
_threads_stop = threading.Event()


def log(msg):
    """写日志文件（GBK），超过上限自动清空重写"""
    try:
        size = os.path.getsize(LOG_PATH) if os.path.exists(LOG_PATH) else 0
        mode = "a" if size < LOG_MAX else "w"
        with open(LOG_PATH, mode, encoding="gbk", errors="replace") as f:
            f.write("[%s] %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg))
    except OSError:
        pass


def base_dir():
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


def load_config():
    """读 voice_server.ini；不存在则生成默认配置。返回 (port, folder)"""
    path = os.path.join(base_dir(), "voice_server.ini")
    cfg = configparser.ConfigParser()
    if os.path.isfile(path):
        cfg.read(path, encoding="gbk")
    else:
        with open(path, "w", encoding="gbk") as f:
            f.write("# 声记上位机配置文件（静默后台版）\n")
            f.write("[server]\n")
            f.write("# 端口：必须与 ESP32 固件 main/uploader.c 的 UP_PORT 一致\n")
            f.write("port = 9001\n")
            f.write("# 录音保存文件夹：留空 = 本程序所在目录；示例：D:\\Recordings\n")
            f.write("folder =\n")
        log("已生成默认配置文件: %s" % path)
    port = int(cfg.get("server", "port", fallback="9001"))
    folder = cfg.get("server", "folder", fallback="").strip().strip('"')
    if not folder:
        folder = base_dir()
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
    log("[tcp] connected: %s" % peer)
    try:
        while True:
            head = recv_exact(conn, 4)
            if head is None:
                break
            if head != MAGIC:
                log("[tcp] %s bad magic, closing" % peer)
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
            log("[tcp] %s -> %s (%d bytes)" % (peer, name, size))

            # 以"传输开始时间"命名：20261111_153012_REC000.WAV，永不覆盖
            stamp = time.strftime("%Y%m%d_%H%M%S")
            final = os.path.join(save_dir, "%s_%s" % (stamp, name))
            tmp = final + ".part"
            got = 0
            ok = True
            try:
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
                    log("[tcp] saved: %s (%d bytes)" % (final, size))
                else:
                    conn.sendall(b"NG")
                    log("[tcp] incomplete: %s (%d/%d)" % (name, got, size))
            finally:
                # 任何退出路径都不留 .part 半截文件
                if os.path.exists(tmp):
                    try:
                        os.remove(tmp)
                        log("[tcp] cleaned partial: %s" % tmp)
                    except OSError:
                        pass
    except (ConnectionResetError, socket.timeout, OSError) as e:
        log("[tcp] connection error (%s): %s" % (peer, e))
    finally:
        conn.close()
        log("[tcp] disconnected: %s" % peer)


def udp_discovery(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.settimeout(1)
    _sockets.append(s)
    try:
        s.bind(("", port))
    except OSError as e:
        log("[udp] bind 失败（端口 %d 被占用？）：%s" % (port, e))
        return
    log("[udp] discovery listener on 0.0.0.0:%d" % port)
    while not _threads_stop.is_set():
        try:
            data, addr = s.recvfrom(128)
        except socket.timeout:
            continue
        except OSError:
            break
        if data == DISC_REQ:
            try:
                s.sendto(DISC_RSP, addr)
                log("[udp] discovery from %s -> replied" % addr[0])
            except OSError:
                pass
    s.close()


def tcp_server(port, save_dir):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.settimeout(1)
    _sockets.append(srv)
    try:
        srv.bind(("", port))
        srv.listen(2)
    except OSError as e:
        log("[tcp] listen 失败（端口 %d 被占用？）：%s" % (port, e))
        return
    log("[tcp] file server on 0.0.0.0:%d" % port)
    while not _threads_stop.is_set():
        try:
            conn, addr = srv.accept()
        except socket.timeout:
            continue
        except OSError:
            break
        threading.Thread(target=handle_conn, args=(conn, addr, save_dir),
                         daemon=True).start()
    srv.close()


def clean_leftover_parts(save_dir):
    """启动清理：上次退出/断电遗留的 .part 全部删除"""
    try:
        leftovers = [fn for fn in os.listdir(save_dir) if fn.endswith(".part")]
    except OSError:
        return
    for fn in leftovers:
        try:
            os.remove(os.path.join(save_dir, fn))
            log("[clean] removed leftover partial file: %s" % fn)
        except OSError:
            pass
    if leftovers:
        log("[clean] %d leftover .part file(s) removed" % len(leftovers))


def main():
    global LOG_PATH
    LOG_PATH = os.path.join(base_dir(), "voice_server_bg.log")

    port, save_dir = load_config()
    save_dir = os.path.abspath(save_dir)
    if not os.path.isdir(save_dir):
        os.makedirs(save_dir)
    clean_leftover_parts(save_dir)
    log("=====================================================")
    log("声记上位机（静默后台版）启动")
    log("保存文件夹: %s" % save_dir)
    log("端口: %d  |  停止: 任务管理器结束 voice_server_bg.exe" % port)

    t = threading.Thread(target=udp_discovery, args=(port,), daemon=True)
    t.start()
    tcp_server(port, save_dir)      # 主线程直接跑 TCP 循环，永不返回


if __name__ == "__main__":
    main()
