#include "esp_crc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "sdkconfig.h"

#include "driver/gpio.h"

#include "debug.h"
#include "espnow_esp.h"
#include "espnow_manage_data.h"
#include "main_settings.h"
#include "modbus_esp.h"
#include "uart_data.h"

uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF,
                                             0xFF, 0xFF, 0xFF};
xQueueHandle s_espnow_queue;

// static void espnow_deinit(espnow_send *send_param);

void init_gpio() {
  // zero-initialize the config structure.
  gpio_config_t io_conf = {};
  // disable interrupt
  io_conf.intr_type = GPIO_INTR_DISABLE;
  // set as output mode
  io_conf.mode = GPIO_MODE_OUTPUT;
  // bit mask of the pins that you want to set,e.g.GPIO18/19
  io_conf.pin_bit_mask = 1ULL << GPIO_NUM_2;
  // disable pull-down mode
  io_conf.pull_down_en = 0;
  // disable pull-up mode
  io_conf.pull_up_en = 0;
  // configure GPIO with the given settings

  gpio_reset_pin(GPIO_NUM_2);
  gpio_config(&io_conf);
  gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
  ESP_LOGI(TAG, "Started GPIO");
}

void init_uart() {
  uart_config_t uart_config = {
      //.baud_rate = 1200,
      .baud_rate = 9600,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      //.rx_flow_ctrl_thresh = 122,
      .source_clk = UART_SCLK_APB,
  };
  int intr_alloc_flags = 0;

#if CONFIG_UART_ISR_IN_IRAM
  intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif
  ESP_LOGI(TAG, "intr_alloc_flags %d", intr_alloc_flags);
  ESP_ERROR_CHECK(
      uart_driver_install(UART_NUM_2, 256, 0, 0, NULL, intr_alloc_flags));

  ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &uart_config));

  ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, 17, 16, -1, -1));

  // ESP_ERROR_CHECK(uart_set_mode(UART_NUM_2, UART_MODE_RS485_HALF_DUPLEX));

  // ESP_ERROR_CHECK(uart_set_rx_timeout(UART_NUM_2, 3));
  ESP_LOGI(TAG, "Started uart");
}

// Manage Wifi
static void wifi_init(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(ESPNOW_WIFI_MODE));
  ESP_ERROR_CHECK(esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_LR));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(TAG, "Started Wifi");
}

void espnow_deinit_func(espnow_send *send_param) {
  free(send_param->buffer);
  free(send_param);
  vSemaphoreDelete(s_espnow_queue);
  esp_now_deinit();
}

static esp_err_t espnow_init_minimal(void) {
  s_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_event_t));
  if (s_espnow_queue == NULL) {
    ESP_LOGE(TAG, "Create mutex fail");
    return ESP_FAIL;
  }

  /* Initialize ESPNOW and register sending and receiving callback function. */
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

  /* Set primary master key. */
  ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)ESPNOW_PMK));

  /* Add broadcast peer information to peer list. */
  espnow_addpeer(s_broadcast_mac);

  // xTaskCreate(espnow_task, "espnow_task", 2048, send_param, 4, NULL);

  ESP_LOGI(TAG, "Exiting espnow minimal init");
  return ESP_OK;
}

void app_main(void) {
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  wifi_init();

  unsigned char mac_base[6] = {0};
  esp_efuse_mac_get_default(mac_base);
  esp_read_mac(mac_base, ESP_MAC_WIFI_STA);
  unsigned char mac_local_base[6] = {0};
  unsigned char mac_uni_base[6] = {0};
  esp_derive_local_mac(mac_local_base, mac_uni_base);
  printf("Local Address: ");
  print_mac(mac_local_base);
  printf("\nUni Address: ");
  print_mac(mac_uni_base);
  printf("\nMAC Address: ");
  print_mac(mac_base);
  printf("\n");

  espnow_init_minimal();
  vTaskDelay(1500 / portTICK_RATE_MS);
  init_uart();
  vTaskDelay(1500 / portTICK_RATE_MS);
  init_gpio();

// https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/wdts.html#task-watchdog-timer-twdt
// Idk if needed:
// https://github.com/espressif/esp-idf/blob/495d35949d50033ebcb89def98f107aa267388c0/examples/system/task_watchdog/main/task_watchdog_example_main.c#L43

// Initialize watchdog
#if !CONFIG_ESP_TASK_WDT
  // If the TWDT was not initialized automatically on startup, manually
  // intialize it now
  esp_task_wdt_config_t twdt_config = {
      .timeout_ms = 45000,
      .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, // Bitmask of all cores
      .trigger_panic = true, // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/kconfig.html#config-esp-system-panic
  };
  ESP_ERROR_CHECK(esp_task_wdt_init(&twdt_config));
  ESP_LOGI(TAG, "TWDT initialized");
#endif

  bool modbus_communication_bool = true;
  bool espnow_esp_bool = false;

  if (modbus_communication_bool == true) {
    xTaskCreatePinnedToCore(modbus_communication, "modbus_communication", 32768,
                            xTaskGetCurrentTaskHandle(), 10, NULL, 0);
    ESP_LOGI(TAG, "created task - modbus communication");
  } else if (espnow_esp_bool == true) {
    xTaskCreatePinnedToCore(espnow_communication, "espnow_communication", 32768,
                            xTaskGetCurrentTaskHandle(), 10, NULL, 0);
    ESP_LOGI(TAG, "created task - espnow communication");
  }
}