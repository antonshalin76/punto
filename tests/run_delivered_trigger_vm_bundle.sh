#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build="$root/cpp/build-c2c-bfdc-g1p"
driver="$build/punto-delivered-trigger-driver"
probe="$build/punto-delivered-trigger-probe"
adapter="$build/prototype/gtk-3.0/modules/libpunto-delivered-trigger-gtk-adapter.so"
im="$build/prototype/immodules/im-punto-delayed-commit.so"
tool_dir="$build/prototype/bin"
result_dir=${PUNTO_PROTO_RESULT_DIR:-"$root/results"}
private_test="$root/tests/test_editor_delivered_trigger_vm.py"
base_test="$root/tests/test_event_loop_gtk_e2e.py"
common=(PUNTO_REQUIRE_EVENT_LOOP_E2E=1 PUNTO_DELIVERED_PROBE="$probe"
        PUNTO_PROTO_TOOL_DIR="$tool_dir" PATH="$tool_dir:/usr/bin:/bin")

if [[ ${PUNTO_PROTO_ALLOW_LOCAL_REHEARSAL:-0} != 1 && $(hostname) != punto-r23 ]]; then
  printf 'private prototype runner requires disposable punto-r23 VM\n' >&2
  exit 2
fi
if [[ $result_dir != "$root/"* ]]; then
  printf 'private result directory must remain inside the visible bundle\n' >&2
  exit 2
fi

for file in "$driver" "$probe" "$adapter" "$im" "$tool_dir/Xvfb" \
            "$private_test" "$base_test"; do
  test -f "$file" || { printf 'MISSING %s\n' "$file" >&2; exit 2; }
done

run_private() {
  local case_id=$1
  shift
  env "${common[@]}" GTK_MODULES="$adapter" \
      PUNTO_DELIVERED_CASE="$case_id" "$@" \
      /usr/bin/python3 "$private_test" "$driver"
}

run_base() {
  local test_name=$1
  env "${common[@]}" GTK_MODULES="$adapter" \
      PUNTO_EVENT_LOOP_E2E_TEST="$test_name" \
      /usr/bin/python3 "$base_test" "$driver"
}

if [[ ${1:-} == --case ]]; then
  test $# -eq 2 || exit 2
  case "$2" in
    entry|early|origin|receiver_death) run_private "$2" ;;
    absent_receiver)
      env -u GTK_MODULES "${common[@]}" PUNTO_DELIVERED_CASE=absent_receiver \
        /usr/bin/python3 "$private_test" "$driver" ;;
    installed_im) run_private installed_im GTK_IM_MODULE=cedilla ;;
    delayed)
      run_private delayed GTK_IM_MODULE=punto-delayed-commit \
        GTK_IM_MODULE_FILE="$result_dir/immodules.cache" \
        PUNTO_PROTO_IM_DELAY_MS=300 ;;
    late)
      run_private late GTK_IM_MODULE=punto-delayed-commit \
        GTK_IM_MODULE_FILE="$result_dir/immodules.cache" \
        PUNTO_PROTO_IM_DELAY_MS=650 ;;
    tab_entry) run_base test_auto_entry_tab_corrects_before_focus_transfer ;;
    tab_textview) run_base test_auto_word_layout_preserves_literal_textview_tab ;;
    tab_nested) run_base test_auto_entry_tab_nested_delete_preserves_following_input ;;
    *) printf 'unknown private slot %s\n' "$2" >&2; exit 2 ;;
  esac
  exit 0
fi

if [[ ${1:-} == --p12-inner ]]; then
  run_private protocol
  run_base test_auto_tab_repeated_corrections_preserve_release_and_next_input
  run_base test_auto_tab_pre_ready_saturation_preserves_input
  exit 0
fi

test $# -eq 0 || { printf 'unexpected argument\n' >&2; exit 2; }
mkdir -p "$result_dir"
query=/usr/lib/x86_64-linux-gnu/libgtk-3-0t64/gtk-query-immodules-3.0
test -x "$query" || { printf 'missing GTK query binary\n' >&2; exit 2; }
"$query" "$im" > "$result_dir/immodules.cache"
grep -q 'punto-delayed-commit' "$result_dir/immodules.cache"

run_slot() {
  local slot=$1
  shift
  printf '%s START %s\n' "$slot" "$(date -u +%FT%TZ)" | tee -a "$result_dir/status.txt"
  if timeout --signal=TERM --kill-after=2s 30s "$@" > "$result_dir/$slot.log" 2>&1; then
    printf '%s PASS %s\n' "$slot" "$(date -u +%FT%TZ)" | tee -a "$result_dir/status.txt"
  else
    local status=$?
    printf '%s FAIL exit=%s %s\n' "$slot" "$status" "$(date -u +%FT%TZ)" | tee -a "$result_dir/status.txt"
    return "$status"
  fi
}

{
  date -u +%FT%TZ
  dpkg-query -W -f='punto-switcher ${Version} ${Status}\n' punto-switcher
  systemctl is-active udevmon
} > "$result_dir/guest-before.txt"

closeout() {
  local status=$?
  {
    date -u +%FT%TZ
    dpkg-query -W -f='punto-switcher ${Version} ${Status}\n' punto-switcher
    systemctl is-active udevmon
    printf 'runner_exit=%s\n' "$status"
  } > "$result_dir/guest-after.txt"
}
trap closeout EXIT

run_slot P01 /bin/bash "$0" --case entry
run_slot P02 /bin/bash "$0" --case early
run_slot P03 /bin/bash "$0" --case origin
run_slot P04 /bin/bash "$0" --case absent_receiver
run_slot P05 /bin/bash "$0" --case receiver_death
run_slot P06 /bin/bash "$0" --case installed_im
run_slot P07 /bin/bash "$0" --case delayed
run_slot P08 /bin/bash "$0" --case late
run_slot P09 /bin/bash "$0" --case tab_entry
run_slot P10 /bin/bash "$0" --case tab_textview
run_slot P11 /bin/bash "$0" --case tab_nested
run_slot P12 /bin/bash "$0" --p12-inner
