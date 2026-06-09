"""
Ponte Controle (Pico) <-> Jogo (Subway Surfers no navegador).

- Lê os pacotes que o Pico envia pela serial USB (CDC), separa por EOP ('\n')
  e simula a tecla correspondente na janela do jogo que estiver em foco.
- Responde CONNECTED ao HEARTBEAT do controle (mantém o LED de status verde).
- Tecla de teste 'm' envia PLAYER_DIED (simula a morte do jogador) para
  acionar o LED vermelho + vibração no controle.

Uso:
    python controller.py                 # usa a porta padrão (COM7)
    python controller.py --port COM5     # outra porta
    (descubra a porta com:  python -m serial.tools.list_ports )

Protocolo (em sincronia com main.c):
    Controle -> PC : MOVE_LEFT MOVE_RIGHT JUMP ROLL START PAUSE HOVERBOARD
                     RESET_IMU HEARTBEAT
    PC -> Controle : CONNECTED DISCONNECTED PLAYER_DIED
"""

import argparse
import sys
import threading

import serial
import pydirectinput
from pynput import keyboard

# EOP definido no firmware (main.c: #define EOP '\n').
EOP = "\n"

# Porta serial padrão do Pico nesta máquina.
DEFAULT_PORT = "COM7"

# Baud rate é irrelevante para USB CDC (porta serial virtual), mas o
# pyserial exige um valor. Qualquer número válido serve.
BAUDRATE = 115200

# Mapeia cada token do protocolo na tecla simulada no jogo de navegador.
# Ajuste conforme a sua versão do Subway Surfers (setas é o mais comum).
# RESET_IMU é tratado localmente no controle, então é ignorado aqui.
TOKEN_TO_KEY = {
    "MOVE_LEFT": "left",
    "MOVE_RIGHT": "right",
    "JUMP": "up",
    "ROLL": "down",
    "HOVERBOARD": "space",
    "START": "enter",
    "PAUSE": "esc",
    "RESET_IMU": None,
}

# pydirectinput injeta SCANCODES de hardware via SendInput (não só o
# virtual-key code). Assim o navegador preenche event.code (ex: "Escape"),
# que muitos jogos checam — é o que faz o Esc sintético ser detectado,
# ao contrário do pyautogui.
pydirectinput.FAILSAFE = False
pydirectinput.PAUSE = 0  # sem atraso entre comandos: menor latência

# Serial compartilhada entre o loop principal (respostas ao heartbeat) e a
# thread do teclado (PLAYER_DIED). Protegida por lock.
_ser = None
_ser_lock = threading.Lock()


def send(msg: str) -> None:
    """Escreve uma mensagem (TOKEN + EOP) na serial, de forma thread-safe."""
    if _ser is None:
        return
    with _ser_lock:
        try:
            _ser.write((msg + EOP).encode())
        except serial.SerialException:
            pass


def handle_token(token: str) -> None:
    """Traduz um token em uma tecla e a pressiona, se houver mapeamento."""
    if token not in TOKEN_TO_KEY:
        # Linha desconhecida (ex: debug do firmware) -> apenas mostra e ignora.
        print(f"[ignorado] {token!r}")
        return

    key = TOKEN_TO_KEY[token]
    if key is None:
        print(f"[interno]  {token}")
        return

    pydirectinput.press(key)
    print(f"[tecla]    {token} -> {key}")


# ── Tecla de teste: 'm' envia PLAYER_DIED (uma vez por aperto) ──────────
_m_down = False


def on_press(key) -> None:
    global _m_down
    try:
        if key.char == "m" and not _m_down:
            _m_down = True
            send("PLAYER_DIED")
            print("[teste]    PLAYER_DIED enviado")
    except AttributeError:
        pass  # teclas especiais (shift, etc.) não têm .char


def on_release(key) -> None:
    global _m_down
    try:
        if key.char == "m":
            _m_down = False
    except AttributeError:
        pass


def main() -> int:
    global _ser
    parser = argparse.ArgumentParser(description="Ponte Pico <-> Subway Surfers")
    parser.add_argument("--port", default=DEFAULT_PORT,
                        help=f"porta serial do Pico (padrão: {DEFAULT_PORT})")
    parser.add_argument("--baud", type=int, default=BAUDRATE,
                        help="baud rate (ignorado em USB CDC)")
    args = parser.parse_args()

    try:
        _ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as exc:
        print(f"Erro ao abrir {args.port}: {exc}", file=sys.stderr)
        return 1

    # Escuta global do teclado para a tecla de teste 'm'.
    listener = keyboard.Listener(on_press=on_press, on_release=on_release)
    listener.start()

    print(f"Conectado em {args.port}. Jogo em foco; 'm' simula morte. Ctrl+C para sair.")
    # Anuncia conexão imediatamente (o LED do controle deve ficar verde).
    send("CONNECTED")
    try:
        while True:
            # readline() lê até o EOP ('\n') ou estourar o timeout.
            raw = _ser.readline()
            if not raw:
                continue  # timeout sem dado: segue tentando
            token = raw.decode("utf-8", errors="ignore").strip()
            if not token:
                continue
            if token == "HEARTBEAT":
                # Responde ao heartbeat para manter o LED de status conectado.
                send("CONNECTED")
                continue
            handle_token(token)
    except KeyboardInterrupt:
        print("\nEncerrando.")
    finally:
        # Avisa o controle que o PC saiu (LED volta a vermelho na hora).
        send("DISCONNECTED")
        listener.stop()
        _ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
