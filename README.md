# Punto Switcher для Linux

Высокопроизводительная реализация Punto Switcher на C++20 для Linux.

> **Безопасный режим v2.8.7.** Анализ, passthrough, CLI, tray и runtime-
> диагностика работают штатно, но автоматические и ручные изменения текста
> временно пропускаются до отправки клавиш. Универсальный X11 clipboard не даёт
> причинно связанного подтверждения, что именно подготовленный paste уже применён
> редактором; восстановление старого clipboard при задержке могло повредить текст.

![Version](https://img.shields.io/badge/version-2.8.7-blue)
![C++](https://img.shields.io/badge/C%2B%2B-20-orange)
![License](https://img.shields.io/badge/license-Personal%20Use%20Only-red)

## Возможности

### Управление через трей (v2.4+)

Иконка `punto-tray` в системном трее позволяет:
- **Визуальный статус** — показывает, доступно ли изменение текста; в v2.8.7
  всегда `disabled`
- **Безопасный режим** — пункт изменения текста явно недоступен, поэтому tray
  не может показать ложный статус `enabled`
- **Звук исправлений** — явно недоступен, пока изменение текста отключено;
  настройка сохраняется в YAML только для совместимости
- **Настройки...** — диалог (GTK3): параметры анализа и совместимая настройка
  звука (`~/.config/punto/config.yaml`)
  - **max_rollback_words** сохраняется для совместимости конфига, но откат и
    повторный ввод в v2.8.7 не выполняются
- **О программе** — окно со справкой/версией
- **Автозапуск** — desktop entry в `/etc/xdg/autostart/` идемпотентно
  активирует package-owned `systemd --user` unit; PID-файлы и raw PID signals
  не используются

### Автоматическое переключение (v2.1+, async в v2.5)

| Режим                       | Действие                                                  |
| --------------------------- | --------------------------------------------------------- |
| **АВТО** (при пробеле/табе) | Анализирует слово; в v2.8.7 не изменяет текст или раскладку |

#### Асинхронный pipeline анализа

- **Анализ в фоне**: слово может уходить в пул воркеров после passthrough; путь
  ввода не ждёт X11 или классификатор.
- **Строгий порядок телеметрии**: результаты учитываются по `task_id`, но в
  v2.8.7 ни один результат не применяется к документу.
- **Fail-before-dispatch**: rollback/replay, clipboard и эмуляция клавиш
  недостижимы из product entry points.

Гибридный анализ (v2.6+):
- **Словари** (приоритет): если слово есть только в одной раскладке — решение однозначно
- **N-граммы**: используются только в случае, когда слово есть в обоих словарях (ambiguous)
- **Если слова нет в словарях** — анализ не предлагает смену раскладки.

#### Классификаторы v2.7 и CLI

Перечисленные ниже преобразования описывают классификацию и историческое
поведение. В v2.8.7 результаты не изменяют документ.

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

| Комбинация           | Действие                                                                              |
| -------------------- | ------------------------------------------------------------------------------------- |
| **Pause**            | Зарезервировано; в v2.8.7 событие поглощается без изменения текста                    |
| **Shift+Pause**      | Зарезервировано; в v2.8.7 событие поглощается без изменения selection/clipboard       |
| **Ctrl+Pause**       | Зарезервировано; в v2.8.7 событие поглощается без изменения текста                    |
| **Alt+Pause**        | Зарезервировано; в v2.8.7 событие поглощается без изменения selection/clipboard       |
| **LCtrl+LAlt+Pause** | Зарезервировано; в v2.8.7 событие поглощается без изменения selection/clipboard       |
| **Ctrl+Z**           | Обычный undo приложения; Punto не создаёт новые correction-record в безопасном режиме |

### Примеры поведения до v2.8.6

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
│  │ EventLoop   │ │ InputBuffer │ │ AnalysisWorker    │       │
│  └─────────────┘ └─────────────┘ │ Pool (async)      │       │
│  ┌───────────────┐ ┌───────────────┐ ┌────────────────┐      │
│  │ RuntimeHealth │ │   X11Session  │ │ Sequencer      │      │
│  └───────────────┘ └───────────────┘ │ + Telemetry    │      │
│  ┌───────────────┐ ┌───────────────┐ └────────────────┘      │
│  │LayoutAnalyzer │ │  Dictionary   │ ┌────────────────┐      │
│  └───────────────┘ └───────────────┘ │   IpcServer    │      │
│  ┌───────────────┐ ┌───────────────┐ └────────────────┘      │
│  │TextProcessor  │ │ Config loader │                         │
│  └───────────────┘ └───────────────┘                         │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  uinput -d $DEVNODE                                         │
│              (эмулирует нажатия клавиш)                     │
└─────────────────────────────────────────────────────────────┘
```

### Текущий runtime v2.8.7

- **Асинхронный анализ** — анализ слова в фоне (worker pool), ввод не блокируется
- **Строгая упорядоченность** — результаты анализа учитываются по порядку слов (`task_id`)
- **Нулевая мутация** — автоматические и ручные ветки завершаются до X11,
  clipboard, смены раскладки или эмуляции исправляющих клавиш
- **Телеметрия** — логирует `queue_us`, `analysis_us`, `macro_us`, длину хвоста (удобно смотреть в `journalctl -u udevmon -f`)
- **Auto-budget worker pool** — при нескольких `punto-daemon` суммарный analysis pool автоматически делится между процессами и не раздувается линейно от числа клавиатур
- **Primary control-plane** — только один `punto-daemon` держит `/var/run/punto.sock`; secondary daemons синхронизируют `RELOAD`/`SET_STATUS` через shared state в `/var/run/punto-control.state`

Также доступны возможности v2.4:
- **Управление через трей** — `punto-tray` (GTK3 + AppIndicator/Ayatana),
  честный disabled-статус изменения текста и параметры анализа
- **Layout snapshot** — читается только подтверждённым X11 probe; конфигурационный
  `hotkey` в v2.8.7 оставлен исключительно для совместимости схемы и игнорируется
- **Настройки + bounded reload** — чтение пользовательского конфига выполняется
  вне input-thread; IPC принимает задачу без остановки passthrough
- **IPC через Unix Socket** — `/var/run/punto.sock` (GET_STATUS, SET_STATUS, RELOAD, STATS)
- **Анализ раскладки** — словарь-first; N-граммы используются только для
  ambiguous слов (есть в обоих словарях). Применение исправлений в v2.8.7
  отключено описанной выше безопасной границей.
- **Звуковая настройка** сохраняется для совместимости, но в безопасном режиме
  v2.8.7 звук исправлений не воспроизводится
- **Изолированный X11 clipboard backend** — отдельный проверяемый компонент и
  contract-test target; product runtime v2.8.7 его не вызывает

## Production Checklist

- IPC socket `/var/run/punto.sock` рассчитан на режим `root:punto` с правами `0660`.
- Shared control-plane state `/var/run/punto-control.state` и lease `/var/run/punto-control.lock` также создаются с `root:punto` и служат для failover primary daemon.
- После захвата lease новый primary сначала применяет последний безопасный
  state snapshot. Если прежний user-path больше не разрешён текущей desktop-
  сессией, выполняется reload стандартного config этой сессии; если явно
  сохранённый path исчез, допускается только успешно загруженный fallback.
  Пока ни один разрешённый config не применён, процесс остаётся secondary,
  не создаёт primary IPC и не повышает generation. Успешный failover всегда
  публикует строго следующее поколение.
- Пользователи tray и локальных IPC-клиентов должны состоять в группе `punto`.
- Tray поддерживается в графической сессии с `systemd --user` версии 249.10 или
  новее. Desktop entry и CLI управляют только статическим package-owned unit
  `punto-tray.service`; отсутствие user manager выдаёт `WARN tray-unavailable`
  и не меняет состояние уже здорового backend. User manager должен получить
  `DISPLAY`/`WAYLAND_DISPLAY` и `XAUTHORITY` от desktop session; это штатно для
  GNOME, а в иных окружениях переменные нужно импортировать в user manager до
  запуска unit. Несколько одновременных GUI-сессий одного UID разделяют один
  tray unit и не являются поддержанной конфигурацией.
- Периодический `STATS` tray выполняет в single-flight background worker:
  недоступный IPC не замораживает GTK main loop и не накапливает запросы.
- При старте daemon доверяет только `/etc/punto/config.yaml`; пользовательский
  конфиг становится доступен после подтверждения активного desktop-сеанса.
- Root daemon читает Xauthority активной сессии только из закреплённого
  regular-файла владельца с правами без group/world write. Для локального X11
  точная запись display имеет приоритет, но поддерживается и используемая GDM
  запись с пустым номером display; внутри одного класса действует порядок
  `FamilyLocal` → `FamilyLocalHost` → `FamilyWild`. Повреждённый файл
  отклоняется целиком, а XCB выполняет одну bounded auth-попытку без перебора
  cookies после отказа сервера.
- `RELOAD <path>` разрешён только для `/etc/punto/`, `$XDG_CONFIG_HOME/punto/`
  и `~/.config/punto/`. Путь открывается относительно закреплённого корневого
  дескриптора; обход от `/` не следует ни одному symlink-компоненту, а подмена
  каталога не позволяет выйти из закреплённого root: читается уже открытый
  inode либо операция отклоняется. Чтение и YAML parse выполняются в
  single-flight background loader: команда возвращает `OK Scheduled`,
  параллельный пользовательский reload — `ERROR Config reload in progress`.
  Смена подтверждённой X11-сессии создаёт внутреннее поколение reload: если
  loader занят, последний session intent выполняется после завершения старой
  работы, а результат прежней сессии не может быть опубликован.
- `echo "STATS" | nc -U /var/run/punto.sock` возвращает агрегированные runtime-счётчики.
- В `STATS` видны `daemon_peers`, `analysis_mode`, `log_dropped`,
  `text_mutation=disabled`, эффективный `enabled=0` и отдельный
  `configured_enabled`, чтобы не смешивать возможность мутации с намерением
  запускать анализ. `config_pending`, `config_generation` и `config_result`
  (`none|ok|error`) показывают завершение асинхронного reload.
- Словари загружаются в отдельном потоке после безопасного запуска passthrough,
  signal-fd и диагностического IPC. Пока immutable snapshot не готов, `STATS`
  показывает `analysis_health=degraded` и `worker_threads=0`; ввод при этом
  проходит без блокировки. Один файл ограничен 16 MiB, общий объём —
  32 MiB, строка — 4096 байт, загрузка — 2 000 000 строк.
- Диагностические сообщения демона уходят в syslog/journald; уровень задаётся через `logging.level`.
- При отсутствии, повреждении или превышении лимита словарей daemon публикует
  диагностическое состояние, затем завершает старт с кодом 2 и удаляет свой
  IPC socket вместо перехода в бессрочный degraded-режим.
- Для CI используйте `./build-deb.sh --non-interactive --skip-runtime-installs`.
  Состав пакета детерминирован: tray обязателен по умолчанию; явный
  `--without-tray` собирает daemon-only вариант. Без внешнего
  `SOURCE_DATE_EPOCH` применяется каноническая дата релиза v2.8.7
  (`1788566400`, 2026-09-05 UTC), а заданное окружением значение позволяет
  независимо воспроизвести сборку с другой закреплённой датой.

## Known Limitations / Threat Model

- Полной поддержки Wayland пока нет. Без подтверждённого X11 probe layout-
  snapshot остаётся неизвестным/default EN; это может снизить точность только
  телеметрии анализа, потому что изменение документа в v2.8.7 отключено.
- Подтверждённый X11 layout обновляется периодическим probe, поэтому после
  внешней смены раскладки snapshot может отставать до следующего цикла (обычно
  не более 3 секунд). После временной ошибки сохраняется последний
  подтверждённый snapshot; до первого успешного probe используется default EN.
  Поле `hotkey` это окно не сокращает.
- При logout или невалидном конфиге нового desktop-пользователя сохраняется
  последний успешно проверенный config snapshot. В v2.8.7 он влияет только на
  анализ, уровень логов и resource policy, но не может включить изменение
  документа; исправленный конфиг применяется командой `RELOAD` или следующим
  успешным session reload.
- Maintainer scripts не стартуют неактивный статический user unit от root.
  Обычный full-package upgrade выполняет user-manager daemon-reload и
  per-manager `try-restart` только уже активного `punto-tray.service` в общем
  жёстком лимите 3 секунды, чтобы процесс не продолжал исполнять удалённый ELF
  и не запускался в других
  активных user managers. Fresh install user units не перезапускает. Явный
  переход на `--without-tray` останавливает только этот unit и помечает
  системный XDG autostart conffile как `remove-on-upgrade`.
- Compose/dead keys/AltGr пока не поддерживаются полноценно и считаются отдельной инициативой.
- Безопасная замена слова и преобразование выделения требуют проверяемой,
  причинно связанной с действием редактора транзакции. Такого подтверждения нет
  у универсального X11 clipboard, поэтому в v2.8.7 эти операции во всех окнах
  пропускаются до изменения `PRIMARY`, `CLIPBOARD` или отправки клавиш.
- Clipboard backend ограничен payload 4 KiB и поддерживает только проверяемые
  text-targets без INCR. В v2.8.7 product entry points не вызывают его для
  изменения текста: ручное Pause-действие завершается до чтения или смены
  ownership.
- Демон работает в root-контексте и читает `/proc/<pid>/environ` только для процессов активного GUI-пользователя.
- Логи должны содержать только типы IPC-команд и агрегированные счётчики, а не содержимое набранных слов.

Undo-обучение и его файл `/etc/punto/undo_exclusions.txt` не читаются и не
изменяются product runtime v2.8.7. Компонент сохранён только как изолированный
contract-test до появления доказуемо безопасной транзакции изменения текста.

## Установка

### Способ 1: Сборка и установка deb-пакета (рекомендуется)

`build-deb.sh`:
- проверит зависимости;
- соберёт `punto` и `punto-tray`; отсутствие GTK3/AppIndicator dev-пакетов
  завершает default-сборку явной ошибкой;
- соберёт deb-пакет;
- не будет устанавливать или изменять пакеты в системе сборки.

```bash
git clone https://github.com/antonshalin76/punto.git
cd punto
./build-deb.sh --non-interactive --skip-runtime-installs
sudo dpkg -i "punto-switcher_2.8.7_$(dpkg --print-architecture).deb"
```

### Способ 2: Сборка из исходников

#### Зависимости

> Примечание: `build-deb.sh` рассчитан на Debian/Ubuntu. В CI и автоматизации
> используйте `--non-interactive`; `--skip-runtime-installs` разрешает собрать
> пакет без установки runtime-зависимостей, а `--without-tray` — явно выбрать
> daemon-only состав. Наличие dev-пакетов больше не меняет состав молча.

```bash
# Ubuntu/Debian (минимум для сборки punto)
sudo apt install build-essential cmake pkg-config libyaml-cpp-dev \
  libsystemd-dev libxcb1-dev libxcb-xkb-dev libxau-dev \
  libhunspell-dev \
  interception-tools util-linux

# Только для сборки clipboard contract-тестов (не product runtime)
sudo apt install libxcb-xfixes0-dev

# Обязательно для default-пакета с tray (GTK3 + AppIndicator/Ayatana)
sudo apt install libgtk-3-dev libayatana-appindicator3-dev

# Runtime-словари: daemon требует рабочие EN и RU словари и завершает старт,
# если ни один поддерживаемый источник для языка не найден. .deb устанавливает
# hunspell-en-us и hunspell-ru как зависимости.
sudo apt install hunspell hunspell-en-us hunspell-ru

# Опционально: дополнительный расширенный английский словарь
sudo apt install wamerican-huge

```

#### Сборка

```bash
git clone https://github.com/antonshalin76/punto.git
cd punto
./build-deb.sh
sudo dpkg -i punto-switcher_2.8.7_amd64.deb
```

#### Ручная сборка без пакета

```bash
cd cpp
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build . -j$(nproc)
sudo install -m 0755 punto /usr/bin/punto-daemon
sudo cp ../../config.yaml /etc/punto/
```

Этот минимальный ручной путь является daemon-only: он не создаёт проверяемый
`/etc/punto/runtime-gid`, не устанавливает CLI/tray user unit и поэтому не
обещает IPC. Для полного контракта `0660 root:punto`, CLI и tray используйте
`.deb`, чей maintainer script атомарно публикует runtime GID. Не создавайте этот
файл через world-writable каталог или ослабленные права.

### Настройка udevmon

Пакет намеренно не перезаписывает `/etc/interception/udevmon.yaml` и не
перезапускает/останавливает чужой `udevmon.service`: в нём могут быть другие
interception-пайплайны. Создайте или вручную объедините конфигурацию
`/etc/interception/udevmon.yaml`; после установки `.deb` пример находится в
`/usr/share/doc/punto-switcher/examples/udevmon.yaml` (в исходниках —
`udevmon.yaml`):

```yaml
- JOB: "interception -g $DEVNODE | /usr/bin/punto-daemon | uinput -d $DEVNODE"
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

Команды `start`, `restart` и `stop` вызывают `sudo -n systemctl`: нужен уже
активный sudo-сеанс или узкое правило `NOPASSWD` только для `udevmon.service`.
При отсутствии такого доступа CLI завершается с `ERROR service-error` и не
показывает интерактивный запрос пароля.
Если `udevmon` уже активен, `punto start` только проверяет существующий IPC и
не перезапускает общий сервис при `denied`, timeout или ошибке протокола;
явный перезапуск выполняет только команда `punto restart`.

#### Вручную через systemd

```bash
sudo systemctl enable udevmon
sudo systemctl start udevmon
```

## Настройка

Конфигурация по умолчанию: `/etc/punto/config.yaml`

Пользовательский конфиг (получает приоритет после подтверждения активного
desktop-сеанса): `~/.config/punto/config.yaml`

```yaml
# Совместимость схемы: v2.8.7 принимает, сохраняет и игнорирует это поле
hotkey:
  modifier: leftctrl   # leftctrl, rightctrl, leftalt, rightalt, leftshift, rightshift, leftmeta, rightmeta
  key: grave           # grave (` ~), space, tab, backslash, capslock, а также left/right: shift/ctrl/alt/meta

# Раздел delays удалён в v2.8.0 и строгой схемой не принимается. Product runtime
# v2.8.7 не выполняет эмуляцию исправляющих клавиш.
# Анализ слова при нажатии пробела/таба; в v2.8.7 без изменения текста
auto_switch:
  enabled: true        # Включить анализ; не включает изменение текста
  threshold: 3.5       # Порог срабатывания (разница скоров)
  min_word_len: 2      # Минимальная длина слова для анализа
  min_score: 5.0       # Минимальный скор для уверенного решения
  max_rollback_words: 5 # Совместимость; в v2.8.7 rollback не выполняется

# Совместимость: в безопасном режиме v2.8.7 звук исправлений не воспроизводится
sound:
  enabled: true

logging:
  level: info

runtime:
  analysis_threads: 0              # 0 = auto-budget, >0 = фиксированное число на daemon
  max_analysis_threads_per_daemon: 4
```

### Переключение раскладки

Настройте реальное переключение раскладки средствами GNOME/KDE/X11. Поля
`hotkey.modifier` и `hotkey.key` в v2.8.7 оставлены только для обратной
совместимости конфигурации: daemon их не использует, а tray не показывает их и
не запускает `gsettings`/`setxkbmap`.

После изменения можно применить настройки без перезапуска:

```bash
# Через tray-приложение: диалог настроек -> "Сохранить" (применяется сразу)

# Или через командную строку:

echo "RELOAD" | nc -U /var/run/punto.sock
# OK Scheduled; завершение: STATS config_pending=0 config_result=ok

# Эффективная возможность изменения текста фиксированно отключена:
echo "SET_STATUS 1" | nc -U /var/run/punto.sock
# ERROR Text mutation disabled
echo "SET_STATUS 0" | nc -U /var/run/punto.sock
# OK DISABLED

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
│   │   ├── key_injector.hpp      # Изолированный mutation contract-test
│   │   ├── clipboard_manager.hpp # Изолированный X11 contract-test
│   │   ├── x11_session.hpp       # Управление X11 сессией
│   │   ├── sound_manager.hpp     # Изолированный sound contract-test
│   │   ├── text_processor.hpp    # Обработка текста
│   │   ├── event_loop.hpp        # Главный цикл
│   │   ├── layout_analyzer.hpp   # Анализатор раскладки (биграммы+триграммы)
│   │   ├── dictionary.hpp        # Словарный анализатор (hunspell)
│   │   ├── ipc_server.hpp        # IPC сервер (/var/run/punto.sock)
│   │   ├── ipc_client.hpp        # IPC клиент (для tray)
│   │   ├── tray_app.hpp          # Tray UI
│   │   ├── settings_dialog.hpp   # Диалог настроек (GTK)
│   │   ├── history_manager.hpp   # Изолированный history contract-test
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
│   ├── prerm
│   └── postrm
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

Это ожидаемое поведение безопасного режима v2.8.7: комбинация поглощается до
чтения или изменения X11 selection и до отправки paste.

### Автопереключение не срабатывает

Изменение документа ожидаемо отключено в v2.8.7. Для проверки только анализа:

1. Проверьте, что `auto_switch.enabled: true` в конфиге.
2. Убедитесь, что установлены hunspell словари:

```bash
sudo apt install hunspell-en-us hunspell-ru
```

3. Проверьте `STATS`: `configured_enabled` отражает конфиг, а
   `text_mutation=disabled enabled=0` — эффективную безопасную границу.

### Переключение раскладки не срабатывает

Punto v2.8.7 сам не переключает раскладку. Настройте комбинацию средствами
рабочего стола. Поле `hotkey` в конфиге совместимое, но неактивное; анализатор
обновляет layout-snapshot только периодическим подтверждённым X11 probe.

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
| libsystemd                 | любая                     |
| libxcb + libxcb-xkb        | любая                     |
| XFixes                     | любая (только тесты)      |
| libyaml-cpp                | 0.8                       |
| libgtk-3-0                 | любая (tray, опционально) |
| libayatana-appindicator3-1 | любая (tray, опционально) |
| libhunspell                | любая                     |
| hunspell-en-us             | любая (runtime)           |
| hunspell-ru                | любая (runtime)           |
| wamerican-huge             | любая (опционально)       |

## История изменений

### v2.8.7 — X11-авторизация и безопасное обновление tray

- X11 probe принимает GDM `Xauthority` с пустым display number как fallback,
  сохраняя приоритет точной записи и fail-closed разбор повреждённых файлов.
- Full-package upgrade обновляет уже активный tray отдельно в каждом user
  manager и не запускает неактивный static unit или одноимённый unit при fresh
  install.
- Policy, user-manager reload и upgrade restart выполняются в общем жёстком
  трёхсекундном лимите, поэтому зависший desktop bus не блокирует `dpkg`.

### v2.8.6 — Fail-closed runtime и воспроизводимый релиз

- Конфигурация разбирается строгим `yaml-cpp`: неизвестные/повторные ключи, неверные типы и небезопасные значения отклоняются целиком.
- Startup использует только системный конфиг до установления активного
  desktop-пользователя; `RELOAD` в bounded background lane читает файл через
  descriptor-rooted walk от `/`, отклоняет symlink/rename escape и публикует
  typed completion в `STATS`.
- IPC, runtime-файлы и control plane получили ограниченные очереди, строгие протоколы, безопасные права и отдельный трёхсекундный deadline для каждого shutdown-барьера.
- X11/clipboard backend переведён на bounded XCB, проверку владельца сессии и
  сериализованную generation lease. Product entry points v2.8.6 завершаются до
  X11 clipboard I/O и эмуляции клавиш: ни terminal `Ctrl+Shift+V`, ни GUI
  `Ctrl+V` вместе с наблюдаемыми selection-событиями не дают причинно связанного
  подтверждения применения подготовленного payload.
- Rich/custom clipboard не упрощается до текста: такие операции завершаются
  безопасным пропуском до смены X11 ownership.
- Desktop-сессия обнаруживается в фоне. Sound backend изолирован в отдельном
  contract-test target и не входит в product runtime безопасного режима.
- Session config reload получил generation-owned latest-intent retry: смена
  пользователя во время занятого loader не оставляет daemon на старом пути.
- Failover control plane выполняет reconcile сохранённого config до promotion:
  трёхролевой commit→crash→promotion не может переопубликовать stale snapshot
  с прежним generation, а отсутствующий config не открывает primary IPC без
  успешного fallback текущей authority.
- Tray status poll переведён с синхронного IPC на coalesced background worker;
  lifecycle процесса принадлежит статическому `systemd --user` unit.
- Запись в syslog вынесена из input/worker threads в отдельный sink с
  ограниченной очередью; переполнение учитывается в `STATS`, а зависший sink не
  блокирует ввод и аварийно завершает shutdown по трёхсекундному deadline.
- CLI и `.deb` используют `/usr/bin`, единый файл `VERSION`, неинтерактивную воспроизводимую сборку и идемпотентные lifecycle-скрипты.
- Пакет не владеет глобальным `udevmon.yaml` и не управляет чужим `udevmon.service`; пример конфигурации устанавливается отдельно для явного ручного merge.
- Mutation-компоненты (clipboard, key injection, sound, macro/history/undo)
  физически исключены из production target и проверяются только отдельными
  contract-test target.
- Загрузка словарей и сканирование `/proc` имеют строгие byte/entry/candidate/
  time limits; диагностический IPC доступен в состоянии pending, а фатальная
  инициализация завершается с детерминированным кодом.
- `SET_STATUS 0` — совместимый no-op, `SET_STATUS 1` честно отклоняется:
  управление анализом определяется только `auto_switch.enabled`, а возможность
  изменения текста в этой версии всегда выключена.
- XFixes удалён из production link graph и требуется только clipboard
  contract-тесту.
- Добавлены contract/e2e-тесты для daemon, IPC, nested-X11 GTK/VTE platform integration, конфигурации, CLI и package lifecycle.

### v2.8.5 — Стабильность control plane и IPC

- Синхронизирован доступ к общему состоянию control plane при работе нескольких `punto-daemon`.
- Исправлены гонки при reload/config/status, восстановлении primary IPC и остановке IPC-сервера.
- Добавлена регрессия на продолжение поколений control plane после рестарта primary.

### v2.8.4 — Стабильность async-обработки и Hunspell

- **Исправлен use-after-move в async typo fix**: результат отправляется строго один раз, без дубликатов и мусорных данных.
- **Hunspell теперь потокобезопасен**: доступ к `spell()`/`suggest()` защищён мьютексом, чтобы избежать гонок.

### v2.8.3 — Исправление маппинга знаков препинания + XKB sync

- **Полный маппинг знаков препинания** при инверсии раскладки:
  - `?` ↔ `,` (Shift + / в EN ↔ Shift + / в RU)
  - `/` ↔ `.` (точка и слэш корректно меняются)
  - `|` ↔ `/` (Shift + \\ корректно меняется)
- **Полный маппинг цифрового ряда** с Shift:
  - `@` ↔ `"` (Shift + 2)
  - `#` ↔ `№` (Shift + 3)
  - `$` ↔ `;` (Shift + 4)
  - `^` ↔ `:` (Shift + 6)
  - `&` ↔ `?` (Shift + 7)
- **Исправлена синхронизация XKB** после рестарта сервиса:
  - Периодическая ре-проверка XKB set (каждые 10 сек) для восстановления после временных сбоев
  - Решена проблема десинхронизации раскладки сразу после `punto restart`
- **Синхронизированы таблицы** в `scancode_map.hpp` и `typo_corrector.cpp`
- **Исправленный сценарий**: `ghbdtn?` → `привет,` (ранее: `привет?`)

### v2.8.2 — Undo последнего исправления + dictionary-first автоинверсия + фикс selection hotkeys

- **Undo последнего исправления**: `Ctrl+Z` отменяет последнюю автокоррекцию/хоткей Punto.
  - Перехватывается только в коротком окне сразу после исправления (иначе `Ctrl+Z` идёт в приложение).
- **Автопереключение раскладки** теперь строго dictionary-first по правилам:
  - если слова нет в словаре текущей раскладки, но есть в противоположном — делаем инверсию;
  - если слово есть в текущем словаре и нет в противоположном — инверсию не делаем;
  - если слова нет в обоих словарях — инверсию не делаем;
  - если слово есть в обоих словарях — решение через N-граммы.
- **Хоткеи по выделению** стали заменять выделенный текст (а не вставлять рядом с курсором).
  - В версии 2.8.2 это относилось к GUI-редакторам; терминальные хоткеи тогда
    вставляли преобразованный текст в позицию курсора. Начиная с 2.8.6
    терминальные преобразования fail-closed пропускаются до отправки клавиш.

### v2.8.1 — Исправление oneshot вставки в терминалах

- **Исправлена вставка в терминалах** после oneshot-замены:
  - расширена детекция терминальных окон по `WM_CLASS` (instance/class);
  - добавлен подъём по дереву окон (активным может быть дочерний window);
  - в терминалах paste выполняется через `Ctrl+Shift+V`, в остальных окнах —
    через `Shift+Insert`;
  - перед paste выставляются оба selection: `CLIPBOARD` и `PRIMARY`;
  - увеличены паузы вокруг `Clipboard+Paste`, чтобы избежать гонок и раннего restore.

### v2.8.0 — Oneshot замена через Clipboard+Paste + удаление delays

- **Oneshot замена текста**: все режимы замен (авто/ручные, слово/selection, async corrections) выполняются через Clipboard+Paste вместо посимвольного retype.
- **Удалён раздел `delays`** из конфига и UI: настройки задержек больше не редактируются в YAML/`punto-tray`.

### v2.7.5 — Исправление ложного переключения при вводе чисел

- **Исправлен баг ложного автопереключения для чисел**:
  - Ввод чисел ("123", "2024", "3.14") больше не вызывает переключение раскладки
  - Добавлена проверка `has_invalid_chars` в `AnalysisWorkerPool` перед обращением к словарю
  - Синхронизировано поведение асинхронного воркера с логикой `LayoutAnalyzer::should_switch()`
  - Цифры теперь корректно распознаются как нейтральные символы для целей определения раскладки

### v2.7.4 — Исправление привязки к login screen (GDM)

- **Сервис больше не "прилипает" к greeter-сессии** (gdm/lightdm) после boot.
- **Автоматическое переподключение к активной user-сессии** после логина/логаута.
- **Пересоздание X11/Audio-зависимых компонентов** (clipboard, звук) при смене сессии.

### v2.7.3 — Персистентный Undo Detector

- **Персистентные исключения Undo**: теперь сохраняются между сессиями
  - Файл хранения: `/etc/punto/undo_exclusions.txt`
  - Автозагрузка при старте сервиса
  - Атомарное сохранение с межпроцессной блокировкой
  - До 128 lowercase ASCII-слов по 63 байта, файл `0600`
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
