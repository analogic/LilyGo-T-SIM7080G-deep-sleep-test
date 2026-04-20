#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

static const char *TAG = "sleep_test";

// PMU (AXP2101) on I2C bus 0
#define I2C_SDA_0       15
#define I2C_SCL_0        7

// Modem UART
#define MODEM_TXD_PIN    5
#define MODEM_RXD_PIN    4

static i2c_master_bus_handle_t i2c_bus_0 = NULL;
static XPowersPMU pmu;
static bool pmu_ready = false;

// ---------------------------------------------------------------------------
// Modem helpers
// ---------------------------------------------------------------------------

static esp_err_t modem_send_at(const char *cmd)
{
int written = uart_write_bytes(UART_NUM_1, cmd, strlen(cmd));
if (written < 0) {
return ESP_FAIL;
}
uart_write_bytes(UART_NUM_1, "\r\n", 2);
return ESP_OK;
}

static bool modem_test_at(void)
{
esp_err_t err = modem_send_at("AT");
if (err != ESP_OK) {
return false;
}
vTaskDelay(pdMS_TO_TICKS(200));
return true;
}

static void modem_poweroff_with_retry(void)
{
ESP_LOGI(TAG, "Powering off modem...");
for (int retries = 0; retries < 10; retries++) {
if (!modem_test_at()) {
ESP_LOGI(TAG, "Modem not responding. Modem turned off.");
return;
}
ESP_LOGW(TAG, "Modem still active... (attempt %d)", retries + 1);
vTaskDelay(pdMS_TO_TICKS(1000));
}
modem_send_at("+CPOWD=1");
vTaskDelay(pdMS_TO_TICKS(1000));
ESP_LOGI(TAG, "Modem poweroff command sent.");
}

static esp_err_t modem_uart_init(void)
{
uart_config_t uart_config = {
.baud_rate = 115200,
.data_bits = UART_DATA_8_BITS,
.parity = UART_PARITY_DISABLE,
.stop_bits = UART_STOP_BITS_1,
.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
.rx_flow_ctrl_thresh = 0,
.source_clk = UART_SCLK_DEFAULT
};

esp_err_t ret = uart_param_config(UART_NUM_1, &uart_config);
if (ret != ESP_OK) {
ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
return ret;
}

ret = uart_set_pin(UART_NUM_1, MODEM_TXD_PIN, MODEM_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
if (ret != ESP_OK) {
ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
return ret;
}

ret = uart_driver_install(UART_NUM_1, 256, 256, 0, NULL, 0);
if (ret != ESP_OK) {
ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
return ret;
}

ESP_LOGI(TAG, "Modem UART initialized");
return ESP_OK;
}

// ---------------------------------------------------------------------------
// I2C helpers
// ---------------------------------------------------------------------------

static esp_err_t i2c_buses_init(void)
{
i2c_master_bus_config_t cfg;
memset(&cfg, 0, sizeof(cfg));
cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
cfg.i2c_port          = I2C_NUM_0;
cfg.sda_io_num        = static_cast<gpio_num_t>(I2C_SDA_0);
cfg.scl_io_num        = static_cast<gpio_num_t>(I2C_SCL_0);
cfg.glitch_ignore_cnt = 7;

esp_err_t ret = i2c_new_master_bus(&cfg, &i2c_bus_0);
if (ret != ESP_OK) {
ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
return ret;
}
ESP_LOGI(TAG, "I2C bus 0 initialized (SDA=%d SCL=%d)", I2C_SDA_0, I2C_SCL_0);
return ESP_OK;
}

// ---------------------------------------------------------------------------
// PMU helpers
// ---------------------------------------------------------------------------

static esp_err_t pmu_init(void)
{
if (!pmu.begin(i2c_bus_0, AXP2101_SLAVE_ADDRESS)) {
ESP_LOGE(TAG, "PMU init failed: AXP2101 not responding");
return ESP_FAIL;
}
pmu_ready = true;
ESP_LOGI(TAG, "PMU initialized successfully");
return ESP_OK;
}

static void pmu_setup_sleep(void)
{
if (!pmu_ready) {
ESP_LOGW(TAG, "PMU not ready, skipping sleep configuration");
return;
}

pmu.setChargingLedMode(XPOWERS_CHG_LED_BLINK_4HZ);

pmu.disableTemperatureMeasure();
pmu.disableBattDetection();
pmu.disableVbusVoltageMeasure();
pmu.disableBattVoltageMeasure();
pmu.disableSystemVoltageMeasure();

pmu.disableDC2();
pmu.disableDC3();
pmu.disableDC4();
pmu.disableDC5();

pmu.disableALDO1();
pmu.disableALDO2();
pmu.disableALDO3();
pmu.disableALDO4();
pmu.disableBLDO1();
pmu.disableBLDO2();
pmu.disableCPUSLDO();
pmu.disableDLDO1();
pmu.disableDLDO2();
}

static void pmu_prepare_sleep(void)
{
if (!pmu_ready) {
return;
}
pmu.clearIrqStatus();
pmu.setChargingLedMode(XPOWERS_CHG_LED_OFF);
}

// ---------------------------------------------------------------------------
// Pin helpers
// ---------------------------------------------------------------------------

static inline void set_pin_input(gpio_num_t pin)
{
gpio_reset_pin(pin);
gpio_set_direction(pin, GPIO_MODE_INPUT);
gpio_set_pull_mode(pin, GPIO_FLOATING);
}

static inline void set_pin_input_pulldown(gpio_num_t pin)
{
gpio_reset_pin(pin);
gpio_set_direction(pin, GPIO_MODE_INPUT);
gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
}

// ---------------------------------------------------------------------------
// Sleep
// ---------------------------------------------------------------------------

static void setup_sleep_mode(void)
{
ESP_LOGI(TAG, "=== Starting Sleep Mode Setup ===");

pmu_setup_sleep();
modem_poweroff_with_retry();
pmu_prepare_sleep();

uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(100));
uart_driver_delete(UART_NUM_1);

if (pmu_ready) {
pmu.deinit();
pmu_ready = false;
}
if (i2c_bus_0 != NULL) {
esp_err_t err = i2c_del_master_bus(i2c_bus_0);
if (err != ESP_OK) {
ESP_LOGW(TAG, "i2c_del_master_bus(0) failed: %s", esp_err_to_name(err));
}
i2c_bus_0 = NULL;
}

set_pin_input(GPIO_NUM_4);
set_pin_input(GPIO_NUM_5);
set_pin_input(GPIO_NUM_7);
set_pin_input_pulldown(GPIO_NUM_15);
set_pin_input_pulldown(GPIO_NUM_38);
set_pin_input_pulldown(GPIO_NUM_39);
set_pin_input_pulldown(GPIO_NUM_40);
set_pin_input_pulldown(GPIO_NUM_41);
set_pin_input_pulldown(GPIO_NUM_45);
set_pin_input_pulldown(GPIO_NUM_46);
set_pin_input_pulldown(GPIO_NUM_47);
set_pin_input_pulldown(GPIO_NUM_48);

uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(100));
fflush(stdout);

ESP_ERROR_CHECK(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
ESP_LOGI(TAG, "Entering deep sleep (no wakeup sources)...");
esp_deep_sleep_start();
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

extern "C" void app_main(void)
{
ESP_LOGI(TAG, "Application started. Initializing drivers...");

esp_err_t ret = i2c_buses_init();
if (ret != ESP_OK) {
ESP_LOGE(TAG, "Failed to initialize I2C buses: %s", esp_err_to_name(ret));
}

ret = pmu_init();
if (ret != ESP_OK) {
ESP_LOGE(TAG, "Failed to initialize PMU: %s", esp_err_to_name(ret));
}

ret = modem_uart_init();
if (ret != ESP_OK) {
ESP_LOGE(TAG, "Failed to initialize modem UART: %s", esp_err_to_name(ret));
}

ESP_LOGI(TAG, "Drivers initialized. Preparing to enter deep sleep...");
vTaskDelay(pdMS_TO_TICKS(2000));
setup_sleep_mode();
}
