#!/usr/bin/env bash
# FastFind installer — installs GTK4 deps and builds from source
set -e

COLOR_GREEN='\033[0;32m'
COLOR_CYAN='\033[0;36m'
COLOR_RED='\033[0;31m'
NC='\033[0m'

info()  { echo -e "${COLOR_CYAN}[FastFind]${NC} $*"; }
ok()    { echo -e "${COLOR_GREEN}[  OK  ]${NC} $*"; }
err()   { echo -e "${COLOR_RED}[ ERR ]${NC} $*"; exit 1; }

# ── Detect distro and install deps ──────────────────────────────
install_deps() {
    if command -v apt-get &>/dev/null; then
        info "Detected Debian/Ubuntu — installing dependencies…"
        sudo apt-get update -qq
        sudo apt-get install -y libgtk-4-dev meson ninja-build gcc pkg-config
    elif command -v dnf &>/dev/null; then
        info "Detected Fedora/RHEL — installing dependencies…"
        sudo dnf install -y gtk4-devel meson ninja-build gcc pkg-config
    elif command -v pacman &>/dev/null; then
        info "Detected Arch — installing dependencies…"
        sudo pacman -Sy --noconfirm gtk4 meson ninja gcc pkg-config
    elif command -v zypper &>/dev/null; then
        info "Detected openSUSE — installing dependencies…"
        sudo zypper install -y gtk4-devel meson ninja gcc pkg-config
    else
        err "Unsupported distro. Install libgtk-4-dev, meson, ninja-build manually."
    fi
}

# ── Build ────────────────────────────────────────────────────────
build() {
    info "Setting up build…"
    meson setup build --wipe 2>/dev/null || meson setup build
    info "Compiling…"
    ninja -C build
    ok "Build complete!"
}

# ── Install ──────────────────────────────────────────────────────
install_app() {
    info "Installing to /usr/local…"
    sudo ninja -C build install
    # Install desktop file
    sudo cp data/io.github.fastfind.desktop /usr/share/applications/ 2>/dev/null || true
    sudo update-desktop-database 2>/dev/null || true
    ok "FastFind installed! Run: fastfind"
}

# ── Main ─────────────────────────────────────────────────────────
case "${1:-all}" in
    deps)    install_deps ;;
    build)   build ;;
    install) build && install_app ;;
    all)
        install_deps
        build
        install_app
        ;;
    run)
        build
        info "Launching FastFind…"
        ./build/fastfind
        ;;
    *)
        echo "Usage: $0 [deps|build|install|run|all]"
        ;;
esac
