# Punto Switcher для Linux (C-версия)

Легковесная реализация Punto Switcher на C для Linux с использованием Interception Tools.
Позволяет исправлять текст, набранный в неправильной раскладке клавиатуры.

## Возможности

- **Ручная инверсия слова (Pause)**: Исправление последнего набранного слова с сохранением регистра
- **Повторная инверсия (Pause)**: Многократное нажатие Pause переключает слово туда-обратно
- **Инверсия выделенного текста (Shift+Pause)**: Исправление любого выделенного фрагмента текста
- **Поддержка системных хоткеев**: Ctrl, Alt, Meta комбинации работают без помех
- **Минимальное потребление ресурсов**: Написано на чистом C

## Требования

### Системные зависимости

#### Ubuntu/Debian

```bash
# Сборка
sudo apt install build-essential cmake

# Interception Tools (ядро системы)
sudo apt install interception-tools

# Инструменты для работы с буфером обмена и X11
sudo apt install xclip xsel xdotool x11-utils

# Python 3 (для скрипта инверсии выделенного текста)
sudo apt install python3
```

#### Arch Linux

```bash
sudo pacman -S base-devel cmake interception-tools xclip xsel xdotool xorg-xprop python
```

#### Fedora

```bash
sudo dnf install gcc make cmake interception-tools xclip xsel xdotool xprop python3
```

### Назначение зависимостей

| Пакет                | Назначение                                                         |
| ------------------------- | ---------------------------------------------------------------------------- |
| `build-essential`       | Компилятор GCC и make                                             |
| `interception-tools`    | Перехват и эмуляция клавиатурных событий |
| `xclip`                 | Работа с буфером обмена X11                              |
| `xsel`                  | Работа с буфером обмена X11 (альтернатива)   |
| `xdotool`               | Эмуляция нажатий клавиш в X11                          |
| `x11-utils` (`xprop`) | Определение активного окна                           |
| `python3`               | Для скрипта `punto-invert`                                       |

### Сборка Interception Tools из исходников

Если `interception-tools` недоступен в репозиториях:

```bash
git clone https://gitlab.com/interception/linux/tools.git interception-tools
cd interception-tools
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

## Установка

### Способ 1: Установка deb-пакета (рекомендуется)

```bash
# Скачайте или соберите пакет
git clone https://github.com/antonshalin76/punto.git
sudo apt install ./punto-ubuntu.deb
```

Пакет автоматически:

- Установит все зависимости
- Скопирует бинарники в `/usr/local/bin/`
- Создаст конфигурации в `/etc/punto/` и `/etc/interception/`
- Включит и запустит сервис `udevmon`

### Способ 2: Сборка deb-пакета из исходников

```bash
git clone https://github.com/antonshalin76/punto.git
./build-deb.sh
sudo apt install ./punto-ubuntu.deb
```

### Способ 3: Ручная установка

```bash
make clean && make
sudo make install
```

### Настройка udevmon

Создайте файл конфигурации `/etc/interception/udevmon.yaml`:

```yaml
- JOB: "intercept -g $DEVNODE | punto | uinput -d $DEVNODE"
  DEVICE:
    EVENTS:
      EV_KEY: [KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z, KEY_SPACE, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_PAUSE, KEY_DOT, KEY_COMMA, KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTMETA, KEY_RIGHTMETA, KEY_GRAVE]
```

### Запуск сервиса

```bash
sudo systemctl enable udevmon
sudo systemctl start udevmon
```

## Использование

### Горячие клавиши

| Комбинация               | Действие                                                                               |
| ---------------------------------- | ---------------------------------------------------------------------------------------------- |
| **Pause**                    | Инвертировать раскладку текущего/последнего слова |
| **Pause** (повторно) | Инвертировать обратно (toggle)                                             |
| **Shift+Pause**              | Инвертировать раскладку выделенного текста              |

### Примеры

#### Инверсия слова

1. Набираете: `ghbdtn` (вместо "привет")
2. Нажимаете: **Pause**
3. Результат: `привет`

#### Инверсия с сохранением регистра

1. Набираете: `GhBdTn`
2. Нажимаете: **Pause**
3. Результат: `ПрИвЕт`

#### Инверсия выделенного текста

1. Набираете: `Yfgbitv ldf ckjdf`
2. Выделяете текст (Ctrl+A или мышью)
3. Нажимаете: **Shift+Pause**
4. Результат: `Напишем два слова`

## Архитектура

```
┌─────────────────────────────────────────────────────────────┐
│                         udevmon                             │
│  (запускает pipeline для каждой клавиатуры)                 │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  intercept -g $DEVNODE                                      │
│  (перехватывает события клавиатуры)                         │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  punto (C)                                                  │
│  • Буферизирует набранные символы                           │
│  • Отслеживает модификаторы (Shift, Ctrl, Alt, Meta)        │
│  • Обрабатывает Pause/Shift+Pause                           │
│  • Вызывает punto-switch для переключения раскладки         │
│  • Вызывает punto-invert для инверсии выделенного текста    │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  uinput -d $DEVNODE                                         │
│  (эмулирует нажатия клавиш)                                 │
└─────────────────────────────────────────────────────────────┘
```

### Компоненты

| Файл         | Назначение                                                                    |
| ---------------- | --------------------------------------------------------------------------------------- |
| `src/punto.c`  | Основная логика обработки клавиш                           |
| `punto-switch` | Bash-скрипт переключения раскладки через IBus/GSettings |
| `punto-invert` | Python-скрипт инверсии выделенного текста                |
| `udevmon.yaml` | Конфигурация для udevmon                                                 |
| `Makefile`     | Сборка и установка                                                      |

## Настройка

Конфигурационный файл: `/etc/punto/config.yaml`

```yaml
# Punto Switcher Configuration

# Хоткей переключения раскладки
hotkey:
  modifier: leftctrl    # leftctrl, rightctrl, leftalt, rightalt, leftshift, rightshift
  key: grave            # grave (` ~), space, tab, backslash, capslock

# Задержки (в миллисекундах)
delays:
  key_press: 20         # Задержка между нажатием и отпусканием клавиши
  layout_switch: 100    # Задержка после переключения раскладки
  retype: 3             # Задержка между символами при перепечатывании
```

### Доступные клавиши

| Имя                        | Клавиша |
| ----------------------------- | -------------- |
| `leftctrl`, `rightctrl`   | Ctrl           |
| `leftalt`, `rightalt`     | Alt            |
| `leftshift`, `rightshift` | Shift          |
| `leftmeta`, `rightmeta`   | Super/Win      |
| `grave`                     | \` ~           |
| `space`                     | Пробел   |
| `tab`                       | Tab            |
| `backslash`                 | \\             |
| `capslock`                  | Caps Lock      |

После изменения конфига перезапустите сервис:

```bash
sudo systemctl restart udevmon
```

## Удаление

### Если установлен через deb-пакет

```bash
sudo apt remove punto-switcher
```

Для полного удаления вместе с конфигурацией:

```bash
sudo apt purge punto-switcher
sudo rm -rf /etc/punto
```

### Если установлен вручную (make install)

```bash
sudo make uninstall
sudo systemctl stop udevmon
sudo systemctl disable udevmon
sudo rm -rf /etc/punto
```

## Решение проблем

### Проверка работы сервиса

```bash
sudo systemctl status udevmon
sudo journalctl -u udevmon -f
```

### Клавиатура не работает после установки

```bash
sudo systemctl stop udevmon
```

### Инверсия выделенного текста не работает

Убедитесь, что установлены зависимости:

```bash
sudo apt install xclip xsel xdotool
```

### Переключение раскладки не работает

Проверьте, что ваш хоткей переключения раскладки соответствует
настроенному в `punto.c` (по умолчанию `Ctrl+\``).

## Лицензия

MIT License

## Автор

Anton Shalin <anton.shalin@gmail.com>
