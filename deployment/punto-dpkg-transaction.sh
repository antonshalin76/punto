#!/bin/sh
set -eu

mode=${1:-}
package=${2:-}
shift 2 2>/dev/null || {
    echo "punto-package: usage: install|downgrade|remove|purge PACKAGE [dpkg options]" >&2
    exit 64
}
case "$mode" in
    install) downgrade= ;;
    downgrade) downgrade=--force-downgrade ;;
    remove|purge) downgrade= ;;
    *) echo "punto-package: invalid transaction mode" >&2; exit 64 ;;
esac
if [ ! -f "$package" ] || [ -L "$package" ]; then
    echo "punto-package: package is not a regular file" >&2
    exit 66
fi

observe_seconds=${PUNTO_DPKG_OBSERVE_SECONDS:-30}
case "$observe_seconds" in
    ''|*[!0-9]*) echo "punto-package: invalid observation deadline" >&2; exit 64 ;;
esac
if [ "$observe_seconds" -lt 1 ] || [ "$observe_seconds" -gt 300 ]; then
    echo "punto-package: observation deadline is outside 1..300 seconds" >&2
    exit 64
fi

state_dir=/run/punto-deployment
target_root=/
admin_dir=/var/lib/dpkg
admin_explicit=0
for argument do
    case "$argument" in
        --admindir=*) admin_dir=${argument#--admindir=}; admin_explicit=1 ;;
        --root=*) target_root=${argument#--root=} ;;
        --log=*) ;;
        --force-depends) ;;
        *) echo "punto-package: unsupported dpkg option" >&2; exit 64 ;;
    esac
done
if [ ! -d "$target_root" ]; then
    echo "punto-package: target root is not an existing directory" >&2
    exit 66
fi
target_root=$(/usr/bin/realpath -e -- "$target_root") || exit 66
if [ "$target_root" = / ]; then
    for argument do
        if [ "$argument" = --force-depends ]; then
            echo "punto-package: force-depends is forbidden on the live root" >&2
            exit 64
        fi
    done
fi
expected_admin=${target_root%/}/var/lib/dpkg
if [ "$admin_explicit" -eq 0 ]; then admin_dir=$expected_admin; fi
admin_dir=$(/usr/bin/realpath -e -- "$admin_dir") || exit 66
if [ ! -d "$admin_dir" ] || [ "$admin_dir" != "$expected_admin" ]; then
    echo "punto-package: administrative directory differs from target root" >&2
    exit 66
fi
set -- "$@" "--root=$target_root" "--admindir=$admin_dir"
if [ "$admin_dir" != /var/lib/dpkg ]; then state_dir=$admin_dir/.punto-deployment; fi
if [ -L "$state_dir" ]; then
    echo "punto-package: unsafe state directory" >&2
    exit 66
fi
/usr/bin/install -d -m 0700 "$state_dir"
if [ ! -d "$state_dir" ] || [ -L "$state_dir" ] ||
   [ "$(/usr/bin/stat -c %u:%a "$state_dir")" != 0:700 ]; then
    echo "punto-package: state directory must be root-only" >&2
    exit 66
fi
lock_file=$state_dir/transaction.lock
workflow_lock=$state_dir/workflow.lock
if [ -L "$lock_file" ] || [ -L "$workflow_lock" ]; then
    echo "punto-package: unsafe deployment lock" >&2
    exit 66
fi
workflow_inherited=0
if [ "${PUNTO_WORKFLOW_LOCK_FD:-}" = 8 ]; then
    inherited=$(/usr/bin/stat -Lc '%d:%i' /proc/self/fd/8 2>/dev/null || true)
    expected=$(/usr/bin/stat -Lc '%d:%i' "$workflow_lock" 2>/dev/null || true)
    if [ -z "$inherited" ] || [ "$inherited" != "$expected" ]; then
        echo "punto-package: invalid inherited deployment lock" >&2
        exit 66
    fi
    workflow_inherited=1
else
    exec 8>"$workflow_lock"
fi
if ! /usr/bin/flock -n 8; then
    echo "punto-package: TRANSACTION_UNRESOLVED workflow_busy=1" >&2
    exit 75
fi
if { [ -e "$state_dir/legacy-mask" ] || [ -L "$state_dir/legacy-mask" ]; } &&
   [ "$workflow_inherited" -ne 1 ]; then
    echo "punto-package: TRANSACTION_UNRESOLVED guard_reconciliation_required=1" >&2
    exit 75
fi
exec 9>"$lock_file"
if ! /usr/bin/flock -n 9; then
    echo "punto-package: TRANSACTION_UNRESOLVED lock_busy=1" >&2
    exit 75
fi
unresolved=$state_dir/unresolved
if [ -e "$unresolved" ] || [ -L "$unresolved" ]; then
    echo "punto-package: TRANSACTION_UNRESOLVED reconciliation_required=1" >&2
    exit 75
fi
case "$mode" in
    remove|purge)
        operation_name=$(/usr/bin/dpkg-deb -f "$package" Package 2>/dev/null || true)
        operation_version=$(/usr/bin/dpkg-deb -f "$package" Version 2>/dev/null || true)
        case "$operation_name" in
            ''|*[!a-z0-9+.-]*) echo "punto-package: invalid archive identity" >&2; exit 66 ;;
        esac
        case "$operation_name" in
            [a-z0-9]*) ;;
            *) echo "punto-package: invalid archive identity" >&2; exit 66 ;;
        esac
        installed_version=$(/usr/bin/dpkg-query --admindir="$admin_dir" -W \
            -f='${Version}' "$operation_name" 2>/dev/null || true)
        if [ -z "$operation_version" ] || [ "$installed_version" != "$operation_version" ]; then
            echo "punto-package: absence transaction requires exact installed archive version" >&2
            exit 66
        fi
        set -- "$@" "--$mode" "$operation_name"
        ;;
    *) set -- "$@" --install "$package" ;;
esac
if [ -n "$downgrade" ]; then
    set -- "$downgrade" "$@"
fi
umask 077
echo 'phase=dpkg_admitted outcome=unknown' >"$unresolved"
(
    DEBIAN_FRONTEND=noninteractive
    export DEBIAN_FRONTEND
    exec /usr/bin/dpkg --force-confdef --force-confold "$@" </dev/null
) &
dpkg_pid=$!
dpkg_start=$(/usr/bin/awk '{print $22}' "/proc/$dpkg_pid/stat" 2>/dev/null || true)
echo "phase=dpkg_active pid=$dpkg_pid start_ticks=${dpkg_start:-unknown} outcome=unknown" >"$unresolved"
deadline=$(( $(/usr/bin/date +%s) + observe_seconds ))
while [ -n "$dpkg_start" ] && [ -r "/proc/$dpkg_pid/stat" ]; do
    identity=$(/usr/bin/awk '{print $3 ":" $22}' "/proc/$dpkg_pid/stat" 2>/dev/null || true)
    if [ "$identity" = "Z:$dpkg_start" ] ||
       [ "${identity#*:}" != "$dpkg_start" ]; then
        break
    fi
    if [ "$(/usr/bin/date +%s)" -ge "$deadline" ]; then
        identity=$(/usr/bin/awk '{print $3 ":" $22}' "/proc/$dpkg_pid/stat" 2>/dev/null || true)
        if [ "${identity#*:}" = "$dpkg_start" ] &&
           [ "${identity%%:*}" != Z ]; then
            package_name=$(/usr/bin/dpkg-deb -f "$package" Package 2>/dev/null || true)
            package_state=unknown
            if [ -n "$package_name" ]; then
                package_state=$(/usr/bin/dpkg-query --admindir="$admin_dir" -W -f='${db:Status-Abbrev}:${Version}' \
                    "$package_name" 2>/dev/null || true)
                package_state=${package_state:-unknown}
            fi
            echo "punto-package: TRANSACTION_UNRESOLVED pid=$dpkg_pid start_ticks=${dpkg_start:-unknown} package_state=$package_state workflow_lock=$workflow_lock" >&2
            exit 75
        fi
        break
    fi
    /usr/bin/sleep 0.05
done
set +e
wait "$dpkg_pid"
result=$?
set -e
/usr/bin/rm -- "$unresolved"
exit "$result"
