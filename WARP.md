# WARP.md

This file provides guidance to WARP (warp.dev) when working with code in this repository.

## Ключевые команды (сборка/запуск/отладка)

### Сборка deb-пакета (основной сценарий)

Скрипт собирает `cpp/build/punto`, опционально `cpp/build/punto-tray`, затем упаковывает их в deb и может установить пакет.

```bash
./build-deb.sh
# или без интерактива:
./build-deb.sh --install
```

Важно: `build-deb.sh` может ставить зависимости через `apt-get` и при установке пакета перезапускает `punto-tray` (если GUI доступен).

### Локальная сборка (без упаковки)

Release:
```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build -j"$(nproc)"
```

Debug (с sanitizers, см. `cpp/CMakeLists.txt`):
```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Debug
cmake --build cpp/build -j"$(nproc)"
```

Отключить сборку tray-приложения:
```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TRAY=OFF
cmake --build cpp/build -j"$(nproc)"
```

`compile_commands.json`:
- CMake генерирует `cpp/build/compile_commands.json` (включено в `cpp/CMakeLists.txt` через `CMAKE_EXPORT_COMPILE_COMMANDS`).
- `build-deb.sh` копирует его в корень репозитория.

### «Lint» / статпроверки

Отдельного линтер-конфига в репозитории нет. Базовая проверка качества кода — сборка с предупреждениями (`-Wall -Wextra -Wpedantic`, часть предупреждений повышена до ошибок) и Debug-сборка с sanitizer-ами.

### Тесты

Есть простые unit-тесты через CTest (`cpp/tests/test_main.cpp`):

```bash
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build -j"$(nproc)"
ctest --test-dir cpp/build --output-on-failure
```

Интеграционных тестов пока нет.

Практический smoke-test делается через реальный pipeline interception-tools (требует root) или через установленный сервис `udevmon`.

### Запуск сервиса

После установки пакета:
```bash
punto start
punto status
punto restart
punto stop
```

Ручная диагностика systemd:
```bash
sudo systemctl status udevmon
sudo journalctl -u udevmon -f
```

IPC (низкоуровнево):
```bash
echo "GET_STATUS" | nc -U /var/run/punto.sock
echo "SET_STATUS 0" | nc -U /var/run/punto.sock
echo "RELOAD" | nc -U /var/run/punto.sock
```

## Архитектура (big picture)

Проект — высокопроизводительный «фильтр» для interception-tools:

- `udevmon` поднимает pipeline для каждой клавиатуры.
- `interception -g $DEVNODE` читает события клавиатуры.
- `punto-daemon` (в deb кладётся как `/usr/local/bin/punto-daemon`) читает `input_event` из stdin, принимает решения (авто/ручные хоткеи), может инжектить макросы (backspace/retype), пишет события в stdout.
- `uinput -d $DEVNODE` получает события из stdout и эмулирует ввод.

Кодовая база делится на две бинарные цели:

- Backend: `cpp/src/main.cpp` → `punto::EventLoop` (чтение stdin/poll, обработка ввода, коррекции, IPC).
- Tray UI (опционально): `cpp/src/tray/main.cpp` → `punto::TrayApp` (GTK3 + AppIndicator/Ayatana), управление backend через Unix-socket.

## Backend: где «начинается жизнь»

Точка входа:
- `cpp/src/main.cpp` — парсит `--help/--version`, загружает конфиг, запускает `EventLoop`.

Главный цикл и диспетчеризация событий:
- `cpp/include/punto/event_loop.hpp`, `cpp/src/event_loop.cpp`.
- Основной цикл делает `poll()` по stdin с тиком 1ms и параллельно «дожимает» готовые результаты асинхронного анализа, даже когда пользователь перестал печатать.

Ключевые внутренние подсистемы EventLoop:
- `InputBuffer` (`cpp/include/punto/input_buffer.hpp`) — хранит текущее/последнее слово + trailing whitespace.
- `HistoryManager` (`cpp/include/punto/history_manager.hpp`) — хранит поток введённых `KeyEntry` в окне последних N слов и даёт координаты для rollback/replay.
- `KeyInjector` (`cpp/include/punto/key_injector.hpp`) — пишет `input_event` в stdout; умеет макросы (backspace/retype/layout hotkey). В EventLoop ему подставляется `wait_func`, чтобы во время задержек буферизовать входящие события (Input Guard).
- `X11Session` (`cpp/include/punto/x11_session.hpp`) — решает проблему работы из root-контекста: находит активную user-сессию (DISPLAY/XAUTHORITY/$HOME/$XDG_RUNTIME_DIR), умеет refresh при login/logout и «не прилипает» к greeter.
- IPC: `IpcServer` (`cpp/include/punto/ipc_server.hpp`, `cpp/src/ipc_server.cpp`) — отдельный поток, принимает команды через Unix Domain Socket.

## Автопереключение: асинхронный pipeline и rollback/replay

Асинхронный анализ:
- `AnalysisWorkerPool` (`cpp/include/punto/analysis_worker_pool.hpp`) — пул `std::jthread`, принимает `WordTask`, отдаёт `WordResult`.
- На границе слова (SPACE/TAB) EventLoop:
  - сначала пропускает разделитель дальше (чтобы ввод не тормозил),
  - создаёт задачу анализа слова (с `task_id`),
  - применяет результаты строго по порядку `task_id`.

Применение коррекций:
- EventLoop хранит метаданные слова (позиции в истории) и при необходимости делает rollback/replay через `KeyInjector`.
- Для переключения раскладки используется:
  - быстрый путь: XKB LockGroup (`X11Session::set_keyboard_layout()`),
  - fallback: эмуляция системного хоткея раскладки из конфига.
- Есть «инвариант длины»: число удалённых токенов должно совпасть с числом вставленных (слово + хвост); при нарушении коррекция пропускается.

Защита от нежелательных/ложных коррекций:
- `Smart Bypass` (`cpp/include/punto/smart_bypass.hpp`) — отключает только регистровые исправления для URL/путей/camelCase/snake_case/аббревиатур; переключение раскладки при необходимости всё равно разрешено.
- `UndoDetector` (`cpp/include/punto/undo_detector.hpp`) — если пользователь быстро отменяет коррекцию (Backspace/Undo), слово попадает в исключения (по умолчанию `/etc/punto/undo_exclusions.txt`).

## Анализ языка/слова (dict → typo → n-grams)

Воркерный анализ (см. `AnalysisWorkerPool`) устроен примерно так:
- Фильтр «цифры/спецсимволы»: `LayoutAnalyzer::has_invalid_chars()` — слова с цифрами не должны инициировать переключение.
- Словарь: `Dictionary` (`cpp/include/punto/dictionary.hpp`)
  - при наличии Hunspell (define `HAVE_HUNSPELL`) используется spell/suggest;
  - есть резервная hash/bloom-логика.
- Исправление опечаток/залипшего Shift: `typo_corrector` (`cpp/include/punto/typo_corrector.hpp`).
- N-gram анализ: `LayoutAnalyzer` (`cpp/include/punto/layout_analyzer.hpp`) + данные в `cpp/include/punto/ngram_data.hpp`.

## Конфигурация и hot reload

Где лежит конфиг:
- системный: `/etc/punto/config.yaml` (см. `config.yaml` в корне репо как шаблон),
- пользовательский: `~/.config/punto/config.yaml` (используется в приоритете, когда backend смог определить HOME активного пользователя).

Загрузка и валидация:
- `cpp/include/punto/config.hpp`, `cpp/src/config.cpp`.
- `load_config_checked(path)` — строгая загрузка (без «тихих» фолбэков) и возвращает ошибку.
- `load_config()` — best-effort (используется при старте до уточнения окружения).

Hot reload:
- IPC команда `RELOAD [path]` вызывает `EventLoop::reload_config()`.
- Reload публикует новые снапшоты `Config/LayoutAnalyzer/KeyInjector` через `std::atomic_store(shared_ptr<...>)`, чтобы не блокировать main loop.
- При RELOAD runtime-флаг включения авто-переключения синхронизируется с `auto_switch.enabled` в конфиге (конфиг — «source of truth»).

Если добавляете новые настройки:
- обновите шаблон `config.yaml`,
- добавьте парсинг в `cpp/src/config.cpp`,
- при необходимости расширьте UI в `cpp/src/tray/settings_dialog.cpp` (и сохранение),
- убедитесь, что изменение реально применяется через reload (snapshot-обновления в `EventLoop::reload_config()`).

## Tray-приложение (GTK) и IPC

Tray:
- `cpp/src/tray/tray_app.cpp` — меню (toggle авто/звук, настройки, about), периодический опрос статуса.
- `cpp/src/tray/settings_dialog.cpp` — редактирует user config и вызывает `IpcClient::reload_config()`.

IPC особенности:
- Основной сокет: `/var/run/punto.sock`.
- Если основной сокет занят (несколько инстансов backend), `IpcServer` поднимет `/var/run/punto-<pid>.sock`.
- `IpcClient` сканирует `/var/run` и шлёт команды во все найденные `punto-*.sock`.

IPC протокол (см. `cpp/src/ipc_server.cpp`):
- `GET_STATUS` → `OK ENABLED|DISABLED`
- `SET_STATUS 0|1` → `OK ENABLED|DISABLED`
- `RELOAD [path]` → `OK ...` / `ERROR ...`
- `SHUTDOWN` запрещён (возвращает ошибку)
