"""Рандомний рух мишки — без жодних кліків.

Запуск:  python mouse_mover.py
Зупинка: Ctrl+C у терміналі, або різко посунь мишку у лівий верхній кут екрана.

Потрібна бібліотека pyautogui:  pip install pyautogui
"""

import random
import sys
import time

try:
    import pyautogui
except ImportError:
    print("Спочатку встанови бібліотеку:  pip install pyautogui")
    sys.exit(1)

# Захист: якщо посунути мишку у лівий верхній кут — програма зупиниться
pyautogui.FAILSAFE = True

MIN_PAUSE = 1.0   # мінімальна пауза між рухами, секунд
MAX_PAUSE = 4.0   # максимальна пауза між рухами, секунд
MARGIN = 50       # відступ від країв екрана, пікселів


def main():
    width, height = pyautogui.size()
    print(f"Екран: {width}x{height}. Воджу мишкою без кліків.")
    print("Зупинка: Ctrl+C або мишку в лівий верхній кут.")

    while True:
        x = random.randint(MARGIN, width - MARGIN)
        y = random.randint(MARGIN, height - MARGIN)
        duration = random.uniform(0.5, 2.0)  # плавний рух, а не стрибок
        pyautogui.moveTo(x, y, duration=duration, tween=pyautogui.easeInOutQuad)
        time.sleep(random.uniform(MIN_PAUSE, MAX_PAUSE))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nЗупинено.")
    except pyautogui.FailSafeException:
        print("\nЗупинено (мишка в куті екрана).")
