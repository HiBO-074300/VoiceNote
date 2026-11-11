#pragma once

/* 启动自动上传任务（发现上位机 → TCP 上传录音 → 确认后删除）。
   必须在 esp_netif_init()（lwIP 就绪）之后调用 */
void uploader_start(void);
