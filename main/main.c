#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "mpu6050.h"
#include "Fusion.h"
#include "hardware/gpio.h"

#include "power_up.h"
#include "pause.h"

/* DEBUG_PRINT=1 liga os prints de bancada no console USB.
 * Mantenha 0 ao jogar: a serial USB precisa transportar APENAS o
 * protocolo, senão o parser do PC recebe lixo misturado. */
#define DEBUG_PRINT 0
#if DEBUG_PRINT
#define DBG(...) printf(__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif





/* ───────────────────────── Pinos ─────────────────────────────────── */
/* Botões (entradas digitais por interrupção) */
const int BTN_PIN_PAUSE = 3; //red
const int BTN_PIN_HOVERBOARD = 2; //blue
const int BTN_PIN_IMU = 5; //white
const int BTN_PIN_START = 4; //green

const int PIN_VIBRA = 18; //Vibra

const int AUDIO_PIN = 28; // audio

/* Janela de debounce dos botões: toques (bounce) dentro deste intervalo
 * no mesmo pino são descartados, garantindo 1 evento por aperto. */
#define DEBOUNCE_MS   200

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
#define INVERT_ROLL     1
#define INVERT_PITCH    0

/* Calibração do centro/neutro por média: descarta CAL_SETTLE_SAMPLES
 * amostras iniciais (deixa o AHRS assentar) e tira a média das
 * CAL_AVG_SAMPLES seguintes. A 100 Hz dá ~0,7 s, com o controle parado. */
#define CAL_SETTLE_SAMPLES   20
#define CAL_AVG_SAMPLES      50

/* ─────────────── Status / conexão (caminho RX, PC → Controle) ────────
 * O controle envia HEARTBEAT periódico; o PC responde CONNECTED. Se
 * nenhum CONNECTED chegar dentro de CONN_TIMEOUT_MS, o status_task
 * considera o controle desconectado. */
#define HEARTBEAT_PERIOD_MS  1000
#define CONN_TIMEOUT_MS      2500
#define STATUS_PERIOD_MS     50    /* período de atualização do LED */
#define STATUS_BLINK_TICKS   2     /* meio-período do pisca (×STATUS_PERIOD_MS) */
#define DEATH_RED_MS         1500  /* tempo de LED vermelho ao morrer */

/* ─────────────────────── Feedback háptico (motor GP18) ──────────────── */
#define VIBRA_SHORT_MS       80    /* pulso de eventos locais */
#define VIBRA_LONG_MS        400   /* pulso da morte do jogador */

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

/* ───────────────────────── Protocolo (Controle → PC) ────────────────
 * Cada mensagem é um token de texto terminado por EOP. O script Python
 * lê a serial, separa por EOP e simula a tecla correspondente no jogo.
 * Mantenha estes tokens em sincronia com pc/controller.py e o README. */
#define EOP '\n'

/* Mensagem que trafega de game_command_task → uart_tx_task. Carrega o
 * token já pronto; "HOVERBOARD" (10) é o maior, então 16 sobra. */
typedef struct {
    char text[16];
} uart_msg_t;

/* Eventos enviados ao status_task (dono único do LED). A desconexão é
 * inferida por timeout, mas também pode chegar explícita do PC. */
typedef enum {
    STATUS_CONNECTED = 0,
    STATUS_DISCONNECTED,
    STATUS_CAL_BEGIN,
    STATUS_CAL_END,
    STATUS_PLAYER_DIED,
} status_evt_t;

/* Padrões de vibração tocados pelo feedback_task (dono do motor). */
typedef enum {
    FEEDBACK_SHORT = 0,   /* eventos locais: hoverboard, start, calibrado, conexão */
    FEEDBACK_LONG,        /* morte do jogador */
} feedback_evt_t;

/* ───────────────────── Filas e semáforos ────────────────────────── */
QueueHandle_t xQueueBtn;
QueueHandle_t xQueueGameCmd;
QueueHandle_t xQueueUartTx;
QueueHandle_t xQueueStatus;
QueueHandle_t xQueueFeedback;
SemaphoreHandle_t xSemaphoreCalibrateImu;

/* Helper de LED RGB (cátodo comum: nível alto acende). */
static void set_rgb(bool r, bool g, bool b) {
    gpio_put(RGB_PIN_R, r);
    gpio_put(RGB_PIN_G, g);
    gpio_put(RGB_PIN_B, b);
}

/* ════════════════════════ Botões (ISR + task) ═══════════════════════ */

/* Último instante (ms desde o boot) aceito por pino, para o debounce.
 * 30 = nº de GPIOs do banco 0 no RP2040/RP2350; indexado pelo gpio. */
static volatile uint32_t last_press_ms[30];

volatile int wav_position_pause = 0;
volatile bool pause = false;
volatile int wav_position_power_up = 0;
volatile bool prancha = false;

void pwm_interrupt_handler() {
    pwm_clear_irq(pwm_gpio_to_slice_num(AUDIO_PIN));    
    if (pause){
        if (wav_position_pause < (WAV_DATA_LENGTH_PAUSE<<3) - 1) { 
            // set pwm level 
            // allow the pwm value to repeat for 8 cycles this is >>3 
            pwm_set_gpio_level(AUDIO_PIN, WAV_DATA_PAUSE[wav_position_pause>>3]);  
            wav_position_pause++;
        }
    } else if (prancha){
        if (wav_position_power_up < (WAV_DATA_LENGTH_POWER_UP<<3) - 1) { 
            // set pwm level 
            // allow the pwm value to repeat for 8 cycles this is >>3 
            pwm_set_gpio_level(AUDIO_PIN, WAV_DATA_POWER_UP[wav_position_power_up>>3]);  
            wav_position_power_up++;
        }
    }
}

void btn_callback(uint gpio, uint32_t events) {
    if (events == GPIO_IRQ_EDGE_FALL) {
        /* Debounce na ISR: descarta quiques dentro de DEBOUNCE_MS antes
         * de ocupar a fila, garantindo 1 evento por aperto. */
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_press_ms[gpio] < DEBOUNCE_MS) {
            return;
        }
        last_press_ms[gpio] = now;

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
    while (true) {
        if (xQueueReceive(xQueueBtn, &btn, portMAX_DELAY)) {
            /* O LED é responsabilidade exclusiva do status_task; aqui só
             * publicamos comandos de jogo ou o pedido de recalibração. */
            if (btn == (uint32_t)BTN_PIN_PAUSE) {
                cmd = GAME_CMD_PAUSE;
                xQueueSend(xQueueGameCmd, &cmd, 0);
            } else if (btn == (uint32_t)BTN_PIN_HOVERBOARD) {
                cmd = GAME_CMD_HOVERBOARD;
                xQueueSend(xQueueGameCmd, &cmd, 0);
                feedback_evt_t fb = FEEDBACK_SHORT;
                xQueueSend(xQueueFeedback, &fb, 0);
            } else if (btn == (uint32_t)BTN_PIN_START) {
                cmd = GAME_CMD_START;
                xQueueSend(xQueueGameCmd, &cmd, 0);
                feedback_evt_t fb = FEEDBACK_SHORT;
                xQueueSend(xQueueFeedback, &fb, 0);
            } else if (btn == (uint32_t)BTN_PIN_IMU) {
                /* Botão Reset IMU: pede recalibração à mpu_task. O feedback
                 * visual (pisca branco → verde) é dado pela própria mpu_task,
                 * que conhece o início e o fim da calibração. */
                xSemaphoreGive(xSemaphoreCalibrateImu);
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

    /* Centro/neutro (média) e estado da calibração. */
    float roll_center = 0.0f, pitch_center = 0.0f;
    bool calibrating = true;   /* calibra automaticamente no boot */
    int cal_iter = 0;          /* amostras decorridas na calibração atual */
    int cal_used = 0;          /* amostras efetivamente somadas na média */
    float roll_sum = 0.0f, pitch_sum = 0.0f;

    /* Estado de histerese por eixo: -1 (esq/baixo), 0 (centro), +1 (dir/cima) */
    int roll_state = 0;
    int pitch_state = 0;

    /* Calibração de boot já está ativa (calibrating = true): avisa o LED. */
    status_evt_t cal_evt = STATUS_CAL_BEGIN;
    xQueueSend(xQueueStatus, &cal_evt, 0);

    TickType_t last_wake = xTaskGetTickCount();
    int dbg_count = 0; /* contador para throttle do print de debug */

    for (;;) {
        /* Pedido de recalibração vindo do botão Reset IMU (não bloqueia):
         * reinicia o acumulador para uma nova média. */
        if (xSemaphoreTake(xSemaphoreCalibrateImu, 0) == pdTRUE) {
            calibrating = true;
            cal_iter = 0;
            cal_used = 0;
            roll_sum = 0.0f;
            pitch_sum = 0.0f;
            cal_evt = STATUS_CAL_BEGIN;
            xQueueSend(xQueueStatus, &cal_evt, 0);
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

        /* ── Calibração por média (LED tratado pelo status_task) ─────── */
        if (calibrating) {
            /* Só acumula após o tempo de assentamento do AHRS. */
            if (cal_iter >= CAL_SETTLE_SAMPLES) {
                roll_sum += roll;
                pitch_sum += pitch;
                cal_used++;
            }
            cal_iter++;

            if (cal_iter >= CAL_SETTLE_SAMPLES + CAL_AVG_SAMPLES) {
                roll_center = roll_sum / (float)cal_used;
                pitch_center = pitch_sum / (float)cal_used;
                calibrating = false;
                roll_state = 0;
                pitch_state = 0;
                cal_evt = STATUS_CAL_END;
                xQueueSend(xQueueStatus, &cal_evt, 0);
                feedback_evt_t fb = FEEDBACK_SHORT;
                xQueueSend(xQueueFeedback, &fb, 0);
                DBG("[MPU] calibrado: roll_c=%.1f pitch_c=%.1f\n",
                    roll_center, pitch_center);
            }

            /* Enquanto calibra, não detecta inclinação. */
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MPU_PERIOD_MS));
            continue;
        }

        float roll_rel = roll - roll_center;
        float pitch_rel = pitch - pitch_center;

        /* Debug: imprime ~2 Hz (1 a cada MPU_SAMPLE_HZ/2 amostras) para
         * acompanhar os ângulos e ajustar TILT_ENTER_DEG sem floodar. */
        if (++dbg_count >= MPU_SAMPLE_HZ / 2) {
            dbg_count = 0;
            DBG("[MPU] roll=%.1f pitch=%.1f\n", roll_rel, pitch_rel);
        }

        /* ── Eixo roll → MOVE_RIGHT (>0) / MOVE_LEFT (<0) ──────────── */
        if (roll_state == 0) {
            if (roll_rel > TILT_ENTER_DEG) {
                roll_state = 1;
                game_cmd_t cmd = GAME_CMD_MOVE_RIGHT;
                xQueueSend(xQueueGameCmd, &cmd, 0);
                DBG("Virou para a direita\n");

            } else if (roll_rel < -TILT_ENTER_DEG) {
                roll_state = -1;
                game_cmd_t cmd = GAME_CMD_MOVE_LEFT;
                xQueueSend(xQueueGameCmd, &cmd, 0);
                DBG("Virou para a esquerda\n");
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
                DBG("Pulou!\n");
            } else if (pitch_rel < -TILT_ENTER_DEG) {
                pitch_state = -1;
                game_cmd_t cmd = GAME_CMD_ROLL;
                xQueueSend(xQueueGameCmd, &cmd, 0);
                DBG("Rolou!\n");
            }
        } else if (fabsf(pitch_rel) < TILT_EXIT_DEG) {
            pitch_state = 0; /* voltou ao centro: rearma */
        }

        /* Período fixo de amostragem (não acumula jitter). */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MPU_PERIOD_MS));
    }
}

/* ══════════════════════ Protocolo: comando → token ══════════════════ */
/* Traduz um game_cmd_t no token de texto enviado ao PC. Retorna NULL
 * para comandos que não têm representação no protocolo de saída. */
static const char *cmd_to_token(game_cmd_t cmd) {
    switch (cmd) {
        case GAME_CMD_MOVE_LEFT:  return "MOVE_LEFT";
        case GAME_CMD_MOVE_RIGHT: return "MOVE_RIGHT";
        case GAME_CMD_JUMP:       return "JUMP";
        case GAME_CMD_ROLL:       return "ROLL";
        case GAME_CMD_START:      return "START";
        case GAME_CMD_PAUSE:      return "PAUSE";
        case GAME_CMD_HOVERBOARD: return "HOVERBOARD";
        case GAME_CMD_RESET_IMU:  return "RESET_IMU";
        default:                  return NULL;
    }
}

/* ════════════════════════════════════════════════════════════════════
 * game_command_task — consome os comandos de alto nível publicados por
 * mpu_task e btn_task na xQueueGameCmd, converte para o token do
 * protocolo e encaminha para uart_tx_task. Centraliza a "tradução" de
 * evento de jogo → mensagem de fio num único ponto.
 * ════════════════════════════════════════════════════════════════════ */
void game_command_task(void *p) {
    (void)p;
    game_cmd_t cmd;
    uart_msg_t out;
    for (;;) {
        if (xQueueReceive(xQueueGameCmd, &cmd, portMAX_DELAY)) {
            const char *token = cmd_to_token(cmd);
            if (token == NULL) {
                continue; /* comando sem token: nada a enviar */
            }
            strncpy(out.text, token, sizeof(out.text) - 1);
            out.text[sizeof(out.text) - 1] = '\0';
            xQueueSend(xQueueUartTx, &out, 0);
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 * uart_tx_task — ÚNICO escritor da serial USB. Recebe mensagens prontas
 * e as envia como TOKEN + EOP. Concentrar a escrita numa só task evita
 * que prints de tasks diferentes se intercalem no mesmo fluxo.
 * ════════════════════════════════════════════════════════════════════ */
void uart_tx_task(void *p) {
    (void)p;
    uart_msg_t msg;
    for (;;) {
        if (xQueueReceive(xQueueUartTx, &msg, portMAX_DELAY)) {
            printf("%s%c", msg.text, EOP);
            stdio_flush(); /* envia já, sem esperar encher o buffer */
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 * status_task — ÚNICO dono do LED RGB. Mantém o estado de conexão e de
 * calibração e renderiza o LED por prioridade:
 *   calibrando → pisca branco; conectado → verde; senão → vermelho.
 * Recebe eventos por xQueueStatus e infere desconexão por timeout do
 * heartbeat (sem CONNECTED dentro de CONN_TIMEOUT_MS).
 * ════════════════════════════════════════════════════════════════════ */
void status_task(void *p) {
    (void)p;
    bool connected = false;
    bool calibrating = false;
    uint32_t last_conn_ms = 0;
    uint32_t death_until_ms = 0;   /* enquanto now < isto, LED fica vermelho */
    int blink = 0;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        /* Drena todos os eventos pendentes (não bloqueia). */
        status_evt_t e;
        while (xQueueReceive(xQueueStatus, &e, 0) == pdTRUE) {
            switch (e) {
                case STATUS_CONNECTED:
                    if (!connected) {
                        /* Borda de subida: avisa o háptico (conexão estabelecida). */
                        feedback_evt_t fb = FEEDBACK_SHORT;
                        xQueueSend(xQueueFeedback, &fb, 0);
                    }
                    connected = true;
                    last_conn_ms = to_ms_since_boot(get_absolute_time());
                    break;
                case STATUS_DISCONNECTED:
                    connected = false;
                    break;
                case STATUS_CAL_BEGIN:
                    calibrating = true;
                    break;
                case STATUS_CAL_END:
                    calibrating = false;
                    break;
                case STATUS_PLAYER_DIED:
                    death_until_ms =
                        to_ms_since_boot(get_absolute_time()) + DEATH_RED_MS;
                    break;
            }
        }

        /* Timeout do heartbeat: sem CONNECTED recente → desconectado. */
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (connected && (now - last_conn_ms > CONN_TIMEOUT_MS)) {
            connected = false;
        }

        /* Renderização por prioridade: calibrando > morte > conectado > off. */
        if (calibrating) {
            bool on = ((blink / STATUS_BLINK_TICKS) % 2) == 0;
            set_rgb(on, on, on);            /* branco piscando */
        } else if (now < death_until_ms) {
            set_rgb(true, false, false);    /* vermelho: morte do jogador */
        } else if (connected) {
            set_rgb(false, true, false);    /* verde: conectado */
        } else {
            set_rgb(true, true, false);    /* amarelo: desconectado */
        }
        blink++;

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(STATUS_PERIOD_MS));
    }
}

/* ════════════════════════════════════════════════════════════════════
 * uart_rx_task — lê mensagens do PC pela serial USB, separa por EOP e
 * interpreta os comandos. Por ora trata CONNECTED/DISCONNECTED e os
 * encaminha como eventos para o status_task.
 * ════════════════════════════════════════════════════════════════════ */
void uart_rx_task(void *p) {
    (void)p;
    char line[32];
    int idx = 0;

    for (;;) {
        int c = getchar_timeout_us(0);  /* não bloqueia */
        if (c == PICO_ERROR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (c == EOP) {
            line[idx] = '\0';
            idx = 0;
            status_evt_t e;
            if (strcmp(line, "CONNECTED") == 0) {
                e = STATUS_CONNECTED;
                xQueueSend(xQueueStatus, &e, 0);
            } else if (strcmp(line, "DISCONNECTED") == 0) {
                e = STATUS_DISCONNECTED;
                xQueueSend(xQueueStatus, &e, 0);
            } else if (strcmp(line, "PLAYER_DIED") == 0) {
                /* Evento vindo do PC: pisca vermelho + vibração longa. */
                e = STATUS_PLAYER_DIED;
                xQueueSend(xQueueStatus, &e, 0);
                feedback_evt_t fb = FEEDBACK_LONG;
                xQueueSend(xQueueFeedback, &fb, 0);
            }
        } else if (idx < (int)sizeof(line) - 1) {
            line[idx++] = (char)c;
        } else {
            idx = 0; /* linha longa demais: descarta */
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 * heartbeat_task — envia HEARTBEAT periódico ao PC. O PC responde
 * CONNECTED, o que mantém o status_task no estado "conectado".
 * ════════════════════════════════════════════════════════════════════ */
void heartbeat_task(void *p) {
    (void)p;
    uart_msg_t hb;
    strncpy(hb.text, "HEARTBEAT", sizeof(hb.text) - 1);
    hb.text[sizeof(hb.text) - 1] = '\0';
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        xQueueSend(xQueueUartTx, &hb, 0);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}

/* ════════════════════════════════════════════════════════════════════
 * feedback_task — ÚNICO dono do motor de vibração (GP18). Recebe eventos
 * por xQueueFeedback e toca um pulso: curto para eventos locais, longo
 * para a morte do jogador. Só esta task bloqueia durante o pulso, então
 * o resto do sistema não trava.
 * ════════════════════════════════════════════════════════════════════ */
void feedback_task(void *p) {
    (void)p;
    feedback_evt_t e;
    for (;;) {
        if (xQueueReceive(xQueueFeedback, &e, portMAX_DELAY)) {
            int ms = (e == FEEDBACK_LONG) ? VIBRA_LONG_MS : VIBRA_SHORT_MS;
            gpio_put(PIN_VIBRA, 1);
            vTaskDelay(pdMS_TO_TICKS(ms));
            gpio_put(PIN_VIBRA, 0);
        }
    }
}

int main(void) {
    /* Inicializações de stdio, botões e LED */
    stdio_init_all();

    set_sys_clock_khz(176000, true); 
    gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);
    int audio_pin_slice = pwm_gpio_to_slice_num(AUDIO_PIN);
    // Setup PWM interrupt to fire when PWM cycle is complete
    pwm_clear_irq(audio_pin_slice);
    pwm_set_irq_enabled(audio_pin_slice, true);
    // set the handle function above
    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_interrupt_handler); 
    irq_set_enabled(PWM_IRQ_WRAP, true);

    // Setup PWM for audio output
    pwm_config config = pwm_get_default_config();
    /* Base clock 176,000,000 Hz divide by wrap 250 then the clock divider further divides
     * to set the interrupt rate. 
     * 
     * 11 KHz is fine for speech. Phone lines generally sample at 8 KHz
     * 
     * 
     * So clkdiv should be as follows for given sample rate
     *  8.0f for 11 KHz
     *  4.0f for 22 KHz
     *  2.0f for 44 KHz etc
     */
    pwm_config_set_clkdiv(&config, 8.0f); 
    pwm_config_set_wrap(&config, 255); 
    pwm_init(audio_pin_slice, &config, true);

    pwm_set_gpio_level(AUDIO_PIN, 0);

    gpio_init(RGB_PIN_R);
    gpio_set_dir(RGB_PIN_R, GPIO_OUT);
    gpio_put(RGB_PIN_R, 0);

    gpio_init(RGB_PIN_B);
    gpio_set_dir(RGB_PIN_B, GPIO_OUT);
    gpio_put(RGB_PIN_B, 0);

    gpio_init(RGB_PIN_G);
    gpio_set_dir(RGB_PIN_G, GPIO_OUT);
    gpio_put(RGB_PIN_G, 0);

    /* Motor de vibração (ativo em nível alto). */
    gpio_init(PIN_VIBRA);
    gpio_set_dir(PIN_VIBRA, GPIO_OUT);
    gpio_put(PIN_VIBRA, 0);

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
    xQueueUartTx = xQueueCreate(32, sizeof(uart_msg_t));
    xQueueStatus = xQueueCreate(16, sizeof(status_evt_t));
    xQueueFeedback = xQueueCreate(16, sizeof(feedback_evt_t));
    xSemaphoreCalibrateImu = xSemaphoreCreateBinary();

    /* Tasks */
    xTaskCreate(mpu_task, "mpu", 4096, NULL, 2, NULL);
    xTaskCreate(btn_task, "btn task", 4096, NULL, 2, NULL);
    xTaskCreate(game_command_task, "gamecmd", 2048, NULL, 2, NULL);
    xTaskCreate(uart_tx_task, "uarttx", 2048, NULL, 2, NULL);
    xTaskCreate(uart_rx_task, "uartrx", 2048, NULL, 2, NULL);
    xTaskCreate(status_task, "status", 1024, NULL, 2, NULL);
    xTaskCreate(heartbeat_task, "hbeat", 512, NULL, 1, NULL);
    xTaskCreate(feedback_task, "feedback", 512, NULL, 2, NULL);

    vTaskStartScheduler();

    // Should never reach here
    while(1) {
        __wfi(); // Wait for Interrupt
    }
}
