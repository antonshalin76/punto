# Автокоррекция и преобразование выделения: 20 октября 2026

Основа: `master e85f403`, ветка `fix/punto-hotkeys-20261020`.
Локально установлен и принят неопубликованный кандидат2.8.15 (r2); прежний
опубликованный2.8.14 не переписывается. Старый dirty checkout сохранён.

- [x] Проверить фактически работающие daemon, helper, конфигурацию и словари.
- [x] Разделить причины отказов: анализ слова, origin редактора, границы выделения.
- [x] Сохранить русские буквы на физических клавишах пунктуации. Словарь выбирает
  самое длинное известное слово; оставшийся суффикс сохраняется.
- [x] Поднять ограниченный бюджет origin с 20 до 100 ms. Реальный GTK provider
  с одноразовой задержкой 40 ms даёт RED на прежнем и GREEN на новом бюджете.
- [x] Добавить отдельный ObserveSelection без изменения прежних методов D-Bus.
  Для Chromium использовать Document.GetTextSelections, реальные дочерние
  объекты и их offsets, а не считать U+FFFC текстом выбранного абзаца.
- [x] Проверить 16 новых Chromium-сценариев: слово/фраза/несколько абзацев,
  регистр, транслитерация, текущие слова, stale identity и восстановление.
- [x] Независимое combined review runtime/protocol/тестов: PASS.
- [x] Проверить отказ при превышении origin deadline и следующий успешный ввод.
- [x] Release gate, sanitizer и статический анализ исходного кандидата до
  обнаружения потолка registry; результаты ниже не выдают полный GREEN новой правке.
- [x] Собрать/проверить пакет r1 и установить только Punto со свежим резервом.
- [x] Проверить host GTK: пять хоткеев, AUTO, слова пользователя, следующий RU
  символ; сохранить config/learning и идентичности основных desktop-процессов.
- [x] Причинно воспроизвести потолок32 AT-SPI-приложений; убрать избыточный
  count guard, сохранив8KiB/deadline; private34-app RED/GREEN1/1, review PASS.
- [x] Закрыть проверки исправления registry: весь accessibility GTK14/14,
  crowded ASan1/1, native Chromium ASan17/17.
- [x] Пересобрать пакет r2 и установить со свежим quiescent snapshot, не архивом r1.
- [x] Принять установленный GTK и Chromium: выделение, регистр, транслитерация,
  AUTO/recovery, clipboard, модификаторы и следующий RU-символ.
- [x] Закрыть локальное внедрение: exact payload, config/learning, core processes,
  освобождённые модификаторы, пустые очереди, активные daemon/helper/tray.
- [ ] Отдельный этап: commit/push, hosted CI и публикация GitHub-релиза.

## Границы изменения

Не меняем GNOME JavaScript, IBus, GDM, графическую сессию и политику неизвестных
слов. Новых зависимостей нет. Новый selection proof ограничен 128 прямыми
дочерними узлами и 4096 bytes; вложенные неподдерживаемые структуры, несовпадение
PRIMARY, смена provider/focus/range и deadline приводят к безопасному отказу.

Origin может задержать первый физический символ нового слова максимум своим
100-ms бюджетом; нормальный provider отвечает сразу. Это не обещание нулевой
задержки и не ослабление проверки перед изменением документа.

В примере пользователя `rfrb[`, `ctqxfc`, `dsltktybt` соответствуют словарным
словам. Однобуквенные `d`, `b` ниже min_word_len=2; `ghkj;tybz[` содержит опечатку
и соответствует «прложениях». Автоматическое исправление неизвестных слов не
включается ради этого примера. Ручное преобразование сохраняет исходные опечатки.

## Доказательства

Локальные артефакты расположены рядом с source в
`.cache/punto-hotkeys-20261020.49Zxx6/` исходного checkout.

| Проверка | Результат | Артефакт |
| --- | --- | --- |
| Native GTK origin 40 ms, старый бюджет | RED: origin отсутствует, текст не исправлен | gtk-origin-red.log |
| Native GTK origin 40 ms, новый бюджет | PASS 1/1 | gtk-origin-green.log |
| Native GTK буквенное окончание и суффикс | PASS 2/2 | gtk-endings-green.log |
| Worker: 4 окончания × 8 суффиксов × 2 раскладки | PASS | unit-final.log |
| D-Bus contract | PASS 11/11 | protocol-tests.log |
| Native Chromium новые сценарии | PASS 16/16, 29.941 s | chromium-native-selection-final.log |
| Fingerprints до/после Chromium | совпадают | chromium-native-selection-final-{before,after}.sha256 |
| Полная сборка Release | PASS | full-build.log |
| GTK origin40/250ms: допуск, отказ, следующий успешный ввод | PASS 2/2 | gtk-origin-budget.log |
| Native rich-paragraph AUTO и следующий RU символ | PASS 1/1 | chromium-rich-automatic.log |
| ASan/UBSan/LSan: native Chromium после исправления утечки | PASS 16/16 | sanitizer-chromium-leakfix.log |
| ASan/UBSan/LSan: AUTO в абзаце и отказ/paste recovery | PASS 2/2 | sanitizer-auto-recovery.log |
| ASan/UBSan/LSan: GTK origin/окончания/суффикс | PASS 4/4 | sanitizer-gtk.log |
| ASan/UBSan/LSan: unit matrix | PASS | sanitizer-unit.log |
| Clang-tidy: 6 затронутых production TU | PASS | clang-tidy.log |
| Бounded raw queue после корректировки fixture | PASS 2/2 | gtk-raw-candidate-nine.log |
| Package Python contracts | PASS 20/20 | package-python.log |
| Общий Release gate | 46/50 PASS; четыре отказа разобраны ниже | release-ctest.log |
| Адресный конечный Release recheck | PASS 4/4, 225.95 s; вместе закрыты все50 групп | final-source-recheck.log |
| Пакет r1 | сборка PASS; package965/965; Lintian warnings сохранены | package-build.log, package-contract.log, package-lintian.log |
| Host GTK r1 | пять хоткеев, AUTO, слова пользователя, next RU: PASS | deploy-20261020/host-gtk-quiet.log |
| Host Chromium r1 | FAIL editor_context; приёмка не закрыта | deploy-20261020/host-chrome-quiet.log |
| Native registry34 apps, прежний потолок32 | RED: отказ GetChildren до ready_editor | crowded-desktop-red.log |
| Native registry34 apps, без count guard | GREEN 1/1, 4.488s; AUTO/recovery/clipboard/modifiers | crowded-desktop-green.log |
| Весь класс Accessible GTK после поправки | PASS 14/14, 10.822s | accessible-final.log |
| Native registry34 apps, ASan | PASS 1/1, 5.115s | crowded-sanitizer-green.log |
| Native Chromium после поправки, ASan | PASS 17/17, 33.134s | chromium-cap-final.log |

Host r1 DEB SHA256:
`6fc826bedc318ef959767f8ec41d7b2c4004883e205430daf9464f8356096ca6`.
Перед собственными GTK и Chromium было31 registry-приложение, после их запуска33.
Отказ helper на потолке32 не связан с неподдерживаемым выделением: он происходил
ещё до чтения редактора. Native34-app RED/GREEN подтверждает эту причину.
Исправление не повышает лимит до нового произвольного числа: избыточная проверка
количества удалена при сохранённых8KiB ответа и deadline. Адресные конечные
accessibility/sanitizer проверки, пакет/установка r2 и реальная приёмка
Chromium завершены, см. итог ниже.

## Итог локального внедрения r2

DEB SHA256: `b66c40b3bb9f6fa79d2f25b976ea712edafd57f013992fc6a04e1b900695d66f`.
Установленный daemon: `c5715fc98a688a239f9ff92cc0b91c5ba3b06c9ec3faa23532b35a3af06c687d`.
Helper: `54a76710321376ffb563e8684db311df6c506520496296b85ae6926860cd3161`.

- `accessible-final-capture.log`: GTK14/14 PASS на конечном helper; tidy без замечаний.
- `deploy-20261020-r2/host-gtk.log`: пять хоткеев, AUTO/recovery, слова пользователя PASS.
- `deploy-20261020-r2/host-chrome.log`: Pause в обе стороны, rich selection,
  selected/current case, transliteration, AUTO/recovery, следующий RU glyph PASS.
- `deploy-20261020-r2/host-word-ending.log`: реальное `rfrb[` → «каких» PASS.
- Root packet `/var/lib/punto-hotkeys-update-20261020-r2`: transaction.result=0,
  свежий quiescent archive, точный предыдущий DEB и отдельный rollback сохранены.
- Все23 immutable payload совпадают. Четыре файла настроек/обучения совпадают
  с cutover archive по bytes, uid/gid, mode, mtime, ACL и xattrs после проверок.
- GNOME Shell, IBus и GDM сохранили PID/start time. Backend/helper/tray active,
  enabled=1, config_result=ok, pending/ready/queued/in-flight=0.

Root packet содержит rollback предыдущего локального r1, не автоматически
вызывает его. Более ранний резерв2.8.14 также сохранён. Перезапуска сеанса нет.
Сложный clipboard на host сохранён, portable-paste cohort на этом host не
заявляется; его проверки выполнены в private suite. Это конечная локальная
приёмка затронутых функций, не проверка всех редакторов и не опубликованный релиз.
DEB содержит документацию, зафиксированную перед приёмкой; этот итог добавлен
после установки без изменения исполняемых файлов или пересборки того же пакета.
Runtime delta:7 файлов,+418/−85, без новых зависимостей; рост — поддержка
native selection, удаление count guard само по себе уменьшило runtime.

Ранние punctuation RED не являются причинным доказательством: одна fixture
передавала пустой набор клавиш, другая не содержала слова в тестовом словаре.
Они не учитываются как дефекты продукта. Успех private suite не является
приёмкой установленного сервиса или обещанием поддержки всех редакторов.

Первая LSan-проверка обнаружила лишний ref при помещении native endpoints в
GVariant proof; финальная проверка выполнена с detect_leaks=1 без suppression.
Широкий Release-прогон выявил две fixture-проблемы:33 слова превышали2-second
fault hold с новым origin-бюджетом; reader xclip мог потребовать STRING во время
асинхронного восстановления clipboard. Переполнение очереди теперь проверяется
на9 словах (больше лимитов1 и5), а ожидание восстановления использует штатный
UTF-8 reader. Точные текст, dispatch, освобождение клавиш и recovery не ослаблены.
Отдельный legacy platform-тест унаследовал IBus settings пользовательского
сеанса; в его private Xvfb input method теперь задан явно и исходное окружение
восстанавливается. Все4 отказавшие группы повторены успешно. Это объединение
валидных результатов, не новый монолитный50/50 прогон. Зависимости не менялись.

Combined architecture/code audit исходного кандидата до registry-поправки: PASS.
Runtime delta того checkpoint:7 файлов,+416/−78;
рост относится к новой поддержке нативных paragraph selections и её границам
безопасности, не к рефакторингу. Новых runtime-библиотек и обходных транспортов нет.
