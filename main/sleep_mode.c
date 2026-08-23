/*
 * This file is part of the WiCAN project.
 *
 * Copyright (C) 2022  Meatpi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include  "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>
#include "comm_server.h"
#include "lwip/sockets.h"
#include "driver/twai.h"
#include "types.h"
#include "esp_timer.h"
#include "config_server.h"
#include "slcan.h"
#include "can.h"
#include "ble.h"
#include "esp_sleep.h"
#include "wifi_network.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "sleep_mode.h"
#include "mqtt_client.h"
#include "ver.h"
#include "driver/rtc_io.h"
#include "led.h"
#include "led_indicator.h"
#include "obd.h"
#include "esp_adc/adc_oneshot.h"
#include "dev_status.h"
#include "wifi_mgr.h"
#include "restart_tracker.h"
#include "can_wake.h"   /* #4 wake-on-CAN */

// #define TAG 		__func__
#define TAG         "SLEEP_MODE"

#if HARDWARE_VER != WICAN_PRO

#define TIMES              256
#define GET_UNIT(x)        ((x>>3) & 0x1)
#if CONFIG_IDF_TARGET_ESP32
#define ADC_RESULT_BYTE     2
#define ADC_CONV_LIMIT_EN   1                       //For ESP32, this should always be set to 1
#define ADC_CONV_MODE       ADC_CONV_SINGLE_UNIT_1  //ESP32 only supports ADC1 DMA mode
#define ADC_OUTPUT_TYPE     ADC_DIGI_OUTPUT_FORMAT_TYPE1
#elif CONFIG_IDF_TARGET_ESP32S2
#define ADC_RESULT_BYTE     2
#define ADC_CONV_LIMIT_EN   0
#define ADC_CONV_MODE       ADC_CONV_BOTH_UNIT
#define ADC_OUTPUT_TYPE     ADC_DIGI_OUTPUT_FORMAT_TYPE2
#elif CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32C2
#define ADC_RESULT_BYTE     4
#define ADC_CONV_LIMIT_EN   0
#define ADC_CONV_MODE       ADC_CONV_ALTER_UNIT     //ESP32C3 only supports alter mode
#define ADC_OUTPUT_TYPE     ADC_DIGI_OUTPUT_FORMAT_TYPE2
#elif CONFIG_IDF_TARGET_ESP32S3
#define ADC_RESULT_BYTE     4
#define ADC_CONV_LIMIT_EN   0
#define ADC_CONV_MODE       ADC_CONV_SINGLE_UNIT_1
#define ADC_OUTPUT_TYPE     ADC_DIGI_OUTPUT_FORMAT_TYPE2
#endif

#if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32C2
#if CONFIG_IDF_TARGET_ESP32C3
static uint16_t adc1_chan_mask = BIT(4);
static adc_channel_t channel[1] = {ADC1_CHANNEL_4};
#else
static uint16_t adc1_chan_mask = BIT(6);
//static uint16_t adc2_chan_mask = BIT(0);
static adc_channel_t channel[1] = {ADC1_CHANNEL_6};
#endif
#endif
#if CONFIG_IDF_TARGET_ESP32S2
static uint16_t adc1_chan_mask = BIT(2) | BIT(3);
static uint16_t adc2_chan_mask = BIT(0);
static adc_channel_t channel[3] = {ADC1_CHANNEL_2, ADC1_CHANNEL_3, (ADC2_CHANNEL_0 | 1 << 3)};
#endif
#if CONFIG_IDF_TARGET_ESP32
static uint16_t adc1_chan_mask = BIT(7);
static uint16_t adc2_chan_mask = 0;
static adc_channel_t channel[1] = {ADC1_CHANNEL_7};
#endif
//#define THRESHOLD_VOLTAGE		13.0f
#define SLEEP_TIME_DELAY		(180*1000*1000)
#define WAKEUP_TIME_DELAY		(200*1000)

static EventGroupHandle_t s_mqtt_event_group = NULL;
#define MQTT_CONNECTED_BIT 			BIT0
#define PUB_SUCCESS_BIT     		BIT1

static float sleep_voltage = 13.1f;
static uint8_t enable_sleep = 0;
static QueueHandle_t voltage_queue = NULL;
static esp_adc_cal_characteristics_t adc1_chars;
// Static queue storage for voltage_queue (queue length = 1, item size = sizeof(float))
static StaticQueue_t voltage_queue_struct;
static uint8_t voltage_queue_storage[sizeof(float)];


static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%ld", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
//    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
//        esp_mqtt_client_stop(client);
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        xEventGroupSetBits(s_mqtt_event_group, PUB_SUCCESS_BIT);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
//        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
//            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
//            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
//            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
//            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
//
//        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}
static esp_mqtt_client_handle_t client = NULL;
static void mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
		.broker.address.uri = config_server_get_alert_url(),
		.broker.address.port = config_server_get_alert_port(),
		.credentials.username = config_server_get_alert_mqtt_user(),
		.credentials.authentication.password = config_server_get_alert_mqtt_pass(),
		.network.disable_auto_reconnect = true,
		.network.reconnect_timeout_ms = 4000,
//         .uri = config_server_get_alert_url(),
// 		.port = config_server_get_alert_port(),
// 		.username = config_server_get_alert_mqtt_user(),
// 		.password = config_server_get_alert_mqtt_pass(),
// //		.disable_auto_reconnect = 1,
// 		.reconnect_timeout_ms = 4000
    };
    ESP_LOGI(TAG, "mqtt_cfg.uri: %s", mqtt_cfg.broker.address.uri);
    if(client == NULL)
    {
    	client = esp_mqtt_client_init(&mqtt_cfg);
        /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
        esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(client);
    }
    else
    {
    	esp_mqtt_client_reconnect(client);
    }

    EventBits_t bits = xEventGroupWaitBits(s_mqtt_event_group,
						MQTT_CONNECTED_BIT,
						pdFALSE,
						pdFALSE,
						pdMS_TO_TICKS(10000));
    if (bits & MQTT_CONNECTED_BIT)
    {
    	static char pub_data[128];
    	float batt_voltage = 0;
    	sleep_mode_get_voltage(&batt_voltage);
    	sprintf(pub_data, "{\"alert\": \"low_battery\", \"battery_voltage\": %f}", batt_voltage);
        int msg_id = esp_mqtt_client_publish(client, config_server_get_alert_topic(), pub_data, 0, 1, 0);
        ESP_LOGI(TAG, "publish, msg_id=%d", msg_id);

    }
    else
    {
    	ESP_LOGE(TAG, "unable to connect to broker...");
    }
}


static void continuous_adc_init(uint16_t adc1_chan_mask, uint16_t adc2_chan_mask, adc_channel_t *channel, uint8_t channel_num)
{
    adc_digi_init_config_t adc_dma_config = {
        .max_store_buf_size = 1024,
        .conv_num_each_intr = TIMES,
        .adc1_chan_mask = adc1_chan_mask,
//        .adc2_chan_mask = adc2_chan_mask,
    };
    ESP_ERROR_CHECK(adc_digi_initialize(&adc_dma_config));

    adc_digi_configuration_t dig_cfg = {
        .conv_limit_en = ADC_CONV_LIMIT_EN,
        .conv_limit_num = 250,
        .sample_freq_hz = 10 * 1000,
        .conv_mode = ADC_CONV_MODE,
        .format = ADC_OUTPUT_TYPE,
    };

    adc_digi_pattern_config_t adc_pattern[SOC_ADC_PATT_LEN_MAX] = {0};
    dig_cfg.pattern_num = channel_num;
    for (int i = 0; i < channel_num; i++) {
        uint8_t unit = GET_UNIT(channel[i]);
        uint8_t ch = channel[i] & 0x7;
        adc_pattern[i].atten = ADC_ATTEN_DB_11;
        adc_pattern[i].channel = ch;
        adc_pattern[i].unit = unit;
        adc_pattern[i].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

        ESP_LOGI(TAG, "adc_pattern[%d].atten is :%x", i, adc_pattern[i].atten);
        ESP_LOGI(TAG, "adc_pattern[%d].channel is :%x", i, adc_pattern[i].channel);
        ESP_LOGI(TAG, "adc_pattern[%d].unit is :%x", i, adc_pattern[i].unit);
    }
    dig_cfg.adc_pattern = adc_pattern;
    ESP_ERROR_CHECK(adc_digi_controller_configure(&dig_cfg));
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_DEFAULT, 0, &adc1_chars);


}

//ADC Attenuation
#define ADC_ATTEN           ADC_ATTEN_DB_11

//ADC Calibration
#if CONFIG_IDF_TARGET_ESP32
#define ADC_CALI_SCHEME     ESP_ADC_CAL_VAL_EFUSE_VREF
#elif CONFIG_IDF_TARGET_ESP32S2
#define ADC_CALI_SCHEME     ESP_ADC_CAL_VAL_EFUSE_TP
#elif CONFIG_IDF_TARGET_ESP32C3
#define ADC_CALI_SCHEME     ESP_ADC_CAL_VAL_EFUSE_TP
#elif CONFIG_IDF_TARGET_ESP32S3
#define ADC_CALI_SCHEME     ESP_ADC_CAL_VAL_EFUSE_TP_FIT
#endif
static esp_adc_cal_characteristics_t adc1_chars;
static bool adc_calibration_init(void)
{
    esp_err_t ret;
    bool cali_enable = false;

    ret = esp_adc_cal_check_efuse(ADC_CALI_SCHEME);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Calibration scheme not supported, skip software calibration");
    } else if (ret == ESP_ERR_INVALID_VERSION) {
        ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
    } else if (ret == ESP_OK) {
        cali_enable = true;
        esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN, ADC_WIDTH_BIT_DEFAULT, 0, &adc1_chars);
    } else {
        ESP_LOGE(TAG, "Invalid arg");
    }

    return cali_enable;
}

#if !CONFIG_IDF_TARGET_ESP32
static bool check_valid_data(const adc_digi_output_data_t *data)
{
    const unsigned int unit = data->type2.unit;
    if (unit > 2) return false;
    if (data->type2.channel >= SOC_ADC_CHANNEL_NUM(unit)) return false;

    return true;
}
#endif

#define RUN_STATE			0
#define	SLEEP_DETECTED		1
#define SLEEP_STATE			2
#define WAKEUP_STATE		3

static void adc_task(void *pvParameters)
{
    esp_err_t ret;
    uint32_t ret_num = 0;
    uint8_t result[TIMES] = {0};
    static uint8_t sleep_state = 0;
    static int64_t sleep_detect_time = 0;
    static int64_t wakeup_detect_time = 0;
    static int64_t pub_time = 0;
    static float alert_voltage = 0;
    static uint64_t alert_time;

    alert_time = config_server_get_alert_time();
    alert_time *= (3600000000);
//    alert_time = 10000000;
    ESP_LOGW(TAG, "%" PRIu64 "\n", alert_time);

    if(config_server_get_alert_volt(&alert_voltage) != -1)
    {
    	alert_voltage = 16.0f;
    }

    memset(result, 0xcc, TIMES);
    adc_calibration_init();
    continuous_adc_init(adc1_chan_mask, adc1_chan_mask, channel, sizeof(channel) / sizeof(adc_channel_t));
    adc_digi_start();

    while(1)
    {
    	uint32_t count = 0;
    	uint64_t avg = 0;
    	uint32_t adc_val = 0;
    	for(int j = 0; j < 10; j++)
    	{
			ret = adc_digi_read_bytes(result, TIMES, &ret_num, ADC_MAX_DELAY);
			if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE)
			{
				if (ret == ESP_ERR_INVALID_STATE)
				{
					/**
					 * @note 1
					 * Issue:
					 * As an example, we simply print the result out, which is super slow. Therefore the conversion is too
					 * fast for the task to handle. In this condition, some conversion results lost.
					 *
					 * Reason:
					 * When this error occurs, you will usually see the task watchdog timeout issue also.
					 * Because the conversion is too fast, whereas the task calling `adc_digi_read_bytes` is slow.
					 * So `adc_digi_read_bytes` will hardly block. Therefore Idle Task hardly has chance to run. In this
					 * example, we add a `vTaskDelay(1)` below, to prevent the task watchdog timeout.
					 *
					 * Solution:
					 * Either decrease the conversion speed, or increase the frequency you call `adc_digi_read_bytes`
					 */
				}

	//            ESP_LOGI("TASK:", "ret is %x, ret_num is %d", ret, ret_num);
				count = 0;
				avg = 0;
				for (int i = 0; i < ret_num; i += ADC_RESULT_BYTE)
				{
					adc_digi_output_data_t *p = (void*)&result[i];

					if (ADC_CONV_MODE == ADC_CONV_SINGLE_UNIT_1 || ADC_CONV_MODE == ADC_CONV_ALTER_UNIT)
					{
						if (check_valid_data(p))
						{
							count++;
							avg += (esp_adc_cal_raw_to_voltage(p->type2.data, &adc1_chars));
	//                        ESP_LOGI(TAG, "Unit: %d,_Channel: %d, Value: %d", p->type2.unit+1, p->type2.channel, p->type2.data);
						}
						else
						{
							// abort();
							ESP_LOGI(TAG, "Invalid data [%d_%d_%x]", p->type2.unit+1, p->type2.channel, p->type2.data);
						}
					}
				}
				//See `note 1`
//				ESP_LOGI(TAG, "value: %u",(uint32_t)(avg/count));
				vTaskDelay(10);
			}
			else if (ret == ESP_ERR_TIMEOUT)
			{
				/**
				 * ``ESP_ERR_TIMEOUT``: If ADC conversion is not finished until Timeout, you'll get this return error.
				 * Here we set Timeout ``portMAX_DELAY``, so you'll never reach this branch.
				 */
				ESP_LOGW(TAG, "No data, increase timeout or reduce conv_num_each_intr");
				vTaskDelay(2000);
			}
    	}
    	adc_val = (uint32_t)(avg/count);
    	float battery_voltage;

    	if(HARDWARE_VER == WICAN_V300)
    	{
    		battery_voltage = (adc_val*116)/(16*1000.0f);
    	}
    	else if(HARDWARE_VER == WICAN_USB_V100)
    	{
    		battery_voltage = (adc_val*106.49f)/(6.49f*1000.0f);
    	}
    	battery_voltage += 0.2;
    	// if(project_hardware_rev == WICAN_V210)
    	// {
    	// 	battery_voltage = -1;
    	// }

    	xQueueOverwrite( voltage_queue, &battery_voltage );
        if (battery_voltage < sleep_voltage)
        {
        	dev_status_clear_bits(DEV_WAKE_VOLTAGE_OK_BIT);
        }
        else
        {
        	dev_status_set_bits(DEV_WAKE_VOLTAGE_OK_BIT);
        }
    	if(enable_sleep == 1)
    	{
			switch(sleep_state)
			{
				case RUN_STATE:
				{
					if(battery_voltage < sleep_voltage)
					{
						ESP_LOGI(TAG, "low voltage, value: %lu, voltage: %f",adc_val, battery_voltage);
						sleep_detect_time = esp_timer_get_time();
						sleep_state++;
					}
					break;
				}
				case SLEEP_DETECTED:
				{
					if(battery_voltage > sleep_voltage)
					{
						ESP_LOGI(TAG, "low voltage, value: %lu, voltage: %f",adc_val, battery_voltage);
						sleep_state = RUN_STATE;
					}

					if((esp_timer_get_time() - sleep_detect_time) > SLEEP_TIME_DELAY)
					{
						sleep_state = SLEEP_STATE;
	//    	    		wifi_network_deinit();
	//    	    		ble_disable();
					}

					break;
				}
				case SLEEP_STATE:
				{
					ESP_LOGI(TAG, "Go to sleep");
					if(battery_voltage > sleep_voltage)
					{
						wakeup_detect_time = esp_timer_get_time();
						ESP_LOGI(TAG, "low voltage, value: %lu, voltage: %f",adc_val, battery_voltage);
						sleep_state = WAKEUP_STATE;
					}

					if(config_server_get_battery_alert_config())
					{
						if(battery_voltage < alert_voltage)
						{
							ESP_LOGW(TAG, "battery alert!");
							if(((esp_timer_get_time() - pub_time) > alert_time) || (pub_time == 0))
							{
								pub_time = esp_timer_get_time();
								wifi_network_init(config_server_get_alert_ssid(), config_server_get_alert_pass());
								vTaskDelay(1000 / portTICK_PERIOD_MS);
								uint8_t count = 0;
								while(!wifi_network_is_connected())
								{
									vTaskDelay(1000 / portTICK_PERIOD_MS);
									if(count++ > 10)
									{
										break;
									}
								}
								if(wifi_network_is_connected())
								{
									ESP_LOGI(TAG, " wifi connectred try to publish");
									mqtt_init();
									EventBits_t bits = xEventGroupWaitBits(s_mqtt_event_group,
																			PUB_SUCCESS_BIT,
																			pdFALSE,
																			pdFALSE,
																			pdMS_TO_TICKS(10000));
									if (bits & PUB_SUCCESS_BIT)
									{
										ESP_LOGI(TAG, "publish ok");
										xEventGroupClearBits(s_mqtt_event_group, PUB_SUCCESS_BIT);
									}
									else
									{
										ESP_LOGE(TAG, "publish error");
									}
									esp_mqtt_client_disconnect(client);
									vTaskDelay(1000 / portTICK_PERIOD_MS);
									wifi_network_deinit();
								}
							}
						}
					}
					break;
				}
				case WAKEUP_STATE:
				{
					if(battery_voltage > sleep_voltage)
					{
						if((esp_timer_get_time() - wakeup_detect_time) > WAKEUP_TIME_DELAY)
						{
							ESP_LOGI(TAG, "Wake up now...");
                            restart_tracker_restart(RESTART_TRACKER_PLANNED_REASON_POWER_WAKE,
                                                    RESTART_TRACKER_SOURCE_SLEEP_MODE,
                                                    RESTART_TRACKER_FLAG_NONE);

						}
					}
					else if(battery_voltage < sleep_voltage)
					{
						sleep_state = SLEEP_STATE;
					}
					break;
				}
			}

	//    	ESP_LOGI(TAG, "value: %u",adc_val);
			if(sleep_state == SLEEP_STATE)
			{
				ESP_LOGW(TAG, "sleeping");
				can_disable();
				wifi_network_deinit();
				ble_disable();
				esp_sleep_enable_timer_wakeup(2*1000000);
				esp_light_sleep_start();;
			}
			else vTaskDelay(pdMS_TO_TICKS(1000));
    	}
    	else
    	{
    		vTaskDelay(pdMS_TO_TICKS(1000));
    	}
    }

    adc_digi_stop();
    ret = adc_digi_deinitialize();
    assert(ret == ESP_OK);
}

int8_t sleep_mode_get_voltage(float *val)
{
	if(voltage_queue != NULL)
	{
		if(xQueuePeek( voltage_queue, val, 0 ))
		{
			return 1;
		}
		else return -1;
	}
	return -1;
}

int8_t sleep_mode_init(uint8_t enable, float sleep_volt)
{
	enable_sleep = enable;
	sleep_voltage = sleep_volt;
	ESP_LOGW(TAG, "sleep_volt: %2.2f", sleep_volt);
	s_mqtt_event_group = xEventGroupCreate();
    voltage_queue = xQueueCreateStatic(1, sizeof(float), voltage_queue_storage, &voltage_queue_struct);
	xTaskCreate(adc_task, "adc_task", 4096, (void*)AF_INET, 5, NULL);

	return 1;
}
#elif HARDWARE_VER == WICAN_PRO
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// #include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "elm327.h"
#include "math.h"
#include "wc_timer.h"
#include "hw_config.h"
#include "sdcard.h"
#include "ble.h"
#include "esp_heap_caps.h"   /* resume health: heap + largest-block, logged on every resume */
#include "event_log.h"   /* #86: record a postponed sleep so a stalled flash is explainable */
/* Bring-up guards read by sleep_mode_recovery_needed(): a wake resumes in place, so the sleep
 * path is now the only thing that can still hand these subsystems the reboot they are waiting
 * for. */
#include "poll_log.h"
#include "fast_log.h"
#include "csv_logger.h"

#define ADC_UNIT          ADC_UNIT_1
#define ADC_CONV_MODE     ADC_CONV_SINGLE_UNIT_1
#define ADC_ATTEN         ADC_ATTEN_DB_6  // 0-3.3V
#define ADC_BIT_WIDTH     ADC_BITWIDTH_DEFAULT 
#define ADC_READ_LEN      256

#define ADC_OUTPUT_TYPE   ADC_DIGI_OUTPUT_FORMAT_TYPE2
#define ADC_GET_CHANNEL(p_data)   ((p_data)->type2.channel)
#define ADC_GET_DATA(p_data)      ((p_data)->type2.data)

#define CRITICAL_VOLTAGE  11.90f
#define ERROR_VOLTAGE     12.1f

/* Battery sample period. 500 ms, down from 3000 ms: a crank pulls the battery to 9-11 V for well
 * under a second, and at 3 s that dip was usually sampled either side of and never seen at all.
 * The 8-sample ADC burst behind each reading is cheap; what this rate really costs is that every
 * voltage comparison in this task now fires 6x more often, which is why the DEV_WAKE_VOLTAGE_OK
 * latch and the LOW_VOLTAGE recovery dwell below both exist.
 *
 * This is the awake period only, and it is enforced in exactly ONE place: the vTaskDelay at the
 * bottom of light_sleep_task(). Do not add a separate poll timer on top of it -- a timer armed
 * for the same value as the loop period is a race against tick rounding, and the pass it loses
 * halves the real sample rate without anything reporting it. */
#define VOLTAGE_READ_PERIOD_MS   500

/* How long the recovery must hold to abandon the sleep countdown -- see STATE_LOW_VOLTAGE. Stated
 * in ms and converted to samples so the dwell stays 2 s if the sample period is ever retuned. */
#define VOLT_RECOVER_MS          2000
#define VOLT_RECOVER_SAMPLES     (VOLT_RECOVER_MS / VOLTAGE_READ_PERIOD_MS)

/* Issue #86: how long to wait before re-testing the flash interlock when sleep was postponed.
 * Long enough that a multi-minute ROM write is not re-probed hundreds of times, short enough that
 * sleep resumes promptly once the flash ends. The device stays fully awake meanwhile, which is
 * exactly what a flash needs. */
#define SLEEP_FLASH_RETRY_MS     60000

/* Issue #86, upper bound on the postpone. A genuine flash is minutes; past this we treat the
 * flag as stuck and sleep anyway rather than refuse forever and flatten the battery. Owner's
 * call, 2026-08-06: give up after 30 minutes. */
#define SLEEP_FLASH_STUCK_CEILING_MS  (30 * 60 * 1000)

/* Gap after which a refusal belongs to a NEW streak rather than continuing the last one. Three
 * retry periods: long enough that an ordinary 60 s retry cadence never splits one flash into
 * several streaks, short enough that a streak abandoned when the countdown was cancelled cannot
 * still be "running" the next time a flash arrives. Without this the stuck-flash ceiling can be
 * armed by a stale timestamp and fire on a flash seconds old -- see the reset in
 * sleep_mode_teardown for why that is a brick-direction failure. */
#define SLEEP_FLASH_STREAK_GAP_MS     (3 * SLEEP_FLASH_RETRY_MS)

/* Issue #88: how long to wait after raising the sleep fence for the CAN producers to notice it
 * and park. Sized to cover one poll_log loop iteration for a task that was already past its
 * park check when the fence went up; proven sufficient by the #89 spike, which used the same
 * value against the same race. Paid once per sleep entry, not per cycle. */
#define SLEEP_FENCE_PARK_WAIT_MS 300

/* How long the teardown waits for the CSV writer to close an open session. Best-effort: the file
 * is closed cleanly on the writer's next pass regardless, so this only bounds how long we hold
 * the sleep for a tidy close. */
#define SLEEP_CSV_CLOSE_WAIT_MS  400

/* How many times the sleep loop will nudge an OBD chip that has not gone to sleep, per SLEEP
 * SESSION. Two, not six, and it NEVER escalates to a reboot -- see the babysitter for why. The
 * counters are file-scope so the teardown can reset them per session; as function statics they
 * were effectively per-boot, which means nothing on a device that no longer reboots to wake. */
#define SLEEP_ELM327_MAX_NUDGES  2

/* How many ~2 s passes GPIO7 is allowed to still read "awake" before we believe the chip really
 * refused to sleep.
 *
 * Was 1, which is one pass too few. Measured on the bench and in the car, the pin settles at
 * ~4010 ms after elm327_sleep() ("DIAG gpio7 settled asleep after 4011 ms / 1 passes") -- and one
 * grace pass allows about 4 s. Zero margin: a chip 50 ms slower than usual gets hard-reset, and a
 * hardreset WAKES it, so the babysitter creates the very symptom it is reacting to.
 *
 * Two costs nothing when the chip is on time: grace passes are only consumed while the pin still
 * reads awake, so a chip that has settled never spends the second one. */
#define SLEEP_ELM327_SETTLE_PASSES  2

/* How long to wait for the shared UART lock inside the babysitter, per attempt. Short on purpose:
 * this runs inside the 2 s sleep loop, so blocking the full ELM327_CMD_MUTEX_TIMOUT of 10 s here
 * would stall the whole sleep state machine -- including the CAN-wake check -- for five loop
 * passes. A parked device has nothing else talking to the chip, so a lock that is free is free
 * immediately; a lock that is busy is a bug worth naming, not worth waiting 10 s for. */
#define SLEEP_ELM327_LOCK_WAIT_MS   1000

/* Cap on lock-failed nudge attempts per SLEEP SESSION, and it is a real requirement, not tidiness.
 * "Could not get the lock -- try again next pass" without a cap means retrying every 2 s for the
 * whole night in a parked car, each attempt burning the lock wait above. That is exactly the
 * battery drain sleeping exists to prevent. Same budget as the chip nudges. */
#define SLEEP_ELM327_MAX_LOCK_FAILS 2

static uint8_t s_elm327_sleep_nudges  = 0;
static uint8_t s_elm327_settle_passes = 0;
static uint8_t s_elm327_lock_fails    = 0;   /* nudges abandoned because the UART lock was held */

/* DIAGNOSTIC (2026-08-17): is "MIC chip would not sleep" a real refusal, or a false alarm?
 *
 * It fired on three sleep entries out of three in the car, and the owner reports a SECOND WiCAN
 * doing the same -- so it is a fixed-constant firmware behaviour, not one bad chip. The suspicion
 * is the settle grace above: GPIO7 is known to lag (it still reads "awake" straight after a
 * successful elm327_sleep()), and the babysitter allows exactly ONE ~2 s pass before it decides
 * the chip disobeyed and hardresets it -- which WAKES the chip, so the cure re-creates the
 * symptom. To settle it we need the one number nobody has measured: how long GPIO7 actually takes
 * to flip after elm327_sleep(). These record it. */
static uint32_t s_elm327_sleep_entry_ms   = 0;   /* when elm327_sleep() returned, this session */
static uint16_t s_elm327_sleep_passes     = 0;   /* sleeping passes since then */
static bool     s_elm327_asleep_logged    = false; /* one line per SESSION, never per cycle */

/* One "resume stack low" warning per boot -- see the emit site for why boot scope is correct here
 * (the watermark it reports is itself a historic minimum that cannot recover within an uptime). */
static bool s_resume_stack_warned = false;

/* Shortest gap between two "sleep countdown started" event lines. Not a per-episode latch: every
 * entry into STATE_LOW_VOLTAGE really is a fresh countdown, so each line is true.
 *
 * #98: this budget is now only ever spent by countdowns that SURVIVED SLEEP_COUNTDOWN_REAL_MS.
 * Before that, the throwaway countdown every boot arms and cancels claimed the budget first, and a
 * REAL countdown starting within the next minute was silently swallowed -- the device then slept
 * with nothing in the log saying a countdown had begun. Do not move the announcement back to
 * arming time without re-breaking that.
 *
 * This only stops
 * a battery parked exactly on sleep_volt from writing one every few seconds all night. */
#define SLEEP_COUNTDOWN_LOG_MIN_GAP_US  (60LL * 1000000LL)

/* How long to let the event-log writer reach the SD card before a restart wipes the RAM ring.
 * restart_tracker_restart() marks and then calls esp_restart() immediately, so any line emitted
 * just beforehand is lost without this. */
#define SLEEP_EVENT_LOG_FLUSH_MS 1500

/* How long a countdown must SURVIVE before it counts as real (#98). Below this it gets no event
 * line at all -- neither "started" nor "cancelled".
 *
 * Found on the bench, not in review: EVERY boot arms a countdown ~1.2 s in (the ECU has not
 * answered yet, and the battery sits below sleep_volt whenever the engine is not turning) and
 * cancels it ~2 s later the moment the ECU replies. That throwaway countdown was writing the exact
 * two-lines-that-say-one-thing pair this work set out to delete:
 *     sleep countdown started -- 1 min at 12.03V
 *     IGNITION_ON  ignition on -- ECU answering
 *     sleep countdown cancelled -- ECU answering at 12.00V
 * A countdown that lived two seconds was never news. One that ran a while and then got called off
 * IS news -- it is the only proof in the log that the #4/#97 ECU veto stopped this device sleeping
 * mid-drive.
 *
 * Deliberately the same 5 s the UI banner uses to decide whether to appear, so the log and the
 * screen tell the same story: a countdown too short to be worth showing anyone is too short to be
 * worth a line either. */
#define SLEEP_COUNTDOWN_REAL_MS  5000

/* Stack headroom below which a resume emits an ALWAYS-VISIBLE warning (#98).
 *
 * ABSOLUTE BYTES ON PURPOSE, not a fraction of the stack. What this guards against is a change
 * that pushes the resume path's peak usage deeper, and the margin that decides whether that
 * overflows is the distance to ZERO -- which does not scale with how big the stack happens to be.
 * A percentage would also interact BACKWARDS with issue #96 (trim the sleep task's stack): a 25%
 * floor would shrink exactly when the real margin got thinner, while 25% of today's oversized
 * 10 KB stack would fire after a perfectly healthy trim.
 *
 * COUPLING TO #96 -- READ THIS BEFORE TRIMMING light_sleep_task_stack: the measured free in the
 * resume path is 6036-6740 B out of 10240, so the worst observed peak is ~4.2 KB. It is LESS
 * deterministic than it first looked: an early reading of 6516-6740 B was revised down by a later
 * 6036 B sample on the same bench, which is itself an argument against trimming aggressively. Any
 * trim must keep (stack size - worst peak) >= this floor + 1 KB of slack, so DO NOT TRIM BELOW
 * 8192: at 8192 the expected free is ~4.0 KB and this stays quiet, while #96's 6144 option would
 * leave ~1.9 KB -- BELOW the floor, i.e. screaming from day one.
 * This floor cannot go dead with age -- it IS the margin any future trim has to preserve. */
#define SLEEP_RESUME_STACK_WARN_MIN_FREE 2048

/* Was BLE actually up when we tore down? The resume must restore exactly what was taken
 * down, never more -- see the note at the assignment site. */
static bool s_ble_was_enabled = false;
/* If VOLTAGE_READ_PERIOD_MS ever exceeded VOLT_RECOVER_MS this would divide to 0, "count >= 0"
 * would always be true, STATE_LOW_VOLTAGE would exit on entry every time, and the device would
 * NEVER SLEEP -- flattening the car battery with nothing in the log to say why. */
_Static_assert(VOLT_RECOVER_SAMPLES >= 1, "VOLT_RECOVER_MS must be >= VOLTAGE_READ_PERIOD_MS");

static adc_oneshot_unit_handle_t adc_handle;
static adc_cali_handle_t cali_handle = NULL;
static bool do_calibration = false;
static QueueHandle_t voltage_queue = NULL;
static QueueHandle_t sleep_state_queue = NULL;
// Static queue storage for voltage_queue (queue length = 1, item size = sizeof(float))
static StaticQueue_t voltage_queue_struct;
static uint8_t voltage_queue_storage[sizeof(float)];
// Static queue storage for sleep_state_queue (queue length = 1, item size = sizeof(sleep_state_info_t))
static StaticQueue_t sleep_state_queue_struct;
static uint8_t sleep_state_queue_storage[sizeof(sleep_state_info_t)];

static void calibration_init(void)
{
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    ESP_LOGI(TAG, "calibration scheme version is %s", "Curve Fitting");
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT,
        .chan = ADC_CHANNEL_3,
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BIT_WIDTH,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle);
    if (ret == ESP_OK) {
        calibrated = true;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) 
	{
        ESP_LOGI(TAG, "calibration scheme version is %s", "Line Fitting");
        adc_cali_line_fitting_config_t cali_config = 
		{
            .unit_id = ADC_UNIT,
            .atten = ADC_ATTEN,
            .bitwidth = ADC_BIT_WIDTH,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &cali_handle);
        if (ret == ESP_OK) 
		{
            calibrated = true;
        }
    }
#endif

    do_calibration = calibrated;
    if (do_calibration) 
	{
        ESP_LOGI(TAG, "Calibration Success");
    } else {
        ESP_LOGW(TAG, "Calibration Failed");
    }
}

void oneshot_adc_init(void)
{
    // Initialize ADC
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    // Configure ADC channel
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BIT_WIDTH,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &config));

    ESP_LOGI(TAG, "ADC channel: %d, Attenuation: %d", ADC_CHANNEL_3, ADC_ATTEN);

    calibration_init();
    // Initialize calibration
    // do_calibration = example_adc_calibration_init(ADC_UNIT, ADC_CHANNEL_3, ADC_ATTEN, &cali_handle);
}

static void update_battery_voltage(float *new_volt)
{
    if (new_volt != NULL) {
        xQueueOverwrite(voltage_queue, new_volt);
    }
}

esp_err_t read_ss_adc_voltage(float *voltage_out)
{
    if (voltage_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int NUM_SAMPLES = 8; // Similar to conv_frame_size in continuous version
    uint32_t sum_raw = 0;
    uint32_t valid_samples = 0;
    uint32_t min_raw = UINT32_MAX;
    uint32_t max_raw = 0;
    int sum_voltage = 0;

    // Take multiple readings
    for (int i = 0; i < NUM_SAMPLES; i++)
    {
        int raw_value;
        esp_err_t ret = adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw_value);
        
        if (ret == ESP_OK && raw_value < 4096)
        {
            int voltage = 0;
            
            // Convert raw to voltage using calibration
            if (do_calibration)
            {
                ret = adc_cali_raw_to_voltage(cali_handle, raw_value, &voltage);
            } 
            else
            {
                voltage = (raw_value * 3300) / 4095;
            }
            
            if(ret == ESP_OK)
            {
                sum_raw += raw_value;
                sum_voltage += voltage;
                valid_samples++;
                
                if (raw_value < min_raw) min_raw = raw_value;
                if (raw_value > max_raw) max_raw = raw_value;
                
                // Print first few samples for debugging
                // if (valid_samples <= 5) 
                // {
                //     ESP_LOGI(TAG, "Sample[%d]: Chan=%d, Raw=%d, Voltage=%dmV", 
                //             (int)valid_samples, ADC_CHANNEL_3, raw_value, voltage);
                // }
            }
            else
            {
                ESP_LOGE(TAG, "ADC adc_cali_raw_to_voltage error: %d", ret);
            }
            // Small delay between readings
            // vTaskDelay(pdMS_TO_TICKS(1));
        }
        else
        {
            ESP_LOGE(TAG, "ADC read error: %d", ret);
        }
    }

    // Calculate averages
    if (valid_samples > 0) 
    {
        int avg_raw = sum_raw / valid_samples;
        float avg_voltage = (float)sum_voltage / valid_samples;
        
        #ifdef HV_PRO_V140
        float volts = ((float)avg_voltage * 7.25f) / 1000;
        #else
        float volts = ((float)avg_voltage * 11) / 1000;
        volts+=0.1f;  // Adjust for calibration offset
        #endif

        /* Deliberately NOT rounded to 0.1 V. Rounding cost more than it bought: every threshold
         * compare downstream shifted by up to 0.05 V (a raw 13.16 used to round UP to 13.2 and
         * pass a >= 13.2 test; it now fails it), and a crank dip to 9-11 V for under a second was
         * indistinguishable from noise, which made a low battery impossible to diagnose from
         * /check_status. That is a real change to sleep/wake edges -- see the dwell on the
         * LOW_VOLTAGE escape and the DEV_WAKE_VOLTAGE_OK latch, both of which exist because this
         * value now moves 6x more often.
         * NOTE: this was originally landed as the instrument for a crank dip-wake detector. That
         * detector was never built and is not coming -- wake-on-CAN (#4) reads the bus waking up
         * directly, which is earlier and unambiguous, so nothing needs to infer it from a
         * sub-second voltage dip. The rounding decision stands on the reasons above. */
        *voltage_out = volts;

        ESP_LOGD(TAG, "Summary: Raw=%d (min=%lu, max=%lu, avg of %lu), Voltage=%.2f V [%s]",
                 avg_raw, min_raw, max_raw, valid_samples, *voltage_out,
                 do_calibration ? "CALIBRATED" : "UNCALIBRATED");
                 
        return ESP_OK;
    }
    
    return ESP_ERR_INVALID_STATE;  // No valid samples
}

void configure_wakeup_sources(void)
{
    esp_sleep_enable_ext0_wakeup(OBD_READY_PIN, 0);
    rtc_gpio_pullup_dis(OBD_READY_PIN);
}

void enter_deep_sleep(void)
{
	static char response_buffer[32];
	static uint32_t response_len = 0;
	static int64_t response_cmd_time = 0;
	esp_err_t sleep_ret = ESP_OK;
    ESP_LOGI(TAG, "Entering deep sleep");
    configure_wakeup_sources();
    
    // adc_continuous_stop(adc_handle);
    
	if(gpio_get_level(OBD_READY_PIN) == 1)
	{
		printf("MIC is already sleeping!!!!!!!\r\n");
	}
    gpio_set_level(CAN_STDBY_GPIO_NUM, 1);
	// sleep_ret = elm327_sleep();
    
	vTaskDelay(pdMS_TO_TICKS(5000));

	if(sleep_ret == ESP_OK && gpio_get_level(OBD_READY_PIN) == 1)
	{
		printf("MIC chip is sleeping...\r\n");
	}
	else
	{
		printf("MIC sleep failed...\r\n");
	}
    gpio_set_level(CAN_STDBY_GPIO_NUM, 1);
	ESP_LOGI(TAG, "Going to sleep now");
	// Returns after any in-progress indicator repaint; nothing relights the
	// LED between here and esp_deep_sleep_start().
	led_indicator_suspend();
	led_set_level(0,0,0);
    esp_wifi_stop();
    ble_disable();
    sd_card_deinit();
	vTaskDelay(pdMS_TO_TICKS(1000));
	// Enter deep sleep
	esp_deep_sleep_start();
}

/* The ONE sleep-entry teardown. Previously this existed as two copies -- the low-voltage timeout
 * path and the boot-loop guard -- which had to be kept in step by hand and had already drifted
 * (the LED suspend sat on different sides of the state_info publish). One body means they cannot
 * diverge again. Each caller keeps only its own tail: the low-voltage path adds the 1 s settle
 * and the periodic-wakeup timer, the boot-loop guard adds its printf and the red breathing LED.
 *
 * Returns false when sleep must be POSTPONED -- see the flash interlock below. The caller must
 * then NOT move to STATE_SLEEPING.
 *
 * Order matters and is preserved from the original: the state_info publish is the gate that stops
 * anything writing to the MIC/ELM327 chip over UART (consumer: elm327.c:1622-1625, which refuses
 * the UART once state == STATE_SLEEPING), so it happens before the caller's settle delay. */
static bool sleep_mode_teardown(sleep_state_info_t *state_info, float battery_voltage, bool force)
{
    /* Issue #86 -- LIVE HAZARD, and the reason this returns a bool.
     * An ECU flash runs key-on / engine-off, where the battery sits near 12.4 V -- BELOW the
     * 13.0 V sleep threshold. So the sleep countdown starts the moment the key turns, before the
     * flash even begins, and expiring mid-transfer would run can_disable() + wifi_mgr_deinit()
     * straight through a TransferData sequence. That leaves the PCM in its bootloader: the car
     * will not start and a recovery flash is required.
     * FLASH_ACTIVE_BIT is the codec-owned "a flash/read is on the bus right now" fence
     * (can.c:204-206). While it is raised, refuse to sleep. The caller re-arms its countdown and
     * asks again shortly; a flash is finite, so this postpones sleep, it never cancels it. */
    static bool    postpone_logged  = false;  /* one line per flash session, not per retry */
    static int64_t postpone_start_us = 0;     /* when this refusal streak began */
    static int64_t postpone_last_us  = 0;     /* last pass that REFUSED -- see the streak reset */

    /* A firmware OTA is the same hazard against a different resource. The teardown's
     * wifi_mgr_deinit() pulls the network stack out from under a live HTTP upload; on the bench
     * that panicked the device at a non-code address twelve seconds after a sleep entry, with an
     * upload in flight. It does not brick anything -- the new image is only marked bootable at
     * the very end, so an interrupted upload just leaves the old firmware running -- but it makes
     * firmware updates fail by CRASHING rather than by returning an error, and it is easy to hit
     * whenever the sleep countdown is short. Shares the same bounded postpone as the ECU-flash
     * case below, so a stuck upload can never hold sleep off forever and flatten the battery. */
    const bool busy_flashing = can_flash_active() || config_server_ota_active();

    /* Issue #92 -- a host session that is NOT a flash. NC Flash raises the bus-claim lease for
     * the whole of any ECU conversation (DTC reads, RAM operations, the authentication window),
     * and only some of those raise FLASH_ACTIVE_BIT. Sleeping through one tears the bus and the
     * network out from under a live session for no reason.
     *
     * Gate on the LEASE, never on can_host_bus_claim_active(): the raw flag stays set until the
     * dead-man reaper clears it, and the reaper is gated on the bus going idle -- which, key-on,
     * does not happen until the key turns off (#70). Holding sleep off on the raw flag would
     * therefore be unbounded, trading a rare cut session for a flat battery. The lease is
     * self-bounding instead -- but read the bound precisely. The host renews every 4 s against a
     * 75 s TTL, so once the owning 35001 socket is GONE this drops within the TTL. The socket
     * dying is what every real host death looks like: a crash, a suspend, a WiFi drop or an
     * unplugged cable all surface as a closed or dead socket within ~20 s (TCP keepalive on
     * 35001 is 5 s idle / 5 s interval / 3 probes), and the owner_alive term exists to cover
     * exactly that tail, where the lease has expired but the socket is still retransmitting.
     *
     * What this is NOT is a hard 75 s ceiling. owner_alive stays true for as long as that same
     * socket stays open, so a host application that FREEZES while its machine stays awake and
     * connected -- renewals stopped, socket never closed -- holds sleep off for as long as it
     * sits there, not for 75 s. Left overnight in a parked car that flattens the battery. It is
     * accepted knowingly: before #92 the same situation cut a live flash instead, which is the
     * worse of the two. Do not design anything new against a TTL bound that is not there.
     *
     * This consults no reaper output at all, so a stuck reaper cannot extend it. */
    can_coexist_snapshot_t coexist;
    can_coexist_snapshot(&coexist);
    const bool session_live = (coexist.host_bus_claimed &&
                               (!coexist.claim_expired || coexist.claim_owner_alive)) ||
                              /* A park being actively RENEWED is a live session too, and must be
                               * caught here as well as by the veto -- otherwise a park-only session
                               * that slipped through the sub-pass race would sleep, which is the one
                               * hole this backstop exists to close. Lease-validity again, never the
                               * raw flag: a park a dead host left behind goes stale in 12 s and must
                               * NOT hold sleep off. */
                              (coexist.datalog_parked &&
                               (!coexist.park_expired || coexist.park_owner_alive));

    if ((busy_flashing || session_live) && !force)
    {
        const int64_t now = esp_timer_get_time();

        /* Start a NEW streak whenever the last refusal is too old to belong to this one.
         *
         * The reset at the bottom of this block only runs when teardown is called again AND the
         * refusal condition has gone false -- and most streaks never end that way. A backstop
         * refusal leaves the machine in LOW_VOLTAGE, and the very next pass the #92 veto cancels
         * it back to NORMAL, so teardown is never re-entered and postpone_start_us would keep a
         * stale timestamp indefinitely. (Pre-existing form of the same leak: the voltage recovers
         * mid-postpone and LOW_VOLTAGE exits without another teardown call.)
         *
         * That is dangerous in the BRICK direction, which is why the gap test is here rather than
         * left as tidy-up: a later, claim-less busy_flashing case -- an in-car OTA at 12.5 V, or a
         * curl-driven fastwrite -- would compute elapsed from the stale stamp, read it as hours,
         * and fire the "assume it is stuck, sleep anyway" ceiling on a flash that started seconds
         * ago. That tears down WiFi under a live upload and the CAN bus under a live ECU write:
         * exactly the PCM brick #86 exists to prevent.
         *
         * Deliberate tradeoff: a genuinely stuck flash whose postpones are broken up by voltage
         * flapping across the threshold now restarts its 30-min clock per streak instead of
         * accumulating across them. Every streak is still ceiling-bounded, so the cost is a slower
         * battery drain in a rare case -- taken knowingly over a misfire in the brick direction. */
        if (postpone_start_us == 0 ||
            (now - postpone_last_us) > ((int64_t)SLEEP_FLASH_STREAK_GAP_MS * 1000))
        {
            postpone_start_us = now;
            postpone_logged   = false;   /* a new streak earns its own line */
        }
        postpone_last_us = now;

        /* The postpone MUST be bounded. FLASH_ACTIVE_BIT is codec-owned with no reaper, so a
         * codec that crashes or hangs with it raised would refuse sleep forever and flatten the
         * car battery over days, emitting a single log line the whole time. A real flash takes
         * minutes. Past the ceiling we assume it is stuck and sleep anyway: the PCM risk from
         * cutting a flash that has been frozen for half an hour is already realised -- that
         * transfer is dead either way -- whereas the battery is still savable. */
        /* The ceiling exists for the FLASH/OTA causes only, and a live claim still vetoes it.
         * A claim needs no ceiling of its own -- its 75 s TTL already is one -- so if the claim
         * is still live here, the host is genuinely renewing and cutting it is exactly the bug
         * this guard was added for. Sleeping anyway is only ever the lesser evil against a bit
         * that nothing will ever lower. */
        if (busy_flashing && !session_live &&
            (now - postpone_start_us) > (int64_t)SLEEP_FLASH_STUCK_CEILING_MS * 1000)
        {
            ESP_LOGE(TAG, "flash active %d min -- assuming stuck, sleeping anyway (#86)",
                     SLEEP_FLASH_STUCK_CEILING_MS / 60000);
            event_log_emit(EVL_INFO,
                           "flash active %d min -- assuming STUCK, sleeping anyway (bus torn down)",
                           SLEEP_FLASH_STUCK_CEILING_MS / 60000);
            /* fall through into the teardown */
        }
        else
        {
            if (!postpone_logged)
            {
                postpone_logged = true;
                if (can_flash_active())
                {
                    ESP_LOGW(TAG, "sleep postponed: ECU flash active on the bus (#86)");
                    event_log_emit(EVL_INFO, "sleep postponed -- ECU flash active (would have cut the bus mid-write)");
                }
                else if (config_server_ota_active())
                {
                    ESP_LOGW(TAG, "sleep postponed: firmware OTA upload in progress");
                    event_log_emit(EVL_INFO, "sleep postponed -- firmware OTA in progress (would have cut the upload)");
                }
                else
                {
                    /* Session postpone. Bounded by the lease once the host's socket is gone --
                     * TTL plus one retry -- but not while that socket stays open; see the gate
                     * comment above for the frozen-host case this deliberately does not bound. */
                    ESP_LOGW(TAG, "sleep postponed: host session active -- claim/park lease held (#92)");
                    event_log_emit(EVL_INFO, "sleep postponed -- host session active (claim/park lease held)");
                }
            }
            return false;
        }
    }

    /* Flash finished (or we gave up on it) -- re-arm the log and the streak for next time. */
    postpone_logged   = false;
    postpone_start_us = 0;

    /* Issue #88: raise the sleep fence FIRST, before anything is torn down, and give the CAN
     * producers time to see it. poll_log checks can_should_park() at the TOP of its loop
     * (poll_log.c:1079); a task already past that check is mid-iteration and could still reach
     * its can_enable() brackets (poll_log.c:1358-1360 / :1405-1407), which drive the transceiver
     * standby pin LOW (can.c:438) -- landing AFTER our can_disable() below and leaving the bus
     * awake all night. The wait lets any such task come round to the check and park.
     * 300 ms is the value the #89 spike proved sufficient against this same race. */
    can_sleep_fence_set();
    vTaskDelay(pdMS_TO_TICKS(SLEEP_FENCE_PARK_WAIT_MS));

    gpio_set_level(CAN_STDBY_GPIO_NUM, 1);
    dev_status_clear_bits(DEV_AWAKE_BIT);
    dev_status_set_bits(DEV_SLEEP_BIT);
    if(dev_status_is_autopid_enabled())
    {
        dev_status_wait_for_bits(DEV_AUTOPID_IDLE_BIT, pdMS_TO_TICKS(20000));
    }
    /* Record what was actually UP before we take it down, so the resume restores exactly that and
     * no more. ble_disable() below is a no-op when BLE was already off, so an unconditional
     * ble_enable() on resume would switch on a radio the user never asked for -- and initialising
     * the BT controller from this task panics the device with an interrupt watchdog. Bench-proven:
     * that is exactly what crash-looped this build every ~68 s. */
    s_ble_was_enabled = dev_status_is_bit_set(DEV_BLE_ENABLED_BIT);

    /* Close and score the CAN-wake window, if one is open. Sleep entry is the single point every
     * wake passes through exactly once, whichever way it came back, so this is where a wake is
     * judged worthwhile or fruitless. Placed before the teardown proper so the accounting is done
     * even if something below decides to reboot instead. */
    can_wake_note_sleep_entry();

    /* Ask the CSV writer to close any open session and give it a moment to do so. Normally the
     * ignition/engine gate closed it long ago; this covers manual FORCE_ON, where a file would
     * otherwise stay open across the sleep and come back with its timers jumped. Best-effort: if
     * the writer does not get there in time the file is still closed cleanly on the next pass. */
    csv_logger_set_sleep_requested(true);
    for(int i = 0; i < (SLEEP_CSV_CLOSE_WAIT_MS / 20) && csv_logger_session_active(); i++)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* Fresh budget for this sleep session: two nudges, two lock-failed attempts, and two passes of
     * grace for GPIO7 to settle before we believe it. Resetting HERE rather than relying on a
     * function-static is the whole point -- a static that is only ever cleared at boot means
     * nothing now that a wake resumes. */
    s_elm327_sleep_nudges  = 0;
    s_elm327_settle_passes = 0;
    s_elm327_sleep_passes  = 0;
    s_elm327_lock_fails    = 0;
    s_elm327_asleep_logged = false;

    /* The result was thrown away here too. It is worth one line: a sleep entry whose STSLEEP0 never
     * reached the chip explains everything the babysitter is about to see, and saying so at the
     * moment it happens beats inferring it two minutes later from GPIO7. */
    {
        const esp_err_t sleep_ret = elm327_sleep();
        if(sleep_ret == ESP_ERR_TIMEOUT)
        {
            char holder[16] = {0};
            elm327_lock_holder_name(holder, sizeof(holder));
            event_log_emit(EVL_INFO, "sleep entry: UART lock held by %s -- chip not told to sleep",
                           holder);
        }
        else if(sleep_ret != ESP_OK)
        {
            event_log_emit(EVL_INFO, "sleep entry: chip refused STSLEEP0 (%s)",
                           esp_err_to_name(sleep_ret));
        }
    }
    s_elm327_sleep_entry_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /* The one permanent marker that a sleep entry happened. Nothing else in the teardown writes to
     * the event log, so without this a sleep is invisible in the record and can only be inferred
     * from the device going quiet on the network -- which, on a box with no serial console, means
     * every future field diagnosis starts by guessing. Emitted after the chip sleep so the line
     * cannot itself delay the teardown. */
    event_log_emit(EVL_INFO, "entering sleep (%.2fV)", (double)battery_voltage);

    can_disable();
    /* #4: TWAI is now uninstalled, so GPIO1 is a free input again. Put it in the exact state
     * the wake sampler needs (both pulls OFF) while we still know nothing else owns it. */
    can_wake_prepare();
    wifi_mgr_deinit();
    ble_disable();
    // Update immediately to prevent elm327 wakeup
    state_info->state = STATE_SLEEPING;
    state_info->voltage = battery_voltage;
    state_info->timer = 0;   /* countdown is over, not paused -- never leave a stale value here */
    xQueueOverwrite(sleep_state_queue, state_info);
    led_indicator_suspend();
    led_set_level(0,0,0);
    return true;
}

/* Resume from light sleep IN PLACE, without rebooting.
 *
 * Historical note, because the reboot looked deliberate and was not: waking used to be a DEEP
 * sleep, where the wake IS a hardware reset and app_main re-initialised everything for free.
 * Upstream commit dfb8ced ("oneshot adc") changed deep sleep to light sleep and, rather than write
 * a resume for the five things it had just torn down, appended esp_restart() next to the existing
 * resume line. The dead `current_state = STATE_NORMAL;` it orphaned is still in the tree. So the
 * ~10 s reboot on every wake was an accident of that edit, never a design decision.
 *
 * Order is the exact reverse of sleep_mode_teardown(), and it matters:
 *   1. GPIO9 hold FIRST -- until the pad hold is released the OBD chip cannot be woken at all.
 *   2. Bus back BEFORE the fence drops, so a producer released by the fence can never transmit
 *      into a disabled controller.
 *   3. dev_status + the state publish LAST -- the publish is the gate that lets the ELM327 UART
 *      task talk to the chip again (elm327.c:1622-1625), so nothing may touch the chip before it.
 *
 * Returns false if any step that matters failed, and the caller then falls back to the reboot.
 * That fallback is deliberate: this path only ever runs on a device with no WiFi and no serial
 * console, so "reboot instead" must always remain reachable. */
/* Is anything on this device waiting for a reboot to repair itself?
 *
 * Several subsystems protect against a crash during their own start-up with the same pattern:
 * arm an RTC flag before the risky work, clear it once stable, and if a boot finds it still
 * armed, skip that subsystem for one boot and disarm "so the next boot retries". That was sound
 * while every wake rebooted -- the device got a repair attempt at least once per sleep cycle,
 * for free. A wake that resumes in place removes the next boot entirely, so a skipped subsystem
 * stays skipped for the whole uptime: observed live, a crash disarmed poll_log and the device
 * ran for hours answering HTTP with the datalogger silently dead.
 *
 * So the reboot does not go away -- it becomes the RECOVERY channel, taken only when something
 * is actually broken. Resume is the optimisation; reboot is the repair. Any future "retry on the
 * next boot" mechanism joins this list by adding one predicate here, and nothing else. */
static bool sleep_mode_recovery_needed(void)
{
    return poll_log_bringup_skipped()
        || fast_log_bringup_skipped()
        || csv_logger_bringup_skipped()
        || event_log_bringup_skipped();
}

static bool sleep_mode_resume(sleep_state_info_t *state_info, float battery_voltage)
{
    ESP_LOGW(TAG, "resuming in place (no reboot)");

    /* 0. REFUSALS FIRST, before anything is touched. Each of these is a case where resuming in
     *    place is known-unsafe or unproven; returning false hands the wake to the reboot
     *    fallback, which is how these configurations always came back before this work. Doing
     *    the checks up front (rather than part-way down) means a refusal costs nothing and
     *    leaves no half-restored state behind.
     *
     *    EVERY refusal and failure below emits an event-log line, deliberately. This device has
     *    no serial console, so an ESP_LOG that nobody can read is the same as no log at all: a
     *    wake that quietly rebooted instead of resuming would be indistinguishable from a wake
     *    that crashed. Observed on the bench -- one wake took the fallback and there was no way
     *    to tell which of these five paths did it. If you add a refusal, log it too.
     *
     *    (a) SmartConnect. smartconnect_init() (smartconnect.c:689) has no re-entry guard: it
     *        overwrites a static stack pointer -- leaking the old PSRAM stack -- and calls
     *        xTaskCreateStatic() with the SAME static task control block while the first
     *        smartconnect task is still running, because there is no smartconnect_deinit() and
     *        wifi_mgr_deinit() knows nothing about it. Building a second task on a live task's
     *        TCB is scheduler-state corruption; the crash can surface anywhere, long after.
     *        Remove this refusal only once smartconnect has a real init guard and deinit.
     *
     *    (b) BLE. ble_enable() is NOT symmetric with ble_disable() -- disable no-ops when BLE is
     *        off, enable does not -- and bringing the BT controller back up in place panicked
     *        the device with INT_WDT on every cycle. On this platform BLE is unreachable from
     *        the UI anyway, so this is effectively never taken. */
    if(sleep_mode_recovery_needed())
    {
        ESP_LOGW(TAG, "resume: a subsystem is waiting on a boot to retry -- rebooting to repair it");
        event_log_emit(EVL_INFO, "resume refused: bring-up guard set (poll_log %d fast_log %d "
                                 "csv %d event_log %d) -- rebooting to repair",
                       (int)poll_log_bringup_skipped(), (int)fast_log_bringup_skipped(),
                       (int)csv_logger_bringup_skipped(), (int)event_log_bringup_skipped());
        return false;
    }
    if(config_server_get_wifi_mode() == SMARTCONNECT_MODE)
    {
        ESP_LOGW(TAG, "resume: SmartConnect mode -- rebooting instead (no safe re-init)");
        event_log_emit(EVL_INFO, "resume refused: SmartConnect mode has no safe re-init -- rebooting");
        return false;
    }
    if(s_ble_was_enabled)
    {
        ESP_LOGW(TAG, "resume: BLE was active -- rebooting instead (in-place BLE re-init is unsafe)");
        event_log_emit(EVL_INFO, "resume refused: BLE was up, in-place re-init is unsafe -- rebooting");
        return false;
    }

    /* DIAGNOSTIC (2026-08-17): measure where the resume actually spends its time.
     *
     * Measured in the car: every wake left the LED dark and WiFi unreachable for ~23 s while the
     * datalogger was already recording, and /wifi_diag put the first WiFi association ATTEMPT 20 s
     * after CAN_WAKE -- so ~20 s goes somewhere BELOW, before the network is touched. Reading the
     * code narrows it to the chip step but cannot pin it, because elm327_hardreset_chip() is really two
     * mutex takes and up to six UART timeouts (see elm327_hardreset_timing_t).
     *
     * One line per WAKE, never per cycle -- the 2 s sleep loop runs ~43k times a night. */
    const uint32_t t_resume_start = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t t_hold_ms = 0, t_can_ms = 0, t_fence_ms = 0, t_chip_ms = 0, t_wifi_ms = 0, t_led_ms = 0;
    uint32_t t_mark = t_resume_start;
    #define SLEEP_RESUME_LAP(dst) do { \
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000); \
        (dst) = now_ms - t_mark; \
        t_mark = now_ms; \
    } while(0)

    /* 1. Undo the pad hold that pins the OBD chip asleep. Instant (pin work only), and it must
     *    precede any attempt to talk to that chip. */
    elm327_release_sleep_hold();
    SLEEP_RESUME_LAP(t_hold_ms);

    /* 2. Hand the LED back to its own task.
     *
     *    READ THIS BEFORE ASSUMING IT LIGHTS THE LED HERE -- IT DOES NOT, AND AN EARLIER VERSION
     *    OF THIS COMMENT CLAIMED IT DID. led_indicator_resume() only decrements the suspend
     *    counter (led_indicator.c:105-118). The task that actually paints is parked on
     *    DEV_AWAKE_BIT (led_indicator.c:152) and refuses to paint without dev_status_is_awake()
     *    as a second backstop (led_indicator.c:186). That bit is set by dev_status_set_awake() at
     *    the very END of this function, so the LED cannot come on before the state publish no
     *    matter where this call sits. The measured t_led=0 is that: the call returns instantly
     *    because it paints nothing.
     *
     *    So moving it here is tidiness, not a fix. The owner's "the device looks dead" symptom was
     *    cured by removing the 20 s stall further down, NOT by this line. If light-before-network
     *    is ever genuinely wanted, the change belongs in the indicator task's DEV_AWAKE_BIT gate
     *    -- and that gate is deliberate, it is what lets the sleep paths darken the LED, so do not
     *    remove it casually.
     *
     *    Position still matters for one reason: the suspend count must be back to zero before the
     *    publish, or the first paint after the publish is skipped. Keeping it above every step
     *    that can fail guarantees that without depending on where the failures are. */
    led_indicator_resume();
    SLEEP_RESUME_LAP(t_led_ms);

    /* 3. BUS FIRST -- this is the whole point of resuming rather than rebooting.
     *    poll_log drives the TWAI controller directly and needs the interpreter chip for
     *    NOTHING, so getting the bus back before the chip handshake means logging restarts in
     *    single-digit milliseconds instead of waiting ~1-2 s (worst case ~4.5 s) for a chip that
     *    the datalogger does not use. can_enable() re-installs TWAI and drives the transceiver's
     *    standby pin low itself (can.c:438), reclaiming GPIO1 from the wake sampler. */
    can_enable();
    SLEEP_RESUME_LAP(t_can_ms);
    if(!can_is_enabled())
    {
        ESP_LOGE(TAG, "resume: can_enable() failed -- falling back to reboot");
        event_log_emit(EVL_INFO, "resume FAILED: can_enable() did not bring the bus back -- rebooting");
        return false;
    }

    /* 4. Only now release the parked producers. Bus first, fence second -- never the reverse,
     *    or an unparked task could transmit into a disabled controller. */
    can_sleep_fence_clear();
    csv_logger_set_sleep_requested(false);   /* the writer may open a fresh session again */
    SLEEP_RESUME_LAP(t_fence_ms);

    /* 5. Network back, and note it now runs BEFORE the interpreter chip rather than after.
     *    The chip step is the only slow one left, and the owner's way of asking "is it alive?" is
     *    to reach the device over WiFi -- so the network must not queue behind a chip nobody is
     *    waiting on. Safe in this order because nothing in wifi_network_init() touches UART1, so
     *    the two steps share no state; only wall-clock order changes.
     *
     *    wifi_mgr deinit/init at runtime is proven live: smartconnect does exactly
     *    this pair on every home/drive transition (smartconnect.c:320 -> :352) with no reboot.
     *    ap_ssid is rebuilt from the MAC rather than reached for in main.c, which owns a static. */
    {
        uint8_t mac[6] = {0};
        char ap_ssid[33] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(ap_ssid, sizeof(ap_ssid), "WiCAN_%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        /* A resume that comes back awake but WITHOUT a network is the worst outcome available:
         * the device logs happily to SD and cannot be reached to find out. esp_wifi wants large
         * contiguous allocations, so after long uptimes heap fragmentation is the realistic way
         * this fails. Hand it to the reboot fallback, which defragments by construction. */
        const esp_err_t wifi_ret = wifi_network_init(ap_ssid);
        if(wifi_ret != ESP_OK)
        {
            ESP_LOGE(TAG, "resume: wifi_network_init() failed (%s) -- falling back to reboot",
                     esp_err_to_name(wifi_ret));
            event_log_emit(EVL_INFO, "resume FAILED: wifi bring-up returned %s -- rebooting",
                           esp_err_to_name(wifi_ret));
            return false;
        }
    }

    SLEEP_RESUME_LAP(t_wifi_ms);

    /* 6. LAST of the real work, and now BOUNDED: the interpreter chip. A hard reset is what the
     *    existing retry path uses and the only sequence proven to bring it back from STSLEEP0.
     *
     *    The 1000 ms is the wait for the shared UART lock, not for the chip. With the old fixed
     *    10 s, a lock held by another task cost this wake 10 s here plus another 10 s inside the
     *    elm327_set_baudrate() tail call -- the measured 20 s of dark LED and dead WiFi. Bounded,
     *    the worst case is ~1 s (lock never free) or ~4.5 s (chip genuinely sick), never 20 s.
     *
     *    FAILURE IS NON-FATAL and always was: we log it and carry on rather than rebooting. A
     *    device that is awake, on the network and logging CAN beats one that reboots because a
     *    chip the datalogger does not even use failed to answer.
     *
     *    Doing this on a worker task so the resume never waits at all was considered and rejected:
     *    it buys ~1.8 s typical at the price of task lifetime, re-entry and publish-ordering
     *    questions on the one path that must not grow new failure modes. Synchronous and bounded.
     *
     *    Nothing touches this chip until the state publish at the end reopens its UART gate. */
    if(!elm327_hardreset_chip_timeout(1000))
    {
        ESP_LOGW(TAG, "resume: chip reset did not complete -- continuing anyway (non-fatal)");
    }
    SLEEP_RESUME_LAP(t_chip_ms);

    /* DIAGNOSTIC: emitted BEFORE the state publish below, so it lands even if something after this
     * point misbehaves. hardreset_* is the breakdown INSIDE the chip step: mutex is the wait for
     * the UART lock (cap 10 s), rd/rt are the two possible reads (~1.5 s each), baud is
     * elm327_set_baudrate() which takes the same lock a SECOND time. calls>1 within one wake would
     * mean the chip is being reset more than once, which is the other way to reach ~20 s. */
    /* The whole block is behind ONE explicit debug check rather than relying on the three
     * EVENT_LOG_DEBUG macros below. Those do already avoid evaluating their arguments when the
     * gate is shut, but elm327_hardreset_get_timings() sits OUTSIDE them and would otherwise copy
     * the ~88-byte timing struct on every single wake for nothing. */
    if(event_log_debug_enabled())
    {
        elm327_hardreset_timing_t hr;
        elm327_hardreset_get_timings(&hr);
        /* TWO lines, not one: EVENT_LOG_DETAIL_MAX is 112 chars and the combined line was ~150,
         * which silently truncated exactly the fields that matter most (calls= tells us whether a
         * single wake resets the chip twice). Two lines per WAKE still honours the per-wake rule --
         * what the rule forbids is per-CYCLE logging in the 2 s loop. */
        /* Fields listed in EXECUTION order, which changed with the reorder above: led now comes
         * second and chip last. Same names, same meanings -- so old and new logs stay comparable
         * field by field -- but read left to right they are a timeline again. */
        /* DEBUG-GATED (owner request): on a healthy device these three lines are pure numbers and
         * they crowd the event page, which is the owner's normal view of what the device did. The
         * EVENTS they describe are still recorded unconditionally -- CAN_WAKE, IGNITION_ON,
         * DATALOG_*, and every resume refusal/failure line are untouched. What is hidden is only
         * the millisecond breakdown, which matters when investigating and never otherwise.
         *
         * Turn them back on with the stored "debug" config flag when a wake needs measuring:
         * GET /event_log/status reports the flag as "debug". Without it these will NOT appear, so
         * do not read their absence as "the fix stopped working". */
        EVENT_LOG_DEBUG(EVL_INFO,
                       "resume ms: hold=%u led=%u can=%u fence=%u wifi=%u chip=%u tot=%u",
                       (unsigned)t_hold_ms, (unsigned)t_led_ms, (unsigned)t_can_ms,
                       (unsigned)t_fence_ms, (unsigned)t_wifi_ms, (unsigned)t_chip_ms,
                       (unsigned)((uint32_t)(esp_timer_get_time() / 1000) - t_resume_start));
        EVENT_LOG_DEBUG(EVL_INFO,
                       "hardreset ms: mutex=%u%s rd=%u rt=%u baud=%u tot=%u g7=%d %s %s calls=%u",
                       (unsigned)hr.mutex_ms, hr.mutex_ok ? "ok" : "TIMEOUT",
                       (unsigned)hr.reset_read_ms, (unsigned)hr.retry_read_ms,
                       (unsigned)hr.baudrate_ms, (unsigned)hr.total_ms,
                       (int)hr.gpio7_at_entry,
                       hr.used_reset_line ? "rstline" : "atz",
                       hr.answered ? "ans" : "NOANS",
                       (unsigned)hr.calls);
        /* The answer to "who?" -- the question the first round could not reach. A slow wake is two
         * 10 s lock timeouts, so whatever is named here is the actual defect. Empty fields mean
         * that particular take did NOT time out, which on a healthy wake is all of them. */
        EVENT_LOG_DEBUG(EVL_INFO, "uart lock: pre=%s rstTO=%s baudTO=%s",
                       hr.holder_before[0]  ? hr.holder_before  : "-",
                       hr.holder_rst_to[0]  ? hr.holder_rst_to  : "-",
                       hr.holder_baud_to[0] ? hr.holder_baud_to : "-");
    }
    #undef SLEEP_RESUME_LAP

    /* 7. STAYS LAST. Publish the state FIRST, then release the tasks parked on the awake bit. The order
     *    matters: the elm327 task wakes on DEV_AWAKE_BIT, pulls a queued command, then checks
     *    the sleep-state queue and DISCARDS the command if it still reads SLEEPING
     *    (elm327.c:1657-1663). Setting the bit first opens a window where the first command
     *    after every wake is silently dropped.
     *
     *    This must stay the FINAL step of the resume, however the steps above are reordered. The
     *    LED moved to the top and the chip reset moved to the bottom precisely because they could;
     *    the publish cannot, and moving it earlier "so the device looks awake sooner" reintroduces
     *    that dropped-command window on every single wake. */
    state_info->state   = STATE_NORMAL;
    state_info->voltage = battery_voltage;
    state_info->timer   = 0;   /* awake again: no countdown until the state machine arms a new one */
    xQueueOverwrite(sleep_state_queue, state_info);
    dev_status_set_awake();

    /* Record the two things that decide whether resuming in place stays safe over hundreds of
     * cycles: how close this task came to overflowing its stack during the WiFi bring-up, and
     * whether the heap is fragmenting. Both are silent failures otherwise -- the stack one
     * announces itself as a panic at a nonsense address, and the heap one as a resume that
     * quietly comes back with no network.
     *
     * #98 hid the routine numbers behind the debug gate (they are noise on a healthy device, and
     * GET /wake_probe reports the same three figures live at any time, debug on or off). What
     * must NEVER be hidden is the bad case, so the floor check below stays always-visible: an
     * overflow here has no fallback and no serial console to confess on. */
    const unsigned stack_free =
        (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));

    /* Latched once per boot, and that is exact rather than lazy: uxTaskGetStackHighWaterMark is a
     * HISTORIC MINIMUM, so within one uptime this condition can never clear and re-emitting on
     * every wake would just repeat the same number forever. The latched thing is itself
     * boot-scoped, so the usual "boot-scoped state breaks now that wake resumes in place" trap
     * does not apply here. It re-arms on any reboot, and a device in real stack trouble reboots
     * eventually -- by panicking. */
    if (stack_free < SLEEP_RESUME_STACK_WARN_MIN_FREE && !s_resume_stack_warned)
    {
        s_resume_stack_warned = true;
        event_log_emit(EVL_WARN, "resume stack low: %u B free (floor %u B)",
                       stack_free, (unsigned)SLEEP_RESUME_STACK_WARN_MIN_FREE);
    }

    /* Macro, so with debug off we do not even CALL heap_caps_get_largest_free_block() -- it walks
     * the heap free lists under the heap lock, and this is the resume path. */
    EVENT_LOG_DEBUG(EVL_INFO,
                    "resumed in place (stack free %u B, heap %u B, largest %u B)",
                    stack_free,
                    (unsigned)esp_get_free_heap_size(),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return true;
}

/* One CAN-wake confirmation pass, and the action it implies. Returns true when the caller should
 * wake the device; it does NOT wake it itself, and it always returns. Factored out because it is
 * needed in TWO places: before sleeping (when the pin is already LOW, so the wake condition may
 * already be true) and after a GPIO wake. */
static bool sleep_mode_can_wake_check(float battery_voltage)
{
    uint32_t cw_edges = 0, cw_rises = 0, cw_ms = 0;
    const can_wake_verdict_t cw = can_wake_confirm(&cw_edges, &cw_rises, &cw_ms);

    if(!can_wake_handle(cw, cw_edges, cw_rises, cw_ms))
    {
        return false;  /* quiet, stuck, or cooling down -- can_wake_handle() logged if it mattered */
    }

    /* Same hard floor the periodic path uses: never reboot into a brown-out. Cranking dips to
     * 9-11 V and clears within one 2 s cycle, so a deferral costs one cycle and the next pass
     * re-checks against a fresh ADC read. */
    if(battery_voltage <= CRITICAL_VOLTAGE)
    {
        ESP_LOGW(TAG, "CAN wake confirmed but %.2fV <= %.2fV -- deferring reboot",
                 battery_voltage, CRITICAL_VOLTAGE);
        event_log_emit(EVL_CAN_WAKE, "confirmed but volts %.2f <= %.2f, wake deferred",
                       battery_voltage, CRITICAL_VOLTAGE);
        return false;
    }

    /* Open the wake window. It is closed and scored at the next sleep entry, so this is correct
     * whether the caller goes on to resume in place or to take the reboot fallback. */
    can_wake_note_wake();
    return true;
}

/* Wake up: resume in place, and fall back to the historical reboot only if the resume fails.
 * The fallback is not optional -- this runs on a device with no WiFi and no serial console, so a
 * half-finished resume must always have somewhere safe to land. */
static system_state_t sleep_mode_wake_now(sleep_state_info_t *state_info, float battery_voltage,
                                          restart_tracker_planned_reason_t reason)
{
    if(sleep_mode_resume(state_info, battery_voltage))
    {
        return STATE_NORMAL;
    }

    ESP_LOGE(TAG, "resume failed -- rebooting instead");
    event_log_emit(EVL_INFO, "resume FAILED -> falling back to reboot");
    /* Let the line reach SD; restart_tracker_restart() calls esp_restart() immediately. */
    vTaskDelay(pdMS_TO_TICKS(SLEEP_EVENT_LOG_FLUSH_MS));
    restart_tracker_restart(reason, RESTART_TRACKER_SOURCE_SLEEP_MODE, RESTART_TRACKER_FLAG_NONE);
    return STATE_SLEEPING;   /* not reached -- esp_restart() does not return */
}

/* One line per veto EPISODE, never per pass (issue #4).
 *
 * This is called from the 500 ms sampling loop, so an unlatched line would write two entries a
 * second for the whole of every drive and bury everything else in the log -- the same per-CYCLE
 * mistake the wake-on-CAN work already had to fix once. The caller clears the latch as soon as
 * the ECU stops answering, so each continuous "held awake" period costs exactly one line.
 * Same shape as postpone_logged in the teardown.
 *
 * #98: this helper no longer emits any EVENT line at all -- it is the shared serial line plus the
 * shared latch, and nothing else. The event line that used to live here ("held awake -- ECU
 * answering") said almost nothing new: IGNITION_ON lands within ~2 s of it on every boot, the
 * voltage is already on /check_status, and "below the threshold but not counting down" is exactly
 * what GET /sleep_status now reports continuously as state:"normal".
 *
 * The countdown-cancelled event line lives at the STATE_LOW_VOLTAGE call site instead, next to the
 * countdown state it talks about. Keeping it out of here is what makes it one-shot BY CONSTRUCTION
 * (it is gated on cd.armed_us, which that site zeroes immediately) rather than by an
 * argument about this latch -- so a future change to the latch cannot silently cost us the only
 * evidence in the log that the #4/#97 ECU veto ever stopped a sleep mid-drive.
 *
 * The latch stays SHARED between the two call sites on purpose: without it this serial line would
 * print twice a second for the whole of every drive. */
static void sleep_log_ecu_veto(float volts, bool *logged)
{
    if (*logged) return;
    *logged = true;

    ESP_LOGI(TAG, "ECU answering at %.2fV -- holding off the sleep countdown (ignition is on)",
             (double)volts);
}

void light_sleep_task(void *pvParameters)
{
    static float battery_voltage = 0.0;
    esp_err_t ret = ESP_FAIL;
    system_state_t current_state = STATE_NORMAL;
    sleep_state_info_t state_info = {0};
    static uint8_t sleep_en = -1;
    float sleep_voltage;
    float wakeup_voltage;
    static wc_timer_t sleep_timer;
    static wc_timer_t wakeup_timer;
	static uint32_t sleep_time;
	static int8_t periodic_wakeup;
	static uint32_t wakeup_interval;
	static wc_timer_t periodic_wakeup_timer;
	/* #4: latch for the ECU-veto lines (serial at both sites, plus the "sleep countdown cancelled"
	 * event line at the STATE_LOW_VOLTAGE site -- see sleep_log_ecu_veto). Cleared every pass the
	 * ECU is NOT answering, so it is scoped to a veto episode and not to a boot -- this device
	 * resumes in place instead of rebooting, and boot-scoped state has already broken this file
	 * five separate times. That clearing is also what guarantees the cancel line can never be
	 * swallowed; the proof is at the call site. */
	static bool ecu_veto_logged = false;
	/* #92: the same latch for the host-claim veto. DELIBERATELY separate from ecu_veto_logged --
	 * the two causes are independent episodes, and sharing one latch would swallow whichever line
	 * came second whenever both vetoes overlapped. Cleared every pass the claim is not live, for
	 * the same resume-in-place reason as above. */
	static bool claim_veto_logged = false;
	/* Rate limit for the "sleep countdown started" line. Entering LOW_VOLTAGE is a genuinely new
	 * countdown every time, so a per-episode latch would be wrong -- but a battery sitting exactly
	 * on the threshold (a tender, or a cycling key-off load) can cross it every ~2.5 s, which would
	 * write hundreds of truthful-but-useless lines an hour to the SD card. One per minute is plenty
	 * to reconstruct what happened. Seeded negative so the first countdown always logs. */
	static int64_t countdown_logged_us = -SLEEP_COUNTDOWN_LOG_MIN_GAP_US;
	/* The CURRENT countdown, as one object so it cannot half-reset. Three loose variables with one
	 * shared validity condition is the shape that rots: the next person to add an exit path from
	 * STATE_LOW_VOLTAGE would clear one of three and leave the others stale. Clearing armed_us is
	 * the single "no countdown running" signal; arming assigns the whole struct at once.
	 *
	 *   armed_us      -- when this countdown was armed, 0 while none is running. Distinct from
	 *                    countdown_logged_us, which is a rate limit spanning MANY countdowns; this
	 *                    is per-countdown and decides whether this one is real enough to log.
	 *                    Deliberately an explicit stamp and NOT derived from (sleep_time -
	 *                    remaining): the #86 flash retry re-arms sleep_timer to SLEEP_FLASH_RETRY_MS
	 *                    rather than sleep_time, so the derived form would report a wrong age
	 *                    exactly when a flash overlaps a countdown -- the case nobody bench-tests.
	 *   volts         -- the reading that ARMED it. Kept because the "countdown started" line is
	 *                    emitted a few seconds later and must report what started it, not what the
	 *                    battery happens to read by then.
	 *   announce_done -- this countdown has been through the announce decision already. Note it is
	 *                    set even when the rate limit swallows the line, so it means "considered",
	 *                    not "printed" -- that is what keeps at most one announcement per countdown.
	 */
	static struct {
		int64_t armed_us;
		float   volts;
		bool    announce_done;
	} cd = {0};

    // Initialize configuration
    sleep_en = config_server_get_sleep_config();
    if(config_server_get_sleep_volt(&sleep_voltage) == -1)
	{
        sleep_voltage = 13.1f;
    }

    // if(config_server_get_wakeup_volt(&wakeup_voltage) == -1) 
	// {
    //     wakeup_voltage = 13.4f;
    // }
    wakeup_voltage = sleep_voltage + 0.1f;
    
	if(config_server_get_sleep_time(&sleep_time) == -1)
	{
		sleep_time = 120000;
		ESP_LOGE(TAG, "Failed to get sleep time");
	}
	else
	{
		sleep_time *= 60000; //change min to ms
	}

	periodic_wakeup = config_server_get_periodic_wakeup();

    if(periodic_wakeup == -1)
    {
        periodic_wakeup = 0;
    }

    if(config_server_get_wakeup_interval(&wakeup_interval) == -1)
    {
        wakeup_interval = 30*60000; //5 min
        ESP_LOGW(TAG, "Failed to get wakeup interval, using default 5 min");
    }
    else
    {
        wakeup_interval *= 60000; //change min to ms
    }

    // Create queues (static allocation)
    voltage_queue = xQueueCreateStatic(1, sizeof(float), voltage_queue_storage, &voltage_queue_struct);
    sleep_state_queue = xQueueCreateStatic(1, sizeof(sleep_state_info_t), sleep_state_queue_storage, &sleep_state_queue_struct);

    // Initialize ADC
    // calibration_init();
    // continuous_adc_init();
    oneshot_adc_init();
    // ESP_ERROR_CHECK(adc_continuous_start(handle));

    // Log initial configuration
    ESP_LOGI(TAG, "Sleep task started. Sleep enabled: %d, Sleep voltage: %.2f, Wakeup voltage: %.2f, Sleep time: %lu, Periodic wakeup: %d, Wakeup interval: %lu", 
             sleep_en, sleep_voltage, wakeup_voltage, sleep_time, periodic_wakeup, wakeup_interval);

    /* Shadow of DEV_WAKE_VOLTAGE_OK_BIT: -1 = not resolved yet, 0 = cleared, 1 = set.
     *
     * DEV_WAKE_VOLTAGE_OK_BIT is the only voltage compare with no hysteresis of its own, and
     * AutoPID parks on it with portMAX_DELAY -- so a bit that chatters parks and unparks that task
     * repeatedly. At 3 s that was rare; at 500 ms it would not be. The latch below only ever
     * changes on the two OUTER edges and holds inside the band. It needs one exception: on the
     * very first sample (and after any ADC failure gap) there is no previous state to hold, and
     * resolving an unknown state as "cleared" would park AutoPID forever on a device that booted
     * inside the band -- at a voltage that works fine today. So the first sample resolves against
     * sleep_voltage exactly as the old code did, and only later samples use the raised set edge.
     *
     * The shadow also keeps us off dev_status entirely on the passes that change nothing:
     * dev_status_set_bits()/clear_bits() log unconditionally at INFO, so calling them once per
     * sample inside a band would print twice a second forever. */
    int8_t volt_ok_shadow = -1;

    /* Consecutive fresh samples seen at or above wakeup_voltage while in STATE_LOW_VOLTAGE. */
    uint8_t volt_recover_count = 0;

    /* Boot-loop guard input, cached once. See the guard below for why this is not re-read.
     * restart_tracker_get_state() leaves the struct untouched on every failure path, so the
     * zero-init is what a failed read resolves to. */
    restart_tracker_state_t boot_state = {0};
    (void)restart_tracker_get_state(&boot_state);
    const uint32_t boot_unexpected_resets = boot_state.unexpected_reset_count;

    /* #4: tell the wake module whether THIS boot was caused by a CAN wake, so it can tell a
     * useful wake (engine started) from a fruitless one (bus chirped, nothing happened) and
     * throttle accordingly. The counter lives in RTC RAM and survives the wake reboot.
     * planned_reason lives on the history RECORD, not on the top-level state struct, so go
     * through the accessor rather than indexing history[] by hand. A failed read leaves the
     * zero-init, i.e. "not a CAN wake" -- the conservative answer. */
    restart_tracker_record_t boot_rec = {0};
    (void)restart_tracker_get_latest_record(&boot_rec);
    can_wake_boot_init(boot_rec.was_planned &&
                       boot_rec.planned_reason == RESTART_TRACKER_PLANNED_REASON_CAN_WAKE);

    vTaskDelay(pdMS_TO_TICKS(1000));
    while (1)
	{
        /* One ADC burst per loop pass. The pacing lives in ONE place, at the bottom of this loop:
         * awake, the vTaskDelay(VOLTAGE_READ_PERIOD_MS) makes this ~2 Hz; asleep, the 2 s
         * light-sleep wake makes it ~0.5 Hz. There is deliberately no separate poll timer. */

        /* True only on a pass whose reading actually landed. Anything counting consecutive
         * samples must count this, not loop passes, so an ADC failure cannot be mistaken for a
         * good sample repeated. */
        bool fresh_sample = false;

        // ret = sleep_mode_get_voltage(&battery_voltage);
        // ret = read_adc_voltage(&battery_voltage);
        ret = read_ss_adc_voltage(&battery_voltage);
        if(ret == ESP_OK)
        {
            fresh_sample = true;
            update_battery_voltage(&battery_voltage);

            /* Unresolved (-1) has no state to hold, so it resolves against sleep_voltage exactly
             * as the pre-hysteresis code did; once resolved, the set edge rises to wakeup_voltage
             * and the band between them holds. */
            const float set_edge = (volt_ok_shadow < 0) ? sleep_voltage : wakeup_voltage;
            int8_t volt_ok_want = volt_ok_shadow;

            if (battery_voltage < sleep_voltage)
            {
                volt_ok_want = 0;
            }
            else if (battery_voltage >= set_edge)
            {
                volt_ok_want = 1;
            }
            /* else: inside [sleep_voltage, wakeup_voltage) -> hold */

            if (volt_ok_want != volt_ok_shadow)
            {
                if (volt_ok_want == 1)
                {
                    dev_status_set_bits(DEV_WAKE_VOLTAGE_OK_BIT);
                }
                else
                {
                    dev_status_clear_bits(DEV_WAKE_VOLTAGE_OK_BIT);
                }
                volt_ok_shadow = volt_ok_want;
            }

            /* #4: credit a CAN wake that actually led somewhere.
             * This MUST live on the main sampling path, in ANY state -- not inside
             * case STATE_SLEEPING. A successful wake ends with the device AWAKE in STATE_NORMAL
             * when the engine starts, so a check confined to the sleeping state would never see
             * the recovery and would score every good morning as "fruitless". Three normal
             * mornings would then trip the cooldown and silently disable the feature on a car
             * that had started perfectly every time. */
            if (volt_ok_want == 1)
            {
                can_wake_note_voltage_ok();
            }
        }
        else
        {
            /* Lost the ADC. Whatever the bit says now is stale by the time reads resume, so
             * make the next good sample re-resolve from scratch instead of holding. */
            volt_ok_shadow = -1;
        }

        /* ---- #4 sleep veto: "the ECU is talking, so the ignition is on" -----------------
         * Sampled ONCE per pass so the credit below and the state machine cannot disagree
         * within a single iteration. Traffic-derived and voltage-free by design; it fails
         * closed when nothing maintains the signal -- see poll_log_ecu_answering(). */
        const bool ecu_answering = poll_log_ecu_answering();

        if (!ecu_answering)
        {
            ecu_veto_logged = false;   /* re-arm the one-line-per-episode latch */
        }

        /* ---- #92 sleep veto: "NC Flash is using the device right now" -------------------
         * Sampled ONCE per pass on the same rule as the ECU veto above, so the entry veto, the
         * cancel and the log line cannot disagree within one iteration.
         *
         * This is NOT redundant with the ECU veto -- it is the veto the ECU one gives up. The
         * instant a host claims the bus, can_should_park() goes true (can.c:255) and therefore
         * poll_log_ecu_answering() goes FALSE even with the engine running and the ECU answering
         * normally (poll_log.c:1826). That is deliberate -- poll_log.c:1815-1818 says it "hands
         * the decision back to the teardown's own bounded interlock instead" -- but before #92
         * that interlock only covered flash and OTA, so a plain bus claim (DTC reads, RAM ops,
         * the pre-flash auth window) fell through it and the device could sleep mid-session.
         *
         * Read the LEASE, never can_host_bus_claim_active(): the raw flag stays set until the
         * dead-man reaper clears it, and that reap waits for the bus to go idle -- which, key-on,
         * means key-off (#70). Vetoing on the raw flag would hide a device that never sleeps
         * behind a UI reporting NORMAL. The lease self-bounds, with the caveat spelled out at the
         * teardown gate: the host renews every 4 s against a 75 s TTL, so this drops within the
         * TTL once the owning 35001 socket is gone (~20 s of TCP keepalive covers every way the
         * host machine dies) -- but for as long as that socket stays open it holds. */
        can_coexist_snapshot_t sleep_coexist;
        can_coexist_snapshot(&sleep_coexist);
        const bool claim_live = sleep_coexist.host_bus_claimed &&
                                (!sleep_coexist.claim_expired || sleep_coexist.claim_owner_alive);
        /* The park counts too, on the same lease-validity rule and for the same reason: a park
         * being RENEWED means a host is working right now, and sleeping would cut its connection.
         * Note this is not the "do not block sleep on a park" rule from the teardown design -- that
         * was about the RAW flag, which a dead host leaves raised until the reaper clears it. The
         * park TTL is only 12 s (can.h:94) against the same 4 s host keepalive, so a dead host's
         * park goes stale FASTER than its claim does: this tightens the battery bound rather than
         * loosening it. */
        const bool park_live  = sleep_coexist.datalog_parked &&
                                (!sleep_coexist.park_expired || sleep_coexist.park_owner_alive);
        const bool session_live = claim_live || park_live;

        if (!session_live)
        {
            claim_veto_logged = false;   /* independent episode from the ECU latch */
        }

        /* The same credit the voltage path gives just above, on the other proof. Deliberately
         * OUTSIDE the ADC-success block: an answering ECU shows the wake led somewhere whether or
         * not this pass managed to read the battery, and tying it to a good ADC read would let a
         * failing ADC block the one proof that does not need the ADC. The asymmetry with the
         * voltage credit (which sits inside the fresh-sample branch) is deliberate, not an
         * oversight -- do not "tidy" it by moving this inside. */
        if (ecu_answering)
        {
            can_wake_note_ecu_ok();
        }

        if (ret == ESP_OK && sleep_en == 1)
		{
            // State machine logic
            switch (current_state) 
			{
                case STATE_NORMAL:
                    if (battery_voltage < sleep_voltage)
					{
                        /* #4: an answering ECU means the ignition is on, so the countdown never
                         * starts and the reported state stays honestly NORMAL. Voltage alone used
                         * to decide this, and it cost a real drive: with sleep_volt above the
                         * alternator's output the dongle read "engine off" the whole way, slept a
                         * minute in, and the fruitless throttle then disabled wake-on-CAN for an
                         * hour. No voltage floor and no time cap here on purpose -- with the
                         * ignition on the car itself draws amps, so bounding our tens of mA buys
                         * nothing. */
                        if (ecu_answering)
                        {
                            /* Serial only (#98): nothing started, so there is nothing to report
                             * that IGNITION_ON and /sleep_status do not already say. */
                            sleep_log_ecu_veto(battery_voltage, &ecu_veto_logged);
                        }
                        else if (session_live)
                        {
                            /* #92: same reasoning, other cause -- serial only, because nothing was
                             * started and /datalog already reports the claim. Touch NO cd fields
                             * here: no countdown exists, cd.armed_us is already 0, and the arm
                             * below assigns the whole struct in one go. */
                            if (!claim_veto_logged)
                            {
                                claim_veto_logged = true;
                                ESP_LOGW(TAG, "Battery low (%.2fV) but NC Flash is using the device "
                                              "-- sleep countdown not started (#92)", battery_voltage);
                            }
                        }
                        else
                        {
                            ESP_LOGW(TAG, "Battery voltage low (%.2fV), starting low voltage timer", battery_voltage);
                            current_state = STATE_LOW_VOLTAGE;
                            volt_recover_count = 0;
                            wc_timer_set(&sleep_timer, sleep_time);
                            /* Stamp WHEN this countdown was armed. Both countdown event lines are
                             * deferred and keyed off this: see the SURVIVED-5s emit in
                             * STATE_LOW_VOLTAGE below, and sleep_log_ecu_veto for the cancel. */
                            /* One assignment, so no field can be left over from the last countdown. */
                            cd.armed_us      = esp_timer_get_time();
                            cd.volts         = battery_voltage;
                            cd.announce_done = false;
                        }
                    }
                    break;

                case STATE_LOW_VOLTAGE:
                    /* #4: the ECU started answering mid-countdown -- the key just went on.
                     * Abandon the countdown and go back to NORMAL. Re-entering later re-arms the
                     * FULL sleep_time, which is deliberate: "sleep N minutes after the car goes
                     * quiet", counted from ECU silence rather than from the voltage dipping. */
                    /* #92 shares this exact exit rather than adding a second one. The comment on
                     * the countdown struct warns that the next person to add an exit path from
                     * STATE_LOW_VOLTAGE would clear one of three fields and leave the others
                     * stale -- one shared path with two labels cannot drift that way. */
                    if (ecu_answering || session_live)
                    {
                        if (ecu_answering)
                        {
                            sleep_log_ecu_veto(battery_voltage, &ecu_veto_logged);
                        }
                        else if (!claim_veto_logged)
                        {
                            claim_veto_logged = true;
                            ESP_LOGW(TAG, "NC Flash claimed the bus mid-countdown (%.2fV) "
                                          "-- cancelling the sleep countdown (#92)", battery_voltage);
                        }

                        /* An ARMED countdown is being thrown away -- worth an event line (#98).
                         * This is the counterpart of "sleep countdown started" and the only proof
                         * in the log that the #4/#97 ECU veto stopped a sleep mid-drive.
                         *
                         * One-shot BY CONSTRUCTION, not by argument: the only guard is cd.armed_us,
                         * and it is zeroed three lines below, before any other pass can run. A
                         * countdown too short to matter earns nothing -- see SLEEP_COUNTDOWN_REAL_MS
                         * for the per-boot noise that suppresses. */
                        if (cd.armed_us != 0 &&
                            (esp_timer_get_time() - cd.armed_us) >=
                                ((int64_t)SLEEP_COUNTDOWN_REAL_MS * 1000))
                        {
                            if (ecu_answering)
                            {
                                event_log_emit(EVL_INFO,
                                               "sleep countdown cancelled -- ECU answering at %.2fV (ignition on)",
                                               (double)battery_voltage);
                            }
                            else
                            {
                                event_log_emit(EVL_INFO,
                                               "sleep countdown cancelled -- host session active at %.2fV (bus claim held)",
                                               (double)battery_voltage);
                            }
                        }
                        current_state      = STATE_NORMAL;
                        volt_recover_count = 0;
                        cd.armed_us        = 0;   /* no countdown is running any more */
                        break;
                    }

                    /* Announce the countdown once it has SURVIVED long enough to be real (#98).
                     *
                     * This line used to be emitted the instant the countdown was armed, which broke
                     * in two ways at once. Every boot arms a countdown ~1.2 s in (the ECU has not
                     * answered yet and the battery is below sleep_volt whenever the engine is not
                     * turning) and cancels it ~2 s later -- so the log always opened with a
                     * countdown that never meant anything. Worse, that throwaway line ATE THE
                     * 60-SECOND RATE LIMIT below, so a REAL countdown starting within a minute of
                     * boot was silently swallowed and the device then slept with nothing in the log
                     * to say a countdown had ever started. That is visible in real bench logs:
                     * "countdown started" at up=1.2s, quiesce at up=22s, then "entering sleep" at
                     * up=83s with no second countdown line.
                     *
                     * Deferring the announcement fixes both: the boot countdown is gone before it
                     * qualifies, so it costs no line AND no rate-limit budget, and the rate limit
                     * now only ever applies between countdowns that were actually real.
                     *
                     * Reports the voltage the countdown was ARMED at, not the current one, so the
                     * line still answers "what reading started this?".
                     *
                     * The rate limit itself stays: a battery parked exactly on sleep_volt can still
                     * cross the threshold repeatedly, and each crossing really is a fresh countdown,
                     * so this needs a rate limit rather than a latch.
                     *
                     * Do NOT flatten these two ifs into one && chain: announce_done is set BEFORE
                     * the rate-limit test on purpose, so a countdown the limit swallows is done with
                     * for good. Flattened, it would retry every pass and fire the line a minute into
                     * the countdown carrying a minute-stale arming voltage. */
                    const int64_t now_us = esp_timer_get_time();
                    if (!cd.announce_done && cd.armed_us != 0 &&
                        (now_us - cd.armed_us) >= ((int64_t)SLEEP_COUNTDOWN_REAL_MS * 1000))
                    {
                        cd.announce_done = true;
                        if ((now_us - countdown_logged_us) >= SLEEP_COUNTDOWN_LOG_MIN_GAP_US)
                        {
                            countdown_logged_us = now_us;
                            event_log_emit(EVL_INFO,
                                           "sleep countdown started -- %lu min at %.2fV (below %.2fV)",
                                           (unsigned long)(sleep_time / 60000UL),
                                           (double)cd.volts, (double)sleep_voltage);
                        }
                    }

                    /* Leaving LOW_VOLTAGE ABANDONS the sleep countdown, and re-entering re-arms
                     * the FULL sleep_time. On a single sample that is a trap now that readings are
                     * unrounded and 6x more frequent: one noise spike above wakeup_voltage throws
                     * away however much of the countdown had elapsed, and a car resting near the
                     * threshold could keep resetting it and never sleep at all -- which is exactly
                     * the parked-battery drain this state machine exists to prevent. Require the
                     * recovery to hold across VOLT_RECOVER_SAMPLES real samples (~2 s). */
                    if (fresh_sample)
                    {
                        volt_recover_count = (battery_voltage >= wakeup_voltage)
                                           ? (uint8_t)(volt_recover_count + 1) : 0;
                    }

                    if (volt_recover_count >= VOLT_RECOVER_SAMPLES)
					{
                        ESP_LOGI(TAG, "Battery voltage recovered (%.2fV)", battery_voltage);
                        current_state = STATE_NORMAL;
                        volt_recover_count = 0;
                        cd.armed_us = 0;   /* abandoned by voltage recovery */
                    }
                    else if (wc_timer_is_expired(&sleep_timer))
					{
                        ESP_LOGI(TAG, "Low voltage timeout expired, entering sleep mode");
                        if(sleep_mode_teardown(&state_info, battery_voltage, false))
                        {
                            current_state = STATE_SLEEPING;
                            cd.armed_us = 0;   /* it ran out; nothing left to cancel */
                            vTaskDelay(pdMS_TO_TICKS(1000));
                            if(periodic_wakeup)
                            {
                                wc_timer_set(&periodic_wakeup_timer, wakeup_interval);
                            }
                        }
                        else
                        {
                            /* #86: a flash owns the bus. Stay awake and ask again shortly --
                             * a flash is finite, so sleep is postponed, never cancelled. */
                            wc_timer_set(&sleep_timer, SLEEP_FLASH_RETRY_MS);
                        }
                    }
                    break;

                case STATE_SLEEPING:
                    if (battery_voltage >= wakeup_voltage) 
					{
                        ESP_LOGI(TAG, "Voltage above wakeup threshold, starting wakeup timer");
                        current_state = STATE_WAKE_PENDING;
                        wc_timer_set(&wakeup_timer, 1000); // 2 second timer for stable voltage
                    }
                    else if(battery_voltage > CRITICAL_VOLTAGE && periodic_wakeup && wc_timer_is_expired(&periodic_wakeup_timer))
                    {
                        ESP_LOGI(TAG, "Periodic wakeup timer expired, returning to normal mode");
                        // current_state = STATE_NORMAL;
                        restart_tracker_restart(RESTART_TRACKER_PLANNED_REASON_POWER_WAKE,
                                                RESTART_TRACKER_SOURCE_SLEEP_MODE,
                                                RESTART_TRACKER_FLAG_NONE);
                    }
                    break;

                case STATE_WAKE_PENDING:
                    if (battery_voltage < wakeup_voltage) 
					{
                        // Voltage dropped again, go back to sleep
                        current_state = STATE_SLEEPING;
                    }
                    else if (wc_timer_is_expired(&wakeup_timer)) 
					{
                        ESP_LOGI(TAG, "Voltage stable above threshold, returning to normal mode");
                        current_state = sleep_mode_wake_now(&state_info, battery_voltage,
                                                            RESTART_TRACKER_PLANNED_REASON_POWER_WAKE);
                    }
                    break;
            }

            /* Milliseconds left before this device sleeps, for GET /sleep_status and the UI's
             * countdown banner (#85/#98). Computed HERE because sleep_timer is a local of this
             * task and nothing else can see it.
             *
             * Publishing it through the existing 1-deep queue rather than exposing the deadline is
             * deliberate: a wc_timer_t is an int64_t, and a 64-bit read from the httpd task would
             * TEAR on this 32-bit core. xQueueOverwrite/xQueuePeek copy the whole struct under the
             * kernel's critical section, so a reader always sees `state` and `timer` that agree
             * with each other.
             *
             * Only LOW_VOLTAGE has a live countdown. Every other state reports 0, which is what
             * makes "not counting down" unambiguous on the wire. */
            const uint32_t remain_ms = (current_state == STATE_LOW_VOLTAGE)
                                     ? wc_timer_remaining_ms(&sleep_timer) : 0;

            // Update state info and send to queue
            state_info.state = current_state;
            state_info.voltage = battery_voltage;
            state_info.timer    = remain_ms;
            state_info.total_ms = sleep_time;   /* what this task actually armed, not a re-read */
            xQueueOverwrite(sleep_state_queue, &state_info);

            // Log current status
            /* LOGD, not LOGI: this fires once per loop pass, which is 2x/s after the sample-rate
             * change. At LOGI it floods the log and pushes out anything useful. */
            ESP_LOGD(TAG, "State: %d, Battery: %.2fV", current_state, battery_voltage);
        } 
        else if (ret != ESP_OK) 
		{
            ESP_LOGW(TAG, "Failed to read ADC: %d", ret);
        }

        // Handle sleep entry
        // if(current_state == STATE_SLEEPING && gpio_get_level(OBD_READY_PIN) == 1) 
        // {
        //     // adc_continuous_stop(handle);
        //     ESP_LOGW(TAG, "Sleep...");
        //     esp_sleep_enable_timer_wakeup(2*1000000);
        //     esp_light_sleep_start();
        //     ESP_LOGW(TAG, "Wakeup...");
        //     if(gpio_get_level(OBD_READY_PIN) == 0)
        //     {
        //         esp_restart();
        //     }
        //     // adc_continuous_start(handle);
        // }
        /* unexpected_reset_count is read ONCE, above the loop -- see boot_unexpected_resets. The
         * count only ever changes inside restart_tracker_init() at boot, so a cached copy is exact,
         * and restart_tracker_get_state() is not free: it CRC32s ~450 bytes of PSRAM and copies the
         * struct under a spinlock. Running that every pass was already wasteful; at the 500 ms loop
         * it would run twice as often, on a device with a history of interrupt_wdt panics in the SD
         * write path. The comparison below still runs every pass -- only its input is cached. */
        if(boot_unexpected_resets >= 3 && battery_voltage < ERROR_VOLTAGE && current_state != STATE_SLEEPING)
        {
            // Guard on !STATE_SLEEPING so this teardown runs once on entry, not every
            // loop pass while voltage stays in the low band (#47).
            /* Emergency path: force=true, which skips ONLY the flash interlock and always runs the
             * FULL teardown.
             * This must not merely ignore the return value. The interlock is the first thing in the
             * teardown, so a refusal would return having done NOTHING -- no STB raise, no
             * can_disable(), no wifi_mgr_deinit() -- and the loop below would then call
             * esp_light_sleep_start() on a fully live system: an in-flight flash frozen mid-write
             * (the exact PCM brick #86 exists to prevent), WiFi light-slept while started, and a
             * wake armed on a pin TWAI still owns. Forcing the teardown is what actually stops the
             * boot loop. A flash still running after 3 unexpected resets below ERROR_VOLTAGE is
             * already dead -- and repeated resets at low voltage are exactly what a crashing flash
             * session produces. */
            (void)sleep_mode_teardown(&state_info, battery_voltage, true);
            current_state = STATE_SLEEPING;
            printf("\r\nUnexpected reset count: %lu, entering sleep mode to prevent potential boot loop...\r\n", boot_unexpected_resets);
            led_pattern_ms_t breathing_pattern = {
                .rise_time_ms = 1000,    // 1 second fade in
                .hold_time_ms = 500,     // Hold for 0.5 seconds
                .fall_time_ms = 1000,    // 1 second fade out
                .off_time_ms = 3000,      // Off for 0.5 seconds
                .delay_time_ms = 0,      // No initial delay
                .repeat_times = 0        // Repeat forever
            };
            led_set_level(100, 0, 0);  // Set red color
            led_set_pattern_ms(LED_RED, &breathing_pattern);
            // Do NOT esp_light_sleep_start() here: no wakeup source is armed yet on
            // this boot (esp_sleep_enable_timer_wakeup runs in the STATE_SLEEPING block
            // below), so sleeping here would hang the device until a physical power
            // cycle. Fall through to that block, which arms the 2 s timer first (#47).
        }
        if(current_state == STATE_SLEEPING)
        {
            static wc_timer_t waketime = 0;
            /* DEBUG, not WARN: this fires every 2 s for the entire time the device is parked, and
             * formatting plus UART TX is real work on a box that has no serial console attached to
             * read it. The event log is where a parked device's story is actually recorded. */
            ESP_LOGD(TAG, "Sleep...");
            ESP_LOGD(TAG, "Wake time: %lld", (esp_timer_get_time()-waketime)/1000);

            /* Issue #4 wake-on-CAN. The 2 s timer below is armed unconditionally either way, so
             * the battery-voltage machinery is never affected by anything this feature does. */
            const can_wake_arm_t cw_arm = can_wake_arm();

            /* Pin was already LOW, so we did not arm. The wake condition may ALREADY be true --
             * decide right now rather than sleeping on it. */
            if(cw_arm == CAN_WAKE_ARM_SKIP_LOW && sleep_mode_can_wake_check(battery_voltage))
            {
                current_state = sleep_mode_wake_now(&state_info, battery_voltage,
                                                    RESTART_TRACKER_PLANNED_REASON_CAN_WAKE);
            }

            /* Everything below only applies if we are STILL going to sleep. Since a wake now
             * RESUMES in place instead of rebooting, the check above can return with the machine
             * fully awake -- WiFi re-initialising, producers unparked, bus enabled. Falling
             * through into esp_light_sleep_start() there would freeze a half-built WiFi stack
             * (beacons stop, the station association dies) and park poll_log mid-RX. On a device
             * in the car with no serial console that is an unreachable unit. */
            /* This recurring sleep needs NO flash/claim interlock, by invariant: the teardown has
             * already run, so WiFi is down (no HTTP, so no new bus-claim or OTA can arrive) and
             * CAN is disabled (so no flash can start). The only entry into this state is through
             * sleep_mode_teardown(), which is where both guards live. */
            if(current_state == STATE_SLEEPING)
            {
                esp_sleep_enable_timer_wakeup(2*1000000);
                esp_light_sleep_start();

                /* Disarm FIRST, before any branch. Wake-source flags persist across sleep calls,
                 * so a LOW-level trigger left armed on a LOW pin would livelock the loop. */
                can_wake_disarm();

                const esp_sleep_wakeup_cause_t cw_cause = esp_sleep_get_wakeup_cause();

                waketime = esp_timer_get_time();
                ESP_LOGW(TAG, "Wakeup...");

                /* ONLY confirm when the GPIO is what actually woke us.
                 * Sampling on every timer wake instead would busy-spin the full confirm window on
                 * a quiet bus (no edges means no early exit) once every 2 s, all night -- roughly
                 * an hour of full-CPU spinning per night and several mA of extra draw on a floor
                 * well under 1 mA. That would break the entire premise that watching the pin is
                 * free. Requiring `armed` as well as the cause bounds the one stale-cause case:
                 * esp_sleep_get_wakeup_cause() reports the LAST wake and would go stale if
                 * esp_light_sleep_start() ever declined to sleep.
                 * Placed before the ELM327 babysitting below, which can burn seconds on UART
                 * timeouts -- a bus that just came alive should not wait for it. */
                if(cw_arm == CAN_WAKE_ARM_OK && cw_cause == ESP_SLEEP_WAKEUP_GPIO
                   && sleep_mode_can_wake_check(battery_voltage))
                {
                    current_state = sleep_mode_wake_now(&state_info, battery_voltage,
                                                        RESTART_TRACKER_PLANNED_REASON_CAN_WAKE);
                }
            }
            else
            {
                /* Resumed before we ever slept. Nothing was armed on this pass -- SKIP_LOW is
                 * precisely the case where can_wake_arm() declined to arm -- but disarm anyway so
                 * the invariant "this block never exits with a wake source still armed" holds
                 * however the path above is edited later. */
                can_wake_disarm();
            }

            /* The ELM327 babysitter below is SLEEP maintenance, so it must
             * not run on a pass that just woke the device. sleep_mode_resume() hardresets the
             * interpreter chip to READY; the babysitter reads READY as "the chip failed to go to
             * sleep", hardresets it again, sends it STSLEEP0 and re-arms the GPIO7 pad holds. The
             * device would come back reporting NORMAL with its ELM327 port open onto a chip that
             * is asleep -- every ELM327 client talking to nothing until a manual reboot. poll_log
             * drives TWAI directly and needs that chip for nothing, which is why a bench soak on
             * the datalogger alone cannot see this. */
            if(current_state == STATE_SLEEPING)
            {
                /* --- The interpreter-chip babysitter -----------------------------------------
                 * If the OBD chip did not go to sleep, nudge it -- but do NOT escalate to a
                 * reboot. That escalation used to be the last resort and it was the wrong one:
                 *
                 *  - A reboot cannot fix any plausible cause. Bus traffic re-waking the chip, a
                 *    settle lag, a desynced UART -- every one of them survives a restart. So if
                 *    the condition persists the reboot RECURS: boot, count down, sleep, retry,
                 *    reboot, every few minutes, all night, in a parked car. That is precisely the
                 *    battery drain this whole feature exists to avoid, and it was observed once
                 *    on the bench.
                 *  - The cure costs more than the disease. Each retry is a chip hardreset plus
                 *    UART timeouts -- seconds of full-power activity. A chip left awake costs a
                 *    few mA. Six retries cost more than simply letting it be.
                 *  - Reboot-as-recovery already has a designated safe entry point: the next wake,
                 *    through sleep_mode_resume()'s fallback. This path does not need its own.
                 *
                 * So: at most two nudges per SLEEP SESSION, then log it and leave the chip alone
                 * until the next sleep entry. And at most two ABANDONED attempts per session as
                 * well, for the case where the UART lock is held and we never reach the chip --
                 * "failed, try again in 2 s" all night is its own battery drain.
                 *
                 * The first checks are deliberately SKIPPED. GPIO7 lags -- measured on the bench,
                 * it still reads "awake" immediately after a successful elm327_sleep() and only
                 * settles once the chip has actually powered down (~4010 ms, measured). Judging
                 * too early would hardreset a chip that was going to sleep on its own -- and a
                 * hardreset WAKES it, so the babysitter would be racing its own cause. */
                /* DIAGNOSTIC: the settle time nobody had measured. Log the FIRST pass on which
                 * GPIO7 finally reads asleep, and how long after elm327_sleep() that was. It came
                 * back at ~4010 ms / 1 pass, against a grace that allowed about 4 s -- which is
                 * why SLEEP_ELM327_SETTLE_PASSES is now 2. Latched, so it costs one line per sleep
                 * SESSION and not one per 2 s cycle. */
                s_elm327_sleep_passes++;

                /* Sample the pin ONCE per pass and use that one answer everywhere below.
                 * Re-reading it per branch let a single pass both log "settled asleep" and then
                 * nudge the chip, because GPIO7 can change between two reads microseconds apart --
                 * which is precisely the lag this whole block exists to tolerate. */
                const elm327_chip_status_t chip_status = elm327_chip_get_status();

                if(!s_elm327_asleep_logged && chip_status == ELM327_SLEEP)
                {
                    s_elm327_asleep_logged = true;
                    /* DEBUG-GATED (owner request): a normal, healthy settle is noise on the event
                     * page. The FAILURE case is not gated -- if the chip never settles, the
                     * babysitter's "would not sleep" line still fires unconditionally, so the bad
                     * news can never be hidden by a config flag. */
                    EVENT_LOG_DEBUG(EVL_INFO,
                                   "DIAG gpio7 settled asleep after %u ms / %u passes "
                                   "(grace allows %u passes; nudges used %u)",
                                   (unsigned)((uint32_t)(esp_timer_get_time() / 1000)
                                              - s_elm327_sleep_entry_ms),
                                   (unsigned)s_elm327_sleep_passes,
                                   (unsigned)SLEEP_ELM327_SETTLE_PASSES,
                                   (unsigned)s_elm327_sleep_nudges);
                }

                if(chip_status == ELM327_READY
                   && s_elm327_settle_passes < SLEEP_ELM327_SETTLE_PASSES)
                {
                    /* Give the pin a couple of full passes (~2 s each, so ~4-6 s since
                     * elm327_sleep()) to settle before believing it. Measured settle is ~4010 ms,
                     * so one pass left literally no margin. */
                    s_elm327_settle_passes++;
                }
                else if(chip_status == ELM327_READY)
                {
                    if(s_elm327_sleep_nudges < SLEEP_ELM327_MAX_NUDGES
                       && s_elm327_lock_fails < SLEEP_ELM327_MAX_LOCK_FAILS)
                    {
                        /* TWO different failures used to look identical here, and telling them
                         * apart is the point of this block.
                         *
                         * Old code called elm327_hardreset_chip() and elm327_sleep() and looked at
                         * neither result, so a nudge that never even got the UART lock -- chip
                         * untouched, nothing asked of it -- counted as a nudge, and after two of
                         * them the log said "MIC chip would not sleep". That message accused an
                         * innocent chip and cost an evening of investigation. Now: if we could not
                         * take the lock, we name the task holding it, spend a LOCK-FAIL credit
                         * rather than a nudge credit, and leave the chip alone. */
                        ESP_LOGW(TAG, "ELM327 chip still awake -- nudging (%u/%u)",
                                 (unsigned)(s_elm327_sleep_nudges + 1),
                                 (unsigned)SLEEP_ELM327_MAX_NUDGES);

                        (void)elm327_hardreset_chip_timeout(SLEEP_ELM327_LOCK_WAIT_MS);
                        elm327_hardreset_timing_t hr;
                        elm327_hardreset_get_timings(&hr);

                        /* mutex_ok alone, deliberately: the return value adds nothing here because
                         * a failed lock ALWAYS returns false (elm327.c, the early return on the
                         * !mutex_ok path), so testing both only reads as if two independent things
                         * were being checked. What we need to know is specifically "did we reach
                         * the chip at all", and that is exactly mutex_ok. */
                        if(!hr.mutex_ok)
                        {
                            /* Lock never obtained: the chip was not reset and must not be told to
                             * sleep either -- elm327_sleep() would just queue behind the same
                             * unavailable lock for another wait. One line per ATTEMPT, and the cap
                             * above keeps that to at most two lines per sleep session. */
                            s_elm327_lock_fails++;
                            event_log_emit(EVL_INFO,
                                           "nudge skipped: UART lock held by %s (%u/%u)",
                                           hr.holder_rst_to[0] ? hr.holder_rst_to : "?",
                                           (unsigned)s_elm327_lock_fails,
                                           (unsigned)SLEEP_ELM327_MAX_LOCK_FAILS);
                        }
                        else
                        {
                            /* We really did reach the chip, so this really is a nudge. */
                            s_elm327_sleep_nudges++;
                            vTaskDelay(pdMS_TO_TICKS(500));
                            const esp_err_t sleep_ret = elm327_sleep();
                            if(sleep_ret == ESP_ERR_TIMEOUT)
                            {
                                /* Reset got the lock, STSLEEP0 did not. Still not the chip's
                                 * fault, so name the holder rather than blaming it. */
                                char holder[16] = {0};
                                elm327_lock_holder_name(holder, sizeof(holder));
                                event_log_emit(EVL_INFO,
                                               "nudge %u: reset ok but UART lock held by %s at "
                                               "STSLEEP0", (unsigned)s_elm327_sleep_nudges, holder);
                            }
                            /* The GPIO7 pad holds that used to live here are gone on purpose. They
                             * were deep-sleep-flow leftovers doing nothing for this light-sleep
                             * loop, nothing ever released them, and gpio_deep_sleep_hold_en() is a
                             * GLOBAL flag affecting every held pad on the chip. The copies inside
                             * elm327_sleep() serve the deep-sleep failsafe and stay. */
                            vTaskDelay(pdMS_TO_TICKS(100));
                        }
                    }
                    /* Budget exhausted. The nudge check comes FIRST on purpose: if the chip really
                     * was reset and told to sleep twice and is still awake, that IS a chip refusal
                     * and deserves the accusing message, whatever else also ran out. Only when the
                     * nudges were never spent can "we never reached the chip" be the true story. */
                    else if(s_elm327_sleep_nudges == SLEEP_ELM327_MAX_NUDGES)
                    {
                        s_elm327_sleep_nudges++;   /* step past, so this logs once per session */
                        ESP_LOGW(TAG, "ELM327 chip will not sleep -- leaving it awake this session");
                        event_log_emit(EVL_INFO,
                                       "MIC chip would not sleep after %u nudges -- left awake "
                                       "(costs mA, NOT rebooting) [DIAG %u ms / %u passes since "
                                       "elm327_sleep(), gpio7=%d]",
                                       (unsigned)SLEEP_ELM327_MAX_NUDGES,
                                       (unsigned)((uint32_t)(esp_timer_get_time() / 1000)
                                                  - s_elm327_sleep_entry_ms),
                                       (unsigned)s_elm327_sleep_passes,
                                       (int)gpio_get_level(OBD_READY_PIN));
                    }
                    else if(s_elm327_lock_fails == SLEEP_ELM327_MAX_LOCK_FAILS)
                    {
                        /* Out of lock-fail credit. Step past so this logs once per session, and say
                         * plainly that the chip was never asked -- the OPPOSITE conclusion from the
                         * "would not sleep" line above, and the one the old code got wrong. */
                        s_elm327_lock_fails++;
                        char holder[16] = {0};
                        elm327_lock_holder_name(holder, sizeof(holder));
                        ESP_LOGW(TAG, "ELM327 nudges abandoned -- UART lock held (by %s)", holder);
                        /* Kept under EVENT_LOG_DETAIL_MAX (112) -- a longer line truncates away
                         * the holder name, which is the only new information in it. */
                        event_log_emit(EVL_INFO,
                                       "MIC left awake: UART lock held by %s after %u tries -- "
                                       "chip never asked, NOT rebooting",
                                       holder, (unsigned)SLEEP_ELM327_MAX_LOCK_FAILS);
                    }
                }
                else
                {
                    s_elm327_sleep_nudges  = 0;
                    s_elm327_settle_passes = 0;
                    s_elm327_lock_fails    = 0;
                }
            }
        }
        if(current_state != STATE_SLEEPING)
        {
            /* The ONLY thing setting the awake sample rate. There is no separate ADC poll timer
             * on purpose -- see VOLTAGE_READ_PERIOD_MS. */
            vTaskDelay(pdMS_TO_TICKS(VOLTAGE_READ_PERIOD_MS));
        }
        else
        {
            /* One-tick yield, NOT dead code: the sleeping path has no other delay, so if
             * esp_light_sleep_start() above ever returns without sleeping (reject/error -- its
             * return value is ignored), this is the only thing between us and a busy-spin that
             * starves the idle task and trips the task watchdog. Keep it at one tick: it runs
             * AWAKE between every 2 s sleep cycle, so a longer delay here directly raises the
             * parked battery drain that sleeping exists to prevent. */
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

esp_err_t sleep_mode_get_state(sleep_state_info_t *state_info)
{
    if (state_info == NULL) 
	{
        return ESP_ERR_INVALID_ARG;
    }
    
    if (sleep_state_queue == NULL) 
	{
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xQueuePeek(sleep_state_queue, state_info, 0) != pdTRUE) 
	{
        return ESP_ERR_NOT_FOUND;
    }
    
    return ESP_OK;
}

esp_err_t sleep_mode_get_voltage(float *val)
{
    if (voltage_queue != NULL)
    {
        if (xQueuePeek(voltage_queue, val, 0) == pdTRUE)
        {
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void sleep_mode_print_wakeup_reason(void)
{
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    switch(wakeup_reason)
    {
        case ESP_SLEEP_WAKEUP_EXT0:
            ESP_LOGI(TAG, "Wake up from ext0");
            break;
        case ESP_SLEEP_WAKEUP_EXT1:
            {
                uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
                if (wakeup_pin_mask != 0) 
				{
                    int pin = __builtin_ffsll(wakeup_pin_mask) - 1;
                    ESP_LOGI(TAG, "Wake up from GPIO %d", pin);
                } 
				else 
				{
                    ESP_LOGI(TAG, "Wake up from GPIO (pin not identified)");
                }
            }
            break;
        case ESP_SLEEP_WAKEUP_TIMER:
            ESP_LOGI(TAG, "Wake up from timer");
            break;
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            ESP_LOGI(TAG, "Wake up from touchpad");
            break;
        case ESP_SLEEP_WAKEUP_ULP:
            ESP_LOGI(TAG, "Wake up from ULP");
            break;
        case ESP_SLEEP_WAKEUP_GPIO:
            ESP_LOGI(TAG, "Wake up from GPIO");
            break;
        case ESP_SLEEP_WAKEUP_UART:
            ESP_LOGI(TAG, "Wake up from UART");
            break;
        default:
            ESP_LOGI(TAG, "Wake up not caused by deep sleep: %d", wakeup_reason);
            break;
    }
}

void sleep_mode_init(void)
{
	// if(config_server_get_sleep_config())
	{
		// xTaskCreate(sleep_task, "sleep_task", 4096, (void*)AF_INET, 5, NULL);
        /* StackType_t is uint8_t on Xtensa, so this array is BYTES, not words: 8 KB, raised from
         * the original 4 KB.
         *
         * Why it had to grow: this task now RESUMES from sleep instead of rebooting, which means
         * the whole network bring-up -- wifi_network_init() -> wifi_mgr_init() -> esp_wifi_init()
         * -> esp_wifi_start() -- runs on this stack. That work used to happen only in app_main,
         * which gets CONFIG_ESP_MAIN_TASK_STACK_SIZE = 5120 bytes and survived it. Doing it in
         * 4 KB, on top of the CAN re-enable and the ELM327 hard reset, left no margin at all.
         * Overflowing a STATIC stack does not trip the usual canary cleanly -- it corrupts the
         * neighbouring .bss and typically panics with a jump to a nonsense address.
         *
         * Sized at 10 KB rather than 8: measured baseline is ~2.7 KB before any resume, the
         * wifi_mgr_config_t alone is ~760 bytes of stack inside wifi_network_init(), and
         * esp_wifi's own init/start work lands on the caller's stack on top of that -- an
         * estimated peak near 6 KB. This task is the most safety-critical one on the device: if
         * IT overflows, the device may stop sleeping or stop waking, and it fails as a panic at
         * a nonsense address, possibly while unreachable in a car. A couple of KB of static RAM
         * buys the question outright. /wake_probe reports the live headroom as
         * sleep_task_stack_free, and every resume checks it against
         * SLEEP_RESUME_STACK_WARN_MIN_FREE. */
        static StackType_t light_sleep_task_stack[10240];
        static StaticTask_t light_sleep_task_buffer;

        // Create static task
        TaskHandle_t sleep_task_handle = xTaskCreateStatic(
            light_sleep_task,
            "sleep_task",
            sizeof(light_sleep_task_stack),
            (void*)AF_INET,
            5,
            light_sleep_task_stack,
            &light_sleep_task_buffer
        );
        
        if (sleep_task_handle == NULL)
        {
            ESP_LOGE(TAG, "Failed to create light sleep task");
            return;
        }
	}
}

#endif