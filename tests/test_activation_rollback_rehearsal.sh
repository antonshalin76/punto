#!/bin/bash
set -euo pipefail

if [[ ${PUNTO_REHEARSAL_QUIESCE_CALLBACK:-0} == 1 ]]; then
    python3 - "$@" <<'PY'
import importlib.util, json, os, pathlib, sys, time

assert sys.argv[1:] == ['quiesce-v1', 'upgrade']
path = pathlib.Path(os.environ['PUNTO_REHEARSAL_ORCHESTRATOR']).parent / 'punto-installed-lifecycle-provider.py'
loader = importlib.util.spec_from_file_location('punto_rehearsal_provider', path)
module = importlib.util.module_from_spec(loader)
loader.loader.exec_module(module)
trace = pathlib.Path(os.environ['PUNTO_REHEARSAL_TRACE'])
stopped = pathlib.Path(os.environ['PUNTO_REHEARSAL_PIPELINE_STOPPED'])
legacy = os.environ['PUNTO_REHEARSAL_PROFILE'] == 'legacy'
fault = os.environ.get('PUNTO_REHEARSAL_QUIESCE_FAULT', '')
roles = ['udevmon', 'interception', 'daemon', 'uinput', 'tray'] + ([] if legacy else ['helper'])
processes = {role: '/fixture/' + role for role in roles}
profile = {'processes': processes, 'installed': {path: role for role, path in processes.items()}}
initial = {role: [100 + i, str(1000 + i)] for i, role in enumerate(roles)}
alive = set(roles) - ({'udevmon', 'interception', 'daemon', 'uinput'} if stopped.exists() else set())
handles = {}

def record(value):
    with trace.open('a') as stream:
        stream.write(value + '\n')

class Facts:
    def writer_processes(self):
        return {initial[role][0]: {'identity': initial[role] if fault != 'drift' or role != 'tray' else [999, '9999'],
                'executable': processes[role], 'uid': 1000 if role in ('tray', 'helper') else 0,
                'deleted': False, 'sha256': role} for role in roles if role in alive}
    def pin_process(self, pid, executable, expected):
        role = next(role for role in roles if processes[role] == executable)
        assert role in alive and expected == initial[role] and pid == expected[0]
        read, write = os.pipe()
        handles[role] = (read, write)
        record('pin ' + role)
        return expected, read

class FixtureProvider(module.Provider):
    def authority(self):
        self.remaining()
    def current_profile(self):
        return 'original'
    def payload(self, name):
        assert name == 'original'
        return profile
    def user(self, argv):
        return argv
    def unit(self, unit, user=False):
        role = {'udevmon.service': 'udevmon', 'punto-tray.service': 'tray',
                'punto-session-bridge.service': 'helper'}.get(unit)
        present = role in roles and not (legacy and role == 'tray')
        return {'LoadState': 'masked' if role == 'udevmon' else 'loaded' if present else 'not-found',
                'ActiveState': 'active' if present and role in alive else 'inactive',
                'MainPID': initial[role][0] if present and role in alive else 0,
                'FragmentPath': '/dev/null' if role == 'udevmon' else '/fixture/' + unit,
                'DropInPaths': []}
    def unit_authority(self, unit, state, selected):
        if unit == 'udevmon.service':
            # Exercise the existing actual masked-/dev/null authority branch.
            return super().unit_authority(unit, state, selected)
        assert state['DropInPaths'] == [] and selected is profile
    def tray_owner(self, pid):
        assert pid == initial['tray'][0]
        return ':1.42'
    def run(self, argv):
        assert argv[-2] == 'stop'
        unit = argv[-1]
        assert len(handles) == len(roles), 'STOP before every admitted writer was pinned'
        record('stop ' + unit)
        dying = {'udevmon', 'interception', 'daemon', 'uinput'} if unit == 'udevmon.service' else {
            'helper' if unit == 'punto-session-bridge.service' else 'tray'}
        for role in dying & alive:
            alive.remove(role)
            os.close(handles[role][1])
            handles[role] = (handles[role][0], None)
        if fault == 'unknown':
            raise module.Refusal('private STOP completion unknown', 75)
        return ''

spec = {'profiles': {'original': profile}, 'initial_profile': 'original', 'initial_writers': initial,
        'legacy_tray_unit': 'punto-legacy-tray-' + 'a' * 32 + '.service'}
provider = FixtureProvider(spec, Facts(), time.monotonic() + 1)
record('callback alive=' + ','.join(role for role in roles if role in alive))
try:
    # Legacy tray is represented as the existing admitted managed transient unit.
    original_unit = provider.unit
    if legacy:
        def legacy_unit(unit, user=False):
            if unit == spec['legacy_tray_unit']:
                return {'LoadState': 'loaded', 'ActiveState': 'active' if 'tray' in alive else 'inactive',
                        'MainPID': initial['tray'][0] if 'tray' in alive else 0,
                        'FragmentPath': '/fixture/' + unit, 'DropInPaths': []}
            return original_unit(unit, user)
        provider.unit = legacy_unit
    result = provider.quiesce()
    assert result['writers_dead'] == len(roles) and not alive and not provider.pending_effect
    record('quiesce_complete')
    print(json.dumps(result))
except module.Refusal as error:
    print('actual Provider.quiesce refused: ' + str(error), file=sys.stderr)
    raise SystemExit(error.status)
finally:
    for _, write in handles.values():
        if write is not None:
            os.close(write)
PY
    exit "$?"
fi

transaction=${1:?transaction helper required}
orchestrator=${2:?activation orchestrator required}
if [[ ${PUNTO_REHEARSAL_SOURCE_ONLY:-0} == 1 ]]; then
python3 - "$transaction" "$orchestrator" "$0" <<'PY'
import pathlib, re, sys
t, o, fixture = [pathlib.Path(p).read_text() for p in sys.argv[1:]]
owners = pathlib.Path(sys.argv[2]).parent
p = (owners / 'punto-exact285-preflight.sh').read_text()
f = (owners / 'punto-exact285-files.sh').read_text()
rollback = o.split('rollback_package() {', 1)[-1].split('\n}\n', 1)[0]
release = o.split('legacy_guard_release() {', 1)[-1].split('\n}\n', 1)[0]
enter = o.split('legacy_guard_enter() {', 1)[-1].split('\n}\n', 1)[0]
reuse = o.split('if [ "$guard_owned" -eq 1 ]; then', 1)[-1].split('return 0', 1)[0]
admission = fixture.split('            admissionreplacement)\n', 1)[-1].split('            malformed)', 1)[0]
checks = {
    'remove/purge use archive-derived package identity': 'remove|purge)' in t and 'operation_name=' in t,
    'orphan legacy guard blocks standalone dpkg': 'legacy-mask' in t and 'guard_reconciliation_required=1' in t,
    'owned mask admission and release exist': 'legacy_guard_enter()' in o and 'legacy_guard_release()' in o,
    'mask is atomic, effective and ownership checked': '/usr/bin/ln -s /dev/null' in o and 'LoadState' in o and 'stat -c' in o,
    'rollback has bounded explicit postflight': 'PUNTO_ROLLBACK_POSTFLIGHT' in o and 'rollback_postflight_status=' in o,
    'rollback order fences archive and restores before release/postflight': all(x in rollback for x in ['legacy_guard_enter', '"$transaction" downgrade', '"$files_helper" restore', 'legacy_guard_release', '"$rollback_postflight" rollback']) and [rollback.index(x) for x in ['legacy_guard_enter', '"$transaction" downgrade', '"$files_helper" restore', 'legacy_guard_release', '"$rollback_postflight" rollback']] == sorted(rollback.index(x) for x in ['legacy_guard_enter', '"$transaction" downgrade', '"$files_helper" restore', 'legacy_guard_release', '"$rollback_postflight" rollback']),
    'typed UNKNOWN75 callbacks retain marker in both phases': bool(re.search(r'if \[ "\$rollback_postflight_status" -eq 75 \].*?return 75', rollback, re.S)) and bool(re.search(r'if \[ "\$activation_status" -eq 75 \].*?exit 75', o, re.S)),
    'mask identity captured before atomic publication': '/usr/bin/ln -PT' in o and o.index('>"$guard_receipt" || return 75') < o.index('/usr/bin/ln -PT'),
    'canonical root admission matches executed target in both owners': all('/usr/bin/realpath -e --' in s and s.index('/usr/bin/realpath -e --') < s.index('if [ "$' + root + '" = / ]') and '"--root=$' + root + '"' in s for s, root in [(t, 'target_root'), (o, 'root_dir')]),
    'guard release fences persistent and effective fragment drift': '"$persistent_unit"' in release and release.index('"$persistent_unit"') < release.index('/usr/bin/rm -- "$mask_path"') and 'FragmentPath' in release and release.index('FragmentPath') < release.index('/usr/bin/rm -- "$guard_receipt"'),
    'guard reuse rejects foreign persistent unit': '"$persistent_unit"' in reuse,
    'replacement oracle retains exact readonly prefix': 'expected_readonly=' in admission and '[[ -L $mask_path && -f $guard_receipt && ! -e $trace ]]' not in admission,
    'explicit baseline and fresh phases retain upgrade admission': 'baseline285|fresh)' in o and 'lifecycle_mode=upgrade' in o and 'baseline285|fresh)' in p and 'installed baseline is not configured 2.8.5' in p,
    'original and phase285 recovery snapshots stay distinct': 'original_snapshot' in o and 'snapshot-original' in f and 'snapshot-285' in f,
    'baseline projects retained legacy job rather than original job': 'project285)' in f and 'legacy/usr/share/punto-switcher/udevmon.yaml' in f,
    'quiescence precedes destructive fresh phase': 'PUNTO_QUIESCE' in o and '"$transaction" purge' in o and o.index('"$quiesce" quiesce-v1') < o.index('"$transaction" purge'),
    'fresh requires exact package absence before install': 'fresh_absence_verified=1' in o and o.index('fresh_absence_verified=1') < o.index('"$transaction" install'),
    'dpkg database observation uses the same canonical target': all('expected_admin=' in s and 'administrative directory differs from target root' in s and '"--admindir=$admin_dir"' in s for s in (t, o)),
    'runtime mask leaves all STOP/death to admitted quiescence provider': 'unit_call stop' not in enter and 'ActiveState' not in enter,
}
for name, ok in checks.items():
    print(('PASS ' if ok else 'FAIL ') + name)
assert all(checks.values()), 'P3 missing deployment contract'
PY
    exit 0
fi
if { (( EUID != 0 )) || [[ ${PUNTO_REHEARSAL_GUARD_ONLY:-0} == 1 ]]; } &&
   [[ ${PUNTO_REHEARSAL_IN_NAMESPACE:-0} != 1 ]]; then
    if [[ ${PUNTO_REHEARSAL_GUARD_ONLY:-0} == 1 ]]; then
        seam=$(mktemp -d)
        trap 'rm -rf -- "$seam"' EXIT
        : >"$seam/real-ln"
        printf '%s\n' '#!/bin/sh' \
            '"$PUNTO_REAL_LN" "$@" || exit "$?"' \
            'for argument do destination=$argument; done' \
            'if [ -n "${PUNTO_GUARD_REPLACE_DEST:-}" ] && [ "$destination" = "$PUNTO_GUARD_REPLACE_DEST" ]; then' \
            '    "$PUNTO_REAL_LN" -s /dev/null "$destination.foreign.$$" || exit "$?"' \
            '    /usr/bin/mv -T -- "$destination.foreign.$$" "$destination" || exit "$?"' \
            'fi' >"$seam/ln-seam"
        chmod 0755 "$seam/ln-seam"
        bwrap --ro-bind / / --dev /dev --proc /proc --bind /tmp /tmp \
            --ro-bind /usr/bin/ln "$seam/real-ln" --ro-bind "$seam/ln-seam" /usr/bin/ln \
            --unshare-user --unshare-pid --unshare-net --uid 0 --gid 0 \
            --die-with-parent --new-session \
            --setenv PUNTO_REHEARSAL_IN_NAMESPACE 1 --setenv PUNTO_REAL_LN "$seam/real-ln" \
            "$0" "$transaction" "$orchestrator"
        exit "$?"
    fi
    exec bwrap --ro-bind / / --dev /dev --proc /proc --bind /tmp /tmp \
        --unshare-user --unshare-pid --unshare-net --uid 0 --gid 0 \
        --die-with-parent --new-session \
        --setenv PUNTO_REHEARSAL_IN_NAMESPACE 1 \
        "$0" "$transaction" "$orchestrator"
fi
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

if [[ ${PUNTO_REHEARSAL_GUARD_ONLY:-0} == 1 ]]; then
    # Execute the actual guard functions, replacing only systemctl transport.
    # shellcheck disable=SC1090
    source <(python3 - "$orchestrator" <<'PY'
import pathlib, re, sys
s = pathlib.Path(sys.argv[1]).read_text()
for name in ['legacy_guard_matches', 'legacy_guard_enter', 'legacy_guard_release', 'controller_interrupted', 'quiesce_writers']:
    m = re.search(r'(?ms)^' + name + r'\(\) \{.*?^\}', s)
    assert m, 'missing guard function: ' + name
    print(m.group())
PY
    )
    unit_call() {
        printf '%s\n' "$*" >>"$trace"
        case "$*" in
            'show --property=LoadState --value udevmon.service')
                if [[ -L $mask_path && $ineffective == 0 ]]; then printf 'masked\n'; else printf 'loaded\n'; fi ;;
            'show --property=FragmentPath --value udevmon.service') printf '%s\n' "$effective_fragment" ;;
            'show --property=ActiveState --value udevmon.service')
                if [[ -e $pipeline_stopped ]]; then printf 'inactive\n'; else printf 'active\n'; fi ;;
            'show --property=MainPID --value udevmon.service')
                if [[ -e $pipeline_stopped ]]; then printf '0\n'; else printf '100\n'; fi ;;
            'stop udevmon.service') touch "$pipeline_stopped" ;;
            'daemon-reload') ;;
            'restart udevmon.service') [[ ! -L $mask_path ]] ;;
            *) return 88 ;;
        esac
    }
    guard_case() (
        local case_name=$1
        mkdir -m 0755 "$work/$case_name" "$work/$case_name/system" "$work/$case_name/system/units"
        mkdir -m 0700 "$work/$case_name/state"
        mask_path="$work/$case_name/system/units/udevmon.service"
        persistent_unit="$work/$case_name/persistent-unit"
        guard_receipt="$work/$case_name/state/legacy-mask"
        unresolved="$work/$case_name/state/unresolved"
        trace="$work/$case_name/trace"
        pipeline_stopped="$work/$case_name/pipeline-stopped"
        # Read by the extracted production functions.
        # shellcheck disable=SC2034
        root_dir=/ candidate_name=punto-switcher guard_owned=0 ineffective=0
        effective_fragment=/usr/lib/systemd/system/udevmon.service
        case "$case_name" in
            foreign)
                ln -s /dev/null "$mask_path"
                if legacy_guard_enter; then exit 1; else [[ $? == 66 ]]; fi
                [[ -L $mask_path && ! -e $guard_receipt && ! -e $trace ]] ;;
            precedence)
                printf 'foreign unit\n' >"$persistent_unit"
                if legacy_guard_enter; then exit 1; else [[ $? == 66 ]]; fi
                [[ ! -e $trace && ! -e $guard_receipt ]] ;;
            ineffective)
                ineffective=1
                if legacy_guard_enter; then exit 1; else [[ $? == 75 ]]; fi
                [[ -L $mask_path && -f $guard_receipt ]]
                ! grep -q '^stop ' "$trace" || exit 1 ;;
            alternate)
                # shellcheck disable=SC2034
                root_dir="$work/private-root"
                legacy_guard_enter && legacy_guard_release
                [[ ! -e $trace && ! -e $guard_receipt ]] ;;
            owned)
                legacy_guard_enter
                ! unit_call restart udevmon.service || exit 1
                ! grep -q '^stop ' "$trace" || exit 1
                legacy_guard_release
                [[ ! -e $guard_receipt && ! -L $mask_path ]]
                unit_call restart udevmon.service ;;
            replacement)
                legacy_guard_enter
                ln -s /dev/null "$work/$case_name/foreign-link"
                mv -T "$work/$case_name/foreign-link" "$mask_path"
                if legacy_guard_release; then exit 1; else [[ $? == 75 ]]; fi
                [[ -L $mask_path && -f $guard_receipt ]] ;;
            persistentdrift)
                legacy_guard_enter
                printf 'foreign unit\n' >"$persistent_unit"
                if legacy_guard_enter; then exit 1; else [[ $? == 75 ]]; fi
                if legacy_guard_release; then exit 1; else [[ $? == 75 ]]; fi
                [[ -L $mask_path && -f $guard_receipt && -f $persistent_unit ]]
                [[ $(grep -c '^daemon-reload$' "$trace") == 1 ]] ;;
            fragmentdrift)
                legacy_guard_enter
                effective_fragment=/etc/systemd/system/foreign.service
                if legacy_guard_release; then exit 1; else [[ $? == 75 ]]; fi
                [[ ! -L $mask_path && -f $guard_receipt ]]
                ! grep -q '^restart ' "$trace" || exit 1 ;;
            admissionreplacement)
                # Transport executes real ln, then replaces only this public destination
                # before the owner's next instruction. No production hook is added.
                export PUNTO_GUARD_REPLACE_DEST="$mask_path"
                if legacy_guard_enter; then exit 1; else [[ $? == 75 ]]; fi
                [[ -L $mask_path && -f $guard_receipt ]]
                expected_readonly=$'show --property=LoadState --value udevmon.service\nshow --property=FragmentPath --value udevmon.service'
                [[ $(cat "$trace") == "$expected_readonly" ]]
                [[ $(cat "$guard_receipt") != "$(stat -c %d:%i:%u:%g:%h "$mask_path")" ]] ;;
            malformed)
                legacy_guard_enter
                printf 'unknown\n' >"$guard_receipt"
                if legacy_guard_release; then exit 1; else [[ $? == 75 ]]; fi
                [[ -L $mask_path && -f $guard_receipt ]] ;;
            interrupted)
                legacy_guard_enter
                if (controller_interrupted); then exit 1; else [[ $? == 75 ]]; fi
                [[ -L $mask_path && -f $guard_receipt && -f $unresolved ]] ;;
            quiescemodern|quiescelegacy|quiescedrift|quiesceunknown)
                legacy_guard_enter
                ! unit_call restart udevmon.service || exit 1
                admitted_active=$(unit_call show --property=ActiveState --value udevmon.service)
                admitted_pid=$(unit_call show --property=MainPID --value udevmon.service)
                export PUNTO_REHEARSAL_QUIESCE_CALLBACK=1 PUNTO_REHEARSAL_ORCHESTRATOR="$orchestrator"
                export PUNTO_REHEARSAL_TRACE="$trace" PUNTO_REHEARSAL_PIPELINE_STOPPED="$pipeline_stopped"
                export PUNTO_REHEARSAL_PROFILE=modern PUNTO_REHEARSAL_QUIESCE_FAULT=
                [[ $case_name != quiescelegacy ]] || export PUNTO_REHEARSAL_PROFILE=legacy
                [[ $case_name != quiescedrift ]] || export PUNTO_REHEARSAL_QUIESCE_FAULT=drift
                [[ $case_name != quiesceunknown ]] || export PUNTO_REHEARSAL_QUIESCE_FAULT=unknown
                lifecycle_mode=upgrade quiesce="$0" activation_timeout=2
                if [[ $case_name == quiescemodern || $case_name == quiescelegacy ]]; then
                    quiesce_writers >"$work/$case_name/callback.log" 2>&1 || {
                        cat "$work/$case_name/callback.log" >&2
                        exit 1
                    }
                    [[ $admitted_active == active && $admitted_pid == 100 ]]
                    [[ ! -e $unresolved ]]
                    [[ $(grep -c '^stop udevmon.service$' "$trace") == 1 ]]
                    [[ $(grep -c '^pin ' "$trace") == $([[ $case_name == quiescelegacy ]] && echo 5 || echo 6) ]]
                    grep -q '^quiesce_complete$' "$trace"
                    printf 'snapshot_transaction_allowed\n' >>"$trace"
                    python3 - "$trace" <<'PY'
import pathlib, sys
events = pathlib.Path(sys.argv[1]).read_text().splitlines()
assert max(i for i, event in enumerate(events) if event.startswith('pin ')) < events.index('stop udevmon.service')
assert events.index('quiesce_complete') < events.index('snapshot_transaction_allowed')
PY
                else
                    if quiesce_writers >"$work/$case_name/callback.log" 2>&1; then exit 1; else [[ $? == 75 ]]; fi
                    [[ -L $mask_path && -f $guard_receipt && -f $unresolved ]]
                    ! grep -q '^snapshot_transaction_allowed$' "$trace" || exit 1
                    if [[ $case_name == quiescedrift ]]; then
                        ! grep -q '^pin ' "$trace" || exit 1
                    else
                        [[ $(grep -c '^pin ' "$trace") == 6 ]]
                        [[ $(grep -c '^stop udevmon.service$' "$trace") == 1 ]]
                    fi
                fi ;;
        esac
        printf 'PASS guard %s\n' "$case_name"
    )
    guard_failures=0
    for scenario in foreign precedence ineffective alternate owned replacement persistentdrift fragmentdrift admissionreplacement malformed interrupted quiescemodern quiescelegacy quiescedrift quiesceunknown; do
        set +e
        (set -e; guard_case "$scenario")
        guard_status=$?
        set -e
        if [[ $guard_status != 0 ]]; then
            printf 'FAIL guard %s status=%s\n' "$scenario" "$guard_status" >&2
            guard_failures=$((guard_failures + 1))
        fi
    done
    [[ $guard_failures == 0 ]]
    exit 0
fi

make_package() {
    local version=$1 payload=$2 output=$3 tree
    tree="$work/tree-$version"
    mkdir -p "$tree/DEBIAN" "$tree/etc/punto"
    printf 'Package: punto-rehearsal\nVersion: %s\nArchitecture: all\nMaintainer: test\nDescription: rehearsal\n' \
        "$version" >"$tree/DEBIAN/control"
    printf '/etc/punto/config.yaml\n' >"$tree/DEBIAN/conffiles"
    if [[ ${4:-} == fail-configure ]]; then
        printf '#!/bin/sh\nexit 47\n' >"$tree/DEBIAN/postinst"
        chmod 0755 "$tree/DEBIAN/postinst"
    fi
    printf 'upstream=%s\n' "$payload" >"$tree/etc/punto/config.yaml"
    dpkg-deb --build --root-owner-group "$tree" "$output" >/dev/null
}

root="$work/root"
mkdir -p "$root/var/lib/dpkg/updates" "$root/var/lib/dpkg/triggers" \
    "$root/var/lib/dpkg/parts" "$root/var/log" "$root/etc/punto"
: >"$root/var/lib/dpkg/status"
: >"$root/var/lib/dpkg/available"
old="$work/punto-old.deb"
new="$work/punto-new.deb"
make_package 1.0 old "$old"
make_package 2.0 new "$new"
broken="$work/punto-broken.deb"
make_package 3.0 broken "$broken" fail-configure

dpkg_args=(--root="$root" --admindir="$root/var/lib/dpkg" \
    --log="$root/var/log/dpkg.log" --force-depends)
"$transaction" install "$old" "${dpkg_args[@]}" >/dev/null
printf 'local=true\n' >"$root/etc/punto/config.yaml"
printf 'learned-sentinel\n' >"$root/etc/punto/undo_exclusions.txt"
config_hash=$(sha256sum "$root/etc/punto/config.yaml" | cut -d' ' -f1)
learned_hash=$(sha256sum "$root/etc/punto/undo_exclusions.txt" | cut -d' ' -f1)

"$transaction" remove "$old" "${dpkg_args[@]}" >/dev/null
[[ $(dpkg-query --admindir="$root/var/lib/dpkg" -W -f='${db:Status-Abbrev}' punto-rehearsal) == 'rc ' ]]
[[ $(sha256sum "$root/etc/punto/config.yaml" | cut -d' ' -f1) == "$config_hash" ]]
[[ $(sha256sum "$root/etc/punto/undo_exclusions.txt" | cut -d' ' -f1) == "$learned_hash" ]]
"$transaction" install "$old" "${dpkg_args[@]}" >/dev/null
"$transaction" purge "$old" "${dpkg_args[@]}" >/dev/null
[[ ! -e $root/etc/punto/config.yaml ]]
[[ $(sha256sum "$root/etc/punto/undo_exclusions.txt" | cut -d' ' -f1) == "$learned_hash" ]]
"$transaction" install "$old" "${dpkg_args[@]}" >/dev/null
printf 'local=true\n' >"$root/etc/punto/config.yaml"

"$transaction" install "$new" "${dpkg_args[@]}" >/dev/null
[[ $(dpkg-query --admindir="$root/var/lib/dpkg" -W -f='${db:Status-Abbrev}' punto-rehearsal) == 'ii ' ]]
[[ $(sha256sum "$root/etc/punto/config.yaml" | cut -d' ' -f1) == "$config_hash" ]]
[[ $(sha256sum "$root/etc/punto/undo_exclusions.txt" | cut -d' ' -f1) == "$learned_hash" ]]

activation_failure="$work/activation-failure"
printf '#!/bin/sh\nexit 42\n' >"$activation_failure"
chmod 0700 "$activation_failure"
orchestrator_log="$work/orchestrator.log"
if "$orchestrator" "$transaction" "$broken" "$old" \
    "$activation_failure" "${dpkg_args[@]}" \
    >"$orchestrator_log" 2>&1; then
    echo "failed configure unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'install_status=1' "$orchestrator_log"
grep -q 'package_restored=1' "$orchestrator_log"
[[ $(dpkg-query --admindir="$root/var/lib/dpkg" -W -f='${Version}' punto-rehearsal) == 1.0 ]]
[[ $(dpkg-query --admindir="$root/var/lib/dpkg" -W -f='${db:Status-Abbrev}' punto-rehearsal) == 'ii ' ]]
start=$(date +%s)
if "$orchestrator" "$transaction" "$new" "$old" \
    "$activation_failure" "${dpkg_args[@]}" \
    >"$orchestrator_log" 2>&1; then
    echo "activation failure unexpectedly succeeded" >&2
    exit 1
else
    orchestrator_status=$?
fi
(( orchestrator_status == 42 ))
(( $(date +%s) - start < 3 ))
grep -q 'activation_status=42 package_restored=1' "$orchestrator_log"
rollback_postflight="$work/rollback-postflight"
printf '#!/bin/sh\n[ "$1" = rollback-v1 ] && [ "$2" = 1.0 ] && [ "$3" = none ] || exit 49\ntouch "$PUNTO_POSTFLIGHT_MARKER"\n' >"$rollback_postflight"
chmod 0700 "$rollback_postflight"
set +e
PUNTO_ROLLBACK_POSTFLIGHT="$rollback_postflight" PUNTO_POSTFLIGHT_MARKER="$work/postflight-entered" \
    "$orchestrator" "$transaction" "$new" "$old" "$activation_failure" "${dpkg_args[@]}" \
    >"$work/postflight.log" 2>&1
postflight_status=$?
set -e
(( postflight_status == 42 ))
[[ -f $work/postflight-entered ]]
grep -q 'rollback_postflight_status=0 package_restored=1 files_restored=1 runtime_restored=unknown input_verified=unknown' "$work/postflight.log"
rollback_postflight_fail="$work/rollback-postflight-fail"
printf '#!/bin/sh\nexit 48\n' >"$rollback_postflight_fail"
chmod 0700 "$rollback_postflight_fail"
set +e
PUNTO_ROLLBACK_POSTFLIGHT="$rollback_postflight_fail" \
    "$orchestrator" "$transaction" "$new" "$old" "$activation_failure" "${dpkg_args[@]}" \
    >"$work/postflight-fail.log" 2>&1
postflight_fail_status=$?
set -e
(( postflight_fail_status == 70 ))
grep -q 'rollback_postflight_status=48 package_restored=1 files_restored=1 runtime_restored=unknown input_verified=unknown' "$work/postflight-fail.log"
! grep -q 'package_restored=0' "$work/postflight-fail.log"
[[ $(dpkg-query --admindir="$root/var/lib/dpkg" -W -f='${Version}' punto-rehearsal) == 1.0 ]]
[[ $(dpkg-query --admindir="$root/var/lib/dpkg" -W -f='${db:Status-Abbrev}' punto-rehearsal) == 'ii ' ]]
[[ $(sha256sum "$root/etc/punto/config.yaml" | cut -d' ' -f1) == "$config_hash" ]]
[[ $(sha256sum "$root/etc/punto/undo_exclusions.txt" | cut -d' ' -f1) == "$learned_hash" ]]

start=$(date +%s)
if "$orchestrator" "$transaction" "$new" "$work/missing.deb" \
    "$activation_failure" "${dpkg_args[@]}" \
    >"$orchestrator_log" 2>&1; then
    echo "missing rollback package unexpectedly succeeded" >&2
    exit 1
else
    orchestrator_status=$?
fi
(( orchestrator_status == 66 ))
(( $(date +%s) - start < 3 ))
grep -q 'candidate or rollback is not a regular file' "$orchestrator_log"
[[ $(dpkg-query --admindir="$root/var/lib/dpkg" -W -f='${Version}' punto-rehearsal) == 1.0 ]]

activation_slow="$work/activation-slow"
printf '#!/bin/sh\ntouch "$PUNTO_ACTIVATION_MARKER"\nsleep 3\n' >"$activation_slow"
chmod 0700 "$activation_slow"
PUNTO_ACTIVATION_MARKER="$work/activation-entered" \
    "$orchestrator" "$transaction" "$new" "$old" \
    "$activation_slow" "${dpkg_args[@]}" \
    >"$orchestrator_log" 2>&1 &
orchestrator_pid=$!
for (( attempt=0; attempt<100; ++attempt )); do
    [[ -e $work/activation-entered ]] && break
    sleep 0.05
done
[[ -e $work/activation-entered ]]
set +e
"$transaction" downgrade "$old" "${dpkg_args[@]}" \
    >"$work/overlap.log" 2>&1
overlap_status=$?
set -e
(( overlap_status == 75 ))
grep -q 'TRANSACTION_UNRESOLVED' "$work/overlap.log"
wait "$orchestrator_pid"
[[ $(dpkg-query --admindir="$root/var/lib/dpkg" -W -f='${Version}' punto-rehearsal) == 2.0 ]]
"$transaction" downgrade "$old" "${dpkg_args[@]}" >/dev/null

activation_hang="$work/activation-hang"
printf '#!/bin/sh\ntrap "exit 124" TERM\nwhile :; do sleep 1; done\n' \
    >"$activation_hang"
chmod 0700 "$activation_hang"
start=$(date +%s)
if PUNTO_ACTIVATION_TIMEOUT_SECONDS=1 \
    "$orchestrator" "$transaction" "$new" "$old" \
    "$activation_hang" "${dpkg_args[@]}" \
    >"$orchestrator_log" 2>&1; then
    echo "hanging activation unexpectedly succeeded" >&2
    exit 1
else
    orchestrator_status=$?
fi
(( orchestrator_status == 75 ))
(( $(date +%s) - start < 5 ))
grep -q 'ACTIVATION_UNRESOLVED status=124' "$orchestrator_log"
[[ -f $root/var/lib/dpkg/.punto-deployment/unresolved ]]
[[ $(dpkg-query --admindir="$root/var/lib/dpkg" -W -f='${Version}' punto-rehearsal) == 2.0 ]]
set +e
"$transaction" downgrade "$old" "${dpkg_args[@]}" \
    >"$work/after-activation-timeout.log" 2>&1
after_status=$?
set -e
(( after_status == 75 ))
grep -q 'reconciliation_required=1' "$work/after-activation-timeout.log"

# A separate private root keeps the prior activation-timeout receipt intact.
postflight_root="$work/postflight-root"
mkdir -p "$postflight_root/var/lib/dpkg/updates" "$postflight_root/var/lib/dpkg/triggers" \
    "$postflight_root/var/lib/dpkg/parts" "$postflight_root/var/log"
: >"$postflight_root/var/lib/dpkg/status"
: >"$postflight_root/var/lib/dpkg/available"
postflight_args=(--root="$postflight_root" --admindir="$postflight_root/var/lib/dpkg" \
    --log="$postflight_root/var/log/dpkg.log" --force-depends)
"$transaction" install "$old" "${postflight_args[@]}" >/dev/null
set +e
PUNTO_ROLLBACK_POSTFLIGHT="$activation_hang" PUNTO_ACTIVATION_TIMEOUT_SECONDS=1 \
    "$orchestrator" "$transaction" "$new" "$old" "$activation_failure" "${postflight_args[@]}" \
    >"$work/postflight-timeout.log" 2>&1
postflight_timeout_status=$?
set -e
(( postflight_timeout_status == 75 ))
grep -q 'rollback_postflight_status=124 package_restored=1 files_restored=1 runtime_restored=unknown input_verified=unknown' "$work/postflight-timeout.log"
grep -q 'phase=rollback_postflight_admitted outcome=unknown' "$postflight_root/var/lib/dpkg/.punto-deployment/unresolved"
[[ $(dpkg-query --admindir="$postflight_root/var/lib/dpkg" -W -f='${Version}' punto-rehearsal) == 1.0 ]]
set +e
"$transaction" install "$new" "${postflight_args[@]}" >"$work/after-postflight-timeout.log" 2>&1
after_postflight_status=$?
set -e
(( after_postflight_status == 75 ))
grep -q 'reconciliation_required=1' "$work/after-postflight-timeout.log"

unknown_callback="$work/unknown-callback"
printf '#!/bin/sh\nexit 75\n' >"$unknown_callback"
chmod 0700 "$unknown_callback"
for unknown_phase in activation rollback_postflight; do
    unknown_root="$work/unknown-$unknown_phase"
    mkdir -p "$unknown_root/var/lib/dpkg/updates" "$unknown_root/var/lib/dpkg/triggers" \
        "$unknown_root/var/lib/dpkg/parts" "$unknown_root/var/log"
    : >"$unknown_root/var/lib/dpkg/status"
    : >"$unknown_root/var/lib/dpkg/available"
    unknown_args=(--root="$unknown_root" --admindir="$unknown_root/var/lib/dpkg" \
        --log="$unknown_root/var/log/dpkg.log" --force-depends)
    "$transaction" install "$old" "${unknown_args[@]}" >/dev/null
    unknown_activate="$activation_failure"
    unknown_postflight="$unknown_callback"
    unknown_version=1.0
    if [[ $unknown_phase == activation ]]; then
        unknown_activate="$unknown_callback"
        unknown_postflight=
        unknown_version=2.0
    fi
    set +e
    PUNTO_ROLLBACK_POSTFLIGHT="$unknown_postflight" \
        "$orchestrator" "$transaction" "$new" "$old" "$unknown_activate" "${unknown_args[@]}" \
        >"$work/unknown-$unknown_phase.log" 2>&1
    unknown_status=$?
    set -e
    (( unknown_status == 75 ))
    grep -q "phase=${unknown_phase}_admitted outcome=unknown" "$unknown_root/var/lib/dpkg/.punto-deployment/unresolved"
    [[ $(dpkg-query --admindir="$unknown_root/var/lib/dpkg" -W -f='${Version}' punto-rehearsal) == "$unknown_version" ]]
    set +e
    "$transaction" install "$new" "${unknown_args[@]}" >"$work/after-unknown-$unknown_phase.log" 2>&1
    after_unknown_status=$?
    set -e
    (( after_unknown_status == 75 ))
    grep -q 'reconciliation_required=1' "$work/after-unknown-$unknown_phase.log"
done

echo "test_activation_rollback_rehearsal: OK"
