

# APS 2 - Controle Embarcado

## 1. Jogo

O projeto será desenvolvido para o jogo Subway Surfers.

No jogo, o personagem corre automaticamente para frente e o jogador precisa desviar de obstáculos, coletar itens e sobreviver pelo maior tempo possível.

As ações principais do jogo serão:

- Mudar para a faixa da direita
- Mudar para a faixa da esquerda
- Pular obstáculos
- Rolar/deslizar por baixo de obstáculos
- Ativar o hoverboard como habilidade especial
---

## 2. Ideia do controle

O controle será um **controle físico em formato de hoverboard**. A fabricação poderá ser feita por:

- Impressão 3D da carcaça ou
- Corte emm mdf + corte em acrílico com montagem em camadas

  A estrutura mecânica prevista terá:

- Corpo principal alongado em formato de hoverboard
- Parte superior plana para fixação dos botões
- Espaço interno para Raspberry Pi Pico, IMU, fios e bateria
- Abertura para cabo
- Abertura para LED de status
- Base inferior com apoios simple

O jogador irá segurar o controle com as duas mãos e controlar o personagem por meio de inclinações detectadas por uma IMU.

### Comandos por inclinação

| Movimento físico do controle | Ação no jogo |
|---|---|
| Inclinar para a direita | Personagem muda para a faixa da direita |
| Inclinar para a esquerda | Personagem muda para a faixa da esquerda |
| Inclinar para cima | Personagem pula |
| Inclinar para baixo | Personagem rola/desliza |

### Botões físicos

O controle terá botões físicos na parte superior:

| Botão | Função |
|---|---|
| Pause | Pausa/retoma o jogo |
| Reset IMU | Recalibra o centro da IMU |
| Hoverboard | Ativa a habilidade especial |



##  Inputs e Outputs

## 3.1 Inputs

### IMU

A IMU será o principal sensor do controle.

Ela será usada para detectar a inclinação do hoverboard e transformar o movimento físico em comandos do jogo.

Funções da IMU:

- Detectar inclinação para a direita
- Detectar inclinação para a esquerda
- Detectar inclinação para cima
- Detectar inclinação para baixo
- Permitir calibração do centro/neutro do controle

### Botões digitais

Os botões digitais serão conectados a GPIOs e tratados por interrupção/callback.

| Entrada | Tipo | Função |
|---|---|---|
| Botão Pause | Digital | Pausar/retomar o jogo |
| Botão Reset IMU | Digital | Recalibrar a IMU |
| Botão Hoverboard | Digital | Ativar habilidade especial |


---

## 3.2 Outputs

### LED de status

O controle terá um LED de status para indicar o estado do sistema.

| Estado | Indicação |
|---|---|
| Controle ligado | LED aceso |
| Controle conectado ao PC | LED verde |
| Calibração da IMU | LED piscando |
| Erro/desconectado | LED vermelho ou piscando rápido |

### Vibração ou buzzer

O controle poderá ter um motor de vibração ou buzzer para feedback físico.

Usos previstos:

- Vibração curta ao ativar o hoverboard
- Vibração ou sinal sonoro ao recalibrar a IMU
- Vibração ao colidir/perder no jogo, caso o PC envie esse evento para o controle

---

## 4. Protocolo utilizado

A comunicação entre o controle e o computador será feita por **UART**, seguindo a lógica dos Labs 5/6 da disciplina.
O Raspberry Pi Pico enviará comandos para um programa em Python rodando no computador. Cada comando será enviado como um pacote simples, indicando a ação detectada no controle.
Para delimitar o fim de cada mensagem, será utilizado um byte de EOP no fim de cada mensagem.

## 4. Diagrama de Blocos firmware
<img width="1664" height="3553" alt="diagrama_firmware_aps_2" src="https://github.com/user-attachments/assets/0953eb2e-74a1-4b5b-9c75-81ea32af9eeb" />


Adicionar apenas na entrega final.

## 5. Proposta de controle
<img width="1448" height="1086" alt="image" src="https://github.com/user-attachments/assets/092d2fd6-ab2f-43a0-9e85-eb22f2e60134" />

```markdown






