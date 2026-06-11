/* ════════════════════════════════════════════════════════════════════
 * gesture.cpp — classificador de gestos (Edge Impulse) rodando na mesma
 * Pico que o controlador. Lê o acelerômetro pela MPU6050 (via mpu_read_accel,
 * compartilhada com o controlador sob mutex de I2C), monta a janela de
 * amostras, roda o modelo e, ao reconhecer "prancha", dispara o MESMO
 * sinal do botão de prancha (controller_trigger_hoverboard).
 *
 * Sem prints na serial: o protocolo Controle->PC exige a USB limpa.
 * ════════════════════════════════════════════════════════════════════ */
#include "integration.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"
#include "model-parameters/model_metadata.h"

using namespace ei;

/* Confiança mínima para aceitar o gesto como "prancha". */
#define PRANCHA_THRESHOLD 0.75f

extern "C" void gesture_task(void *p)
{
    (void)p;

    /* Estado de borda: a IA classifica continuamente, mas o botão gera UM
     * evento por aperto. Só disparamos ao ENTRAR no gesto (não-prancha ->
     * prancha), equivalendo a um único aperto. */
    bool prancha_active = false;
    int16_t accel[3];

    for (;;) {
        /* Monta a janela de entrada: 3 eixos do acelerômetro por amostra. */
        float buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE] = { 0 };
        for (size_t ix = 0; ix < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; ix += 3) {
            mpu_read_accel(accel);
            buffer[ix + 0] = accel[0];
            buffer[ix + 1] = accel[1];
            buffer[ix + 2] = accel[2];
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        signal_t signal;
        if (numpy::signal_from_buffer(buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal) != 0) {
            continue;
        }

        ei_impulse_result_t result = { 0 };
        if (run_classifier(&signal, &result, false) != EI_IMPULSE_OK) {
            continue;
        }

        bool prancha_now = false;
        for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
            if (result.classification[ix].value > PRANCHA_THRESHOLD &&
                strcmp(result.classification[ix].label, "prancha") == 0) {
                prancha_now = true;
            }
        }

        if (prancha_now && !prancha_active) {
            controller_trigger_hoverboard();
        }
        prancha_active = prancha_now;
    }
}
