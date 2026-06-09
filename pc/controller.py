"""
Ponte Controle (Pico) -> Jogo (Subway Surfers no navegador).

Lê os pacotes que o Pico envia pela serial USB (CDC), separa por EOP ('\n')
e simula a tecla correspondente na janela do jogo que estiver em foco.

Uso:
    python controller.py                 # usa a porta padrão (COM7)
    python controller.py --port COM5     # outra porta
    (descubra a porta com:  python -m serial.tools.list_ports )

Tokens recebidos do controle (Controle -> PC), em sincronia com main.c:
    MOVE_LEFT  MOVE_RIGHT  JUMP  ROLL  START  PAUSE  HOVERBOARD  RESET_IMU
"""

import argparse
import sys

import serial
import pyautogui

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

# Segurança do pyautogui: não aborta ao levar o mouse ao canto da tela.
pyautogui.FAILSAFE = False


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

    pyautogui.press(key)
    print(f"[tecla]    {token} -> {key}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Ponte Pico -> Subway Surfers")
    parser.add_argument("--port", default=DEFAULT_PORT,
                        help=f"porta serial do Pico (padrão: {DEFAULT_PORT})")
    parser.add_argument("--baud", type=int, default=BAUDRATE,
                        help="baud rate (ignorado em USB CDC)")
    args = parser.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as exc:
        print(f"Erro ao abrir {args.port}: {exc}", file=sys.stderr)
        return 1

    print(f"Conectado em {args.port}. Deixe a janela do jogo em foco. Ctrl+C para sair.")
    try:
        while True:
            # readline() lê até o EOP ('\n') ou estourar o timeout.
            raw = ser.readline()
            if not raw:
                continue  # timeout sem dado: segue tentando
            token = raw.decode("utf-8", errors="ignore").strip()
            if token:
                handle_token(token)
    except KeyboardInterrupt:
        print("\nEncerrando.")
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
