#!/bin/sh
set -eu

if [ "$#" -ne 5 ]; then
    echo "punto-files: usage: snapshot|snapshot-285|snapshot-original|verify|migrate|project285|restore ROOT LEGACY CANDIDATE SNAPSHOT" >&2
    exit 64
fi
mode=$1
root=$2
legacy=$3
candidate=$4
snapshot=$5
case "$mode" in
    snapshot|snapshot-285|snapshot-original|verify|migrate|project285|restore) ;;
    *) echo "punto-files: invalid mode" >&2; exit 64 ;;
esac
case "$root" in
    /*) ;;
    *) echo "punto-files: root must be absolute" >&2; exit 64 ;;
esac
root=$(/usr/bin/realpath -e -- "$root") || exit 66
base=$root/var/lib/punto-switcher/deployment
job=$root/etc/interception/udevmon.yaml
tray=$root/etc/xdg/autostart/punto-tray.desktop
config=$root/etc/punto/config.yaml
exclusions=$root/etc/punto/undo_exclusions.txt
old_job=50161d1bae210ec039183748fe05ba2ec5116ccd2498b2a9c5a82b05bafda468
new_job=185a49a3860ddd73fbcbbb8b0bd8fdd9723f592306a8bf20adb1b3a6267c1b51
old_tray=a43f8a5eae11059515a6064a041a963fa42e27dca97ec4b77f871fdb1d99f934
new_tray=39d534efa9069bb54c5ae11879fdc6a91f6cb4a5ce53f9caa66d10181b78cee5

fail() {
    echo "punto-files: $1" >&2
    exit 66
}
hash_of() {
    /usr/bin/sha256sum "$1" | /usr/bin/cut -d ' ' -f1
}
safe_file() {
    [ -f "$1" ] && [ ! -L "$1" ] &&
        [ "$(/usr/bin/stat -c '%u:%g:%h' "$1")" = 0:0:1 ] ||
        fail "file is not trusted: $1"
}
safe_snapshot() {
    [ -d "$base" ] && [ ! -L "$base" ] &&
        [ "$(/usr/bin/stat -c '%u:%g:%a' "$base")" = 0:0:700 ] ||
        fail "recovery parent is not root-only"
    case "$snapshot" in
        "$base"/recovery.*) ;;
        *) fail "snapshot path is outside recovery parent" ;;
    esac
    [ -d "$snapshot" ] && [ ! -L "$snapshot" ] &&
        [ "$(/usr/bin/stat -c '%u:%g:%a' "$snapshot")" = 0:0:700 ] ||
        fail "recovery snapshot is not root-only"
}
copy_atomic() {
    source=$1
    destination=$2
    safe_file "$source"
    if [ -e "$destination" ] || [ -L "$destination" ]; then
        safe_file "$destination"
    fi
    temporary=$destination.punto-migration.$$
    [ ! -e "$temporary" ] && [ ! -L "$temporary" ] ||
        fail "migration temporary path exists"
    /usr/bin/cp -a -- "$source" "$temporary" || fail "cannot stage owned file"
    /usr/bin/mv -fT -- "$temporary" "$destination" ||
        fail "cannot publish owned file"
}
known_current() {
    safe_file "$1"
    actual=$(hash_of "$1")
    [ "$actual" = "$2" ] || [ "$actual" = "$3" ] ||
        fail "owned file changed outside migration"
}
no_alternate_access() {
    access=$(LC_ALL=C /usr/bin/ls -ld -- "$1") || fail "cannot inspect access metadata"
    access=${access%% *}
    [ "${#access}" = 10 ] || fail "alternate access metadata is not admitted: $1"
}
protected_directory() {
    [ -d "$1" ] && [ ! -L "$1" ] &&
        [ "$(/usr/bin/stat -c '%u:%g:%a' "$1")" = 0:0:755 ] ||
        fail "permission ancestor is not protected: $1"
    no_alternate_access "$1"
}
directory_state() {
    [ -d "$1" ] && [ ! -L "$1" ] || fail "permission target is not a directory: $1"
    no_alternate_access "$1"
    value=$(/usr/bin/stat -c '%d:%i:%u:%g:%a' "$1")
    case "$value" in
        *:0:0:755|*:0:0:775) printf '%s\n' "$value" ;;
        *) fail "permission directory metadata is not admitted: $1" ;;
    esac
}
legacy_file_state() {
    permission_file=$1
    expected_hash=$2
    safe_file "$permission_file"
    no_alternate_access "$permission_file"
    value=$(/usr/bin/stat -c '%d:%i:%u:%g:%a:%h:%s:%y' "$permission_file")
    case "$(/usr/bin/stat -c '%a' "$permission_file")" in
        644|664) ;;
        *) fail "permission file mode is not admitted: $permission_file" ;;
    esac
    exec 7<"$permission_file"
    [ "$(/usr/bin/stat -Lc '%d:%i:%u:%g:%a:%h:%s:%y' /proc/self/fd/7)" = "$value" ] &&
        [ "$(hash_of /proc/self/fd/7)" = "$expected_hash" ] &&
        [ ! -L "$permission_file" ] &&
        [ "$(/usr/bin/stat -c '%d:%i:%u:%g:%a:%h:%s:%y' "$permission_file")" = "$value" ] ||
        fail "permission file changed during admission: $permission_file"
    printf '%s\n' "$value"
}
admit_legacy_permissions() {
    for ancestor in "$root" "$root/etc" "$root/etc/interception" "$root/etc/xdg" \
                    "$root/etc/xdg/autostart" "$root/usr" "$root/usr/local" "$root/usr/share"; do
        protected_directory "$ancestor"
    done
    dir1=$(directory_state "$root/etc/punto")
    dir2=$(directory_state "$root/usr/local/bin")
    dir3=$(directory_state "$root/usr/share/punto-switcher")
    dir4=$(directory_state "$root/usr/share/punto-switcher/sounds")
    exec 3<"$root/etc/punto" 4<"$root/usr/local/bin" \
         5<"$root/usr/share/punto-switcher" 6<"$root/usr/share/punto-switcher/sounds"
    for permission_pair in "3:$dir1:$root/etc/punto" "4:$dir2:$root/usr/local/bin" \
                           "5:$dir3:$root/usr/share/punto-switcher" "6:$dir4:$root/usr/share/punto-switcher/sounds"; do
        descriptor=${permission_pair%%:*}
        rest=${permission_pair#*:}
        permission_dir=${rest#*:*:*:*:*:}
        value=${rest%:"$permission_dir"}
        [ "$(/usr/bin/stat -Lc '%d:%i:%u:%g:%a' "/proc/self/fd/$descriptor")" = "$value" ] &&
            [ ! -L "$permission_dir" ] &&
            [ "$(/usr/bin/stat -c '%d:%i:%u:%g:%a' "$permission_dir")" = "$value" ] ||
            fail "permission directory changed during admission"
    done
    legacy_records=
    for relative in usr/share/punto-switcher/config.yaml usr/share/punto-switcher/udevmon.yaml \
                    usr/share/punto-switcher/sounds/en_ru.wav usr/share/punto-switcher/sounds/ru_en.wav \
                    etc/interception/udevmon.yaml etc/xdg/autostart/punto-tray.desktop; do
        case "$relative" in
            etc/interception/*) expected_hash=$(hash_of "$job")
                [ "$expected_hash" = "$old_job" ] || [ "$expected_hash" = "$new_job" ] || fail "job is not admitted" ;;
            etc/xdg/*) expected_hash=$(hash_of "$tray")
                [ "$expected_hash" = "$old_tray" ] || [ "$expected_hash" = "$new_tray" ] || fail "tray is not admitted" ;;
            *) expected_hash=$(hash_of "$snapshot/legacy/$relative") ;;
        esac
        value=$(legacy_file_state "$root/$relative" "$expected_hash")
        legacy_records="${legacy_records}${legacy_records:+
}$relative|$value|$expected_hash"
    done
}
normalize_legacy_permissions() {
    for permission_pair in "3:$dir1:$root/etc/punto" "4:$dir2:$root/usr/local/bin" \
                           "5:$dir3:$root/usr/share/punto-switcher" "6:$dir4:$root/usr/share/punto-switcher/sounds"; do
        descriptor=${permission_pair%%:*}
        rest=${permission_pair#*:}
        permission_dir=${rest#*:*:*:*:*:}
        value=${rest%:"$permission_dir"}
        [ ! -L "$permission_dir" ] &&
            [ "$(/usr/bin/stat -c '%d:%i:%u:%g:%a' "$permission_dir")" = "$value" ] &&
            [ "$(/usr/bin/stat -Lc '%d:%i:%u:%g:%a' "/proc/self/fd/$descriptor")" = "$value" ] ||
            fail "permission directory changed before normalization"
        no_alternate_access "$permission_dir"
        if [ "${value##*:}" = 775 ]; then
            /usr/bin/chmod 0755 "/proc/self/fd/$descriptor" || fail "cannot secure legacy directory"
        fi
        [ "$(/usr/bin/stat -Lc '%d:%i:%u:%g:%a' "/proc/self/fd/$descriptor")" = "${value%:*}:755" ] &&
            [ "$(/usr/bin/stat -c '%d:%i:%u:%g:%a' "$permission_dir")" = "${value%:*}:755" ] ||
            fail "secured permission directory differs"
    done
    printf '%s\n' "$legacy_records" | while IFS='|' read -r relative value expected_hash; do
        case "$relative" in
            etc/interception/*|etc/xdg/*)
                case "$relative" in
                    etc/interception/*) expected_hash=$old_job ;;
                    etc/xdg/*) expected_hash=$old_tray ;;
                esac
                value=$(legacy_file_state "$root/$relative" "$expected_hash") ;;
        esac
        current=$(legacy_file_state "$root/$relative" "$expected_hash")
        [ "$current" = "$value" ] || fail "permission file changed before normalization"
        exec 7<"$root/$relative"
        [ "$(/usr/bin/stat -Lc '%d:%i:%u:%g:%a:%h:%s:%y' /proc/self/fd/7)" = "$value" ] ||
            fail "permission file changed while opening"
        inode_state=$(/usr/bin/stat -Lc '%d:%i:%u:%g:%h:%s:%y' /proc/self/fd/7)
        if [ "$(/usr/bin/stat -Lc '%a' /proc/self/fd/7)" = 664 ]; then
            /usr/bin/chmod 0644 /proc/self/fd/7 || fail "cannot secure legacy file"
        fi
        protected_directory "${root}/${relative%/*}"
        safe_file "$root/$relative"
        [ "$(/usr/bin/stat -Lc '%d:%i:%u:%g:%h:%s:%y' /proc/self/fd/7)" = "$inode_state" ] &&
            [ "$(/usr/bin/stat -c '%d:%i:%u:%g:%h:%s:%y' "$root/$relative")" = "$inode_state" ] &&
            [ "$(/usr/bin/stat -Lc '%a' /proc/self/fd/7)" = 644 ] &&
            [ "$(hash_of /proc/self/fd/7)" = "$expected_hash" ] || fail "secured legacy file differs"
    done
}

case "$mode" in
snapshot|snapshot-285|snapshot-original)
    [ "$snapshot" = auto ] || fail "snapshot mode requires auto destination"
    [ -d "$root/var/lib" ] && [ ! -L "$root/var/lib" ] ||
        fail "recovery parent is unsafe"
    [ ! -L "$root/var/lib/punto-switcher" ] && [ ! -L "$base" ] ||
        fail "recovery parent is a link"
    /usr/bin/install -d -m 0700 "$base" || fail "cannot create recovery parent"
    [ "$(/usr/bin/stat -c '%u:%g:%a' "$base")" = 0:0:700 ] ||
        fail "recovery parent is not root-only"
    snapshot=$(/usr/bin/mktemp -d "$base/recovery.XXXXXXXX") ||
        fail "cannot allocate recovery snapshot"
    for pair in "$job:job" "$tray:tray" "$config:config"; do
        source=${pair%:*}
        name=${pair#*:}
        safe_file "$source"
        /usr/bin/cp -a -- "$source" "$snapshot/$name" ||
            fail "cannot back up owned file"
    done
    if [ -e "$exclusions" ] || [ -L "$exclusions" ]; then
        safe_file "$exclusions"
        /usr/bin/cp -a -- "$exclusions" "$snapshot/exclusions" ||
            fail "cannot back up learned exclusions"
    else
        /usr/bin/touch "$snapshot/exclusions.absent"
    fi
    /usr/bin/dpkg-deb -x "$candidate" "$snapshot/candidate" >/dev/null ||
        fail "cannot retain candidate payload"
    /usr/bin/dpkg-deb -x "$legacy" "$snapshot/legacy" >/dev/null ||
        fail "cannot retain legacy payload"
    [ "$(hash_of "$snapshot/candidate/usr/share/doc/punto-switcher/examples/udevmon.yaml")" = "$new_job" ] ||
        fail "retained candidate job differs"
    [ "$(hash_of "$snapshot/candidate/etc/xdg/autostart/punto-tray.desktop")" = "$new_tray" ] ||
        fail "retained candidate tray differs"
    if [ "$mode" = snapshot-original ]; then
        safe_file "$candidate"
        recovery_hash=$(hash_of "$candidate")
        /usr/bin/cp -a -- "$candidate" "$snapshot/recovery.deb" || fail "cannot retain recovery archive"
        [ "$(hash_of "$snapshot/recovery.deb")" = "$recovery_hash" ] &&
            [ "$(hash_of "$candidate")" = "$recovery_hash" ] || fail "recovery archive changed while sealing"
        for path in usr/bin/punto-daemon usr/libexec/punto-session-bridge \
                    usr/share/punto-switcher/protocol/punto-dbus-v1.xml \
                    etc/xdg/autostart/punto-session-bridge.desktop \
                    usr/lib/systemd/user/punto-session-bridge.service; do
            source=$root/$path
            safe_file "$source"
            /usr/bin/install -d -m 0700 "$snapshot/overlay/$(dirname -- "$path")"
            /usr/bin/cp -a -- "$source" "$snapshot/overlay/$path" || fail "cannot seal original overlay"
        done
    fi
    printf '%s\n' "$mode" >"$snapshot/kind"
    (cd "$snapshot" && /usr/bin/find . -type f ! -name receipt.sha256 -print0 |
        /usr/bin/sort -z | /usr/bin/xargs -0 /usr/bin/sha256sum >receipt.sha256) || fail "cannot seal snapshot"
    echo "$snapshot"
    exit 0
    ;;
esac

safe_snapshot
safe_file "$snapshot/kind"
safe_file "$snapshot/receipt.sha256"
(cd "$snapshot" && /usr/bin/sha256sum --status -c receipt.sha256) || fail "snapshot receipt differs"
safe_file "$snapshot/job"
safe_file "$snapshot/tray"
safe_file "$snapshot/config"
if [ -e "$snapshot/exclusions" ]; then
    safe_file "$snapshot/exclusions"
elif [ ! -f "$snapshot/exclusions.absent" ]; then
    fail "learned-exclusions snapshot is incomplete"
fi
if [ "$mode" = verify ]; then
    echo 'PUNTO_SNAPSHOT_VERIFIED files=sealed'
    exit 0
fi

case "$mode" in
project285)
    admit_legacy_permissions
    known_current "$job" "$old_job" "$new_job"
    known_current "$tray" "$old_tray" "$new_tray"
    source=$snapshot/legacy/usr/share/punto-switcher/udevmon.yaml
    [ "$(hash_of "$source")" = "$old_job" ] || fail "retained legacy job differs"
    copy_atomic "$source" "$job"
    source=$snapshot/legacy/etc/xdg/autostart/punto-tray.desktop
    [ "$(hash_of "$source")" = "$old_tray" ] || fail "retained legacy tray differs"
    copy_atomic "$source" "$tray"
    normalize_legacy_permissions
    ;;
migrate)
    known_current "$job" "$old_job" "$new_job"
    known_current "$tray" "$old_tray" "$new_tray"
    if [ "$(hash_of "$job")" = "$old_job" ]; then
        source=$snapshot/candidate/usr/share/doc/punto-switcher/examples/udevmon.yaml
        [ "$(hash_of "$source")" = "$new_job" ] || fail "candidate job differs"
        copy_atomic "$source" "$job"
    fi
    if [ "$(hash_of "$tray")" = "$old_tray" ]; then
        source=$snapshot/candidate/etc/xdg/autostart/punto-tray.desktop
        [ "$(hash_of "$source")" = "$new_tray" ] || fail "candidate tray differs"
        copy_atomic "$source" "$tray"
    fi
    echo "EXACT285_MIGRATED job=target tray=target"
    if [ "$(cat "$snapshot/kind")" != snapshot-original ]; then exit 0; fi
    ;;
restore)
if [ "$(cat "$snapshot/kind")" != snapshot-original ]; then
    admit_legacy_permissions
fi

known_current "$job" "$old_job" "$new_job"
known_current "$tray" "$old_tray" "$new_tray"
if [ "$(hash_of "$job")" != "$(hash_of "$snapshot/job")" ]; then
    copy_atomic "$snapshot/job" "$job"
fi
if [ "$(hash_of "$tray")" != "$(hash_of "$snapshot/tray")" ]; then
    copy_atomic "$snapshot/tray" "$tray"
fi
if [ "$(cat "$snapshot/kind")" = snapshot-original ]; then
    for path in usr/bin/punto-daemon usr/libexec/punto-session-bridge \
                usr/share/punto-switcher/protocol/punto-dbus-v1.xml \
                etc/xdg/autostart/punto-session-bridge.desktop \
                usr/lib/systemd/user/punto-session-bridge.service; do
        copy_atomic "$snapshot/overlay/$path" "$root/$path"
    done
else
    normalize_legacy_permissions
fi
    ;;
esac
safe_file "$config"
original_config=$(hash_of "$snapshot/config")
current_config=$(hash_of "$config")
legacy_stock=$(hash_of "$snapshot/legacy/usr/share/punto-switcher/config.yaml")
candidate_stock=$(hash_of "$snapshot/candidate/etc/punto/config.yaml")
if [ "$current_config" != "$original_config" ]; then
    [ "$current_config" = "$legacy_stock" ] || [ "$current_config" = "$candidate_stock" ] ||
        fail "configuration changed outside rollback"
    copy_atomic "$snapshot/config" "$config"
fi
if [ -f "$snapshot/exclusions.absent" ]; then
    [ ! -e "$exclusions" ] && [ ! -L "$exclusions" ] ||
        fail "new learned exclusions require reconciliation"
else
    safe_file "$exclusions"
    [ "$(hash_of "$exclusions")" = "$(hash_of "$snapshot/exclusions")" ] ||
        fail "learned exclusions changed outside rollback"
fi
echo "EXACT285_RESTORED files=verified"
