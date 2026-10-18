#!/bin/sh
set -eu

mode=upgrade
recovery=
case "${1:-}" in
    baseline285|fresh)
        mode=$1
        shift
        [ "$#" -eq 4 ] || exit 64
        recovery=$3
        root=$4
        ;;
    *) root=${3:-/} ;;
esac
if { [ "$mode" = upgrade ] && { [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; }; }; then
    echo "punto-preflight: usage: [baseline285|fresh] EXACT_285.deb CANDIDATE.deb [RECOVERY.deb ROOT]" >&2
    exit 64
fi
legacy=$1
candidate=$2
expected_legacy=80c977e4b195f73bdbf89c9e2ba8b1c4162a6c318ea74142fedd6010d222ebd4
legacy_job=50161d1bae210ec039183748fe05ba2ec5116ccd2498b2a9c5a82b05bafda468
candidate_job=185a49a3860ddd73fbcbbb8b0bd8fdd9723f592306a8bf20adb1b3a6267c1b51
legacy_tray=a43f8a5eae11059515a6064a041a963fa42e27dca97ec4b77f871fdb1d99f934
candidate_tray=39d534efa9069bb54c5ae11879fdc6a91f6cb4a5ce53f9caa66d10181b78cee5

fail() {
    echo "punto-preflight: $1" >&2
    exit 66
}
case "$root" in
    /*) ;;
    *) fail "root must be absolute" ;;
esac
[ -d "$root" ] && [ ! -L "$root" ] || fail "root is not a directory"
root=$(/usr/bin/realpath -e -- "$root") || fail "root cannot be resolved"
for package in "$legacy" "$candidate"; do
    [ -f "$package" ] && [ ! -L "$package" ] || fail "package is not regular"
done
actual=$(/usr/bin/sha256sum "$legacy" | /usr/bin/cut -d ' ' -f1)
[ "$actual" = "$expected_legacy" ] || fail "legacy archive hash differs"
[ "$(/usr/bin/dpkg-deb -f "$legacy" Package)" = punto-switcher ] ||
    fail "legacy package identity differs"
[ "$(/usr/bin/dpkg-deb -f "$legacy" Version)" = 2.8.5 ] ||
    fail "legacy package version differs"
[ "$(/usr/bin/dpkg-deb -f "$candidate" Package)" = punto-switcher ] ||
    fail "candidate package identity differs"
candidate_version=$(/usr/bin/dpkg-deb -f "$candidate" Version)
/usr/bin/dpkg --compare-versions "$candidate_version" gt 2.8.5 ||
    fail "candidate is not an upgrade"

admin_dir=$root/var/lib/dpkg
installed=$(/usr/bin/dpkg-query --admindir="$admin_dir" -W \
    -f='${Version}:${db:Status-Abbrev}' punto-switcher 2>/dev/null || true)
case "$mode" in
    upgrade)
        [ "$installed" = '2.8.5:ii ' ] || fail "installed baseline is not configured 2.8.5"
        control_archive=$legacy
        ;;
    baseline285|fresh)
        [ -f "$recovery" ] && [ ! -L "$recovery" ] || fail "recovery archive is not regular"
        [ "$(/usr/bin/dpkg-deb -f "$recovery" Package)" = punto-switcher ] || fail "recovery package identity differs"
        [ "$(/usr/bin/dpkg-deb -f "$recovery" Version)" = 2.8.14 ] || fail "supported recovery version is not 2.8.14"
        [ "$installed" = '2.8.14:ii ' ] || fail "installed recovery is not configured 2.8.14"
        control_archive=$recovery
        ;;
esac

tmp=$(/usr/bin/mktemp -d) || fail "cannot make inspection directory"
trap '/usr/bin/rm -rf -- "$tmp"' 0 HUP INT TERM
/usr/bin/dpkg-deb -e "$control_archive" "$tmp/control" >/dev/null ||
    fail "cannot inspect admitted control"
/usr/bin/dpkg-deb -x "$candidate" "$tmp/payload" >/dev/null ||
    fail "cannot inspect candidate payload"
if [ "$mode" = upgrade ]; then
    /usr/bin/dpkg-deb -x "$legacy" "$tmp/legacy" >/dev/null || fail "cannot inspect legacy payload"
    legacy_daemon=$root/usr/local/bin/punto-daemon
    [ -f "$legacy_daemon" ] && [ ! -L "$legacy_daemon" ] &&
        [ "$(/usr/bin/stat -c '%u:%g:%h' "$legacy_daemon")" = 0:0:1 ] ||
        fail "installed legacy daemon metadata differs"
    /usr/bin/cmp -s "$legacy_daemon" "$tmp/legacy/usr/local/bin/punto-daemon" ||
        fail "installed legacy daemon differs"
fi
for script in postinst prerm postrm; do
    expected=$tmp/control/$script
    installed_script=$admin_dir/info/punto-switcher.$script
    if [ -f "$expected" ]; then
        [ -f "$installed_script" ] && [ ! -L "$installed_script" ] ||
            fail "installed legacy control is unavailable"
        /usr/bin/cmp -s "$expected" "$installed_script" ||
            fail "installed legacy control differs"
    elif [ -e "$installed_script" ] || [ -L "$installed_script" ]; then
        fail "installed control is not in admitted archive"
    fi
done
job_payload=$tmp/payload/usr/share/doc/punto-switcher/examples/udevmon.yaml
tray_payload=$tmp/payload/etc/xdg/autostart/punto-tray.desktop
for pair in "$job_payload:$candidate_job" "$tray_payload:$candidate_tray"; do
    path=${pair%:*}
    hash=${pair#*:}
    [ -f "$path" ] && [ ! -L "$path" ] || fail "candidate file is missing"
    actual=$(/usr/bin/sha256sum "$path" | /usr/bin/cut -d ' ' -f1)
    [ "$actual" = "$hash" ] || fail "candidate owned file differs"
done

for directory in "$root/etc" "$root/etc/punto" \
                 "$root/etc/interception" "$root/etc/xdg" \
                 "$root/etc/xdg/autostart"; do
    [ -d "$directory" ] && [ ! -L "$directory" ] ||
        fail "owned directory is unsafe"
    owner=$(/usr/bin/stat -c '%u:%g' "$directory")
    [ "$owner" = 0:0 ] || fail "owned directory is not root-owned"
    directory_mode=$(/usr/bin/stat -c '%a' "$directory")
    if [ "$directory" = "$root/etc/punto" ] && [ "$directory_mode" = 775 ]; then
        : # Exact 2.8.5 shipped this root:root directory mode.
    elif [ $((0$directory_mode & 0022)) -ne 0 ]; then
        fail "owned directory is writable outside root: $directory"
    fi
done
set -- "$root/etc/punto/config.yaml" "$root/etc/interception/udevmon.yaml" \
       "$root/etc/xdg/autostart/punto-tray.desktop"
if [ "$mode" != upgrade ]; then
    set -- "$@" "$root/usr/bin/punto-daemon" \
        "$root/usr/libexec/punto-session-bridge" \
        "$root/usr/share/punto-switcher/protocol/punto-dbus-v1.xml" \
        "$root/etc/xdg/autostart/punto-session-bridge.desktop"
fi
for file do
    [ -f "$file" ] && [ ! -L "$file" ] || fail "owned file is unsafe"
    metadata=$(/usr/bin/stat -c '%u:%g:%h' "$file")
    [ "$metadata" = 0:0:1 ] || fail "owned file metadata is unsafe"
    file_mode=$(/usr/bin/stat -c '%a' "$file")
    [ $((0$file_mode & 0002)) -eq 0 ] || fail "owned file is world-writable"
done
exclusions=$root/etc/punto/undo_exclusions.txt
if [ -e "$exclusions" ] || [ -L "$exclusions" ]; then
    [ -f "$exclusions" ] && [ ! -L "$exclusions" ] ||
        fail "learned exclusions are unsafe"
    metadata=$(/usr/bin/stat -c '%u:%g:%h' "$exclusions")
    [ "$metadata" = 0:0:1 ] || fail "learned exclusions metadata is unsafe"
    file_mode=$(/usr/bin/stat -c '%a' "$exclusions")
    [ $((0$file_mode & 0002)) -eq 0 ] ||
        fail "learned exclusions are world-writable"
fi

classify() {
    path=$1
    old_hash=$2
    new_hash=$3
    actual=$(/usr/bin/sha256sum "$path" | /usr/bin/cut -d ' ' -f1)
    if [ "$actual" = "$old_hash" ]; then
        echo legacy
    elif [ "$actual" = "$new_hash" ]; then
        echo target
    else
        fail "foreign job or tray file"
    fi
}
job_state=$(classify "$root/etc/interception/udevmon.yaml" \
    "$legacy_job" "$candidate_job")
tray_state=$(classify "$root/etc/xdg/autostart/punto-tray.desktop" \
    "$legacy_tray" "$candidate_tray")
echo "EXACT285_PREFLIGHT_OK job=$job_state tray=$tray_state mode=$mode"
