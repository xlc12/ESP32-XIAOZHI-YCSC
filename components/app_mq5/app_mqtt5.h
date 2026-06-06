//file: app_mqtt5.h
#ifndef APP_MQTT5_H
#define APP_MQTT5_H

#ifdef __cplusplus
extern "C" {
#endif
#define VER_MQTT5 "1.0.11"
#define TEST_MQTT5_PERIODIC_REPORT
#include "app_mq_data.h"

int app_mqtt5_set_cfg(char *client_id, char *username, char *password, char *uuid);
void test_app_mqtt5(void);
const char* app_mqtt5_get_device_id(void);
const char* app_mqtt5_get_uuid(void);

int app_mqtt5_dev2server(const char *msg);
int app_mqtt5_dev2app(const char *msg);


#ifdef __cplusplus
}
#endif //__cplusplus

#endif

