# Punto Switcher для Linux

Высокопроизводительная реализация Punto Switcher на C++20 для Linux.
Позволяет исправлять текст, набранный в неправильной раскладке клавиатуры.

![Version](https://img.shields.io/badge/version-2.2.0-blue)
![C++](https://img.shields.io/badge/C%2B%2B-20-orange)
![License](https://img.shields.io/badge/license-MIT-green)

## Возможности

### Управление через трей (v2.2)

Иконка `punto-tray` в системном трее позволяет:
- **Визуальный статус** — видно, включено ли автопереключение
- **Быстрое вкл/выкл** — без перезапуска сервиса
- **Настройки** — встроенный модальный диалог (GTK), редактирует `~/.config/punto/config.yaml`
- **Сохранить** — мгновенное применение (hot reload) без рестарта udevmon

### Автоматическое переключение (v2.1)

| Режим                | Действие                                                      |
| -------------------- | ------------------------------------------------------------- |
| **АВТО** (при пробеле) | Анализирует слово и переключает раскладку если нужно        |

Гибридный анализ:
- **Словарь** (приоритет) — hunspell словари EN/RU
- **N-граммы** (fallback) — частотный анализ биграмм

### Ручные горячие клавиши

| Комбинация           | Действие                                   |
| -------------------- | ------------------------------------------ |
| **Pause**            | Инвертировать раскладку последнего слова   |
| **Shift+Pause**      | Инвертировать раскладку выделенного текста |
| **Ctrl+Pause**       | Инвертировать регистр последнего слова     |
| **Alt+Pause**        | Инвертировать регистр выделенного текста   |
| **LCtrl+LAlt+Pause** | Транслитерировать выделенный текст         |

### Примеры

```
ghbdtn[пробел]  →  [АВТО]  →  привет 
ghbdtn  →  [Pause]  →  привет
пРИВЕТ  →  [Ctrl+Pause]  →  Привет
Privet  →  [LCtrl+LAlt+Pause]  →  Привет
```

## Архитектура

```
┌─────────────────────────────────────────────────────────────┐
│                         udevmon                             │
│        (запускает pipeline для каждой клавиатуры)           │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  intercept -g $DEVNODE                                      │
│              (перехватывает события клавиатуры)             │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    punto (C++20)                            │
│  ┌─────────────┐ ┌─────────────┐ ┌──────────────────┐       │
│  │ EventLoop   │ │ InputBuffer │ │ ClipboardManager │       │
│  └─────────────┘ └─────────────┘ └──────────────────┘       │
│  ┌─────────────┐ ┌─────────────┐ ┌──────────────────┐       │
│  │ KeyInjector │ │TextProcessor│ │   X11Session     │       │
│  └─────────────┘ └─────────────┘ └──────────────────┘       │
│  ┌───────────────┐ ┌────────────┐ ┌──────────────────┐       │
│  │LayoutAnalyzer │ │ Dictionary │ │   IpcServer    │       │
│  └───────────────┘ └────────────┘ └──────────────────┘       │
│  ┌───────────────┐ ┌────────────┐ ┌──────────────────┐       │
│  │  punto-tray   │ │            │ │                  │       │
│  │  (GTK3)       │ │            │ │                  │       │
│  └───────────────┘ └────────────┘ └──────────────────┘       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  uinput -d $DEVNODE                                         │
│              (эмулирует нажатия клавиш)                     │
└─────────────────────────────────────────────────────────────┘
```

### Ключевые особенности v2.2

- **Управление через трей** — иконка `punto-tray` для быстрого управления
- **Встроенные настройки** — модальное окно, редактирует `~/.config/punto/config.yaml`
- **IPC через Unix Socket** — команды GET_STATUS, SET_STATUS, RELOAD
- **Hot Reload конфига** — применение настроек без перезапуска udevmon
- **Автопереключение раскладки** — гибридный анализ (словарь + биграммы)
- **Hunspell словари** — высокая точность определения языка
- **Zero external dependencies** — никаких Python, xdotool, xclip
- **< 1ms латентность** — вместо 200-500ms при вызове скриптов

## Установка

### Способ 1: Установка deb-пакета (рекомендуется)

```bash
git clone https://github.com/antonshalin76/punto.git
cd punto
sudo dpkg -i punto-switcher_2.2.0_amd64.deb
```

### Способ 2: Сборка из исходников

#### Зависимости

```bash
# Ubuntu/Debian
sudo apt install build-essential cmake libx11-dev interception-tools xsel

# Arch Linux
sudo pacman -S base-devel cmake libx11 interception-tools xsel

# Fedora
sudo dnf install gcc-c++ cmake libX11-devel interception-tools xsel
```

#### Сборка

```bash
git clone https://github.com/antonshalin76/punto.git
cd punto
./build-deb.sh
sudo dpkg -i punto-switcher_2.2.0_amd64.deb
```

#### Ручная сборка без пакета

```bash
cd cpp
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
sudo cp punto /usr/local/bin/
sudo cp ../../config.yaml /etc/punto/
```

### Настройка udevmon

Создайте `/etc/interception/udevmon.yaml`:

```yaml
- JOB: "intercept -g $DEVNODE | punto | uinput -d $DEVNODE"
  DEVICE:
    EVENTS:
      EV_KEY: [KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H,
               KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P,
               KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X,
               KEY_Y, KEY_Z, KEY_SPACE, KEY_ENTER, KEY_TAB, KEY_BACKSPACE,
               KEY_PAUSE, KEY_DOT, KEY_COMMA, KEY_SEMICOLON, KEY_APOSTROPHE,
               KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_GRAVE, KEY_SLASH,
               KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_RIGHTCTRL,
               KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTMETA, KEY_RIGHTMETA,
               KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
               KEY_MINUS, KEY_EQUAL, KEY_BACKSLASH,
               KEY_KP0, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4, KEY_KP5,
               KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9, KEY_KPDOT,
               KEY_KPMINUS, KEY_KPPLUS, KEY_KPASTERISK, KEY_KPSLASH, KEY_KPENTER]
```

### Запуск

```bash
sudo systemctl enable udevmon
sudo systemctl start udevmon
```

## Настройка

Конфигурация по умолчанию: `/etc/punto/config.yaml`

Пользовательский конфиг (используется в приоритете): `~/.config/punto/config.yaml`

```yaml
# Хоткей переключения раскладки (ваша системная комбинация)
hotkey:
  modifier: leftctrl   # leftctrl, rightctrl, leftalt, rightalt
  key: grave           # grave (` ~), space, tab, capslock

# Задержки (в миллисекундах)
delays:
  key_press: 12        # Задержка между нажатием и отпусканием
  layout_switch: 150   # Задержка после переключения раскладки
  retype: 15           # Задержка между символами при перепечатывании
  turbo_key_press: 12  # Турбо-задержки для автокоррекции
  turbo_retype: 20

# Автоматическое переключение раскладки при нажатии пробела
auto_switch:
  enabled: true        # Включить автопереключение
  threshold: 3.5       # Порог срабатывания (разница скоров)
  min_word_len: 2      # Минимальная длина слова для анализа
  min_score: 5.0       # Минимальный скор для уверенного решения
```

После изменения можно применить настройки без перезапуска:

```bash
# Через tray-приложение: диалог настроек -> "Сохранить" (применяется сразу)

# Или через командную строку:
echo "RELOAD" | nc -U /var/run/punto.sock

# Или перезапуском сервиса:
sudo systemctl restart udevmon
```

## Структура проекта

```
punto/
├── cpp/                          # Исходный код C++20
│   ├── CMakeLists.txt            # Конфигурация CMake
│   ├── include/punto/            # Заголовочные файлы
│   │   ├── types.hpp             # Базовые типы
│   │   ├── scancode_map.hpp      # Маппинги клавиш и раскладок
│   │   ├── config.hpp            # Конфигурация
│   │   ├── input_buffer.hpp      # Буфер ввода
│   │   ├── key_injector.hpp      # Генератор input_event
│   │   ├── clipboard_manager.hpp # X11 clipboard
│   │   ├── x11_session.hpp       # Управление X11 сессией
│   │   ├── text_processor.hpp    # Обработка текста
│   │   ├── event_loop.hpp        # Главный цикл
│   │   ├── layout_analyzer.hpp   # Анализатор раскладки (биграммы)
│   │   ├── dictionary.hpp        # Словарный анализатор (hunspell)
│   │   ├── ngram_data.hpp        # Данные частотности биграмм
│   │   └── asm_utils.hpp         # ASM оптимизации
│   └── src/                      # Реализации
├── DEBIAN/                       # Файлы для deb-пакета
├── config.yaml                   # Конфигурация по умолчанию
├── udevmon.yaml                  # Пример конфигурации udevmon
├── build-deb.sh                  # Скрипт сборки пакета
└── README.md
```

## Решение проблем

### Клавиатура не работает после установки

```bash
sudo systemctl stop udevmon
```

### Проверка работы сервиса

```bash
sudo systemctl status udevmon
sudo journalctl -u udevmon -f
```

### Инверсия выделенного текста не работает

Убедитесь, что установлен `xsel`:

```bash
sudo apt install xsel
```

### Автопереключение не срабатывает

1. Проверьте, что `auto_switch.enabled: true` в конфиге
2. Убедитесь, что установлены hunspell словари:

```bash
sudo apt install hunspell-en-us hunspell-ru
```

3. Проверьте логи: `sudo journalctl -u udevmon -f`

### Переключение раскладки не срабатывает

Проверьте, что хоткей в `/etc/punto/config.yaml` соответствует вашей системной комбинации переключения раскладки.

## Удаление

```bash
sudo dpkg -r punto-switcher
sudo rm -rf /etc/punto
```

## Требования

| Компонент          | Версия              |
| ------------------ | ------------------- |
| C++                | 20                  |
| CMake              | ≥ 3.16              |
| GCC                | ≥ 10 или Clang ≥ 11 |
| interception-tools | любая               |
| libX11             | любая               |
| xsel               | любая               |
| hunspell-en-us     | любая (опционально) |
| hunspell-ru        | любая (опционально) |

## История изменений

### v2.2.0 — Управление через системный трей

- **punto-tray**: GTK3 приложение с иконкой в трее
- **IPC сервер**: Unix Domain Socket `/var/run/punto.sock`
- **Hot Reload**: Перезагрузка конфига без перезапуска udevmon
- **Атомарное вкл/выкл**: Быстрое отключение автопереключения
- **Автозапуск**: Desktop entry в `/etc/xdg/autostart/`

### v2.1.0 — Автопереключение раскладки

- **Автоматическое переключение**: Анализ слова при нажатии пробела
- **Гибридный анализ**: Словарь (приоритет) + N-граммы (fallback)
- **Hunspell интеграция**: Загрузка словарей из `/usr/share/hunspell/`
- **Настраиваемые параметры**: threshold, min_word_len, min_score
- **Новые компоненты**: LayoutAnalyzer, Dictionary

### v2.0.0 — C++20 Rewrite

- **Полностью переписан на C++20**
- **Удалены зависимости**: Python, xdotool, xclip
- **Нативный X11**: Прямой доступ к буферу обмена
- **Латентность < 1ms**: Вместо 200-500ms
- **Поддержка Numpad**: KEY_KP0-KEY_KP9, операторы
- **Модульная архитектура**: EventLoop, InputBuffer, KeyInjector, ClipboardManager, TextProcessor

### v1.x (устаревшая)

Предыдущая версия на C с Python скриптами. Больше не поддерживается.

## Лицензия

MIT License

## Автор

Anton Shalin <anton.shalin@gmail.com>
