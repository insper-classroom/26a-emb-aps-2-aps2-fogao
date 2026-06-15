#ifndef INTEGRATION_H
#define INTEGRATION_H

/* Fronteira entre o controlador (main.c, C) e o classificador da IA
 * (gesture.cpp, C++/Edge Impulse). Mantém as filas/semáforos do controlador
 * privados ao main.c: a IA só enxerga estas três funções. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Leitura do acelerômetro da MPU6050 (3 eixos, contagens cruas), protegida
 * pelo mutex de I2C. Dona do barramento é a main.c; consumida pela gesture_task. */
void mpu_read_accel(int16_t accel[3]);

/* Dispara a MESMA ação do botão de prancha: comando HOVERBOARD na fila de
 * jogo + som power_up + vibração curta. Chamada pela IA ao reconhecer o gesto. */
void controller_trigger_hoverboard(void);

/* Task do classificador Edge Impulse (definida em gesture.cpp). */
void gesture_task(void *p);

#ifdef __cplusplus
}
#endif

#endif /* INTEGRATION_H */
