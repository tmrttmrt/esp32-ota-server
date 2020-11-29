#include <string.h>

#include "esp_system.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "ota_server.h"

static const char * TAG = "OTA";

/*socket*/
static int server_socket = 0;

static int get_socket_error_code(int socket)
{
    int result;
    u32_t optlen = sizeof(int);
    int err = getsockopt(socket, SOL_SOCKET, SO_ERROR, &result, &optlen);
    if (err == -1) {
        ESP_LOGE(TAG, "getsockopt failed:%s", strerror(err));
        return -1;
    }
    return result;
}

static int show_socket_error_reason(const char *str, int socket)
{
    int err = get_socket_error_code(socket);

    if (err != 0) {
        ESP_LOGW(TAG, "%s socket error %d %s", str, err, strerror(err));
    }

    return err;
}

static esp_err_t create_tcp_server()
{
    ESP_LOGI(TAG, "server socket....port=%d", OTA_LISTEN_PORT);
    struct sockaddr_in server_addr;
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        show_socket_error_reason("create_server", server_socket);
        return ESP_FAIL;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(OTA_LISTEN_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        show_socket_error_reason("bind_server", server_socket);
        close(server_socket);
        return ESP_FAIL;
    }
    if (listen(server_socket, 5) < 0) {
        show_socket_error_reason("listen_server", server_socket);
        close(server_socket);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void ota_server_start()
{
    int connect_socket = 0;
    struct sockaddr_in client_addr;
    unsigned int socklen = sizeof(client_addr);
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *factory_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                         ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);

    ESP_ERROR_CHECK( create_tcp_server() );
    if(NULL==update_partition){
        ESP_LOGI(TAG, "Cannot find update partition!");
        return;
    }
    if(NULL==factory_partition){
        ESP_LOGI(TAG, "Cannot find factory partition!");
        return;
    }   
    while(true){
        ESP_LOGI(TAG, "Listening for connection ...");
        connect_socket = accept(server_socket, (struct sockaddr *)&client_addr, &socklen);
        if (connect_socket < 0) {
            show_socket_error_reason("accept_server", connect_socket);
            continue;
        }
        /*connection established，now can send/recv*/
        ESP_LOGI(TAG, "tcp connection established!");
        bool is_req_body_started = false;
        int content_length = -1;
        int content_received = 0;    
        int recv_len;
        char ota_buff[OTA_BUFF_SIZE] = {0};
        char *pbuff = ota_buff;
        int buff_len = OTA_BUFF_SIZE;
        char res_buff[60];
        int send_len;
        const char *cmd = "factory";
        const char * ct_tp_start_p = "text/parameters";
        char *content_type_start_p = 0;
        bool ota_flag = false;
        esp_ota_handle_t ota_handle; 
        do {
            recv_len = recv(connect_socket, pbuff, buff_len, 0);
            if (recv_len > 0) {
                if (!is_req_body_started) {
                    const char *content_length_start = "Content-Length: ";
                    char *content_length_start_p = strstr(ota_buff, content_length_start) + strlen(content_length_start);
                    sscanf(content_length_start_p, "%d", &content_length);
                    ESP_LOGI(TAG, "Detected content length: %d", content_length);
                    const char *content_type_start = "Content-Type: ";
                    content_type_start_p = strstr(ota_buff, content_type_start) + strlen(content_type_start);
                    const char *header_end = "\r\n\r\n";
                    char *body_start_p = strstr(ota_buff, header_end) + strlen(header_end);
                    int body_part_len = recv_len - (body_start_p - ota_buff);
                    const char * ct_aos_start_p = "application/octet-stream";
                    if (!memcmp(content_type_start_p,ct_aos_start_p,strlen(ct_aos_start_p))){
                        ota_flag=true;
                        ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
                                    update_partition->subtype, update_partition->address);
                        ESP_ERROR_CHECK( esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle) );
                        esp_ota_write(ota_handle, body_start_p, body_part_len);
                        
                    }
                    else if(!memcmp(content_type_start_p,ct_aos_start_p,strlen(ct_aos_start_p))){
                        memcpy(ota_buff, body_start_p, body_part_len);
                        pbuff += body_part_len;
                        buff_len-=body_part_len;
                    }
                    content_received += body_part_len;
                    is_req_body_started = true;
                } else {
                    if (ota_flag){
                        esp_ota_write(ota_handle, ota_buff, recv_len);
                    } else {
                        buff_len -= recv_len;  
                    }
                    content_received += recv_len;
                }
            }
            ESP_LOGI(TAG, "Received: %d%%", (content_received*100)/content_length);
        } while (recv_len > 0 && content_received < content_length && 
                    (ota_flag || (content_received < strlen(cmd))));

        if(recv_len < 0){
            ESP_LOGE(TAG, "Error: recv data error! errno=%d", errno);
            send_len=sprintf(res_buff, "400 Bad Request\n\nFailure. Error code: 0x%x\n", errno);
            send(connect_socket, res_buff, send_len, 0);
            close(connect_socket);
            if(ota_flag){
                esp_err_t err = esp_ota_end(ota_handle);
                if (ESP_OK != err){
                    ESP_LOGE(TAG, "OTA update failed: error code: 0x%x", err);
                }
            }
            continue;
        }
        if (ota_flag){
            ESP_LOGI(TAG, "Binary transferred finished: %d bytes", content_received);

            esp_err_t err = esp_ota_end(ota_handle);
            if (ESP_OK == err){
                err = esp_ota_set_boot_partition(update_partition);
            }

            if (err == ESP_OK) {
                send_len = sprintf(res_buff, "200 OK\n\nSuccess. Next boot partition is %s\n", update_partition->label);
                send(connect_socket, res_buff, send_len, 0);
                close(connect_socket);

                const esp_partition_t *boot_partition = esp_ota_get_boot_partition();
                ESP_LOGI(TAG, "Next boot partition subtype %d at offset 0x%x",
                        boot_partition->subtype, boot_partition->address);
                ESP_LOGI(TAG, "Prepare to restart system!");
                vTaskDelay(2000/ portTICK_PERIOD_MS);
                esp_restart();
            } else {
                ESP_LOGE(TAG, "OTA update failed: error code: 0x%x", err);
                send_len = sprintf(res_buff, "400 Bad Request\n\nFailure. Error code: 0x%x\n", err);
            }
            send(connect_socket, res_buff, send_len, 0);
            close(connect_socket);
        }
        else {
            if(!memcmp(content_type_start_p,ct_tp_start_p,strlen(ct_tp_start_p)) && strstr(ota_buff, cmd)){
                esp_err_t err = esp_ota_set_boot_partition(factory_partition);
                if (err == ESP_OK) {
                    send_len = sprintf(res_buff, "200 OK\n\nSuccess. Next boot partition is %s\n", 
                                    factory_partition->label);
                    send(connect_socket, res_buff, send_len, 0);
                    close(connect_socket);
                    ESP_LOGI(TAG, "Rebooting from factory partition ...");
                    vTaskDelay(2000/ portTICK_PERIOD_MS);
                    esp_restart();
                                    
                } else {
                    send_len = sprintf(res_buff, "400 Bad Request\n\nFailure. Error code: 0x%x\n", err);
                    send(connect_socket, res_buff, send_len, 0);
                    ESP_LOGE(TAG, "Failed to set factory boot partition. Error code: 0x%x\n", err);
               }
            } 
            close(connect_socket);
        }
    }
}
