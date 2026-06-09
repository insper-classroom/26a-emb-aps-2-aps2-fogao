#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"

#include <stdio.h>
#include <math.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "mpu6050.h"
#include "Fusion.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"





/* ───────────────────────── Pinos ─────────────────────────────────── */
/* Botões (entradas digitais por interrupção) */
const int BTN_PIN_PAUSE = 3; //red
const int BTN_PIN_HOVERBOARD = 2; //blue
const int BTN_PIN_IMU = 5; //white
const int BTN_PIN_START = 4; //green

// LED RGB catodo comum; PWM 8 bits, resistor serie 220-330R.
#define RGB_PIN_R   13
#define RGB_PIN_G   14
#define RGB_PIN_B   15

/* IMU MPU6050 via I2C (mesma pinagem da PRA-7) */
#define MPU_ADDRESS   0x68
#define I2C_PORT      i2c0
#define I2C_SDA_GPIO  16
#define I2C_SCL_GPIO  17
#define I2C_BAUDRATE  (400 * 1000)

/* ─────────────────────── Parâmetros da IMU ──────────────────────── */
/* Escala dos registradores no reset: ±2 g e ±250 °/s */
#define ACCEL_SCALE   16384.0f
#define GYRO_SCALE    131.0f

/* Fusion AHRS */
#define MPU_SAMPLE_HZ     100
#define MPU_PERIOD_MS     (1000 / MPU_SAMPLE_HZ)
#define MPU_SAMPLE_PERIOD (1.0f / MPU_SAMPLE_HZ)   /* segundos por amostra */
#define GYRO_RANGE_DPS    250.0f

/*
 * Detecção de inclinação com histerese (edge-triggered):
 *   - ENTER: ao ultrapassar TILT_ENTER_DEG num eixo, dispara UM comando.
 *   - EXIT : só rearma quando o ângulo volta para dentro de TILT_EXIT_DEG.
 * Isso evita comandos repetidos/ruído enquanto o jogador segura a inclinação.
 *
 * Mapeamento (README): roll = eixo esquerda/direita, pitch = eixo cima/baixo.
 * Use os INVERT_* se o controle estiver montado na orientação oposta.
 */
#define TILT_ENTER_DEG  20.0f
#define TILT_EXIT_DEG   10.0f
#define INVERT_ROLL     0
#define INVERT_PITCH    0

/* ─────────────────── Comandos de jogo (fila compartilhada) ──────── */
/*
 * Tipo compartilhado entre mpu_task e (futuramente) btn_task: ambas
 * publicam comandos de alto nível na xQueueGameCmd, que serão
 * consumidos pela game_command_task (ainda não implementada).
 */
typedef enum {
    GAME_CMD_MOVE_LEFT = 0,
    GAME_CMD_MOVE_RIGHT,
    GAME_CMD_JUMP,
    GAME_CMD_ROLL,
    GAME_CMD_START,
    GAME_CMD_PAUSE,
    GAME_CMD_RESET_IMU,
    GAME_CMD_HOVERBOARD,
} game_cmd_t;

/* ───────────────────── Filas e semáforos ────────────────────────── */
QueueHandle_t xQueueBtn;
QueueHandle_t xQueueGameCmd;
SemaphoreHandle_t xSemaphoreCalibrateImu;

/* ════════════════════════ Botões (ISR + task) ═══════════════════════ */

void btn_callback(uint gpio, uint32_t events) {
    if (events == GPIO_IRQ_EDGE_FALL) {
        /* ISR curta: apenas publica o GPIO acionado na fila.
         * (xQueueSendFromISR exige ponteiro para o item, por isso a var.) */
        uint32_t pin = gpio;
        BaseType_t higher_priority_woken = pdFALSE;
        xQueueSendFromISR(xQueueBtn, &pin, &higher_priority_woken);
        portYIELD_FROM_ISR(higher_priority_woken);
    }           
}

void btn_task(void *p) {
    (void)p;
    uint32_t btn = 0;
    game_cmd_t cmd;
    //Led
    int delay = 200;
    while (true) {
        if (xQueueReceive(xQueueBtn, &btn, portMAX_DELAY)) {
            if (btn == (uint32_t)BTN_PIN_PAUSE) {
                // Pausa o jogo
                // Indica com o led em amarelo
                cmd = GAME_CMD_PAUSE;
                xQueueSend(xQueueGameCmd, &cmd, 0);
                // Indica com o led (em amarelo)
                gpio_put(RGB_PIN_R, 1);
                vTaskDelay(pdMS_TO_TICKS(delay));
                gpio_put(RGB_PIN_R, 0);

            } else if (btn == (uint32_t)BTN_PIN_HOVERBOARD) {
                // Ativa o Hoverboard
                // Indica com o led verde que o hoverboard está ativado
                cmd = GAME_CMD_HOVERBOARD;
                xQueueSend(xQueueGameCmd, &cmd, 0);

                // Indica com o led (em amarelo)
                gpio_put(RGB_PIN_B, 1);
                vTaskDelay(pdMS_TO_TICKS(delay));
                gpio_put(RGB_PIN_B, 0);

            } else if (btn ==  (uint32_t)BTN_PIN_START){
                cmd = GAME_CMD_START;
                xQueueSend(xQueueGameCmd, &cmd, 0);

                // Indica com o led (em amarelo)
                gpio_put(RGB_PIN_G, 1);
                vTaskDelay(pdMS_TO_TICKS(delay));
                gpio_put(RGB_PIN_G, 0);
            }
            else {// Botão de reset da IMU
                // Pede recalibração para a mpu_task
                xSemaphoreGive(xSemaphoreCalibrateImu);

                // Indica com o led (em amarelo)
                gpio_put(RGB_PIN_G, 1);
                gpio_put(RGB_PIN_R, 1);
                gpio_put(RGB_PIN_B, 1);
                vTaskDelay(pdMS_TO_TICKS(delay));
                gpio_put(RGB_PIN_G, 0);
                gpio_put(RGB_PIN_R, 0);
                gpio_put(RGB_PIN_B, 0);
            }
        }
    }
}

/* ═══════════════════════════ MPU6050 (I2C) ══════════════════════════ */

static void mpu6050_init(void) {
    i2c_init(I2C_PORT, I2C_BAUDRATE);
    gpio_set_function(I2C_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_GPIO, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_GPIO);
    gpio_pull_up(I2C_SCL_GPIO);

    /* Acorda a IMU: limpa o bit de sleep em PWR_MGMT_1 (0x6B) */
    uint8_t buf[] = {MPUREG_PWR_MGMT_1, 0x00};
    i2c_write_blocking(I2C_PORT, MPU_ADDRESS, buf, 2, false);
}

/* Burst-read de 14 bytes a partir de ACCEL_XOUT_H (0x3B). Ignora a
 * temperatura (bytes 6-7), guardando apenas accel e gyro. */
static void mpu6050_read_raw(int16_t accel[3], int16_t gyro[3]) {
    uint8_t buf[14];
    uint8_t reg = MPUREG_ACCEL_XOUT_H;
    i2c_write_blocking(I2C_PORT, MPU_ADDRESS, &reg, 1, true);
    i2c_read_blocking(I2C_PORT, MPU_ADDRESS, buf, 14, false);

    for (int i = 0; i < 3; i++)
        accel[i] = (int16_t)((buf[i * 2] << 8) | buf[i * 2 + 1]);
    for (int i = 0; i < 3; i++)
        gyro[i] = (int16_t)((buf[8 + i * 2] << 8) | buf[8 + i * 2 + 1]);
}

/* ════════════════════════════════════════════════════════════════════
 * mpu_task — lê a IMU a 100 Hz, roda Fusion AHRS para obter roll/pitch
 * e converte a inclinação em comandos de jogo (MOVE_LEFT/RIGHT, JUMP,
 * ROLL) publicados na xQueueGameCmd.
 *
 * Calibração: o centro/neutro é capturado no boot e a cada pedido vindo
 * de xSemaphoreCalibrateImu (botão Reset IMU). Os ângulos usados na
 * detecção são sempre relativos a esse centro.
 * ════════════════════════════════════════════════════════════════════ */
void mpu_task(void *p) {
    (void)p;
    mpu6050_init();

    FusionAhrs ahrs;
    FusionAhrsInitialise(&ahrs);
    const FusionAhrsSettings settings = {
        .convention            = FusionConventionNwu,
        .gain                  = 0.5f,
        .gyroscopeRange        = GYRO_RANGE_DPS,
        .accelerationRejection = 10.0f,
        .magneticRejection     = 0.0f,
        .recoveryTriggerPeriod = 5 * MPU_SAMPLE_HZ, /* 5 s */
    };
    FusionAhrsSetSettings(&ahrs, &settings);

    /* Centro/neutro da calibração e flag de "capturar centro". */
    float roll_center = 0.0f, pitch_center = 0.0f;
    bool capture_center = true; /* calibra na primeira amostra estável */

    /* Estado de histerese por eixo: -1 (esq/baixo), 0 (centro), +1 (dir/cima) */
    int roll_state = 0;
    int pitch_state = 0;

    TickType_t last_wake = xTaskGetTickCount();
    int dbg_count = 0; /* contador para throttle do print de debug */

    for (;;) {
        /* Pedido de recalibração vindo do botão Reset IMU (não bloqueia). */
        if (xSemaphoreTake(xSemaphoreCalibrateImu, 0) == pdTRUE) {
            capture_center = true;
        }

        int16_t raw_a[3], raw_g[3];
        mpu6050_read_raw(raw_a, raw_g);

        FusionVector accel = {{
            raw_a[0] / ACCEL_SCALE,
            raw_a[1] / ACCEL_SCALE,
            raw_a[2] / ACCEL_SCALE,
        }};
        FusionVector gyro = {{
            raw_g[0] / GYRO_SCALE,
            raw_g[1] / GYRO_SCALE,
            raw_g[2] / GYRO_SCALE,
        }};

        FusionAhrsUpdateNoMagnetometer(&ahrs, gyro, accel, MPU_SAMPLE_PERIOD);
        FusionEuler euler =
            FusionQuaternionToEuler(FusionAhrsGetQuaternion(&ahrs));

        float roll = euler.angle.roll;
        float pitch = euler.angle.pitch;
#if INVERT_ROLL
        roll = -roll;
#endif
#if INVERT_PITCH
        pitch = -pitch;
#endif

        if (capture_center) {
            roll_center = roll;
            pitch_center = pitch;
            capture_center = false;
            roll_state = 0;
            pitch_state = 0;
        }

        float roll_rel = roll - roll_center;
        float pitch_rel = pitch - pitch_center;

        /* Debug: imprime ~2 Hz (1 a cada MPU_SAMPLE_HZ/2 amostras) para
         * acompanhar os ângulos e ajustar TILT_ENTER_DEG sem floodar. */
        if (++dbg_count >= MPU_SAMPLE_HZ / 2) {
            dbg_count = 0;
            printf("[MPU] roll=%.1f pitch=%.1f\n", roll_rel, pitch_rel);
        }

        /* ── Eixo roll → MOVE_RIGHT (>0) / MOVE_LEFT (<0) ──────────── */
        if (roll_state == 0) {
            if (roll_rel > TILT_ENTER_DEG) {
                roll_state = 1;
                game_cmd_t cmd = GAME_CMD_MOVE_RIGHT;
                xQueueSend(xQueueGameCmd, &cmd, 0);

                //Debug
                printf("Virou para a direita\n");
                //

            } else if (roll_rel < -TILT_ENTER_DEG) {
                roll_state = -1;
                game_cmd_t cmd = GAME_CMD_MOVE_LEFT;
                xQueueSend(xQueueGameCmd, &cmd, 0);

                //Debug
                printf("Virou para a esquerda\n");
                //
            }
        } else if (fabsf(roll_rel) < TILT_EXIT_DEG) {
            roll_state = 0; /* voltou ao centro: rearma */
        }

        /* ── Eixo pitch → JUMP (cima, >0) / ROLL (baixo, <0) ──────── */
        if (pitch_state == 0) {
            if (pitch_rel > TILT_ENTER_DEG) {
                pitch_state = 1;
                game_cmd_t cmd = GAME_CMD_JUMP;
                xQueueSend(xQueueGameCmd, &cmd, 0);

                //Debug
                printf("Pulou!\n");
                //
            } else if (pitch_rel < -TILT_ENTER_DEG) {
                pitch_state = -1;
                game_cmd_t cmd = GAME_CMD_ROLL;
                xQueueSend(xQueueGameCmd, &cmd, 0);

                //Debug
                printf("Rolou!\n");
                //
            }
        } else if (fabsf(pitch_rel) < TILT_EXIT_DEG) {
            pitch_state = 0; /* voltou ao centro: rearma */
        }

        /* Período fixo de amostragem (não acumula jitter). */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MPU_PERIOD_MS));
    }
}

int main(void) {
    /* Inicializações de stdio, botões e LED */
    stdio_init_all();

    gpio_init(RGB_PIN_R);
    gpio_set_dir(RGB_PIN_R, GPIO_OUT);
    gpio_put(RGB_PIN_R, 0);

    gpio_init(RGB_PIN_B);
    gpio_set_dir(RGB_PIN_B, GPIO_OUT);
    gpio_put(RGB_PIN_B, 0);

    gpio_init(RGB_PIN_G);
    gpio_set_dir(RGB_PIN_G, GPIO_OUT);
    gpio_put(RGB_PIN_G, 0);

    gpio_init(BTN_PIN_PAUSE);
    gpio_set_dir(BTN_PIN_PAUSE, GPIO_IN);
    gpio_pull_up(BTN_PIN_PAUSE);

    gpio_init(BTN_PIN_HOVERBOARD);
    gpio_set_dir(BTN_PIN_HOVERBOARD, GPIO_IN);
    gpio_pull_up(BTN_PIN_HOVERBOARD);

    gpio_init(BTN_PIN_IMU);
    gpio_set_dir(BTN_PIN_IMU, GPIO_IN);
    gpio_pull_up(BTN_PIN_IMU);

    gpio_init(BTN_PIN_START);
    gpio_set_dir(BTN_PIN_START, GPIO_IN);
    gpio_pull_up(BTN_PIN_START);

    /* Registro do callback de interrupção dos botões */
    gpio_set_irq_enabled_with_callback(BTN_PIN_PAUSE, GPIO_IRQ_EDGE_FALL, true, &btn_callback);
    gpio_set_irq_enabled(BTN_PIN_HOVERBOARD, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BTN_PIN_IMU, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BTN_PIN_START, GPIO_IRQ_EDGE_FALL, true);

    /* Filas e semáforos */
    xQueueBtn = xQueueCreate(32, sizeof(uint32_t));
    xQueueGameCmd = xQueueCreate(32, sizeof(game_cmd_t));
    xSemaphoreCalibrateImu = xSemaphoreCreateBinary();

    /* Tasks */
    xTaskCreate(mpu_task, "mpu", 4096, NULL, 2, NULL);
    xTaskCreate(btn_task, "btn task", 4096, NULL, 2, NULL);

    vTaskStartScheduler();

    // Should never reach here
    for (;;);
}
