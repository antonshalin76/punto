# Punto Switcher для Linux

Высокопроизводительная реализация Punto Switcher на C++20 для Linux.
Позволяет исправлять текст, набранный в неправильной раскладке клавиатуры.

![Version](https://img.shields.io/badge/version-2.4.0-blue)
![C++](https://img.shields.io/badge/C%2B%2B-20-orange)
![License](https://img.shields.io/badge/license-Personal%20Use%20Only-red)

## Возможности

### Управление через трей (v2.4)

Иконка `punto-tray` в системном трее позволяет:
- **Визуальный статус** — видно, включено ли автопереключение
- **Автопереключение (toggle)** — быстро вкл/выкл без перезапуска `udevmon`
- **Звук (toggle)** — вкл/выкл звуковой индикации (пишется в `~/.config/punto/config.yaml`, применяется через RELOAD)
- **Настройки...** — диалог (GTK3): автопереключение, звук, задержки, хоткей раскладки (`~/.config/punto/config.yaml`)
  - **Синхронизация хоткея с системой**: GNOME (через gsettings) и Generic X11 (через setxkbmap / XKB grp:*_toggle)
  - Вкладка «Горячие клавиши» показывает, какие комбинации применимы для GNOME и для X11
- **О программе** — окно со справкой/версией
- **Автозапуск** — desktop entry в `/etc/xdg/autostart/` (если `punto-tray` включён в пакет)

### Автоматическое переключение (v2.1+)

| Режим                      | Действие                                                         |
| -------------------------- | ---------------------------------------------------------------- |
| **АВТО** (при пробеле/табе) | Анализирует слово и переключает раскладку, если это нужно         |

Гибридный анализ:
- **Словарь** (приоритет) — hunspell словари EN/RU (если установлены)
- **N-граммы** (fallback) — частотный анализ биграмм + триграмм

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
│  interception -g $DEVNODE                                   |
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

### Ключевые особенности v2.4

- **Управление через трей** — `punto-tray` (GTK3 + AppIndicator/Ayatana), toggle-пункты, "О программе"
- **Синхронизация хоткея раскладки с системой** — GNOME (gsettings) и Generic X11 (setxkbmap / XKB grp:*_toggle)
- **Настройки + hot reload** — редактирование `~/.config/punto/config.yaml` и мгновенное применение через IPC (без перезапуска `udevmon`)
- **IPC через Unix Socket** — `/var/run/punto.sock` (GET_STATUS, SET_STATUS, RELOAD)
- **Автопереключение раскладки** — гибрид: hunspell (если есть) → N-граммы (биграммы+триграммы)
- **Звуковая индикация** — `paplay`/`aplay` (если доступны)
- **Нативный X11** — чтение selection напрямую; для записи используется `xsel`
- **< 1ms латентность** — вместо 200-500ms при вызове скриптовых реализаций

## Установка

### Способ 1: Установка deb-пакета (рекомендуется)

```bash
git clone https://github.com/antonshalin76/punto.git
cd punto
sudo dpkg -i punto-switcher_2.4.0_amd64.deb
```

### Способ 2: Сборка из исходников

#### Зависимости

> Примечание: `build-deb.sh` рассчитан на Debian/Ubuntu и сам предложит установить зависимости.

```bash
# Ubuntu/Debian (минимум для сборки punto)
sudo apt install build-essential cmake pkg-config libx11-dev interception-tools xsel

# Опционально: сборка tray-приложения (GTK3 + AppIndicator/Ayatana)
sudo apt install libgtk-3-dev libayatana-appindicator3-dev

# Опционально: словари для более точного авто-определения языка
sudo apt install hunspell-en-us hunspell-ru

# Опционально: звук при переключении раскладки (paplay/aplay)
sudo apt install pulseaudio-utils alsa-utils
```

#### Сборка

```bash
git clone https://github.com/antonshalin76/punto.git
cd punto
./build-deb.sh
sudo dpkg -i punto-switcher_2.4.0_amd64.deb
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

Создайте `/etc/interception/udevmon.yaml` (пример — `udevmon.yaml` в корне репозитория):

```yaml
- JOB: "interception -g $DEVNODE | /usr/local/bin/punto | uinput -d $DEVNODE"
  DEVICE:
    EVENTS:
      EV_KEY: [KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE, KEY_TAB, KEY_ENTER, KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTALT, KEY_RIGHTALT, KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_BACKSLASH, KEY_GRAVE, KEY_SPACE, KEY_PAUSE, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN, KEY_INSERT, KEY_DELETE]
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
  modifier: leftctrl   # leftctrl, rightctrl, leftalt, rightalt, leftshift, rightshift, leftmeta, rightmeta
  key: grave           # grave (` ~), space, tab, backslash, capslock, а также left/right: shift/ctrl/alt/meta

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

# Звуковая индикация переключения раскладки
sound:
  enabled: true
```

### Синхронизация хоткея с системой

- **GNOME**: применяется автоматически через `gsettings`.
- **Generic X11**: применяется через `setxkbmap` (XKB `grp:*_toggle`). Поддерживаются только:
  - Alt+Shift
  - Ctrl+Shift
  - Ctrl+Alt
  - Alt+Space
  - Ctrl+Space
  - Win+Space
  - Shift+CapsLock
- **KDE/Plasma**: автоматическая синхронизация пока не поддерживается (настройте хоткей в системе вручную).

После изменения можно применить настройки без перезапуска:

```bash
# Через tray-приложение: диалог настроек -> "Сохранить" (применяется сразу)

# Или через командную строку:

echo "RELOAD" | nc -U /var/run/punto.sock

# Быстро вкл/выкл автопереключение (не меняя конфиг):
echo "SET_STATUS 1" | nc -U /var/run/punto.sock
# echo "SET_STATUS 0" | nc -U /var/run/punto.sock

# Проверить текущий статус:
echo "GET_STATUS" | nc -U /var/run/punto.sock

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
│   │   ├── clipboard_manager.hpp # X11 clipboard (частично, запись через xsel)
│   │   ├── x11_session.hpp       # Управление X11 сессией
│   │   ├── sound_manager.hpp     # Звуковая индикация (paplay/aplay)
│   │   ├── text_processor.hpp    # Обработка текста
│   │   ├── event_loop.hpp        # Главный цикл
│   │   ├── layout_analyzer.hpp   # Анализатор раскладки (биграммы+триграммы)
│   │   ├── dictionary.hpp        # Словарный анализатор (hunspell)
│   │   ├── ipc_server.hpp        # IPC сервер (/var/run/punto.sock)
│   │   ├── ipc_client.hpp        # IPC клиент (для tray)
│   │   ├── tray_app.hpp          # Tray UI
│   │   ├── settings_dialog.hpp   # Диалог настроек (GTK)
│   │   ├── ngram_data.hpp        # Данные частотности N-грамм
│   │   └── asm_utils.hpp         # ASM оптимизации
│   └── src/
│       ├── tray/                 # Исходники tray-приложения
│       └── sound/                # WAV файлы
├── DEBIAN/                       # Файлы для deb-пакета
│   ├── control
│   ├── postinst
│   └── prerm
├── config.yaml                   # Конфигурация по умолчанию
├── udevmon.yaml                  # Пример конфигурации udevmon
├── punto-tray.desktop            # Autostart entry для tray
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

1. Проверьте, что хоткей в `~/.config/punto/config.yaml` (или `/etc/punto/config.yaml`) соответствует вашей системной комбинации.
2. В `punto-tray` откройте "Настройки..." → вкладка "Горячие клавиши" и убедитесь, что выбранная комбинация "применима" для вашего backend (GNOME/X11).
3. Если у вас KDE/Plasma или Wayland-ограничения — настройте хоткей в системе вручную и выставьте такое же значение в конфиге.

## Удаление

```bash
sudo dpkg -r punto-switcher
sudo rm -rf /etc/punto
```

## Требования

| Компонент          | Версия              |
| ------------------ | ------------------- |
| C++                          | 20                  |
| CMake                        | ≥ 3.16              |
| GCC                          | ≥ 10 или Clang ≥ 11 |
| interception-tools           | любая               |
| libX11                       | любая               |
| xsel                         | любая               |
| libgtk-3-0                   | любая (tray, опционально) |
| libayatana-appindicator3-1   | любая (tray, опционально) |
| pulseaudio-utils             | любая (звук, опционально) |
| alsa-utils                   | любая (звук, опционально) |
| hunspell-en-us               | любая (опционально) |
| hunspell-ru                  | любая (опционально) |

## История изменений

### v2.4.0 — Синхронизация системного хоткея + модернизация UI

- **Tray menu**: toggle-пункты вместо "вкл/выкл", убран пункт "Сервис", добавлено "О программе"
- **Настройки**: переключатели в виде button toggle, вкладка хоткеев показывает применимость комбинаций для GNOME/X11 и пытается применить хоткей в систему
- **Лицензия**: смена на Personal Use Only (см. `LICENSE`)

### v2.3.0 — Улучшения tray UI + звук

- **Меню tray**: переключатель звука, пункт перезапуска `udevmon` (через `pkexec`)
- **Диалог настроек**: вкладки для авто-переключения, звука, задержек и хоткея
- **SoundManager**: звуковая индикация при переключении раскладки (`paplay` → `aplay`)

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

Personal Use Only — см. файл `LICENSE`.

## Автор

Anton Shalin <anton.shalin@gmail.com>
