/* MQTT Broker Publisher

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

#if CONFIG_PUBLISH

//static const char *sub_topic = "#";
static const char *will_topic = "WILL";

static EventGroupHandle_t s_wifi_event_group;
/* The event group allows multiple bits for each event, but we only care about one event
 * - are we connected to the MQTT? */
static int MQTT_CONNECTED_BIT = BIT0;

#define TX_QUEUE_SIZE 20
static QueueHandle_t pubTxQueue;


static void fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
  if (ev == MG_EV_ERROR) {
	// On error, log error message
	ESP_LOGE(pcTaskGetName(NULL), "MG_EV_ERROR %p %s", c->fd, (char *) ev_data);
	xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
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
	mg_mqtt_sub(c, &topic);
	LOG(LL_INFO, ("SUBSCRIBED to %.*s", (int) topic.len, topic.ptr));
#endif

#if 0
	struct mg_str topic = mg_str("esp32"), data = mg_str("hello");
	mg_mqtt_pub(c, &topic, &data, 1, false);
	LOG(LL_INFO, ("PUBSLISHED %.*s -> %.*s", (int) data.len, data.ptr,
				  (int) topic.len, topic.ptr));
#endif

  } else if (ev == MG_EV_MQTT_MSG) {
	// When we get echo response, print it
	struct mg_mqtt_message *mm = (struct mg_mqtt_message *) ev_data;
	ESP_LOGI(pcTaskGetName(NULL), "RECEIVED %.*s <- %.*s", (int) mm->data.len, mm->data.ptr,
				  (int) mm->topic.len, mm->topic.ptr);
  }


  if (ev == MG_EV_ERROR || ev == MG_EV_CLOSE) {
		xEventGroupClearBits(s_wifi_event_group, MQTT_CONNECTED_BIT);
  }

}

void mqtt_publish_topic( char *topic_name, char * payload) {
    // stuff topic name and payload into a single memory block as follows
	// topic_name, payload

	uint8_t* pub_allocation = malloc( strlen(topic_name) + strlen(payload) + 2);
	if( pub_allocation == NULL) {
		ESP_LOGE(pcTaskGetName(NULL), "Error occurred in mqtt topic publication: Memory could not be allocated");
		return;
	}
	strcpy( (char*)pub_allocation, topic_name);
	strcpy( (char*)pub_allocation + strlen(topic_name) + 1, payload);

	uint8_t* dummy_ptr;
    if( xQueueSend( pubTxQueue, ( void * ) &pub_allocation, 0) != pdPASS ) {
    	// try to pop oldest publication if queue full
    	xQueueReceive( pubTxQueue, &dummy_ptr, 0);
		if(dummy_ptr != NULL) free(dummy_ptr);
	    if( xQueueSend( pubTxQueue, ( void * ) &pub_allocation, 0) != pdPASS ) {
			free( pub_allocation);
			ESP_LOGE(pcTaskGetName(NULL), "Error occurred in mqtt topic publication: pubTxQueue error");
			return;
	    }
    }

}

void mqtt_publisher(void *pvParameters)
{
	char *task_parameter = (char *)pvParameters;
	ESP_LOGD(pcTaskGetName(NULL), "Start task_parameter=%s", task_parameter);
	char url[64];
	strcpy(url, task_parameter);
	ESP_LOGI(pcTaskGetName(NULL), "started on %s", url);

	/* Starting Publisher */
	struct mg_mgr mgr;
	struct mg_connection *mgc;
	struct mg_mqtt_opts opts;  // MQTT connection options
	//bool done = false;		 // Event handler flips it to true when done
	mg_mgr_init(&mgr);		   // Initialise event manager
	memset(&opts, 0, sizeof(opts));					// Set MQTT options
	//opts.client_id = mg_str("PUB");				// Set Client ID
	opts.client_id = mg_str(pcTaskGetName(NULL));   // Set Client ID
	opts.will_qos = 1;									// Set QoS to 1
	opts.will_topic = mg_str(will_topic);			// Set last will topic
	opts.will_message = mg_str("goodbye");			// And last will message

	pubTxQueue = xQueueCreate( TX_QUEUE_SIZE, sizeof(char*));

	if ( pubTxQueue == 0) {
        ESP_LOGE(pcTaskGetName(NULL), "Unable to create publication txQUEUE");
        return;
	}

	// Connect address is x.x.x.x:1883
	// 0.0.0.0:1883 not work
	ESP_LOGD(pcTaskGetName(NULL), "url=[%s]", url);
	//static const char *url = "mqtt://broker.hivemq.com:1883";
	//mg_mqtt_connect(&mgr, url, &opts, fn, &done);  // Create client connection
	//mg_mqtt_connect(&mgr, url, &opts, fn, &done);  // Create client connection
	//mg_mqtt_connect(&mgr, url, &opts, fn, &url);	// Create client connection
	mgc = mg_mqtt_connect(&mgr, url, &opts, fn, &url);	// Create client connection

	/* Processing events */
	s_wifi_event_group = xEventGroupCreate();
//	int32_t counter = 0;
//	struct mg_str topic;
	char* pub_block;
	uint16_t reconnect_interval = 0;

	while (1) {
		EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
			MQTT_CONNECTED_BIT,
			pdFALSE,
			pdTRUE,
			0);
		if ((bits & MQTT_CONNECTED_BIT) != 0) {
			// connected: publish one topic if available
			if( xQueueReceive( pubTxQueue, &pub_block, 0) == pdPASS) {
				struct mg_str topic =  mg_str((char*)pub_block);
				struct mg_str payload =  mg_str((char*)( pub_block + strlen( pub_block) + 1));

				mg_mqtt_pub(mgc, &topic, &payload, 1, false);
				ESP_LOGI( pcTaskGetName(NULL), "PUBLISHED %.*s -> %.*s", payload.len,
				        payload.ptr, topic.len, topic.ptr);

				free(pub_block);
			}
			reconnect_interval = 0;
		} else if(reconnect_interval++ > 2000) {

			reconnect_interval = 0;
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
