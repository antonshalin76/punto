#!/usr/bin/env bash

set -euo pipefail

if [[ ${GITHUB_ACTIONS:-} != true ]]; then
    printf 'ERROR: the Punto Bubblewrap AppArmor profile is CI-only\n' >&2
    exit 2
fi

restriction=/proc/sys/kernel/apparmor_restrict_unprivileged_userns
if [[ ! -r $restriction || $(<"$restriction") != 1 ]]; then
    exit 0
fi

profile_source=${BASH_SOURCE[0]%/*}/../apparmor/punto-ci-bwrap
if [[ ! -f $profile_source || -L $profile_source ]]; then
    printf 'ERROR: missing regular Punto Bubblewrap AppArmor profile\n' >&2
    exit 1
fi
if [[ ! -x /usr/sbin/apparmor_parser ]]; then
    printf 'ERROR: apparmor_parser is required on restricted GitHub runners\n' >&2
    exit 1
fi

sudo /usr/sbin/apparmor_parser -r -K "$profile_source"
