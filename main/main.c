#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>

#include <stdio.h>
#include "pico/stdlib.h"

//Definindo Botões (mudar conforme o desejado)

const int LED_PIN = 14;
const int BTN_PIN_PAUSE = 26;
const int BTN_PIN_HOVERBOARD = 25;
const int BTN_PIN_IMU = 24;


/* Semaphores e Queues */
SemaphoreHandle_t semaphores[4];

QueueHandle_t xQueueBtn;
SemaphoreHandle_t xSemaphoreCalibrateImu;



/* Plano
1.  GPIO inits + btn_calback
1.2 Criação de filas enquanto vai criando btn_callback


2.  Criação das tasks
2.1 Counter_task
2.2 b_task

3.  Inicialização das tasks na main
*/

/* Task function */

void btn_callback(uint gpio, uint32_t events) {
    if (events == GPIO_IRQ_EDGE_FALL) {
        if (gpio == BTN_PIN_PAUSE){ //botão que pausa o jogo
            xQueueSendFromISR(xQueueBtn,BTN_PIN_PAUSE , 0);
        }
        else if (gpio == BTN_PIN_HOVERBOARD){ //botão que ativa o hoverboard
            xQueueSendFromISR(xQueueBtn,BTN_PIN_HOVERBOARD , 0);
        }
        else { //botão de reset da imu
            xQueueSendFromISR(xQueueBtn,BTN_PIN_IMU , 0);
        }
    }
}

void btn_task(void *p){
    int btn = 0;
    while (true){
        if (xQueueReceive(xQueueBtn, &btn, portMAX_DELAY)){
            if (btn == BTN_PIN_PAUSE){
                //Pausa o jogo
                //Indica com o led em amarelo
            }
            else if (btn == BTN_PIN_HOVERBOARD){
                //Ativa o Hoverboard
                //Indica com o led verde que o hoverboard está ativado
            }
            else{//Manda semáforo que reseta a imu// Indica piscando o led em amarelo que a imu está resetando
                xSemaphoreGive(xSemaphoreCalibrateImu);              
            }
            
        }
    }
}

void mpu_task(void *p){
    while (true){
        if (xSemaphoreTake(xSemaphoreCalibrateImu, pdMS_TO_TICKS(10) )){
            //Recalibra a imu
        } 
         
    }


}











// void vTask(void *pvParameters)
// {
//     int taskNum = (int)pvParameters;

//     for (;;)
//     {
//         // Wait for my semaphore
//         xSemaphoreTake(semaphores[taskNum], portMAX_DELAY);

//         // Critical section: do my work
//         printf("Hello from task %d\n", taskNum + 1);

//         // Release next task’s semaphore
//         int nextTask = (taskNum + 1) % 4;
//         vTaskDelay(pdTICKS_TO_MS(100)); // Simulate work with a delay
//         xSemaphoreGive(semaphores[nextTask]);
//     }
// }

int main(void){

    //Inicializações botões e leds
    stdio_init_all();
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);
    
    gpio_init(BTN_PIN_PAUSE);
    gpio_set_dir(BTN_PIN_PAUSE, GPIO_IN);
    gpio_pull_up(BTN_PIN_PAUSE);

    gpio_init(BTN_PIN_HOVERBOARD);
    gpio_set_dir(BTN_PIN_HOVERBOARD, GPIO_IN);
    gpio_pull_up(BTN_PIN_HOVERBOARD);

    gpio_init(BTN_PIN_IMU);
    gpio_set_dir(BTN_PIN_IMU, GPIO_IN);
    gpio_pull_up(BTN_PIN_IMU);

    //Registro callback
    gpio_set_irq_enabled_with_callback(BTN_PIN_PAUSE, GPIO_IRQ_EDGE_FALL, true, &btn_callback);
    gpio_set_irq_enabled(BTN_PIN_HOVERBOARD, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(BTN_PIN_IMU, GPIO_IRQ_EDGE_FALL, true);



    // for (int i = 0; i < 4; i++)
    // {
    //     semaphores[i] = xSemaphoreCreateBinary();
    // }

    // for (int i = 0; i < 4; i++)
    // {
    //     char name[10];
    //     snprintf(name, sizeof(name), "Task%d", i + 1);
    //     xTaskCreate(vTask, name, configMINIMAL_STACK_SIZE, (void *)i, 1, NULL);
    // }

    // xSemaphoreGive(semaphores[0]);

    //Criação de filas e semáforos
    xQueueBtn = xQueueCreate(32, sizeof(int));
    xSemaphoreCalibrateImu = xSemaphoreCreateBinary();

    vTaskStartScheduler();

    // Should never reach here
    for (;;);
}
