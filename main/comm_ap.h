/*
 * comm_ap.h — WiFi AP + TCP Server 通信模块
 *
 * 本 ESP32 开启 AP 热点，另一块 ESP32 连上来后通过 TCP 发送数据。
 * TCP 收到的数据通过回调函数交给上层处理。
 */
#ifndef COMM_AP_H
#define COMM_AP_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TCP 数据回调
 *   data:   收到的数据指针
 *   len:    数据长度（字节）
 *   user_ctx: 用户自定义上下文
 */
typedef void (*comm_ap_recv_cb_t)(const uint8_t *data, int len, void *user_ctx);

/*
 * 启动 AP + TCP Server
 *   ssid:     AP 热点名称
 *   password: AP 密码（长度 >= 8 才启用加密，否则开放网络）
 *   port:     TCP 监听端口
 *   cb:       收到数据时的回调
 *   user_ctx: 回调透传参数
 */
esp_err_t comm_ap_start(const char *ssid, const char *password,
                        uint16_t port, comm_ap_recv_cb_t cb, void *user_ctx);

/*
 * 向已连接的 TCP 客户端发送数据
 *   data: 发送数据
 *   len:  数据长度
 * 返回: ESP_OK 成功，ESP_FAIL 没有客户端连接
 */
esp_err_t comm_ap_send(const uint8_t *data, int len);

/*
 * 停止 AP 和 TCP Server
 */
void comm_ap_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* COMM_AP_H */
