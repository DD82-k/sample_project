/*
 * comm_ap.c — WiFi AP + TCP Server 实现
 *
 * ==================== 工作原理 ====================
 *
 * 1. 本 ESP32 开启 SoftAP 模式，成为一个 WiFi 热点
 *    ┌─────────────┐         ┌─────────────┐
 *    │  本 ESP32    │  WiFi  │  另一 ESP32  │
 *    │  (AP 热点)  │◄───────│  (STA 客户端) │
 *    │  TCP Server │  连接   │  TCP Client  │
 *    └─────────────┘         └─────────────┘
 *
 * 2. TCP Server 监听指定端口，等待客户端连接
 * 3. 客户端连接后，TCP Server 接收数据 → 回调通知上层
 * 4. 上层也可以通过 comm_ap_send() 发送数据给客户端
 *
 * ==================== WiFi 模式说明 ====================
 *
 * ESP32 WiFi 有两种主要模式:
 *   STA 模式: 像手机一样连接路由器
 *   AP 模式:  自己成为热点，让其他设备连接
 *
 * 这里用 AP 模式，不需要路由器，两块 ESP32 直连。
 * 最大连接数设为 1（只接受一个客户端）。
 *
 * ==================== TCP 协议说明 ====================
 *
 * TCP 是面向连接的可靠传输协议:
 *   - 三次握手建立连接
 *   - 数据按序到达，不丢包
 *   - 适合传输音频、文件等需要完整性的数据
 *
 * FreeRTOS 多任务模型:
 *   - WiFi 事件由系统事件循环处理
 *   - TCP Server 在自己的任务中运行，阻塞等待连接和数据
 *   - 收到数据时通过回调通知调用者
 *
 * ==================== 音频传输流程设想 ====================
 *
 * 另一 ESP32 采集音频 → TCP 发送 → 本 ESP32 TCP Server 接收
 * → 回调 → VAD → ASR → 显示文字
 *
 * 这样麦克风可以放在远处，手表只负责处理和显示。
 */

#include "comm_ap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"   /* BSD socket API, like Linux */
#include "lwip/netdb.h"
#include "esp_mac.h"         /* MACSTR / MAC2STR macros */
#include <string.h>

static const char *TAG = "comm_ap";

/* ---- 全局状态 ---- */
static int           server_sock  = -1;   /* TCP server socket */
static int           client_sock  = -1;   /* 已连接的客户端 socket */
static TaskHandle_t  server_task  = NULL; /* TCP 服务器任务句柄 */
static comm_ap_recv_cb_t recv_cb  = NULL; /* 数据回调 */
static void         *recv_ctx     = NULL; /* 回调上下文 */

/*
 * ==================== WiFi 事件处理 ====================
 *
 * ESP-IDF 的 WiFi 驱动通过事件循环通知状态变化。
 * AP 模式关键事件:
 *   WIFI_EVENT_AP_START         — AP 启动成功
 *   WIFI_EVENT_AP_STACONNECTED  — 有设备连上我们的 AP
 *   WIFI_EVENT_AP_STADISCONNECTED — 设备断开
 */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_AP_START) {
            ESP_LOGI(TAG, "AP started — 热点已开启");

        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            /* 有设备连上了 */
            wifi_event_ap_staconnected_t *evt = data;
            ESP_LOGI(TAG, "STA connected: " MACSTR, MAC2STR(evt->mac));

        } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
            /* 设备断开 */
            wifi_event_ap_stadisconnected_t *evt = data;
            ESP_LOGI(TAG, "STA disconnected: " MACSTR, MAC2STR(evt->mac));
            /* 关闭旧连接，准备接受新连接 */
            if (client_sock >= 0) {
                close(client_sock);
                client_sock = -1;
            }
        }
    }
}

/*
 * ==================== TCP Server 任务 ====================
 *
 * 独立 FreeRTOS 任务，循环执行:
 *   1. accept() — 阻塞等待客户端连接（TCP 三次握手）
 *   2. recv()  — 阻塞等待客户端发数据
 *   3. 回调    — 把数据交给上层
 *
 * socket API 和 Linux 一模一样:
 *   socket() → bind() → listen() → accept() → recv()/send() → close()
 */
static void tcp_server_task(void *arg)
{
    /*
     * 第一步: 创建 socket
     *
     * AF_INET      = IPv4
     * SOCK_STREAM  = TCP（如果是 SOCK_DGRAM 就是 UDP）
     * IPPROTO_TCP  = 明确指定 TCP 协议
     */
    server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        vTaskDelete(NULL);
        return;
    }

    /*
     * 第二步: 绑定地址和端口
     *
     * INADDR_ANY = 监听所有网络接口（AP 的 IP 通常是 192.168.4.1）
     * htons()    = 把端口号从主机字节序转成网络字节序（大端）
     */
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)(uintptr_t)arg), /* 端口号 */
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        goto cleanup;
    }

    /*
     * 第三步: 开始监听
     *
     * listen() 后 socket 进入被动模式，等待客户端 connect()
     * 第二个参数是 backlog — 等待队列最大长度，这里设为 1（只接受一个）
     */
    if (listen(server_sock, 1) < 0) {
        ESP_LOGE(TAG, "listen() failed");
        goto cleanup;
    }
    ESP_LOGI(TAG, "TCP server listening on port %d", (int)(uintptr_t)arg);

    /*
     * 第四步: 主循环 — 接受连接 + 接收数据
     */
    uint8_t rx_buf[2048]; /* 接收缓冲区 */

    while (1) {
        /*
         * accept() 阻塞等待客户端连接。
         * 第二、三个参数可以拿到客户端地址（这里设为 NULL 不需要）。
         * 返回一个新的 socket 专用于和这个客户端通信。
         */
        ESP_LOGI(TAG, "Waiting for TCP client...");
        client_sock = accept(server_sock, NULL, NULL);
        if (client_sock < 0) {
            ESP_LOGE(TAG, "accept() failed");
            continue;
        }
        ESP_LOGI(TAG, "TCP client connected");

        /*
         * 连接建立后循环接收数据
         * recv() 阻塞等待数据到达
         * 返回 0 = 对方关闭连接
         * 返回 <0 = 错误
         * 返回 >0 = 收到的字节数
         */
        while (1) {
            int len = recv(client_sock, rx_buf, sizeof(rx_buf), 0);
            if (len <= 0) {
                ESP_LOGI(TAG, "TCP client disconnected");
                break;
            }
            ESP_LOGI(TAG, "TCP recv %d bytes", len);

            /* 通过回调通知上层 */
            if (recv_cb) {
                recv_cb(rx_buf, len, recv_ctx);
            }
        }

        /* 清理旧连接，回到 accept() 等待下一个客户端 */
        close(client_sock);
        client_sock = -1;
    }

cleanup:
    if (server_sock >= 0) {
        close(server_sock);
        server_sock = -1;
    }
    vTaskDelete(NULL);
}

/*
 * ==================== 公共 API ====================
 */

esp_err_t comm_ap_start(const char *ssid, const char *password,
                        uint16_t port, comm_ap_recv_cb_t cb, void *user_ctx)
{
    /* 保存回调 */
    recv_cb  = cb;
    recv_ctx = user_ctx;

    /*
     * 初始化 WiFi 底层
     * nvs_flash: WiFi 配置存在 NVS 分区里（即使没有也能工作）
     * esp_netif: 网络接口抽象层
     * esp_event: 事件循环
     */
    nvs_flash_init(); /* 可重复调用 */
    esp_netif_init();
    esp_event_loop_create_default();

    /*
     * 创建 AP 的网络接口
     * esp_netif_create_default_wifi_ap() 内部会:
     *   1. 创建 netif 接口
     *   2. 启动 DHCP Server（给连上的设备分配 IP）
     *   3. 设置默认 IP 为 192.168.4.1
     */
    esp_netif_create_default_wifi_ap();

    /* 注册 WiFi 事件回调 */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               wifi_event_handler, NULL);

    /* 配置 WiFi 为 AP 模式 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_AP);

    /*
     * 设置 AP 参数
     *
     * authmode:
     *   密码 >= 8 位 → WPA2_PSK 加密
     *   密码为空     → OPEN 开放网络
     *
     * max_connection: 最大连接数，设为 1（只接受一个 ESP32）
     * channel: WiFi 信道（1-13），0 表示自动选择
     */
    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(ssid);
    ap_cfg.ap.max_connection = 1;
    ap_cfg.ap.channel = 0;

    if (password && strlen(password) >= 8) {
        strncpy((char *)ap_cfg.ap.password, password, sizeof(ap_cfg.ap.password) - 1);
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_LOGI(TAG, "AP: %s (WPA2)", ssid);
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGI(TAG, "AP: %s (OPEN)", ssid);
    }

    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();
    ESP_LOGI(TAG, "WiFi AP starting...");

    /*
     * 创建 TCP Server 任务
     *
     * 栈大小 4096 字节，优先级 4。
     * 参数传入端口号（通过指针强转，32 位足够装 16 位端口号）。
     */
    BaseType_t ret = xTaskCreate(tcp_server_task, "tcp_srv", 4096,
                                 (void *)(uintptr_t)port, 4, &server_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TCP server task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t comm_ap_send(const uint8_t *data, int len)
{
    /*
     * 向已连接的 TCP 客户端发送数据
     * send() 和 recv() 相反，把数据推给对方
     */
    if (client_sock < 0) {
        return ESP_FAIL; /* 没有客户端连接 */
    }
    int sent = send(client_sock, data, len, 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "send() failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "TCP sent %d bytes", sent);
    return ESP_OK;
}

void comm_ap_stop(void)
{
    if (client_sock >= 0) { close(client_sock); client_sock = -1; }
    if (server_sock >= 0) { close(server_sock); server_sock = -1; }
    if (server_task) { vTaskDelete(server_task); server_task = NULL; }
    esp_wifi_stop();
    esp_wifi_deinit();
    recv_cb = NULL;
}
