idf_component_register(SRCS "app_timer_manager.c" 
                            "app_mq_data.c"
                            "app_mqtt5.c"
                        INCLUDE_DIRS "."
                        REQUIRES mqtt nvs_flash esp_netif json)



