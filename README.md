# Punto Switcher для Linux

Высокопроизводительная реализация Punto Switcher на C++20 для Linux.
Позволяет исправлять текст, набранный в неправильной раскладке клавиатуры.

![Version](https://img.shields.io/badge/version-2.7.4-blue)
![C++](https://img.shields.io/badge/C%2B%2B-20-orange)
![License](https://img.shields.io/badge/license-Personal%20Use%20Only-red)

## Возможности

### Управление через трей (v2.4+)

Иконка `punto-tray` в системном трее позволяет:
- **Визуальный статус** — видно, включено ли автопереключение
- **Автопереключение (toggle)** — быстро вкл/выкл без перезапуска `udevmon`
- **Звук (toggle)** — вкл/выкл звуковой индикации (пишется в `~/.config/punto/config.yaml`, применяется через RELOAD)
- **Настройки...** — диалог (GTK3): автопереключение, звук, задержки, хоткей раскладки (`~/.config/punto/config.yaml`)
  - **max_rollback_words** — максимальная глубина отката (сколько последних слов можно безопасно перепечатать при позднем срабатывании анализа)
  - **Синхронизация хоткея с системой**: GNOME (через gsettings) и Generic X11 (через setxkbmap / XKB grp:*_toggle)
  - Вкладка «Горячие клавиши» показывает, какие комбинации применимы для GNOME и для X11
- **О программе** — окно со справкой/версией
- **Автозапуск** — desktop entry в `/etc/xdg/autostart/` (если `punto-tray` включён в пакет)

### Автоматическое переключение (v2.1+, async в v2.5)

| Режим                       | Действие                                                  |
| --------------------------- | --------------------------------------------------------- |
| **АВТО** (при пробеле/табе) | Анализирует слово и переключает раскладку, если это нужно |

#### v2.5: асинхронный pipeline (быстрый ввод без блокировок)

- **Анализ в фоне**: ввод не блокируется — слово уходит в пул воркеров, результаты применяются позже.
- **Строгий порядок**: решения применяются строго по `task_id` (без перемешивания слов при быстром наборе).
- **Rollback/Replay**: корректировка может откатывать и перепечатывать до `auto_switch.max_rollback_words` последних слов.
- **Инвариант длины**: число удалённых токенов (KeyEntry) должно совпасть с числом вставленных (слово + хвост). При нарушении — коррекция пропускается.
- **Input Guard**: во время макроса события ввода буферизуются; release-события могут форвардиться раньше, чтобы не "пропадали" пробелы/буквы при очень быстром нажатии.

Гибридный анализ (v2.6+):
- **Словари** (приоритет) — hunspell (en_US, ru_RU), wamerican-huge, scowl и др.
- **N-граммы** (fallback только EN→RU) — частотный анализ биграмм + триграмм
- **Защита от ложных срабатываний** — RU→EN только по словарю

#### v2.7: исправление залипшего Shift + typo fix + CLI

- **Sticky Shift Fix**: автоматическое исправление ошибок регистра:
  - `ПРивет` → `Привет` (паттерн UU+L+: несколько заглавных в начале)
  - `кОЛБАСА` → `Колбаса` (паттерн L+U+: Caps Lock)
  - `GHbdtn` → `Привет` (комбинированное: смена раскладки + регистр)
  - **Смешанный регистр НЕ исправляется** (например, `СНиП`)
- **Typo Fix**: автоматическое исправление опечаток:
  - `ппривет` → `привет` (удаление дублей)
  - Использует Hunspell spell() для проверки правильности слова
  - Защита от ложных срабатываний: правильные слова не изменяются
- **CLI wrapper**: удобное управление сервисом:
  - `punto start` — запуск сервиса (backend + frontend)
  - `punto stop` — остановка сервиса
  - `punto restart` — перезапуск с перезагрузкой конфига
  - `punto status` — показать статус
- **Новые настройки**:
  - `sticky_shift_correction_enabled` — вкл/выкл исправление регистра
  - `typo_correction_enabled` — вкл/выкл исправление опечаток
  - `max_typo_diff` — максимальное расстояние редактирования (1-2)

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
│  ┌─────────────┐ ┌───────────────┐ ┌─────────────────┐       │
│  │ KeyInjector │ │ HistoryManager │ │ AnalysisWorker  │       │
│  └─────────────┘ └───────────────┘ │ Pool (async)     │       │
│  ┌─────────────┐ ┌─────────────┐ └─────────────────┘       │
│  │TextProcessor│ │   X11Session │  ┌──────────────────┐      │
│  └─────────────┘ └─────────────┘  │ Sequencer/Replay │      │
│  ┌───────────────┐ ┌────────────┐ │ + Telemetry      │      │
│  │LayoutAnalyzer │ │ Dictionary │ └──────────────────┘      │
│  └───────────────┘ └────────────┘ ┌──────────────────┐      │
│  ┌───────────────┐ ┌────────────┐ │   IpcServer       │      │
│  │  punto-tray   │ │            │ └──────────────────┘      │
│  │  (GTK3)       │ │            │                           │
│  └───────────────┘ └────────────┘                           │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  uinput -d $DEVNODE                                         │
│              (эмулирует нажатия клавиш)                     │
└─────────────────────────────────────────────────────────────┘
```

### Ключевые особенности v2.5

- **Асинхронное автопереключение** — анализ слова в фоне (worker pool), ввод не блокируется
- **Строгая упорядоченность** — применение коррекций строго по порядку слов (`task_id`)
- **Rollback/Replay** — исправление может перепечатать до `auto_switch.max_rollback_words` последних слов
- **Защита от "пропавших" пробелов/букв** — Input Guard буферизует ввод во время макросов и умеет ранний форвард release-событий
- **Телеметрия** — логирует `queue_us`, `analysis_us`, `macro_us`, длину хвоста (удобно смотреть в `journalctl -u udevmon -f`)
- **KeyInjector оптимизирован** — меньше overhead на вывод событий (пакетная запись EV_KEY+SYN одним write)

Также доступны возможности v2.4:
- **Управление через трей** — `punto-tray` (GTK3 + AppIndicator/Ayatana), toggle-пункты, "О программе"
- **Синхронизация хоткея раскладки с системой** — GNOME (gsettings) и Generic X11 (setxkbmap / XKB grp:*_toggle)
- **Настройки + hot reload** — редактирование `~/.config/punto/config.yaml` и мгновенное применение через IPC (без перезапуска `udevmon`)
- **IPC через Unix Socket** — `/var/run/punto.sock` (GET_STATUS, SET_STATUS, RELOAD)
- **Автопереключение раскладки** — гибрид: hunspell (если есть) → N-граммы (биграммы+триграммы)
- **Звуковая индикация** — `paplay`/`aplay` (если доступны)
- **Нативный X11** — чтение selection напрямую; для записи используется `xsel`

## Установка

### Способ 1: Сборка и установка deb-пакета (рекомендуется)

`build-deb.sh`:
- проверит/предложит установить зависимости;
- соберёт `punto` и `punto-tray` (если доступны GTK3/AppIndicator dev-пакеты);
- соберёт deb-пакет и установит его.

```bash
git clone https://github.com/antonshalin76/punto.git
cd punto
./build-deb.sh --install
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
sudo apt install hunspell hunspell-en-us hunspell-ru wamerican-huge

# Опционально: звук при переключении раскладки (paplay/aplay)
sudo apt install pulseaudio-utils alsa-utils
```

#### Сборка

```bash
git clone https://github.com/antonshalin76/punto.git
cd punto
./build-deb.sh
sudo dpkg -i punto-switcher_2.7.4_amd64.deb
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
- JOB: "interception -g $DEVNODE | /usr/local/bin/punto-daemon | uinput -d $DEVNODE"
  DEVICE:
    EVENTS:
      EV_KEY: [KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE, KEY_TAB, KEY_ENTER, KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTALT, KEY_RIGHTALT, KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_BACKSLASH, KEY_GRAVE, KEY_SPACE, KEY_PAUSE, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN, KEY_INSERT, KEY_DELETE]
```

### Запуск

#### С помощью CLI (рекомендуется)

```bash
punto start     # Запуск сервиса (backend + frontend)
punto status    # Проверка статуса
punto restart   # Перезапуск (после изменения конфига)
punto stop      # Остановка
```

#### Вручную через systemd

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
  max_rollback_words: 5 # Глубина отката (сколько последних слов можно перепечатать)

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
│   │   ├── history_manager.hpp   # История токенов для rollback/replay (async)
│   │   ├── concurrent_queue.hpp  # Потокобезопасная очередь (worker pool)
│   │   ├── analysis_worker_pool.hpp # Пул воркеров анализа (async)
│   │   ├── typo_corrector.hpp    # Алгоритмы исправления опечаток
│   │   ├── ngram_data.hpp        # Данные частотности N-грамм
│   │   └── asm_utils.hpp         # ASM/AVX2 оптимизации
│   └── src/
│       ├── tray/                 # Исходники tray-приложения
│       └── sound/                # WAV файлы
├── DEBIAN/                       # Файлы для deb-пакета
│   ├── control
│   ├── postinst
│   └── prerm
├── config.yaml                   # Конфигурация по умолчанию
├── udevmon.yaml                  # Пример конфигурации udevmon
├── punto-cli.sh                  # CLI wrapper для управления сервисом
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

| Компонент                  | Версия                    |
| -------------------------- | ------------------------- |
| C++                        | 20                        |
| CMake                      | ≥ 3.16                    |
| GCC                        | ≥ 10 или Clang ≥ 11       |
| interception-tools         | любая                     |
| libX11                     | любая                     |
| xsel                       | любая                     |
| libgtk-3-0                 | любая (tray, опционально) |
| libayatana-appindicator3-1 | любая (tray, опционально) |
| pulseaudio-utils           | любая (звук, опционально) |
| alsa-utils                 | любая (звук, опционально) |
| libhunspell                | любая (рекомендуется)     |
| hunspell-en-us             | любая (опционально)       |
| hunspell-ru                | любая (опционально)       |
| wamerican-huge             | любая (опционально)       |

## История изменений

### v2.7.4 — Исправление привязки к login screen (GDM)

- **Сервис больше не "прилипает" к greeter-сессии** (gdm/lightdm) после boot.
- **Автоматическое переподключение к активной user-сессии** после логина/логаута.
- **Пересоздание X11/Audio-зависимых компонентов** (clipboard, звук) при смене сессии.

### v2.7.3 — Персистентный Undo Detector

- **Персистентные исключения Undo**: теперь сохраняются между сессиями
  - Файл хранения: `/etc/punto/undo_exclusions.txt`
  - Автозагрузка при старте сервиса
  - Инкрементальное сохранение при добавлении
  - Система самообучается по мере использования

### v2.7.2 — Улучшение точности

- **Smart Bypass**: пропуск регистровых исправлений для технических слов
  - URL (`https://`, `www.`, `@`)
  - Пути (`/home/user`, `.config`)  
  - camelCase, PascalCase, snake_case
  - **Важно**: layout switch по-прежнему работает!
- **Исключения аббревиатур**: СНиП, ДНК, API не исправляются
  - Эвристика: короткие слова (2-5 символов) с ≤1 гласной
- **Детектор Undo**: сессионные исключения
  - 3+ Backspace после коррекции → слово добавляется в исключения
  - Исключения действуют до перезапуска сервиса
- **Контекстное окно**: инфраструктура для учёта языка предыдущих слов
- **Расширение N-грамм**: 128 → 256 записей для EN и RU
- **IT-словарь**: +150 технических терминов (docker, kubernetes, python, react...)

### v2.7.1 — Исправление CPU spin

- **Исправлен CPU spin при остановке сервиса**:
  - Добавлена обработка флагов `POLLHUP`, `POLLERR`, `POLLNVAL` в главном цикле
  - Ранее при закрытии stdin (остановка udevmon) процесс входил в busy loop и грузил CPU ~2 мин
  - Теперь процесс корректно завершается при `punto stop`, `restart` и обновлении пакета

### v2.7.0 — Typo Fix + Sticky Shift + CLI

- **Typo Fix**: автоматическое исправление опечаток:
  - `ппривет` → `привет` (удаление дублей букв)
  - Расстояние Дамерау-Левенштейна (перестановки, вставки, удаления, замены)
  - Интеграция с Hunspell spell() для проверки правильности
  - **Защита от ложных срабатываний**: правильные слова не изменяются
- **Sticky Shift Fix**: автоматическое исправление ошибок регистра:
  - `ПРивет` → `Привет` (паттерн UU+L+)
  - `кОЛБАСА` → `Колбаса` (паттерн L+U+)
  - `GHbdtn` → `Привет` (комбинированное: смена раскладки + регистр)
  - Смешанный регистр (`СНиП`) НЕ исправляется
- **CLI wrapper `punto`** для удобного управления:
  - `punto start/stop/restart/status`
  - Запускает backend (udevmon) + frontend (punto-tray)
- **Новые настройки конфигурации**:
  - `sticky_shift_correction_enabled` — вкл/выкл исправление регистра
  - `typo_correction_enabled` — вкл/выкл исправление опечаток
  - `max_typo_diff` — максимальное расстояние редактирования (1-2)
- **Автоматическое обновление конфига**: при обновлении пакета старый конфиг сохраняется в backup
- **Рефакторинг**:
  - Новый модуль `typo_corrector.hpp/cpp`
  - `CorrectionType` enum для телеметрии
  - Расширен `WordResult` полем `correction`
  - Бинарник переименован в `punto-daemon`, CLI wrapper — `punto`

### v2.6.0 — libhunspell + полная поддержка словоформ

- **Интеграция libhunspell**: полная поддержка словоформ с учётом:
  - Падежей, склонений, времён, родов и чисел
  - Аффиксов из .aff файлов (все словоформы без отдельной загрузки)
  - Двусторонняя проверка: конвертация слова в обе раскладки
- **Расширенные словари** (fallback если hunspell недоступен):
  - hunspell (en_US.dic, en_GB.dic, ru_RU.dic)
  - wamerican-huge (~300k английских слов)
  - /usr/share/dict/* (american-english, words, russian)
- **Улучшенная логика автопереключения**:
  - Приоритет 1: Hunspell spell() с полной поддержкой словоформ
  - Приоритет 2: Hash-based проверка в загруженных словарях
  - Приоритет 3: N-граммы + анализ невалидных биграмм
  - RU→EN: требуется отсутствие невалидных EN биграмм
  - EN→RU: срабатывает при невалидных EN или перевесе ru_score
- **Примеры работающих конверсий**:
  - `lheubt` → `другие`, `ntreotq` → `текущей`, `,erd` → `букв`
  - `туеещ` → `netto` (если в словаре)

### v2.5.1 — Рефакторинг и мелкие улучшения

- Удалён мёртвый код (неиспользуемые методы/поля/константы).
- Окно «О программе»: email кликабелен и доступен для копирования.

### v2.5.0 — Асинхронное автопереключение + повышение надёжности

- **Async pipeline**: пул воркеров для анализа + строгий sequencer применения результатов
- **Rollback/Replay**: параметр `auto_switch.max_rollback_words` (дефолт 5)
- **Input Guard**: защита от потери пробелов/букв при очень быстрых нажатиях (ранний форвард release-событий)
- **Телеметрия**: `queue_us`, `analysis_us`, `macro_us`, tail_len
- **Оптимизация вывода событий**: меньше syscall/flush overhead в `KeyInjector`
- **Tray UI**: настройка `max_rollback_words` в диалоге

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
