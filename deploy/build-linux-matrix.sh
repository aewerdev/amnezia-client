#!/usr/bin/env bash
set -Eeuo pipefail
IFS=$'\n\t'

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"

MODE="auto"
TARGETS_RAW="all"
GENERATORS_RAW="auto"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
BUILD_ROOT="${BUILD_ROOT:-$SOURCE_DIR/deploy/build/linux-matrix}"
ARTIFACT_DIR="${ARTIFACT_DIR:-$SOURCE_DIR/deploy/artifacts/linux}"
QT_VERSION="${QT_VERSION:-6.10.0}"
QT_ARCH="${QT_ARCH:-gcc_64}"
QT_ROOT_PATH="${QT_ROOT_PATH:-}"
QIF_ROOT_PATH="${QIF_ROOT_PATH:-}"
CONTAINER_RUNTIME="${CONTAINER_RUNTIME:-}"
INSTALL_QT="auto"
INSTALL_DEPS="auto"
FORCE=0
DRY_RUN=0
KEEP_GOING=0
JOBS="${JOBS:-}"
CMAKE_GENERATOR="${CMAKE_GENERATOR:-Ninja}"
AMNEZIA_CONAN_USE_REMOTE="${AMNEZIA_CONAN_USE_REMOTE:-ON}"

usage() {
    cat <<'EOF'
Usage: deploy/build-linux-matrix.sh [options]

Build AmneziaVPN Linux artifacts for multiple distro targets. This script does
not cross-compile glibc userspace binaries; for distro-specific targets it runs
the normal CMake/Conan/CPack build inside the matching host, container, or Nix
shell.

Targets:
  ubuntu        Build in/for Ubuntu
  debian, deb   Build in/for Debian
  arch          Build in/for Arch Linux
  nix           Build in a Nix shell
  all           ubuntu,debian,arch,nix

Options:
  --targets <list>             Comma-separated targets (default: all)
  --mode <auto|host|container> Execution mode (default: auto)
  --container-runtime <name>   docker or podman
  --generators <list|auto>     CPack generators. Defaults:
                               ubuntu/debian: DEB,TGZ
                               arch: TXZ,TGZ
                               nix: TGZ
  --build-type <type>          Release, Debug, RelWithDebInfo (default: Release)
  --build-root <path>          Build/cache root (default: deploy/build/linux-matrix)
  --artifacts <path>           Artifact output root (default: deploy/artifacts/linux)
  --qt-root <path>             Qt platform prefix, e.g. /opt/Qt/6.10.0/gcc_64
  --qif-root <path>            Qt Installer Framework root, for IFW generator
  --qt-version <version>       Qt version for aqt fallback (default: 6.10.0)
  --install-qt                 Install Qt/IFW with aqt if missing
  --no-install-qt              Never install Qt/IFW automatically
  --install-deps               Install distro build dependencies
  --no-install-deps            Do not install distro build dependencies
  --amnezia-remote             Use the Amnezia Conan binary remote (default)
  --no-amnezia-remote          Build missing Conan packages from source
  --force                      Remove the selected target build dir first
  --jobs <n>                   Parallel build jobs
  --keep-going                 Continue other targets after a target fails
  --dry-run                    Print commands without executing
  -h, --help                   Show this help

Examples:
  deploy/build-linux-matrix.sh --targets ubuntu,debian,arch --mode container
  deploy/build-linux-matrix.sh --targets deb --mode host --generators DEB
  deploy/build-linux-matrix.sh --targets nix --mode host

Useful environment:
  QT_INSTALL_DIR, QT_ROOT_PATH, QIF_ROOT_PATH, CMAKE_PREFIX_PATH, CONAN_HOME
  AMNEZIA_CONAN_USE_REMOTE=ON|OFF, NIX_FLAKE_REF=path:/repo#default
  NIX_SHELL_PACKAGES="nixpkgs#cmake nixpkgs#ninja ..."
EOF
}

log() {
    printf '\033[1;34m==>\033[0m %s\n' "$*" >&2
}

warn() {
    printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2
}

die() {
    printf '\033[1;31merror:\033[0m %s\n' "$*" >&2
    exit 1
}

run() {
    printf '\033[1;30m+\033[0m' >&2
    printf ' %q' "$@" >&2
    printf '\n' >&2
    if [[ "$DRY_RUN" -eq 0 ]]; then
        "$@"
    fi
}

split_csv() {
    local value="$1"
    printf '%s\n' "$value" | tr ',[:space:]' '\n' | awk 'NF'
}

normalize_target() {
    case "$1" in
        ubuntu|ubu) echo "ubuntu" ;;
        debian|deb|debu) echo "debian" ;;
        arch|archlinux) echo "arch" ;;
        nix|nixos) echo "nix" ;;
        all) echo "all" ;;
        *) die "Unknown target '$1'" ;;
    esac
}

target_image() {
    case "$1" in
        ubuntu) echo "${UBUNTU_IMAGE:-ubuntu:24.04}" ;;
        debian) echo "${DEBIAN_IMAGE:-debian:12}" ;;
        arch) echo "${ARCH_IMAGE:-archlinux:latest}" ;;
        *) return 1 ;;
    esac
}

current_distro_id() {
    if [[ -r /etc/os-release ]]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        echo "${ID:-linux}"
    else
        uname -s | tr '[:upper:]' '[:lower:]'
    fi
}

jobs() {
    if [[ -n "$JOBS" ]]; then
        echo "$JOBS"
    elif command -v nproc >/dev/null 2>&1; then
        nproc
    else
        echo 4
    fi
}

can_run_target_on_host() {
    local target="$1"
    local id
    id="$(current_distro_id)"
    case "$target" in
        ubuntu) [[ "$id" == "ubuntu" ]] ;;
        debian) [[ "$id" == "debian" ]] ;;
        arch) [[ "$id" == "arch" || "$id" == "archarm" ]] ;;
        nix) command -v nix >/dev/null 2>&1 ;;
        *) return 1 ;;
    esac
}

detect_container_runtime() {
    if [[ -n "$CONTAINER_RUNTIME" ]]; then
        command -v "$CONTAINER_RUNTIME" >/dev/null 2>&1 || die "Container runtime '$CONTAINER_RUNTIME' not found"
        echo "$CONTAINER_RUNTIME"
    elif command -v docker >/dev/null 2>&1; then
        echo "docker"
    elif command -v podman >/dev/null 2>&1; then
        echo "podman"
    else
        return 1
    fi
}

want_install_deps() {
    [[ "$INSTALL_DEPS" == "yes" ]] || [[ "$INSTALL_DEPS" == "auto" && "$(id -u)" == "0" ]]
}

want_install_qt() {
    [[ "$INSTALL_QT" == "yes" ]] || [[ "$INSTALL_QT" == "auto" ]]
}

ensure_command() {
    local cmd="$1"
    local hint="$2"
    command -v "$cmd" >/dev/null 2>&1 || die "Missing '$cmd'. $hint"
}

fix_container_ownership() {
    if [[ "$DRY_RUN" -eq 0 && "$(id -u)" == "0" && -n "${HOST_UID:-}" && -n "${HOST_GID:-}" ]]; then
        chown -R "$HOST_UID:$HOST_GID" "$BUILD_ROOT" "$ARTIFACT_DIR" 2>/dev/null || true
    fi
}

install_deps() {
    local target="$1"

    if ! want_install_deps; then
        ensure_command cmake "Install CMake or pass --install-deps."
        ensure_command git "Install Git or pass --install-deps."
        ensure_command python3 "Install Python 3 or pass --install-deps."
        return
    fi

    case "$target" in
        ubuntu|debian)
            export DEBIAN_FRONTEND=noninteractive
            run apt-get update
            run apt-get install -y --no-install-recommends \
                build-essential ca-certificates cmake curl file git ninja-build \
                patchelf pkg-config python3 python3-pip python3-venv \
                libdbus-1-dev libgl1-mesa-dev libx11-xcb-dev libxkbcommon-dev \
                libxcb-cursor-dev libxcb-icccm4-dev libxcb-image0-dev \
                libxcb-keysyms1-dev libxcb-render-util0-dev libxcb-shape0-dev \
                libxcb-xinerama0-dev libxcb-xkb-dev zstd xz-utils
            ;;
        arch)
            run pacman -Syu --noconfirm --needed \
                base-devel ca-certificates cmake curl dbus file git libglvnd \
                libx11 libxkbcommon ninja patchelf pkgconf python python-pip \
                python-virtualenv xcb-util-cursor xcb-util-image \
                xcb-util-keysyms xcb-util-renderutil xcb-util-wm xcb-util-xrm \
                zstd xz
            ;;
        nix)
            ensure_command nix "Install Nix first, or run another target."
            ;;
    esac
}

ensure_python_tooling() {
    local need_aqt="${1:-no}"
    local venv="$BUILD_ROOT/.tools/py"

    if command -v conan >/dev/null 2>&1 && { [[ "$need_aqt" != "yes" ]] || command -v aqt >/dev/null 2>&1; }; then
        return
    fi

    if [[ ! -x "$venv/bin/python" ]]; then
        run mkdir -p "$(dirname "$venv")"
        run python3 -m venv "$venv"
    fi

    # shellcheck disable=SC1091
    source "$venv/bin/activate"
    run python -m pip install --upgrade pip wheel

    if ! command -v conan >/dev/null 2>&1; then
        run python -m pip install --upgrade conan
    fi
    if [[ "$need_aqt" == "yes" ]] && ! command -v aqt >/dev/null 2>&1; then
        run python -m pip install --upgrade aqtinstall
    fi
}

qt_prefix_is_valid() {
    [[ -n "$1" && -f "$1/lib/cmake/Qt6/Qt6Config.cmake" ]]
}

find_qt_prefix() {
    local candidates=()

    [[ -n "${CMAKE_PREFIX_PATH:-}" ]] && candidates+=("${CMAKE_PREFIX_PATH%%:*}")
    [[ -n "$QT_ROOT_PATH" ]] && candidates+=("$QT_ROOT_PATH" "$QT_ROOT_PATH/$QT_ARCH")
    [[ -n "${QT_INSTALL_DIR:-}" ]] && candidates+=(
        "$QT_INSTALL_DIR/$QT_VERSION/$QT_ARCH"
        "$QT_INSTALL_DIR/Qt/$QT_VERSION/$QT_ARCH"
    )
    candidates+=(
        "$HOME/Qt/$QT_VERSION/$QT_ARCH"
        "/opt/Qt/$QT_VERSION/$QT_ARCH"
        "$BUILD_ROOT/.qt/$QT_VERSION/$QT_ARCH"
    )

    if command -v qtpaths6 >/dev/null 2>&1; then
        candidates+=("$(qtpaths6 --install-prefix 2>/dev/null || true)")
    fi

    local candidate
    for candidate in "${candidates[@]}"; do
        if qt_prefix_is_valid "$candidate"; then
            echo "$candidate"
            return 0
        fi
    done

    return 1
}

ensure_qt() {
    local prefix
    if prefix="$(find_qt_prefix)"; then
        echo "$prefix"
        return 0
    fi

    want_install_qt || die "Qt $QT_VERSION for $QT_ARCH not found. Pass --qt-root or --install-qt."

    ensure_python_tooling yes >&2
    run mkdir -p "$BUILD_ROOT/.qt" >&2
    run aqt install-qt linux desktop "$QT_VERSION" "$QT_ARCH" \
        -O "$BUILD_ROOT/.qt" \
        --modules qt5compat qtremoteobjects qtshadertools qtsvg qttools >&2

    prefix="$BUILD_ROOT/.qt/$QT_VERSION/$QT_ARCH"
    qt_prefix_is_valid "$prefix" || die "Qt installation completed but '$prefix' is not a valid Qt prefix"
    echo "$prefix"
}

cmake_prefix_with_qt() {
    local qt_prefix="$1"
    local current_prefix="${CMAKE_PREFIX_PATH:-}"

    if [[ -z "$current_prefix" ]]; then
        echo "$qt_prefix"
    elif [[ ":$current_prefix:" == *":$qt_prefix:"* ]]; then
        echo "$current_prefix"
    else
        echo "$qt_prefix:$current_prefix"
    fi
}

find_qif_root() {
    local candidates=()
    [[ -n "$QIF_ROOT_PATH" ]] && candidates+=("$QIF_ROOT_PATH")
    [[ -n "${QT_INSTALL_DIR:-}" ]] && candidates+=("$QT_INSTALL_DIR/Tools/QtInstallerFramework"/*)
    candidates+=(
        "$HOME/Qt/Tools/QtInstallerFramework"/*
        "/opt/Qt/Tools/QtInstallerFramework"/*
        "$BUILD_ROOT/.qt/Tools/QtInstallerFramework"/*
    )

    local candidate
    for candidate in "${candidates[@]}"; do
        if [[ -x "$candidate/bin/binarycreator" ]]; then
            echo "$candidate"
            return 0
        fi
    done

    return 1
}

ensure_qif() {
    local qif
    if qif="$(find_qif_root)"; then
        echo "$qif"
        return 0
    fi

    want_install_qt || die "Qt Installer Framework not found. Pass --qif-root or --install-qt."

    ensure_python_tooling yes >&2
    run mkdir -p "$BUILD_ROOT/.qt" >&2
    run aqt install-tool linux desktop tools_ifw qt.tools.ifw.47 -O "$BUILD_ROOT/.qt" >&2

    qif="$(find_qif_root)" || die "Qt Installer Framework installation completed, but binarycreator was not found"
    echo "$qif"
}

default_generators_for_target() {
    case "$1" in
        ubuntu|debian) echo "DEB TGZ" ;;
        arch) echo "TXZ TGZ" ;;
        nix) echo "TGZ" ;;
    esac
}

generators_for_target() {
    if [[ "$GENERATORS_RAW" == "auto" ]]; then
        default_generators_for_target "$1"
    else
        split_csv "$GENERATORS_RAW"
    fi
}

copy_artifacts() {
    local target="$1"
    local build_dir="$2"
    local out_dir="$ARTIFACT_DIR/$target"

    run mkdir -p "$out_dir"

    while IFS= read -r -d '' artifact; do
        run cp -f "$artifact" "$out_dir/"
    done < <(
        find "$build_dir" -maxdepth 1 -type f \
            \( -name 'AmneziaVPN_*' -o -name '*.deb' -o -name '*.rpm' -o -name '*.tar.*' -o -name '*.txz' \) \
            -print0
    )

    log "Artifacts for $target:"
    if [[ "$DRY_RUN" -eq 0 ]]; then
        find "$out_dir" -maxdepth 1 -type f -printf '  %p\n' 2>/dev/null || true
    fi
}

smoke_cli() {
    local build_dir="$1"
    local candidate
    for candidate in \
        "$build_dir/client/amnezia" \
        "$build_dir/client/vpn" \
        "$build_dir/client/AmneziaVPN-cli"; do
        if [[ -x "$candidate" ]]; then
            run "$candidate" --version
            return 0
        fi
    done
    warn "CLI smoke binary was not found under '$build_dir/client'"
}

build_host_target() {
    local target="$1"
    local qt_prefix
    local cmake_prefix
    local conan_force_build="${AMNEZIA_CONAN_FORCE_BUILD:-}"
    local platform_cmake_args=()
    local build_dir="$BUILD_ROOT/$target/$BUILD_TYPE"
    local conan_home="${CONAN_HOME:-$BUILD_ROOT/conan/$target}"
    local generator
    local qif_root=""

    log "Building target '$target' on host distro '$(current_distro_id)'"
    install_deps "$target"
    ensure_python_tooling no
    qt_prefix="$(ensure_qt)"
    cmake_prefix="$(cmake_prefix_with_qt "$qt_prefix")"

    # Conan Center build tools are generic Linux binaries. Rebuild native tools
    # with the Nix toolchain so their ELF interpreters point into the Nix store.
    if [[ "$target" == "nix" && -z "$conan_force_build" ]]; then
        conan_force_build="m4/*;ninja/*;pkgconf/*"
    fi
    if [[ "$target" == "nix" ]]; then
        platform_cmake_args+=(
            -DAMNEZIA_NIXOS_BUILD=ON
            -DQT_DEPLOY_FORCE_ADJUST_RPATHS=OFF
        )
    fi

    if [[ "$(id -u)" == "0" ]] && command -v git >/dev/null 2>&1; then
        git config --global --add safe.directory "$SOURCE_DIR" 2>/dev/null || true
    fi

    if [[ "$FORCE" -eq 1 ]]; then
        run rm -rf "$build_dir"
    fi

    run mkdir -p "$build_dir" "$conan_home"

    export CONAN_HOME="$conan_home"
    export CMAKE_PREFIX_PATH="$cmake_prefix"

    run cmake -S "$SOURCE_DIR" -B "$build_dir" \
        -G "$CMAKE_GENERATOR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_PREFIX_PATH="$cmake_prefix" \
        -DAMNEZIA_CONAN_USE_REMOTE="$AMNEZIA_CONAN_USE_REMOTE" \
        -DAMNEZIA_CONAN_FORCE_BUILD="$conan_force_build" \
        -DCONAN_INSTALL_BUILD_CONFIGURATIONS="$BUILD_TYPE" \
        "${platform_cmake_args[@]}"

    run cmake --build "$build_dir" --config "$BUILD_TYPE" --parallel "$(jobs)"
    smoke_cli "$build_dir"

    while IFS= read -r generator; do
        [[ -n "$generator" ]] || continue
        local cpack_args=(-G "$generator")

        if [[ "$generator" == "IFW" ]]; then
            qif_root="${qif_root:-$(ensure_qif)}"
            cpack_args+=(-D "QTIFWDIR=$qif_root")
        elif [[ "$generator" == "DEB" ]]; then
            cpack_args+=(
                -D "CPACK_DEBIAN_PACKAGE_MAINTAINER=${CPACK_DEBIAN_PACKAGE_MAINTAINER:-AmneziaVPN}"
                -D "CPACK_DEBIAN_PACKAGE_SECTION=net"
                -D "CPACK_DEBIAN_PACKAGE_SHLIBDEPS=OFF"
                -D "CPACK_DEBIAN_FILE_NAME=DEB-DEFAULT"
            )
        fi

        log "Packaging $target with CPack generator $generator"
        (cd "$build_dir" && run cpack "${cpack_args[@]}")
    done < <(generators_for_target "$target")

    copy_artifacts "$target" "$build_dir"

    fix_container_ownership
}

run_container_target() {
    local target="$1"
    local runtime image
    local extra_args=()

    [[ "$FORCE" -eq 1 ]] && extra_args+=(--force)
    [[ "$AMNEZIA_CONAN_USE_REMOTE" == "OFF" ]] && extra_args+=(--no-amnezia-remote)

    image="$(target_image "$target")" || die "Target '$target' does not have a container image"
    runtime="$(detect_container_runtime)" || die "Docker/Podman not found. Install one or use --mode host."

    log "Running $target build in $image via $runtime"
    run "$runtime" run --rm -t \
        -v "$SOURCE_DIR:/src" \
        -w /src \
        -e "HOST_UID=$(id -u)" \
        -e "HOST_GID=$(id -g)" \
        -e "QT_VERSION=$QT_VERSION" \
        -e "QT_ARCH=$QT_ARCH" \
        -e "BUILD_ROOT=/src/deploy/build/linux-matrix" \
        -e "ARTIFACT_DIR=/src/deploy/artifacts/linux" \
        "$image" \
        bash deploy/build-linux-matrix.sh \
            --mode host \
            --targets "$target" \
            --build-type "$BUILD_TYPE" \
            --generators "$GENERATORS_RAW" \
            --install-deps \
            --install-qt \
            --jobs "$(jobs)" \
            "${extra_args[@]}"
}

run_nix_target() {
    ensure_command nix "Install Nix or skip the nix target."

    local packages_string="${NIX_SHELL_PACKAGES:-nixpkgs#cmake nixpkgs#ninja nixpkgs#gcc nixpkgs#git nixpkgs#python3 nixpkgs#conan nixpkgs#pkg-config nixpkgs#patchelf nixpkgs#qt6.qtbase nixpkgs#qt6.qtdeclarative nixpkgs#qt6.qtsvg nixpkgs#qt6.qttools nixpkgs#qt6.qt5compat nixpkgs#qt6.qtremoteobjects nixpkgs#qt6.qtshadertools nixpkgs#qt6.qtimageformats nixpkgs#qt6.qtwayland}"
    local packages=()
    local extra_args=()
    local inner_args=()

    [[ "$FORCE" -eq 1 ]] && extra_args+=(--force)
    [[ "$AMNEZIA_CONAN_USE_REMOTE" == "OFF" ]] && extra_args+=(--no-amnezia-remote)

    inner_args=(
        --mode host
        --targets nix
        --build-type "$BUILD_TYPE"
        --generators "$GENERATORS_RAW"
        --no-install-deps
        --no-install-qt
        --jobs "$(jobs)"
        "${extra_args[@]}"
    )

    if [[ -f "$SOURCE_DIR/flake.nix" && -z "${NIX_SHELL_PACKAGES:-}" ]]; then
        local flake_ref="${NIX_FLAKE_REF:-path:$SOURCE_DIR#default}"
        log "Running nix target through nix develop ($flake_ref)"
        run nix --extra-experimental-features "nix-command flakes" develop "$flake_ref" --command \
            bash "$SOURCE_DIR/deploy/build-linux-matrix.sh" "${inner_args[@]}"
        return
    fi

    while IFS= read -r package; do
        [[ -n "$package" ]] && packages+=("$package")
    done < <(split_csv "$packages_string")

    log "Running nix target through nix shell"
    run nix --extra-experimental-features "nix-command flakes" shell "${packages[@]}" --command \
        bash "$SOURCE_DIR/deploy/build-linux-matrix.sh" "${inner_args[@]}"
}

run_target() {
    local target="$1"

    case "$MODE" in
        host)
            build_host_target "$target"
            ;;
        container)
            if [[ "$target" == "nix" ]]; then
                run_nix_target
            else
                run_container_target "$target"
            fi
            ;;
        auto)
            if can_run_target_on_host "$target"; then
                if [[ "$target" == "nix" ]]; then
                    run_nix_target
                else
                    build_host_target "$target"
                fi
            elif [[ "$target" == "nix" ]]; then
                run_nix_target
            else
                run_container_target "$target"
            fi
            ;;
        *)
            die "Unknown mode '$MODE'"
            ;;
    esac
}

parse_targets() {
    local raw="$1"
    local target normalized
    local result=()

    while IFS= read -r target; do
        normalized="$(normalize_target "$target")"
        if [[ "$normalized" == "all" ]]; then
            result+=(ubuntu debian arch nix)
        else
            result+=("$normalized")
        fi
    done < <(split_csv "$raw")

    printf '%s\n' "${result[@]}" | awk '!seen[$0]++'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --targets|-t) TARGETS_RAW="$2"; shift 2 ;;
        --mode) MODE="$2"; shift 2 ;;
        --container-runtime) CONTAINER_RUNTIME="$2"; shift 2 ;;
        --generators) GENERATORS_RAW="$2"; shift 2 ;;
        --build-type) BUILD_TYPE="$2"; shift 2 ;;
        --build-root) BUILD_ROOT="$2"; shift 2 ;;
        --artifacts) ARTIFACT_DIR="$2"; shift 2 ;;
        --qt-root) QT_ROOT_PATH="$2"; shift 2 ;;
        --qif-root) QIF_ROOT_PATH="$2"; shift 2 ;;
        --qt-version) QT_VERSION="$2"; shift 2 ;;
        --install-qt) INSTALL_QT="yes"; shift ;;
        --no-install-qt) INSTALL_QT="no"; shift ;;
        --install-deps) INSTALL_DEPS="yes"; shift ;;
        --no-install-deps) INSTALL_DEPS="no"; shift ;;
        --amnezia-remote) AMNEZIA_CONAN_USE_REMOTE="ON"; shift ;;
        --no-amnezia-remote) AMNEZIA_CONAN_USE_REMOTE="OFF"; shift ;;
        --force) FORCE=1; shift ;;
        --jobs|-j) JOBS="$2"; shift 2 ;;
        --keep-going) KEEP_GOING=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "Unknown argument '$1'. Use --help." ;;
    esac
done

case "${AMNEZIA_CONAN_USE_REMOTE^^}" in
    ON|TRUE|YES|1) AMNEZIA_CONAN_USE_REMOTE="ON" ;;
    OFF|FALSE|NO|0) AMNEZIA_CONAN_USE_REMOTE="OFF" ;;
    *) die "AMNEZIA_CONAN_USE_REMOTE must be ON or OFF" ;;
esac

mapfile -t TARGETS < <(parse_targets "$TARGETS_RAW")
[[ "${#TARGETS[@]}" -gt 0 ]] || die "No targets selected"

trap fix_container_ownership EXIT

status=0
for target in "${TARGETS[@]}"; do
    if [[ "$KEEP_GOING" -eq 1 ]]; then
        set +e
        (
            set -Eeuo pipefail
            run_target "$target"
        )
        target_status=$?
        set -e
        [[ "$target_status" -eq 0 ]] || status=1
    else
        run_target "$target"
    fi
done

exit "$status"
