# Punto Switcher 2.8.17 — явное обучение исключениям

Удерживайте Ctrl+Shift и дважды нажмите Backspace с отпусканием клавиши
между нажатиями, в течение2.5s после автоматического исправления. При успешном
возврате исходного слова Punto сохраняет исключение. Автоповтор не считается
вторым нажатием. Обычное удаление и Ctrl+Z больше не обучают исключения.

Жест требует того же подтверждённого поля без последующего ввода. Истечение
срока, смена поля, ошибка обратного преобразования или ручное исправление
не создают исключение. Начатый жест не удаляет текст в другом поле; отпускание
Ctrl/Shift отменяет захват. Вне допустимого контекста сочетание передаётся
приложению и не объявляется свободным во всех программах.

Старый список из12 случайно накопленных исключений очищен по запросу
пользователя. Новые записи сохраняются прежним защищённым хранилищем;
ручная коррекция остаётся доступной для исключённого слова.
Проверки и внедрение: [рабочий план](protocol/explicit-learning-gesture-plan-20261020.md).
Кандидат2.8.17 установлен локально и проверен в owned GTK-поле через установленный
daemon/uinput. Тестовое исключение удалено; список пуст. Контрольная сумма DEB
и фактический охват проверок приведены в рабочем плане.

## Punto Switcher 2.8.16 — вероятностный анализ и native-редакторы

- Неизвестное слово может сменить раскладку при сильных совместных признаках
  биграмм и триграмм. Словарь текущего языка, технические токены и неоднозначные
  окончания защищены. Сама опечатка сохраняется; в2.8.16 undo обучал исключения.
- Однобуквенное RU/EN-слово проверяется после согласования обоих соседей.
  Используются прежняя ограниченная история и точное подтверждение suffix.
- Допуск редактора учитывает несколько providers одного PID, включая Qt/GTK
  Telegram. Для providers без Collection добавлен bounded native tree; Qt Text
  properties читаются типизированным Get. Password-поля исключены.
- Обратное выделение GTK4 с пустым PRIMARY меняет направление только под
  native-проверками границ/SHA. Замена требует настоящего нового PRIMARY.
- Если приложение перехватывает одиночный Alt, для регистра выделения доступен
  существующий вариант RCtrl+Alt+Pause; LCtrl+LAlt+Pause остаётся транслитерацией.

Проверки и внедрение кандидата: [рабочий план](protocol/probabilistic-editor-coverage-plan-20261020.md).
Кандидат установлен локально; публикация в GitHub не выполнялась.
GTK/Chrome, Unknown и контекст проверены на host. Сам Telegram прошёл пять
хоткеев, Unknown AUTO, одиночную букву, следующий RU-символ и cleanup в
разрешённом пустом поле без отправки сообщений.

Native-приёмка на изолированных документах проходит для GTK3/4, Chromium,
Qt5, VS Code, GNOME Text Editor, Firefox и SWT Text. Отдельная host-приёмка
Telegram также прошла. WebKit cross-process, Writer без AT-SPI provider и произвольный
Unicode paste в Code не объявляются поддержанными по результатам этих проверок.

## Punto Switcher 2.8.15 — автокоррекция и выделение

Локальный кандидат r2 установлен, но не опубликован в GitHub. В реальных GTK
и Chrome прошли пять горячих клавиш, AUTO, слова пользователя, отказ/recovery,
следующий RU-символ и сохранность clipboard. Настройки и обучение сохранены.
Опубликованные файлы2.8.14 не меняются. Подробности — в рабочем плане.

- Исправлено преждевременное обрезание русских «ю», «б», «ж», «э» как
  пунктуации. Словарный анализ предпочитает полное известное слово.
- Origin редактора получает до100ms вместо20ms. Превышение срока не разрешает
  замену неподтверждённого текста и не блокирует следующую корректную операцию.
- Shift+Pause, Alt+Pause и Ctrl+Alt+Pause поддерживают подтверждённые plain-text
  выделения в Chromium между форматированными фрагментами и абзацами.
  Нативные границы и идентичность дочерних узлов перепроверяются перед заменой.
- Старые методы D-Bus, GNOME InputSource API, пользовательские настройки и
  словарная политика неизвестных слов не меняются. Обновляются daemon/helper;
  перезапуск GNOME Shell, IBus, GDM или графической сессии не нужен.
- В исходниках убран произвольный потолок32 зарегистрированных AT-SPI-приложений,
  блокировавший чтение любого редактора на загруженном desktop. Причинный
  private RED/GREEN на34 приложениях прошёл; лимиты ответа8KiB и времени
  сохранены. Финальные private-проверки: GTK accessibility14/14, crowded ASan1/1,
  Chromium ASan17/17. Повторная сборка/установка r2 и host-приёмка завершены.

Ограничения: однобуквенные слова при min_word_len=2 и неизвестные слова с
опечатками не обязаны исправляться автоматически. Вложенные таблицы, картинки,
несовпадение PRIMARY и неподтверждённые диапазоны по-прежнему дают безопасный
отказ. Это X11 replay с проверками, не атомарная транзакция редактора.
Текущие проверки и внедрение: [рабочий план](protocol/hotkeys-autocorrection-plan-20261020.md).

## Historical 2.8.14

## Changes

- Coherent X11 daemon, authenticated session helper, GNOME RU/EN bridge and tray.
  Startup requires typed desktop readiness instead of reporting partial success.
- Manual Pause conversion, Shift+Pause selection conversion and inverse/undo
  keep focus, text, layout, clipboard and modifier guards aligned.
- Selection-only commands require a current nonempty editor range matching
  PRIMARY, including after layout waits. Cached PRIMARY without a visible
  selection cannot append text; a safe refusal preserves ordinary Pause recovery.
- Explicit systemd state handling: transitions and failed services no longer
  masquerade as stopped writers. Helper retirement waits for backend shutdown.
- Session-helper protection permits read-only access to the user session bus.
- Sound settings open their own dialog instead of the general settings window.
- Bounded default build parallelism, explicit OpenSSL dependencies, package
  lifecycle/recovery checks and extended real-layout GTK/Chromium coverage.

## Installation and upgrades

The Debian package is built by `./build-deb.sh`; this command does not install
software or restart the running service. Install the release DEB through the
normal package manager. Preserve user configuration and learned exclusions.

When upgrading an already loaded GNOME extension, save documents and log out,
then log back in using Ubuntu/GNOME on Xorg. A machine reboot or forced Shell/
IBus restart is not required. Use `punto start` and `punto status` after login.

## Validation and boundaries

Qualified private suites cover GTK209, real-layout GTK/VTE14, Chromium114 plus
two real-layout cases, repeated conversion/recovery, sanitizers, package lifecycle
and exact-old-version recovery. Installed-host checks independently exercise
supported GTK/Chrome manual, selection, next-glyph, empty recovery, Space-auto,
clipboard and modifier behavior through the installed daemon/uinput pipeline.
These finite checks are not a guarantee that every future failure is impossible.

- The normal supported desktop is GNOME46 on X11; no full Wayland backend.
- No browser/editor extension is required. The GNOME layout bridge is required.
- Automatic Tab correction requires a fully portable clipboard snapshot. Private
  Chromium targets such as `chromium/x-source-url` are not silently discarded:
  correction is refused, clipboard retained, and the original Tab is delivered.
- Compose/dead-key/AltGr input and arbitrary editor callback atomicity are not
  promised. Unknown focus, selection, clipboard or delivery outcomes fail closed.
- Current Chrome can report an empty accessibility range for a visible selection
  containing non-BMP emoji. Punto refuses that inconsistent selection instead of
  treating cached PRIMARY as permission to edit. Select text with a valid
  accessibility range to continue; no service restart is required.
- The current host run is software-pipeline validation, not physical-device
  qualification or a new long-duration idle soak. Historical soak evidence is
  identified separately in RESTORATION.md.
