/* MQTT Broker Subscriber

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"

#include "mongoose.h"

#if CONFIG_SUBSCRIBE

#define SUB_QUEUE_SIZE 20

static const char *sub_topic = "esp32";
static const char *will_topic = "WILL";

typedef struct _topic_t
{
	char *name;
//	uint8_t no_arg;
//	void(*func)(int argc, char **argv);
	struct _topic_t *next;
} topic_t;

static topic_t *topic_tbl, *topic_tbl_list = NULL;
//bool subscribed = false;
static QueueHandle_t mqttSubQueue;

static EventGroupHandle_t s_wifi_event_group;
/* The event group allows multiple bits for each event, but we only care about one event
 * - are we connected to the MQTT? */
static int MQTT_CONNECTED_BIT = BIT0;
static int MQTT_SUBSCRIBED_BIT = BIT1;

static void fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
  if (ev == MG_EV_ERROR) {
	// On error, log error message
	ESP_LOGE(pcTaskGetName(NULL), "MG_EV_ERROR %p %s", c->fd, (char *) ev_data);
	xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT | MQTT_SUBSCRIBED_BIT);
  } else if (ev == MG_EV_CONNECT) {
	ESP_LOGI(pcTaskGetName(NULL), "MG_EV_CONNECT");
	// If target URL is SSL/TLS, command client connection to use TLS
	if (mg_url_is_ssl((char *)fn_data)) {
	  struct mg_tls_opts opts = {.ca = "ca.pem"};
	  mg_tls_init(c, &opts);
	}
  } else if (ev == MG_EV_MQTT_OPEN) {
	ESP_LOGI(pcTaskGetName(NULL), "MG_EV_OPEN");
	// MQTT connect is successful
	ESP_LOGI(pcTaskGetName(NULL), "CONNECTED to %s", (char *)fn_data);
	xEventGroupSetBits(s_wifi_event_group, MQTT_CONNECTED_BIT);


#if 0
	struct mg_str topic = mg_str(sub_topic);
	struct mg_str data = mg_str("hello");
	mg_mqtt_sub(c, &topic, 1);
	ESP_LOGI(pcTaskGetName(NULL), "SUBSCRIBED to %.*s", (int) topic.len, topic.ptr);
#endif

#if 0
	mg_mqtt_pub(c, &topic, &data);
	LOG(LL_INFO, ("PUBSLISHED %.*s -> %.*s", (int) data.len, data.ptr,
				  (int) topic.len, topic.ptr));
#endif

  } else if (ev == MG_EV_MQTT_MSG) {
	// When we get echo response, print it
	struct mg_mqtt_message *mm = (struct mg_mqtt_message *) ev_data;
	ESP_LOGI(pcTaskGetName(NULL), "RECEIVED %.*s <- %.*s", (int) mm->data.len, mm->data.ptr,
				  (int) mm->topic.len, mm->topic.ptr);
	uint8_t* linePtr = (uint8_t*) malloc(mm->topic.len + mm->data.len + 4);
	if( linePtr == NULL) {
		ESP_LOGE( pcTaskGetName(NULL), "Error occurred during reception: Memory could not be allocated");
	} else {
        memcpy( linePtr, mm->topic.ptr, mm->topic.len);
        linePtr[mm->topic.len] = ',';
        memcpy( linePtr + mm->topic.len + 1, mm->data.ptr, mm->data.len);
        memcpy( linePtr + mm->topic.len + mm->data.len + 1, "\r\n\0", 3);
		if( xQueueSend( mqttSubQueue, ( void * ) &linePtr, 0) != pdPASS) {
			free( linePtr);
			ESP_LOGE(pcTaskGetName(NULL), "Error occurred during reception: subQueue full");
		} else {
			ESP_LOGI( pcTaskGetName(NULL), "SUBCRIPTION received: %.*s -> %.*s",
					mm->data.len, mm->data.ptr,	mm->topic.len, mm->topic.ptr);
		}
	}

  } else if (ev == MG_EV_ERROR || ev == MG_EV_CLOSE) {
		xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
  }
}

void mqtt_add_sub_topic( char* name)
{

	// alloc memory for topic struct
	topic_tbl = (topic_t *)malloc(sizeof(topic_t));

	if (topic_tbl == NULL)
	{
		ESP_LOGE(pcTaskGetName(NULL), "mqtt_add_sub_topic 0 malloc could not allocate memory");
	}
	// alloc memory for topic name
	char *topic_name = (char *)malloc(strlen(name) + 1);

	if (topic_name == NULL)
	{
		ESP_LOGE(pcTaskGetName(NULL), "mqtt_add_sub_topic 1 malloc could not allocate memory");
	}
	// copy name
	strcpy(topic_name, name);

	topic_tbl->name = topic_name;
	topic_tbl->next = topic_tbl_list;
	topic_tbl_list = topic_tbl;
	xEventGroupClearBits(s_wifi_event_group, MQTT_SUBSCRIBED_BIT);

}

char* mqtt_get_sub_line(void) {
	char* line_ptr;
	if( xQueueReceive( mqttSubQueue, &line_ptr, 0) == pdPASS)
		return (line_ptr);
	else return NULL;
}

void mqtt_subscriber(void *pvParameters)
{
	char *task_parameter = (char *)pvParameters;
	ESP_LOGD(pcTaskGetName(NULL), "Start task_parameter=%s", task_parameter);
	char url[64];
	strcpy(url, task_parameter);
	ESP_LOGI(pcTaskGetName(NULL), "started on %s", url);

	mqttSubQueue = xQueueCreate( SUB_QUEUE_SIZE, sizeof(char*));

	if (mqttSubQueue == 0) {
        ESP_LOGE(pcTaskGetName(NULL), "Unable to create QUEUE");
        return;
	}

	/* Starting Subscriber */
	struct mg_mgr mgr;
	struct mg_mqtt_opts opts;  // MQTT connection options
	//bool done = false;		 // Event handler flips it to true when done
	mg_mgr_init(&mgr);		   // Initialise event manager
	memset(&opts, 0, sizeof(opts));					// Set MQTT options
	//opts.client_id = mg_str("SUB");				// Set Client ID
	opts.client_id = mg_str(pcTaskGetName(NULL));	// Set Client ID
	opts.will_qos = 1;									// Set QoS to 1
	opts.will_topic = mg_str(will_topic);			// Set last will topic
	opts.will_message = mg_str("goodbye");			// And last will message

	// Connect address is x.x.x.x:1883
	// 0.0.0.0:1883 not work
	ESP_LOGD(pcTaskGetName(NULL), "url=[%s]", url);
	//static const char *url = "mqtt://broker.hivemq.com:1883";
	//mg_mqtt_connect(&mgr, url, &opts, fn, &done);  // Create client connection
	//mg_mqtt_connect(&mgr, url, &opts, fn, &done);  // Create client connection
	struct mg_connection *mgc;
	mgc = mg_mqtt_connect(&mgr, url, &opts, fn, &url);	// Create client connection

    /* Processing events */
	topic_t* topic_entry;
	s_wifi_event_group = xEventGroupCreate();
	uint16_t reconnect_interval = 0;

	while (1) {
		EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
			MQTT_CONNECTED_BIT,
			pdFALSE,
			pdTRUE,
			0);
		if ((bits & MQTT_CONNECTED_BIT) != 0) {
			if((bits & MQTT_SUBSCRIBED_BIT) == 0) {
				ESP_LOGI(pcTaskGetName(NULL), "Populating topics...");
				for (topic_entry = topic_tbl; topic_entry != NULL; topic_entry = topic_entry->next) {

					struct mg_str topic = mg_str(topic_entry->name);
					//mg_mqtt_sub(mgc, &topic);
					mg_mqtt_sub(mgc, &topic, 1);
					ESP_LOGI(pcTaskGetName(NULL), "Registering topic: %s", topic_entry->name);
				}
				xEventGroupSetBits(s_wifi_event_group, MQTT_SUBSCRIBED_BIT);
			}
		    reconnect_interval = 0;
		} else if(reconnect_interval++ > 3000) {

		    reconnect_interval = 0;
			xEventGroupClearBits(s_wifi_event_group, MQTT_SUBSCRIBED_BIT);
		    mgc = mg_mqtt_connect(&mgr, url, &opts, fn, &url);	// try to reconnect
		}

		mg_mgr_poll(&mgr, 0);
		vTaskDelay(1);
	}

	// Never reach here
	ESP_LOGI(pcTaskGetName(NULL), "finish");
	mg_mgr_free(&mgr);								// Finished, cleanup
}
#endif
