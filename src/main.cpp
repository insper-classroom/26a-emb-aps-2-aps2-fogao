#include "FreeRTOS.h"
#include "ei_accelerometer.h"
#include "ei_analogsensor.h"
#include "ei_at_handlers.h"
#include "ei_classifier_porting.h"
#include "ei_device_raspberry_rp2xxx.h"
#include "ei_dht11sensor.h"
#include "ei_inertialsensor.h"
#include "ei_rp2xxx_internal_temperature.h"
#include "ei_run_impulse.h"
#include "ei_ultrasonicsensor.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "task.h"
#include <stdio.h>
#include <time.h>

// imu
#include <hardware/gpio.h>
#include <hardware/i2c.h>
#include <hardware/uart.h>
#include <pico/stdio.h>

// freertos
#include <FreeRTOS.h>
#include <queue.h>
#include <semphr.h>
#include <stdlib.h>
#include <task.h>

// // específico
#include "mpu6050.h"
// edited
// -- Adicione estas 3 linhas em main.cpp --
#include "edge-impulse-sdk/classifier/ei_model_types.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"
#include "model-parameters/model_metadata.h"

using namespace ei;

extern "C" EI_IMPULSE_ERROR
run_classifier(ei::signal_t *signal, ei_impulse_result_t *result, bool debug);

static bool debug_nn = false;

const int MPU_ADDRESS = 0x68;
const int I2C_SDA_GPIO = 17;
const int I2C_SCL_GPIO = 16;
const int LED_R_rgb = 27;
const int LED_G_rgb = 26;
const int LED_B_rgb = 21;
const int LED_B = 18;
const int LED_G = 19;
const int LED_R = 20;
const int BTN_PIN_G = 6;
const int BTN_PIN_R = 7;
const int BTN_PIN_Y = 8;



QueueHandle_t xQueueClass, xQueueBtn;

static void mpu6050_init()
{
    i2c_init(i2c_default, 400 * 1000);
    gpio_set_function(I2C_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_GPIO, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_GPIO);
    gpio_pull_up(I2C_SCL_GPIO);

    // Two byte reset. First byte register, second byte data
    // There are a load more options to set up the device in different ways that could be added here
    uint8_t buf[] = { 0x6B, 0x00 };
    i2c_write_blocking(i2c_default, MPU_ADDRESS, buf, 2, false);
}

static void mpu6050_read_raw(int16_t accel[3], int16_t gyro[3], int16_t *temp)
{
    uint8_t buffer[14];

    // Read all data sequentially starting from acceleration registers (0x3B)
    // 0x3B-0x40: acceleration (6 bytes)
    // 0x41-0x42: temperature (2 bytes)
    // 0x43-0x48: gyro (6 bytes)
    uint8_t val = 0x3B;
    i2c_write_blocking(i2c_default, MPU_ADDRESS, &val, 1, true);
    i2c_read_blocking(i2c_default, MPU_ADDRESS, buffer, 14, false);

    // Parse acceleration
    for (int i = 0; i < 3; i++) {
        accel[i] = (buffer[i * 2] << 8 | buffer[(i * 2) + 1]);
    }

    // Parse temperature
    *temp = buffer[6] << 8 | buffer[7];

    // Parse gyro
    for (int i = 0; i < 3; i++) {
        gyro[i] = (buffer[8 + i * 2] << 8 | buffer[8 + (i * 2) + 1]);
    }
}

static void gesture_recognize_task(void *p)
{
    mpu6050_init();
    int16_t accelerometer[3], gyro[3], temp;

    while (true) {
        //        ei_printf("\nStarting inferencing in 2 seconds...\n");
        //        vTaskDelay(pdMS_TO_TICKS(2000));
        //        ei_printf("Sampling...\n");

        float buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE] = { 0 };

        for (size_t ix = 0; ix < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; ix += 3) {
            mpu6050_read_raw(accelerometer, gyro, &temp);
            buffer[ix + 0] = accelerometer[0];
            buffer[ix + 1] = accelerometer[1];
            buffer[ix + 2] = accelerometer[2];

            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Prepara sinal
        ei::signal_t signal;
        int err = numpy::signal_from_buffer(buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
        if (err != 0) {
            ei_printf("Failed to create signal from buffer (%d)\n", err);
            break;
        }

        // Run the classifier
        ei_impulse_result_t result = { 0 };
        err = run_classifier(&signal, &result, debug_nn);
        if (err != EI_IMPULSE_OK) {
            ei_printf("ERR: Failed to run classifier (%d)\n", err);
            break;
        }

        // print the predictions
        ei_printf("Predictions ");
        ei_printf(
            "(DSP: %d ms., Classification: %d ms., Anomaly: %d ms.)",
            result.timing.dsp,
            result.timing.classification,
            result.timing.anomaly);
        ei_printf(": \n");
        for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
            ei_printf(
                "teste    %s: %.5f\n",
                result.classification[ix].label,
                result.classification[ix].value);
                int value;
                if (result.classification[ix].value > 0.5){
                    if (result.classification[ix].label == "idle"){
                        value = 1;
                    }
                    else if (result.classification[ix].label == "wave"){
                        value = 2;
                    }
                    else {
                        value = 3;
                    }
                    xQueueSend(xQueueClass, &value, 0);
                }
        }

#if EI_CLASSIFIER_HAS_ANOMALY == 1
        ei_printf("    anomaly score: %.3f\n", result.anomaly);
#endif
    }
}

void btn_callback(uint gpio, uint32_t events) {
    int i;
    if (gpio==BTN_PIN_R) {
        i = BTN_PIN_R;
    } else if (gpio==BTN_PIN_Y) {
        i = BTN_PIN_Y;
    }
    else if (gpio==BTN_PIN_G) {
        i = BTN_PIN_G;
    }
    xQueueSendFromISR(xQueueBtn, &i, 0);
}

void btn_task(void* p) {
    gpio_init(BTN_PIN_R);
    gpio_set_dir(BTN_PIN_R, GPIO_IN);
    gpio_pull_up(BTN_PIN_R);
    gpio_set_irq_enabled_with_callback(BTN_PIN_R, GPIO_IRQ_EDGE_FALL, true,
                                       &btn_callback);
    gpio_init(BTN_PIN_Y);
    gpio_set_dir(BTN_PIN_Y, GPIO_IN);
    gpio_pull_up(BTN_PIN_Y);
    gpio_set_irq_enabled(BTN_PIN_Y,GPIO_IRQ_EDGE_FALL, true);

    gpio_init(BTN_PIN_G);
    gpio_set_dir(BTN_PIN_G, GPIO_IN);
    gpio_pull_up(BTN_PIN_G);
    gpio_set_irq_enabled(BTN_PIN_G,GPIO_IRQ_EDGE_FALL, true);
    int gpio;
    while (true) {
        if (xQueueReceive(xQueueBtn, &gpio, 0)) {
            if (gpio == BTN_PIN_R) {
                printf("botao vermelho");
            } else if (gpio == BTN_PIN_Y) {
                printf("botao amarelo");
            } else if (gpio == BTN_PIN_G){
                printf("botao verde");
            }
        }
    }
}

void led_task(void* p) {
    gpio_init(LED_R_rgb);
    gpio_set_dir(LED_R_rgb, GPIO_OUT);

    gpio_init(LED_B_rgb);
    gpio_set_dir(LED_B_rgb, GPIO_OUT);

    gpio_init(LED_G_rgb);
    gpio_set_dir(LED_G_rgb, GPIO_OUT);

    gpio_init(LED_R);
    gpio_set_dir(LED_R, GPIO_OUT);

    gpio_init(LED_B);
    gpio_set_dir(LED_B, GPIO_OUT);

    gpio_init(LED_G);
    gpio_set_dir(LED_G, GPIO_OUT);
   
    int value;
    while (true) {
        gpio_put(LED_B,1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_put(LED_B,0);
        gpio_put(LED_G,1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_put(LED_G,0);
        gpio_put(LED_R,1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_put(LED_R,0);
        gpio_put(LED_B_rgb,1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_put(LED_B_rgb,0);
        gpio_put(LED_R_rgb,1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_put(LED_R_rgb,0);
        gpio_put(LED_G_rgb,1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_put(LED_G_rgb,0);
    }
}


int main(void)
{
    stdio_init_all();

    xQueueClass = xQueueCreate(32, sizeof(int));
    xQueueBtn = xQueueCreate(32, sizeof(int));
    xTaskCreate(gesture_recognize_task, "gesture_task 1", 8192, NULL, 1, NULL);
    xTaskCreate(led_task, "led_task", 256, NULL, 2, NULL);
    xTaskCreate(btn_task, "btn_task", 256, NULL, 2, NULL);
    vTaskStartScheduler();

    while (true)
        ;
}
