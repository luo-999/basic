#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "nvs_flash.h"

const char* WIFI_SSID = "514-Controller";  // 改成你的WiFi名字
const char* WIFI_PASS = "123456";       // 改成你的WiFi密码
const char* SERVER_URL = "http://47.111.75.41:3000/led";

bool got_ip = false;
char http_response[1024] = {0}; // 加大缓冲区，容纳更多引脚状态
int response_len = 0;

void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        printf("✅ 拿到IP了: " IPSTR "\n", IP2STR(&event->ip_info.ip));
        got_ip = true;
    }
}

esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (response_len < sizeof(http_response) - 1) {
                int copy_len = evt->data_len;
                if (response_len + copy_len > sizeof(http_response) - 1) {
                    copy_len = sizeof(http_response) - 1 - response_len;
                }
                memcpy(http_response + response_len, evt->data, copy_len);
                response_len += copy_len;
                http_response[response_len] = '\0';
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

void wifi_init() {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    
    wifi_config_t wifi_config = {0};
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);
    
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    printf("正在连WiFi...\n");
}

// 核心解析函数
void parse_and_set_gpio(const char* data) {
    // 数据格式: "4:1;5:0;"
    char *token;
    char *rest = (char *)data;

    while ((token = strtok_r(rest, ";", &rest))) {
        int pin, state;
        if (sscanf(token, "%d:%d", &pin, &state) == 2) {
            // 安全检查：避开 GPIO11~17（内部 Flash 使用）
            if (pin >= 11 && pin <= 17) {
                printf("⚠️ 忽略受保护的 GPIO%d\n", pin);
                continue;
            }
            
            // 动态配置引脚为输出模式
            gpio_reset_pin(pin);
            gpio_set_direction(pin, GPIO_MODE_OUTPUT);
            gpio_set_level(pin, state);
            
            printf("⚙️ 设置 GPIO%d = %d\n", pin, state);
        }
    }
}

void app_main(void)
{
    // 初始化默认已知的引脚（可选）
    gpio_reset_pin(GPIO_NUM_4);
    gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_4, 0);
    
    gpio_reset_pin(GPIO_NUM_5);
    gpio_set_direction(GPIO_NUM_5, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_5, 0);

    wifi_init();
    
    int wait_count = 0;
    while (!got_ip && wait_count < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait_count++;
    }
    
    if (!got_ip) {
        printf("❌ WiFi连接失败，重启...\n");
        esp_restart();
    }
    
    vTaskDelay(pdMS_TO_TICKS(2000));
    printf("开始轮询服务器...\n");

    while (1) {
        response_len = 0;
        http_response[0] = '\0';

        esp_http_client_config_t config = {
            .url = SERVER_URL,
            .timeout_ms = 3000,
            .event_handler = http_event_handler,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            // printf("服务器返回: %s\n", http_response); // 调试时可打开
            parse_and_set_gpio(http_response);
        } else {
            printf("请求失败: %s\n", esp_err_to_name(err));
        }
        
        esp_http_client_cleanup(client);
        vTaskDelay(pdMS_TO_TICKS(1000)); // 每秒轮询一次
    }
}