#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_err.h"

#include "nvs_flash.h"

#include "http_server.h"
#include "webpage.h"

static void handle_page_request(http_context_t http_ctx, void* ctx);

static EventGroupHandle_t s_wifi_event_group;
const int CONNECTED_BIT = BIT0;
static ip4_addr_t s_ip_addr;

#define TAG "webpage"

#define N_POINTS (720)

static const char header[] = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"/><script type=\"application/javascript\">";
static uint32_t data_points[N_POINTS];
static uint32_t current_point = 0;

static const char footer[] = 
"function draw() {\n"
"  var canvas = document.getElementById('canvas');\n"
"  if (canvas.getContext) {\n"
"    var ctx = canvas.getContext('2d');\n"
"    var i;\n"
"    var scale_x, scale_y;\n"
"    var max_val, max_scale;\n"
"    var label;\n"
"\n"
"    scale_s = 10;\n"
"    scale_y = 10;\n"
"\n"
"    if(values.length > 1) {\n"
"      scale_x = 1440.0 / (values.length-1);\n"
"    }\n"
"\n"
"    max_val = Math.max(...values);\n"
"	total = 0;\n"
"    for(i = 0; i <  values.length; i++) {\n"
"	   total += values[i];\n"
"	}\n"
"    	\n"
"	average_power  = Math.round(total     * 30 / values.length);\n"
"	peak_power     = Math.round(max_val * 30);\n"
"    \n"
"    if(max_val * 30 < 1000)      {max_scale =   1.0; divs = 5}\n"
"    else if(max_val * 30 < 2000) {max_scale =   2.0; divs = 4}\n"
"    else if(max_val * 30 < 5000) {max_scale =   5.0; divs = 5}\n"
"    else if(max_val * 30 < 10000){max_scale =  10.0; divs = 5}\n"
"    else if(max_val * 30 < 20000){max_scale =  20.0; divs = 4}\n"
"    else if(max_val * 30 < 50000){max_scale =  50.0; divs = 5}\n"
"    else                          {max_scale = 100.0; divs = 5}\n"
"\n"
"    scale_y   = (30.0/1000)*720.0/max_scale;\n"
"\n"
"    ctx.fillStyle = 'rgb(80, 80, 80)';\n"
"    ctx.fillRect(0, 0, 1528, 840);\n"
"\n"
"    ctx.textAlign = 'center'; \n"
"    ctx.fillStyle = 'rgb(255, 255, 255)';\n"
"    ctx.font = '20px Arial';\n"
"    ctx.fillText('ESP32 Powermon : Household power usage over the last 24 hours',760,40);\n"
"    \n"
"    ctx.textAlign = 'center'; \n"
"    ctx.fillStyle = 'rgb(255, 255, 255)';\n"
"    ctx.font = '16px Arial';\n"
"    label = 'Average power usage is ';\n"
"    label = label.concat(average_power);\n"
"    label = label.concat(' W');\n"
"    ctx.fillText(label, 400, 820);\n"
"\n"
"    label = 'Peak power usage is ';\n"
"    label = label.concat(peak_power);\n"
"    label = label.concat(' W');\n"
"    ctx.fillText(label, 1120, 820);\n"
"	\n"
"	\n"
"    ctx.fillStyle = 'rgb(0, 0, 0)';\n"
"    ctx.fillRect(60, 60, 1440, 720);\n"
"\n"
"    ctx.strokeStyle = '#404040';\n"
"	for(i = 1; i < divs; i++) {\n"
"      ctx.moveTo(60,      780-(720/divs)*i)\n"
"      ctx.lineTo(60+1440, 780-(720/divs)*i);\n"
"      ctx.stroke();\n"
"	}\n"
"    ctx.font = '12px Arial';\n"
"	ctx.fillStyle = 'rgb(255, 255, 255)';\n"
"  	ctx.textAlign = 'end'; \n"
"	for(i = 0; i <= divs; i++) {\n"
"      label = '';\n"
"      label = label.concat(max_scale*i/divs);\n"
"      label = label.concat(' kW');\n"
"      ctx.fillText(label, 55, 785-(720/divs)*i);\n"
"	}\n"
"\n"
"	for(i = 1; i < 8; i++) {\n"
"      ctx.moveTo(60+i*180, 40)\n"
"      ctx.lineTo(60+i*180, 780);\n"
"      ctx.stroke();\n"
"	}\n"
"\n"
"    ctx.strokeStyle='#FFFF8F';\n"
"    ctx.beginPath();\n"
"    ctx.moveTo(60, 780-values[0]*scale_y)\n"
"    for(i = 1; i <  values.length; i++) {\n"
"      ctx.lineTo(60+scale_x*i, 780-values[i]*scale_y);\n"
"    }\n"
"    ctx.stroke();\n"
"  }\n"
"}\n"
"</script>\n"
"</head>\n"
"<body onload=\"draw();\">\n"
"  <canvas id=\"canvas\" width=\"1520\" height=\"840\"></canvas>\n"
"</body>\n"
"</html>\n";

/****************************************************************************/
void webpage_add_point(uint32_t count) {
  static uint32_t last = 0;
  static uint32_t first = 1;

  /* Get the starting point */
  if(first == 1) {
    last = count;
    first = 0;
    return;
  }

  /* Advance the pointe */
  current_point++;
  if(current_point == N_POINTS)
    current_point = 0;

  /* Update the pointer */
  data_points[current_point] = count - last;
  printf("add_point(%i)\n",count-last);

  /* Remember the last value */
  last = count;
}

/****************************************************************************/
static int webpage_write(http_context_t http_ctx, void* ctx) {
  int i,j;
  static const char varstart[] = "var values = [\n";
  char buffer[32];

  http_buffer_t fb_data;

  fb_data.data = header;
  fb_data.size = sizeof(header)-1;
  fb_data.data_is_persistent = true;
  http_response_write(http_ctx, &fb_data);

  fb_data.data = varstart;
  fb_data.size = sizeof(varstart)-1;
  fb_data.data_is_persistent = true;
  http_response_write(http_ctx, &fb_data);

  j = current_point+1;
  if(j == N_POINTS)
    j = 0;

  for(i = 0; i < N_POINTS-1; i++) {
    sprintf(buffer, "%i,", data_points[j]);
    fb_data.data = buffer; 
    fb_data.size = strlen(buffer);
    fb_data.data_is_persistent = false;
    http_response_write(http_ctx, &fb_data);

    j++;
    if(j == N_POINTS)
      j = 0;
  }
  sprintf(buffer, "%i];\n", data_points[j]);
  fb_data.data = buffer; 
  fb_data.size = strlen(buffer);
  fb_data.data_is_persistent = false;
  http_response_write(http_ctx, &fb_data);

  fb_data.data = footer;
  fb_data.size = sizeof(footer)-1;
  fb_data.data_is_persistent = true;
  http_response_write(http_ctx, &fb_data);
  return 0;
}
/****************************************************************************/
static void handle_page_request(http_context_t http_ctx, void* ctx)
{
    esp_err_t err;
    err = http_response_begin(http_ctx, 200, "text/html; charset=utf-8", HTTP_RESPONSE_SIZE_UNKNOWN);
    if(err) {
        ESP_LOGI(TAG, "Response failed");
    }
    webpage_write(http_ctx, ctx);
    http_response_end(http_ctx);
    ESP_LOGI(TAG, "Reply sent");
}


/****************************************************************************/
static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
            s_ip_addr = event->event_info.got_ip.ip_info.ip;
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            esp_wifi_connect();
            xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
            break;
        default:
            break;
    }
    return ESP_OK;
}

/****************************************************************************/
void webpage_setup(void)
{
    tcpip_adapter_init();
    nvs_flash_init();

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid =     CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
    ESP_ERROR_CHECK( esp_wifi_set_ps(WIFI_PS_NONE) );
    ESP_LOGI(TAG, "Connecting to \"%s\"", wifi_config.sta.ssid);
    xEventGroupWaitBits(s_wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Connected");

    http_server_t server;
    http_server_options_t http_options = HTTP_SERVER_OPTIONS_DEFAULT();
    ESP_ERROR_CHECK( http_server_start(&http_options, &server) );
    ESP_ERROR_CHECK( http_register_handler(server, "/index.html", HTTP_GET, HTTP_HANDLE_RESPONSE, &handle_page_request, NULL) );
}
/****************************************************************************/

