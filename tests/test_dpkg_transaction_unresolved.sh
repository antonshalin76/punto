#!/bin/bash
set -euo pipefail

transaction=${1:?transaction helper required}
if [[ ${PUNTO_TRANSACTION_TEST_INNER:-0} == 1 ]]; then
    test_root=$PUNTO_TEST_WORK/root
    test_admin=$test_root/var/lib/dpkg
    set +e
    "$transaction" install "$PUNTO_TEST_PACKAGE" \
        "--admindir=$PUNTO_TEST_WORK/state" --force-depends \
        >"$PUNTO_TEST_WORK/unsafe-option.log" 2>&1
    unsafe_status=$?
    set -e
    (( unsafe_status == 64 ))
    grep -q 'force-depends is forbidden' "$PUNTO_TEST_WORK/unsafe-option.log"
    [[ ! -e $PUNTO_TEST_WORK/launches ]]
    ln -s / "$PUNTO_TEST_WORK/root-alias"
    for root_alias in /. /tmp/.. "$PUNTO_TEST_WORK/root-alias"; do
        set +e
        "$transaction" install "$PUNTO_TEST_PACKAGE" "--admindir=$PUNTO_TEST_WORK/state" \
            "--root=$root_alias" --force-depends >"$PUNTO_TEST_WORK/unsafe-alias.log" 2>&1
        alias_status=$?
        set -e
        if (( alias_status != 64 )); then
            printf 'root alias admitted: root=%s status=%s\n' "$root_alias" "$alias_status" >&2
            exit 1
        fi
        grep -q 'force-depends is forbidden' "$PUNTO_TEST_WORK/unsafe-alias.log"
        [[ ! -e $PUNTO_TEST_WORK/launches ]]
    done
    for invalid_root in "$PUNTO_TEST_WORK/missing-root" "$PUNTO_TEST_PACKAGE"; do
        set +e
        "$transaction" install "$PUNTO_TEST_PACKAGE" "--admindir=$PUNTO_TEST_WORK/state" \
            "--root=$invalid_root" >"$PUNTO_TEST_WORK/invalid-root.log" 2>&1
        invalid_root_status=$?
        set -e
        (( invalid_root_status == 66 ))
        [[ ! -e $PUNTO_TEST_WORK/launches ]]
    done
    set +e
    "$transaction" install "$PUNTO_TEST_PACKAGE" "--root=$test_root" \
        "--admindir=$PUNTO_TEST_WORK/state" >"$PUNTO_TEST_WORK/mismatched-db.log" 2>&1
    mismatch_status=$?
    set -e
    (( mismatch_status == 66 ))
    grep -q 'administrative directory differs from target root' "$PUNTO_TEST_WORK/mismatched-db.log"
    [[ ! -e $PUNTO_TEST_WORK/launches && ! -e $test_admin/.punto-deployment ]]
    mkdir -m 0700 "$test_admin/.punto-deployment"
    printf 'phase=mask_admitted outcome=unknown\n' >"$test_admin/.punto-deployment/legacy-mask"
    set +e
    "$transaction" install "$PUNTO_TEST_PACKAGE" "--root=$test_root" "--admindir=$test_admin" \
        >"$PUNTO_TEST_WORK/orphan-mask.log" 2>&1
    guard_status=$?
    set -e
    (( guard_status == 75 ))
    grep -q 'guard_reconciliation_required=1' "$PUNTO_TEST_WORK/orphan-mask.log"
    [[ ! -e $PUNTO_TEST_WORK/launches ]]
    rm -- "$test_admin/.punto-deployment/legacy-mask"
    start=$(date +%s)
    set +e
    PUNTO_DPKG_OBSERVE_SECONDS=1 "$transaction" install "$PUNTO_TEST_PACKAGE" \
        "--root=$test_root" \
        >"$PUNTO_TEST_WORK/first.log" 2>&1
    first_status=$?
    set -e
    if (( first_status != 75 || $(date +%s) - start >= 3 )); then
        echo "expected bounded unresolved result; status=$first_status" >&2
        tail -n 12 "$PUNTO_TEST_WORK/first.log" >&2
        exit 1
    fi
    grep -q 'TRANSACTION_UNRESOLVED' "$PUNTO_TEST_WORK/first.log"
    set +e
    PUNTO_DPKG_OBSERVE_SECONDS=1 "$transaction" install "$PUNTO_TEST_PACKAGE" \
        "--root=$test_root" "--admindir=$test_admin" \
        >"$PUNTO_TEST_WORK/second.log" 2>&1
    second_status=$?
    set -e
    if (( second_status != 75 )); then
        echo "competing dpkg was admitted; status=$second_status" >&2
        exit 1
    fi
    grep -q 'TRANSACTION_UNRESOLVED' "$PUNTO_TEST_WORK/second.log"
    sleep 3
    [[ -e $PUNTO_TEST_WORK/completed ]]
    [[ ! -e $PUNTO_TEST_WORK/signalled ]]
    [[ $(wc -l <"$PUNTO_TEST_WORK/launches") == 1 ]]
    [[ -f $test_admin/.punto-deployment/unresolved ]]
    set +e
    PUNTO_DPKG_OBSERVE_SECONDS=1 "$transaction" install "$PUNTO_TEST_PACKAGE" \
        "--root=$test_root" "--admindir=$test_admin" \
        >"$PUNTO_TEST_WORK/after-child.log" 2>&1
    after_status=$?
    set -e
    (( after_status == 75 ))
    grep -q 'reconciliation_required=1' "$PUNTO_TEST_WORK/after-child.log"
    [[ $(wc -l <"$PUNTO_TEST_WORK/launches") == 1 ]]
    echo 'test_dpkg_transaction_unresolved: OK'
    exit 0
fi

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
printf 'dummy\n' >"$work/package.deb"
printf '%s\n' '#!/bin/sh' \
    'last_root= last_admin=' \
    'for argument do case "$argument" in --root=*) last_root=${argument#--root=} ;; --admindir=*) last_admin=${argument#--admindir=} ;; esac; done' \
    '[ "$last_root" = "$PUNTO_TEST_WORK/root" ] && [ "$last_admin" = "$PUNTO_TEST_WORK/root/var/lib/dpkg" ] || exit 83' \
    'trap '\''touch "$PUNTO_TEST_WORK/signalled"'\'' TERM INT' \
    'printf "launch\\n" >>"$PUNTO_TEST_WORK/launches"' \
    'sleep 3' \
    'touch "$PUNTO_TEST_WORK/completed"' >"$work/dpkg"
chmod 0755 "$work/dpkg"
mkdir -m 0700 "$work/state"
mkdir -p "$work/root/var/lib/dpkg"
bwrap --ro-bind / / --dev /dev --proc /proc --bind /tmp /tmp \
    --bind "$work/dpkg" /usr/bin/dpkg \
    --unshare-user --unshare-pid --unshare-net --uid 0 --gid 0 \
    --new-session --setenv PUNTO_TRANSACTION_TEST_INNER 1 \
    --setenv PUNTO_TEST_WORK "$work" \
    --setenv PUNTO_TEST_PACKAGE "$work/package.deb" \
    /bin/bash "$0" "$transaction"
