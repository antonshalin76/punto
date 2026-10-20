# Вероятностное переключение и охват редакторов

База: установленный локальный2.8.15 r2, исходники в текущем изолированном
worktree `fix/punto-hotkeys-20261020`. Предыдущие изменения сохраняются.

- [x] Найти владельцев решения: AnalysisWorkerPool/LayoutAnalyzer — язык;
  EventLoop — ограниченная история/порядок; AccessibleContext — редактор;
  WordEditor — подтверждённая замена и clipboard.
- [x] Подтвердить две причины: Unknown явно запрещён в worker; Telegram
  регистрирует Qt и GTK providers с одинаковым PID, helper отвергает дубль.
- [x] Закрепить единым design/coverage review правила и отрицательные контроли.
- [x] Включить Unknown только при сильном N-gram свидетельстве; сохранить
  правильные слова, код/URL/числа/неоднозначные сочетания и пользовательский undo.
- [x] Добавить отложенную проверку однобуквенного слова по двум соседям.
  Использовать прежнюю bounded history и точное подтверждение всего suffix;
  смена focus/editor, ручная правка, Tab и противоречивый контекст отменяют её.
- [x] Исправить выбор единственного focused editable среди providers одного
  процесса. Два редактора, чужой provider/UID, password-role — отказ.
- [x] Матрица редакторов хоста: GTK Entry/TextView, Chromium plain/rich,
  Electron/VS Code, Qt/Telegram, Firefox, VTE/терминал и найденные иные toolkit.
  Для каждого отдельно записать обнаружение, native scratch check и ограничения.
- [x] Пройти causal RED/GREEN, проверки ложных переключений и контекста,
  native E2E и sanitizer затронутых путей; финальный combined review.
- [x] Собрать пакет, сохранить свежий резерв, установить только Punto;
  выполнить host-приёмку, сохранив документы, сообщения, clipboard и настройки.
  - [x] Пакет2.8.16: build,107 lifecycle checks PASS; lintian без errors
    (четыре предупреждения о man pages/changelog).
  - [x] Fresh quiescent snapshot и точный rollback на2.8.15 r2 подготовлены;
    dpkg transaction0,23 immutable payloads и четыре mutable файла проверены.
  - [x] Punto запущен через typed readiness; GNOME/IBus/GDM PID не изменились.
  - [x] Host GTK/Chrome: все пять хоткеев, AUTO, отказ/recovery, следующий RU
    glyph, clipboard и модификаторы PASS.
  - [x] Host Unknown/noise/context PASS; burst сохраняет двухсловный предел
    отката, явные ручные переключения не используются для обхода reset-политики.
  - [x] Сам Telegram Qt6: пять хоткеев, обе стороны, Unknown AUTO, одиночная
    буква, следующий RU glyph PASS. Поле «Избранного» пустое, сообщений нет,
    clipboard/начальная раскладка/освобождённые клавиши проверены.

## Критерии и ограничения

Не приравнивать высокий ratio к вероятности: нужен абсолютный score и
несколько независимых N-gram совпадений. Dictionary-current имеет приоритет.
Имена/опечатки не обязаны исправляться все; сомнительный токен остаётся как есть.
Решение Unknown меняет только раскладку, а не угадывает/переписывает опечатку.

Однобуквенные слова: RU «а в и к о с у я», EN «a I». Не соединять соседние
слова для создания вымышленных триграмм. Соседи должны однозначно указывать
один язык; одиночная буква без надёжного контекста не меняется.

Секретные поля и редактор без доказанного native origin не получают расширенных
эвристик. Автотесты не отправляют сообщения, не открывают личные документы и
не перезапускают GNOME Shell/IBus/GDM. Один рабочий план, без C2C и agent-collab.

После source gates установлен2.8.16, пакет SHA256
`b73fa488d7b84a162fcc842187d9a41773aa43124e2bcd9af8fee6e4b266a39a`.
Резерв/rollback: `/var/lib/punto-feature-update-20261020-2816`.
Host-приёмка закрыта; публикация не выполнялась.
Матрица не выдаёт один GTK/Qt fixture за все приложения.

## Текущий checkpoint

Реализованы строгий Unknown, контекст одиночной буквы, выбор среди нескольких
providers, bounded native discovery без Collection и типизированные Text
properties для Qt. Runtime-зависимостей не добавлено. Review выявил right-first
гонку сохранённой раскладки: после успешной коррекции запись теперь хранит свою
целевую раскладку. Детерминированный RED вернул EN после русского suffix;
GREEN проверил RU и следующий обычный символ.

GTK4.14 экспортирует пустой PRIMARY для выделения справа налево. Независимая
проверка без Punto подтвердила причину. Реализовано ограниченное изменение
направления native-диапазона с повторной проверкой границ и SHA; текст по-прежнему
меняется только после получения настоящего PRIMARY и прежних receipt-проверок.
Финальные GTK4 Entry/TextView проходят обе полные вертикали под ASan/UBSan.
Подготовка направления использует общий macro-bound только после положительной
проверки исходного token/range; подтверждение нового диапазона и pre-publication
по-прежнему ограничены300ms. Hard deadline удержанного Tab не увеличен.

Матрица внешних редакторов должна использовать
`punto-event-loop-layout-bridge-e2e-driver` вместе с
`PUNTO_REAL_LAYOUT_BRIDGE_E2E=1`. Сочетание этого режима с legacy test-driver
давало ложный XKB timeout. Legacy passive grab также создавал focus-события,
которых нет у продуктового GNOME bridge. Эти результаты не являются дефектами
продукта и не оправдывают ослабления проверок.

## Evidence ledger

Все журналы находятся в родительском task cache. Для изменённого владельца
повторяется только его зависимый gate; упаковка и host-приёмка ждут source gate.

| Gate | Проверенный вход / результат | Журнал | Срок применимости |
| --- | --- | --- | --- |
| Unknown unit RED/GREEN | Старый worker RED; 28 примеров и 288 contextual combinations GREEN | probabilistic-red.log / probabilistic-green.log | До изменения classifier/worker/tests |
| ASan/UBSan unit | Debug unit текущего classifier PASS | probabilistic-asan-unit.log | До изменения classifier/worker/tests |
| GTK contextual | 8/8 включая minimum, history, reset, обе стороны и forced right-first PASS | context-right-first-green.log | До изменения EventLoop/WordEditor/helper/fixture |
| Qt native | 9/9, три реальных Qt editor classes, все хоткеи и negative provider cases PASS | qt-final-native.log | Проверено до нового направления выделения; включить в финальный aggregate |
| GTK4 platform cause | Reverse PRIMARY empty; forward/reorientation restores exact payload | gtk4-primary-direction-repair.log | Причинный diagnostic, не финальный gate |
| External editor acceptance | Реальные Code/GNOME Text Editor/Firefox3/3 PASS; GTK4 Entry/TextView ASan/UBSan2/2 PASS | final-product-app-acceptance.log / final-product-surface-asan-matrix.log | WordEditor SHA14af5db0412c8e3276d808f677fae75e46e335835c00af05141a1b03a0650963 |
| ASan/UBSan negative controls | 6/6 deadline/receipt +6/6 Chromium rich/retry/Tab PASS, без sanitizer errors/skips | final-product-surface-asan-negative.log / final-product-surface-asan-chromium.log | Тот же WordEditor SHA |
| Release aggregate | Первый54-gate прогон45 PASS,8 failed,1 явный WebKit SKIP; затронутые группы закрыты повтором6/8 и последним2/2. Итого53 PASS +1 explicit SKIP, прежние failures сохранены | final-release-aggregate.log / final-source-affected.log / final-harness-recheck.log | Runtime WordEditor SHA14af5d; последние два изменения только harness |
| Shared-session harness | Все20 повторов PASS47.86s после изменения только общего watchdog45→90; локальные deadline сохранены | final-harness-recheck.log | До изменения cohort/runtime/fixture |
| Chromium physical selection | Preflight100ms вместо1ms; весь shard34 tests PASS, focused ASan1/1 PASS, real PRIMARY fault и exact refusal сохранены | final-harness-recheck.log / final-harness-asan-selection.log | До изменения selection helper/receipt owner |
| Static | Четыре владельца clang-tidy PASS; финальный WordEditor отдельно PASS; форматирование и diff-check PASS | final-owner-tidy.log / final-word-editor-tidy.log | До следующей правки владельца |
| Package/install | Build PASS,107/107 lifecycle; transaction0, exact config/learning; typed readiness ready | feature-2816-package-build.log / feature-2816-package-contract.log / deploy-feature-2816/install.log | Exact DEB SHA b73fa488d7b84a162fcc842187d9a41773aa43124e2bcd9af8fee6e4b266a39a |
| Host acceptance | GTK/Chrome/Unknown/noise/context и настоящий Telegram hotkeys/AUTO/context PASS; пустое поле, нет отправки, clipboard/модификаторы/начальная раскладка сохранены | deploy-feature-2816/host-gtk-eligible.log / host-chrome.log / host-feature-burst.log / host-telegram-cleanup.log / host-telegram-auto.log | Exact installed DEB b73fa488…; primary2341337/start9107588, все unit/desktop identities проверены |

## Закрытие

Реализованный объём завершён и внедрён локально. UNKNOWN по-прежнему не означает
автоматическое исправление любой опечатки. Существующее исключение `ghbdtn`
сохранено: первые host AUTO fixtures ошибочно ожидали его исправления.
Использованные неисключённые слова проверяются перед тестом; обучение не сбрасывается.
Первый Telegram проход завершил продуктовые действия, но упал на лишнем возврате
в скрытое scratch-окно. Следующий полный проход и отдельный AUTO/context закрыли
cleanup, пустой draft, clipboard и исходную раскладку с exit0.
Все предыдущие неуспешные журналы сохранены и не переименованы в успешные.

Не подтверждены Writer без provider, WebKit cross-process, реальный DBeaver
StyledText и default Code profile/его unrepresentable paste. Это границы
поставленного backend, не скрытые PASS. Точная матрица связана выше.
Публикация/merge в GitHub не входят в выполненные действия этой итерации.
