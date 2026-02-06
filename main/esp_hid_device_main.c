/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"

#include "esp_bt_defs.h"
#if CONFIG_BT_BLE_ENABLED
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#endif
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#if CONFIG_BT_SDP_COMMON_ENABLED
#include "esp_sdp_api.h"
#endif /* CONFIG_BT_SDP_COMMON_ENABLED */

#include "esp_hidd.h"
#include "esp_hid_gap.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_netif.h"
#include "esp_chip_info.h"
#include "nvs_flash.h"
#include "mdns.h"           // Ditambahkan
#include "lwip/apps/netbiosns.h" // Ditambahkan
#include "esp_http_server.h" // Ditambahkan untuk server API

// --- DEFINISI MDNS & API ---
#define MDNS_INSTANCE "esp32_rest_api"
#define MDNS_HOST_NAME "feiraved" // Ganti dengan CONFIG_EXAMPLE_MDNS_HOST_NAME jika menggunakan menuconfig
[[maybe_unused]] static const char *TAG_HTTP = "REST_API";
// ----------------------------

/* AP Configuration */
#define EXAMPLE_ESP_WIFI_AP_SSID            CONFIG_ESP_WIFI_AP_SSID
#define EXAMPLE_ESP_WIFI_AP_PASSWD          CONFIG_ESP_WIFI_AP_PASSWORD
#define EXAMPLE_ESP_WIFI_CHANNEL            CONFIG_ESP_WIFI_AP_CHANNEL


#define MAX_COMMAND_LEN 128
#define COMMAND_QUEUE_LENGTH 5
typedef struct {
    char command[MAX_COMMAND_LEN];
} ble_command_t;
QueueHandle_t ble_command_queue;


static const char *TAG_AP = "WiFi SoftAP";

// Kita tidak memerlukan s_wifi_event_group karena kita hanya menunggu koneksi klien AP, bukan STA.

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG_AP, "Station "MACSTR" joined, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
        ESP_LOGI(TAG_AP, "Station "MACSTR" left, AID=%d, reason:%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        const ip_event_assigned_ip_to_client_t *e = (const ip_event_assigned_ip_to_client_t *)event_data;
        ESP_LOGI(TAG_AP, "Assigned IP to client: " IPSTR ", MAC=" MACSTR ", hostname='%s'",
                 IP2STR(&e->ip), MAC2STR(e->mac), e->hostname);
    }
}

/* Initialize soft AP */
esp_netif_t *wifi_init_softap(void)
{
    // DHCP Server diaktifkan secara default di sini
    esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_AP_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_AP_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_AP_PASSWD,
            .max_connection = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {.required = false,},
        },
    };

    if (strlen(EXAMPLE_ESP_WIFI_AP_PASSWD) == 0) {
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    ESP_LOGI(TAG_AP, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             EXAMPLE_ESP_WIFI_AP_SSID, EXAMPLE_ESP_WIFI_AP_PASSWD, EXAMPLE_ESP_WIFI_CHANNEL);

    return esp_netif_ap;
}

// --- FUNGSI MDNS ---
static void initialise_mdns(void)
{
    mdns_init();
    mdns_hostname_set(MDNS_HOST_NAME);
    mdns_instance_name_set(MDNS_INSTANCE);

    mdns_txt_item_t serviceTxtData[] = {
        {"chip", CONFIG_IDF_TARGET},
        {"path", "/"}
    };

    ESP_ERROR_CHECK(mdns_service_add("ESP32-AP-API", "_http", "_tcp", 80, serviceTxtData,
                                     sizeof(serviceTxtData) / sizeof(serviceTxtData[0])));
}

// --- HANDLER RESTFUL API /API/INFO ---
static esp_err_t api_info_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    cJSON_AddStringToObject(root, "chip", CONFIG_IDF_TARGET);
    cJSON_AddStringToObject(root, "idf_version", IDF_VER);
    cJSON_AddNumberToObject(root, "cores", chip_info.cores);
    const char *sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((void *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}


static esp_err_t command_post_handler(httpd_req_t *req)
{
    char content[MAX_COMMAND_LEN] = {0};
    int ret = 0;
    
    // Batasi ukuran data POST untuk menghindari OOM dan Timeout
    if (req->content_len >= MAX_COMMAND_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }

    // Baca data dari body request
    int recv_len = httpd_req_recv(req, content, req->content_len);
    if (recv_len <= 0) {
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }

    // --- Pemrosesan JSON ---
    cJSON *root = cJSON_Parse(content);
    cJSON *command_json = cJSON_GetObjectItem(root, "command"); 
    
    if (command_json && cJSON_IsString(command_json)) {
        ble_command_t cmd;
        const char *command_str = cJSON_GetStringValue(command_json);
        
        // Salin perintah dan kirim ke Queue
        strlcpy(cmd.command, command_str, MAX_COMMAND_LEN);
        
        // Mengirim ke Queue. Timeout 0 (non-blocking).
        if (xQueueSend(ble_command_queue, &cmd, 0) != pdPASS) {
            ESP_LOGE(TAG_HTTP, "Queue penuh. Perintah gagal dikirim: %s", command_str);
            ret = httpd_resp_send_err(req, 503, "BLE service busy (Queue Full)");
        } else {
            ESP_LOGI(TAG_HTTP, "Perintah dikirim ke Queue: %s", command_str);
            ret = httpd_resp_sendstr(req, "{\"status\": \"OK\", \"action\": \"command queued\"}");
        }

    } else {
        ESP_LOGE(TAG_HTTP, "JSON tidak valid atau key 'command' hilang");
        ret = httpd_resp_send_err(req, 400, "Invalid JSON or missing 'command' key");
    }

    cJSON_Delete(root);
    return ret;
}

// --- FUNGSI REST SERVER ---
esp_err_t start_rest_server(const char *base_path)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    // Mulai HTTP Server
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG_HTTP, "Error starting HTTP server!");
        return ESP_FAIL;
    }

    // Daftarkan URI Handler untuk /api/info
    httpd_uri_t api_info_uri = {
        .uri      = "/api/info",
        .method   = HTTP_GET,
        .handler  = api_info_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_info_uri);

    httpd_uri_t command_post_uri = {
        .uri      = "/api/command",
        .method   = HTTP_POST,
        .handler  = command_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &command_post_uri);
    
    ESP_LOGI(TAG_HTTP, "REST Server started on %s. Access via http://%s.local/api/info", base_path, MDNS_HOST_NAME);
    return ESP_OK;
}


static const char *TAG = "EVIL_KBD";

typedef struct
{
    TaskHandle_t task_hdl;
    esp_hidd_dev_t *hid_dev;
    uint8_t protocol_mode;
    uint8_t *buffer;
} local_param_t;

static local_param_t s_ble_hid_param = {0};


#define CASE(a, b, c)  \
                case a: \
				buffer[0] = b;  \
				buffer[2] = c; \
                break;\

// USB keyboard codes
#define USB_HID_MODIFIER_LEFT_CTRL      0x01
#define USB_HID_MODIFIER_LEFT_SHIFT     0x02
#define USB_HID_MODIFIER_LEFT_ALT       0x04
#define USB_HID_MODIFIER_RIGHT_CTRL     0x10
#define USB_HID_MODIFIER_RIGHT_SHIFT    0x20
#define USB_HID_MODIFIER_RIGHT_ALT      0x40
#define USB_HID_MODIFIER_LEFT_GUI    0x08 // Tombol Windows
#define USB_HID_SPACE                   0x2C
#define USB_HID_DOT                     0x37
#define USB_HID_NEWLINE                 0x28
#define USB_HID_FSLASH                  0x38
#define USB_HID_BSLASH                  0x31
#define USB_HID_COMMA                   0x36
#define USB_HID_DOT                     0x37

const unsigned char keyboardReportMap[] = { //7 bytes input (modifiers, resrvd, keys*5), 1 byte output
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0xE0,        //   Usage Minimum (0xE0)
    0x29, 0xE7,        //   Usage Maximum (0xE7)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x03,        //   Input (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x91, 0x02,        //   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x03,        //   Output (Const,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0x00,        //   Usage Minimum (0x00)
    0x29, 0x65,        //   Usage Maximum (0x65)
    0x81, 0x00,        //   Input (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0xC0,              // End Collection

    // 65 bytes
};

static void char_to_code(uint8_t *buffer, char ch)
{
	// Check if lower or upper case
	if(ch >= 'a' && ch <= 'z')
	{
		buffer[0] = 0;
		// convert ch to HID letter, starting at a = 4
		buffer[2] = (uint8_t)(4 + (ch - 'a'));
	}
	else if(ch >= 'A' && ch <= 'Z')
	{
		// Add left shift
		buffer[0] = USB_HID_MODIFIER_LEFT_SHIFT;
		// convert ch to lower case
		ch = ch - ('A'-'a');
		// convert ch to HID letter, starting at a = 4
		buffer[2] = (uint8_t)(4 + (ch - 'a'));
	}
	else if(ch >= '0' && ch <= '9') // Check if number
	{
		buffer[0] = 0;
		// convert ch to HID number, starting at 1 = 30, 0 = 39
		if(ch == '0')
		{
			buffer[2] = 39;
		}
		else
		{
			buffer[2] = (uint8_t)(30 + (ch - '1'));
		}
	}
	else // not a letter nor a number
	{
		switch(ch)
		{
            CASE(' ', 0, USB_HID_SPACE);
			CASE('.', 0,USB_HID_DOT);
            CASE('\n', 0, USB_HID_NEWLINE);
			CASE('?', USB_HID_MODIFIER_LEFT_SHIFT, USB_HID_FSLASH);
			CASE('/', 0 ,USB_HID_FSLASH);
			CASE('\\', 0, USB_HID_BSLASH);
			CASE('|', USB_HID_MODIFIER_LEFT_SHIFT, USB_HID_BSLASH);
			CASE(',', 0, USB_HID_COMMA);
			CASE('<', USB_HID_MODIFIER_LEFT_SHIFT, USB_HID_COMMA);
			CASE('>', USB_HID_MODIFIER_LEFT_SHIFT, USB_HID_COMMA);
			CASE('@', USB_HID_MODIFIER_LEFT_SHIFT, 31);
			CASE('!', USB_HID_MODIFIER_LEFT_SHIFT, 30);
			CASE('#', USB_HID_MODIFIER_LEFT_SHIFT, 32);
			CASE('$', USB_HID_MODIFIER_LEFT_SHIFT, 33);
			CASE('%', USB_HID_MODIFIER_LEFT_SHIFT, 34);
			CASE('^', USB_HID_MODIFIER_LEFT_SHIFT,35);
			CASE('&', USB_HID_MODIFIER_LEFT_SHIFT, 36);
			CASE('*', USB_HID_MODIFIER_LEFT_SHIFT, 37);
			CASE('(', USB_HID_MODIFIER_LEFT_SHIFT, 38);
			CASE(')', USB_HID_MODIFIER_LEFT_SHIFT, 39);
			CASE('-', 0, 0x2D);
			CASE('_', USB_HID_MODIFIER_LEFT_SHIFT, 0x2D);
			CASE('=', 0, 0x2E);
			CASE('+', USB_HID_MODIFIER_LEFT_SHIFT, 39);
			CASE(8, 0, 0x2A); // backspace
			CASE('\t', 0, 0x2B);
			default:
				buffer[0] = 0;
				buffer[2] = 0;
		}
	}
}


int get_modifier_tag(const char *str, uint8_t *mod) {
    if (str[0] != '[') return 0;

    if (strncmp(str, "[CTRL]", 6) == 0)  { *mod = 0x01; return 6; }
    if (strncmp(str, "[SHIFT]", 7) == 0) { *mod = 0x02; return 7; }
    if (strncmp(str, "[ALT]", 5) == 0)   { *mod = 0x04; return 5; }
    if (strncmp(str, "[GUI]", 5) == 0)   { *mod = 0x08; return 5; } // Tombol Windows
    
    return 0;
}

void ble_hid_demo_task_kbd(void *pvParameters)
{
    ble_command_t received_cmd;
    uint8_t buffer[8];

    ESP_LOGI(TAG, "HID Keyboard Task Running with Shortcut Support...");

    while (1) {
        if (xQueueReceive(ble_command_queue, &received_cmd, portMAX_DELAY) == pdPASS) {
            
            int i = 0;
            const char *cmd = received_cmd.command;

            while (cmd[i] != '\0') {
                memset(buffer, 0, 8);
                uint8_t modifier = 0;
                
                // Cek apakah ada tag modifier seperti [GUI]
                int tag_len = get_modifier_tag(&cmd[i], &modifier);

                if (tag_len > 0) {
                    i += tag_len; // Loncat ke karakter setelah tag
                    
                    if (cmd[i] != '\0') {
                        char_to_code(buffer, cmd[i]);
                        buffer[0] |= modifier; 
                    } else {
                        buffer[0] = modifier;
                    }
                } else {
                    char_to_code(buffer, cmd[i]);
                }

                // --- EKSEKUSI TEKAN ---
                esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, 0, 1, buffer, 8);
                vTaskDelay(pdMS_TO_TICKS(10)); // Non-blocking delay

                // --- EKSEKUSI LEPAS ---
                memset(buffer, 0, 8);
                esp_hidd_dev_input_set(s_ble_hid_param.hid_dev, 0, 1, buffer, 8);
                vTaskDelay(pdMS_TO_TICKS(10)); 

                if (cmd[i] != '\0') i++;
            }
        }
    }
}

static esp_hid_raw_report_map_t ble_report_maps[] = {
#if !CONFIG_BT_NIMBLE_ENABLED || CONFIG_EXAMPLE_HID_DEVICE_ROLE == 1

    {
        .data = keyboardReportMap,
        .len = sizeof(keyboardReportMap)
    },
#endif
};

static esp_hid_device_config_t ble_hid_config = {
    .vendor_id          = 0x16C0,
    .product_id         = 0x05DF,
    .version            = 0x0100,
    .device_name        = "Evil Keyboard",
    .manufacturer_name  = "Espressif",
    .serial_number      = "1234567890",
    .report_maps        = ble_report_maps,
    .report_maps_len    = 1
};



void ble_hid_task_start_up(void)
{
    if (s_ble_hid_param.task_hdl) {
        // Task already exists
        return;
    }
#if !CONFIG_BT_NIMBLE_ENABLED
    /* Executed for bluedroid */
    xTaskCreate(ble_hid_demo_task_kbd, "ble_hid_demo_task_kbd", 3 * 1024, NULL, configMAX_PRIORITIES - 3,
                &s_ble_hid_param.task_hdl);
#endif
}

void ble_hid_task_shut_down(void)
{
    if (s_ble_hid_param.task_hdl) {
        vTaskDelete(s_ble_hid_param.task_hdl);
        s_ble_hid_param.task_hdl = NULL;
    }
}

static void ble_hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;
    static const char *TAG = "HID_DEV_BLE";

    switch (event) {
    case ESP_HIDD_START_EVENT: {
        ESP_LOGI(TAG, "START");
        esp_hid_ble_gap_adv_start();
        break;
    }
    case ESP_HIDD_CONNECT_EVENT: {
        ESP_LOGI(TAG, "CONNECT");
        break;
    }
    case ESP_HIDD_PROTOCOL_MODE_EVENT: {
        ESP_LOGI(TAG, "PROTOCOL MODE[%u]: %s", param->protocol_mode.map_index, param->protocol_mode.protocol_mode ? "REPORT" : "BOOT");
        break;
    }
    case ESP_HIDD_CONTROL_EVENT: {
        ESP_LOGI(TAG, "CONTROL[%u]: %sSUSPEND", param->control.map_index, param->control.control ? "EXIT_" : "");
        if (param->control.control)
        {
            // exit suspend
            ble_hid_task_start_up();
        } else {
            // suspend
            ble_hid_task_shut_down();
        }
    break;
    }
    case ESP_HIDD_OUTPUT_EVENT: {
        ESP_LOGI(TAG, "OUTPUT[%u]: %8s ID: %2u, Len: %d, Data:", param->output.map_index, esp_hid_usage_str(param->output.usage), param->output.report_id, param->output.length);
        ESP_LOG_BUFFER_HEX(TAG, param->output.data, param->output.length);
        break;
    }
    case ESP_HIDD_FEATURE_EVENT: {
        ESP_LOGI(TAG, "FEATURE[%u]: %8s ID: %2u, Len: %d, Data:", param->feature.map_index, esp_hid_usage_str(param->feature.usage), param->feature.report_id, param->feature.length);
        ESP_LOG_BUFFER_HEX(TAG, param->feature.data, param->feature.length);
        break;
    }
    case ESP_HIDD_DISCONNECT_EVENT: {
        ESP_LOGI(TAG, "DISCONNECT: %s", esp_hid_disconnect_reason_str(esp_hidd_dev_transport_get(param->disconnect.dev), param->disconnect.reason));
        ble_hid_task_shut_down();
        esp_hid_ble_gap_adv_start();
        break;
    }
    case ESP_HIDD_STOP_EVENT: {
        ESP_LOGI(TAG, "STOP");
        break;
    }
    default:
        break;
    }
    return;
}



#if CONFIG_BT_SDP_COMMON_ENABLED
static void esp_sdp_cb(esp_sdp_cb_event_t event, esp_sdp_cb_param_t *param)
{
    switch (event) {
    case ESP_SDP_INIT_EVT:
        ESP_LOGI(TAG, "ESP_SDP_INIT_EVT: status:%d", param->init.status);
        if (param->init.status == ESP_SDP_SUCCESS) {
            esp_bluetooth_sdp_dip_record_t dip_record = {
                .hdr =
                    {
                        .type = ESP_SDP_TYPE_DIP_SERVER,
                    },
                .vendor           = bt_hid_config.vendor_id,
                .vendor_id_source = ESP_SDP_VENDOR_ID_SRC_BT,
                .product          = bt_hid_config.product_id,
                .version          = bt_hid_config.version,
                .primary_record   = true,
            };
            esp_sdp_create_record((esp_bluetooth_sdp_record_t *)&dip_record);
        }
        break;
    case ESP_SDP_DEINIT_EVT:
        ESP_LOGI(TAG, "ESP_SDP_DEINIT_EVT: status:%d", param->deinit.status);
        break;
    case ESP_SDP_SEARCH_COMP_EVT:
        ESP_LOGI(TAG, "ESP_SDP_SEARCH_COMP_EVT: status:%d", param->search.status);
        break;
    case ESP_SDP_CREATE_RECORD_COMP_EVT:
        ESP_LOGI(TAG, "ESP_SDP_CREATE_RECORD_COMP_EVT: status:%d, handle:0x%x", param->create_record.status,
                 param->create_record.record_handle);
        break;
    case ESP_SDP_REMOVE_RECORD_COMP_EVT:
        ESP_LOGI(TAG, "ESP_SDP_REMOVE_RECORD_COMP_EVT: status:%d", param->remove_record.status);
        break;
    default:
        break;
    }
}
#endif /* CONFIG_BT_SDP_COMMON_ENABLED */




void app_main(void)
{
    esp_err_t ret;
    
    // --- 1. INISIALISASI NVS, Netif, Event Loop ---
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // --- 2. INISIALISASI QUEUE BARU (JEMBATAN KOMUNIKASI) ---
    // Pastikan MAX_COMMAND_LEN dan ble_command_t dideklarasikan secara global
    ble_command_queue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(ble_command_t));
    if (ble_command_queue == NULL) {
        ESP_LOGE(TAG, "Gagal membuat Queue perintah BLE!");
        return;
    }
    ESP_LOGI(TAG, "FreeRTOS Command Queue initialized.");
    
    // --- 3. INIALISASI WI-FI SOFTAP & MDNS/NETBIOS ---
    
    /* Register Event handler */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID,
                    &wifi_event_handler,
                    NULL,
                    NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                    IP_EVENT_ASSIGNED_IP_TO_CLIENT, // Event SoftAP/DHCP Server
                    &wifi_event_handler,
                    NULL,
                    NULL));

    initialise_mdns();
    netbiosns_init();
    netbiosns_set_name(MDNS_HOST_NAME);

    /*Initialize WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    /* Initialize AP */
    ESP_LOGI(TAG_AP, "ESP_WIFI_MODE_AP");
    esp_netif_t *esp_netif_ap = wifi_init_softap();


    // --- 4. INISIALISASI BLUETOOTH HID (LOGIKA ASLI ANDA) ---
#if HID_DEV_MODE == HIDD_IDLE_MODE
    ESP_LOGE(TAG, "Please turn on BT HID device or BLE!");
    return;
#endif

    ESP_LOGI(TAG, "setting hid gap, mode:%d", HID_DEV_MODE);
    ret = esp_hid_gap_init(HID_DEV_MODE);
    ESP_ERROR_CHECK( ret );

#if CONFIG_BT_BLE_ENABLED || CONFIG_BT_NIMBLE_ENABLED
    ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD, ble_hid_config.device_name);
    ESP_ERROR_CHECK( ret );
#if CONFIG_BT_BLE_ENABLED
    if ((ret = esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler)) != ESP_OK) {
        ESP_LOGE(TAG, "GATTS register callback failed: %d", ret);
        return;
    }
#endif
    ESP_LOGI(TAG, "setting ble device");
    ESP_ERROR_CHECK(
        esp_hidd_dev_init(&ble_hid_config, ESP_HID_TRANSPORT_BLE, ble_hidd_event_callback, &s_ble_hid_param.hid_dev));
#endif

#if CONFIG_BT_HID_DEVICE_ENABLED
    ESP_LOGI(TAG, "setting device name");
    esp_bt_gap_set_device_name(bt_hid_config.device_name);
    ESP_LOGI(TAG, "setting cod major, peripheral");
    esp_bt_cod_t cod = {0};
    cod.major = ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
    cod.minor = ESP_BT_COD_MINOR_PERIPHERAL_POINTING;
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_MAJOR_MINOR);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "setting bt device");
    ESP_ERROR_CHECK(
        esp_hidd_dev_init(&bt_hid_config, ESP_HID_TRANSPORT_BT, bt_hidd_event_callback, &s_bt_hid_param.hid_dev));
#if CONFIG_BT_SDP_COMMON_ENABLED
    ESP_ERROR_CHECK(esp_sdp_register_callback(esp_sdp_cb));
    ESP_ERROR_CHECK(esp_sdp_init());
#endif /* CONFIG_BT_SDP_COMMON_ENABLED */
#endif /* CONFIG_BT_HID_DEVICE_ENABLED */


    // --- 5. MULAI LAYANAN ---
    
    /* Start WiFi */
    ESP_ERROR_CHECK(esp_wifi_start() );
    
    // Tetapkan AP sebagai antarmuka default
    esp_netif_set_default_netif(esp_netif_ap);

    // Mulai Server RESTful API (Termasuk handler /command POST)
    ESP_ERROR_CHECK(start_rest_server("")); 
}
