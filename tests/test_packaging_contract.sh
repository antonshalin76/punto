#!/usr/bin/env bash
# RED contract for B14/B15/B21 packaging behaviour.
# Builds run only in disposable copies under bubblewrap; packages are never installed.

set -u -o pipefail
export LC_ALL=C.UTF-8

REPO_ROOT=$(cd "${BASH_SOURCE[0]%/*}/.." && pwd)
EXPECTED_VERSION=""
VERSION_VALID=0
MUTATED_VERSION=9.8.7
SOURCE_EPOCH=1704067200
DEFAULT_SOURCE_EPOCH=1788566400
failures=0
checks=0
tmp_root=""

REQUIRED_PACKAGES=(
    build-essential cmake pkg-config libhunspell-dev libyaml-cpp-dev
    libsystemd-dev libxcb1-dev libxcb-xkb-dev libxau-dev
    dpkg-dev binutils file
)
REQUIRED_TOOLS=(
    cmake dpkg dpkg-query dpkg-deb dpkg-shlibdeps pkg-config
    file strip gzip sha256sum
)
TRAY_PACKAGES=(libgtk-3-dev libayatana-appindicator3-dev)
DICTIONARY_PACKAGES=(hunspell hunspell-en-us hunspell-ru wamerican-huge)
RUNTIME_PACKAGES=(
    interception-tools libgtk-3-0 libayatana-appindicator3-1
    netcat-openbsd passwd sudo util-linux
)
declare -A REQUIRED_TOOL_PROVIDERS=(
    [cmake]=cmake
    [dpkg]=dpkg
    [dpkg-query]=dpkg
    [dpkg-deb]=dpkg
    [dpkg-shlibdeps]=dpkg-dev
    [pkg-config]=pkg-config
    [file]=file
    [strip]=binutils
    [gzip]=gzip
    [sha256sum]=coreutils
)
declare -A BASE_BUILD_TOOL_PROVIDERS=(
    [bash]=bash [chmod]=coreutils [cp]=coreutils [cut]=coreutils
    [date]=coreutils [du]=coreutils [head]=coreutils [install]=coreutils
    [mkdir]=coreutils [mktemp]=coreutils [mv]=coreutils [nproc]=coreutils
    [rm]=coreutils [sort]=coreutils [stat]=coreutils [tr]=coreutils
    [find]=findutils [grep]=grep [sed]=sed [awk]=mawk [tar]=tar
)
BASE_BUILD_TOOLS=(
    bash chmod cp cut date du head install mkdir mktemp mv nproc rm sort stat tr
    find grep sed awk tar
)
TEST_ONLY_TOOLS=(
    bwrap strace python3 busybox ldd readelf realpath timeout strings cmp readlink ln chroot
)
declare -A TEST_TOOL_PROVIDERS=(
    [bwrap]=bubblewrap [strace]=strace [python3]=python3 [busybox]=busybox-static [ldd]=libc-bin
    [readelf]=binutils [realpath]=coreutils [timeout]=coreutils
    [strings]=binutils [cmp]=diffutils [readlink]=coreutils [ln]=coreutils
    [chroot]=coreutils
)

pass() {
    checks=$((checks + 1))
    printf 'PASS: %s\n' "$1"
}

fail() {
    checks=$((checks + 1))
    failures=$((failures + 1))
    printf 'FAIL: %s\n' "$1" >&2
}

strip_ansi() {
    sed $'s/\033\\[[0-9;]*m//g'
}

assert_zero() {
    local rc=$1 message=$2
    if [[ $rc -eq 0 ]]; then pass "$message"; else fail "$message (rc=$rc)"; fi
}

assert_nonzero() {
    local rc=$1 message=$2
    if [[ $rc -ge 1 && $rc -le 125 ]]; then
        pass "$message"
    else
        fail "$message (rc=$rc)"
    fi
}

assert_file() {
    local path=$1 message=$2
    if [[ -f $path ]]; then pass "$message"; else fail "$message (missing $path)"; fi
}

assert_executable() {
    local path=$1 message=$2
    if [[ -f $path && -x $path ]]; then pass "$message"; else fail "$message ($path)"; fi
}

assert_contains() {
    local haystack=$1 needle=$2 message=$3
    if grep -Fq -- "$needle" <<<"$haystack"; then pass "$message"; else fail "$message (missing: $needle)"; fi
}

assert_not_contains() {
    local haystack=$1 needle=$2 message=$3
    if grep -Fq -- "$needle" <<<"$haystack"; then fail "$message (found: $needle)"; else pass "$message"; fi
}

assert_file_not_contains() {
    local path=$1 needle=$2 message=$3
    if [[ ! -f $path || -L $path ]]; then
        fail "$message (missing regular non-symlink file: $path)"
    elif grep -Fq -- "$needle" "$path"; then
        fail "$message (found: $needle)"
    else
        pass "$message"
    fi
}

assert_matches() {
    local haystack=$1 pattern=$2 message=$3
    if grep -Eiq -- "$pattern" <<<"$haystack"; then pass "$message"; else fail "$message (pattern: $pattern)"; fi
}

assert_exact_line() {
    local haystack=$1 expected=$2 message=$3 plain
    plain=$(strip_ansi <<<"$haystack")
    if grep -Fxq -- "$expected" <<<"$plain"; then pass "$message"; else fail "$message (missing exact line: $expected)"; fi
}

assert_no_mutating_execve() {
    local trace=$1 label=$2 violations parser_rc
    if [[ ! -f $trace ]]; then
        fail "$label has no execve audit trace"
        return
    fi
    set +e
    violations=$(/usr/bin/python3 - "$trace" <<'PY'
import pathlib
import re
import sys
import ast

mutators = {
    "sudo", "apt", "apt-get", "aptitude", "systemctl", "service",
    "update-rc.d", "invoke-rc.d", "deb-systemd-invoke", "useradd",
    "groupadd", "usermod", "adduser", "addgroup", "pkill", "kill",
    "killall", "install-system-service", "fakeroot",
}
trace = pathlib.Path(sys.argv[1])
approved_dpkg_paths = {
    pathlib.Path("/usr/bin/dpkg"),
    trace.parent / "bin" / "dpkg",
    trace.parent / "repo" / ".contract-bin" / "dpkg",
}
for line in trace.read_text(encoding="utf-8", errors="replace").splitlines():
    match = re.search(r'execve\("([^"]+)", \[([^]]*)\]', line)
    if not match:
        continue
    name = pathlib.PurePosixPath(match.group(1)).name
    arguments = match.group(2)
    if name == "dpkg":
        try:
            argv = ast.literal_eval(f"[{arguments}]")
        except (SyntaxError, ValueError):
            print(line)
            continue
        path = pathlib.Path(match.group(1))
        allowed_argv = (
            len(argv) == 2 and argv[1] == "--print-architecture"
        ) or (
            len(argv) == 3
            and argv[1] in {"-s", "--status", "--validate-version"}
        )
        if path in approved_dpkg_paths and allowed_argv:
            continue
        print(line)
    elif name in mutators:
        print(line)
PY
    )
    parser_rc=$?
    set +e
    if [[ $parser_rc -ne 0 ]]; then
        fail "$label execve audit parser failed closed (rc=$parser_rc)"
        return
    fi
    if [[ -z $violations ]]; then
        pass "$label execve audit contains no host mutation attempt"
    else
        fail "$label attempted a mutating executable: ${violations//$'\n'/; }"
    fi
}

parse_canonical_version_file() {
    /usr/bin/python3 - "$1" <<'PY'
import pathlib
import re
import sys

data = pathlib.Path(sys.argv[1]).read_bytes()
pattern = rb"[0-9]+(?:[.][0-9]+){2}"
if not (2 <= len(data) <= 128 and data.endswith(b"\n") and data.count(b"\n") == 1):
    raise SystemExit(2)
value = data[:-1]
if re.fullmatch(pattern, value) is None:
    raise SystemExit(2)
sys.stdout.write(value.decode("ascii"))
PY
}

load_root_version() {
    local version_file="$REPO_ROOT/VERSION" parsed parse_rc
    if [[ ! -f $version_file || -L $version_file ]]; then
        fail "B15: repository VERSION is an existing regular non-symlink file"
        return
    fi
    set +e
    parsed=$(parse_canonical_version_file "$version_file" 2>/dev/null)
    parse_rc=$?
    set +e
    EXPECTED_VERSION=$parsed
    if [[ $parse_rc -ne 0 ]] || \
       ! dpkg --validate-version "$EXPECTED_VERSION" >/dev/null 2>&1; then
        fail "B15: repository VERSION is one LF-terminated valid Debian version"
        EXPECTED_VERSION=""
        return
    fi
    VERSION_VALID=1
    if [[ $MUTATED_VERSION == "$EXPECTED_VERSION" ]]; then
        MUTATED_VERSION=9.8.8
    fi
    pass "B15: repository VERSION is one LF-terminated valid Debian version"
}

cleanup() {
    if [[ ${PUNTO_CONTRACT_KEEP_TMP:-0} == 1 ]]; then
        printf 'NOTE: retained packaging workspace: %s\n' "$tmp_root" >&2
        return
    fi
    if [[ -n ${tmp_root:-} && $tmp_root == /tmp/punto-packaging-contract.* ]]; then
        chmod -R u+w -- "$tmp_root" 2>/dev/null || true
        rm -rf -- "$tmp_root"
    fi
}
trap cleanup EXIT
trap 'cleanup; trap - EXIT; exit 130' INT
trap 'cleanup; trap - EXIT; exit 143' TERM

missing_test_tools=""
if [[ -n ${PUNTO_CONTRACT_LIFECYCLE_ONLY_ARTIFACT:-} ]]; then
    prerequisite_tools=(
        bash bwrap dpkg dpkg-query dpkg-deb realpath timeout python3 busybox ldd
        awk mkdir chmod cp dirname ln sed rm sha256sum grep chroot mktemp
    )
else
    prerequisite_tools=(
        bash bwrap strace cmake dpkg dpkg-query dpkg-deb dpkg-shlibdeps file readelf
        strip strings tar realpath readlink sha256sum timeout python3 busybox ldd
        gzip pkg-config awk cmp stat find chmod cp sed grep wc mkdir mktemp rm sort
        tr cut head ln chroot basename dirname date du install mv nproc
    )
fi
for required in "${prerequisite_tools[@]}"; do
    if ! command -v "$required" >/dev/null 2>&1; then
        missing_test_tools+=" $required"
    fi
done
if [[ -n $missing_test_tools ]]; then
    printf 'SKIP: packaging contract prerequisites missing:%s\n' "$missing_test_tools"
    exit 77
fi

tmp_root=$(mktemp -d /tmp/punto-packaging-contract.XXXXXX)
if [[ -z ${PUNTO_CONTRACT_LIFECYCLE_ONLY_ARTIFACT:-} ]]; then
    load_root_version
    assert_file_not_contains "$REPO_ROOT/README.md" "./build-deb.sh --install" \
        "B15: README does not recommend an unsupported build option"
fi

copy_repo() {
    local destination=$1
    mkdir -p "$destination"
    cp -a "$REPO_ROOT/." "$destination/"
    if [[ -L $destination/VERSION ]]; then
        rm -f -- "$destination/VERSION"
    fi
    rm -rf -- "$destination/.git" "$destination/cpp/build" \
        "$destination/cpp/build-release" "$destination/cpp/build-debug" \
        "$destination/build-deb"
    find "$destination" -xdev \( -type f -o -type l \) \
        \( -name '*.deb' -o -name SHA256SUMS \) -delete
}

write_mutation_spies() {
    local directory=$1 name
    mkdir -p "$directory"
    for name in sudo apt apt-get aptitude systemctl service update-rc.d \
        invoke-rc.d deb-systemd-invoke useradd groupadd usermod adduser \
        addgroup pkill killall install-system-service; do
        cat >"$directory/$name" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'MUTATION %s' "$(basename "$0")" >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
exit 97
SPY
        chmod 0755 "$directory/$name"
    done
}

write_package_query_spies() {
    local directory=$1
    cat >"$directory/dpkg" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'DPKG' >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
[[ ${TEST_MISSING_TOOL:-} == dpkg ]] && exit 127
if [[ ${1:-} == --print-architecture ]]; then
    printf 'amd64\n'
    exit 0
fi
if [[ ${1:-} == -s ]]; then
    case ",${TEST_MISSING_PACKAGES:-}," in
        *",${2:-},"*) exit 1 ;;
        *) exit 0 ;;
    esac
fi
printf 'MUTATION dpkg' >>"$TEST_CALLS"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
exit 97
SPY

    cat >"$directory/dpkg-query" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'DPKG_QUERY' >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
[[ ${TEST_MISSING_TOOL:-} == dpkg-query ]] && exit 127
package=${!#}
case ",${TEST_MISSING_PACKAGES:-}," in
    *",$package,"*) exit 1 ;;
    *) printf 'install ok installed\n'; exit 0 ;;
esac
SPY
    chmod 0755 "$directory/dpkg" "$directory/dpkg-query"
}

write_fake_build_tools() {
    local directory=$1
    mkdir -p "$directory"
    write_mutation_spies "$directory"
    write_package_query_spies "$directory"

    cat >"$directory/cmake" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'CMAKE' >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
[[ ${TEST_MISSING_TOOL:-} == cmake ]] && exit 127
mkdir -p "${TEST_REPO:?}/cpp/build"
if [[ " $* " == *' -DBUILD_TRAY=OFF '* ]]; then
    printf 'off\n' >"$TEST_REPO/cpp/build/.tray-mode"
elif [[ " $* " == *' -DBUILD_TRAY=ON '* ]]; then
    printf 'on\n' >"$TEST_REPO/cpp/build/.tray-mode"
elif [[ ! -f $TEST_REPO/cpp/build/.tray-mode ]]; then
    printf 'on\n' >"$TEST_REPO/cpp/build/.tray-mode"
fi
cp /bin/true "$TEST_REPO/cpp/build/punto"
chmod 0755 "$TEST_REPO/cpp/build/punto"
if [[ $(<"$TEST_REPO/cpp/build/.tray-mode") == on ]]; then
    cp /bin/true "$TEST_REPO/cpp/build/punto-tray"
    chmod 0755 "$TEST_REPO/cpp/build/punto-tray"
else
    rm -f -- "$TEST_REPO/cpp/build/punto-tray"
fi
printf '[{"command":"c++ -O2","file":"fixture.cpp"}]\n' \
    >"$TEST_REPO/cpp/build/compile_commands.json"
exit 0
SPY

    cat >"$directory/dpkg-shlibdeps" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'DPKG_SHLIBDEPS' >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
[[ ${TEST_MISSING_TOOL:-} == dpkg-shlibdeps ]] && exit 127
printf 'shlibs:Depends=libc6 (>= 2.34), libstdc++6 (>= 11)\n'
SPY

cat >"$directory/dpkg-deb" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'DPKG_DEB' >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
printf 'DPKG_DEB_SOURCE_DATE_EPOCH %q\n' "${SOURCE_DATE_EPOCH:-}" >>"$TEST_CALLS"
[[ ${TEST_MISSING_TOOL:-} == dpkg-deb ]] && exit 127
if [[ " $* " == *' --build '* || ${1:-} == --build ]]; then
    source_dir=""
    output=""
    for argument in "$@"; do
        case $argument in --build|--root-owner-group|-*) continue ;; esac
        if [[ -z $source_dir ]]; then source_dir=$argument; else output=$argument; fi
    done
    [[ -n $source_dir ]] || exit 64
    [[ -n $output ]] || output="${source_dir%/}.deb"
    rm -rf -- "${TEST_STAGE:?}/root"
    cp -a "$source_dir" "$TEST_STAGE/root"
    mkdir -p "$(dirname "$output")"
    printf 'fake-deb\n' >"$output"
    exit 0
fi
case ${1:-} in -I|-c|--info|--contents|-f) exit 0 ;; esac
exit 64
SPY

    local tool target
    for tool in pkg-config file strip gzip sha256sum; do
        target=/usr/bin/$tool
        cat >"$directory/$tool" <<SPY
#!/usr/bin/env bash
set -u
printf '${tool^^}' >>"\${TEST_CALLS:?}"
printf ' %q' "\$@" >>"\$TEST_CALLS"
printf '\\n' >>"\$TEST_CALLS"
[[ \${TEST_MISSING_TOOL:-} == '$tool' ]] && exit 127
exec '$target' "\$@"
SPY
    done
    chmod 0755 "$directory/"*
}

write_bash_env() {
    local path=$1
    cat >"$path" <<'ENV'
command() {
    if [[ ${1:-} == -v && ${2:-} == "${TEST_MISSING_TOOL:-}" ]]; then
        return 1
    fi
    builtin command "$@"
}
ENV
    chmod 0644 "$path"
}

FAKE_REPO=""
FAKE_STAGE=""
FAKE_CALLS=""
FAKE_OUTPUT=""
FAKE_RC=0
FAKE_TRACE=""

run_fake_build() {
    local name=$1 missing_packages=${2:-} missing_tool=${3:-} version_mode=${4:-preserve}
    local build_flavor=${5:-full}
    local source_epoch=${6:-}
    local work="$tmp_root/fake-$name" spies bash_env output_file
    local -a build_options=(--non-interactive --skip-runtime-installs)
    local -a epoch_environment=()
    [[ $build_flavor == daemon ]] && build_options+=(--without-tray)
    [[ -n $source_epoch ]] && epoch_environment=(--setenv SOURCE_DATE_EPOCH "$source_epoch")
    FAKE_REPO="$work/repo"
    FAKE_STAGE="$work/stage"
    FAKE_CALLS="$work/calls.log"
    output_file="$work/output.log"
    FAKE_TRACE="$work/execve.log"
    spies="$work/bin"
    bash_env="$work/bash-env"
    mkdir -p "$work/home" "$FAKE_STAGE"
    : >"$FAKE_CALLS"
    copy_repo "$FAKE_REPO"
    case $version_mode in
        preserve) ;;
        missing) rm -f -- "$FAKE_REPO/VERSION" ;;
        malformed)
            rm -f -- "$FAKE_REPO/VERSION"
            printf 'not a version\nsecond line\n' >"$FAKE_REPO/VERSION"
            ;;
        nul)
            rm -f -- "$FAKE_REPO/VERSION"
            /usr/bin/python3 - "$FAKE_REPO/VERSION" <<'PY'
import pathlib
import sys

pathlib.Path(sys.argv[1]).write_bytes(b"2.8\0.5\n")
PY
            ;;
        *) fail "test fixture requested unknown VERSION mode $version_mode" ;;
    esac
    write_fake_build_tools "$spies"
    write_bash_env "$bash_env"

    set +e
    timeout --signal=KILL 20s strace -f -qq -s 4096 -e trace=execve \
        -o "$FAKE_TRACE" bwrap \
        --ro-bind / / --bind "$work" "$work" --tmpfs /run --proc /proc --dev /dev \
        --unshare-net --unshare-pid --die-with-parent --new-session --clearenv \
        --chdir "$FAKE_REPO" \
        --setenv PATH "$spies:/usr/bin:/bin" --setenv HOME "$work/home" \
        --setenv LC_ALL C.UTF-8 --setenv CI 1 --setenv TEST_CALLS "$FAKE_CALLS" \
        --setenv TEST_REPO "$FAKE_REPO" --setenv TEST_STAGE "$FAKE_STAGE" \
        --setenv TEST_MISSING_PACKAGES "$missing_packages" \
        --setenv TEST_MISSING_TOOL "$missing_tool" --setenv BASH_ENV "$bash_env" \
        "${epoch_environment[@]}" \
        bash ./build-deb.sh "${build_options[@]}" </dev/null \
        >"$output_file" 2>&1
    FAKE_RC=$?
    set +e
    FAKE_OUTPUT=$(<"$output_file")
}

run_version_source_gate() {
    local artifact_count mode expected_category
    for mode in missing malformed nul; do
        run_fake_build "version-$mode" "" "" "$mode"
        assert_nonzero "$FAKE_RC" "B15: $mode repository VERSION is fatal"
        if [[ $mode == missing ]]; then
            expected_category='ERROR invalid-version-source: missing'
        else
            expected_category='ERROR invalid-version-source: malformed'
        fi
        assert_exact_line "$FAKE_OUTPUT" "$expected_category" \
            "B15: $mode VERSION has a stable failure category"
        assert_not_contains "$(<"$FAKE_CALLS")" 'CMAKE ' \
            "B15: $mode VERSION fails before configure"
        artifact_count=$(find "$FAKE_REPO" -maxdepth 1 -type f -name '*.deb' | wc -l)
        if [[ $artifact_count -eq 0 ]]; then
            pass "B15: $mode VERSION leaves no artifact"
        else
            fail "B15: $mode VERSION left $artifact_count artifact(s)"
        fi
        assert_no_prompt_or_mutation "$FAKE_OUTPUT" "$(<"$FAKE_CALLS")" \
            "B15: $mode VERSION" "$FAKE_TRACE"
    done
}

run_default_source_epoch_gate() {
    local changelog_date expected_date future_epoch

    run_fake_build default-source-epoch
    assert_zero "$FAKE_RC" "B15: build without SOURCE_DATE_EPOCH uses the canonical release epoch"
    if [[ ! -d $FAKE_STAGE/root ]]; then
        fail "B15: default-epoch build leaves no inspectable package stage"
        return
    fi

    assert_contains "$(<"$FAKE_CALLS")" \
        "DPKG_DEB_SOURCE_DATE_EPOCH $DEFAULT_SOURCE_EPOCH" \
        "B15: dpkg-deb receives the canonical nonzero release epoch"
    if ((DEFAULT_SOURCE_EPOCH <= $(date -u +%s))); then
        pass "B15: canonical release epoch is not in the future"
    else
        fail "B15: canonical release epoch is in the future"
    fi

    expected_date=$(date -u -d "@$DEFAULT_SOURCE_EPOCH" -R)
    changelog_date=$(gzip -dc "$FAKE_STAGE/root/usr/share/doc/punto-switcher/changelog.Debian.gz" |
        tail -n 1)
    assert_exact_line "$changelog_date" " -- Anton Shalin <anton.shalin@gmail.com>  $expected_date" \
        "B15: default changelog uses the canonical release epoch"

    future_epoch=$(($(date -u +%s) + 86400))
    run_fake_build future-source-epoch "" "" preserve full "$future_epoch"
    assert_nonzero "$FAKE_RC" "B15: future SOURCE_DATE_EPOCH is rejected"
    assert_exact_line "$FAKE_OUTPUT" "ERROR invalid-source-date-epoch: future" \
        "B15: future SOURCE_DATE_EPOCH has a stable failure category"
    assert_not_contains "$(<"$FAKE_CALLS")" 'CMAKE ' \
        "B15: future SOURCE_DATE_EPOCH fails before configure"
}

assert_no_prompt_or_mutation() {
    local output=$1 calls=$2 label=$3 trace=$4
    if grep -Eiq 'Установить пакет сейчас|Install (the )?package now|\[[Yy]/[Nn]\]|\[[Nn]/[Yy]\]' <<<"$output"; then
        fail "$label emitted an interactive prompt"
    else
        pass "$label never prompts"
    fi
    assert_not_contains "$calls" 'MUTATION ' "$label invokes no host mutator"
    assert_no_mutating_execve "$trace" "$label"
    if grep -Eq '^DPKG .* (-i|--install)( |$)' <<<"$calls"; then
        fail "$label attempted dpkg installation"
    else
        pass "$label never invokes dpkg installation"
    fi
}

run_required_package_matrix() {
    local package artifact_count
    for package in "${REQUIRED_PACKAGES[@]}"; do
        run_fake_build "missing-package-${package//[^a-zA-Z0-9]/_}" "$package"
        assert_nonzero "$FAKE_RC" "B21d: missing required package $package is fatal"
        assert_exact_line "$FAKE_OUTPUT" "ERROR missing-required-package: $package" \
            "B21d: $package has the stable missing-package category"
        assert_not_contains "$(<"$FAKE_CALLS")" 'CMAKE ' \
            "B21d: $package rejection precedes configure/build"
        assert_no_prompt_or_mutation "$FAKE_OUTPUT" "$(<"$FAKE_CALLS")" \
            "B21d: missing $package" "$FAKE_TRACE"
        artifact_count=$(find "$FAKE_REPO" -type f -name '*.deb' | wc -l)
        if [[ $artifact_count -eq 0 ]]; then
            pass "B21d: missing $package leaves no partial artifact"
        else
            fail "B21d: missing $package left $artifact_count artifact(s)"
        fi
    done
}

run_required_tool_matrix() {
    local tool artifact_count
    for tool in "${REQUIRED_TOOLS[@]}"; do
        run_fake_build "missing-tool-${tool//[^a-zA-Z0-9]/_}" "" "$tool"
        assert_nonzero "$FAKE_RC" "B21d: unavailable required tool $tool is fatal"
        assert_exact_line "$FAKE_OUTPUT" "ERROR missing-required-tool: $tool" \
            "B21d: required tool failure names only $tool"
        assert_no_prompt_or_mutation "$FAKE_OUTPUT" "$(<"$FAKE_CALLS")" \
            "B21d: unavailable tool $tool" "$FAKE_TRACE"
        artifact_count=$(find "$FAKE_REPO" -type f -name '*.deb' | wc -l)
        if [[ $artifact_count -eq 0 ]]; then
            pass "B21d: unavailable $tool leaves no partial artifact"
        else
            fail "B21d: unavailable $tool left $artifact_count artifact(s)"
        fi
    done
}

run_tool_provider_gate() {
    local tool provider essential priority package_list=" ${REQUIRED_PACKAGES[*]} "
    for tool in "${REQUIRED_TOOLS[@]}"; do
        provider=${REQUIRED_TOOL_PROVIDERS[$tool]:-}
        if [[ -z $provider ]]; then
            fail "B21d: required tool $tool has no declared Debian provider"
            continue
        fi
        if /usr/bin/dpkg-query -W -f='${db:Status-Abbrev}' "$provider" 2>/dev/null | \
           grep -Fxq 'ii '; then
            pass "B21d: required tool $tool maps to installed provider $provider"
        else
            fail "B21d: required tool $tool provider $provider is unavailable"
        fi
        essential=$(/usr/bin/dpkg-query -W -f='${Essential}' "$provider" 2>/dev/null || true)
        if [[ $essential == yes ]]; then
            pass "B21d: provider $provider for $tool is explicitly classified Essential"
        elif [[ $package_list == *" $provider "* ]]; then
            pass "B21d: non-Essential provider $provider for $tool is an explicit build dependency"
        else
            fail "B21d: non-Essential provider $provider for $tool is not declared"
        fi
    done
    for tool in "${BASE_BUILD_TOOLS[@]}"; do
        provider=${BASE_BUILD_TOOL_PROVIDERS[$tool]}
        essential=$(/usr/bin/dpkg-query -W -f='${Essential}' "$provider" 2>/dev/null || true)
        priority=$(/usr/bin/dpkg-query -W -f='${Priority}' "$provider" 2>/dev/null || true)
        if command -v "$tool" >/dev/null 2>&1 && \
           [[ $essential == yes || $priority == required ]]; then
            pass "B21d: base build command $tool is justified by $provider ($essential/$priority)"
        else
            fail "B21d: base build command $tool lacks an Essential/required provider"
        fi
    done
    for tool in "${TEST_ONLY_TOOLS[@]}"; do
        provider=${TEST_TOOL_PROVIDERS[$tool]}
        if command -v "$tool" >/dev/null 2>&1 && \
           /usr/bin/dpkg-query -W -f='${db:Status-Abbrev}' "$provider" 2>/dev/null | \
               grep -Fxq 'ii '; then
            pass "B21d: test-only command $tool is supplied by runner prerequisite $provider"
        else
            fail "B21d: test-only command $tool lacks runner prerequisite $provider"
        fi
    done
}

run_dependency_inventory_gate() {
    local expected actual calls package obsolete_hits obsolete_rc
    local -a allowed=(
        "${REQUIRED_PACKAGES[@]}" "${TRAY_PACKAGES[@]}"
        "${DICTIONARY_PACKAGES[@]}" "${RUNTIME_PACKAGES[@]}"
    )
    run_fake_build dependency-inventory
    assert_zero "$FAKE_RC" "B21d: complete dependency inventory build"
    calls=$(<"$FAKE_CALLS")
    expected=$(printf '%s\n' "${allowed[@]}" | LC_ALL=C sort -u)
    actual=$(awk '
        $1 == "DPKG" && $2 == "-s" { print $3 }
        $1 == "DPKG_QUERY" { print $NF }
    ' <<<"$calls" | LC_ALL=C sort -u)
    if [[ $actual == "$expected" ]]; then
        pass "B21d: build queries exactly the declared required/optional/runtime dependency inventory"
    else
        fail "B21d: queried dependency inventory differs (actual: ${actual//$'\n'/; }; expected: ${expected//$'\n'/; })"
    fi
    for package in libevent-dev fakeroot; do
        if grep -Fxq -- "$package" <<<"$actual"; then
            fail "B21d: build retains unjustified dependency coupling to $package"
        else
            pass "B21d: build has no dependency coupling to $package"
        fi
    done
    set +e
    obsolete_hits=$(/usr/bin/python3 - "$REPO_ROOT" <<'PY'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
paths = [root / "build-deb.sh", root / "cpp" / "CMakeLists.txt"]
cmake_dir = root / "cpp" / "cmake"
if cmake_dir.is_dir():
    paths.extend(path for path in cmake_dir.rglob("*") if path.is_file())
pattern = re.compile(r"(?:^|[^A-Za-z0-9_-])(?:libevent(?:-dev)?|fakeroot)(?:[^A-Za-z0-9_-]|$)", re.I)
for path in paths:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise SystemExit(f"cannot audit {path}: {error}")
    for number, line in enumerate(lines, 1):
        code = line.split("#", 1)[0]
        if pattern.search(code):
            print(f"{path.relative_to(root)}:{number}:{code.strip()}")
PY
    )
    obsolete_rc=$?
    set +e
    if [[ $obsolete_rc -ne 0 ]]; then
        fail "B21d: obsolete-dependency source parser failed closed (rc=$obsolete_rc)"
    elif [[ -n $obsolete_hits ]]; then
        fail "B21d: build sources retain libevent/fakeroot coupling: ${obsolete_hits//$'\n'/; }"
    else
        pass "B21d: build sources contain no libevent/fakeroot probing or invocation"
    fi
}

run_optional_matrix() {
    local package artifact_count stage_root calls
    for package in "${TRAY_PACKAGES[@]}"; do
        run_fake_build "missing-tray-${package//[^a-zA-Z0-9]/_}" "$package"
        calls=$(<"$FAKE_CALLS")
        assert_nonzero "$FAKE_RC" "B21b: missing tray package $package fails the default build"
        assert_exact_line "$FAKE_OUTPUT" "ERROR missing-required-tray-package: $package" \
            "B21b: $package has a stable default-build failure category"
        assert_not_contains "$calls" 'CMAKE ' \
            "B21b: missing $package fails before configure"
        assert_no_prompt_or_mutation "$FAKE_OUTPUT" "$calls" \
            "B21b: missing $package" "$FAKE_TRACE"
        artifact_count=$(find "$FAKE_REPO" -type f -name '*.deb' | wc -l)
        if [[ $artifact_count -eq 0 ]]; then
            pass "B21b: missing $package leaves no partial default artifact"
        else
            fail "B21b: missing $package left $artifact_count default artifact(s)"
        fi

        run_fake_build "explicit-daemon-${package//[^a-zA-Z0-9]/_}" \
            "$package" "" preserve daemon
        calls=$(<"$FAKE_CALLS")
        assert_zero "$FAKE_RC" "B21b: --without-tray tolerates missing $package"
        assert_contains "$calls" '-DBUILD_TRAY=OFF' \
            "B21b: explicit daemon flavor selects BUILD_TRAY=OFF"
        assert_no_prompt_or_mutation "$FAKE_OUTPUT" "$calls" \
            "B21b: explicit daemon build without $package" "$FAKE_TRACE"
        stage_root="$FAKE_STAGE/root"
        if [[ -d $stage_root ]] && ! find "$stage_root" -type f \
            \( -name punto-tray -o -name '*punto*.desktop' -o -name punto-tray.service \) \
            -print -quit | grep -q .; then
            pass "B21b: explicit daemon stage excludes tray payloads"
        else
            fail "B21b: explicit daemon stage contains tray payloads or is absent"
        fi
    done

    for package in "${DICTIONARY_PACKAGES[@]}"; do
        run_fake_build "missing-dictionary-${package//[^a-zA-Z0-9]/_}" "$package"
        calls=$(<"$FAKE_CALLS")
        assert_zero "$FAKE_RC" "B21b: missing dictionary package $package does not block assembly"
        assert_exact_line "$FAKE_OUTPUT" "WARN optional-dictionary-missing: $package" \
            "B21b: dictionary diagnostic names $package without locale coupling"
        assert_not_contains "$calls" '-DBUILD_TRAY=OFF' \
            "B21b: dictionary $package does not disable an available tray"
        assert_no_prompt_or_mutation "$FAKE_OUTPUT" "$calls" \
            "B21b: missing dictionary $package" "$FAKE_TRACE"
    done

    for package in "${RUNTIME_PACKAGES[@]}"; do
        run_fake_build "missing-runtime-${package//[^a-zA-Z0-9]/_}" "$package"
        calls=$(<"$FAKE_CALLS")
        assert_zero "$FAKE_RC" "B21b: --skip-runtime-installs tolerates missing $package"
        assert_exact_line "$FAKE_OUTPUT" "WARN runtime-install-skipped: $package" \
            "B21b: $package has a locale-independent runtime-skip diagnostic"
        assert_no_prompt_or_mutation "$FAKE_OUTPUT" "$calls" \
            "B21b: skipped runtime $package" "$FAKE_TRACE"
        artifact_count=$(find "$FAKE_REPO" -type f -name '*.deb' | wc -l)
        if [[ $artifact_count -eq 1 ]]; then
            pass "B21b: skipped $package still emits one artifact"
        else
            fail "B21b: skipped $package emitted $artifact_count artifacts"
        fi
    done
}

snapshot_tree() {
    /usr/bin/python3 - "$1" <<'PY'
import hashlib
import os
import pathlib
import stat
import sys

root = pathlib.Path(sys.argv[1])
entries = [root]


def walk_error(error: OSError) -> None:
    raise error


for directory, names, files in os.walk(
    root, topdown=True, onerror=walk_error, followlinks=False
):
    names.sort()
    files.sort()
    base = pathlib.Path(directory)
    entries.extend(base / name for name in names)
    entries.extend(base / name for name in files)

rows = []
for path in sorted(set(entries), key=lambda item: os.fsencode(str(item.relative_to(root.parent)))):
    metadata = path.lstat()
    relative = path.relative_to(root.parent)
    kind = "other"
    payload = "-"
    xattrs = []
    for name in sorted(os.listxattr(path, follow_symlinks=False)):
        value = os.getxattr(path, name, follow_symlinks=False)
        xattrs.append(f"{name}={hashlib.sha256(value).hexdigest()}")
    if stat.S_ISREG(metadata.st_mode):
        kind = "file"
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOATIME", 0)
        descriptor = os.open(path, flags)
        try:
            digest = hashlib.sha256()
            while chunk := os.read(descriptor, 1024 * 1024):
                digest.update(chunk)
            payload = digest.hexdigest()
        finally:
            os.close(descriptor)
    elif stat.S_ISDIR(metadata.st_mode):
        kind = "dir"
    elif stat.S_ISLNK(metadata.st_mode):
        kind = "link"
        payload = os.readlink(path)
    rows.append("|".join((
        os.fsdecode(relative), kind, format(stat.S_IMODE(metadata.st_mode), "04o"),
        str(metadata.st_uid), str(metadata.st_gid), str(metadata.st_size),
        str(metadata.st_mtime_ns), str(metadata.st_ctime_ns), str(metadata.st_ino),
        str(metadata.st_nlink), ",".join(xattrs), payload,
    )))
sys.stdout.write("\n".join(rows) + "\n")
PY
}

run_symlink_attack_case() {
    local seam=$1 form=$2 label=$3
    local name="symlink-${seam//[^a-zA-Z0-9]/_}-${form//[^a-zA-Z0-9]/_}"
    local work artifact hostile_path victim before after calls error_path safe_type
    local before_rc after_rc link_target

    run_fake_build "$name"
    work="$tmp_root/fake-$name"
    artifact="$FAKE_REPO/punto-switcher_${EXPECTED_VERSION}_$(/usr/bin/dpkg --print-architecture).deb"
    chmod -R u+w -- "$FAKE_REPO/cpp/build" "$FAKE_REPO/build-deb" 2>/dev/null || true
    rm -rf -- "$FAKE_REPO/cpp/build" "$FAKE_REPO/build-deb"
    rm -f -- "$artifact" "$FAKE_REPO/SHA256SUMS"

    victim="$work/victim"
    mkdir -p "$victim/nested"
    printf '%s-victim\n' "$seam" >"$victim/nested/sentinel"
    chmod 0555 "$victim/nested"
    case $seam in
        build)
            hostile_path="$FAKE_REPO/cpp/build"
            error_path=cpp/build
            safe_type="directory"
            link_target=$victim
            ;;
        stage)
            hostile_path="$FAKE_REPO/build-deb"
            error_path=build-deb
            safe_type="directory"
            link_target=$victim
            ;;
        artifact)
            printf 'artifact-victim\n' >"$victim/target.deb"
            hostile_path=$artifact
            error_path=$(basename "$artifact")
            safe_type="file"
            link_target="$victim/target.deb"
            ;;
        *)
            fail "B14: unknown symlink seam '$seam'"
            return
            ;;
    esac
    case $form in
        absolute) ;;
        relative-escape)
            link_target=$(realpath --relative-to="$(dirname "$hostile_path")" "$link_target")
            ;;
        *)
            fail "B14: unknown symlink target form '$form'"
            return
            ;;
    esac
    ln -s "$link_target" "$hostile_path"
    set +e
    before=$(snapshot_tree "$victim")
    before_rc=$?
    set +e
    : >"$FAKE_CALLS"
    FAKE_TRACE="$work/symlink-execve.log"

    set +e
    timeout --signal=KILL 20s strace -f -qq -s 4096 -e trace=execve \
        -o "$FAKE_TRACE" bwrap \
        --ro-bind / / --bind "$work" "$work" --tmpfs /run --proc /proc --dev /dev \
        --unshare-net --unshare-pid --die-with-parent --new-session --clearenv \
        --chdir "$FAKE_REPO" --setenv PATH "$work/bin:/usr/bin:/bin" \
        --setenv HOME "$work/home" --setenv LC_ALL C.UTF-8 --setenv CI 1 \
        --setenv TEST_CALLS "$FAKE_CALLS" --setenv TEST_REPO "$FAKE_REPO" \
        --setenv TEST_STAGE "$FAKE_STAGE" --setenv TEST_MISSING_PACKAGES '' \
        --setenv TEST_MISSING_TOOL '' --setenv BASH_ENV "$work/bash-env" \
        bash ./build-deb.sh --non-interactive --skip-runtime-installs </dev/null \
        >"$work/symlink-output.log" 2>&1
    FAKE_RC=$?
    set +e
    FAKE_OUTPUT=$(<"$work/symlink-output.log")
    calls=$(<"$FAKE_CALLS")
    set +e
    after=$(snapshot_tree "$victim")
    after_rc=$?
    set +e

    if [[ $before_rc -eq 0 && $after_rc -eq 0 && $before == "$after" ]]; then
        pass "B14: $label leaves its independent victim tree byte/metadata exact with no new files"
    else
        fail "B14: $label victim snapshot failed or changed (before_rc=$before_rc, after_rc=$after_rc)"
    fi
    if [[ $FAKE_RC -eq 0 ]]; then
        if [[ $safe_type == directory && -d $hostile_path && ! -L $hostile_path ]] || \
           [[ $safe_type == file && -f $hostile_path && ! -L $hostile_path ]]; then
            pass "B14: $label is reached and replaced with a regular $safe_type"
        else
            fail "B14: $label build succeeded without replacing the hostile path"
        fi
    elif [[ $FAKE_RC -ge 1 && $FAKE_RC -le 125 ]]; then
        assert_exact_line "$FAKE_OUTPUT" "ERROR unsafe-path: $error_path" \
            "B14: $label fails closed specifically at its reached seam"
    else
        fail "B14: $label gate timed out (rc=$FAKE_RC)"
    fi
    assert_not_contains "$calls" 'MUTATION ' "$label invokes no host mutator"
    assert_no_mutating_execve "$FAKE_TRACE" "$label"
}

run_symlink_attack_gate() {
    local seam
    if [[ $VERSION_VALID -ne 1 ]]; then
        fail "B14: symlink attack gate requires the valid repository VERSION"
        return
    fi
    for seam in build stage artifact; do
        run_symlink_attack_case "$seam" absolute \
            "hostile $seam absolute symlink"
        run_symlink_attack_case "$seam" relative-escape \
            "hostile $seam relative escaping symlink"
    done
}

write_real_tool_spies() {
    local directory=$1 name target upper
    mkdir -p "$directory"
    write_mutation_spies "$directory"
    for name in cmake dpkg-deb dpkg-shlibdeps pkg-config file strip gzip sha256sum; do
        target=/usr/bin/$name
        upper=${name^^}
        cat >"$directory/$name" <<SPY
#!/usr/bin/env bash
set -u
printf '$upper' >>"\${TEST_CALLS:?}"
printf ' %q' "\$@" >>"\$TEST_CALLS"
printf '\\n' >>"\$TEST_CALLS"
exec '$target' "\$@"
SPY
        chmod 0755 "$directory/$name"
    done

    cat >"$directory/dpkg" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'DPKG' >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
if [[ ${1:-} == -s ]]; then
    case ",${TEST_MISSING_PACKAGES:-}," in
        *",${2:-},"*) exit 1 ;;
        *) exec /usr/bin/dpkg "$@" ;;
    esac
fi
if [[ ${1:-} == --print-architecture ]]; then exec /usr/bin/dpkg "$@"; fi
printf 'MUTATION dpkg' >>"$TEST_CALLS"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
exit 97
SPY

    cat >"$directory/dpkg-query" <<'SPY'
#!/usr/bin/env bash
set -u
printf 'DPKG_QUERY' >>"${TEST_CALLS:?}"
printf ' %q' "$@" >>"$TEST_CALLS"
printf '\n' >>"$TEST_CALLS"
package=${!#}
case ",${TEST_MISSING_PACKAGES:-}," in
    *",$package,"*) exit 1 ;;
    *) exec /usr/bin/dpkg-query "$@" ;;
esac
SPY
    chmod 0755 "$directory/dpkg" "$directory/dpkg-query"
}

RUN_REPO=""
RUN_ARTIFACT=""
RUN_SHA_FILE=""
RUN_LOG=""
RUN_CALLS=""
RUN_RC=0
RUN_TRACE=""
PROBE_OUTPUT=""
PROBE_RC=0
PROBE_TRACE=""

run_real_build() {
    local name=$1 mutation_version=${2:-} missing_packages=${3:-}
    local build_flavor=${4:-full}
    local work="$tmp_root/real-$name" spies artifact_count
    RUN_REPO="$work/repo"
    RUN_LOG="$work/build.log"
    RUN_CALLS="$work/calls.log"
    RUN_TRACE="$work/execve.log"
    RUN_ARTIFACT=""
    RUN_SHA_FILE=""
    mkdir -p "$work/home" "$work/tmp"
    : >"$RUN_CALLS"
    copy_repo "$RUN_REPO"
    if [[ -n $mutation_version ]]; then
        printf '%s\n' "$mutation_version" >"$RUN_REPO/VERSION"
    fi
    spies="$RUN_REPO/.contract-bin"
    write_real_tool_spies "$spies"

    set +e
    # The single-quoted payload expands only in the isolated inner bash.
    # shellcheck disable=SC2016
    timeout --signal=KILL 240s strace -f -qq -s 4096 -e trace=execve \
        -o "$RUN_TRACE" bwrap \
        --ro-bind / / --bind "$work" "$work" --tmpfs /run \
        --proc /proc --dev /dev --unshare-net --unshare-pid --die-with-parent \
        --new-session --clearenv --chdir "$RUN_REPO" \
        --setenv PATH "$spies:/usr/bin:/bin" --setenv HOME "$work/home" \
        --setenv TMPDIR "$work/tmp" --setenv LC_ALL C.UTF-8 --setenv CI 1 \
        --setenv SOURCE_DATE_EPOCH "$SOURCE_EPOCH" --setenv TEST_CALLS "$RUN_CALLS" \
        --setenv TEST_MISSING_PACKAGES "$missing_packages" \
        --setenv TEST_BUILD_FLAVOR "$build_flavor" \
        bash -c 'umask 0002; args=(--non-interactive --skip-runtime-installs); [[ ${TEST_BUILD_FLAVOR:-full} == daemon ]] && args+=(--without-tray); exec bash ./build-deb.sh "${args[@]}" </dev/null' \
        >"$RUN_LOG" 2>&1
    RUN_RC=$?
    set +e

    artifact_count=$(find "$RUN_REPO" -maxdepth 1 -type f -name '*.deb' | wc -l)
    if [[ $artifact_count -eq 1 ]]; then
        RUN_ARTIFACT=$(find "$RUN_REPO" -maxdepth 1 -type f -name '*.deb' -print -quit)
    fi
    [[ -f $RUN_REPO/SHA256SUMS ]] && RUN_SHA_FILE="$RUN_REPO/SHA256SUMS"
}

run_artifact_probe() {
    local name=$1 version_file=$2
    local work="$tmp_root/artifact-probe-$name" output_file
    shift 2
    mkdir -p "$work/home" "$work/tmp"
    chmod 0777 "$work" "$work/home" "$work/tmp"
    output_file="$work/output.log"
    PROBE_TRACE="$work/execve.log"
    set +e
    timeout --signal=KILL 5s strace -f -qq -s 4096 -e trace=execve \
        -o "$PROBE_TRACE" bwrap \
        --ro-bind / / --bind "$work" "$work" --tmpfs /run --proc /proc --dev /dev \
        --unshare-net --unshare-pid --unshare-user --uid 65534 --gid 65534 \
        --die-with-parent --new-session --clearenv --chdir "$work" \
        --setenv PATH /usr/bin:/bin --setenv HOME "$work/home" \
        --setenv TMPDIR "$work/tmp" --setenv LC_ALL C.UTF-8 \
        --setenv PUNTO_VERSION_FILE "$version_file" \
        "$@" >"$output_file" 2>&1
    PROBE_RC=$?
    set +e
    PROBE_OUTPUT=$(<"$output_file")
    assert_no_mutating_execve "$PROBE_TRACE" "B15: sandboxed artifact probe $name"
}

assert_real_exec_dependencies() {
    local trace=$1 repo=$2 label=$3 executable canonical owner metadata
    local essential priority covered_by count=0 violations=""
    local parser_output parser_rc candidate
    local -a declared_packages=(
        "${REQUIRED_PACKAGES[@]}" "${REQUIRED_TOOL_PROVIDERS[@]}"
        "${BASE_BUILD_TOOL_PROVIDERS[@]}"
    )

    set +e
    parser_output=$(/usr/bin/python3 - "$trace" <<'PY'
import pathlib
import re
import sys

trace = pathlib.Path(sys.argv[1])
pending = {}
executables = set()
prefix = r"^(?:\[pid\s+(\d+)\]|\s*(\d+))?\s*"
start = re.compile(prefix + r'execve\("([^"]+)".*?(<unfinished \.\.\.>|= 0)$')
resumed = re.compile(prefix + r"<\.\.\. execve resumed>\).*?= 0$")
for line in trace.read_text(encoding="utf-8", errors="replace").splitlines():
    match = start.search(line)
    if match:
        key = match.group(1) or match.group(2) or "main"
        path = match.group(3)
        if match.group(4).startswith("<unfinished"):
            pending[key] = path
        else:
            executables.add(path)
        continue
    match = resumed.search(line)
    if match:
        key = match.group(1) or match.group(2) or "main"
        path = pending.pop(key, None)
        if path is not None:
            executables.add(path)
if not executables:
    raise SystemExit("no successful execve records")
for executable in sorted(executables):
    print(executable)
PY
    )
    parser_rc=$?
    set +e
    if [[ $parser_rc -ne 0 ]]; then
        fail "$label execve dependency parser failed closed (rc=$parser_rc)"
        return
    fi

    while IFS= read -r executable; do
        [[ -n $executable ]] || continue
        if [[ $executable == /usr/bin/bwrap ]]; then
            continue
        fi
        if [[ $executable == "$repo/.contract-bin/"* ]]; then
            case ${executable##*/} in
                cmake|dpkg|dpkg-query|dpkg-deb|dpkg-shlibdeps|pkg-config|file|strip|gzip|sha256sum)
                    continue
                    ;;
                *)
                    violations+="unknown controlled executable $executable; "
                    continue
                    ;;
            esac
        fi
        if [[ $executable == "$repo/cpp/build/CMakeFiles/"* ]]; then
            continue
        fi
        if [[ $executable != /* ]]; then
            violations+="relative executable $executable; "
            continue
        fi
        canonical=$(realpath -e "$executable" 2>/dev/null || true)
        if [[ -z $canonical ]]; then
            violations+="unresolvable executable $executable; "
            continue
        fi
        owner=$(/usr/bin/dpkg-query -S "$canonical" 2>/dev/null | \
            sed -n 's/: \/.*$//p' | head -n 1)
        owner=${owner%%:*}
        if [[ -z $owner ]]; then
            violations+="unowned executable $canonical; "
            continue
        fi
        covered_by=""
        for candidate in "${declared_packages[@]}"; do
            if [[ $owner == "$candidate" ]]; then
                covered_by=$candidate
                break
            fi
        done
        if [[ -z $covered_by ]]; then
            case $owner in
                gcc-[0-9]*|g++-[0-9]*|cpp-[0-9]*|make|libc6-dev)
                    covered_by=build-essential
                    ;;
                pkgconf|pkgconf-bin)
                    covered_by=pkg-config
                    ;;
                binutils-*)
                    covered_by=binutils
                    ;;
            esac
        fi
        metadata=$(/usr/bin/dpkg-query -W -f='${Essential}|${Priority}' "$owner" \
            2>/dev/null || true)
        essential=${metadata%%|*}
        priority=${metadata#*|}
        if [[ -n $covered_by || $essential == yes || \
              $priority == required || $priority == important ]]; then
            count=$((count + 1))
        else
            violations+="$canonical owned by undeclared $owner ($metadata); "
        fi
    done <<<"$parser_output"

    if [[ -z $violations && $count -gt 0 ]]; then
        pass "$label successful external execves are Essential/base or covered by declared build providers"
    else
        fail "$label has undeclared executable dependencies: ${violations:-no classified host executable}"
    fi
}

check_compile_portability() {
    local repo=$1 label=$2 result rc source_hits
    set +e
    result=$(/usr/bin/python3 - "$repo/cpp/build/compile_commands.json" <<'PY'
import json
import pathlib
import re
import shlex
import sys

path = pathlib.Path(sys.argv[1])
if not path.is_file():
    print("missing compile_commands.json")
    raise SystemExit(2)
try:
    commands = json.loads(path.read_text(encoding="utf-8"))
except (OSError, UnicodeError, json.JSONDecodeError) as error:
    print(f"invalid compile_commands.json: {error}")
    raise SystemExit(2)
if not isinstance(commands, list) or not commands:
    print("compile_commands.json contains no compile actions")
    raise SystemExit(2)

bad = []


def expand_response_files(arguments, directory, seen=None):
    if seen is None:
        seen = set()
    expanded = []
    for argument in arguments:
        if not argument.startswith("@") or argument == "@":
            expanded.append(argument)
            continue
        response = pathlib.Path(argument[1:])
        if not response.is_absolute():
            response = directory / response
        try:
            response = response.resolve(strict=True)
        except OSError as error:
            bad.append(f"response:{argument}:unreadable:{error}")
            continue
        if response in seen:
            bad.append(f"response:{response}:recursive")
            continue
        try:
            if response.stat().st_size > 1024 * 1024:
                bad.append(f"response:{response}:oversized")
                continue
            nested = shlex.split(response.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, ValueError) as error:
            bad.append(f"response:{response}:unparseable:{error}")
            continue
        expanded.extend(expand_response_files(nested, response.parent, seen | {response}))
    return expanded


for index, entry in enumerate(commands):
    if not isinstance(entry, dict):
        bad.append(f"entry[{index}]:not-object")
        continue
    arguments = entry.get("arguments")
    if arguments is None:
        command = entry.get("command")
        if not isinstance(command, str):
            bad.append(f"entry[{index}]:missing-command")
            continue
        try:
            arguments = shlex.split(command)
        except ValueError as error:
            bad.append(f"entry[{index}]:unparseable:{error}")
            continue
    if not isinstance(arguments, list) or not all(isinstance(arg, str) for arg in arguments):
        bad.append(f"entry[{index}]:invalid-arguments")
        continue
    directory_value = entry.get("directory", str(path.parent))
    if not isinstance(directory_value, str):
        bad.append(f"entry[{index}]:invalid-directory")
        continue
    arguments = expand_response_files(arguments, pathlib.Path(directory_value))
    for argument_index, argument in enumerate(arguments):
        lowered = argument.lower()
        if lowered in ("-march", "-mtune", "-mcpu", "-target", "-target-cpu", "-target-feature"):
            following = arguments[argument_index + 1] if argument_index + 1 < len(arguments) else "<missing>"
            bad.append(f"entry[{index}]:{argument} {following}")
        elif lowered.startswith(("-march=", "-mtune=", "-mcpu=", "--target=", "-target=")):
            bad.append(f"entry[{index}]:{argument}")
        elif re.search(r"(?:^|[^a-z0-9])x86-64-v(?:[2-9]|[1-9][0-9]+)(?:$|[^a-z0-9])", lowered):
            bad.append(f"entry[{index}]:{argument}")
        elif re.match(r"^-m[a-z0-9]", argument) or re.search(r",-m[a-z0-9]", argument):
            bad.append(f"entry[{index}]:{argument}")
        elif re.match(r"^-D__(?:AVX|FMA|BMI|AES|SHA|SSE|GFNI|RDRND|RDSEED|VAES)", argument):
            bad.append(f"entry[{index}]:{argument}")
if bad:
    print("\n".join(bad))
    raise SystemExit(1)
PY
    )
    rc=$?
    set +e
    if [[ $rc -eq 0 ]]; then
        pass "$label compile_commands contains no CPU-target-specific flags"
    else
        fail "$label compile_commands portability violation: ${result//$'\n'/; }"
    fi

    source_hits=$(grep -RInI -E -- \
        '((^|[^[:alnum:]_-])-m[a-z0-9]|(^|[^[:alnum:]_-])-(march|mtune|mcpu|target|target-cpu|target-feature)([=[:space:]]|$)|--target=|x86-64-v([2-9]|[1-9][0-9]+)|-D__(AVX|FMA|BMI|AES|SHA|SSE|GFNI|RDRND|RDSEED|VAES))' \
        "$repo/cpp/CMakeLists.txt" "$repo/cpp/cmake" "$repo/build-deb.sh" \
        2>/dev/null || true)
    if [[ -z $source_hits ]]; then
        pass "$label build sources contain no hidden CPU-target-specific flags"
    else
        fail "$label build sources contain CPU-target-specific flags: ${source_hits//$'\n'/; }"
    fi
}

assert_real_build_safe() {
    local label=$1 log calls artifact_count
    log=$(<"$RUN_LOG")
    calls=$(<"$RUN_CALLS")
    assert_zero "$RUN_RC" "$label succeeds in a read-only-root, networkless sandbox"
    assert_no_prompt_or_mutation "$log" "$calls" "$label" "$RUN_TRACE"
    assert_real_exec_dependencies "$RUN_TRACE" "$RUN_REPO" "$label"
    artifact_count=$(find "$RUN_REPO" -maxdepth 1 -type f -name '*.deb' | wc -l)
    if [[ $artifact_count -eq 1 ]]; then
        pass "$label emits exactly one Debian artifact"
    else
        fail "$label emitted $artifact_count Debian artifacts"
    fi
    if [[ -n $RUN_ARTIFACT ]] && /usr/bin/dpkg-deb --info "$RUN_ARTIFACT" >/dev/null 2>&1; then
        pass "$label output is a readable Debian archive"
    else
        fail "$label output is not a readable Debian archive"
    fi
}

check_tree_modes() {
    local root=$1 label=$2 bad path mode expected
    bad=$(find "$root" -xdev -perm /7022 -printf '%m %P\n' | sort)
    if [[ -z $bad ]]; then
        pass "$label contains no setuid/setgid/sticky or group/world-writable paths"
    else
        fail "$label has unsafe modes: ${bad//$'\n'/, }"
    fi

    while IFS='|' read -r path expected; do
        [[ -e $root/$path ]] || continue
        mode=$(stat -c '%a' "$root/$path")
        if [[ $mode == "$expected" ]]; then
            pass "$label $path mode is $expected"
        else
            fail "$label $path mode is $mode, expected $expected"
        fi
    done <<'MODES'
usr/bin/punto|755
usr/bin/punto-daemon|755
usr/bin/punto-tray|755
etc/punto/config.yaml|644
etc/xdg/autostart/punto-tray.desktop|644
usr/lib/systemd/user/punto-tray.service|644
usr/share/punto-switcher/VERSION|644
MODES

    bad=$(find "$root" -xdev -type d ! -perm 0755 -printf '%m %P\n' | sort)
    if [[ -z $bad ]]; then
        pass "$label directories have normalized 0755 modes"
    else
        fail "$label has non-0755 directories: ${bad//$'\n'/, }"
    fi

    bad=$(find "$root/usr/bin" -xdev -type f ! -perm 0755 -printf '%m %P\n' 2>/dev/null | sort)
    if [[ -z $bad ]]; then
        pass "$label executable payloads have normalized 0755 modes"
    else
        fail "$label has non-0755 executable payloads: ${bad//$'\n'/, }"
    fi

    bad=$(find "$root" -xdev -type f ! -path "$root/usr/bin/*" ! -perm 0644 \
        -printf '%m %P\n' | sort)
    if [[ -z $bad ]]; then
        pass "$label regular data files have normalized 0644 modes"
    else
        fail "$label has non-0644 data files: ${bad//$'\n'/, }"
    fi
}

check_control_modes() {
    local control_root=$1 label=$2 file mode bad expected
    assert_executable "$control_root/postinst" "$label provides executable postinst"
    assert_executable "$control_root/prerm" "$label provides executable prerm"
    assert_executable "$control_root/postrm" "$label provides executable postrm"
    for file in postinst preinst prerm postrm; do
        [[ -e $control_root/$file ]] || continue
        mode=$(stat -c '%a' "$control_root/$file")
        if [[ $mode == 755 ]]; then
            pass "$label control script $file is 0755"
        else
            fail "$label control script $file is $mode, expected 755"
        fi
    done
    for file in control conffiles md5sums; do
        [[ -e $control_root/$file ]] || continue
        mode=$(stat -c '%a' "$control_root/$file")
        if [[ $mode == 644 ]]; then
            pass "$label control file $file is 0644"
        else
            fail "$label control file $file is $mode, expected 644"
        fi
    done
    bad=$(find "$control_root" -xdev -perm /7022 -printf '%m %P\n' | sort)
    if [[ -z $bad ]]; then
        pass "$label control archive has no privileged/writable modes"
    else
        fail "$label control archive has unsafe modes: ${bad//$'\n'/, }"
    fi

    bad=""
    while IFS= read -r -d '' file; do
        case $(basename "$file") in
            preinst|postinst|prerm|postrm) expected=755 ;;
            *) expected=644 ;;
        esac
        mode=$(stat -c '%a' "$file")
        [[ $mode == "$expected" ]] || \
            bad+="$mode ${file#"$control_root"/} (expected $expected)"$'\n'
    done < <(find "$control_root" -xdev -type f -print0)
    if [[ -z $bad ]]; then
        pass "$label all control files have policy-normalized modes"
    else
        fail "$label has non-policy control modes: ${bad//$'\n'/, }"
    fi
}

check_archive_owners_and_types() {
    local artifact=$1 label=$2 work=$3
    local data_tar="$work/data.tar" control_tar="$work/control.tar"
    local bad
    /usr/bin/dpkg-deb --fsys-tarfile "$artifact" >"$data_tar"
    /usr/bin/dpkg-deb --ctrl-tarfile "$artifact" >"$control_tar"
    bad=$(tar --numeric-owner -tvf "$data_tar" | awk '$2 != "0/0" {print}')
    if [[ -z $bad ]]; then
        pass "$label data archive is entirely uid/gid 0/0"
    else
        fail "$label data archive has non-root ownership: ${bad//$'\n'/, }"
    fi
    bad=$(tar --numeric-owner -tvf "$control_tar" | awk '$2 != "0/0" {print}')
    if [[ -z $bad ]]; then
        pass "$label control archive is entirely uid/gid 0/0"
    else
        fail "$label control archive has non-root ownership: ${bad//$'\n'/, }"
    fi
    bad=$(tar -tvf "$data_tar" | awk 'substr($1,1,1) !~ /[-dl]/ {print}')
    if [[ -z $bad ]]; then
        pass "$label data archive contains only regular files/directories/symlinks"
    else
        fail "$label data archive contains special nodes: ${bad//$'\n'/, }"
    fi
    bad=$(tar -tvf "$control_tar" | awk 'substr($1,1,1) !~ /[-d]/ {print}')
    if [[ -z $bad ]]; then
        pass "$label control archive contains only regular files/directories"
    else
        fail "$label control archive contains special nodes or symlinks: ${bad//$'\n'/, }"
    fi
}

check_symlinks_bounded() {
    local root=$1 label=$2 link target resolved bad=""
    while IFS= read -r -d '' link; do
        target=$(readlink "$link")
        if [[ $target == /* ]]; then
            bad+="absolute:${link#"$root"/}->$target "
            continue
        fi
        resolved=$(realpath -m "$(dirname "$link")/$target")
        if [[ $resolved != "$root" && $resolved != "$root/"* ]]; then
            bad+="escape:${link#"$root"/}->$target "
        elif [[ ! -e $link ]]; then
            bad+="dangling:${link#"$root"/}->$target "
        fi
    done < <(find "$root" -xdev -type l -print0)
    if [[ -z $bad ]]; then
        pass "$label contains no absolute, escaping, or dangling symlink"
    else
        fail "$label contains unsafe symlink(s): $bad"
    fi
}

elf_machine_for_arch() {
    case $1 in
        amd64) printf '%s' 'Advanced Micro Devices X86-64' ;;
        arm64) printf '%s' 'AArch64' ;;
        armhf) printf '%s' 'ARM' ;;
        i386) printf '%s' 'Intel 80386' ;;
        riscv64) printf '%s' 'RISC-V' ;;
        *) printf '%s' '' ;;
    esac
}

normalize_dep() {
    sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//; s/[[:space:]]+/ /g; s/[[:space:]]*\([[:space:]]*/ (/; s/[[:space:]]*\)/)/'
}

assert_relation_package() {
    local relationships=$1 package=$2 message=$3
    if grep -Eq "(^|[,|[:space:]])${package//+/\\+}([[:space:](,|]|$)" <<<"$relationships"; then
        pass "$message"
    else
        fail "$message (relations: $relationships)"
    fi
}

check_cli_command_dependencies() {
    local cli=$1 depends=$2 label=$3 owner priority essential command package
    local -a base_commands=(
        bash env timeout id sleep date nohup head tr cut basename dirname stat readlink
        realpath sha256sum rm mkdir mktemp cat wc grep sed flock getent
    )
    local -A providers=(
        [bash]=bash [env]=coreutils [timeout]=coreutils [id]=coreutils [sleep]=coreutils
        [date]=coreutils [nohup]=coreutils [head]=coreutils [tr]=coreutils
        [cut]=coreutils [basename]=coreutils [dirname]=coreutils [stat]=coreutils
        [readlink]=coreutils [realpath]=coreutils [sha256sum]=coreutils
        [rm]=coreutils [mkdir]=coreutils [mktemp]=coreutils
        [cat]=coreutils [wc]=coreutils [grep]=grep [sed]=sed
        [flock]=util-linux [getent]=libc-bin
    )
    if [[ ! -f $cli || -L $cli ]]; then
        fail "$label cannot verify CLI command dependencies without a regular non-symlink CLI"
        return
    fi
    if grep -Eq '(^|[;&|()[:space:]])((/usr/bin/|/bin/)?nc)[[:space:]]+-U([[:space:]]|$)' "$cli"; then
        pass "$label CLI invokes nc with Unix-domain mode"
    else
        fail "$label CLI does not expose the required nc -U transport"
    fi
    assert_relation_package "$depends" netcat-openbsd \
        "$label Depends declares the exact netcat-openbsd transport provider"

    if grep -Eq '(^|[;&|()[:space:]])((/usr/bin/|/bin/)?sudo)([[:space:]]|$)' "$cli"; then
        assert_relation_package "$depends" sudo \
            "$label Depends declares sudo because the packaged CLI invokes it"
    else
        pass "$label CLI does not invoke sudo, so no sudo relationship is required"
    fi

    if grep -Eq '(^|[;&|()[:space:]])((/usr/bin/|/bin/)?systemctl)([[:space:]]|$)' "$cli"; then
        assert_relation_package "$depends" systemd \
            "$label Depends declares the systemd user-manager client"
    fi
    assert_relation_package "$depends" init-system-helpers \
        "$label Depends declares the maintainer-script helper provider"

    for command in "${base_commands[@]}"; do
        if grep -Eq "(^|[;&|()[:space:]])((/usr/bin/|/bin/)?$command)([[:space:]]|$)" "$cli"; then
            package=${providers[$command]}
            essential=$(/usr/bin/dpkg-query -W -f='${Essential}' "$package" 2>/dev/null || true)
            priority=$(/usr/bin/dpkg-query -W -f='${Priority}' "$package" 2>/dev/null || true)
            if [[ $essential == yes || $priority == required ]]; then
                pass "$label classifies $command under Essential/required package $package"
            else
                fail "$label cannot justify excluding $command from explicit Depends"
            fi
        fi
    done
    if grep -Eq '(^|[;&|()[:space:]])((/usr/bin/|/bin/)?setsid)([[:space:]]|$)' "$cli"; then
        if [[ $(/usr/bin/dpkg-query -W -f='${Essential}' util-linux 2>/dev/null || true) == yes ]]; then
            pass "$label classifies setsid under Essential package util-linux"
        else
            fail "$label cannot justify excluding setsid from explicit Depends"
        fi
    fi
}

assert_exact_derived_dependencies() {
    local artifact=$1 data_root=$2 label=$3 work=$4 depends derived rc dep saw_yaml=0
    local actual_normalized expected_normalized
    local -a elf_args=() derived_parts=() control_parts=()
    local -a explicit_parts=(
        interception-tools hunspell-en-us hunspell-ru netcat-openbsd passwd
        'util-linux (>= 2.38)' 'systemd (>= 249.10)' \
        'init-system-helpers (>= 1.66)'
    )
    depends=$(/usr/bin/dpkg-deb -f "$artifact" Depends 2>/dev/null || true)
    [[ -x $data_root/usr/bin/punto-daemon ]] && elf_args+=("-e$data_root/usr/bin/punto-daemon")
    [[ -x $data_root/usr/bin/punto-tray ]] && elf_args+=("-e$data_root/usr/bin/punto-tray")
    if [[ ${#elf_args[@]} -eq 0 ]]; then
        fail "$label cannot derive dependencies without a packaged ELF executable"
        return
    fi
    mkdir -p "$work/debian"
    cat >"$work/debian/control" <<'CONTROL'
Source: punto-contract
Section: utils
Priority: optional
Maintainer: Contract Test <nobody@example.invalid>
Standards-Version: 4.6.2

Package: punto-contract
Architecture: any
Description: isolated dependency derivation fixture
CONTROL
    set +e
    derived=$(cd "$work" && timeout --signal=KILL 20s \
        /usr/bin/dpkg-shlibdeps -O "${elf_args[@]}" 2>"$work/shlibdeps.err")
    rc=$?
    set +e
    assert_zero "$rc" "$label dpkg-shlibdeps resolves packaged ELF dependencies"
    [[ $rc -eq 0 ]] || return
    derived=${derived#shlibs:Depends=}
    IFS=',' read -ra derived_parts <<<"$derived"
    IFS=',' read -ra control_parts <<<"$depends"
    if grep -Eq '(^|[;&|()[:space:]])((/usr/bin/|/bin/)?sudo)([[:space:]]|$)' \
        "$data_root/usr/bin/punto"; then
        explicit_parts+=(sudo)
    fi
    expected_normalized=""
    for dep in "${derived_parts[@]}"; do
        dep=$(normalize_dep <<<"$dep")
        [[ -z $dep ]] && continue
        expected_normalized+="$dep"$'\n'
        if [[ $dep == 'libyaml-cpp0.8 ('*')' ]]; then
            saw_yaml=1
        fi
    done
    for dep in "${explicit_parts[@]}"; do
        expected_normalized+="$dep"$'\n'
    done
    actual_normalized=""
    for dep in "${control_parts[@]}"; do
        dep=$(normalize_dep <<<"$dep")
        [[ -n $dep ]] && actual_normalized+="$dep"$'\n'
    done
    expected_normalized=$(LC_ALL=C sort -u <<<"$expected_normalized" | sed '/^$/d')
    actual_normalized=$(LC_ALL=C sort -u <<<"$actual_normalized" | sed '/^$/d')
    if [[ $actual_normalized == "$expected_normalized" ]]; then
        pass "$label Depends is exactly dpkg-shlibdeps plus justified non-ELF runtime commands"
    else
        fail "$label Depends set differs from the exact justified set (actual: ${actual_normalized//$'\n'/; }; expected: ${expected_normalized//$'\n'/; })"
    fi
    if [[ $saw_yaml -eq 1 ]]; then
        pass "$label preserves the exact version-constrained libyaml-cpp0.8 dependency"
    else
        fail "$label dpkg-shlibdeps emitted no version-constrained libyaml-cpp0.8 dependency"
    fi
}

inspect_artifact() {
    local artifact=$1 expected_version=$2 expect_tray=$3 label=$4 work=$5
    local data="$work/data" control="$work/control" package version architecture section priority
    local maintainer description predepends depends relationships conffiles binary machine expected_machine dynamic
    local elf_header program_headers stack_segment symbols binary_name
    mkdir -p "$data" "$control"
    if /usr/bin/dpkg-deb -x "$artifact" "$data" && /usr/bin/dpkg-deb -e "$artifact" "$control"; then
        pass "$label data and control archives extract"
    else
        fail "$label archive extraction failed"
        return
    fi

    package=$(/usr/bin/dpkg-deb -f "$artifact" Package 2>/dev/null || true)
    version=$(/usr/bin/dpkg-deb -f "$artifact" Version 2>/dev/null || true)
    architecture=$(/usr/bin/dpkg-deb -f "$artifact" Architecture 2>/dev/null || true)
    section=$(/usr/bin/dpkg-deb -f "$artifact" Section 2>/dev/null || true)
    priority=$(/usr/bin/dpkg-deb -f "$artifact" Priority 2>/dev/null || true)
    maintainer=$(/usr/bin/dpkg-deb -f "$artifact" Maintainer 2>/dev/null || true)
    description=$(/usr/bin/dpkg-deb -f "$artifact" Description 2>/dev/null || true)
    predepends=$(/usr/bin/dpkg-deb -f "$artifact" Pre-Depends 2>/dev/null || true)
    depends=$(/usr/bin/dpkg-deb -f "$artifact" Depends 2>/dev/null || true)
    relationships=$(/usr/bin/dpkg-deb -f "$artifact" \
        Pre-Depends Depends Recommends Suggests Enhances 2>/dev/null || true)

    if [[ $package == punto-switcher ]]; then
        pass "$label Package is punto-switcher"
    else
        fail "$label Package is '$package'"
    fi
    if [[ $version == "$expected_version" ]]; then
        pass "$label Version is $expected_version"
    else
        fail "$label Version is '$version', expected '$expected_version'"
    fi
    if [[ $architecture == "$(/usr/bin/dpkg --print-architecture)" ]]; then
        pass "$label Architecture matches the host Debian architecture"
    else
        fail "$label Architecture is '$architecture'"
    fi
    if [[ $section == utils ]]; then
        pass "$label Section is utils"
    else
        fail "$label Section is '$section', expected utils"
    fi
    if [[ $priority == optional ]]; then
        pass "$label Priority is optional"
    else
        fail "$label Priority is '$priority', expected optional"
    fi
    if [[ $maintainer == *'<'*'>'* ]]; then
        pass "$label Maintainer metadata is structured"
    else
        fail "$label Maintainer metadata is invalid: '$maintainer'"
    fi
    if [[ -n $description && $description != punto-switcher ]]; then
        pass "$label Description metadata is meaningful"
    else
        fail "$label Description metadata is empty/trivial"
    fi
    if [[ -n $depends ]]; then
        pass "$label Depends metadata is nonempty"
    else
        fail "$label Depends is empty"
    fi
    if [[ $predepends == 'dpkg (>= 1.20.6)' ]]; then
        pass "$label declares the minimum dpkg remove-on-upgrade contract"
    else
        fail "$label Pre-Depends is '$predepends', expected dpkg (>= 1.20.6)"
    fi
    for package in socat libevent fakeroot; do
        assert_not_contains "$relationships" "$package" \
            "$label relationship fields contain no unjustified $package coupling"
    done

    assert_executable "$data/usr/bin/punto" "$label provides /usr/bin/punto"
    assert_executable "$data/usr/bin/punto-daemon" "$label provides /usr/bin/punto-daemon"
    assert_file "$data/etc/punto/config.yaml" "$label packages the active config"
    if [[ ! -e $data/etc/punto/config.yaml.new ]]; then
        pass "$label ships no inert config.yaml.new"
    else
        fail "$label ships config.yaml.new"
    fi
    assert_file "$data/usr/share/punto-switcher/VERSION" "$label packages canonical VERSION"
    assert_file "$data/usr/share/doc/punto-switcher/README.md" "$label packages README"
    assert_file "$data/usr/share/doc/punto-switcher/copyright" "$label packages copyright"
    assert_file "$data/usr/share/doc/punto-switcher/changelog.Debian.gz" "$label packages changelog"
    assert_file "$data/usr/share/doc/punto-switcher/examples/udevmon.yaml" \
        "$label packages the udevmon example outside global configuration"
    if [[ ! -e $data/usr/share/punto-switcher/sounds ]]; then
        pass "$label omits inactive correction sound payloads"
    else
        fail "$label unexpectedly ships inactive correction sound payloads"
    fi
    if [[ ! -e $data/etc/interception/udevmon.yaml ]]; then
        pass "$label does not own the global interception configuration"
    else
        fail "$label unexpectedly owns /etc/interception/udevmon.yaml"
    fi
    assert_not_contains "$(/usr/bin/dpkg-deb --contents "$artifact")" './usr/local/' "$label contains no /usr/local path"

    conffiles=""
    [[ -f $control/conffiles ]] && conffiles=$(<"$control/conffiles")
    assert_exact_line "$conffiles" '/etc/punto/config.yaml' "$label declares config as conffile"
    assert_not_contains "$conffiles" '/etc/interception/udevmon.yaml' \
        "$label does not declare the foreign udevmon config as a conffile"

    if [[ $expect_tray == yes ]]; then
        assert_executable "$data/usr/bin/punto-tray" "$label includes tray ELF"
        assert_file "$data/etc/xdg/autostart/punto-tray.desktop" "$label includes tray autostart"
        assert_file "$data/usr/lib/systemd/user/punto-tray.service" \
            "$label includes package-owned static user unit"
        assert_exact_line "$conffiles" '/etc/xdg/autostart/punto-tray.desktop' \
            "$label declares tray autostart policy as conffile"
        assert_not_contains "$conffiles" '/usr/lib/systemd/user/punto-tray.service' \
            "$label does not declare the package-owned user unit as conffile"
        if [[ -f $data/usr/lib/systemd/user/punto-tray.service ]]; then
            local unit_source desktop_source
            unit_source=$(<"$data/usr/lib/systemd/user/punto-tray.service")
            desktop_source=$(<"$data/etc/xdg/autostart/punto-tray.desktop")
            assert_contains "$unit_source" 'Type=exec' \
                "$label user unit reports exec readiness"
            assert_contains "$unit_source" 'ExecStart=/usr/bin/punto-tray' \
                "$label user unit starts the exact packaged tray"
            assert_contains "$unit_source" 'TimeoutStopSec=5s' \
                "$label user unit bounds shutdown"
            assert_not_contains "$unit_source" '[Install]' \
                "$label user unit remains static"
            assert_contains "$desktop_source" \
                'Exec=/usr/bin/systemctl --user start --no-block -- punto-tray.service' \
                "$label XDG autostart delegates asynchronously to the user manager"
        fi
    else
        local tray_paths
        if [[ ! -e $data/usr/bin/punto-tray ]]; then
            pass "$label daemon-only payload omits tray"
        else
            fail "$label daemon-only payload contains tray"
        fi
        if [[ ! -e $data/etc/xdg/autostart/punto-tray.desktop ]]; then
            pass "$label daemon-only payload omits desktop entry"
        else
            fail "$label daemon-only payload contains desktop entry"
        fi
        if [[ ! -e $data/usr/lib/systemd/user/punto-tray.service ]]; then
            pass "$label daemon-only payload omits the user unit"
        else
            fail "$label daemon-only payload contains the user unit"
        fi
        assert_exact_line "$conffiles" \
            'remove-on-upgrade /etc/xdg/autostart/punto-tray.desktop' \
            "$label daemon-only flavor retires the obsolete tray conffile"
        tray_paths=$(find "$data" -xdev -mindepth 1 \
            \( -iname '*tray*' -o -iname '*appindicator*' \) -printf '%P\n' | sort)
        if [[ -z $tray_paths ]]; then
            pass "$label daemon-only payload has no tray-specific paths"
        else
            fail "$label daemon-only payload retains tray-specific paths: ${tray_paths//$'\n'/, }"
        fi
        assert_not_contains "$relationships" 'libgtk-3-0' \
            "$label daemon-only relationship fields exclude GTK runtime"
        assert_not_contains "$relationships" 'libayatana-appindicator3-1' \
            "$label daemon-only relationship fields exclude AppIndicator runtime"
    fi

    check_tree_modes "$data" "$label"
    check_control_modes "$control" "$label"
    check_archive_owners_and_types "$artifact" "$label" "$work"
    check_symlinks_bounded "$data" "$label"

    expected_machine=$(elf_machine_for_arch "$architecture")
    for binary in "$data/usr/bin/punto-daemon" "$data/usr/bin/punto-tray"; do
        [[ -e $binary ]] || continue
        binary_name=$(basename "$binary")
        assert_contains "$(/usr/bin/file "$binary")" 'ELF' "$label $binary_name is ELF"
        if grep -Fq 'not stripped' <<<"$(/usr/bin/file "$binary")"; then
            fail "$label $binary_name is not stripped"
        else
            pass "$label $binary_name is stripped"
        fi
        machine=$(/usr/bin/readelf -h "$binary" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
        if [[ -n $expected_machine && $machine == "$expected_machine" ]]; then
            pass "$label $binary_name ELF machine matches $architecture"
        else
            fail "$label $binary_name machine '$machine' does not match '$architecture'"
        fi
        elf_header=$(/usr/bin/readelf -W -h "$binary" 2>/dev/null || true)
        program_headers=$(/usr/bin/readelf -W -l "$binary" 2>/dev/null || true)
        dynamic=$(/usr/bin/readelf -d "$binary" 2>/dev/null || true)
        symbols=$(/usr/bin/readelf -Ws "$binary" 2>/dev/null || true)
        assert_matches "$elf_header" 'Type:[[:space:]]+DYN .*Position-Independent' \
            "$label $binary_name is a PIE executable"
        assert_contains "$program_headers" 'GNU_RELRO' \
            "$label $binary_name has a GNU_RELRO segment"
        assert_matches "$dynamic" 'BIND_NOW|Flags:.*NOW' \
            "$label $binary_name enables immediate binding for full RELRO"
        stack_segment=$(grep -E 'GNU_STACK' <<<"$program_headers" || true)
        assert_matches "$stack_segment" 'GNU_STACK.*RW[[:space:]]' \
            "$label $binary_name declares a non-executable stack"
        assert_not_contains "$stack_segment" 'RWE' \
            "$label $binary_name stack is not executable"
        assert_contains "$symbols" '__stack_chk_fail' \
            "$label $binary_name contains stack-protector instrumentation"
        assert_not_contains "$dynamic" 'libevent' \
            "$label $binary_name has no obsolete libevent dynamic dependency"
    done

    symbols=$(/usr/bin/readelf -Ws "$data/usr/bin/punto-daemon" 2>/dev/null || true)
    assert_matches "$symbols" '__[[:alnum:]_]+_chk(@|$)' \
        "$label punto-daemon contains fortified libc calls"
    assert_not_contains "$(/usr/bin/strings -a "$data/usr/bin/punto-daemon")" \
        'PUNTO_TEST_MAIN_EXCEPTION' \
        "$label production daemon excludes the test-only exception seam"

    assert_matches "$(/usr/bin/readelf -d "$data/usr/bin/punto-daemon" 2>/dev/null || true)" \
        'Shared library: \[libyaml-cpp\.so\.[^]]+\]' \
        "$label daemon records its libyaml-cpp DT_NEEDED dependency"

    for package in interception-tools hunspell-en-us hunspell-ru netcat-openbsd passwd; do
        assert_matches ",$depends," "(^|,)[[:space:]]*${package//+/\\+}([[:space:](,]|$)" \
            "$label Depends includes required runtime $package"
    done
    if grep -Eq '(^|[;&|()[:space:]])((/usr/sbin/|/sbin/)?groupadd)([[:space:]]|$)' \
        "$control/postinst"; then
        assert_relation_package "$depends" passwd \
            "$label Depends declares passwd because postinst invokes groupadd"
    else
        fail "$label postinst does not expose the expected groupadd provisioning path"
    fi
    check_cli_command_dependencies "$data/usr/bin/punto" "$depends" "$label"
    assert_exact_derived_dependencies "$artifact" "$data" "$label" "$work/shlibs"
    check_compile_portability "$work/../repo" "$label"

    if command -v lintian >/dev/null 2>&1; then
        set +e
        timeout --signal=KILL 60s lintian --fail-on error "$artifact" >"$work/lintian.log" 2>&1
        local lint_rc=$?
        set +e
        if [[ $lint_rc -eq 0 ]]; then
            pass "$label lintian reports no errors"
        else
            local lint_detail
            lint_detail=$(tail -n 8 "$work/lintian.log" | tr '\n' ';')
            fail "$label lintian reports no errors (rc=$lint_rc: $lint_detail)"
        fi
    else
        printf 'SKIP: lintian unavailable; manifest/archive assertions remain authoritative\n'
    fi
}

prepare_lifecycle_root() {
    local root=$1 applet library
    mkdir -p "$root/bin" "$root/contract-bin" "$root/contract-state" "$root/dev" \
        "$root/etc" "$root/tmp" "$root/var/lib/dpkg/info" "$root/var/lib/dpkg/updates" \
        "$root/var/lib/dpkg/triggers" "$root/var/lib/dpkg/parts" "$root/var/log" \
        "$root/run/systemd/system" "$root/usr/bin" "$root/usr/sbin"
    chmod 0777 "$root/tmp" "$root/contract-state"
    : >"$root/var/lib/dpkg/status"
    : >"$root/var/lib/dpkg/available"
    : >"$root/dev/null"
    printf 'root:x:0:0:root:/root:/bin/sh\n' >"$root/etc/passwd"
    printf 'root:x:0:\n' >"$root/etc/group"

    cp -- /bin/bash "$root/bin/bash"
    while IFS= read -r library; do
        [[ -n $library ]] || continue
        mkdir -p "$root$(dirname "$library")"
        cp -- "$library" "$root$library"
    done < <(ldd /bin/bash | awk '/=> \// {print $3} $1 ~ /^\// {print $1}')
    cp -- /usr/bin/busybox "$root/bin/busybox"
    for applet in sh cat chmod chown cp date grep id mkdir mv rm sed sleep stat tr; do
        ln -s busybox "$root/bin/$applet"
    done
    cp -- /usr/bin/timeout "$root/usr/bin/timeout"

    cat >"$root/contract-bin/getent" <<'SPY'
#!/bin/sh
if [ "${1:-}" = group ] && [ "${2:-}" = punto ] && \
   [ -f /contract-state/group.exists ]; then
    printf 'punto:x:981:\n'
    exit 0
fi
exit 2
SPY
    cat >"$root/contract-bin/groupadd" <<'SPY'
#!/bin/sh
printf 'GROUPADD' >>/contract-state/calls.log
printf ' %s' "$@" >>/contract-state/calls.log
printf '\n' >>/contract-state/calls.log
if [ "${CONTRACT_GROUP_MODE:-ok}" = fail ]; then
    exit 5
fi
if [ -f /contract-state/group.exists ]; then
    printf 'DUPLICATE groupadd\n' >>/contract-state/calls.log
    exit 9
fi
: >/contract-state/group.exists
exit 0
SPY
    : >"$root/contract-state/tray-1000.active"
    cat >"$root/contract-bin/systemctl" <<'SPY'
#!/bin/sh
printf 'SYSTEMCTL' >>/contract-state/calls.log
printf ' %s' "$@" >>/contract-state/calls.log
printf '\n' >>/contract-state/calls.log
case "$*" in
    '--system --no-legend --plain --state=active --type=service list-units user@*.service')
        printf '%s\n' \
            'user@1000.service loaded active running User Manager for UID 1000' \
            'user@1001.service loaded active running User Manager for UID 1001' \
            'user@01.service loaded active running malformed leading zero' \
            'user@evil.service loaded active running malformed nonnumeric'
        exit 0
        ;;
    '--quiet --no-block --user --machine=1000@ try-restart -- punto-tray.service')
        [ "${CONTRACT_SERVICE_MODE:-ok}" = fail ] && exit 6
        [ -f /contract-state/tray-1000.active ] && \
            printf 'RESTARTED 1000\n' >>/contract-state/calls.log
        ;;
    '--quiet --no-block --user --machine=1001@ try-restart -- punto-tray.service')
        [ "${CONTRACT_SERVICE_MODE:-ok}" = fail ] && exit 6
        [ -f /contract-state/tray-1001.active ] && \
            printf 'RESTARTED 1001\n' >>/contract-state/calls.log
        ;;
    *) exit 97 ;;
esac
exit 0
SPY
    cat >"$root/usr/sbin/policy-rc.d" <<'SPY'
#!/bin/sh
printf 'POLICY_RC_D' >>/contract-state/calls.log
printf ' %s' "$@" >>/contract-state/calls.log
printf '\n' >>/contract-state/calls.log
case "${CONTRACT_POLICY_MODE:-allow}" in
    allow) exit 0 ;;
    deny) exit 101 ;;
    error) exit 103 ;;
    *) exit 97 ;;
esac
SPY
    cat >"$root/contract-bin/invoke-rc.d" <<'SPY'
#!/bin/sh
printf 'INVOKE_RC_D' >>/contract-state/calls.log
printf ' %s' "$@" >>/contract-state/calls.log
printf '\n' >>/contract-state/calls.log
case "$*" in
    '--skip-systemd-native udevmon restart'|'--skip-systemd-native udevmon stop') ;;
    *) exit 97 ;;
esac
[ "${CONTRACT_SERVICE_MODE:-ok}" = fail ] && exit 6
exit 0
SPY
    cat >"$root/contract-bin/deb-systemd-invoke" <<'SPY'
#!/bin/sh
printf 'DEB_SYSTEMD_INVOKE' >>/contract-state/calls.log
printf ' %s' "$@" >>/contract-state/calls.log
printf '\n' >>/contract-state/calls.log
case "$*" in
    '--user daemon-reload'|'--user stop punto-tray.service') ;;
    *) exit 97 ;;
esac
[ "${CONTRACT_SERVICE_MODE:-ok}" = fail ] && exit 6
exit 0
SPY
    for applet in usermod adduser addgroup update-rc.d deb-systemd-helper service; do
        cat >"$root/contract-bin/$applet" <<'SPY'
#!/bin/sh
printf 'UNEXPECTED %s' "${0##*/}" >>/contract-state/calls.log
printf ' %s' "$@" >>/contract-state/calls.log
printf '\n' >>/contract-state/calls.log
exit 97
SPY
    done
    chmod 0755 "$root/contract-bin/"* "$root/usr/sbin/policy-rc.d"
    : >"$root/contract-state/calls.log"
    chmod 0666 "$root/contract-state/calls.log"
}

LIFECYCLE_RC=0
LIFECYCLE_GROUP_MODE=ok
LIFECYCLE_SERVICE_MODE=ok
LIFECYCLE_POLICY_MODE=allow

run_lifecycle_command() {
    local root=$1 name=$2
    local output="$tmp_root/lifecycle-$name.log"
    shift 2
    set +e
    timeout --signal=KILL 45s bwrap \
        --ro-bind / / --bind "$tmp_root" "$tmp_root" \
        --dev-bind /dev/null "$root/dev/null" --proc /proc --dev /dev \
        --unshare-net --unshare-pid --unshare-user --uid 0 --gid 0 \
        --cap-add CAP_SYS_CHROOT \
        --die-with-parent --new-session --clearenv \
        --setenv PATH /contract-bin:/usr/local/sbin:/usr/sbin:/sbin:/bin:/usr/bin \
        --setenv LC_ALL C.UTF-8 --setenv DEBIAN_FRONTEND noninteractive \
        --setenv CONTRACT_GROUP_MODE "$LIFECYCLE_GROUP_MODE" \
        --setenv CONTRACT_SERVICE_MODE "$LIFECYCLE_SERVICE_MODE" \
        --setenv CONTRACT_POLICY_MODE "$LIFECYCLE_POLICY_MODE" \
        "$@" >"$output" 2>&1
    LIFECYCLE_RC=$?
    set +e
}

prepare_upgrade_artifact() {
    local base_artifact=$1 output=$2 tree=$3 upgrade_version=$4
    rm -rf -- "$tree"
    /usr/bin/dpkg-deb -R "$base_artifact" "$tree" >/dev/null
    sed -i "s/^Version:.*/Version: $upgrade_version/" "$tree/DEBIAN/control"
    if [[ -f $tree/etc/punto/config.yaml ]]; then
        printf '\n# contract-upstream-version: %s\n' "$upgrade_version" \
            >>"$tree/etc/punto/config.yaml"
    else
        fail "B14 lifecycle fixture cannot mutate absent /etc/punto/config.yaml conffile"
        return 1
    fi
    rm -f -- "$tree/DEBIAN/md5sums"
    /usr/bin/dpkg-deb --build --root-owner-group "$tree" "$output" >/dev/null
}

assert_package_status() {
    local root=$1 expected=$2 message=$3 actual
    actual=$(/usr/bin/dpkg-query --admindir="$root/var/lib/dpkg" \
        -W -f='${db:Status-Abbrev}' punto-switcher 2>/dev/null || true)
    if [[ $actual == "$expected" ]]; then
        pass "$message package state is $expected"
    else
        fail "$message package state is '$actual', expected '$expected'"
    fi
}

assert_package_absent() {
    local root=$1 message=$2
    if ! /usr/bin/dpkg-query --admindir="$root/var/lib/dpkg" \
        -W -f='${db:Status-Abbrev}' punto-switcher >/dev/null 2>&1; then
        pass "$message package database entry is absent"
    else
        fail "$message package database entry still exists"
    fi
}

assert_lifecycle_service_log() {
    local root=$1 expected=$2 message=$3 actual
    actual=$(grep -E '^(INVOKE_RC_D|DEB_SYSTEMD_INVOKE|SYSTEMCTL)' \
        "$root/contract-state/calls.log" || true)
    if [[ $actual == "$expected" ]] && \
       ! grep -Eq '^(UNEXPECTED|INVOKE_RC_D)' "$root/contract-state/calls.log"; then
        pass "$message does not control a foreign service"
    else
        fail "$message service-helper log differs (actual: ${actual//$'\n'/; })"
    fi
}

run_package_lifecycle_gate() {
    local artifact=$1 root="$tmp_root/lifecycle-happy/root"
    local upgrade_tree="$tmp_root/lifecycle-upgrade-tree"
    local upgrade_artifact="$tmp_root/punto-switcher-lifecycle-upgrade.deb"
    local upgrade_version="${EXPECTED_VERSION}+contract1"
    local before_repeat after_repeat local_hash calls postinst
    local before_punto_rc after_punto_rc
    local expected_service_calls

    expected_service_calls=""

    prepare_lifecycle_root "$root"
    run_lifecycle_command "$root" install /usr/bin/dpkg --root="$root" \
        --log="$root/var/log/dpkg.log" --force-depends --force-confdef \
        --force-confold -i "$artifact"
    assert_zero "$LIFECYCLE_RC" "B14 lifecycle isolated install"
    expected_service_calls='DEB_SYSTEMD_INVOKE --user daemon-reload'
    assert_lifecycle_service_log "$root" "$expected_service_calls" \
        "B14 lifecycle install"
    if [[ ! -e $root/etc/interception ]]; then
        pass "B14 lifecycle install leaves global interception config absent"
    else
        fail "B14 lifecycle install created global interception config"
    fi
    assert_package_status "$root" 'ii ' "B14 lifecycle install"
    assert_file "$root/etc/punto/config.yaml" "B14 lifecycle install creates active conffile"
    if [[ -f $root/contract-state/group.exists ]]; then
        pass "B14 lifecycle install creates the punto group once"
    else
        fail "B14 lifecycle install did not create the punto group"
    fi
    if [[ ! -f $root/etc/punto/config.yaml ]]; then
        run_package_setup_failure "$artifact"
        return
    fi

    if [[ -d $root/etc/punto ]]; then
        set +e
        before_repeat=$(snapshot_tree "$root/etc/punto")
        before_punto_rc=$?
        set +e
    else
        before_repeat="missing-policy-tree"
        before_punto_rc=1
    fi
    postinst="$root/var/lib/dpkg/info/punto-switcher.postinst"
    if [[ -x $postinst ]]; then
        run_lifecycle_command "$root" repeat-postinst /usr/sbin/chroot "$root" \
            /var/lib/dpkg/info/punto-switcher.postinst configure "$EXPECTED_VERSION"
        assert_zero "$LIFECYCLE_RC" "B14 lifecycle repeated postinst"
        expected_service_calls+=$'\nDEB_SYSTEMD_INVOKE --user daemon-reload'
        expected_service_calls+=$'\nSYSTEMCTL --system --no-legend --plain --state=active --type=service list-units user@*.service'
        expected_service_calls+=$'\nSYSTEMCTL --quiet --no-block --user --machine=1000@ try-restart -- punto-tray.service'
        expected_service_calls+=$'\nSYSTEMCTL --quiet --no-block --user --machine=1001@ try-restart -- punto-tray.service'
        assert_lifecycle_service_log "$root" "$expected_service_calls" \
            "B14 lifecycle repeated postinst"
    else
        fail "B14 lifecycle installed no executable postinst"
        run_package_setup_failure "$artifact"
        return
    fi
    if [[ -d $root/etc/punto ]]; then
        set +e
        after_repeat=$(snapshot_tree "$root/etc/punto")
        after_punto_rc=$?
        set +e
    else
        after_repeat="missing-policy-tree-after-repeat"
        after_punto_rc=1
    fi
    if [[ $before_punto_rc -eq 0 && $after_punto_rc -eq 0 && \
          $before_repeat == "$after_repeat" ]]; then
        pass "B14 lifecycle repeated postinst is byte/metadata idempotent"
    else
        fail "B14 lifecycle repeated postinst snapshot failed or policy state changed"
    fi
    calls=$(<"$root/contract-state/calls.log")
    if [[ $(grep -c '^GROUPADD --system punto$' <<<"$calls" || true) -eq 1 ]] && \
       ! grep -q '^DUPLICATE groupadd$' <<<"$calls"; then
        pass "B14 lifecycle repeated postinst never recreates the system group"
    else
        fail "B14 lifecycle group creation is not idempotent: ${calls//$'\n'/; }"
    fi

    printf '\n# contract-local-edit: preserve-me\n' >>"$root/etc/punto/config.yaml"
    local_hash=$(sha256sum "$root/etc/punto/config.yaml" | awk '{print $1}')
    if ! prepare_upgrade_artifact "$artifact" "$upgrade_artifact" "$upgrade_tree" \
        "$upgrade_version"; then
        return
    fi
    run_lifecycle_command "$root" upgrade /usr/bin/dpkg --root="$root" \
        --log="$root/var/log/dpkg.log" --force-depends --force-confdef \
        --force-confold -i "$upgrade_artifact"
    assert_zero "$LIFECYCLE_RC" "B14 lifecycle isolated upgrade"
    expected_service_calls+=$'\nDEB_SYSTEMD_INVOKE --user daemon-reload'
    expected_service_calls+=$'\nSYSTEMCTL --system --no-legend --plain --state=active --type=service list-units user@*.service'
    expected_service_calls+=$'\nSYSTEMCTL --quiet --no-block --user --machine=1000@ try-restart -- punto-tray.service'
    expected_service_calls+=$'\nSYSTEMCTL --quiet --no-block --user --machine=1001@ try-restart -- punto-tray.service'
    assert_lifecycle_service_log "$root" "$expected_service_calls" \
        "B14 lifecycle upgrade"
    assert_package_status "$root" 'ii ' "B14 lifecycle upgrade"
    if [[ -f $root/etc/punto/config.yaml ]]; then
        assert_contains "$(<"$root/etc/punto/config.yaml")" '# contract-local-edit: preserve-me' \
            "B14 lifecycle upgrade preserves the local conffile edit"
    else
        fail "B14 lifecycle upgrade removed the active conffile"
    fi
    if [[ -f $root/etc/punto/config.yaml && \
          $(sha256sum "$root/etc/punto/config.yaml" | awk '{print $1}') == "$local_hash" ]]; then
        pass "B14 lifecycle upgrade preserves the edited conffile byte-for-byte"
    else
        fail "B14 lifecycle upgrade rewrote the locally edited conffile"
    fi
    assert_file "$root/etc/punto/config.yaml.dpkg-dist" \
        "B14 lifecycle upgrade retains the new upstream conffile as .dpkg-dist"
    if [[ -f $root/etc/punto/config.yaml.dpkg-dist ]]; then
        assert_contains "$(<"$root/etc/punto/config.yaml.dpkg-dist")" \
            "# contract-upstream-version: $upgrade_version" \
            "B14 lifecycle .dpkg-dist contains the new upstream config"
    fi

    run_lifecycle_command "$root" remove /usr/bin/dpkg --root="$root" \
        --log="$root/var/log/dpkg.log" --force-depends -r punto-switcher
    assert_zero "$LIFECYCLE_RC" "B14 lifecycle isolated remove"
    expected_service_calls+=$'\nDEB_SYSTEMD_INVOKE --user stop punto-tray.service'
    expected_service_calls+=$'\nDEB_SYSTEMD_INVOKE --user daemon-reload'
    assert_lifecycle_service_log "$root" "$expected_service_calls" \
        "B14 lifecycle remove"
    assert_package_status "$root" 'rc ' "B14 lifecycle remove"
    assert_file "$root/etc/punto/config.yaml" "B14 lifecycle remove retains the conffile"
    if [[ -f $root/etc/punto/config.yaml && \
          $(sha256sum "$root/etc/punto/config.yaml" | awk '{print $1}') == "$local_hash" ]]; then
        pass "B14 lifecycle remove retains the exact local conffile"
    else
        fail "B14 lifecycle remove changed the local conffile"
    fi

    run_lifecycle_command "$root" purge /usr/bin/dpkg --root="$root" \
        --log="$root/var/log/dpkg.log" --force-depends -P punto-switcher
    assert_zero "$LIFECYCLE_RC" "B14 lifecycle isolated purge"
    expected_service_calls+=$'\nDEB_SYSTEMD_INVOKE --user daemon-reload'
    assert_lifecycle_service_log "$root" "$expected_service_calls" \
        "B14 lifecycle purge"
    assert_package_absent "$root" "B14 lifecycle purge"
    if [[ ! -e $root/etc/punto/config.yaml && \
          ! -e $root/etc/punto/config.yaml.dpkg-dist ]]; then
        pass "B14 lifecycle purge removes conffile state"
    else
        fail "B14 lifecycle purge retained conffile state"
    fi
    run_lifecycle_command "$root" purge-repeat /usr/bin/dpkg --root="$root" \
        --log="$root/var/log/dpkg.log" --force-depends -P punto-switcher
    assert_zero "$LIFECYCLE_RC" "B14 lifecycle repeated purge is idempotent"
    assert_lifecycle_service_log "$root" "$expected_service_calls" \
        "B14 lifecycle repeated purge"
    assert_package_absent "$root" "B14 lifecycle repeated purge"

    run_package_setup_failure "$artifact"
    run_package_service_failure "$artifact"
    run_package_policy_denial "$artifact"
}

run_package_setup_failure() {
    local artifact=$1 label=group group_mode=fail service_mode=ok
    local root="$tmp_root/lifecycle-failure-group/root" calls
    prepare_lifecycle_root "$root"
    LIFECYCLE_GROUP_MODE=$group_mode
    LIFECYCLE_SERVICE_MODE=$service_mode
    run_lifecycle_command "$root" "failure-$label" /usr/bin/dpkg --root="$root" \
        --log="$root/var/log/dpkg.log" --force-depends --force-confdef \
        --force-confold -i "$artifact"
    assert_nonzero "$LIFECYCLE_RC" \
        "B14 lifecycle group setup failure propagates"
    if [[ $LIFECYCLE_RC -ne 124 && $LIFECYCLE_RC -ne 137 ]]; then
        pass "B14 lifecycle $label setup failure is bounded"
    else
        fail "B14 lifecycle $label setup failure escaped the bound"
    fi
    calls=$(<"$root/contract-state/calls.log")
    assert_lifecycle_service_log "$root" "" \
        "B14 lifecycle group failure has no service side effect"
    if [[ ! -f $root/contract-state/group.exists ]]; then
        pass "B14 lifecycle failed group setup leaves no false group state"
    else
        fail "B14 lifecycle failed group setup left false success state"
    fi
    if [[ $(/usr/bin/dpkg-query --admindir="$root/var/lib/dpkg" \
          -W -f='${db:Status-Abbrev}' punto-switcher 2>/dev/null || true) != 'ii ' ]]; then
        pass "B14 lifecycle group failure never reports package configured"
    else
        fail "B14 lifecycle group failure has the wrong package state"
    fi
    LIFECYCLE_GROUP_MODE=ok
    LIFECYCLE_SERVICE_MODE=ok
}

run_package_service_failure() {
    local artifact=$1 root="$tmp_root/lifecycle-failure-service/root"
    local expected
    prepare_lifecycle_root "$root"
    run_lifecycle_command "$root" service-fixture-install /usr/bin/dpkg \
        --root="$root" --log="$root/var/log/dpkg.log" --force-depends \
        --force-confdef --force-confold -i "$artifact"
    assert_zero "$LIFECYCLE_RC" \
        "B14 lifecycle service-failure fixture installs"
    : >"$root/contract-state/calls.log"
    LIFECYCLE_SERVICE_MODE=fail
    run_lifecycle_command "$root" failure-service /usr/sbin/chroot "$root" \
        /var/lib/dpkg/info/punto-switcher.postinst configure "$EXPECTED_VERSION"
    assert_zero "$LIFECYCLE_RC" \
        "B14 lifecycle service helper failures are fail-soft"
    expected=$'DEB_SYSTEMD_INVOKE --user daemon-reload\nSYSTEMCTL --system --no-legend --plain --state=active --type=service list-units user@*.service\nSYSTEMCTL --quiet --no-block --user --machine=1000@ try-restart -- punto-tray.service\nSYSTEMCTL --quiet --no-block --user --machine=1001@ try-restart -- punto-tray.service'
    assert_lifecycle_service_log "$root" "$expected" \
        "B14 lifecycle failed helpers keep exact owned-unit ordering"
    assert_package_status "$root" 'ii ' \
        "B14 lifecycle service helper failures"
    LIFECYCLE_GROUP_MODE=ok
    LIFECYCLE_SERVICE_MODE=ok
}

run_package_policy_denial() {
    local artifact=$1 root="$tmp_root/lifecycle-policy-denial/root"
    local expected calls
    prepare_lifecycle_root "$root"
    run_lifecycle_command "$root" policy-fixture-install /usr/bin/dpkg \
        --root="$root" --log="$root/var/log/dpkg.log" --force-depends \
        --force-confdef --force-confold -i "$artifact"
    assert_zero "$LIFECYCLE_RC" "B14 lifecycle policy fixture installs"
    : >"$root/contract-state/calls.log"
    LIFECYCLE_POLICY_MODE=deny
    run_lifecycle_command "$root" policy-denial /usr/sbin/chroot "$root" \
        /var/lib/dpkg/info/punto-switcher.postinst configure "$EXPECTED_VERSION"
    assert_zero "$LIFECYCLE_RC" "B14 lifecycle policy denial is fail-soft"
    expected='DEB_SYSTEMD_INVOKE --user daemon-reload'
    assert_lifecycle_service_log "$root" "$expected" \
        "B14 lifecycle policy denial performs no per-user restart"
    calls=$(<"$root/contract-state/calls.log")
    assert_contains "$calls" 'POLICY_RC_D punto-tray.service restart' \
        "B14 lifecycle policy denial consults policy before enumeration"
    assert_not_contains "$calls" 'SYSTEMCTL ' \
        "B14 lifecycle policy denial skips systemctl"
    LIFECYCLE_POLICY_MODE=allow
}

run_full_to_daemon_lifecycle_gate() {
    local full_artifact=$1 daemon_artifact=$2
    local root="$tmp_root/lifecycle-full-to-daemon/root"
    local upgrade_tree="$tmp_root/lifecycle-full-to-daemon/tree"
    local upgrade_artifact="$tmp_root/lifecycle-full-to-daemon/punto-switcher-daemon-upgrade.deb"
    local expected

    prepare_lifecycle_root "$root"
    run_lifecycle_command "$root" flavor-install /usr/bin/dpkg --root="$root" \
        --log="$root/var/log/dpkg.log" --force-depends --force-confdef \
        --force-confold -i "$full_artifact"
    assert_zero "$LIFECYCLE_RC" "B14 full-to-daemon fixture installs full package"
    [[ $LIFECYCLE_RC -eq 0 ]] || return

    if ! prepare_upgrade_artifact "$daemon_artifact" "$upgrade_artifact" \
        "$upgrade_tree" "${EXPECTED_VERSION}+daemon1"; then
        return
    fi
    : >"$root/contract-state/calls.log"
    run_lifecycle_command "$root" flavor-upgrade /usr/bin/dpkg --root="$root" \
        --log="$root/var/log/dpkg.log" --force-depends --force-confdef \
        --force-confold -i "$upgrade_artifact"
    assert_zero "$LIFECYCLE_RC" "B14 full-to-daemon replacement succeeds"
    expected=$'DEB_SYSTEMD_INVOKE --user stop punto-tray.service\nDEB_SYSTEMD_INVOKE --user daemon-reload'
    assert_lifecycle_service_log "$root" "$expected" \
        "B14 full-to-daemon replacement"
    if ! grep -Eq 'DEB_SYSTEMD_INVOKE .* (start|restart)( |$)' \
        "$root/contract-state/calls.log"; then
        pass "B14 full-to-daemon replacement never starts a static user unit"
    else
        fail "B14 full-to-daemon replacement attempted to start a static user unit"
    fi
    if [[ ! -e $root/usr/bin/punto-tray && \
          ! -e $root/usr/lib/systemd/user/punto-tray.service && \
          ! -e $root/etc/xdg/autostart/punto-tray.desktop ]]; then
        pass "B14 full-to-daemon replacement removes all tray payloads"
    else
        fail "B14 full-to-daemon replacement retained a tray payload"
    fi

    : >"$root/contract-state/calls.log"
    run_lifecycle_command "$root" flavor-purge /usr/bin/dpkg --root="$root" \
        --log="$root/var/log/dpkg.log" --force-depends -P punto-switcher
    assert_zero "$LIFECYCLE_RC" "B14 daemon flavor purges after replacement"
    expected=$'DEB_SYSTEMD_INVOKE --user stop punto-tray.service\nDEB_SYSTEMD_INVOKE --user daemon-reload\nDEB_SYSTEMD_INVOKE --user daemon-reload'
    assert_lifecycle_service_log "$root" "$expected" \
        "B14 daemon flavor purge"
    assert_package_absent "$root" "B14 daemon flavor purge"
    if [[ ! -e $root/etc/xdg/autostart/punto-tray.desktop ]]; then
        pass "B14 daemon flavor purge leaves no obsolete tray autostart"
    else
        fail "B14 daemon flavor purge retained obsolete tray autostart"
    fi
}

run_reproducibility_and_real_artifacts() {
    local artifact_a sha_a sha_file_a artifact_b sha_b sha_file_b
    local mutation_repo mutation_artifact mutation_log version_output tray_output cli_output help_output
    local binary binary_strings readme_path readme_current_lines config_path postinst_path
    local version_rc tray_rc cli_rc help_rc mutation_version_regex
    local daemon_artifact daemon_calls

    if [[ $VERSION_VALID -ne 1 ]]; then
        fail "B14/B15: real release builds require the valid repository VERSION"
        return
    fi

    run_real_build reproducible-a
    artifact_a=$RUN_ARTIFACT; sha_file_a=$RUN_SHA_FILE
    assert_real_build_safe 'B14/B15: clean release build A'
    if cmp -s "$REPO_ROOT/VERSION" "$RUN_REPO/VERSION"; then
        pass "B15: clean build A preserves the copied canonical VERSION byte-for-byte"
    else
        fail "B15: clean build A rewrote its copied canonical VERSION"
    fi
    [[ -n $artifact_a ]] || return
    sha_a=$(sha256sum "$artifact_a" | awk '{print $1}')
    inspect_artifact "$artifact_a" "$EXPECTED_VERSION" yes 'B14: full release artifact' \
        "$tmp_root/real-reproducible-a/inspect"
    run_package_lifecycle_gate "$artifact_a"
    assert_file "$sha_file_a" "B15: build A emits SHA256SUMS"

    run_real_build reproducible-b
    artifact_b=$RUN_ARTIFACT; sha_file_b=$RUN_SHA_FILE
    assert_real_build_safe 'B15: clean release build B'
    if cmp -s "$REPO_ROOT/VERSION" "$RUN_REPO/VERSION"; then
        pass "B15: clean build B preserves the copied canonical VERSION byte-for-byte"
    else
        fail "B15: clean build B rewrote its copied canonical VERSION"
    fi
    [[ -n $artifact_b ]] || return
    sha_b=$(sha256sum "$artifact_b" | awk '{print $1}')
    if [[ $sha_a == "$sha_b" ]]; then
        pass "B15: two clean SOURCE_DATE_EPOCH builds have identical .deb SHA256"
    else
        fail "B15: clean build checksums differ ($sha_a vs $sha_b)"
    fi
    if [[ $(basename "$artifact_a") == "$(basename "$artifact_b")" ]]; then
        pass "B15: reproducible builds use the same artifact name"
    else
        fail "B15: artifact names differ"
    fi
    assert_file "$sha_file_b" "B15: build B emits SHA256SUMS"
    if [[ -f $sha_file_a && -f $sha_file_b ]]; then
        if cmp -s "$sha_file_a" "$sha_file_b"; then
            pass "B15: SHA256SUMS files are byte-identical"
        else
            fail "B15: SHA256SUMS files differ"
        fi
        assert_exact_line "$(<"$sha_file_a")" "$sha_a  $(basename "$artifact_a")" \
            "B15: SHA256SUMS binds the exact artifact"
    fi

    run_real_build version-mutation "$MUTATED_VERSION"
    mutation_repo=$RUN_REPO; mutation_artifact=$RUN_ARTIFACT; mutation_log=$RUN_LOG
    assert_real_build_safe 'B15: canonical VERSION mutation build'
    [[ -n $mutation_artifact ]] || return
    if [[ $(basename "$mutation_artifact") == \
          "punto-switcher_${MUTATED_VERSION}_$(/usr/bin/dpkg --print-architecture).deb" ]]; then
        pass "B15: VERSION mutation changes the artifact name"
    else
        fail "B15: mutated artifact name is $(basename "$mutation_artifact")"
    fi
    if [[ $(/usr/bin/dpkg-deb -f "$mutation_artifact" Version) == "$MUTATED_VERSION" ]]; then
        pass "B15: VERSION mutation changes package metadata"
    else
        fail "B15: VERSION mutation did not change package metadata"
    fi
    mkdir -p "$tmp_root/real-version-mutation/extract" \
        "$tmp_root/real-version-mutation/control"
    /usr/bin/dpkg-deb -x "$mutation_artifact" "$tmp_root/real-version-mutation/extract"
    /usr/bin/dpkg-deb -e "$mutation_artifact" "$tmp_root/real-version-mutation/control"
    assert_file "$tmp_root/real-version-mutation/extract/usr/share/punto-switcher/VERSION" \
        "B15: mutated artifact carries canonical VERSION"
    if [[ -f $tmp_root/real-version-mutation/extract/usr/share/punto-switcher/VERSION ]]; then
        if [[ $(tr -d '[:space:]' <"$tmp_root/real-version-mutation/extract/usr/share/punto-switcher/VERSION") == \
              "$MUTATED_VERSION" ]]; then
            pass "B15: packaged VERSION reflects mutation"
        else
            fail "B15: packaged VERSION is stale"
        fi
    fi
    run_artifact_probe daemon-version \
        "$tmp_root/real-version-mutation/extract/usr/share/punto-switcher/VERSION" \
        "$tmp_root/real-version-mutation/extract/usr/bin/punto-daemon" --version
    version_output=$PROBE_OUTPUT
    version_rc=$PROBE_RC
    assert_zero "$version_rc" "B15: daemon --version exits successfully"
    assert_contains "$version_output" "$MUTATED_VERSION" "B15: daemon --version reflects canonical mutation"
    run_artifact_probe tray-version \
        "$tmp_root/real-version-mutation/extract/usr/share/punto-switcher/VERSION" \
        "$tmp_root/real-version-mutation/extract/usr/bin/punto-tray" --version
    tray_output=$PROBE_OUTPUT
    tray_rc=$PROBE_RC
    assert_zero "$tray_rc" "B15: tray --version exits successfully"
    assert_contains "$tray_output" "$MUTATED_VERSION" "B15: tray --version reflects canonical mutation"
    for binary in \
        "$tmp_root/real-version-mutation/extract/usr/bin/punto-daemon" \
        "$tmp_root/real-version-mutation/extract/usr/bin/punto-tray"; do
        [[ -f $binary ]] || continue
        binary_strings=$(/usr/bin/strings -a "$binary")
        assert_contains "$binary_strings" "$MUTATED_VERSION" \
            "B15: $(basename "$binary") compiled version strings reflect mutation"
        assert_not_contains "$binary_strings" "$EXPECTED_VERSION" \
            "B15: $(basename "$binary") compiled version strings contain no stale root version"
    done
    run_artifact_probe cli-version \
        "$tmp_root/real-version-mutation/extract/usr/share/punto-switcher/VERSION" \
        /bin/bash "$tmp_root/real-version-mutation/extract/usr/bin/punto" --version
    cli_output=$PROBE_OUTPUT
    cli_rc=$PROBE_RC
    assert_zero "$cli_rc" "B15: CLI --version exits successfully"
    if [[ $cli_output == "punto $MUTATED_VERSION" ]]; then
        pass "B15: CLI --version reflects canonical mutation"
    else
        fail "B15: mutated CLI version output is '$cli_output'"
    fi
    run_artifact_probe cli-help \
        "$tmp_root/real-version-mutation/extract/usr/share/punto-switcher/VERSION" \
        /bin/bash "$tmp_root/real-version-mutation/extract/usr/bin/punto" --help
    help_output=$PROBE_OUTPUT
    help_rc=$PROBE_RC
    assert_zero "$help_rc" "B15: CLI --help exits successfully"
    assert_contains "$help_output" "$MUTATED_VERSION" \
        "B15: CLI help/version presentation reflects canonical mutation"
    if [[ -f $tmp_root/real-version-mutation/control/control ]]; then
        assert_contains "$(<"$tmp_root/real-version-mutation/control/control")" \
            "Version: $MUTATED_VERSION" "B15: generated control surface reflects mutation"
    else
        fail "B15: mutation package has no generated control surface"
    fi
    readme_path="$tmp_root/real-version-mutation/extract/usr/share/doc/punto-switcher/README.md"
    if [[ -f $readme_path ]]; then
        readme_current_lines=$(grep -E \
            'img\.shields\.io/badge/version-|dpkg[[:space:]]+-i[[:space:]]+punto-switcher_' \
            "$readme_path" || true)
        assert_matches "$readme_current_lines" \
            "img\\.shields\\.io/badge/version-${MUTATED_VERSION//./\\.}-" \
            "B15: packaged README current-version badge reflects mutation"
        assert_matches "$readme_current_lines" \
            "dpkg[[:space:]]+-i[[:space:]]+punto-switcher_${MUTATED_VERSION//./\\.}_" \
            "B15: packaged README install command reflects mutation"
        assert_not_contains "$readme_current_lines" "$EXPECTED_VERSION" \
            "B15: packaged README current-version surfaces contain no stale root version"
    else
        fail "B15: mutation package has no packaged README surface"
    fi
    config_path="$tmp_root/real-version-mutation/extract/etc/punto/config.yaml"
    if [[ -f $config_path ]]; then
        assert_exact_line "$(<"$config_path")" "# Config version: $MUTATED_VERSION" \
            "B15: packaged config version marker reflects mutation"
        assert_not_contains "$(grep -E '^#[[:space:]]*Config version:' "$config_path" || true)" \
            "$EXPECTED_VERSION" "B15: packaged config marker contains no stale root version"
    else
        fail "B15: mutation package has no active config surface"
    fi
    postinst_path="$tmp_root/real-version-mutation/control/postinst"
    if [[ -f $postinst_path ]]; then
        assert_exact_line "$(<"$postinst_path")" "PKG_VERSION=\"$MUTATED_VERSION\"" \
            "B15: generated postinst version reflects mutation"
        assert_not_contains "$(grep -E '^PKG_VERSION=' "$postinst_path" || true)" \
            "$EXPECTED_VERSION" "B15: generated postinst contains no stale root version"
    else
        fail "B15: mutation package has no postinst version surface"
    fi
    assert_file_not_contains \
        "$tmp_root/real-version-mutation/extract/usr/bin/punto" \
        "$EXPECTED_VERSION" "B15: packaged CLI source contains no stale root version"
    assert_file_not_contains \
        "$tmp_root/real-version-mutation/extract/usr/share/punto-switcher/VERSION" \
        "$EXPECTED_VERSION" "B15: packaged VERSION contains no stale root version"
    assert_file_not_contains \
        "$tmp_root/real-version-mutation/control/control" \
        "Version: $EXPECTED_VERSION" "B15: generated control contains no stale root version"
    for version_output in "$version_output" "$tray_output" "$cli_output" "$help_output" "$(<"$mutation_log")"; do
        assert_not_contains "$version_output" "$EXPECTED_VERSION" \
            "B15: a mutated product-visible version surface contains no stale root version"
    done
    assert_contains "$(<"$mutation_log")" "$MUTATED_VERSION" "B15: build presentation reflects canonical mutation"
    if [[ -f $mutation_repo/cpp/build/CMakeCache.txt ]]; then
        mutation_version_regex=${MUTATED_VERSION//./\\.}
        assert_matches "$(<"$mutation_repo/cpp/build/CMakeCache.txt")" \
            "CMAKE_PROJECT_VERSION(:[^=]*)?=$mutation_version_regex" \
            "B15: CMake project version reflects canonical mutation"
    else
        fail "B15: mutation build did not retain CMakeCache for version verification"
    fi

    run_real_build daemon-only "" \
        'libgtk-3-dev,libayatana-appindicator3-dev,libgtk-3-0,libayatana-appindicator3-1' \
        daemon
    daemon_artifact=$RUN_ARTIFACT; daemon_calls=$RUN_CALLS
    assert_real_build_safe 'B21: real daemon-only release build'
    [[ -n $daemon_artifact ]] || return
    assert_contains "$(<"$daemon_calls")" '-DBUILD_TRAY=OFF' \
        "B21: real daemon-only build configures BUILD_TRAY=OFF"
    inspect_artifact "$daemon_artifact" "$EXPECTED_VERSION" no 'B21: real daemon-only artifact' \
        "$tmp_root/real-daemon-only/inspect"
    run_full_to_daemon_lifecycle_gate "$artifact_a" "$daemon_artifact"
}

if [[ -n ${PUNTO_CONTRACT_LIFECYCLE_ONLY_ARTIFACT:-} ]]; then
    lifecycle_only_input=$PUNTO_CONTRACT_LIFECYCLE_ONLY_ARTIFACT
    if [[ ! -f $lifecycle_only_input || -L $lifecycle_only_input ]]; then
        fail "B14 lifecycle-only artifact is an existing regular non-symlink file"
        printf '\nPackaging lifecycle contract: %d checks, %d failure(s)\n' "$checks" "$failures"
        exit 1
    fi
    lifecycle_only_artifact=$(realpath -e "$lifecycle_only_input" 2>/dev/null || true)
    if [[ -z $lifecycle_only_artifact || ! -f $lifecycle_only_artifact ]] || \
       ! /usr/bin/dpkg-deb --info "$lifecycle_only_artifact" >/dev/null 2>&1; then
        fail "B14 lifecycle-only artifact is a readable Debian archive"
        printf '\nPackaging lifecycle contract: %d checks, %d failure(s)\n' "$checks" "$failures"
        exit 1
    fi
    EXPECTED_VERSION=$(/usr/bin/dpkg-deb -f "$lifecycle_only_artifact" Version 2>/dev/null || true)
    if [[ -z $EXPECTED_VERSION ]] || \
       ! /usr/bin/dpkg --validate-version "$EXPECTED_VERSION" >/dev/null 2>&1; then
        fail "B14 lifecycle-only artifact has a valid Debian version"
        printf '\nPackaging lifecycle contract: %d checks, %d failure(s)\n' "$checks" "$failures"
        exit 1
    fi
    pass "B14 lifecycle-only artifact version is valid"
    run_package_lifecycle_gate "$lifecycle_only_artifact"
    printf '\nPackaging lifecycle contract: %d checks, %d failure(s)\n' "$checks" "$failures"
    [[ $failures -eq 0 ]]
    exit
fi

run_version_source_gate
run_default_source_epoch_gate
run_required_package_matrix
run_required_tool_matrix
run_tool_provider_gate
run_optional_matrix
run_dependency_inventory_gate
run_symlink_attack_gate
run_reproducibility_and_real_artifacts

printf '\nPackaging contract: %d checks, %d failure(s)\n' "$checks" "$failures"
[[ $failures -eq 0 ]]
