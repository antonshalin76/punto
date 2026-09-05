#!/usr/bin/env bash

# Build a Debian package without installing packages or touching the running
# service. All outputs are assembled below this checkout and metadata is
# derived from the canonical VERSION file and the built ELF binaries.
set -Eeuo pipefail

export LC_ALL=C.UTF-8
umask 0022

SCRIPT_DIR=$(cd "${BASH_SOURCE[0]%/*}" && pwd -P)
cd "$SCRIPT_DIR"

PACKAGE_NAME=punto-switcher
DEFAULT_SOURCE_DATE_EPOCH=1788480000
CPP_BUILD_DIR=cpp/build
STAGE_DIR=build-deb
SHA_FILE=SHA256SUMS
BUILD_TRAY=true
SKIP_RUNTIME_INSTALLS=false

required_packages=(
    build-essential cmake pkg-config libsystemd-dev libxcb1-dev
    libxcb-xkb-dev libxau-dev libhunspell-dev libyaml-cpp-dev
    dpkg-dev binutils file
)
required_tools=(
    cmake dpkg dpkg-query dpkg-deb dpkg-shlibdeps pkg-config file strip gzip
    sha256sum
)
tray_packages=(libgtk-3-dev libayatana-appindicator3-dev)
dictionary_packages=(hunspell hunspell-en-us hunspell-ru wamerican-huge)
runtime_packages=(
    interception-tools libgtk-3-0 libayatana-appindicator3-1 netcat-openbsd
    passwd sudo util-linux
)

fail() {
    printf 'ERROR %s\n' "$1" >&2
    exit 1
}

for argument in "$@"; do
    case $argument in
        --non-interactive|--ci) ;;
        --skip-runtime-installs) SKIP_RUNTIME_INSTALLS=true ;;
        --without-tray) BUILD_TRAY=false ;;
        --help)
            printf '%s\n' \
                'Usage: build-deb.sh [--non-interactive|--ci] [--skip-runtime-installs] [--without-tray]' \
                'Builds an artifact only; it never installs packages or restarts services.'
            exit 0
            ;;
        *) fail "unsupported-option: $argument" ;;
    esac
done

load_version() {
    local path=VERSION bytes lines value

    [[ -e $path ]] || fail 'invalid-version-source: missing'
    [[ -f $path && ! -L $path ]] || fail 'invalid-version-source: malformed'
    bytes=$(wc -c <"$path") || fail 'invalid-version-source: malformed'
    lines=$(wc -l <"$path") || fail 'invalid-version-source: malformed'
    if [[ ! $bytes =~ ^[0-9]+$ || ! $lines =~ ^[0-9]+$ ]] ||
       ((bytes < 2 || bytes > 128 || lines != 1)); then
        fail 'invalid-version-source: malformed'
    fi
    LC_ALL=C grep -aEq '^[0-9]+([.][0-9]+){2}$' "$path" ||
        fail 'invalid-version-source: malformed'
    IFS= read -r value <"$path" || fail 'invalid-version-source: malformed'
    if [[ ! $value =~ ^[0-9]+([.][0-9]+){2}$ ]] ||
       ((${#value} + 1 != bytes)); then
        fail 'invalid-version-source: malformed'
    fi
    VERSION=$value
}

require_regular_input() {
    local path=$1
    [[ -f $path && ! -L $path ]] || fail "invalid-source-file: $path"
}

reject_unsafe_output() {
    local path=$1
    if [[ -L $path || ( -e $path && ! -f $path && ! -d $path ) ]]; then
        fail "unsafe-path: $path"
    fi
}

reset_directory() {
    local path=$1
    reject_unsafe_output "$path"
    [[ ! -e $path || -d $path ]] || fail "unsafe-path: $path"
    if [[ -d $path ]]; then
        rm -rf -- "$path"
    fi
    mkdir -m 0755 -- "$path"
    [[ -d $path && ! -L $path ]] || fail "unsafe-path: $path"
}

cleanup_stage() {
    if [[ -d $STAGE_DIR && ! -L $STAGE_DIR ]]; then
        rm -rf -- "$STAGE_DIR"
    fi
}
trap cleanup_stage EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

load_version

for tool in "${required_tools[@]}"; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing-required-tool: $tool"
done

for package in "${required_packages[@]}"; do
    dpkg -s "$package" >/dev/null 2>&1 || fail "missing-required-package: $package"
done

if [[ $BUILD_TRAY == true ]]; then
    for package in "${tray_packages[@]}"; do
        dpkg -s "$package" >/dev/null 2>&1 ||
            fail "missing-required-tray-package: $package"
    done
fi

for package in "${dictionary_packages[@]}"; do
    if ! dpkg -s "$package" >/dev/null 2>&1; then
        printf 'WARN optional-dictionary-missing: %s\n' "$package" >&2
    fi
done

for package in "${runtime_packages[@]}"; do
    if ! dpkg -s "$package" >/dev/null 2>&1; then
        if [[ $SKIP_RUNTIME_INSTALLS == true ]]; then
            printf 'WARN runtime-install-skipped: %s\n' "$package" >&2
        else
            printf 'WARN runtime-package-missing: %s\n' "$package" >&2
        fi
    fi
done

for input in \
    punto-cli.sh config.yaml udevmon.yaml README.md LICENSE \
    DEBIAN/control DEBIAN/postinst DEBIAN/prerm DEBIAN/postrm; do
    require_regular_input "$input"
done
if [[ $BUILD_TRAY == true ]]; then
    require_regular_input punto-tray.desktop
    require_regular_input punto-tray.service
fi

ARCHITECTURE=$(dpkg --print-architecture) || fail 'architecture-detection-failed'
[[ $ARCHITECTURE =~ ^[a-z0-9][a-z0-9-]*$ ]] || fail 'architecture-detection-failed'

SOURCE_EPOCH=${SOURCE_DATE_EPOCH:-$DEFAULT_SOURCE_DATE_EPOCH}
[[ $SOURCE_EPOCH =~ ^[0-9]+$ && ${#SOURCE_EPOCH} -le 12 ]] ||
    fail 'invalid-source-date-epoch'
CURRENT_EPOCH=$(date -u +%s) || fail 'clock-read-failed'
((SOURCE_EPOCH <= CURRENT_EPOCH)) || fail 'invalid-source-date-epoch: future'
export SOURCE_DATE_EPOCH=$SOURCE_EPOCH

OUTPUT_DEB="${PACKAGE_NAME}_${VERSION}_${ARCHITECTURE}.deb"
for output in "$CPP_BUILD_DIR" "$STAGE_DIR" "$OUTPUT_DEB" "$SHA_FILE"; do
    reject_unsafe_output "$output"
done
[[ ! -e $OUTPUT_DEB || -f $OUTPUT_DEB ]] || fail "unsafe-path: $OUTPUT_DEB"
[[ ! -e $SHA_FILE || -f $SHA_FILE ]] || fail "unsafe-path: $SHA_FILE"

reset_directory "$CPP_BUILD_DIR"

tray_flag=OFF
targets=(punto)
if [[ $BUILD_TRAY == true ]]; then
    tray_flag=ON
    targets+=(punto-tray)
fi

printf 'Building %s %s for %s (tray=%s)\n' \
    "$PACKAGE_NAME" "$VERSION" "$ARCHITECTURE" "$tray_flag"
cmake -S cpp -B "$CPP_BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DBUILD_TESTING=OFF \
    -DBUILD_TRAY="$tray_flag" \
    "-DCMAKE_CXX_FLAGS=-ffile-prefix-map=${SCRIPT_DIR}=."
cmake --build "$CPP_BUILD_DIR" --target "${targets[@]}" -j "$(nproc)"

require_regular_input "$CPP_BUILD_DIR/punto"
if [[ $BUILD_TRAY == true ]]; then
    require_regular_input "$CPP_BUILD_DIR/punto-tray"
fi
file "$CPP_BUILD_DIR/punto" | grep -Fq ELF || fail 'invalid-build-output: punto'
if [[ $BUILD_TRAY == true ]]; then
    file "$CPP_BUILD_DIR/punto-tray" | grep -Fq ELF ||
        fail 'invalid-build-output: punto-tray'
fi

reset_directory "$STAGE_DIR"
install -d -m 0755 \
    "$STAGE_DIR/DEBIAN" \
    "$STAGE_DIR/etc/punto" \
    "$STAGE_DIR/usr/bin" \
    "$STAGE_DIR/usr/share/punto-switcher" \
    "$STAGE_DIR/usr/share/doc/punto-switcher/examples" \
    "$STAGE_DIR/usr/share/doc/punto-switcher"

install -m 0755 "$CPP_BUILD_DIR/punto" "$STAGE_DIR/usr/bin/punto-daemon"
install -m 0755 punto-cli.sh "$STAGE_DIR/usr/bin/punto"
strip --strip-unneeded "$STAGE_DIR/usr/bin/punto-daemon"
if [[ $BUILD_TRAY == true ]]; then
    install -d -m 0755 \
        "$STAGE_DIR/etc/xdg/autostart" \
        "$STAGE_DIR/usr/lib/systemd/user"
    install -m 0755 "$CPP_BUILD_DIR/punto-tray" "$STAGE_DIR/usr/bin/punto-tray"
    strip --strip-unneeded "$STAGE_DIR/usr/bin/punto-tray"
    install -m 0644 punto-tray.desktop \
        "$STAGE_DIR/etc/xdg/autostart/punto-tray.desktop"
    install -m 0644 punto-tray.service \
        "$STAGE_DIR/usr/lib/systemd/user/punto-tray.service"
fi

install -m 0644 VERSION "$STAGE_DIR/usr/share/punto-switcher/VERSION"
install -m 0644 udevmon.yaml \
    "$STAGE_DIR/usr/share/doc/punto-switcher/examples/udevmon.yaml"
install -m 0644 LICENSE "$STAGE_DIR/usr/share/doc/punto-switcher/copyright"

sed -E "s/^# Config version: .*/# Config version: ${VERSION}/" config.yaml \
    >"$STAGE_DIR/etc/punto/config.yaml"
[[ $(grep -Fxc "# Config version: $VERSION" \
    "$STAGE_DIR/etc/punto/config.yaml") -eq 1 ]] ||
    fail 'invalid-generated-config-version'
chmod 0644 "$STAGE_DIR/etc/punto/config.yaml"

sed -E \
    -e "s|(img[.]shields[.]io/badge/version-)[0-9]+([.][0-9]+){2}(-)|\\1${VERSION}\\3|" \
    -e "s|(punto-switcher_)[0-9]+([.][0-9]+){2}_[a-z0-9-]+([.]deb)|\\1${VERSION}_${ARCHITECTURE}\\3|" \
    README.md >"$STAGE_DIR/usr/share/doc/punto-switcher/README.md"
chmod 0644 "$STAGE_DIR/usr/share/doc/punto-switcher/README.md"

changelog_date=$(date -u -d "@$SOURCE_EPOCH" -R) || fail 'invalid-source-date-epoch'
{
    printf 'punto-switcher (%s) unstable; urgency=medium\n\n' "$VERSION"
    printf '  * Reproducible upstream release %s.\n\n' "$VERSION"
    printf ' -- Anton Shalin <anton.shalin@gmail.com>  %s\n' "$changelog_date"
} | gzip -n -9 >"$STAGE_DIR/usr/share/doc/punto-switcher/changelog.Debian.gz"
chmod 0644 "$STAGE_DIR/usr/share/doc/punto-switcher/changelog.Debian.gz"

shlib_work="$STAGE_DIR/.shlibdeps"
install -d -m 0755 "$shlib_work/debian"
{
    printf '%s\n' \
        'Source: punto-switcher' \
        'Section: utils' \
        'Priority: optional' \
        'Maintainer: Anton Shalin <anton.shalin@gmail.com>' \
        'Standards-Version: 4.6.2' \
        '' \
        'Package: punto-switcher' \
        'Architecture: any' \
        'Description: isolated dependency derivation fixture'
} >"$shlib_work/debian/control"
chmod 0644 "$shlib_work/debian/control"
elf_arguments=("-e$SCRIPT_DIR/$STAGE_DIR/usr/bin/punto-daemon")
if [[ $BUILD_TRAY == true ]]; then
    elf_arguments+=("-e$SCRIPT_DIR/$STAGE_DIR/usr/bin/punto-tray")
fi
derived_dependencies=$(cd "$shlib_work" && dpkg-shlibdeps -O "${elf_arguments[@]}") ||
    fail 'dependency-derivation-failed'
[[ $derived_dependencies == shlibs:Depends=* ]] || fail 'dependency-derivation-failed'
derived_dependencies=${derived_dependencies#shlibs:Depends=}

explicit_dependencies=(
    interception-tools hunspell-en-us hunspell-ru netcat-openbsd passwd
    'util-linux (>= 2.38)' 'systemd (>= 249.10)' \
    'init-system-helpers (>= 1.66)'
)
if grep -Eq '(^|[;&|()[:space:]])((/usr/bin/|/bin/)?sudo)([[:space:]]|$)' \
    punto-cli.sh; then
    explicit_dependencies+=(sudo)
fi
depends=$derived_dependencies
for dependency in "${explicit_dependencies[@]}"; do
    depends+=", $dependency"
done

sed \
    -e "s|@PUNTO_VERSION@|$VERSION|g" \
    -e "s|@PUNTO_ARCHITECTURE@|$ARCHITECTURE|g" \
    -e "s|@PUNTO_DEPENDS@|$depends|g" \
    DEBIAN/control >"$STAGE_DIR/DEBIAN/control"
sed "s|@PUNTO_VERSION@|$VERSION|g" DEBIAN/postinst \
    >"$STAGE_DIR/DEBIAN/postinst"
install -m 0755 DEBIAN/prerm "$STAGE_DIR/DEBIAN/prerm"
install -m 0755 DEBIAN/postrm "$STAGE_DIR/DEBIAN/postrm"
chmod 0644 "$STAGE_DIR/DEBIAN/control"
chmod 0755 "$STAGE_DIR/DEBIAN/postinst"

{
    printf '%s\n' /etc/punto/config.yaml
    if [[ $BUILD_TRAY == true ]]; then
        printf '%s\n' /etc/xdg/autostart/punto-tray.desktop
    else
        printf '%s\n' \
            'remove-on-upgrade /etc/xdg/autostart/punto-tray.desktop'
    fi
} | sort >"$STAGE_DIR/DEBIAN/conffiles"
chmod 0644 "$STAGE_DIR/DEBIAN/conffiles"

rm -rf -- "$shlib_work"
find "$STAGE_DIR" -xdev -type d -exec chmod 0755 {} +
find "$STAGE_DIR" -xdev -type f ! -path "$STAGE_DIR/usr/bin/*" \
    ! -path "$STAGE_DIR/DEBIAN/postinst" ! -path "$STAGE_DIR/DEBIAN/prerm" \
    ! -path "$STAGE_DIR/DEBIAN/postrm" \
    -exec chmod 0644 {} +
chmod 0755 "$STAGE_DIR/usr/bin/"* "$STAGE_DIR/DEBIAN/postinst" \
    "$STAGE_DIR/DEBIAN/prerm" "$STAGE_DIR/DEBIAN/postrm"

# Re-check output types immediately before replacing regular prior artifacts.
reject_unsafe_output "$OUTPUT_DEB"
reject_unsafe_output "$SHA_FILE"
[[ ! -e $OUTPUT_DEB || -f $OUTPUT_DEB ]] || fail "unsafe-path: $OUTPUT_DEB"
[[ ! -e $SHA_FILE || -f $SHA_FILE ]] || fail "unsafe-path: $SHA_FILE"
rm -f -- "$OUTPUT_DEB" "$SHA_FILE"

dpkg-deb --build --root-owner-group "$STAGE_DIR" "$OUTPUT_DEB" >/dev/null
sha256sum "$(basename "$OUTPUT_DEB")" >"$SHA_FILE"

printf 'Built %s\n' "$OUTPUT_DEB"
printf 'Checksum file: %s\n' "$SHA_FILE"
