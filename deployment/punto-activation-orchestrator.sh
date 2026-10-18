#!/bin/sh
set -eu

lifecycle_mode=upgrade
exact285=
case "${1:-}" in
    baseline285|fresh)
        lifecycle_mode=$1
        shift
        [ "$#" -ge 5 ] || exit 64
        transaction=$1 candidate=$2 exact285=$3 rollback=$4 activate=$5
        shift 5
        ;;
    *)
if [ "$#" -lt 4 ]; then
    echo "punto-activation: usage: TRANSACTION CANDIDATE ROLLBACK ACTIVATE [dpkg options]" >&2
    exit 64
fi
transaction=$1
candidate=$2
rollback=$3
activate=$4
shift 4
        ;;
esac
activation_timeout=${PUNTO_ACTIVATION_TIMEOUT_SECONDS:-60}
rollback_postflight=${PUNTO_ROLLBACK_POSTFLIGHT:-}
quiesce=${PUNTO_QUIESCE:-}
original_snapshot=

if [ ! -f "$transaction" ] || [ -L "$transaction" ] ||
   [ ! -x "$transaction" ] || [ ! -f "$activate" ] ||
   [ -L "$activate" ] || [ ! -x "$activate" ]; then
    echo "punto-activation: helper is not an executable regular file" >&2
    exit 66
fi
if [ ! -f "$candidate" ] || [ -L "$candidate" ] ||
   [ ! -f "$rollback" ] || [ -L "$rollback" ]; then
    echo "punto-activation: candidate or rollback is not a regular file" >&2
    exit 66
fi
if [ -n "$rollback_postflight" ] &&
   { [ ! -f "$rollback_postflight" ] || [ -L "$rollback_postflight" ] ||
     [ ! -x "$rollback_postflight" ]; }; then
    echo "punto-activation: rollback postflight is not an executable regular file" >&2
    exit 66
fi
if [ -n "$quiesce" ] &&
   { [ ! -f "$quiesce" ] || [ -L "$quiesce" ] || [ ! -x "$quiesce" ]; }; then
    echo "punto-activation: quiescence provider is not an executable regular file" >&2
    exit 66
fi
if [ "$lifecycle_mode" != upgrade ]; then
    if [ ! -f "$exact285" ] || [ -L "$exact285" ] ||
       [ ! -f "$quiesce" ] || [ -L "$quiesce" ] || [ ! -x "$quiesce" ]; then
        echo "punto-activation: lifecycle requires exact legacy archive and writer-quiescence provider" >&2
        exit 66
    fi
fi
case "$activation_timeout" in
    ''|*[!0-9]*)
        echo "punto-activation: invalid activation timeout" >&2
        exit 64
        ;;
esac
if [ "$activation_timeout" -lt 1 ] || [ "$activation_timeout" -gt 120 ]; then
    echo "punto-activation: activation timeout is outside 1..120 seconds" >&2
    exit 64
fi

candidate_name=$(/usr/bin/dpkg-deb -f "$candidate" Package 2>/dev/null || true)
rollback_name=$(/usr/bin/dpkg-deb -f "$rollback" Package 2>/dev/null || true)
rollback_version=$(/usr/bin/dpkg-deb -f "$rollback" Version 2>/dev/null || true)
if [ -z "$candidate_name" ] || [ "$candidate_name" != "$rollback_name" ] ||
   [ -z "$rollback_version" ]; then
    echo "punto-activation: package identity mismatch" >&2
    exit 66
fi

admin_dir=/var/lib/dpkg
admin_explicit=0
root_dir=/
for argument do
    case "$argument" in
        --admindir=*) admin_dir=${argument#--admindir=}; admin_explicit=1 ;;
        --root=*) root_dir=${argument#--root=} ;;
        --log=*|--force-depends) ;;
        *) echo "punto-activation: unsupported dpkg option" >&2; exit 64 ;;
    esac
done
if [ ! -d "$root_dir" ]; then
    echo "punto-activation: target root is not an existing directory" >&2
    exit 66
fi
root_dir=$(/usr/bin/realpath -e -- "$root_dir") || exit 66
if [ "$root_dir" = / ]; then
    for argument do
        if [ "$argument" = --force-depends ]; then
            echo "punto-activation: force-depends is forbidden on the live root" >&2
            exit 64
        fi
    done
fi
expected_admin=${root_dir%/}/var/lib/dpkg
if [ "$admin_explicit" -eq 0 ]; then admin_dir=$expected_admin; fi
admin_dir=$(/usr/bin/realpath -e -- "$admin_dir") || exit 66
if [ ! -d "$admin_dir" ] || [ "$admin_dir" != "$expected_admin" ]; then
    echo "punto-activation: administrative directory differs from target root" >&2
    exit 66
fi
set -- "$@" "--root=$root_dir" "--admindir=$admin_dir"
state_dir=/run/punto-deployment
if [ "$admin_dir" != /var/lib/dpkg ]; then
    state_dir=$admin_dir/.punto-deployment
fi
if [ -L "$state_dir" ]; then
    echo "punto-activation: unsafe state directory" >&2
    exit 66
fi
/usr/bin/install -d -m 0700 "$state_dir"
if [ ! -d "$state_dir" ] || [ -L "$state_dir" ] ||
   [ "$(/usr/bin/stat -c %u:%a "$state_dir")" != 0:700 ] ||
   [ -L "$state_dir/workflow.lock" ]; then
    echo "punto-activation: unsafe deployment lock" >&2
    exit 66
fi
exec 8>"$state_dir/workflow.lock"
if ! /usr/bin/flock -n 8; then
    echo "punto-activation: TRANSACTION_UNRESOLVED workflow_busy=1" >&2
    exit 75
fi
unresolved=$state_dir/unresolved
if [ -e "$unresolved" ] || [ -L "$unresolved" ] ||
   [ -e "$state_dir/legacy-mask" ] || [ -L "$state_dir/legacy-mask" ]; then
    echo "punto-activation: TRANSACTION_UNRESOLVED reconciliation_required=1" >&2
    exit 75
fi
PUNTO_WORKFLOW_LOCK_FD=8
export PUNTO_WORKFLOW_LOCK_FD

# The archived postinst calls systemctl directly; policy-rc.d cannot fence it.
mask_path=/run/systemd/system/udevmon.service
persistent_unit=/etc/systemd/system/udevmon.service
guard_receipt=$state_dir/legacy-mask
guard_owned=0

unit_call() {
    /usr/bin/timeout --signal=TERM --kill-after=2s "${activation_timeout}s" \
        /usr/bin/systemctl --no-pager "$@"
}

legacy_guard_matches() {
    [ -L "$mask_path" ] && [ "$(/usr/bin/readlink "$mask_path")" = /dev/null ] &&
        [ -f "$guard_receipt" ] && [ ! -L "$guard_receipt" ] &&
        [ "$(/usr/bin/stat -c %u:%g:%a:%h "$guard_receipt")" = 0:0:600:1 ] &&
        [ "$(/usr/bin/stat -c %u:%g:%h "$mask_path")" = 0:0:1 ] &&
        [ "$(/usr/bin/cat "$guard_receipt")" = "$(/usr/bin/stat -c %d:%i:%u:%g:%h "$mask_path")" ]
}

legacy_guard_enter() {
    [ "$root_dir" = / ] && [ "$candidate_name" = punto-switcher ] || return 0
    if [ "$guard_owned" -eq 1 ]; then
        [ ! -e "$persistent_unit" ] && [ ! -L "$persistent_unit" ] || return 75
        legacy_guard_matches || return 75
        [ "$(unit_call show --property=LoadState --value udevmon.service)" = masked ] || return 75
        return 0
    fi
    if [ -e "$persistent_unit" ] || [ -L "$persistent_unit" ] ||
       [ -e "$mask_path" ] || [ -L "$mask_path" ] ||
       [ -e "$guard_receipt" ] || [ -L "$guard_receipt" ]; then
        echo "punto-activation: foreign legacy unit or mask" >&2
        return 66
    fi
    mask_parent=$(dirname -- "$mask_path")
    for directory in "$(dirname -- "$mask_parent")" "$mask_parent"; do
        [ -d "$directory" ] && [ ! -L "$directory" ] &&
            [ "$(/usr/bin/stat -c %u:%g:%a "$directory")" = 0:0:755 ] || return 66
    done
    [ "$(unit_call show --property=LoadState --value udevmon.service)" = loaded ] || return 66
    fragment=$(unit_call show --property=FragmentPath --value udevmon.service) || return 66
    case "$fragment" in
        /usr/lib/systemd/system/udevmon.service|/lib/systemd/system/udevmon.service) ;;
        *) echo "punto-activation: foreign legacy unit fragment" >&2; return 66 ;;
    esac
    umask 077
    (set -C; echo 'phase=mask_admitted outcome=unknown' >"$guard_receipt") || return 75
    guard_owned=1
    # Capture the private inode before atomic publication; never adopt a replacement.
    guard_link=$guard_receipt.link
    /usr/bin/ln -s /dev/null "$guard_link" || return 75
    /usr/bin/stat -c %d:%i:%u:%g:%h "$guard_link" >"$guard_receipt" || return 75
    /usr/bin/ln -PT "$guard_link" "$mask_path" || return 75
    /usr/bin/rm -- "$guard_link" || return 75
    legacy_guard_matches || return 75
    unit_call daemon-reload || return 75
    [ "$(unit_call show --property=LoadState --value udevmon.service)" = masked ] || return 75
    # Masking prevents future starts; it does not stop the existing pipeline.
}

legacy_guard_release() {
    [ "$root_dir" = / ] && [ "$candidate_name" = punto-switcher ] || return 0
    [ "$guard_owned" -eq 1 ] && legacy_guard_matches || return 75
    [ ! -e "$persistent_unit" ] && [ ! -L "$persistent_unit" ] || return 75
    /usr/bin/rm -- "$mask_path" || return 75
    unit_call daemon-reload || return 75
    [ "$(unit_call show --property=LoadState --value udevmon.service)" = loaded ] || return 75
    released_fragment=$(unit_call show --property=FragmentPath --value udevmon.service) || return 75
    [ "$released_fragment" = "$fragment" ] || return 75
    /usr/bin/rm -- "$guard_receipt" || return 75
    guard_owned=0
}

# Invoked by trap, not by normal control flow.
# shellcheck disable=SC2317
controller_interrupted() {
    if [ ! -e "$unresolved" ] && [ ! -L "$unresolved" ]; then
        umask 077
        echo 'phase=controller_interrupted outcome=unknown' >"$unresolved"
    fi
    echo "punto-activation: TRANSACTION_UNRESOLVED controller_interrupted=1" >&2
    exit 75
}
trap controller_interrupted HUP INT TERM

quiesce_writers() {
    [ "$candidate_name" = punto-switcher ] && [ -n "$quiesce" ] || return 0
    umask 077
    echo 'phase=quiescence_admitted outcome=unknown' >"$unresolved" || return 75
    set +e
    /usr/bin/timeout --signal=TERM --kill-after=2s "${activation_timeout}s" \
        "$quiesce" quiesce-v1 "$lifecycle_mode" "$@" </dev/null
    quiescence_status=$?
    set -e
    if [ "$quiescence_status" -ne 0 ]; then
        echo "punto-activation: TRANSACTION_UNRESOLVED quiescence_status=$quiescence_status" >&2
        return 75
    fi
    /usr/bin/rm -- "$unresolved"
}

if [ "$candidate_name" = punto-switcher ]; then
    if [ "$lifecycle_mode" = upgrade ] && [ "$rollback_version" != 2.8.5 ]; then
        echo "punto-activation: supported rollback must be exact 2.8.5" >&2
        exit 66
    fi
    preflight=$(dirname -- "$0")/punto-exact285-preflight.sh
    [ -f "$preflight" ] && [ ! -L "$preflight" ] || {
        echo "punto-activation: exact-legacy preflight is unavailable" >&2
        exit 66
    }
    if [ "$lifecycle_mode" = upgrade ]; then
        /bin/sh "$preflight" "$rollback" "$candidate" "$root_dir" || exit 66
        exact285=$rollback
    else
        /bin/sh "$preflight" "$lifecycle_mode" "$exact285" "$candidate" "$rollback" "$root_dir" || exit 66
    fi
    files_helper=$(dirname -- "$0")/punto-exact285-files.sh
    [ -f "$files_helper" ] && [ ! -L "$files_helper" ] || {
        echo "punto-activation: exact-legacy file helper is unavailable" >&2
        exit 66
    }
    if [ "$root_dir" = / ] &&
       { [ ! -f "$quiesce" ] || [ -L "$quiesce" ] || [ ! -x "$quiesce" ]; }; then
        echo "punto-activation: live upgrade requires writer-quiescence provider" >&2
        exit 66
    fi
elif [ "$lifecycle_mode" != upgrade ]; then
    echo "punto-activation: lifecycle mode requires punto-switcher" >&2
    exit 66
elif [ "$root_dir" = / ]; then
    echo "punto-activation: foreign package on host is not supported" >&2
    exit 66
fi

if legacy_guard_enter; then
    :
else
    exit "$?"
fi

quiesce_writers || exit 75
if [ "$candidate_name" = punto-switcher ] && [ "$lifecycle_mode" = upgrade ]; then
    legacy_snapshot=$(/bin/sh "$files_helper" snapshot-285 "$root_dir" \
        "$rollback" "$candidate" auto) || {
        echo 'phase=snapshot_incomplete outcome=unknown' >"$unresolved"
        exit 75
    }
    echo "punto-activation: recovery_snapshot=$legacy_snapshot kind=S-285"
elif [ "$lifecycle_mode" != upgrade ]; then
    original_snapshot=$(/bin/sh "$files_helper" snapshot-original "$root_dir" \
        "$exact285" "$rollback" auto) || {
        echo 'phase=snapshot_incomplete outcome=unknown' >"$unresolved"
        exit 75
    }
    legacy_snapshot=$original_snapshot
    rollback=$original_snapshot/recovery.deb
    echo "punto-activation: recovery_snapshot=$original_snapshot kind=S-original"
fi

set +e
if [ "$lifecycle_mode" = baseline285 ]; then
    "$transaction" downgrade "$exact285" "$@"
    install_status=$?
else
    if [ "$lifecycle_mode" = fresh ]; then
        "$transaction" purge "$rollback" "$@"
        purge_status=$?
        if [ "$purge_status" -eq 75 ]; then exit 75; fi
        if [ "$purge_status" -ne 0 ]; then
            install_status=$purge_status
        else
            # Query the whole installed catalogue: a missing-name exit must
            # not be confused with a failed database observation.
            if ! package_catalog=$(/usr/bin/dpkg-query --admindir="$admin_dir" -W \
                -f='${Package}\n' 2>/dev/null); then
                echo 'phase=fresh_package_catalogue_unavailable outcome=unknown' >"$unresolved"
                exit 75
            fi
            if printf '%s\n' "$package_catalog" | /usr/bin/grep -Fxq -- "$candidate_name"; then
                echo 'phase=fresh_absence_unverified outcome=unknown' >"$unresolved"
                exit 75
            fi
            for conffile in etc/punto/config.yaml etc/xdg/autostart/punto-tray.desktop \
                            etc/xdg/autostart/punto-session-bridge.desktop; do
                if [ -e "$root_dir/$conffile" ] || [ -L "$root_dir/$conffile" ]; then
                    echo 'phase=fresh_conffile_absence_unverified outcome=unknown' >"$unresolved"
                    exit 75
                fi
            done
            echo 'punto-activation: fresh_absence_verified=1'
            "$transaction" install "$candidate" "$@"
            install_status=$?
        fi
    else
        "$transaction" install "$candidate" "$@"
        install_status=$?
    fi
fi
set -e
if [ "$install_status" -eq 75 ]; then
    echo "punto-activation: TRANSACTION_UNRESOLVED install_status=75" >&2
    exit 75
fi

rollback_package() {
    if ! legacy_guard_enter; then
        echo "punto-activation: TRANSACTION_UNRESOLVED rollback_guard=failed" >&2
        return 75
    fi
    quiesce_writers rollback || return 75
    if [ -n "${legacy_snapshot:-}" ] &&
       ! /bin/sh "$files_helper" verify "$root_dir" "$exact285" "$candidate" "$legacy_snapshot"; then
        echo 'phase=recovery_snapshot_unverified outcome=unknown' >"$unresolved"
        return 75
    fi
    set +e
    "$transaction" downgrade "$rollback" "$@"
    rollback_status=$?
    set -e
    if [ "$rollback_status" -eq 75 ]; then
        echo "punto-activation: TRANSACTION_UNRESOLVED rollback_status=75" >&2
        return 75
    fi
    if [ "$rollback_status" -ne 0 ]; then
        echo "punto-activation: package_restored=0 rollback_status=$rollback_status" >&2
        return 70
    fi
    restored=$(/usr/bin/dpkg-query --admindir="$admin_dir" -W \
        -f='${db:Status-Abbrev}:${Version}' "$rollback_name" 2>/dev/null || true)
    if [ "$restored" != "ii :$rollback_version" ]; then
        echo "punto-activation: package_restored=0 package_state=${restored:-unknown}" >&2
        return 70
    fi
    if [ -n "${legacy_snapshot:-}" ]; then
        if ! /bin/sh "$files_helper" restore "$root_dir" "$exact285" \
            "$candidate" "$legacy_snapshot"; then
            echo "punto-activation: package_restored=1 files_restored=0 runtime_restored=unknown input_verified=unknown" >&2
            return 70
        fi
    fi
    if ! legacy_guard_release; then
        echo "punto-activation: TRANSACTION_UNRESOLVED rollback_guard_release=failed" >&2
        return 75
    fi
    if [ -n "$rollback_postflight" ]; then
        umask 077
        echo 'phase=rollback_postflight_admitted outcome=unknown' >"$unresolved" || return 75
        set +e
        /usr/bin/timeout --signal=TERM --kill-after=2s "${activation_timeout}s" \
            "$rollback_postflight" rollback-v1 "$rollback_version" "${legacy_snapshot:-none}" </dev/null
        rollback_postflight_status=$?
        set -e
        echo "punto-activation: rollback_postflight_status=$rollback_postflight_status package_restored=1 files_restored=1 runtime_restored=unknown input_verified=unknown" >&2
        if [ "$rollback_postflight_status" -eq 75 ] ||
           [ "$rollback_postflight_status" -eq 124 ] || [ "$rollback_postflight_status" -eq 137 ]; then
            return 75
        fi
        /usr/bin/rm -- "$unresolved"
        [ "$rollback_postflight_status" -eq 0 ] || return 70
    fi
    echo "punto-activation: package_restored=1 files_restored=1 runtime_restored=unknown input_verified=unknown" >&2
    return 0
}

if [ "$install_status" -ne 0 ]; then
    if rollback_package "$@"; then
        echo "punto-activation: install_status=$install_status package_restored=1" >&2
        exit "$install_status"
    else
        exit "$?"
    fi
fi

if [ -n "${legacy_snapshot:-}" ]; then
    projection=migrate
    if [ "$lifecycle_mode" = baseline285 ]; then projection=project285; fi
    if ! /bin/sh "$files_helper" "$projection" "$root_dir" "$exact285" \
        "$candidate" "$legacy_snapshot"; then
        echo "punto-activation: migration_status=failed" >&2
        if rollback_package "$@"; then
            exit 66
        else
            exit "$?"
        fi
    fi
    if [ "$lifecycle_mode" = baseline285 ]; then
        if ! /bin/sh "$preflight" "$exact285" "$candidate" "$root_dir"; then
            if rollback_package "$@"; then exit 66; else exit "$?"; fi
        fi
    fi
fi

if ! legacy_guard_release; then
    echo "punto-activation: TRANSACTION_UNRESOLVED candidate_guard_release=failed" >&2
    exit 75
fi

umask 077
echo 'phase=activation_admitted outcome=unknown' >"$unresolved"
set +e
/usr/bin/timeout --signal=TERM --kill-after=2s "${activation_timeout}s" \
    "$activate" </dev/null
activation_status=$?
set -e
if [ "$activation_status" -eq 0 ]; then
    /usr/bin/rm -- "$unresolved"
    echo "punto-activation: activation=ok"
    exit 0
fi
if [ "$activation_status" -eq 75 ] ||
   [ "$activation_status" -eq 124 ] || [ "$activation_status" -eq 137 ]; then
    echo "punto-activation: ACTIVATION_UNRESOLVED status=$activation_status package_restored=0 runtime_restored=unknown input_verified=unknown" >&2
    exit 75
fi

/usr/bin/rm -- "$unresolved"

if rollback_package "$@"; then
    echo "punto-activation: activation_status=$activation_status package_restored=1" >&2
    exit "$activation_status"
else
    rollback_result=$?
fi

echo "punto-activation: activation_status=$activation_status rollback_status=$rollback_result" >&2
exit "$rollback_result"
