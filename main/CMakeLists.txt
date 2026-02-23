idf_component_register(SRCS "app_main.cpp" "cJSON.c"
                            "flashmem.c" "factoryreset.c" "lorahandler.cpp"
							"apwebserver/server.c" "ota/ota.c" "statistics/statistics.c" "device/device.c"
		    REQUIRES oled_display RadioLib esp_wifi esp_app_format mqtt nvs_flash esp_eth esp_http_server app_update
		    esp_https_ota
		    INCLUDE_DIRS "."
            EMBED_TXTFILES ${project_dir}/server_certs/ca_cert.pem)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")

	    

