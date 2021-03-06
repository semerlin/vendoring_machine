/**
* This file is part of the vendoring machine project.
*
* Copyright 2018, Huang Yang <elious.huang@gmail.com>. All rights reserved.
*
* See the COPYING file for the terms of usage and distribution.
*/
#include <string.h>
#include <stdio.h>
#include "mqtt.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "esp8266.h"
#include "m26.h"
#include "trace.h"
#include "global.h"
#include "assert.h"
#include "mode.h"


#undef __TRACE_MODULE
#define __TRACE_MODULE  "[mqtt]"

/* mqtt driver */
static mqtt_driver g_driver;

/* send and recive queue */
static xQueueHandle xSendQueue = NULL;
static SemaphoreHandle_t xSendMutex = NULL;

#define MQTT_MAX_MSG_NUM     (6)
#define MQTT_MAX_MSG_SIZE    (128)

typedef struct
{
    uint8_t size;
    uint8_t data[MQTT_MAX_MSG_SIZE];
}mqtt_msg;

 
/* message type definition */
#define TYPE_CONNECT        (0x10)
#define TYPE_CONNACK        (0x20)
#define TYPE_PUBLISH        (0x30)
#define TYPE_PUBACK         (0x40)
#define TYPE_PUBREC         (0x50)
#define TYPE_PUBREL         (0x62)
#define TYPE_PUBCOMP        (0x70)
#define TYPE_SUBSCRIBE      (0x82)
#define TYPE_SUBACK         (0x90)
#define TYPE_UNSUBSCRIBE    (0xa2)
#define TYPE_UNSUBACK       (0xb0)
#define TYPE_PINGREQ        (0xc0)
#define TYPE_PINGRESP       (0xd0)
#define TYPE_DISCONNECT     (0xe0)

/* uuid */
static uint16_t g_uuid = 0;
static uint8_t g_linkid = 0xff;

/* connect */
const uint8_t protocol_name[6] = {0x00, 0x04, 'M', 'Q', 'T', 'T'};
#define PROTOCOL_LEVEL    (0x04)
connect_flag default_connect_flag = {0x00};

static TaskHandle_t xMqttHandle = NULL;

/* process function */
typedef void (*process_func)(const uint8_t *data, uint8_t len);


/**
 * @brief connack default process function
 */
static void mqtt_connack_cb(uint8_t status)
{
    UNUSED(status);
}

/**
 * @brief connack default process function
 */

static void mqtt_publish_cb(const char *topic, uint8_t *content, uint32_t len)
{
    UNUSED(topic);
    UNUSED(content);
    UNUSED(len);
}

/**
 * @brief connack default process function
 */
static void mqtt_puback_cb(uint16_t id)
{
    UNUSED(id);
}

/**
 * @brief connack default process function
 */
static void mqtt_pubrec_cb(uint16_t id)
{
    UNUSED(id);
}

/**
 * @brief connack default process function
 */
static void mqtt_pubrel_cb(uint16_t id)
{
    UNUSED(id);
}

/**
 * @brief connack default process function
 */
static void mqtt_pubcomp_cb(uint16_t id)
{
    UNUSED(id);
}

/**
 * @brief connack default process function
 */
static void mqtt_suback_cb(uint8_t status, uint16_t id)
{
    UNUSED(status);
    UNUSED(id);
}

/**
 * @brief connack default process function
 */
static void mqtt_unsuback_cb(uint16_t id)
{
    UNUSED(id);
}

/**
 * @brief connack default process function
 */
static void mqtt_pingresp_cb(void)
{
}

/**
 * @brief initialize mqtt default driver
 */
static void init_mqtt_driver(void)
{
    g_driver.connack = mqtt_connack_cb;
    g_driver.publish = mqtt_publish_cb;
    g_driver.puback = mqtt_puback_cb;
    g_driver.pubrec = mqtt_pubrec_cb;
    g_driver.pubrel = mqtt_pubrel_cb;
    g_driver.pubcomp = mqtt_pubcomp_cb;
    g_driver.suback = mqtt_suback_cb;
    g_driver.unsuback = mqtt_unsuback_cb;
    g_driver.pingresp = mqtt_pingresp_cb;
}

/**
 * @brief encode data length
 * @param data - data to encode
 * @param encode - encoded length
 * @return valid size
 */
static uint8_t encode_length(uint32_t data, uint8_t *encode)
{
    uint8_t encode_byte = 0;
    uint8_t size = 0;
    do
    {
        encode_byte = data % 128;
        data /= 128;
        if (data > 0)
        {
            encode_byte |= 0x80;
        }
        else
        {
            encode[size] = encode_byte;
            size++;
        }
    }while (data > 0);

    return size;
}

/**
 * @brief decode data length
 * @param decode - data to decode
 * @param step - decode step
 * @return data length 
 */
static uint32_t decode_length(const uint8_t *decode, uint8_t *step)
{
    uint32_t multipiler = 1;
    uint32_t value = 0;
    if (NULL != step)
    {
        *step = 0;
    }

    do
    {
        decode ++;
        if (NULL != step)
        {
            *step += 1; 
        }
        value += (*decode & 0x7f) * multipiler;
        multipiler *= 128;
        if (multipiler > 128 * 128 *128)
        {
            /* error happened */
            return 0xffffffff;
        }
    }while (0 != (*decode & 0x80));

    return value;
}

/**
 * @brief process connack information
 * @param data - data to process
 * @param len - data length
 */
void process_connack(const uint8_t *data, uint8_t len)
{
    if (len >= 4)
    {
        assert_param(decode_length(data, NULL) == 2);
        g_driver.connack(data[3]);
    }
}

/**
 * @brief process puback information
 * @param data - data to process
 * @param len - data length
 */
void process_publish(const uint8_t *data, uint8_t len)
{
    if (len >= 4)
    {
        uint8_t step = 0;
        uint32_t data_len = decode_length((uint8_t *)data, &step);
        uint8_t dup = ((data[0] >> 3) & 0x01);
        UNUSED(dup); /* fix compiler warning */
        uint8_t qos = ((data[0] >> 1) & 0x03);

        char topic[42];
        uint16_t id = 0;
        uint16_t topic_len = 0;
        const uint8_t *pdata = data + step + 1;
        topic_len = *pdata;
        pdata ++;
        topic_len <<= 8;
        topic_len += *pdata;
        pdata ++;

        if (topic_len >= 42)
        {
            strncpy(topic, (const char *)pdata, 41);
            topic[41] = '\0';
        }
        else
        {
            strncpy(topic, (const char *)pdata, topic_len);
            topic[topic_len] = '\0';
        }
        pdata += topic_len;

        if ((1 == qos) || (2 == qos))
        {
            id = *pdata;
            id <<= 8;
            pdata ++;
            id += *pdata;
            pdata ++;
            len -= 2;
        }

        g_driver.publish(topic, (uint8_t *)pdata, len - step - topic_len - 3);
        switch(qos)
        {
        case 1:
            mqtt_puback(id);
            break;
        case 2:
            mqtt_pubrec(id);
            break;
        default:
            break;
        }
    }
}

/**
 * @brief process puback information
 * @param data - data to process
 * @param len - data length
 */
void process_puback(const uint8_t *data, uint8_t len)
{
    if (len >= 4)
    {
        assert_param(decode_length((uint8_t *)data, NULL) == 2);
        uint16_t uuid = data[2];
        uuid <<= 8;
        uuid += data[3];
        g_driver.puback(uuid);
    }
}

/**
 * @brief process pubrec information
 * @param data - data to process
 * @param len - data length
 */
void process_pubrec(const uint8_t *data, uint8_t len)
{
    if (len >= 4)
    {
        assert_param(decode_length((uint8_t *)data, NULL) == 2);
        uint16_t uuid = data[2];
        uuid <<= 8;
        uuid += data[3];
        g_driver.pubrec(uuid);
    }
}

/**
 * @brief process pubrel information
 * @param data - data to process
 * @param len - data length
 */
void process_pubrel(const uint8_t *data, uint8_t len)
{
    if (len >= 4)
    {
        assert_param(decode_length((uint8_t *)data, NULL) == 2);
        uint16_t uuid = data[2];
        uuid <<= 8;
        uuid += data[3];
        mqtt_pubcomp(uuid);
        g_driver.pubrel(uuid);
    }
}

/**
 * @brief process pubcomp information
 * @param data - data to process
 * @param len - data length
 */
void process_pubcomp(const uint8_t *data, uint8_t len)
{
    if (len >= 4)
    {
        assert_param(decode_length((uint8_t *)data, NULL) == 2);
        uint16_t uuid = data[2];
        uuid <<= 8;
        uuid += data[3];
        g_driver.pubcomp(uuid);
    }
}

/**
 * @brief process pubcomp information
 * @param data - data to process
 * @param len - data length
 */
void process_suback(const uint8_t *data, uint8_t len)
{
    if (len >= 4)
    {
        assert_param(decode_length((uint8_t *)data, NULL) == 3);
        uint16_t uuid = data[2];
        uuid <<= 8;
        uuid += data[3];
        uint8_t status = data[4];
        g_driver.suback(status, uuid);
    }
}

/**
 * @brief process unsuback information
 * @param data - data to process
 * @param len - data length
 */
void process_unsuback(const uint8_t *data, uint8_t len)
{
    if (len >= 4)
    {
        assert_param(decode_length((uint8_t *)data, NULL) == 3);
        uint16_t uuid = data[2];
        uuid <<= 8;
        uuid += data[3];
        g_driver.unsuback(uuid);
    }
}

/**
 * @brief process pingresp information
 * @param data - data to process
 * @param len - data length
 */
void process_pingresp(const uint8_t *data, uint8_t len)
{
    g_driver.pingresp();
}

/* function node */
typedef struct
{
    uint8_t type;
    process_func process;
}func_node;

func_node funcs[] = 
{
    {TYPE_CONNACK, process_connack},
    {TYPE_PUBLISH, process_publish},
    {TYPE_PUBACK, process_puback},
    {TYPE_PUBREC, process_pubrec},
    {TYPE_PUBREL, process_pubrel},
    {TYPE_PUBCOMP, process_pubcomp},
    {TYPE_SUBACK, process_suback},
    {TYPE_UNSUBACK, process_unsuback},
    {TYPE_PINGRESP, process_pingresp},
};

/**
 * @brief mqtt send data
 * @param data - data to send
 * @param length - data length
 */
static __INLINE void mqtt_send_data(const mqtt_msg *msg)
{
    xQueueSend(xSendQueue, msg, 200 / portTICK_PERIOD_MS);
}

/**
 * @brief mqtt receive task
 * @param pvParameters - task parameters
 */
void vMqttSend(void *pvParameters)
{
    mqtt_msg msg;
    for (;;)
    {
        if (xQueueReceive(xSendQueue, &msg, portMAX_DELAY))
        {
            if (MODE_NET_WIFI == mode_net())
            {
                if (ESP_ERR_OK == esp8266_prepare_send(g_linkid, msg.size))
                {
                    esp8266_write((const char *)msg.data, msg.size);
                }
            }
            else
            {
                if (M26_ERR_OK == m26_prepare_send(msg.size, 
                                                   3000 / portTICK_PERIOD_MS))
                {
                    m26_write((const char *)msg.data, msg.size, 
                              1000 / portTICK_PERIOD_MS);
                }
            }
        }
    }
}

/**
 * @brief mqtt receive task
 * @param pvParameters - task parameters
 */
void vMqttRecv(void *pvParameters)
{
    uint8_t data[65];
    uint16_t len;
    int count = sizeof(funcs) / sizeof(funcs[0]);
    uint8_t id = 0;
    for (;;)
    {
        if (MODE_NET_WIFI == mode_net())
        {
            if (ESP_ERR_OK == esp8266_recv(&id, data, &len, portMAX_DELAY))
            {
                for (int i = 0; i < count; ++i)
                {
                    if (funcs[i].type == (data[0] & 0xf0))
                    {
                        funcs[i].process(data, len);
                        break;
                    }
                    else if (funcs[i].type == data[0])
                    {
                        funcs[i].process(data, len);
                        break;
                    }
                }
            }
        }
        else
        {
            if (M26_ERR_OK == m26_recv(data, &len, portMAX_DELAY))
            {
                for (int i = 0; i < count; ++i)
                {
                    if (funcs[i].type == data[0])
                    {
                        funcs[i].process(data, len);
                        break;
                    }
                }
            }
        }
         
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

/**
 * @brief initialize mqtt protocol
 */
bool mqtt_init(void)
{
    TRACE("init mqtt...\r\n");
    xSendMutex = xSemaphoreCreateMutex();
    if (NULL == xSendMutex)
    {
        return FALSE;
    }
    
    xSendQueue = xQueueCreate(MQTT_MAX_MSG_NUM, sizeof(mqtt_msg) / sizeof(uint8_t));
    if (NULL == xSendQueue)
    {
        return FALSE;
    }
    
    xTaskCreate(vMqttRecv, "MqttRecv", MQTT_STACK_SIZE, 
            NULL, MQTT_PRIORITY, &xMqttHandle);
    xTaskCreate(vMqttSend, "MqttSend", MQTT_STACK_SIZE, 
            NULL, MQTT_PRIORITY, &xMqttHandle);
    if (NULL == xMqttHandle)
    {
        return FALSE;
    }

    init_mqtt_driver();

    return TRUE;
}

/**
 * @brief refresh driver
 */
static void refresh_driver(void)
{
    if (NULL == g_driver.connack)
    {
        g_driver.connack = mqtt_connack_cb;
    }

    if (NULL == g_driver.puback)
    {
        g_driver.puback = mqtt_puback_cb;
    }

    if (NULL == g_driver.pubrec)
    {
        g_driver.pubrec = mqtt_pubrec_cb;
    }

    if (NULL == g_driver.pubrel)
    {
        g_driver.pubrel = mqtt_pubrel_cb;
    }

    if (NULL == g_driver.pubcomp)
    {
        g_driver.pubcomp = mqtt_pubcomp_cb;
    }

    if (NULL == g_driver.suback)
    {
        g_driver.suback = mqtt_suback_cb;
    }
    
    if (NULL == g_driver.unsuback)
    {
        g_driver.unsuback = mqtt_unsuback_cb;
    }
    
    if (NULL == g_driver.pingresp)
    {
        g_driver.pingresp = mqtt_pingresp_cb;
    }
}

/**
 * @brief attach mqtt driver
 * @param driver - mqtt driver handle
 */
void mqtt_attach(const mqtt_driver *driver)
{
    assert_param(NULL != driver);
    g_driver.connack = driver->connack;
    g_driver.publish = driver->publish;
    g_driver.puback = driver->puback;
    g_driver.pubrec = driver->pubrec;
    g_driver.pubrel = driver->pubrel;
    g_driver.pubcomp = driver->pubcomp;
    g_driver.suback = driver->suback;
    g_driver.unsuback = driver->unsuback;
    g_driver.pingresp = driver->pingresp;
    refresh_driver();
}

/**
 * @brief detach mqtt driver
 */
void mqtt_detach(void)
{
    init_mqtt_driver();
}

/**
 * @brief validation connect parameter
 * @param param - connect parameter
 */
static void check_connect_param(const connect_param *param)
{
    assert_param(NULL != param);

    if (NULL == param->client_id)
    {
        assert_param(0x01 == param->flag._flag.clear_session);
    }

    if (0x00 == param->flag._flag.will_flag)
    {
        assert_param(0 == param->flag._flag.will_qos);
        assert_param(0 == param->flag._flag.will_retain);
    }
    else
    {
        assert_param(param->flag._flag.will_qos < 0x03);
        assert_param(NULL != param->will_topic);
        assert_param(NULL != param->will_msg);
    }

    if (0x01 == param->flag._flag.username_flag)
    {
        assert_param(NULL != param->username);
    }

    if (0x01 == param->flag._flag.password_flag)
    {
        assert_param(NULL != param->password);
    }
}

/**
 * @brief calculate connect parameter length
 * @param param - connect parameter
 */
static uint32_t calculate_connect_payload_len(const connect_param *param)
{
    uint32_t payload_len = 10;
    if (NULL != param->client_id)
    {
        payload_len += (strlen(param->client_id) + 2);
    }

    if (0x01 == param->flag._flag.will_flag)
    {
        /* add will topic and will message */
        payload_len += (strlen(param->will_topic) + 2);
        payload_len += (strlen(param->will_msg) + 2);
    }

    if (0x01 == param->flag._flag.username_flag)
    {
        /* add username */
        payload_len += (strlen(param->username) + 2);
    }

    if (0x01 == param->flag._flag.password_flag)
    {
        /* add password */
        payload_len += (strlen(param->password) + 2);
    }

    return payload_len;
}

/**
 * @brief connect to mqtt server
 * @param ip - server ip address
 * @param port - server port number
 */
int mqtt_connect_server(uint16_t id, const char *ip, uint16_t port)
{
    if (MODE_NET_WIFI == mode_net())
    {
        return esp8266_connect_server(id, "TCP", ip, port);
    }
    else
    {
        char port_str[6];
        sprintf(port_str, "%d", port);
        return m26_connect("TCP", ip, port_str, 3000 / portTICK_PERIOD_MS);
    }
}

/**
 * @brief connect mqtt server
 * @param param - connect parameter
 */
void mqtt_connect(const connect_param *param)
{
    //TRACE("mqtt connect\r\n");
    check_connect_param(param);

    mqtt_msg msg;
    uint8_t *pdata = msg.data;
    uint16_t str_len = 0;
    uint32_t payload_len = 0;

    /* calculate payload length */
    payload_len = calculate_connect_payload_len(param);

    /* packet data */
    *pdata ++ = TYPE_CONNECT;
    uint8_t encode_len = encode_length(payload_len, pdata);
    assert_param((payload_len + encode_len + 1) <= MQTT_MAX_MSG_SIZE);
    pdata += encode_len;
    for (uint8_t i = 0; i < sizeof(protocol_name) / sizeof(uint8_t); ++i)
    {
        *pdata ++ = protocol_name[i];
    }
    *pdata ++ = PROTOCOL_LEVEL;
    *pdata ++ = param->flag.flag;
    *pdata ++ = (uint8_t)((param->alive_time) >> 8);
    *pdata ++ = (uint8_t)((param->alive_time) & 0xff);
    if (NULL != param->client_id)
    {
        str_len = strlen(param->client_id);
        *pdata ++ = (uint8_t)(str_len >> 8);
        *pdata ++ = (uint8_t)(str_len & 0xff);
        strcpy((char *)pdata, param->client_id);
        pdata += str_len;
    }
    else
    {
        *pdata ++ = 0x00;
        *pdata ++ = 0x00;
    }

    if (0x01 == param->flag._flag.will_flag)
    {
        /* add will topic and will message */
        str_len = strlen(param->will_topic);
        *pdata ++ = (uint8_t)(str_len >> 8);
        *pdata ++ = (uint8_t)(str_len & 0xff);
        strcpy((char *)pdata, param->will_topic);
        pdata += str_len;

        str_len = strlen(param->will_msg);
        *pdata ++ = (uint8_t)(str_len >> 8);
        *pdata ++ = (uint8_t)(str_len & 0xff);
        strcpy((char *)pdata, param->will_msg);
        pdata += str_len;
    }

    if (0x01 == param->flag._flag.username_flag)
    {
        /* add username */
        str_len = strlen(param->username);
        *pdata ++ = (uint8_t)(str_len >> 8);
        *pdata ++ = (uint8_t)(str_len & 0xff);
        strcpy((char *)pdata, param->username);
        pdata += str_len;
    }

    if (0x01 == param->flag._flag.password_flag)
    {
        /* add password */
        str_len = strlen(param->password);
        *pdata ++ = (uint8_t)(str_len >> 8);
        *pdata ++ = (uint8_t)(str_len & 0xff);
        strcpy((char *)pdata, param->password);
        pdata += str_len;
    }

    msg.size = payload_len + encode_len + 1;

    if (0x01 == param->flag._flag.clear_session)
    {
        /* clear unack qos1 and qos2 message*/
    }

    /* send message to queue */
    mqtt_send_data(&msg);
}

/**
 * @brief public content to topic
 * @param topic - topic to publish
 * @param content - content to publish
 */

void mqtt_publish(const char *topic, const char *content, uint8_t dup,
                  uint8_t qos, uint8_t retain)
{
    assert_param(NULL != topic);
    assert_param(NULL != content);
    //TRACE("mqtt publish\r\n");
    mqtt_msg msg;
    uint8_t *pdata = msg.data;
    uint16_t str_len = 0;
    uint32_t payload_len = 0;

    /* calculate payload length */
    payload_len = strlen(topic) + 2 + strlen(content);
    if (0 != qos)
    {
        payload_len += 2;
    }

    /* packet data */
    *pdata = TYPE_PUBLISH;
    *pdata |= ((dup & 0x01) << 3);
    *pdata |= ((qos & 0x03) << 1);
    *pdata |= (retain & 0x01);
    pdata++;
    
    uint8_t encode_len = encode_length(payload_len, pdata);
    assert_param((payload_len + encode_len + 1) <= MQTT_MAX_MSG_SIZE);
    pdata += encode_len;
    
    /* add topic */
    str_len = strlen(topic);
    *pdata ++ = (uint8_t)(str_len >> 8);
    *pdata ++ = (uint8_t)(str_len & 0xff);
    strcpy((char *)pdata, topic);
    pdata += str_len;
    if (0 != qos)
    {
        *pdata ++ = (uint8_t)(g_uuid >> 8);
        *pdata ++ = (uint8_t)(g_uuid & 0xff);
        g_uuid ++;
    }
    strcpy((char *)pdata, content);
    
    msg.size = payload_len + encode_len + 1;

    /* send message to queue */
    mqtt_send_data(&msg);
}

/**
 * @brief subscribe topic from server
 * @param topic - topic to subscribe
 * @param qos - topic qos
 */
uint8_t mqtt_subscribe(const char *topic, uint8_t qos)
{
    assert_param(NULL != topic);
    //TRACE("mqtt subscribe\r\n");
    mqtt_msg msg;
    uint8_t *pdata = msg.data;
    uint16_t str_len = 0;
    uint32_t payload_len = 0;

    /* calculate payload length */
    payload_len = strlen(topic) + 5;

    /* packet data */
    *pdata++ = TYPE_SUBSCRIBE;
    
    uint8_t encode_len = encode_length(payload_len, pdata);
    assert_param((payload_len + encode_len + 1) <= MQTT_MAX_MSG_SIZE);
    pdata += encode_len;
    
    /* add uuid */
    *pdata ++ = (uint8_t)(g_uuid >> 8);
    *pdata ++ = (uint8_t)(g_uuid & 0xff);
    g_uuid ++;

    /* add topic */
    str_len = strlen(topic);
    *pdata ++ = (uint8_t)(str_len >> 8);
    *pdata ++ = (uint8_t)(str_len & 0xff);
    strcpy((char *)pdata, topic);
    pdata += str_len;

    *pdata = (qos & 0x03);
    
    msg.size = payload_len + encode_len + 1;

    /* send message to queue */
    mqtt_send_data(&msg);
    
    return g_uuid;
}

/**
 * @brief unsubscribe topic from server
 * @param topic - topic to subscribe
 * @param qos - topic qos
 */
void mqtt_unsubscribe(const char *topic)
{
    assert_param(NULL != topic);
    //TRACE("mqtt unsubscribe\r\n");
    mqtt_msg msg;
    uint8_t *pdata = msg.data;
    uint16_t str_len = 0;
    uint32_t payload_len = 0;

    /* calculate payload length */
    payload_len = strlen(topic) + 4;

    /* packet data */
    *pdata++ = TYPE_SUBSCRIBE;
    
    uint8_t encode_len = encode_length(payload_len, pdata);
    assert_param((payload_len + encode_len + 1) <= MQTT_MAX_MSG_SIZE);
    pdata += encode_len;
    
    /* add uuid */
    *pdata ++ = (uint8_t)(g_uuid >> 8);
    *pdata ++ = (uint8_t)(g_uuid & 0xff);
    g_uuid ++;

    /* add topic */
    str_len = strlen(topic);
    *pdata ++ = (uint8_t)(str_len >> 8);
    *pdata ++ = (uint8_t)(str_len & 0xff);
    strcpy((char *)pdata, topic);
    pdata += str_len;
    
    msg.size = payload_len + encode_len + 1;

    /* send message to queue */
    mqtt_send_data(&msg);
}

/**
 * @brief puback signal
 */
void mqtt_puback(uint16_t id)
{
    //TRACE("mqtt puback\r\n");
    mqtt_msg msg;
    uint8_t *pdata = msg.data;

    /* packet data */
    *pdata ++ = TYPE_PUBACK;
    *pdata ++ = 0x02;
    *pdata ++ = (uint8_t)(id >> 8);
    *pdata ++ = (uint8_t)(id & 0xff);
    
    msg.size = 4;

    /* send message to queue */
    mqtt_send_data(&msg);
}

/**
 * @brief pubrec signal
 */
void mqtt_pubrec(uint16_t id)
{
    TRACE("id1 = %d\r\n", id);
    //TRACE("mqtt pubrec\r\n");
    mqtt_msg msg;
    uint8_t *pdata = msg.data;

    /* packet data */
    *pdata ++ = TYPE_PUBREC;
    *pdata ++ = 0x02;
    *pdata ++ = (uint8_t)(id >> 8);
    *pdata ++ = (uint8_t)(id & 0xff);
    
    msg.size = 4;

    /* send message to queue */
    mqtt_send_data(&msg);
}

/**
 * @brief pubcomp signal
 */
void mqtt_pubcomp(uint16_t id)
{
    //TRACE("mqtt pubcomp\r\n");
    TRACE("id2 = %d\r\n", id);
    mqtt_msg msg;
    uint8_t *pdata = msg.data;

    /* packet data */
    *pdata ++ = TYPE_PUBCOMP;
    *pdata ++ = 0x02;
    *pdata ++ = (uint8_t)(id >> 8);
    *pdata ++ = (uint8_t)(id & 0xff);
    
    msg.size = 4;

    /* send message to queue */
    mqtt_send_data(&msg);
}

/**
 * @brief pingreq topic from server
 */
void mqtt_pingreq(void)
{
    //TRACE("mqtt pingreq\r\n");
    mqtt_msg msg;
    uint8_t *pdata = msg.data;

    /* packet data */
    *pdata ++ = TYPE_PINGREQ;
    *pdata ++ = 0x00;
    
    msg.size = 2;

    /* send message to queue */
    mqtt_send_data(&msg);
}

/**
 * @brief disconnect from server
 */
void mqtt_disconnect(void)
{
    //TRACE("mqtt disconnect\r\n");
    mqtt_msg msg;
    uint8_t *pdata = msg.data;

    /* packet data */
    *pdata ++ = TYPE_DISCONNECT;
    *pdata ++ = 0x00;
    
    msg.size = 2;

    /* send message to queue */
    mqtt_send_data(&msg);
}

/**
 * @brief notify socket connect
 * @param id - connect id
 */
void mqtt_notify_connect(uint8_t id)
{
    g_linkid = id;
}

/**
 * @brief notify socket disconnect
 */
void mqtt_notify_disconnect(void)
{
    g_linkid = 0xff;
}

