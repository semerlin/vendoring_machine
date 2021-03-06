/**
* This file is part of the vendoring machine project.
*
* Copyright 2018, Huang Yang <elious.huang@gmail.com>. All rights reserved.
*
* See the COPYING file for the terms of usage and distribution.
*/
#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "esp8266.h"
#include "serial.h"
#include "global.h"
#include "trace.h"
#include "pinconfig.h"
#include "dbgserial.h"

#undef __TRACE_MODULE
#define __TRACE_MODULE "[esp8266]"

//#define _PRINT_DETAIL 

/* mqtt driver */
static esp8266_driver g_driver;

static TaskHandle_t task_esp8266 = NULL;

/* esp8266 work in block mode */
static struct
{
    const char *status_str;
    uint8_t code;
}status_code[] = 
{
    {"OK", ESP_ERR_OK},
    {"FAIL", ESP_ERR_FAIL},
    {"ERROR", ESP_ERR_FAIL},
    {"ALREADY CONNECTED", ESP_ERR_ALREADY},
    {"SEND OK", ESP_ERR_OK},
};

typedef enum
{
    mode_at,
    mode_tcp_head,
    mode_tcp_data,
}work_mode;

static work_mode g_curmode = mode_at;

/* serial handle */
static serial *g_serial = NULL;

/* recive queue */
static xQueueHandle xStatusQueue = NULL;
static xQueueHandle xTcpQueue = NULL;
static xQueueHandle xAtQueue = NULL;

#define ESP_MAX_NODE_NUM              (6)
#define ESP_MAX_MSG_SIZE_PER_LINE     (64)
#define ESP_MAX_CONNECT_NUM           (5)

typedef struct
{
    uint8_t id;
    uint16_t size;
    uint8_t data[ESP_MAX_MSG_SIZE_PER_LINE];
}tcp_node;

/* timeout time(ms) */
#define DEFAULT_TIMEOUT      (3000 / portTICK_PERIOD_MS)

/**
 * @brief connedted default process function
 */
static void esp8266_ap_connect(void)
{
}

/**
 * @brief connedted default process function
 */
static void esp8266_ap_disconnect(void)
{
}

/**
 * @brief connedted default process function
 */
static void esp8266_server_connect(uint8_t id)
{
    UNUSED(id);
}

/**
 * @brief connedted default process function
 */
static void esp8266_server_disconnect(uint8_t id)
{
    UNUSED(id);
}

/**
 * @brief initialize esp8266 default driver
 */
static void init_esp8266_driver(void)
{
    g_driver.ap_connect = esp8266_ap_connect;
    g_driver.ap_disconnect = esp8266_ap_disconnect;
    g_driver.server_connect = esp8266_server_connect;
    g_driver.server_disconnect = esp8266_server_disconnect;
}

/**
 * @brief send at command
 * @param cmd - at command
 * @param length - command length
 */
static void send_at_cmd(const char *cmd, uint32_t length)
{
    assert_param(NULL != g_serial);
    xQueueReset(xStatusQueue);
    xQueueReset(xAtQueue);
    TRACE("send: %s", cmd);
    serial_putstring(g_serial, cmd, length);
}

/* process function type define */
typedef bool (*process_func)(const char *data, uint8_t len);

/**
 * @brief process status
 * @param data - data to process
 * @param len - data length
 */
static bool try_process_status(const char *data, uint8_t len)
{
    uint8_t status = 0;
    int count = sizeof(status_code) / (sizeof(status_code[0]));
    for (int i = 0; i < count; ++i)
    {
        if (0 == strncmp(data, status_code[i].status_str, len - 2))
        {
            status = status_code[i].code;
            xQueueSend(xStatusQueue, &status, 0);
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief parse id from data
 * @param data - data to parse
 */
static uint16_t parse_id(const char *data)
{
    const char *pdata = data;
    uint16_t val = 0;
    while ((',' != *pdata) && ('\0' != *pdata))
    {
        if (' ' == *pdata)
        {
            continue;
        }
        val *= 10;
        val += (*pdata - '0'); 
        pdata++;
    }
    
    return val;
}

/**
 * @brief process server connect
 * @param data - data to process
 * @param len - data length
 */
static bool try_process_server_connect(const char *data, uint8_t len)
{
    /* find ',' first */
    const char *pdata = data;
    uint16_t id = 0;
    while (('\0' != *pdata) && (',' != *pdata))
    {
        pdata ++;
    }

    if (*pdata != '\0')
    {
        pdata++;
        if (0 == strncmp(pdata, "CONNECT", 7))
        {
            id = parse_id(data);
            g_driver.server_connect(id);
            return TRUE;
        }
        else if (0 == strncmp(pdata, "CLOSED", 6))
        {
            id = parse_id(data);
            g_driver.server_disconnect(id);
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief process ap connect
 * @param data - data to process
 * @param len - data length
 */
static bool try_process_ap_connect(const char *data, uint8_t len)
{
    if (0 == strncmp(data, "WIFI CONNECTED", len - 2))
    {
        g_driver.ap_connect();
        return TRUE;
    }
    else if (0 == strncmp(data, "WIFI DISCONNECT", len - 2))
    {
        g_driver.ap_disconnect();
        return TRUE;
    }

    return FALSE;
}

/**
 * @brief process default 
 * @param data - data to process
 * @param len - data length
 */
static bool try_process_default(const char *data, uint8_t len)
{
    /* at command parameter */
    xQueueSend(xAtQueue, data, 0);

    return TRUE;
}

/* process functions list */
static process_func process_funcs[] = 
{
    try_process_status,
    try_process_server_connect,
    try_process_ap_connect,
    try_process_default,
    NULL
};

/**
 * @brief process line data
 * @param data - line data
 */
static void process_line(const char *data, uint8_t len)
{
    if (len > 2)
    {
        for (int i = 0; NULL != process_funcs[i]; ++i)
        {
            if (process_funcs[i](data, len))
            {
                break;
            }
        }
    }
}

/**
 * @brief process at data
 * @param data - node data
 * @param len - node length
 */
static int process_at_data(const char *data, uint8_t len)
{
    if (len >= 4)
    {
        if (0 == strncmp(data, "+IPD", 4))
        {
            /* mode changed */
            g_curmode = mode_tcp_head;
            return 1;
        }
    }
                
    if (0 == strncmp(data + len - 2, "\r\n", 2))
    {
        /* get line data */
        process_line(data, len);
        return 1;
    }

    return 0;
}

/**
 * @brief process tcp head
 * @param data - data buffer
 * @param len - data length
 * @param id - tcp link id
 * @param length - tcp data length
 */
static int process_tcp_head(const char *data, uint8_t len, uint16_t *id, 
                     uint16_t *length)
{
    assert_param(NULL != id);
    assert_param(NULL != length);
    bool calc_id = FALSE;
    bool calc_len = FALSE;
    uint16_t val = 0;
    if (':' == data[len - 1])
    {
        const char *pdata = data + 1;
        calc_id = TRUE;
        for (int i = 0; i < len - 1; ++i)
        {
           if (calc_id)
           {
               if (',' == pdata[i])
               {
                   calc_id = FALSE;
                   calc_len = TRUE;
                   *id = val;
                   val = 0;
               }
               else
               {
                   val *= 10;
                   val += (pdata[i] - '0');
               }
           }
           else if (calc_len)
           {
               if (':' == pdata[i])
               {
                   *length = val;
                   if (0 == *length)
                   {
                       g_curmode = mode_at;
                   }
                   else
                   {
                       g_curmode = mode_tcp_data;
                   }
                   return 1;
               }
               else
               {
                   val *= 10;
                   val += (pdata[i] - '0');
               }
           }
        }
    }

    return 0;
}

/**
 * @brief process reveived tcp data
 * @param data - data buffer
 * @param len - data length
 */
static int process_tcp_data(uint8_t id, char *data, uint16_t len)
{
    tcp_node node; 
    node.id = id;
    node.size = len;
    for (int i = 0; i < len; ++i)
    {
        node.data[i] = data[i];
    }
    //xQueueSend(xTcpQueue, &node, 100 / portTICK_PERIOD_MS);
    xQueueSend(xTcpQueue, &node, 0);
    return 0;
}

/**
 * @brief esp8266 data process task
 * @param serial handle
 */
static void vESP8266Response(void *pvParameters)
{
    serial *pserial = pvParameters;
    char node_data[ESP_MAX_MSG_SIZE_PER_LINE];
    uint8_t node_size = 0;
    char *pData = node_data;
    uint16_t link_id = 0;
    uint16_t tcp_size = 0;
    char data;
    TickType_t xDelay = 50 / portTICK_PERIOD_MS;
    for (;;)
    {
        if (serial_getchar(pserial, &data, portMAX_DELAY))
        {
            g_curmode = mode_at;
            link_id = 0;
            tcp_size = 0;
            node_size = 0;
            pData = node_data;
            /* receive data */
            *pData++ = data;
            node_size ++;
#ifdef _PRINT_DETAIL
            dbg_putchar(data);
#endif
            while (serial_getchar(pserial, &data, xDelay))
            {
#ifdef _PRINT_DETAIL
                dbg_putchar(data);
#endif
                /* receive data */
                *pData++ = data;
                node_size ++;
                switch (g_curmode)
                {
                case mode_at:
                    if (process_at_data(node_data, node_size) > 0)
                    {
                        pData = node_data;
                        node_size = 0;
                    }
                    break;
                case mode_tcp_head:
                    if (process_tcp_head(node_data, node_size, &link_id, 
                                         &tcp_size) > 0)
                    {
                        pData = node_data;
                        node_size = 0;
                    }
                    break;
                case mode_tcp_data:
                    tcp_size --;
                    if (0 == tcp_size)
                    {
                        process_tcp_data(link_id, node_data, node_size);
                        pData = node_data;
                        node_size = 0;
                        /* reset mode */
                        g_curmode = mode_at;
                    }
                    else
                    {
                        if (node_size >= ESP_MAX_MSG_SIZE_PER_LINE)
                        {
                            process_tcp_data(link_id, node_data, node_size);
                            pData = node_data;
                            node_size = 0;
                        }
                    }
                    break;
                default:
                    break;
                }
            }
        }
    }
}

/**
 * @brief initialize esp8266
 * @return 0 means success, otherwise error code
 */
bool esp8266_init(void)
{
    TRACE("initialize esp8266...\r\n");
    pin_set("WIFI_RST");
    pin_reset("WIFI_EN");
    vTaskDelay(100 / portTICK_PERIOD_MS);
    pin_set("WIFI_EN");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    
    g_serial = serial_request(COM2);
    if (NULL == g_serial)
    {
        TRACE("initialize failed, can't open serial \'COM2\'\r\n");
        return FALSE;
    }
    serial_open(g_serial);

    init_esp8266_driver();
    xStatusQueue = xQueueCreate(ESP_MAX_NODE_NUM, ESP_MAX_NODE_NUM);
    xAtQueue = xQueueCreate(ESP_MAX_NODE_NUM, ESP_MAX_MSG_SIZE_PER_LINE);
    xTcpQueue = xQueueCreate(ESP_MAX_NODE_NUM * 2, 
                             sizeof(tcp_node) / sizeof(char));

    if ((NULL == xStatusQueue) || 
        (NULL == xAtQueue) || 
        (NULL == xTcpQueue))
    {
        TRACE("initialize failed, can't create queue\'COM2\'\r\n");
        serial_release(g_serial);
        g_serial = NULL;
        return FALSE;
    }
    
    xTaskCreate(vESP8266Response, "ESP8266Response", ESP8266_STACK_SIZE, 
            g_serial, ESP8266_PRIORITY, &task_esp8266);
     
    return TRUE;
}

/**
 * @brief test esp8266 module
 * @param cmd - cmd to send
 * @return 0 means connect success, otherwise failed
 */
int esp8266_send_ok(const char *cmd)
{
    uint8_t status;
    send_at_cmd(cmd, strlen(cmd));
    int ret = ESP_ERR_OK;

    if (pdPASS == xQueueReceive(xStatusQueue, &status, DEFAULT_TIMEOUT))
    {
        ret = -status;
    }
    else
    {
        ret = -ESP_ERR_TIMEOUT;
    }

    if (0 != ret)
    {
        TRACE("status: %d\r\n", -ret);
    }
    return ret;
}

/**
 * @brief write data
 * @param data - data
 * @param length - data length
 */
int esp8266_write(const char *data, uint32_t length)
{
    assert_param(NULL != g_serial);
    uint8_t status;
    serial_putstring(g_serial, data, length);
    int ret = ESP_ERR_OK;

    if (pdPASS == xQueueReceive(xStatusQueue, &status, DEFAULT_TIMEOUT))
    {
        ret = -status;
    }
    else
    {
        ret = -ESP_ERR_TIMEOUT;
    }

    if (0 != ret)
    {
        TRACE("status: %d\r\n", -ret);
    }
    return ret;
}

/**
 * @brief set esp8266 work mode
 * @param mode - work mode
 * @return 0 means connect success, otherwise failed
 */
int esp8266_setmode(esp8266_mode mode)
{
    char str_mode[16];
    sprintf(str_mode, "AT+CWMODE_CUR=%d\r\n", mode);
    return esp8266_send_ok(str_mode);
}

/**
 * @brief get esp8266 mode
 * @param time - timeout time
 * @return esp8266 current work mode
 */
esp8266_mode esp8266_getmode(void)
{
    uint8_t status;
    uint8_t buf[ESP_MAX_MSG_SIZE_PER_LINE];
    buf[0] = UNKNOWN + '0';
    send_at_cmd("AT+CWMODE_CUR?\r\n", 16);
    esp8266_mode mode = UNKNOWN;

    if (pdPASS == xQueueReceive(xStatusQueue, &status, DEFAULT_TIMEOUT))
    {
        if (ESP_ERR_OK == status)
        {
            xQueueReceive(xAtQueue, buf, 0);
            mode = (esp8266_mode)(buf[0] - '0');
        }
    }

    TRACE("mode: %d\r\n", mode);
    return mode;
}

/**
 * @brief connect ap
 * @param ssid - ap ssid
 * @param pwd - ap password
 * @return 0 means connect success, otherwise failed
 */
int esp8266_connect_ap(const char *ssid, const char *pwd, TickType_t time)
{
    char str_mode[64];
    sprintf(str_mode, "AT+CWJAP_CUR=\"%s\",\"%s\"\r\n", ssid, pwd);
    send_at_cmd(str_mode, strlen(str_mode));
    int ret = ESP_ERR_OK;

    uint8_t status;
    uint8_t buf[ESP_MAX_MSG_SIZE_PER_LINE];
    buf[0] = ESP_ERR_FAIL + '0';
    if (pdPASS == xQueueReceive(xStatusQueue, &status, time))
    {
        if (ESP_ERR_OK != status)
        {
            ret = -ESP_ERR_FAIL;
            xQueueReceive(xAtQueue, buf, 0);
            for (int i = 0; i < ESP_MAX_MSG_SIZE_PER_LINE; ++i)
            {
                if (':' == buf[i])
                {
                    ret = -(buf[i + 1] - '0');
                }
            }
        }
    }
    else
    {
        ret = -ESP_ERR_TIMEOUT;
    }

    if (0 != ret)
    {
        TRACE("status: %d\r\n", -ret);
    }
    return ret;
}

/**
 * @brief set software ap parameter
 * @param ssid - ap ssid
 * @param pwd - ap password
 * @param chl - ap channel
 * @param ecn - password encode type
 * @return 0 means connect success, otherwise failed
 */
int esp8266_set_softap(const char *ssid, const char *pwd, uint8_t chl, 
                       esp8266_ecn ecn)
{
    char str_mode[64];
    sprintf(str_mode, "AT+CWSAP_CUR=\"%s\",\"%s\",%d,%d\r\n", 
            ssid, pwd, chl, ecn);
    
    return esp8266_send_ok(str_mode);
}

/**
 * @brief set software ap address
 * @param ssid - ap ssid
 * @param pwd - ap password
 * @param chl - ap channel
 * @param ecn - password encode type
 * @return 0 means connect success, otherwise failed
 */
int esp8266_set_apaddr(const char *ip, const char *gateway, const char *netmask)
{
    char str_mode[64];
    sprintf(str_mode, "AT+CIPAP_CUR=\"%s\",\"%s\",\"%s\"\r\n", 
            ip, gateway, netmask);
    
    return esp8266_send_ok(str_mode);
}

/**
 * @brief connect remote server
 * @param mode - connect mode
 * @param ip - remote ip address
 * @param port - remote port
 */
int esp8266_connect_server(uint8_t id, const char *mode, const char *ip, 
                    uint16_t port)
{
    char str_mode[64];
    sprintf(str_mode, "AT+CIPSTART=%d,\"%s\",\"%s\",%d\r\n", id, mode, 
            ip, port);
    return esp8266_send_ok(str_mode);
}

/**
 * @brief disconnect tcp,udp,ssl connection
 * @param id - link id
 * @param time - timeout time
 */
int esp8266_disconnect_server(uint8_t id)
{
    char str_mode[22];
    sprintf(str_mode, "AT+CIPCLOSE=%d\r\n", id);
    
    return esp8266_send_ok(str_mode);
}

/**
 * @brief create server
 * @param port - listen port
 * @param time - timeout time
 * @return error code
 */
int esp8266_listen(uint16_t port)
{
    char str_mode[24];
    sprintf(str_mode, "AT+CIPSERVER=1,%d\r\n", port);

    return esp8266_send_ok(str_mode);
}

/**
 * @brief close server
 * @param time - timeout time
 * @return error code
 */
int esp8266_close(uint16_t port)
{
    char str_mode[24];
    sprintf(str_mode, "AT+CIPSERVER=0,%d\r\n", port);

    return esp8266_send_ok(str_mode);
}

/**
 * @brief get tcp data
 * @param data - tcp data
 * @param len - data length
 * @param xBlockTime - timeout time
 */
int esp8266_recv(uint8_t *id, uint8_t *data, uint16_t *len, TickType_t xBlockTime)
{
    tcp_node node;
    if (xQueueReceive(xTcpQueue, &node, xBlockTime))
    {
        *id = node.id;
        for (int i = 0; i < node.size; ++i)
        {
            data[i] = node.data[i];
        }
        *len = node.size;
        return ESP_ERR_OK;
    }
    else
    {
        return -ESP_ERR_TIMEOUT;
    }
}

/**
 * @brief prepare send tcp data
 * @param chl - connected channel
 * @param length - send length
 */
int esp8266_prepare_send(uint8_t id, uint16_t length)
{
    char str_mode[20];
    sprintf(str_mode, "AT+CIPSEND=%d,%d\r\n", id, length);
    
    return esp8266_send_ok(str_mode);
}

/**
 * @brief set tcp timeout time
 * @param time - timeout time
 * @param time - timeout time
 */
int esp8266_set_tcp_timeout(uint16_t timeout)
{
    char str_mode[22];
    sprintf(str_mode, "AT+CIPSTO=%d\r\n", timeout);
    
    return esp8266_send_ok(str_mode);
}

/**
 * @brief refresh driver
 */
static void refresh_driver(void)
{
    if (NULL == g_driver.ap_connect)
    {
        g_driver.ap_connect = esp8266_ap_connect;
    }

    if (NULL == g_driver.ap_disconnect)
    {
        g_driver.ap_disconnect = esp8266_ap_disconnect;
    }
    
    if (NULL == g_driver.server_connect)
    {
        g_driver.server_connect = esp8266_server_connect;
    }

    if (NULL == g_driver.server_disconnect)
    {
        g_driver.server_disconnect = esp8266_server_disconnect;
    }
}

/**
 * @brief attach mqtt driver
 * @param driver - mqtt driver handle
 */
void esp8266_attach(const esp8266_driver *driver)
{
    g_driver.ap_connect = driver->ap_connect;
    g_driver.ap_disconnect = driver->ap_disconnect;
    g_driver.server_connect = driver->server_connect;
    g_driver.server_disconnect = driver->server_disconnect;
    refresh_driver();
}

/**
 * @brief detach mqtt driver
 */
void esp8266_detach(void)
{
    init_esp8266_driver();
}

/**
 * @brief shutdown esp8266
 */
void esp8266_shutdown(void)
{
    if (NULL != task_esp8266)
    {
        vTaskDelete(task_esp8266);
        task_esp8266 = NULL;
    }
}

