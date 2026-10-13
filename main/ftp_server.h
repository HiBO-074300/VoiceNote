#pragma once

#include "esp_err.h"

/**
 * @brief 启动 FTP 服务器（被动模式 PASV）
 *
 * 支持命令：USER PASS SYST FEAT PWD CWD CDUP TYPE PASV PORT
 *           SIZE MDTM RETR STOR DELE RNFR RNTO MKD RMD LIST NLST NOOP OPTS QUIT
 * 根目录锁定在 root_path，客户端无法用 ../ 逃出该目录
 *
 * @param root_path  虚拟根目录，例如 "/sdcard"
 * @return ESP_OK 表示监听已启动
 */
esp_err_t ftp_server_start(const char *root_path);
