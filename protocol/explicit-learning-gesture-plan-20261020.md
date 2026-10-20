# Явное обучение исключениям

- [x] Очистить старые12 исключений штатным reset с durable readback; резерв сохранён.
- [x] Согласовать жест: удерживать Ctrl+Shift, два отдельных Backspace с отпусканием.
- [x] Причинный RED: обычные три Backspace доставлены редактору, но ошибочно создают исключение.
- [x] Удалить неявное обучение Backspace/Ctrl+Z; владелец политики — EventLoop,
  прежний UndoDetector хранит только явно принятые исключения.
- [x] Два press в прежнем2.5-секундном окне последней успешной AUTO-коррекции;
  свежий Child proof перед обоими. Только успешный exact inverse разрешает Add.
  Незавершённый жест не воспроизводится; проглоченный press сохраняет свой release.
- [x] Unit/native GTK/Chromium: явный жест, ordinary undo, repeats, отмена,
  focus/child, срок, модификаторы, clipboard, сохранение и повторный запуск.
- [x] Один итоговый review и affected sanitizer/static gates.
- [x] Пакет/внедрение только Punto с точным rollback; host scratch, без GNOME/IBus restart.

Вне разрешённого контекста комбинация передаётся приложению. Гарантия отсутствия
такой команды во всех приложениях не даётся. Существующий временный Ctrl+Z undo
сохраняется, но никогда не обучает. По нажатию второго Backspace слово возвращается
в исходный вид; сохранность clipboard, token/range и отказов не ослабляется.

Release-проверки: secure-store contract PASS; GTK32/32 PASS(50.906s),
Chromium3/3 PASS(7.908s), shared input/hotkeys/context14/14 PASS(14.603s).
Четыре независимых случая проверяют левый/правый Ctrl/Shift; отдельные sequences
проверяют autorepeat, duplicate keydown, отпускание модификатора и истечение
срока между нажатиями. Store и native tests используют прежние runners,
без новой property-библиотеки или собственного генератора.
Итоговый независимый source review PASS, EventLoop SHA256
`d40a884233d5f99eb9596e9364e871ec0e2d97d19f2962f7d13d7f3d173c5a0b`.
Clang-format, py_compile, diff-check и affected-owner clang-tidy PASS.

Первоначальные неуспешные GTK/Chromium журналы сохранены. Исправлены захват
после focus drift и допуск до clipboard cleanup с отложенным inverse.
Chromium negative дополнительно проверяет настоящий удерживаемый chord и
первый поглощённый press до смены child; прежний fixture вызывал full press
вместо modifier-down. Старые запуски не переименовываются в успешные.

## Финальная валидация и внедрение

- ASan/LSan/UBSan: secure-store contract PASS(25 top-level checks), GTK32/32
  PASS56.063s, Chromium3/3 PASS9.885s; нет sanitizer errors/skips.
  `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`, `UBSAN_OPTIONS=halt_on_error=1`.
  Эти проверки выполнены на замороженных владельцах EventLoop/UndoDetector;
  последующий VERSION bump меняет только метаданные сборки.
- Пакет2.8.17 SHA256
  `6ef20f5be3fefdfd957de6b667a33ed558e1efbc392683fbdc1e32f1984f033a`;
 107/107 lifecycle PASS. Lintian: три прежних no-manual-page и одно changelog
  warning; ошибок нет. Payload GNOME/protocol побайтно прежний.
- Root recovery `/var/lib/punto-learning-update-20261020-2817`: точный архив
 2.8.16 SHA256b73fa488d7b84a162fcc842187d9a41773aa43124e2bcd9af8fee6e4b266a39a,
  свежий quiescent backup четырёх config/learning файлов; его список исключений
  пуст. Transaction receipt0,23 immutable paths и bytes/metadata mutable files
  проверены. Только Punto units остановлены/запущены.
- Installed primary3369950/start9414810/stdin`pipe:[43157829]`, daemon SHA256
  `7c70c5057124929aa29a0e5af6ac2326faf97febc93a66f98f12df34185302a8`.
  Typed readiness ready/on. Host owned GTK: три обычных Backspace удаляют три
  символа без обучения; Ctrl+Z возвращает слово без обучения; явный жест
  возвращает слово и сохраняет исключение; исключение действует, Pause всё
  ещё исправляет слово, следующий символ RU, clipboard и modifiers сохранены.
  Тестовое исключение удалено штатным reset; финальный список пуст.
- GNOME8042/start3987, IBus8170/start4052 и8342/start4055, GDM3429/start1131
  не перезапускались. Непереносимые host clipboard targets сохранены;
  portable-paste cohort по этому host-прогону не заявляется.
- Дополнительная one-off shell-проверка версии при активации имела quoting error;
  отдельный positive Python validator подтвердил exact installed status,
  transaction0,23paths и desktop identities. Ошибка не скрыта в журналах.

Журналы рядом с source: `explicit-learning-{ordinary-red,gtk-green,
chromium-final,input-regression,asan-gtk,asan-chromium,asan-unit,tidy,
package-build,package-lifecycle,package-lintian}.log` и
`deploy-learning-2817/{install,activation,post-install-check,host-learning}.log`.
Объём этой фичи закрыт локально. Этот checkpoint предшествует публикации;
GitHub refs и CI проверяются отдельно. Проверки нового жеста во всех
существующих приложениях не заявляются.
