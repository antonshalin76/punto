# Punto: реализация и внедрение

Дата: 2026-10-17. Рабочий каталог: `/home/anton/scripts/punto`.
Текущее разрешение пользователя, 2026-10-18: **выполнить оставшиеся этапы**,
перед сменой графической сессии подготовить независимое восстановление.
Пользователь подтвердил: документы сохранены, окно выхода готово.
Это заменяет прежнее «только подготовка». Прямой restart GNOME Shell/IBus,
перезагрузка машины и принудительный logout не выполняются: пользователь
выходит и входит вручную после проверки страховки и установки.
Исходный HEAD: `8f8e24a`, ветка `codex/punto-hardening-2.8.6`.
Этот план заменяет очередь действий в `autonomous-stabilization-plan.md`;
старые отчёты и неуспешные попытки сохраняются как история.

## Результат

Одна согласованная версия daemon, session helper, GNOME RU/EN bridge и tray,
установленная в пользовательской системе и проверенная фактическим вводом.
Пользовательские настройки, clipboard, обученные исключения и обычный ввод
сохраняются. Обновление не должно оставлять видимость работающего сервиса,
когда обязательный компонент отсутствует.

Работаем без C2C и agent-collab. Ponytail full задаёт минимальный полный diff;
developer-ultimate объединяет разработку и проверку; codex-thread-workflows
ведёт зависимости и доказательства; crossreview проверяет итоговый пакет
независимыми рецензентами. Несколько скиллов не создают несколько циклов ревью.
GNOME Shell, IBus и машину не перезапускаем. Тестовый ввод — только в выделенные
окна; чужие процессы и документы не затрагиваем.

## Подтверждённая исходная точка

- [x] Проверить рабочую копию и историю. Изменения до начала этого прохода
  сохраняются; незакоммиченная реализация не считается опубликованным релизом.
- [x] Сверить старую приёмку с кодом. В снимке предыдущего installed-прогона
  совпадают137 из140 файлов; изменились только README, RESTORATION и старый план.
  Исторические352 native и220 installed результатов не означают текущую работу
  на host: гостевая машина сейчас выключена.
- [x] Проверить host без изменений: установлен2.8.13, работают три per-device
  daemon-процесса; session helper отсутствует. GNOME-расширение compatibility-v1
  включено, но объект InputSource1 не экспортирует интерфейс нового протокола.
  Несколько daemon для нескольких устройств сами по себе не являются дефектом.

## Охват актуального содержимого

| Поверхность | Что проверяем |
| --- | --- |
| `cpp/include`, `cpp/src`, `cpp/tests` | Ввод, очередь анализа, origin/receipt, редактирование, clipboard, IPC, сессия, tray и native-контракты |
| `gnome`, units, `config.yaml` | Системная RU/EN-раскладка, helper, запуск и настройки |
| `build-deb.sh`, `DEBIAN`, `punto-cli.sh`, `deployment` | Сборка, установка, активация, сохранение данных, восстановление |
| `tests`, `.github` | Реальные GTK/Chromium/VTE-сценарии, dry-контракты и CI |
| `README`, `RESTORATION`, `protocol`, `platform` | Текущий контракт; отделение исторических экспериментов от штатной поставки |
| GTK/Xorg archives, vendor, build/cache/evidence | Инвентаризация и связь с активной сборкой; не переписываем сторонний код и не читаем заново каждый старый лог |

Инвентаризация не является обещанием построчного аудита всех vendor-файлов.
Штатная поставка не требует GTK/editor/browser extension; отдельный GNOME bridge
управления раскладкой сохраняется. Старые native-adapter/platform эксперименты
не превращаем в новый обязательный путь.

## Рабочие этапы и условия закрытия

- [x] **1. Закрыть подтверждённые в этом проходе дефекты реализации и сборки.**
  - [x] Сборка уважает `CMAKE_BUILD_PARALLEL_LEVEL`, по умолчанию использует2
    задания; зависимости builder/CI/docs и проверки пакетов согласованы.
  - [x] Команды запуска учитывают обязательный session helper; отсутствие
    готового desktop bridge не выдаётся за полностью работающий сервис.
    Используем существующие systemd/GDBus/IPC механизмы, без нового супервизора.
  - [x] Исправить воспроизведённые функциональные дефекты из ревью:
    точный systemd ActiveState, ожидание завершения backend и доступ helper
    к session bus без разрешения записи в home.
    Исторический origin-refusal не объявлять исправленным без причинного RED;
    не ослаблять проверки фокуса, identity, clipboard или сроки операции.
- [x] **2. Проверить текущую реализацию в разрешённом private наборе.**
  - [x] Фокусированные RED/GREEN для правок; unit/seam и dry lifecycle suites.
    Конечная таблица systemd состояний проверяется полностью; PBT-библиотеки
    в проекте нет, свой генератор и новую зависимость не добавляли.
  - [x] Release/ASan/UBSan и shell/static для изменённых runtime поверхностей.
    Package gate итогового артефакта отмечается ниже. Запускать только
    инвалидированные проверки. Одна тяжёлая проверка одновременно; build `-j2`,
    MemoryMax2G, SwapMax0, CPUQuota200%, GUI TasksMax256. Пока CTest выполняется,
    не запускать даже `ctest -N` в том же builddir: он перезаписывает журнал.
    Для чтения регистраций использовать готовый `CTestTestfile.cmake`.
  - [x] Практика GTK/Chromium/VTE на точных бинарниках в существующем private
    display/device sandbox либо согласованной disposable VM, не рядом с host
    input chain: Pause RU↔EN,
    Shift+Pause только выделение, следующий символ, повторное переключение,
    Space-auto, inverse/undo/learning, empty→recovery, возврат после простоя.
    Sound открывает свои настройки; Save и ON/OFF проверяются отдельно.
    В каждом сценарии clipboard сохраняется, модификаторы отпущены.
    - [x] Полные четыре generic GTK shards:209/209 PASS,234.00s;
      реальный layout bridge GTK/VTE:14/14 PASS,15.38s. Отрицательные
      контроли фокуса/выделения и undo сохранены; production guards не менялись.
    - [x] Полные Chromium shards:114/114 PASS,313.13s; отдельно real bridge2/2,
      серия100 преобразований и26 начальных отказов PASS. Два последних oracle
      fixes относятся к другим методам; тела real/soak сценариев и их driver
      configuration не менялись. Длительный installed soak220/1802.791s остаётся
      историческим доказательством, не новым host-прогоном.
- [x] **3. Один независимый source review и один локальный кандидат поставки.**
  - [x] Crossreview по сохранённому roster, одинаковый brief для всех;
    каждое замечание проверить по коду. Недоступный провайдер не считается PASS.
    Совмещаем с архитектурным аудитом, без вложенных review-кампаний.
  - [x] Собрать итоговый DEB-кандидат2.8.14 после исправления runtime;
    сохранить dirty-inclusive snapshot и input manifest, записать хеши,
    relevant source inputs и пригодность старых доказательств. Проверить именно
    этот пакет, не повторять уже покрытые дочерние проверки без изменения входов.
- [x] **4. Внедрить в пользовательский сервис с восстановлением.**
  - [x] Подготовить точный резервный DEB2.8.13 и отдельный private архив данных
    и metadata. Сравнить три установленных executable побайтно; проверить
    isolated upgrade2.8.13→2.8.14→rollback2.8.13:13/13 PASS, config/learning
    сохранены. Это подготовка, не установка на host. Перед будущим внедрением
    обновить архив после остановки старых writers; текущий снимок не считать
    навсегда согласованным с продолжающей работать системой.
  - [x] Сохранить точные установленные файлы/пакет, конфигурацию, права,
    обученные данные, units и per-device topology до изменений.
    Host-пакет восстановления — точный установленный2.8.13 и отдельный архив
    conffiles/learning/metadata под `.cache/punto-product-20261017.WqYT4f/host/`.
    Перед изменениями проверить содержимое, хеши и восстановление на isolated
    lane. UUID-провайдер гостя и orchestrator, принимающий rollback только2.8.5,
    для этого host не подходят; их ограничения не снимать.
    - [x] После остановки всех11 pinned writers создан свежий root-only
      `/var/lib/punto-recovery-20261018/cutover-state.tar`, SHA256
      `027d6607cfaa4e1497f8d16801c56380c5ad09cb39ea66c3279990276d80b67e`.
      Exactinactive/nojob и отсутствие фактических writers подтверждены.
      Root recovery probe исполняется в `/system.slice`, не зависит от VSCode.
  - [x] Проверить доступный безопасный путь загрузки GNOME bridge. До его
    подтверждения и согласованного окна сохранить установленный2.8.13 без
    замены файлов/запуска новой input chain. Если нужна
    новая графическая сессия, согласовать окно с пользователем; не делать
    принудительный logout, GNOME/IBus restart или обход через другой UUID.
  - [x] До замены байтов остановить только принятые старые Punto writers,
    исключить их автоматическое возрождение и подтвердить исчезновение всех
    сохранённых PID/start identities. Так же останавливать candidate перед
    откатом. Канал восстановления — текущий agent shell, без зависимости от
    работоспособности перехватываемой клавиатуры.
  - [x] Установить принятые байты; запустить helper, backend и tray; подтвердить
    ожидаемый InputSource1/DesktopObserver1, live ELF и критические действия
    host: Pause в обе стороны, только выделение, следующий символ, auto,
    empty→recovery, clipboard и освобождение модификаторов. После успеха сверить
    config/learning bytes и metadata, topology и существующий XDG autostart.
    Использовать штатные confdef/confold и defaults, не новый config migration.
    - [x] Первая установка: кандидат250718dd установлен отдельной root-unit: exit0,
      `ii :2.8.14`,12 immutable payload paths совпадают. Конфигурация,
      learning/reset сохранили байты, uid/gid/mode/mtime. Backend runtime-masked,
      writers отсутствуют; GNOME8956/IBus9075 не перезапускались.
    - [x] Повторная установка r2: свежий quiescent резерв, отдельные root units
      и markers; кандидат250718dd установлен,12 payload paths PASS. После
      замены defaults в dpkg системный config восстановлен из свежего резерва
      под обеими блокировками; все4 config/learning bytes+metadata PASS.
      Backend выключен, новая страховка r2 активна до22:44:25.
    - [x] Новый вход и host-приёмка: session11 X11, typed readiness0,12 payload
      hashes, GTK/Chrome manual обе стороны/selection/nextglyph/empty-recovery/
      autoSpace/clipboard и modifiers PASS. Live Sound открывает sound-only.
      Для непереносимых Chromium clipboard targets проверен документированный
      отказ Tab; положительный Tab на host не заявляется, private PASS сохранён.
      Физические нажатия человека и длительный idle не проверялись. Ввод шёл в
      один существующий daemon pipe через production uinput, не новую chain.
      Независимый postflight и финальные4 bytes+metadata PASS; root --accept0,
      timer disabled/inactive, backend/helper/tray работают. Первая попытка
      остаётся в истории: в18:49:04 страховка вернула2.8.13
      до приёмки. После reboot22:04 и входа anton X11 загружен compatibility-v1,
      helper отсутствует, baseline backend/tray работают. Runtime-mask не
      переживает reboot. Перед новой установкой нужны свежий согласованный
      резерв и отдельный recovery attempt: старый `restored` делает rescue
      завершённой попытки no-op. Нужна готовность выйти/войти сразу после
      установки; backend кандидата не запускать до typed readiness.
  - [x] Первая попытка: автоматическое восстановление подтверждено journal,
    marker и хешами трёх установленных2.8.13 executable. Старый timer отключён,
    резерв сохранён. UNKNOWN не наблюдался.
  - [x] Политика восстановления применена: r1 exact rollback успешен; r2
    known-success dpkg и замена defaults сверены под locks, свежий config
    восстановлен без повтора dpkg. Для будущего внедрения при UNKNOWN
    сначала сверить jobs/locks, unit/cgroup и PID/start identities; никаких
    второго dpkg или rollback, пока предыдущий писатель/transaction не остановлен
    положительно. Если это не доказано — сохранить backup и остановить внедрение.
- [ ] **5. Закрыть поставку.**
  - [ ] Закрыть обнаруженный pre-push review дефект stale PRIMARY: команды
    выделения без фактического диапазона не записывают текст. Проверить
    refusal→Pause→новое выделение, Unicode/paste, clipboard и модификаторы
    в GTK/Chromium; обновить точный пакет и установленный runtime без перезапуска
    GNOME/IBus. Прежняя host-приёмка остаётся историческим снимком, не
    подтверждением исправленного артефакта.
  - [ ] Обновить контракт и release notes; выполнить обязательный pre-push
    no-mistakes-daily, если публикуется ветка/PR; commit/push только принятого scope.
  - [ ] Если выполняется исходно запрошенный релиз — подтвердить издателя
    `antonshalin76`, опубликовать точный артефакт и прочитать результат обратно.
    Публикация остаётся обязательной по исходному запросу. Локальная сборка,
    установка и публикация отмечаются раздельно. Публиковать только из принятого
    commit/tag с привязкой к проверенному dirty-inclusive source и хешу DEB;
    раннее review плана не является review финального кода.
  - [ ] Финальная сверка: все обязательные пункты закрыты доказательствами,
    рабочий сервис проверен на host, временные собственные процессы завершены,
    остаточные ограничения названы. Только после этого план закрыт полностью.

## Порядок и контроль отклонения

`актуальный код → исправления → необходимые проверки → review/DEB → host → поставка`.
Crossreview работает параллельно независимой подготовке; одна общая интеграция.
Следующая операция должна закрывать пункт выше. Новые harness, monitoring stack,
FSM/SDK, adapter или переписывание архитектуры без конкретного дефекта не входят
в этот проход. Проверки ограничены стоимостью и сохраняют точные failed outputs.

Локальный граф: `.cache/punto-product-20261017.WqYT4f/run.json`.
Подготовка после сужения разрешений пользователем:
`.cache/punto-product-20261017.WqYT4f/continue-20261018/run-preparation.json`.
Он продолжает тот же task ID и закрывает только подготовку, не исходную поставку.
История: тред `01a06b8c-4493-7852-87ff-f6c61e0dca93`; восстановление частичное,
исторические assistant-отчёты не заменяют текущую проверку. Старый C2C task ID
`c2c_bfdc`/i7/r52 — только provenance, новый INIT не отправляется.

## Дефекты этого прохода и review

| Случай | Решение и доказательство |
| --- | --- |
| Неограниченный nproc и пропуски build dependencies | Исправлено существующим CMake env и списками; focused fake-builder RED5→GREEN12 |
| Устаревший maintainer fixture | Защищённые private mount ancestors без ослабления postinst; RED13+error→GREEN20 |
| PTY reader не импортируется при запуске из корня | Popen cwd исправлен; причинный ModuleNotFoundError→GREEN из обоих каталогов; все431 recovery tests PASS |
| Helper не входит в текущий CLI lifecycle | Исправлено: typed ReadState до backend mutation; helper stop только после exact inactive backend |
| systemctl is-active rc3 принят за остановку | Исправлено общим exact ActiveState; переходы и failed требуют команды, unknown отклоняется; causal RED→GREEN30 CLI tests |
| ProtectHome=true скрывает session bus | Live readonly transient unit RED permission denied→GREEN при read-only; runtime/state остаются writable, home config read-only; CMake installed payload RED1→GREEN1 |
| Shell DEB lifecycle fixture не содержит /usr/bin/ls | Общий fixture использует существующие coreutils/защищённые ancestors; тот же DEB RED107/29→GREEN107/0, production guards не менялись |
| Generic GTK synthetic layout route | Причинный RED: shortcut grab создаёт GTK/AT-SPI focus-out/in и отзывает token. Только legacy fixture фильтрует core/XI2 NotifyGrab/Ungrab; Normal focus и production guards не меняются.8/8 focused и полные209/209 PASS, включая отключение фильтра→реальный безопасный отказ. Papercut pc_09e32b63652d закрыт по проверке |
| Два real-bridge сценария ошибочно включены в generic shards | Перенесены существующим правилом именования в canonical real-layout CTest без skips и удаления assertions.14/14 PASS,15.38s |
| Undo negative ожидал отказ при разрешённом native fallback | Raw XKB mismatch не отзывает полномочия того же editor. Fixture теперь заменяет child в том же окне: inverse и fallback действительно отклоняются;3/3 focused PASS, backspaces и fresh correction проверяются |
| Chromium real-only cases и размер generic shards | Два сценария перенесены к реальному driver с обновлением exact-name setup; generic разбит существующим foreach на4 serial части.114 уникальных случаев распределены29/29/28/28, все PASS; лимиты продукта не увеличивались |
| Chromium oracle зависел от позднего refusal stage и условного debug log | Допущен существующий более ранний editor_context отказ; навигация ожидается по фактическому Right release. Fresh DOM, текст, clipboard, dispatch/history и восстановление сохранены; полный набор PASS |
| Исторический first-auto origin refusal | Причина не подтверждена; не ослаблять guards; классифицировать по существующему сценарию, не объявлять causal fix |

Crossreview плана:3/3 ответа, approve with changes; обязательные поправки
внесены сюда. Отчёт: `.cache/punto-product-20261017.WqYT4f/crossreview-plan/orchestrator-report.md`.
Выполненные и отклонённые советы перечислены там. Новые cosmetic советы не
открывают код заново; проверенный correctness/data-loss дефект открывает только
свой owner. Финальный код/артефакт и host ещё не приняты этим review.

## Текущие доказательства, 17 октября

| Проверка | Результат и граница |
| --- | --- |
| Recovery из корня репозитория |431/431 PASS после исправления cwd дочернего PTY reader |
| Maintainer scripts в private mounts |20/20 PASS; production trust checks сохранены |
| Builder contract |12/12 PASS, включая default2 и caller override1; это не реальная сборка DEB |
| CLI после state-поправки |Python30/30 PASS; shell5315/5315 PASS перед однословным добавлением failed в существующую command-required ветку; её новые случаи проверены Python, полный shell повторно не заявляется |
| Native helper contracts |Release3/3 и ASan/UBSan3/3 PASS, detect_leaks=1; реальный CLI+native probe включён в observer contract |
| GTK/VTE с реальным layout bridge |Канонический CTest расширен существующими сценариями:12/12 PASS,13.50s; selection, repeated RU/EN, undo, empty→recovery, PTY/auto/selection |
| Chromium private display |2/2 PASS,7.900s: owned browser reopen и физический Tab→следующее поле |
| Config, tray IPC/app, clipboard |4/4 CTest PASS,11.47s; реальный sound-only dialog и persistence проверяются tray app contract |
| Strict clang-tidy helper TU |PASS;248 warnings из сторонних headers подавлены штатным clang-tidy, не продуктовые ошибки |
| Host отрицательный контроль |Новый CLI при отсутствующем helper:ERROR bridge-unavailable; старые Punto/GNOME/IBus identities неизменны |
| Изолированный dpkg lifecycle |Итоговый DEB250718dd:107/107 PASS; helper unit/CLI извлечены и побайтно совпадают с исходниками; negative probe извлечённого helper возвращает75 |

Журналы текущего прохода:
`.cache/punto-product-20261017.WqYT4f/` и
`.cache/punto-cli-bridge-contract-20261017.log`.

Продолжение18 октября завершает разрешённую подготовку: GTK209/209,
реальный GTK/VTE bridge14/14, Chromium114/114 плюс real bridge2/2 и серии100/26;
package rollback13/13 и извлечение архива с ACL/xattrs/owner/mode PASS.
Все эти GUI проверки выполнены в private sandbox, не на установленном host.
Команды завершились с exit0. Для Chromium сохранены terminal4/4 PASS и
source-level учёт114 методов; подробный CTest log был перезаписан ошибочным
параллельным `ctest -N`. Пустой successor log не выдаётся за per-case evidence;
неуспешный исходный пакет и его traceback сохранены. Приёмка не повторяется
только ради восстановления журнала. Minor tooling friction pc_f65bc119b642 открыт.
Независимые текущие отчёты: `continue-20261018/fixture-audit.md` и
`continue-20261018/recovery-audit.md` внутри каталога задачи.
Итоговый кандидат:
`.cache/punto-product-20261017.WqYT4f/source-final/punto-switcher_2.8.14_amd64.deb`,
SHA256 `250718dd0571fd825b76342f093479e81e6dae6149d0e829fd4d37c30b54b0e7`.
Dirty-inclusive snapshot сборки содержит109 файлов. На момент сборки все109
совпадали с checkout; последующие изменения README/RESTORATION и
регистрации тестов внутри BUILD_TESTING в CMake — отдельный successor проверки,
не изменение runtime кандидата. GTK fixture также не входит в пакет.
Не утверждаем, что полный текущий checkout побайтно равен старому архиву.
Архив `source-final-inputs.tar.gz`, SHA256
`405b318ada1ca40c0e74ace83d63de69aca84bd4b671d9cec6a7bfe86fec99b1`.
Первый DEB6c5a07 и его исходный архив сохранены, но заменённый unit делает его
непригодным как итоговый кандидат. После исправления одного unit-параметра
пересобран именно новый кандидат; повторная сборка не была косметической.

Финальный ограниченный source audit принял CLI/helper/unit/build/fixture batch.
Три reviewer ответили; Grok сообщил об усечении контекста, поэтому полное
одинаковое чтение всех файлов им не утверждается. Проверяемые замечания
сверены с исходниками и причинными тестами. Отчёт:
`.cache/punto-product-20261017.WqYT4f/crossreview-code/orchestrator-report.md`.
Новых production-файлов, зависимостей и runtime frameworks в этом проходе нет;
добавленный runtime отвечает за обязательную readiness/state correctness,
а не за рефакторинг ради рефакторинга.

Эта таблица сама по себе не закрывает полную продуктовую приёмку, host и
публикацию. Подготовительный этап закрыт; новое разрешение пользователя
открывает внедрение с независимым восстановлением. Журналы подготовки18 октября:
`.cache/punto-product-20261017.WqYT4f/continue-20261018/`.
GitHub API сейчас подтверждает `anton-shalin`, не разрешённого издателя
`antonshalin76`; целевой аккаунт в gh storage отсутствует. Глобальную
авторизацию других задач не меняли, публикацию от другого лица не выполняли.
