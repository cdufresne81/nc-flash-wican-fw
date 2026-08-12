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
static uint8_t s_elm327_sleep_nudges  = 0;
static uint8_t s_elm327_settle_passes = 0;

/* Shortest gap between two "sleep countdown started" event lines. Not a per-episode latch: every
 * entry into STATE_LOW_VOLTAGE really is a fresh countdown, so each line is true. This only stops
 * a battery parked exactly on sleep_volt from writing one every few seconds all night. */
#define SLEEP_COUNTDOWN_LOG_MIN_GAP_US  (60LL * 1000000LL)

/* How long to let the event-log writer reach the SD card before a restart wipes the RAM ring.
 * restart_tracker_restart() marks and then calls esp_restart() immediately, so any line emitted
 * just beforehand is lost without this. */
#define SLEEP_EVENT_LOG_FLUSH_MS 1500

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

    /* A firmware OTA is the same hazard against a different resource. The teardown's
     * wifi_mgr_deinit() pulls the network stack out from under a live HTTP upload; on the bench
     * that panicked the device at a non-code address twelve seconds after a sleep entry, with an
     * upload in flight. It does not brick anything -- the new image is only marked bootable at
     * the very end, so an interrupted upload just leaves the old firmware running -- but it makes
     * firmware updates fail by CRASHING rather than by returning an error, and it is easy to hit
     * whenever the sleep countdown is short. Shares the same bounded postpone as the ECU-flash
     * case below, so a stuck upload can never hold sleep off forever and flatten the battery. */
    const bool busy_flashing = can_flash_active() || config_server_ota_active();

    if (busy_flashing && !force)
    {
        const int64_t now = esp_timer_get_time();
        if (postpone_start_us == 0) postpone_start_us = now;

        /* The postpone MUST be bounded. FLASH_ACTIVE_BIT is codec-owned with no reaper, so a
         * codec that crashes or hangs with it raised would refuse sleep forever and flatten the
         * car battery over days, emitting a single log line the whole time. A real flash takes
         * minutes. Past the ceiling we assume it is stuck and sleep anyway: the PCM risk from
         * cutting a flash that has been frozen for half an hour is already realised -- that
         * transfer is dead either way -- whereas the battery is still savable. */
        if ((now - postpone_start_us) > (int64_t)SLEEP_FLASH_STUCK_CEILING_MS * 1000)
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
                else
                {
                    ESP_LOGW(TAG, "sleep postponed: firmware OTA upload in progress");
                    event_log_emit(EVL_INFO, "sleep postponed -- firmware OTA in progress (would have cut the upload)");
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

    /* Fresh budget for this sleep session: two nudges, and one pass of grace for GPIO7 to settle
     * before we believe it. Resetting HERE rather than relying on a function-static is the whole
     * point -- a static that is only ever cleared at boot means nothing now that a wake resumes. */
    s_elm327_sleep_nudges  = 0;
    s_elm327_settle_passes = 0;

    elm327_sleep();

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

    /* 1. Undo the pad hold that pins the OBD chip asleep. Instant (pin work only), and it must
     *    precede any attempt to talk to that chip. */
    elm327_release_sleep_hold();

    /* 2. BUS FIRST -- this is the whole point of resuming rather than rebooting.
     *    poll_log drives the TWAI controller directly and needs the interpreter chip for
     *    NOTHING, so getting the bus back before the chip handshake means logging restarts in
     *    single-digit milliseconds instead of waiting ~1-2 s (worst case ~4.5 s) for a chip that
     *    the datalogger does not use. can_enable() re-installs TWAI and drives the transceiver's
     *    standby pin low itself (can.c:438), reclaiming GPIO1 from the wake sampler. */
    can_enable();
    if(!can_is_enabled())
    {
        ESP_LOGE(TAG, "resume: can_enable() failed -- falling back to reboot");
        event_log_emit(EVL_INFO, "resume FAILED: can_enable() did not bring the bus back -- rebooting");
        return false;
    }

    /* 3. Only now release the parked producers. Bus first, fence second -- never the reverse,
     *    or an unparked task could transmit into a disabled controller. */
    can_sleep_fence_clear();
    csv_logger_set_sleep_requested(false);   /* the writer may open a fresh session again */

    /* 4. The slow part, deliberately after the bus is already live: the interpreter chip. A hard
     *    reset is what the existing retry path uses and the only sequence proven to bring it back
     *    from STSLEEP0. ~555 ms floor, ~1-1.8 s typical, ~4.5 s if its UART reads time out.
     *    Nothing touches this chip until the state publish at the end reopens its UART gate. */
    elm327_hardreset_chip();

    /* 5. Network back. wifi_mgr deinit/init at runtime is proven live: smartconnect does exactly
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

    led_indicator_resume();

    /* 6. Publish the state FIRST, then release the tasks parked on the awake bit. The order
     *    matters: the elm327 task wakes on DEV_AWAKE_BIT, pulls a queued command, then checks
     *    the sleep-state queue and DISCARDS the command if it still reads SLEEPING
     *    (elm327.c:1619-1625). Setting the bit first opens a window where the first command
     *    after every wake is silently dropped. */
    state_info->state   = STATE_NORMAL;
    state_info->voltage = battery_voltage;
    xQueueOverwrite(sleep_state_queue, state_info);
    dev_status_set_awake();

    /* Record the two things that decide whether resuming in place stays safe over hundreds of
     * cycles: how close this task came to overflowing its stack during the WiFi bring-up, and
     * whether the heap is fragmenting. Both are silent failures otherwise -- the stack one
     * announces itself as a panic at a nonsense address, and the heap one as a resume that
     * quietly comes back with no network. */
    event_log_emit(EVL_INFO,
                   "resumed in place (stack free %u B, heap %u B, largest %u B)",
                   (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
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

/* One event line per veto EPISODE, never per pass (issue #4).
 *
 * This is called from the 500 ms sampling loop, so an unlatched line would write two entries a
 * second for the whole of every drive and bury everything else in the log -- the same per-CYCLE
 * mistake the wake-on-CAN work already had to fix once. The caller clears the latch as soon as
 * the ECU stops answering, so each continuous "held awake" period costs exactly one line.
 * Same shape as postpone_logged in the teardown. */
static void sleep_log_ecu_veto(float volts, bool *logged)
{
    if (*logged) return;
    *logged = true;

    ESP_LOGI(TAG, "ECU answering at %.2fV -- holding off the sleep countdown (ignition is on)",
             (double)volts);
    event_log_emit(EVL_INFO, "staying awake -- ECU answering at %.2fV (ignition on)",
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
	/* #4: latch for the "staying awake -- ECU answering" event line. Cleared every pass the ECU
	 * is NOT answering, so it is scoped to a veto episode and not to a boot -- this device
	 * resumes in place instead of rebooting, and boot-scoped state has already broken this file
	 * five separate times. */
	static bool ecu_veto_logged = false;
	/* Rate limit for the "sleep countdown started" line. Entering LOW_VOLTAGE is a genuinely new
	 * countdown every time, so a per-episode latch would be wrong -- but a battery sitting exactly
	 * on the threshold (a tender, or a cycling key-off load) can cross it every ~2.5 s, which would
	 * write hundreds of truthful-but-useless lines an hour to the SD card. One per minute is plenty
	 * to reconstruct what happened. Seeded negative so the first countdown always logs. */
	static int64_t countdown_logged_us = -SLEEP_COUNTDOWN_LOG_MIN_GAP_US;

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
                            sleep_log_ecu_veto(battery_voltage, &ecu_veto_logged);
                        }
                        else
                        {
                            ESP_LOGW(TAG, "Battery voltage low (%.2fV), starting low voltage timer", battery_voltage);
                            /* Say WHEN, not just THAT. The quiesce line upstream is emitted by
                             * poll_log, which cannot know whether a countdown followed -- with the
                             * alternator up the ECU can fall silent and nothing starts at all. This
                             * is the only place that knows both the voltage and the configured
                             * sleep_time, so this is where the answer to "when does it sleep?"
                             * belongs. Rate limited: see countdown_logged_us. */
                            {
                                const int64_t now_us = esp_timer_get_time();
                                if ((now_us - countdown_logged_us) >= SLEEP_COUNTDOWN_LOG_MIN_GAP_US)
                                {
                                    countdown_logged_us = now_us;
                                    event_log_emit(EVL_INFO,
                                                   "sleep countdown started -- %lu min at %.2fV (below %.2fV)",
                                                   (unsigned long)(sleep_time / 60000UL),
                                                   (double)battery_voltage, (double)sleep_voltage);
                                }
                            }
                            current_state = STATE_LOW_VOLTAGE;
                            volt_recover_count = 0;
                            wc_timer_set(&sleep_timer, sleep_time);
                        }
                    }
                    break;

                case STATE_LOW_VOLTAGE:
                    /* #4: the ECU started answering mid-countdown -- the key just went on.
                     * Abandon the countdown and go back to NORMAL. Re-entering later re-arms the
                     * FULL sleep_time, which is deliberate: "sleep N minutes after the car goes
                     * quiet", counted from ECU silence rather than from the voltage dipping. */
                    if (ecu_answering)
                    {
                        sleep_log_ecu_veto(battery_voltage, &ecu_veto_logged);
                        current_state      = STATE_NORMAL;
                        volt_recover_count = 0;
                        break;
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
                    }
                    else if (wc_timer_is_expired(&sleep_timer))
					{
                        ESP_LOGI(TAG, "Low voltage timeout expired, entering sleep mode");
                        if(sleep_mode_teardown(&state_info, battery_voltage, false))
                        {
                            current_state = STATE_SLEEPING;
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

            // Update state info and send to queue
            state_info.state = current_state;
            state_info.voltage = battery_voltage;
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
                 * until the next sleep entry.
                 *
                 * The first check is deliberately SKIPPED. GPIO7 lags -- measured on the bench,
                 * it still reads "awake" immediately after a successful elm327_sleep() and only
                 * settles once the chip has actually powered down. enter_deep_sleep() allows
                 * 5000 ms for the same thing (:841). Judging on the first ~2 s pass would
                 * hardreset a chip that was going to sleep on its own -- and a hardreset WAKES
                 * it, so the babysitter would be racing its own cause. */
                if(elm327_chip_get_status() == ELM327_READY && s_elm327_settle_passes < 1)
                {
                    /* Give the pin one full pass (~2 s, so ~4 s since elm327_sleep()) to settle
                     * before believing it. */
                    s_elm327_settle_passes++;
                }
                else if(elm327_chip_get_status() == ELM327_READY)
                {
                    if(s_elm327_sleep_nudges < SLEEP_ELM327_MAX_NUDGES)
                    {
                        s_elm327_sleep_nudges++;
                        ESP_LOGW(TAG, "ELM327 chip still awake -- nudging (%u/%u)",
                                 (unsigned)s_elm327_sleep_nudges, (unsigned)SLEEP_ELM327_MAX_NUDGES);
                        elm327_hardreset_chip();
                        vTaskDelay(pdMS_TO_TICKS(500));
                        elm327_sleep();
                        /* The GPIO7 pad holds that used to live here are gone on purpose. They
                         * were deep-sleep-flow leftovers doing nothing for this light-sleep loop,
                         * nothing ever released them, and gpio_deep_sleep_hold_en() is a GLOBAL
                         * flag affecting every held pad on the chip. The copies inside
                         * elm327_sleep() serve the deep-sleep failsafe and stay. */
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    else if(s_elm327_sleep_nudges == SLEEP_ELM327_MAX_NUDGES)
                    {
                        s_elm327_sleep_nudges++;   /* step past, so this logs once per session */
                        ESP_LOGW(TAG, "ELM327 chip will not sleep -- leaving it awake this session");
                        event_log_emit(EVL_INFO,
                                       "MIC chip would not sleep after %u nudges -- left awake "
                                       "(costs mA, NOT rebooting)", (unsigned)SLEEP_ELM327_MAX_NUDGES);
                    }
                }
                else
                {
                    s_elm327_sleep_nudges  = 0;
                    s_elm327_settle_passes = 0;
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
         * sleep_task_stack_free, and every resume logs it. */
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