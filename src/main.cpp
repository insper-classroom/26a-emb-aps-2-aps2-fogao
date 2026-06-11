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
#include <string.h>
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

/* Protocolo Controle -> PC (mesmo do main/main.c): cada comando é um token
 * de texto terminado por EOP. Apertar o botão de prancha no controlador
 * envia "HOVERBOARD"; aqui a IA reproduz exatamente esse mesmo sinal. */
#define EOP '\n'
#define TOKEN_HOVERBOARD "HOVERBOARD"

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

    /* Estado de borda: a IA classifica continuamente, mas o botão gera UM
     * evento por aperto. Só disparamos o sinal ao ENTRAR no gesto "prancha"
     * (transição não-prancha -> prancha), evitando repetir a cada janela. */
    bool prancha_active = false;

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
        bool prancha_now = false;
        for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
            ei_printf(
                "teste    %s: %.5f\n",
                result.classification[ix].label,
                result.classification[ix].value);
            /* strcmp: comparar strings por conteúdo. Usar '==' aqui
             * compararia ponteiros e falharia silenciosamente. */
            if (result.classification[ix].value > 0.75 &&
                strcmp(result.classification[ix].label, "prancha") == 0) {
                prancha_now = true;
            }
        }

        /* Mesmo sinal do botão de prancha, disparado pela IA. Só na borda de
         * subida do gesto, para equivaler a um único aperto de botão. */
        if (prancha_now && !prancha_active) {
            printf("%s%c", TOKEN_HOVERBOARD, EOP);
            stdio_flush();
        }
        prancha_active = prancha_now;

#if EI_CLASSIFIER_HAS_ANOMALY == 1
        ei_printf("    anomaly score: %.3f\n", result.anomaly);
#endif
    }
}


int main(void)
{
    stdio_init_all();

    xTaskCreate(gesture_recognize_task, "gesture_task 1", 8192, NULL, 1, NULL);
    vTaskStartScheduler();

    while (true)
        ;
}
