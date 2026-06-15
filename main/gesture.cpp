/* ════════════════════════════════════════════════════════════════════
 * gesture.cpp — classificador de gestos (Edge Impulse) rodando na mesma
 * Pico que o controlador. Lê o acelerômetro pela MPU6050 (via mpu_read_accel,
 * compartilhada com o controlador sob mutex de I2C), monta a janela de
 * amostras, roda o modelo e, ao reconhecer "prancha", dispara o MESMO
 * sinal do botão de prancha (controller_trigger_hoverboard).
 *
 * GESTURE_DEBUG=1 imprime as confianças de cada classe no serial monitor
 * (para bancada). Mantenha 0 ao jogar: o protocolo Controle->PC exige a
 * USB limpa, senão o parser do controller.py recebe lixo misturado.
 * ════════════════════════════════════════════════════════════════════ */
#include "integration.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"   /* stdio_flush() */

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"
#include "model-parameters/model_metadata.h"

using namespace ei;

/* Liga os prints de bancada (confiança por classe). DESLIGUE (0) para jogar. */
#define GESTURE_DEBUG 1

/* Confiança mínima para aceitar o gesto como "prancha". */
#define PRANCHA_THRESHOLD 0.75f

/* Período de amostragem casado com a taxa de treino do modelo
 * (EI_CLASSIFIER_INTERVAL_MS ≈ 12.987 ms ⇒ 13 ms). Amostrar na frequência
 * de treino + período fixo (vTaskDelayUntil) evita o jitter da preempção
 * pela mpu_task, que deformava a janela e derrubava a confiança. */
#define GESTURE_SAMPLE_MS 13

extern "C" void gesture_task(void *p)
{
    (void)p;

#if GESTURE_DEBUG
    /* Banner de partida: se ESTA linha não aparecer (mas HEARTBEAT sim), a
     * task não foi criada (heap) ou travou na entrada. Se aparecer e nada
     * mais vier, o problema está na coleta/inferência abaixo. */
    printf("[IA] gesture_task iniciada | frame=%d labels=%d sample=%dms\n",
           (int)EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE,
           (int)EI_CLASSIFIER_LABEL_COUNT, (int)GESTURE_SAMPLE_MS);
    stdio_flush();
#endif

    /* Estado de borda: a IA classifica continuamente, mas o botão gera UM
     * evento por aperto. Só disparamos ao ENTRAR no gesto (não-prancha ->
     * prancha), equivalendo a um único aperto. */
    bool prancha_active = false;
    int16_t accel[3];

    for (;;) {
#if GESTURE_DEBUG
        /* Marcador por janela (repete ~1x/s): se ISTO aparece mas as linhas
         * de confiança não, o problema é a inferência; se NEM isto aparece,
         * a task não está rodando. Repetir evita depender de pegar o boot. */
        printf("[IA] coletando janela...\n");
        stdio_flush();
#endif
        /* Monta a janela de entrada: 3 eixos do acelerômetro por amostra.
         * vTaskDelayUntil dá período fixo mesmo sob preempção da mpu_task.
         * last_wake é reiniciado aqui (e não fora do laço) para que o tempo
         * gasto na inferência não vire uma rajada de amostras de recuperação. */
        float buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE] = { 0 };
        TickType_t last_wake = xTaskGetTickCount();
        for (size_t ix = 0; ix < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; ix += 3) {
            mpu_read_accel(accel);
            buffer[ix + 0] = accel[0];
            buffer[ix + 1] = accel[1];
            buffer[ix + 2] = accel[2];
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(GESTURE_SAMPLE_MS));
        }

        signal_t signal;
        int serr = numpy::signal_from_buffer(buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
        if (serr != 0) {
#if GESTURE_DEBUG
            printf("[IA] signal_from_buffer falhou: %d\n", serr);
            stdio_flush();
#endif
            continue;
        }

        ei_impulse_result_t result = { 0 };
        EI_IMPULSE_ERROR cerr = run_classifier(&signal, &result, false);
        if (cerr != EI_IMPULSE_OK) {
#if GESTURE_DEBUG
            /* Erro mais comum aqui é -6 (EI_IMPULSE_OUT_OF_MEMORY): o DSP da
             * FFT não conseguiu alocar. Nesse caso o caminho é liberar RAM,
             * não mexer no gesto. */
            printf("[IA] run_classifier falhou: %d\n", (int)cerr);
            stdio_flush();
#endif
            continue;
        }

        bool prancha_now = false;
        for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
#if GESTURE_DEBUG
            /* Bancada: confiança de cada classe. Use o serial monitor, NÃO o
             * controller.py (estes prints quebrariam o parser do protocolo). */
            printf("[IA] %s: %.3f\n",
                   result.classification[ix].label,
                   result.classification[ix].value);
#endif
            if (result.classification[ix].value > PRANCHA_THRESHOLD &&
                strcmp(result.classification[ix].label, "prancha") == 0) {
                prancha_now = true;
            }
        }
#if GESTURE_DEBUG
        stdio_flush();
#endif

        if (prancha_now && !prancha_active) {
#if GESTURE_DEBUG
            printf("[IA] >>> PRANCHA detectada -> HOVERBOARD\n");
            stdio_flush();
#endif
            controller_trigger_hoverboard();
        }
        prancha_active = prancha_now;
    }
}
