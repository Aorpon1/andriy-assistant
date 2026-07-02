"""Рандомний рух мишки — без жодних кліків.

Запуск на Windows: подвійний клік по start_mouse_mover.bat
   або в терміналі:  python mouse_mover.py

Зупинка: Ctrl+C у вікні програми або просто закрий її вікно.

На Windows нічого встановлювати не треба (працює через вбудований ctypes).
На Mac/Linux потрібна бібліотека pyautogui:  pip install pyautogui
"""

import math
import platform
import random
import sys
import time

MIN_PAUSE = 1.0   # мінімальна пауза між рухами, секунд
MAX_PAUSE = 4.0   # максимальна пауза між рухами, секунд
MARGIN = 50       # відступ від країв екрана, пікселів


def make_backend():
    """Повертає (розмір_екрана, функція_переміщення) без зайвих залежностей."""
    if platform.system() == "Windows":
        import ctypes

        user32 = ctypes.windll.user32
        user32.SetProcessDPIAware()
        size = (user32.GetSystemMetrics(0), user32.GetSystemMetrics(1))

        def move_to(x, y):
            user32.SetCursorPos(int(x), int(y))

        return size, move_to

    try:
        import pyautogui
    except ImportError:
        print("Спочатку встанови бібліотеку:  pip install pyautogui")
        input("Натисни Enter, щоб закрити...")
        sys.exit(1)

    pyautogui.FAILSAFE = False  # плавність робимо самі, курсор у кути не заходить

    def move_to(x, y):
        pyautogui.moveTo(int(x), int(y), _pause=False)

    return pyautogui.size(), move_to


def smooth_move(move_to, x1, y1, x2, y2, duration):
    """Плавний рух від (x1,y1) до (x2,y2) за duration секунд."""
    steps = max(int(duration * 60), 2)
    for i in range(1, steps + 1):
        t = i / steps
        # ease-in-out: повільний старт і фініш, як у живої руки
        t = (1 - math.cos(t * math.pi)) / 2
        move_to(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t)
        time.sleep(duration / steps)


def main():
    (width, height), move_to = make_backend()
    print(f"Екран: {width}x{height}. Воджу мишкою без кліків.")
    print("Зупинка: Ctrl+C або закрий це вікно.")

    x, y = width // 2, height // 2
    move_to(x, y)

    while True:
        nx = random.randint(MARGIN, width - MARGIN)
        ny = random.randint(MARGIN, height - MARGIN)
        smooth_move(move_to, x, y, nx, ny, random.uniform(0.5, 2.0))
        x, y = nx, ny
        time.sleep(random.uniform(MIN_PAUSE, MAX_PAUSE))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nЗупинено.")
