#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_spi_flash.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "driver/i2s.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "webpage.h"

static const char* TAG = "powermon";
#define V_REF   1100
#define ADC1_TEST_CHANNEL (ADC1_CHANNEL_7)
#define PARTITION_NAME   "powermon"

#define EXAMPLE_I2S_NUM           (0)
#define EXAMPLE_I2S_SAMPLE_RATE   (4000)
//i2s data bits
#define EXAMPLE_I2S_SAMPLE_BITS   (16)
//enable display buffer for debug
#define EXAMPLE_I2S_BUF_DEBUG     (0)
//I2S read buffer length
#define EXAMPLE_I2S_READ_LEN      (1024)
//I2S data format
#define EXAMPLE_I2S_FORMAT        (I2S_CHANNEL_FMT_ONLY_LEFT)
//I2S channel number
#define EXAMPLE_I2S_CHANNEL_NUM   (1)
//I2S built-in ADC unit
#define I2S_ADC_UNIT              ADC_UNIT_1
//I2S built-in ADC channel
#define I2S_ADC_CHANNEL           ADC1_CHANNEL_0

/**
 * @brief I2S ADC/DAC mode init.
 */
void example_i2s_init()
{
	 int i2s_num = EXAMPLE_I2S_NUM;
	 i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN,
        .sample_rate =  EXAMPLE_I2S_SAMPLE_RATE,
        .bits_per_sample = EXAMPLE_I2S_SAMPLE_BITS,
	    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
	    .channel_format = EXAMPLE_I2S_FORMAT,
	    .intr_alloc_flags = 0,
	    .dma_buf_count = 2,
	    .dma_buf_len = 1024
	 };
	 //install and start i2s driver
	 i2s_driver_install(i2s_num, &i2s_config, 0, NULL);
	 //init DAC pad
	 i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);
	 //init ADC pad
	 i2s_set_adc_mode(I2S_ADC_UNIT, I2S_ADC_CHANNEL);
}

#define MIN_MAX_LEN (10)
static uint32_t max_buffer[MIN_MAX_LEN];
static uint32_t min_buffer[MIN_MAX_LEN];
static uint32_t min_all_buffers;
static uint32_t max_all_buffers;
static uint32_t min_max_entry;
volatile static uint32_t pulses;
static uint32_t holdoff;
static uint32_t sensor_state;
static uint32_t first_samples = 1;
/****************************************************************************/
static void process_samples(uint16_t* buff, int length)
{
  unsigned min, max;
  unsigned set_point;
   
  if(max_all_buffers < 200) {
     set_point = 0;
  } else if(set_point > max_all_buffers-200) {
    set_point = max_all_buffers-200;
  }

  min = max = buff[0];
  for(int i = 1; i < length; i++) {
    if(buff[i] < min) min = buff[i];
    if(buff[i] > max) max = buff[i];
  }
  if(first_samples) {
    for(int i = 0; i < MIN_MAX_LEN; i++) {
      min_buffer[i] = min;
      max_buffer[i] = max;
    }
  } else {
    min_buffer[min_max_entry] = min;
    max_buffer[min_max_entry] = max;
  }
  first_samples = 0;

  min_max_entry++;
  if(min_max_entry == MIN_MAX_LEN) {
    min_max_entry = 0;
  }

  min_all_buffers = min_buffer[0];
  max_all_buffers = max_buffer[0];
  for(int i = 1; i < MIN_MAX_LEN; i++) {
    if(min_buffer[i] < min_all_buffers) min_all_buffers = min_buffer[i];
    if(max_buffer[i] > max_all_buffers) max_all_buffers = max_buffer[i];
  }

  if(max_all_buffers < 200)
    set_point = 100;  // Sensor error
  else if(max_all_buffers - min_all_buffers < 200) 
    set_point = max_all_buffers - 100;  // No pulses seen
  else
    set_point = (min_all_buffers + max_all_buffers)/2;


  // Deglitched data 
  for(int i = 0; i < length; i++) {
    if(sensor_state == 0) {
      if((buff[i] < set_point)) {
        if(holdoff > 0) {
	  holdoff--;
        } else {
          pulses++;
          ESP_LOGI(TAG, "Pulse detected");
	  sensor_state = 1;
	  holdoff = 100;
	}
      } else {
	  holdoff = 100;
      }
    }  else {
      if((buff[i] < set_point)) {
	  holdoff = 100;
      } else {
        if(holdoff > 0) {
	  holdoff--;
        } else {
	  sensor_state = 0;
	  holdoff = 100;
	}
      }
    }
  }

  gpio_pad_select_gpio(CONFIG_STATUS_PIN);
  gpio_set_direction(CONFIG_STATUS_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(CONFIG_STATUS_PIN, sensor_state ? 0 : 1);
}

/**
 * @brief debug buffer data
 */
void example_disp_buf(uint8_t* buf, int length)
{
#if EXAMPLE_I2S_BUF_DEBUG
    printf("======\n");
    for (int i = 0; i < length; i++) {
        printf("%02x ", buf[i]);
        if ((i + 1) % 8 == 0) {
            printf("\n");
        }
    }
    printf("======\n");
#endif
}

/**
 * @brief I2S ADC/DAC example
 *        1. Erase flash
 *        2. Record audio from ADC and save in flash
 *        3. Read flash and replay the sound via DAC
 *        4. Play an example audio file(file format: 8bit/8khz/single channel)
 *        5. Loop back to step 3
 */
void i2s_adc_read(void*arg)
{
    int i2s_read_len = EXAMPLE_I2S_READ_LEN;
    size_t bytes_read;
    char* i2s_read_buff;
    
    i2s_read_buff = (char*) calloc(i2s_read_len, sizeof(char));
    if(i2s_read_buff == NULL) {
        ets_printf("calloc() failed\n");
    }

    i2s_adc_enable(EXAMPLE_I2S_NUM);
    while(1) {
        //read data from I2S bus, in this case, from ADC.
        i2s_read(EXAMPLE_I2S_NUM, (void*) i2s_read_buff, i2s_read_len, &bytes_read, portMAX_DELAY);
        example_disp_buf((uint8_t*) i2s_read_buff, 64);
        //save original data from I2S(ADC) into flash.
        process_samples((uint16_t*) i2s_read_buff, i2s_read_len/2);
	
    }
    i2s_adc_disable(EXAMPLE_I2S_NUM);
    free(i2s_read_buff);
    i2s_read_buff = NULL;
}

#define MINUTE_SAMPLES (60)
static uint32_t  last_collection_sec;
static uint32_t minute_samples[MINUTE_SAMPLES];

#include "xtensa/hal.h"
extern uint32_t g_ticks_per_us_pro;

uint32_t get_second(void)
{
  return xthal_get_ccount() / (g_ticks_per_us_pro * 1000 * 1000);
}

void count_collector(void *arg) {
  while(1) {
    uint32_t sec;
    sec = xTaskGetTickCount() / configTICK_RATE_HZ;
    if(sec >= last_collection_sec+120)  {
      uint32_t reading = pulses;
      int i;
      last_collection_sec += 120;

      for(i = 0; i < MINUTE_SAMPLES-1; i++) {
        minute_samples[i] = minute_samples[i+1];
      }

      minute_samples[MINUTE_SAMPLES-1] = reading;
      webpage_add_point(minute_samples[i]);

      ESP_LOGI(TAG, "%d collection", sec);
    }
    vTaskDelay(500 / portTICK_RATE_MS);
  }
}

void adc_setup(void)
{
    uint32_t voltage;
    adc1_config_width(ADC_WIDTH_12Bit);
    adc1_config_channel_atten(ADC1_TEST_CHANNEL, ADC_ATTEN_11db);
    esp_adc_cal_characteristics_t characteristics;
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, V_REF, &characteristics);
    vTaskDelay(200 / portTICK_RATE_MS);
    esp_adc_cal_get_voltage(ADC1_TEST_CHANNEL, &characteristics, &voltage);
    ESP_LOGI(TAG, "%d mV", voltage);
    vTaskDelay(200 / portTICK_RATE_MS);
}

esp_err_t app_main()
{
    example_i2s_init();
    adc_setup();
    webpage_setup();
    esp_log_level_set("powermon", ESP_LOG_INFO);
    xTaskCreate(count_collector, "powermon count collector", 2048, NULL, 5, NULL);
    xTaskCreate(i2s_adc_read,    "powermon ADC read task", 2048, NULL, 5, NULL);
    return ESP_OK;
}
