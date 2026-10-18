#!/bin/bash
set -euo pipefail

preflight=${1:?preflight helper required}
legacy=${2:?exact legacy archive required}
candidate=${3:?candidate archive required}
orchestrator=${4:?activation orchestrator required}
files_helper=${5:?exact file helper required}
python3 - "$preflight" "$files_helper" "$0" "$legacy" "$candidate" <<'PY'
import io, pathlib, re, subprocess, sys, tarfile
preflight, files, fixture = [pathlib.Path(p).read_text() for p in sys.argv[1:4]]
inventories = []
for archive in sys.argv[4:]:
    raw = subprocess.run(['dpkg-deb', '--fsys-tarfile', archive], check=True,
                         stdout=subprocess.PIPE, timeout=5).stdout
    with tarfile.open(fileobj=io.BytesIO(raw), mode='r:') as payload:
        inventories.append({m.name.removeprefix('./'): m for m in payload.getmembers()})
legacy, candidate = inventories
preflight_overlay = {'usr/bin/punto-daemon': 0o755,
           'usr/libexec/punto-session-bridge': 0o755,
           'usr/share/punto-switcher/protocol/punto-dbus-v1.xml': 0o644,
           'etc/xdg/autostart/punto-session-bridge.desktop': 0o644}
overlay = {**preflight_overlay, 'usr/lib/systemd/user/punto-session-bridge.service': 0o644}
checks = {}
for name, mode in overlay.items():
    item = candidate.get(name)
    checks['candidate regular root-owned ' + name] = bool(item and item.isfile() and
        item.uid == item.gid == 0 and item.mode == mode)
for name in ('usr/local/bin/punto-daemon', 'usr/local/bin/punto-tray',
             'usr/share/punto-switcher/config.yaml', 'usr/share/punto-switcher/udevmon.yaml',
             'etc/xdg/autostart/punto-tray.desktop'):
    item = legacy.get(name)
    checks['legacy regular root-owned ' + name] = bool(item and item.isfile() and item.uid == item.gid == 0)
checks['legacy has no candidate helper or protocol'] = not any(name in legacy for name in overlay)
for name in ('etc/punto/config.yaml', 'usr/share/doc/punto-switcher/examples/udevmon.yaml',
             'etc/xdg/autostart/punto-tray.desktop'):
    item = candidate.get(name)
    checks['candidate regular root-owned ' + name] = bool(item and item.isfile() and item.uid == item.gid == 0)
admission = preflight.split('if [ "$mode" != upgrade ]; then', 1)[1].split('\nfi', 1)[0]
admitted = set(re.findall(r'"\$root/([^"\s]+)"', admission))
checks['preflight original overlay equals actual candidate inventory'] = admitted == set(preflight_overlay)
loops = re.findall(r'for path in (.*?)\s*; do', files, re.S)
checks['both snapshot and restore match actual candidate inventory'] = len(loops) == 2 and all(
    set(block.replace('\\\n', ' ').split()) == set(overlay) for block in loops)
transport = re.search(r'^cat >"\$work/lifecycle-transaction" <<\x27SH\x27\n(.*?)^SH$',
                      fixture, re.M | re.S).group(1)
checks['lifecycle transport preserves existing directory metadata'] = (
    'dpkg-deb --fsys-tarfile "$2"' in transport and '--no-overwrite-dir' in transport and
    'dpkg-deb -x "$2"' not in transport)
for name, ok in checks.items():
    print(('PASS ' if ok else 'FAIL ') + name)
assert all(checks.values()), 'P3 actual archive/overlay contract differs'
PY
if [[ ${PUNTO_EXACT285_ARTIFACT_ONLY:-0} == 1 ]]; then exit 0; fi
if [[ ${PUNTO_EXACT285_TEST_INNER:-0} != 1 ]]; then
    exec bwrap --ro-bind / / --dev /dev --proc /proc --bind /tmp /tmp \
        --unshare-user --unshare-pid --unshare-net --uid 0 --gid 0 \
        --new-session --setenv PUNTO_EXACT285_TEST_INNER 1 \
        /bin/bash "$0" "$preflight" "$legacy" "$candidate" \
        "$orchestrator" "$files_helper"
fi

work=$(mktemp -d)
finish_fixture() {
    local primary=$? log
    trap - EXIT
    if (( primary != 0 )); then
        for log in "$work"/*.log; do
            [[ -f $log && ! -L $log ]] || continue
            printf '\nEXACT285_FAILURE_LOG %s\n' "${log##*/}" >&2 || :
            tail -c 16384 -- "$log" >&2 || :
        done
    fi
    rm -rf -- "$work" || :
    exit "$primary"
}
trap finish_fixture EXIT
root=$work/root
dpkg-deb -x "$legacy" "$root"
dpkg-deb -e "$legacy" "$work/control"
chmod 0755 "$root" "$root/etc" "$root/etc/xdg" "$root/etc/xdg/autostart" \
    "$root/usr" "$root/usr/local" "$root/usr/share"
mkdir -p "$root/var/lib/dpkg/info" "$root/etc/interception"
chmod 0755 "$root/etc/interception"
printf 'Package: punto-switcher\nStatus: install ok installed\nVersion: 2.8.5\nArchitecture: amd64\n' \
    >"$root/var/lib/dpkg/status"
for script in postinst prerm postrm; do
    if [[ -f $work/control/$script ]]; then
        cp "$work/control/$script" "$root/var/lib/dpkg/info/punto-switcher.$script"
    fi
done
cp "$root/etc/punto/config.yaml.new" "$root/etc/punto/config.yaml"
cp "$root/usr/share/punto-switcher/udevmon.yaml" \
    "$root/etc/interception/udevmon.yaml"

sh "$preflight" "$legacy" "$candidate" "$root" >"$work/positive.log"
grep -qx 'EXACT285_PREFLIGHT_OK job=legacy tray=legacy mode=upgrade' "$work/positive.log"

job=$root/etc/interception/udevmon.yaml
cp "$job" "$work/legacy-job"
printf 'foreign pipeline\n' >"$job"
if sh "$preflight" "$legacy" "$candidate" "$root" >"$work/foreign.log" 2>&1; then
    echo 'foreign job passed preflight' >&2
    exit 1
fi
grep -q 'foreign job or tray' "$work/foreign.log"
printf '#!/bin/sh\ntouch "$PUNTO_TEST_TRANSACTION_MARKER"\n' \
    >"$work/transaction"
printf '#!/bin/sh\nexit 0\n' >"$work/activate"
chmod 0700 "$work/transaction" "$work/activate"
if PUNTO_TEST_TRANSACTION_MARKER="$work/transaction-started" \
    "$orchestrator" "$work/transaction" "$candidate" "$legacy" \
    "$work/activate" "--root=$root" "--admindir=$root/var/lib/dpkg" \
    >"$work/orchestrator.log" 2>&1; then
    echo 'orchestrator admitted foreign job' >&2
    exit 1
fi
[[ ! -e $work/transaction-started ]]
grep -q 'foreign job or tray' "$work/orchestrator.log"
cp "$work/legacy-job" "$job"

tray=$root/etc/xdg/autostart/punto-tray.desktop
mv "$tray" "$work/legacy-tray"
ln -s "$work/legacy-tray" "$tray"
if sh "$preflight" "$legacy" "$candidate" "$root" >"$work/link.log" 2>&1; then
    echo 'symlinked tray passed preflight' >&2
    exit 1
fi
grep -q 'owned file is unsafe' "$work/link.log"
rm "$tray"
cp "$work/legacy-tray" "$tray"

ln "$root/etc/punto/config.yaml" "$work/config-link"
if sh "$preflight" "$legacy" "$candidate" "$root" >"$work/hardlink.log" 2>&1; then
    echo 'hardlinked config passed preflight' >&2
    exit 1
fi
grep -q 'owned file metadata is unsafe' "$work/hardlink.log"
rm "$work/config-link"

printf '#!/bin/sh\nexit 42\n' >"$work/activate-fail"
chmod 0700 "$work/activate-fail"
set +e
PUNTO_TEST_TRANSACTION_MARKER="$work/transaction-started" \
    "$orchestrator" "$work/transaction" "$candidate" "$legacy" \
    "$work/activate-fail" "--root=$root" \
    "--admindir=$root/var/lib/dpkg" >"$work/rollback.log" 2>&1
rollback_status=$?
set -e
(( rollback_status == 42 ))
grep -q 'package_restored=1 files_restored=1' "$work/rollback.log"
[[ $(sha256sum "$job" | cut -d' ' -f1) == \
    50161d1bae210ec039183748fe05ba2ec5116ccd2498b2a9c5a82b05bafda468 ]]
[[ $(sha256sum "$tray" | cut -d' ' -f1) == \
    a43f8a5eae11059515a6064a041a963fa42e27dca97ec4b77f871fdb1d99f934 ]]

printf 'user-choice=true\n' >>"$root/etc/punto/config.yaml"
config_hash=$(sha256sum "$root/etc/punto/config.yaml" | cut -d' ' -f1)
snapshot=$(sh "$files_helper" snapshot "$root" "$legacy" "$candidate" auto)
[[ -d $snapshot && $(stat -c '%u:%g:%a' "$snapshot") == 0:0:700 ]]
sh "$files_helper" migrate "$root" "$legacy" "$candidate" "$snapshot" \
    >"$work/migrate.log"
grep -q 'EXACT285_MIGRATED' "$work/migrate.log"
[[ $(sha256sum "$job" | cut -d' ' -f1) == \
    185a49a3860ddd73fbcbbb8b0bd8fdd9723f592306a8bf20adb1b3a6267c1b51 ]]
[[ $(sha256sum "$tray" | cut -d' ' -f1) == \
    39d534efa9069bb54c5ae11879fdc6a91f6cb4a5ce53f9caa66d10181b78cee5 ]]
sh "$files_helper" migrate "$root" "$legacy" "$candidate" "$snapshot" \
    >"$work/migrate-again.log"
cp "$root/usr/share/punto-switcher/config.yaml" \
    "$root/etc/punto/config.yaml"
sh "$files_helper" restore "$root" "$legacy" "$candidate" "$snapshot" \
    >"$work/restore.log"
grep -q 'EXACT285_RESTORED' "$work/restore.log"
[[ $(sha256sum "$root/etc/punto/config.yaml" | cut -d' ' -f1) == "$config_hash" ]]
[[ $(sha256sum "$job" | cut -d' ' -f1) == \
    50161d1bae210ec039183748fe05ba2ec5116ccd2498b2a9c5a82b05bafda468 ]]
[[ $(sha256sum "$tray" | cut -d' ' -f1) == \
    a43f8a5eae11059515a6064a041a963fa42e27dca97ec4b77f871fdb1d99f934 ]]

# Exercise the actual lifecycle owners with archive-backed transport in the
# private fixture. This does not certify maintscript execution or native input.
dpkg-deb -x "$candidate" "$work/current"
dpkg-deb -e "$candidate" "$work/current-control"
[[ $(dpkg-deb -f "$candidate" Version) == 2.8.14 ]]
current_setup() {
    cp -a "$work/current/." "$root/"
    chmod 0755 "$root/usr/local"
    cp "$work/current/usr/share/doc/punto-switcher/examples/udevmon.yaml" "$job"
    printf 'user-choice=true\n' >>"$root/etc/punto/config.yaml"
    for script in postinst prerm postrm; do
        rm -f "$root/var/lib/dpkg/info/punto-switcher.$script"
        if [[ -f $work/current-control/$script ]]; then
            cp "$work/current-control/$script" "$root/var/lib/dpkg/info/punto-switcher.$script"
        fi
    done
    printf 'Package: punto-switcher\nStatus: install ok installed\nVersion: 2.8.14\nArchitecture: amd64\n' \
        >"$root/var/lib/dpkg/status"
    printf 'original-coherent-overlay\n' >"$root/usr/bin/punto-daemon"
    printf 'original-coherent-helper\n' >"$root/usr/libexec/punto-session-bridge"
    printf 'original-coherent-protocol\n' >"$root/usr/share/punto-switcher/protocol/punto-dbus-v1.xml"
    printf 'original-coherent-session-desktop\n' >"$root/etc/xdg/autostart/punto-session-bridge.desktop"
    printf 'original-coherent-helper-unit\n' >"$root/usr/lib/systemd/user/punto-session-bridge.service"
    printf 'learned-preserved\n' >"$root/etc/punto/undo_exclusions.txt"
    current_config=$(sha256sum "$root/etc/punto/config.yaml" | cut -d' ' -f1)
    current_daemon=$(sha256sum "$root/usr/bin/punto-daemon" | cut -d' ' -f1)
    current_helper=$(sha256sum "$root/usr/libexec/punto-session-bridge" | cut -d' ' -f1)
    current_protocol=$(sha256sum "$root/usr/share/punto-switcher/protocol/punto-dbus-v1.xml" | cut -d' ' -f1)
    current_desktop=$(sha256sum "$root/etc/xdg/autostart/punto-session-bridge.desktop" | cut -d' ' -f1)
    current_unit=$(sha256sum "$root/usr/lib/systemd/user/punto-session-bridge.service" | cut -d' ' -f1)
    current_unit_metadata=$(stat -c '%u:%g:%a:%h:%Y' "$root/usr/lib/systemd/user/punto-session-bridge.service")
    learned=$(sha256sum "$root/etc/punto/undo_exclusions.txt" | cut -d' ' -f1)
}

cat >"$work/lifecycle-transaction" <<'SH'
#!/bin/bash
set -euo pipefail
printf '%s\n' "$1" >>"$PUNTO_TEST_LIFECYCLE_TRACE"
if [[ $1 == purge ]]; then
    if [[ ${PUNTO_TEST_PURGE_RETAIN_CONFIG:-0} != 1 ]]; then
        rm -f "$PUNTO_TEST_ROOT/etc/punto/config.yaml"
    fi
    rm -f "$PUNTO_TEST_ROOT/etc/xdg/autostart/punto-tray.desktop" \
        "$PUNTO_TEST_ROOT/etc/xdg/autostart/punto-session-bridge.desktop"
    : >"$PUNTO_TEST_ROOT/var/lib/dpkg/status"
else
    directories_before=$(stat -c '%u:%g:%a' "$PUNTO_TEST_ROOT" "$PUNTO_TEST_ROOT/etc" \
        "$PUNTO_TEST_ROOT/etc/xdg" "$PUNTO_TEST_ROOT/etc/xdg/autostart")
    dpkg-deb --fsys-tarfile "$2" | tar --extract --no-overwrite-dir --file=- --directory="$PUNTO_TEST_ROOT"
    [[ $(stat -c '%u:%g:%a' "$PUNTO_TEST_ROOT" "$PUNTO_TEST_ROOT/etc" \
        "$PUNTO_TEST_ROOT/etc/xdg" "$PUNTO_TEST_ROOT/etc/xdg/autostart") == "$directories_before" ]]
    control=$(mktemp -d)
    trap 'rm -rf -- "$control"' EXIT
    dpkg-deb -e "$2" "$control"
    for script in postinst prerm postrm; do
        rm -f "$PUNTO_TEST_ROOT/var/lib/dpkg/info/punto-switcher.$script"
        if [[ -f $control/$script ]]; then
            cp "$control/$script" "$PUNTO_TEST_ROOT/var/lib/dpkg/info/punto-switcher.$script"
        fi
    done
    version=$(dpkg-deb -f "$2" Version)
    printf 'Package: punto-switcher\nStatus: install ok installed\nVersion: %s\nArchitecture: amd64\n' \
        "$version" >"$PUNTO_TEST_ROOT/var/lib/dpkg/status"
    if [[ $version == 2.8.5 ]]; then
        cp "$PUNTO_TEST_ROOT/usr/share/punto-switcher/config.yaml" "$PUNTO_TEST_ROOT/etc/punto/config.yaml"
    fi
fi
SH
cat >"$work/quiesce" <<'SH'
#!/bin/sh
[ "$1" = quiesce-v1 ] || exit 64
echo quiesce >>"$PUNTO_TEST_LIFECYCLE_TRACE"
SH
cat >"$work/lifecycle-activate" <<'SH'
#!/bin/sh
echo activate >>"$PUNTO_TEST_LIFECYCLE_TRACE"
[ "$(sha256sum "$PUNTO_TEST_ROOT/etc/punto/config.yaml" | cut -d' ' -f1)" = "$PUNTO_TEST_CONFIG_HASH" ] || exit 81
exit "${PUNTO_TEST_ACTIVATE_RESULT:-0}"
SH
chmod 0700 "$work/lifecycle-transaction" "$work/quiesce" "$work/lifecycle-activate"
export PUNTO_TEST_ROOT="$root" PUNTO_TEST_LIFECYCLE_TRACE="$work/lifecycle.trace"
export PUNTO_QUIESCE="$work/quiesce"
current_setup
export PUNTO_TEST_CONFIG_HASH="$current_config"
sh "$preflight" baseline285 "$legacy" "$candidate" "$candidate" "$root" >"$work/current-admission.log"
grep -qx 'EXACT285_PREFLIGHT_OK job=target tray=target mode=baseline285' "$work/current-admission.log"
cp "$root/var/lib/dpkg/info/punto-switcher.postinst" "$work/current-postinst"
printf 'foreign control\n' >"$root/var/lib/dpkg/info/punto-switcher.postinst"
if sh "$preflight" baseline285 "$legacy" "$candidate" "$candidate" "$root" >"$work/current-foreign.log" 2>&1; then exit 1; fi
grep -q 'control differs' "$work/current-foreign.log"
cp "$work/current-postinst" "$root/var/lib/dpkg/info/punto-switcher.postinst"
mv "$root/usr/share/punto-switcher/protocol/punto-dbus-v1.xml" "$work/current-xml"
if sh "$preflight" baseline285 "$legacy" "$candidate" "$candidate" "$root" >"$work/missing-overlay.log" 2>&1; then exit 1; fi
grep -q 'owned file is unsafe' "$work/missing-overlay.log"
mv "$work/current-xml" "$root/usr/share/punto-switcher/protocol/punto-dbus-v1.xml"

"$orchestrator" baseline285 "$work/lifecycle-transaction" "$candidate" "$legacy" "$candidate" \
    "$work/lifecycle-activate" "--root=$root" "--admindir=$root/var/lib/dpkg" >"$work/baseline.log" 2>&1
[[ $(cat "$work/lifecycle.trace") == $'quiesce\ndowngrade\nactivate' ]]
sh "$preflight" "$legacy" "$candidate" "$root" >"$work/verified-baseline.log"
original=$(sed -n 's/^punto-activation: recovery_snapshot=\([^ ]*\) kind=S-original$/\1/p' "$work/baseline.log")
[[ $(cat "$original/kind") == snapshot-original ]]
[[ $(sha256sum "$original/job" | cut -d' ' -f1) == 185a49a3860ddd73fbcbbb8b0bd8fdd9723f592306a8bf20adb1b3a6267c1b51 ]]
[[ $(sha256sum "$original/overlay/usr/bin/punto-daemon" | cut -d' ' -f1) == "$current_daemon" ]]
[[ $(sha256sum "$original/overlay/usr/libexec/punto-session-bridge" | cut -d' ' -f1) == "$current_helper" ]]
[[ $(sha256sum "$original/overlay/usr/share/punto-switcher/protocol/punto-dbus-v1.xml" | cut -d' ' -f1) == "$current_protocol" ]]
[[ $(sha256sum "$original/overlay/etc/xdg/autostart/punto-session-bridge.desktop" | cut -d' ' -f1) == "$current_desktop" ]]
[[ $(sha256sum "$original/overlay/usr/lib/systemd/user/punto-session-bridge.service" | cut -d' ' -f1) == "$current_unit" ]]
[[ $(stat -c '%u:%g:%a:%h:%Y' "$original/overlay/usr/lib/systemd/user/punto-session-bridge.service") == "$current_unit_metadata" ]]

permission_state() {
    python3 - "$1" "${2:-all}" <<'PY'
import hashlib, json, os, pathlib, stat, sys
root = pathlib.Path(sys.argv[1])
names = ('etc/punto', 'usr/local/bin', 'usr/share/punto-switcher',
         'usr/share/punto-switcher/sounds', 'usr/share/punto-switcher/config.yaml',
         'usr/share/punto-switcher/udevmon.yaml', 'usr/share/punto-switcher/sounds/en_ru.wav',
         'usr/share/punto-switcher/sounds/ru_en.wav')
if sys.argv[2] == 'all':
    names += ('etc/interception/udevmon.yaml', 'etc/xdg/autostart/punto-tray.desktop')
result = {}
for name in names:
    path = root / name
    value = path.lstat()
    record = [value.st_dev, value.st_ino, value.st_uid, value.st_gid,
              value.st_mode, value.st_nlink, value.st_mtime_ns]
    if stat.S_ISREG(value.st_mode):
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NOATIME)
        try:
            digest = hashlib.sha256()
            while chunk := os.read(fd, 65536):
                digest.update(chunk)
            record += [value.st_size, digest.hexdigest()]
        finally:
            os.close(fd)
    elif stat.S_ISLNK(value.st_mode):
        record += [os.readlink(path)]
    record += [{name: hashlib.sha256(os.getxattr(path, name, follow_symlinks=False)).hexdigest()
                for name in os.listxattr(path, follow_symlinks=False)}]
    result[name] = record
print(json.dumps(result, sort_keys=True))
PY
}
for directory in etc/punto usr/local/bin usr/share/punto-switcher usr/share/punto-switcher/sounds; do
    [[ $(stat -c '%u:%g:%a' "$root/$directory") == 0:0:755 ]]
done
for file in etc/interception/udevmon.yaml etc/xdg/autostart/punto-tray.desktop \
            usr/share/punto-switcher/config.yaml usr/share/punto-switcher/udevmon.yaml \
            usr/share/punto-switcher/sounds/en_ru.wav usr/share/punto-switcher/sounds/ru_en.wav; do
    [[ $(stat -c '%u:%g:%a:%h' "$root/$file") == 0:0:644:1 ]]
done
secured=$(permission_state "$root" shared)
sh "$files_helper" project285 "$root" "$legacy" "$candidate" "$original" >"$work/project285-repeat.log"
[[ $(permission_state "$root" shared) == "$secured" ]]

# Admission covers the whole fixed set before either projection or chmod.
for fault in foreign-bytes writable-file linked-file acl-file writable-directory; do
    negative_root=$work/permissions-$fault-root
    cp -a "$root" "$negative_root"
    negative_snapshot=$negative_root${original#"$root"}
    target=$negative_root/usr/share/punto-switcher/sounds/ru_en.wav
    case "$fault" in
        foreign-bytes) printf 'foreign wave\n' >"$target" ;;
        writable-file) chmod 0666 "$target" ;;
        linked-file) rm "$target"; ln -s "$negative_root/etc/punto/config.yaml" "$target" ;;
        writable-directory) chmod 0777 "$negative_root/usr/share/punto-switcher/sounds" ;;
        acl-file)
            python3 - "$target" <<'PY'
import os, struct, sys
entries = ((1, 6, 0xffffffff), (2, 4, 0), (4, 4, 0xffffffff),
           (16, 4, 0xffffffff), (32, 4, 0xffffffff))
os.setxattr(sys.argv[1], 'system.posix_acl_access', struct.pack('<I', 2) +
            b''.join(struct.pack('<HHI', *entry) for entry in entries))
assert 'system.posix_acl_access' in os.listxattr(sys.argv[1])
PY
            ;;
    esac
    before_permissions=$(permission_state "$negative_root")
    if sh "$files_helper" project285 "$negative_root" "$legacy" "$candidate" "$negative_snapshot" \
            >"$work/permissions-$fault.log" 2>&1; then
        echo "untrusted legacy metadata admitted: $fault" >&2
        exit 1
    fi
    [[ $(permission_state "$negative_root") == "$before_permissions" ]]
done
receipt_hash=$(sha256sum "$original/receipt.sha256" | cut -d' ' -f1)
baseline_snapshot=$(sh "$files_helper" snapshot-285 "$root" "$legacy" "$candidate" auto)
[[ $baseline_snapshot != "$original" && $(cat "$baseline_snapshot/kind") == snapshot-285 ]]
[[ $(sha256sum "$original/receipt.sha256" | cut -d' ' -f1) == "$receipt_hash" ]]

# A known failed baseline activation must recover original raw bytes, not just
# the same-version archive payload, through the one rollback owner.
current_setup
export PUNTO_TEST_CONFIG_HASH="$current_config" PUNTO_TEST_ACTIVATE_RESULT=42
: >"$work/lifecycle.trace"
set +e
"$orchestrator" baseline285 "$work/lifecycle-transaction" "$candidate" "$legacy" "$candidate" \
    "$work/lifecycle-activate" "--root=$root" "--admindir=$root/var/lib/dpkg" >"$work/baseline-rollback.log" 2>&1
status=$?
set -e
[[ $status == 42 ]]
[[ $(cat "$work/lifecycle.trace") == $'quiesce\ndowngrade\nactivate\nquiesce\ndowngrade' ]]
[[ $(sha256sum "$root/usr/bin/punto-daemon" | cut -d' ' -f1) == "$current_daemon" ]]
[[ $(sha256sum "$root/usr/libexec/punto-session-bridge" | cut -d' ' -f1) == "$current_helper" ]]
[[ $(sha256sum "$root/usr/share/punto-switcher/protocol/punto-dbus-v1.xml" | cut -d' ' -f1) == "$current_protocol" ]]
[[ $(sha256sum "$root/etc/xdg/autostart/punto-session-bridge.desktop" | cut -d' ' -f1) == "$current_desktop" ]]
[[ $(sha256sum "$root/usr/lib/systemd/user/punto-session-bridge.service" | cut -d' ' -f1) == "$current_unit" ]]
[[ $(stat -c '%u:%g:%a:%h:%Y' "$root/usr/lib/systemd/user/punto-session-bridge.service") == "$current_unit_metadata" ]]
[[ $(sha256sum "$root/etc/punto/config.yaml" | cut -d' ' -f1) == "$current_config" ]]
grep -q 'package_restored=1 files_restored=1 runtime_restored=unknown input_verified=unknown' "$work/baseline-rollback.log"

# A changed sealed unit must stop rollback before its package or callback runs.
current_setup
export PUNTO_TEST_CONFIG_HASH="$current_config"
tamper_root=$work/tamper-unit-root
cp -a "$root" "$tamper_root"
mv "$tamper_root/var/lib/punto-switcher/deployment" "$tamper_root/var/lib/punto-switcher/deployment-before"
cat >"$work/activate-tampered-unit" <<'SH'
#!/bin/sh
echo activate >>"$PUNTO_TEST_LIFECYCLE_TRACE"
set -- "$PUNTO_TEST_ROOT"/var/lib/punto-switcher/deployment/recovery.*
[ "$#" = 1 ] && [ -d "$1" ] || exit 83
printf 'foreign sealed unit\n' >>"$1/overlay/usr/lib/systemd/user/punto-session-bridge.service"
exit 42
SH
chmod 0700 "$work/activate-tampered-unit"
: >"$work/tampered-unit.trace"
set +e
PUNTO_TEST_ROOT="$tamper_root" PUNTO_TEST_LIFECYCLE_TRACE="$work/tampered-unit.trace" \
    "$orchestrator" baseline285 "$work/lifecycle-transaction" "$candidate" "$legacy" "$candidate" \
    "$work/activate-tampered-unit" "--root=$tamper_root" "--admindir=$tamper_root/var/lib/dpkg" \
    >"$work/tampered-unit.log" 2>&1
status=$?
set -e
[[ $status == 75 && $(cat "$work/tampered-unit.trace") == $'quiesce\ndowngrade\nactivate\nquiesce' ]]
grep -q 'snapshot receipt differs' "$work/tampered-unit.log"
grep -q 'recovery_snapshot_unverified' "$tamper_root/var/lib/dpkg/.punto-deployment/unresolved"

current_setup
export PUNTO_TEST_CONFIG_HASH="$current_config" PUNTO_TEST_ACTIVATE_RESULT=0
: >"$work/lifecycle.trace"
"$orchestrator" fresh "$work/lifecycle-transaction" "$candidate" "$legacy" "$candidate" \
    "$work/lifecycle-activate" "--root=$root" "--admindir=$root/var/lib/dpkg" >"$work/fresh.log" 2>&1
[[ $(cat "$work/lifecycle.trace") == $'quiesce\npurge\ninstall\nactivate' ]]
grep -q 'fresh_absence_verified=1' "$work/fresh.log"
[[ $(sha256sum "$root/etc/punto/undo_exclusions.txt" | cut -d' ' -f1) == "$learned" ]]
[[ $(sha256sum "$root/etc/punto/config.yaml" | cut -d' ' -f1) == "$current_config" ]]

tampered=$(sh "$files_helper" snapshot-original "$root" "$legacy" "$candidate" auto)
printf 'receipt-tamper\n' >>"$tampered/job"
if sh "$files_helper" verify "$root" "$legacy" "$candidate" "$tampered" >"$work/tampered.log" 2>&1; then exit 1; fi
grep -q 'snapshot receipt differs' "$work/tampered.log"
[[ $(sha256sum "$root/etc/punto/config.yaml" | cut -d' ' -f1) == "$current_config" ]]

# A broken status database is an unavailable observation, not absence. Isolate
# it so the following terminal conffile control cannot reuse its unresolved root.
catalogue_root=$work/catalogue-root
cp -a "$root" "$catalogue_root"
cat >"$work/catalogue-transaction" <<'SH'
#!/bin/sh
echo "$1" >>"$PUNTO_TEST_LIFECYCLE_TRACE"
[ "$1" = purge ] || exit 82
printf 'not-a-dpkg-record\n' >"$PUNTO_TEST_ROOT/var/lib/dpkg/status"
SH
chmod 0700 "$work/catalogue-transaction"
: >"$work/catalogue.trace"
set +e
PUNTO_TEST_ROOT="$catalogue_root" PUNTO_TEST_LIFECYCLE_TRACE="$work/catalogue.trace" \
    "$orchestrator" fresh "$work/catalogue-transaction" "$candidate" "$legacy" "$candidate" \
    "$work/lifecycle-activate" "--root=$catalogue_root" \
    "--admindir=$catalogue_root/var/lib/dpkg" >"$work/catalogue-refusal.log" 2>&1
status=$?
set -e
[[ $status == 75 && $(cat "$work/catalogue.trace") == $'quiesce\npurge' ]]
grep -q 'fresh_package_catalogue_unavailable' "$catalogue_root/var/lib/dpkg/.punto-deployment/unresolved"

# Terminal refusal: an apparently absent dpkg record is insufficient if a
# managed conffile survived purge. No fresh install/activation may follow.
current_setup
export PUNTO_TEST_CONFIG_HASH="$current_config" PUNTO_TEST_PURGE_RETAIN_CONFIG=1
: >"$work/lifecycle.trace"
set +e
"$orchestrator" fresh "$work/lifecycle-transaction" "$candidate" "$legacy" "$candidate" \
    "$work/lifecycle-activate" "--root=$root" "--admindir=$root/var/lib/dpkg" >"$work/fresh-refusal.log" 2>&1
status=$?
set -e
[[ $status == 75 && $(cat "$work/lifecycle.trace") == $'quiesce\npurge' ]]
grep -q 'fresh_conffile_absence_unverified' "$root/var/lib/dpkg/.punto-deployment/unresolved"

echo 'test_exact285_preflight: OK (lifecycle transport fixture; native input NOT_RUN)'
