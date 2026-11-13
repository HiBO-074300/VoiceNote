# -*- coding: gbk -*-
# ============================================================
#  声记 VoiceNote —— 电脑端上位机（图形界面版）
#  接收 ESP32 自动上传的录音：UDP 发现 + TCP 收文件
#
#  功能：
#    - 界面显示连接状态 / 接收进度 / 已收文件列表 / 事件日志
#    - 可选择保存文件夹、修改端口（写入 voice_server.ini）
#    - 打包：pyinstaller --onefile --noconsole --name voice_server_gui voice_server_gui.py
#
#  配置文件 voice_server.ini（与 exe 同目录，首次运行自动生成）：
#    [server]  port = 9001   folder = （留空=程序所在目录）
# ============================================================

import configparser
import os
import queue
import socket
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext

MAGIC = b"VNF1"
DISC_REQ = b"VNOTE_DISCOVER"
DISC_RSP = b"VNOTE_SERVER"


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
            f.write("# 声记上位机配置文件\n")
            f.write("[server]\n")
            f.write("# 端口：必须与 ESP32 固件 main/uploader.c 的 UP_PORT 一致\n")
            f.write("port = 9001\n")
            f.write("# 录音保存文件夹：留空 = 本程序所在目录；示例：D:\\Recordings\n")
            f.write("folder =\n")
    port = int(cfg.get("server", "port", fallback="9001"))
    folder = cfg.get("server", "folder", fallback="").strip().strip('"')
    if not folder:
        folder = base_dir()
    return port, folder


def save_config(port, folder):
    path = os.path.join(base_dir(), "voice_server.ini")
    with open(path, "w", encoding="gbk") as f:
        f.write("# 声记上位机配置文件\n")
        f.write("[server]\n")
        f.write("# 端口：必须与 ESP32 固件 main/uploader.c 的 UP_PORT 一致\n")
        f.write("port = %d\n" % port)
        f.write("# 录音保存文件夹：留空 = 本程序所在目录\n")
        f.write("folder = %s\n" % folder)


def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


class App:
    def __init__(self, root):
        self.root = root
        root.title("声记上位机 —— 录音自动接收")
        root.geometry("640x430")
        root.resizable(False, True)

        self.log_q = queue.Queue()
        self.stop_evt = threading.Event()
        self.files_received = 0

        self.port, folder = load_config()

        # ---- 顶部：配置区 ----
        top = tk.Frame(root, padx=8, pady=6)
        top.pack(fill="x")
        tk.Label(top, text="保存文件夹:").grid(row=0, column=0, sticky="w")
        self.folder_var = tk.StringVar(value=folder)
        tk.Entry(top, textvariable=self.folder_var, width=52).grid(row=0, column=1, padx=4)
        tk.Button(top, text="浏览...", width=8, command=self.pick_folder).grid(row=0, column=2)
        tk.Label(top, text="端口:").grid(row=1, column=0, sticky="w", pady=(4, 0))
        self.port_var = tk.StringVar(value=str(self.port))
        tk.Entry(top, textvariable=self.port_var, width=10).grid(row=1, column=1, sticky="w", padx=4, pady=(4, 0))
        tk.Button(top, text="保存配置并重启服务", width=18, command=self.apply_config).grid(row=1, column=2, pady=(4, 0))

        # ---- 状态行 ----
        mid = tk.Frame(root, padx=8, pady=4)
        mid.pack(fill="x")
        self.status_var = tk.StringVar(value="等待 ESP32 连接...")
        tk.Label(mid, textvariable=self.status_var, fg="blue").pack(side="left")
        self.stat2_var = tk.StringVar(value="已收文件: 0")
        tk.Label(mid, textvariable=self.stat2_var).pack(side="right")

        # ---- 当前接收进度 ----
        self.prog_var = tk.StringVar(value="")
        tk.Label(root, textvariable=self.prog_var, fg="green", padx=8, anchor="w").pack(fill="x")

        # ---- 已收文件列表 ----
        tk.Label(root, text="已收到的文件：", padx=8, anchor="w").pack(fill="x")
        self.file_list = tk.Listbox(root, height=6)
        self.file_list.pack(fill="both", padx=8, expand=False)

        # ---- 日志 ----
        self.log_box = scrolledtext.ScrolledText(root, height=8, state="disabled")
        self.log_box.pack(fill="both", padx=8, pady=6, expand=True)

        # ---- 启动服务线程 ----
        self.start_servers(self.port, self.folder_var.get())

        root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.root.after(200, self.poll)

    # ---------- 日志与进度（线程安全：走队列） ----------
    def log(self, msg):
        self.log_q.put(("log", msg))

    def progress(self, text):
        self.log_q.put(("prog", text))

    def status(self, text):
        self.log_q.put(("stat", text))

    def poll(self):
        try:
            while True:
                kind, msg = self.log_q.get_nowait()
                if kind == "log":
                    ts = time.strftime("%H:%M:%S")
                    self.log_box.configure(state="normal")
                    self.log_box.insert("end", "[%s] %s\n" % (ts, msg))
                    self.log_box.see("end")
                    self.log_box.configure(state="disabled")
                elif kind == "prog":
                    self.prog_var.set(msg)
                elif kind == "stat":
                    self.status_var.set(msg)
        except queue.Empty:
            pass
        self.root.after(200, self.poll)

    # ---------- 服务线程 ----------
    def start_servers(self, port, folder):
        self.stop_evt.clear()
        threading.Thread(target=self.udp_discovery, args=(port,), daemon=True).start()
        threading.Thread(target=self.tcp_server, args=(port, folder), daemon=True).start()
        self.log("服务已启动：端口 %d，保存到 %s" % (port, folder))
        self.status("等待 ESP32 连接...")

    def stop_servers(self):
        self.stop_evt.set()

    def udp_discovery(self, port):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.settimeout(1)
        try:
            s.bind(("", port))
        except OSError as e:
            self.log("UDP 监听失败（端口 %d 被占用？）：%s" % (port, e))
            return
        self.log("[udp] 发现服务就绪 (0.0.0.0:%d)" % port)
        while not self.stop_evt.is_set():
            try:
                data, addr = s.recvfrom(128)
            except socket.timeout:
                continue
            except OSError:
                break
            if data == DISC_REQ:
                try:
                    s.sendto(DISC_RSP, addr)
                    self.log("[udp] ESP32 发现来自 %s -> 已应答" % addr[0])
                except OSError:
                    pass
        s.close()

    def tcp_server(self, port, folder):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.settimeout(1)
        try:
            srv.bind(("", port))
            srv.listen(2)
        except OSError as e:
            self.log("TCP 监听失败（端口 %d 被占用？）：%s" % (port, e))
            return
        self.log("[tcp] 文件服务就绪 (0.0.0.0:%d)" % port)
        while not self.stop_evt.is_set():
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self.status("ESP32 已连接 (%s)" % addr[0])
            threading.Thread(target=self.handle_conn,
                             args=(conn, addr, folder), daemon=True).start()
        srv.close()

    def handle_conn(self, conn, addr, folder):
        peer = "%s:%d" % (addr[0], addr[1])
        self.log("[tcp] 连接建立: %s" % peer)
        try:
            conn.settimeout(15)
            while True:
                head = recv_exact(conn, 4)
                if head is None:
                    break
                if head != MAGIC:
                    self.log("[tcp] 协议错误（非 VNF1 帧），断开")
                    break
                nl = recv_exact(conn, 2)
                if nl is None:
                    break
                name_len = nl[0] | (nl[1] << 8)
                name_b = recv_exact(conn, name_len)
                sz_b = recv_exact(conn, 4)
                if name_b is None or sz_b is None:
                    break
                name = os.path.basename(
                    name_b.decode("ascii", errors="replace").replace("\\", "/"))
                size = sz_b[0] | (sz_b[1] << 8) | (sz_b[2] << 16) | (sz_b[3] << 24)
                self.log("[tcp] 接收: %s (%d bytes)" % (name, size))

                # 以"传输开始时间"命名，杜绝卡上序号循环导致的覆盖
                stamp = time.strftime("%Y%m%d_%H%M%S")
                final = os.path.join(folder, "%s_%s" % (stamp, name))
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
                                self.progress("%s %3d%% (%d/%d bytes)"
                                              % (name, pct, got, size))
                                last_pct = pct
                finally:
                    if os.path.exists(tmp):
                        try:
                            os.remove(tmp)
                        except OSError:
                            pass
                if ok and got == size:
                    os.replace(tmp, final)
                    conn.sendall(b"OK")
                    self.files_received += 1
                    self.stat2_var.set("已收文件: %d" % self.files_received)
                    self.log("[tcp] 保存完成: %s" % final)
                    self.file_list.insert(0, "%s  (%d bytes)" % (os.path.basename(final), size))
                else:
                    conn.sendall(b"NG")
                    self.log("[tcp] 接收不完整 (%d/%d)，ESP32 将重传" % (got, size))
        except (ConnectionResetError, socket.timeout, OSError) as e:
            self.log("[tcp] 连接断开 (%s): %s" % (peer, e))
        finally:
            conn.close()
            self.progress("")
            self.status("等待 ESP32 连接...")

    # ---------- 配置操作 ----------
    def pick_folder(self):
        d = filedialog.askdirectory(initialdir=self.folder_var.get() or base_dir())
        if d:
            self.folder_var.set(os.path.normpath(d))

    def apply_config(self):
        try:
            port = int(self.port_var.get())
        except ValueError:
            messagebox.showerror("错误", "端口必须是数字")
            return
        folder = self.folder_var.get().strip()
        if not folder:
            folder = base_dir()
            self.folder_var.set(folder)
        if not os.path.isdir(folder):
            os.makedirs(folder)
        save_config(port, folder)
        self.log("配置已保存：端口 %d，文件夹 %s（正在重启服务...）" % (port, folder))
        self.stop_servers()
        time.sleep(0.2)
        self.start_servers(port, folder)

    def on_close(self):
        self.stop_servers()
        self.root.destroy()


def main():
    port, folder = load_config()
    save_config(port, folder)          # 首次运行生成默认 ini
    root = tk.Tk()
    app = App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
