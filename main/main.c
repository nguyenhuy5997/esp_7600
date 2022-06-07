#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#define ECHO_TEST_TXD_1 (17)
#define ECHO_TEST_RXD_1 (16)
#define ECHO_TEST_RTS (UART_PIN_NO_CHANGE)
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)
#define ECHO_UART_PORT_NUM_1    (2)
#define ECHO_UART_BAUD_RATE     (115200)
#define ECHO_TASK_STACK_SIZE    (2048)
#define BUF_SIZE (1024)

#define POWER_KEY (2)
#define GPIO_OUTPUT_PIN_SEL (1ULL << POWER_KEY)
#define TAG "SIMCOM"
#define CLIENT_ID "MAN02ND00074"
#define MQTT_BROKER "tcp://vttmqtt.innoway.vn:1883"
#define CLIENT_PW "NULL"
typedef struct simcom_t
{
	bool AT_buff_avai;
	char AT_buff[BUF_SIZE];
}simcom;
typedef struct client_t
{
	char password[20];
	char id[20];
	char broker[50];
	int	index;
	int sv_type;
}client;
typedef enum
{
	AT_OK,
	AT_ERROR,
	AT_TIMEOUT,
}AT_res;
simcom simcom_7600;
client mqttClient7600 = {};
void Init_gpio_output()
{
	  gpio_config_t io_conf = {};
	  io_conf.intr_type = GPIO_INTR_DISABLE;
	  io_conf.mode = GPIO_MODE_OUTPUT;
	  io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
	  io_conf.pull_down_en = 0;
	  io_conf.pull_up_en = 0;
	  gpio_config(&io_conf);
	  gpio_set_level(POWER_KEY, 1);
}
void powerOn()
{
	gpio_set_level(POWER_KEY, 0);
	vTaskDelay(1000/portTICK_PERIOD_MS);
	gpio_set_level(POWER_KEY, 1);
}
void init_uart_simcom()
{
    uart_config_t uart_config = {
        .baud_rate = ECHO_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    int intr_alloc_flags = 0;
    ESP_ERROR_CHECK(uart_driver_install(ECHO_UART_PORT_NUM_1, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(ECHO_UART_PORT_NUM_1, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(ECHO_UART_PORT_NUM_1, ECHO_TEST_TXD_1, ECHO_TEST_RXD_1, ECHO_TEST_RTS, ECHO_TEST_CTS));
}
static void uart_simcom(void *arg)
{
    char data[BUF_SIZE];
    init_uart_simcom();
    while (1) {
        int len = uart_read_bytes(ECHO_UART_PORT_NUM_1, data, (BUF_SIZE - 1), 100 / portTICK_PERIOD_MS);
        // Write data back to the UART
        if (len) {
            data[len] = '\0';
            ESP_LOGI(TAG, "Rec: %s", data);
            memcpy(simcom_7600.AT_buff, data, len);
            simcom_7600.AT_buff_avai = true;
        }
        vTaskDelay(10/portTICK_PERIOD_MS);
    }
}
void initMqttClient(client* client, char* id, int sv_type, char* user_password, char* broker_mqtt)
{
	client->index = 0;
	memcpy(client->id, id, strlen(id));
	client->sv_type = sv_type;
	memcpy(client->password, user_password, strlen(user_password));
	memcpy(client->broker, broker_mqtt, strlen(broker_mqtt));
}
void _sendAT(char *AT_command)
{
	ESP_LOGI(TAG, "Send: %s", AT_command);
	simcom_7600.AT_buff_avai = false;
	uart_write_bytes(ECHO_UART_PORT_NUM_1, (const char *) AT_command, strlen((char *)AT_command));
	uart_write_bytes(ECHO_UART_PORT_NUM_1, (const char *)"\r\n", strlen("\r\n"));
	vTaskDelay(100/portTICK_PERIOD_MS);
}
bool _readSerial(uint32_t timeout)
{
	uint64_t timeOld = esp_timer_get_time() / 1000;
	while (!simcom_7600.AT_buff_avai && !(esp_timer_get_time() / 1000 > timeOld + timeout))
	{
		vTaskDelay(10/portTICK_PERIOD_MS);
	}
	if(simcom_7600.AT_buff_avai == false) return false;
	else
	{
		simcom_7600.AT_buff_avai = false;
		return true;
	}
}
AT_res isInit(int retry)
{
	while(retry--)
	{
		_sendAT("AT");
		if(_readSerial(1000) == false) continue;
		if(strstr(simcom_7600.AT_buff, "OK")) return AT_OK;
		else return AT_ERROR;
	}
	return AT_ERROR;
}
AT_res powerOff(int retry)
{
	while(retry--)
	{
		_sendAT("AT+CPOF");
		if(_readSerial(1000) == false) continue;
		if(strstr(simcom_7600.AT_buff, "OK")) return AT_OK;
	}
	return AT_ERROR;
}
AT_res isRegistered(int retry)
{
	while(retry--)
	{
		_sendAT("AT+CREG?");
		if(_readSerial(1000) == false) continue;
		if(strstr(simcom_7600.AT_buff, "0,1") || strstr(simcom_7600.AT_buff, "0,5") || strstr(simcom_7600.AT_buff, "1,1") || strstr(simcom_7600.AT_buff, "1,5")) return AT_OK;
		else continue;
	}
	return AT_ERROR;
}
AT_res _mqttStart(int retry)
{
	while(retry--)
	{
		_sendAT("AT+CMQTTSTART");
		if(_readSerial(1000) == false) continue;
		if(strstr(simcom_7600.AT_buff, "+CMQTTSTART: 0")) return AT_OK;
	}
	return AT_ERROR;
}
AT_res _accquireClient(client clientMqtt, int retry)
{
	char aquireClient[100];
	while(retry--)
	{
		sprintf(aquireClient, "AT+CMQTTACCQ=%d,\"%s\",%d", clientMqtt.index, clientMqtt.id, clientMqtt.sv_type);
		_sendAT(aquireClient);
		if(_readSerial(1000) == false) continue;
		if(strstr(simcom_7600.AT_buff, "OK")) return AT_OK;
	}
	return AT_ERROR;
}
AT_res mqttConnect(client clientMqtt, int retry)
{
	AT_res res = false;
	res = _mqttStart(3);
	if(res != AT_OK) return AT_ERROR;
	res = _accquireClient(clientMqtt, 3);
	if(res != AT_OK) return AT_ERROR;
	char mqttConnect[200];
	sprintf(mqttConnect, "AT+CMQTTCONNECT=%d,\"%s\",60,1,\"%s\",\"%s\"", clientMqtt.index, clientMqtt.broker, clientMqtt.id, clientMqtt.password);
	while(retry--)
	{
		_sendAT(mqttConnect);
		if(_readSerial(10000) == false) continue;
		char buff[50];
		sprintf(buff, "CMQTTCONNECT: %d,0", clientMqtt.index);
		if(strstr(simcom_7600.AT_buff, buff)) return AT_OK;
	}
	return AT_ERROR;
}
AT_res _inputPub(client clientMqtt, char* topic, int retry)
{
	int _retry = retry;
	char buff[200];
	sprintf(buff, "AT+CMQTTTOPIC=%d,%d", clientMqtt.index, strlen(topic));
	while(retry--)
	{
		_sendAT(buff);
		if(_readSerial(1000) == false) continue;
		if(!strstr(simcom_7600.AT_buff, ">")) return AT_ERROR;
		while(_retry--)
		{
			_sendAT(topic);
			if(_readSerial(1000) == false) continue;
			if(strstr(simcom_7600.AT_buff, "OK")) return AT_OK;
		}
	}
	return AT_ERROR;
}
void mqttDisconnect(client clientMqtt)
{
	char buff[50];
	int retry = 5;
	sprintf(buff, "AT+CMQTTDISC=%d,60", clientMqtt.index);
	while(retry--)
	{
		_sendAT(buff);
		if(_readSerial(3000) == false) continue;
		if(strstr(simcom_7600.AT_buff, "+CMQTTDISC")) break;
	}
	retry = 3;
	sprintf(buff, "AT+CMQTTREL=%d", clientMqtt.index);
	while(retry--)
	{
		_sendAT(buff);
		if(_readSerial(1000) == false) continue;
		if(strstr(simcom_7600.AT_buff, "+CMQTTREL")) break;
	}
	retry = 3;
	while(retry--)
	{
		_sendAT("AT+CMQTTSTOP");
		if(_readSerial(1000) == false) continue;
		if(strstr(simcom_7600.AT_buff, "+CMQTTSTOP")) break;
	}
}
AT_res  mqttPublish(client clientMqtt, char* data, char* topic, int qos, int retry)
{
	int _retry = retry, __retry = retry;
	AT_res res = false;
	char buff[200];
	res = _inputPub(clientMqtt, topic, 3);
	if(res != AT_OK) return AT_ERROR;
	sprintf(buff, "AT+CMQTTPAYLOAD=%d,%d", clientMqtt.index, strlen(data));
	while(retry--)
	{
		_sendAT(buff);
		if(_readSerial(1000) == false) continue;
		if(!strstr(simcom_7600.AT_buff, ">")) return AT_ERROR;
		while(_retry--)
		{
			_sendAT(data);
			if(_readSerial(1000) == false) continue;
			if(!strstr(simcom_7600.AT_buff, "OK")) return AT_ERROR;
			sprintf(buff, "AT+CMQTTPUB=%d,%d,60,0", clientMqtt.index, qos);
			while(__retry--)
			{
				_sendAT(buff);
				if(_readSerial(1000) == false) continue;
				char buff[50];
				sprintf(buff, "CMQTTPUB: %d,0", clientMqtt.index);
				if(strstr(simcom_7600.AT_buff, buff)) return AT_OK;
			}
		}
	}
	return AT_ERROR;
}
void echoATSwtich(bool enable)
{
	int retry = 3;
	if(enable == 0)
	{
		while(retry--)
		{
			_sendAT("ATE0");
			if(_readSerial(1000) == false) continue;
			else return;
		}
	}
	else
	{
		while(retry--)
		{
			_sendAT("ATE1");
			if(_readSerial(1000) == false) continue;
			else return;
		}
	}
}
static void main_proc(void *arg)
{
	AT_res res;
	while(1)
	{
POWER_ON:
//		powerOn();
//		vTaskDelay(4000/portTICK_PERIOD_MS);
		res = isInit(5);
		if(res == AT_OK) ESP_LOGI(TAG, "Module init OK");
		else
		{
			ESP_LOGE(TAG, "Module init FALSE");
			goto POWER_ON;
		}
		echoATSwtich(0);
		res = isRegistered(10);
		if(res == AT_OK) ESP_LOGI(TAG, "Module registed to network OK");
		else ESP_LOGE(TAG, "Module is not registed to network OK");

		initMqttClient(&mqttClient7600, CLIENT_ID, 0, CLIENT_PW, MQTT_BROKER);

		res = mqttConnect(mqttClient7600, 5);

		if(res == AT_OK) ESP_LOGI(TAG, "MQTT Connected");
		else ESP_LOGE(TAG, "MQTT can not connect");
		char pubBuff[500] = "{\"ss\":0,\"CC\":1,\"Type\":\"DON\",\"r\":0,\"MMC\":{\"P\":2,\"M\":1},\"T\":0,\"V\":\"S3.1.3\",\"B\":0,\"Cn\":\"\",\"N\":3}";

		res = mqttPublish(mqttClient7600, pubBuff, "messages/MAN02ND00074/devconf", 1, 3);
		if(res == AT_OK)
		{
			ESP_LOGI(TAG, "Publish OK");
		}
		else
		{
			ESP_LOGE(TAG, "Publish FALSE");
		}
		mqttDisconnect(mqttClient7600);
		while(1)
		{
			vTaskDelay(10/portTICK_PERIOD_MS);
		}
	}
}

void app_main(void)
{
	Init_gpio_output();
	xTaskCreate(uart_simcom, "uart_echo_task1", 4096, NULL, 10, NULL);
	xTaskCreate(main_proc, "main", 4096, NULL, 10, NULL);
}
