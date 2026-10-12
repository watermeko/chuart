#!/usr/bin/env bash
#
# Install chuart into $PREFIX so it can be run as plain `chuart` instead of
# ./build/chuart, together with the vendor library it needs.
#
#   scripts/install.sh [--prefix DIR] [--install-deps] [--with-cuse-rule[=GROUP]]
#   scripts/install.sh --check
#   scripts/install.sh --uninstall [--prefix DIR]
#
# Works on any glibc/x86-64 Linux distribution: the package manager is detected
# for --install-deps, and the library directory (lib, lib64, lib/<multiarch>)
# comes from CMake's GNUInstallDirs.  See `--check` for the platform verdict.
#
# Building is done as the calling user; only the copy into $PREFIX uses sudo
# (and only when $PREFIX is not writable by the caller).
set -euo pipefail

PROG=$(basename "$0")
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$ROOT/build"
VENDOR_LIB="$ROOT/src/lib/libCH9140lib.so"

PREFIX=/usr/local
DO_UNINSTALL=0
DO_DEPS=0
DO_CHECK=0
CUSE_RULE=0
CUSE_GROUP=dialout
RULE_DIR=/etc/udev/rules.d
RULE_NAME=70-chuart-cuse.rules

usage() {
	cat <<EOF
usage: [sudo] $PROG [options]

Builds chuart and installs it into \$PREFIX/bin (with libCH9140lib.so in the
distribution's library directory), so that users can type

    chuart scan
    sudo chuart serve --mac <MAC> --owner \$USER --mode 0660

instead of spelling out the build directory each time.

options:
  --prefix <dir>          where to install (default $PREFIX)
  --check                 report whether this distribution can run chuart,
                          then exit without changing anything
  --install-deps          install the build dependencies with the detected
                          package manager
  --with-cuse-rule[=GRP]  also install a udev rule letting group GRP (default
                          $CUSE_GROUP) open /dev/cuse, so \`serve\` no longer
                          needs root.  See the caveat below.
  --uninstall             remove a previous installation
  -h, --help              this text

environment:
  CMAKE_ARGS              extra arguments for the cmake configure step, e.g.
                          CMAKE_ARGS="-DCMAKE_INSTALL_LIBDIR=lib64"

notes:
  * \`serve\` needs a kernel with FUSE/CUSE and BlueZ 5 talking over D-Bus.
  * Caveat for --with-cuse-rule: /dev/cuse is root-only by default because any
    process that can open it may create character device nodes in /dev.  Only
    add this rule if you accept that for members of GRP.
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
	--prefix) PREFIX="${2:?--prefix needs a directory}"; shift ;;
	--prefix=*) PREFIX="${1#*=}" ;;
	--check) DO_CHECK=1 ;;
	--install-deps) DO_DEPS=1 ;;
	--with-cuse-rule) CUSE_RULE=1 ;;
	--with-cuse-rule=*) CUSE_RULE=1; CUSE_GROUP="${1#*=}" ;;
	--uninstall) DO_UNINSTALL=1 ;;
	-h|--help) usage; exit 0 ;;
	*) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
	esac
	shift
done

INVOKER="${SUDO_USER:-$(id -un)}"

# Privileged steps (copying into $PREFIX, udev) go through sudo; the build does
# not, so that `sudo scripts/install.sh` leaves no root-owned files behind.
if [ "$(id -u)" -eq 0 ]; then
	SUDO=()
	if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
		BUILD_AS=(sudo -u "$SUDO_USER" --)
	else
		BUILD_AS=()
	fi
elif command -v sudo >/dev/null 2>&1; then
	SUDO=(sudo)
	BUILD_AS=()
else
	SUDO=()
	BUILD_AS=()
fi

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ------------------------------------------------------------- platform facts --
distro_pretty() {
	if [ -r /etc/os-release ]; then
		. /etc/os-release
		echo "${PRETTY_NAME:-${NAME:-unknown}}"
	else
		echo "unknown distribution"
	fi
}

# Which package manager, and which packages provide our build dependencies.
detect_packages() {
	PKG_INSTALL=""
	PKG_PACKAGES=""
	if command -v apt-get >/dev/null 2>&1; then
		PKG_INSTALL="apt-get install -y"
		PKG_PACKAGES="build-essential cmake pkg-config libfuse3-dev libglib2.0-dev libbluetooth-dev"
	elif command -v dnf >/dev/null 2>&1; then
		PKG_INSTALL="dnf install -y"
		PKG_PACKAGES="gcc make cmake pkgconf-pkg-config fuse3-devel glib2-devel bluez-libs-devel"
	elif command -v yum >/dev/null 2>&1; then
		PKG_INSTALL="yum install -y"
		PKG_PACKAGES="gcc make cmake pkgconfig fuse3-devel glib2-devel bluez-libs-devel"
	elif command -v zypper >/dev/null 2>&1; then
		PKG_INSTALL="zypper --non-interactive install"
		PKG_PACKAGES="gcc make cmake pkg-config fuse3-devel glib2-devel bluez-devel"
	elif command -v pacman >/dev/null 2>&1; then
		PKG_INSTALL="pacman -S --needed --noconfirm"
		PKG_PACKAGES="base-devel cmake pkgconf fuse3 glib2 bluez-libs"
	elif command -v apk >/dev/null 2>&1; then
		PKG_INSTALL="apk add --no-cache"
		PKG_PACKAGES="build-base cmake pkgconf fuse3-dev glib-dev bluez-dev"
	fi
}

libc_kind() {
	if [ -e /lib/ld-musl-"$(uname -m)".so.1 ] || [ -e /lib/ld-musl-x86_64.so.1 ]; then
		echo musl
	elif ldd /bin/sh 2>/dev/null | grep -q 'libc\.so\.6'; then
		echo glibc
	else
		echo unknown
	fi
}

# The bundled library is a prebuilt vendor binary: x86-64 and glibc only.
check_vendor_lib() {
	local problems=0
	case "$(uname -m)" in
	x86_64|amd64) ;;
	*)
		warn "libCH9140lib.so is built for x86-64 but this machine is $(uname -m); chuart will build, but the vendor library cannot be loaded. Ask WCH for a $(uname -m) build."
		problems=1
		;;
	esac
	if [ "$(libc_kind)" = musl ]; then
		warn "this looks like a musl system (Alpine?); libCH9140lib.so is linked against glibc and will not load"
		problems=1
	fi
	return $problems
}

missing_build_deps() {
	local missing=""
	command -v cmake >/dev/null 2>&1 || missing+="cmake "
	command -v cc >/dev/null 2>&1 || missing+="cc "
	if command -v pkg-config >/dev/null 2>&1; then
		pkg-config --exists fuse3 2>/dev/null || missing+="libfuse3-dev "
		pkg-config --exists glib-2.0 gio-2.0 gobject-2.0 2>/dev/null ||
			missing+="libglib2.0-dev "
		[ -f /usr/include/bluetooth/bluetooth.h ] ||
			missing+="libbluetooth-dev "
	elif ! command -v pkgconf >/dev/null 2>&1; then
		missing+="pkg-config "
	fi
	echo "$missing"
}

runtime_report() {
	local ok=1
	if [ ! -e /dev/cuse ] && ! modinfo cuse >/dev/null 2>&1; then
		warn "no /dev/cuse and no cuse kernel module: 'chuart serve' cannot create the device node"
		ok=0
	fi
	if command -v ldconfig >/dev/null 2>&1; then
		ldconfig -p 2>/dev/null | grep -q 'libbluetooth\.so\.3' ||
			{ warn "libbluetooth.so.3 not found: install the BlueZ libraries (bluez-libs / libbluetooth3)"; ok=0; }
	fi
	if [ -e "$VENDOR_LIB" ]; then
		local notfound
		notfound=$(ldd "$VENDOR_LIB" 2>/dev/null | grep 'not found' || true)
		[ -n "$notfound" ] && { warn "the vendor library has unmet dependencies:"; echo "$notfound" >&2; ok=0; }
	fi
	command -v bluetoothd >/dev/null 2>&1 ||
		{ warn "bluetoothd not found: install BlueZ (bluez / bluez-utils)"; ok=0; }
	return $((1 - ok))
}

# --------------------------------------------------------------- uninstall ---
find_installed_lib() {
	# lib, lib64 or lib/<multiarch>: whatever GNUInstallDirs chose.
	find "$PREFIX" -maxdepth 3 -name libCH9140lib.so -print -quit 2>/dev/null
}

if [ "$DO_UNINSTALL" -eq 1 ]; then
	say "uninstalling from $PREFIX"
	for f in "$PREFIX/bin/chuart" "$(find_installed_lib)"; do
		[ -n "$f" ] && [ -e "$f" ] || continue
		"${SUDO[@]}" rm -f "$f"
		echo "  removed $f"
	done
	rule="$RULE_DIR/$RULE_NAME"
	if [ -e "$rule" ]; then
		"${SUDO[@]}" rm -f "$rule"
		echo "  removed $rule"
		if command -v udevadm >/dev/null 2>&1; then
			"${SUDO[@]}" udevadm control --reload >/dev/null 2>&1 || true
			"${SUDO[@]}" udevadm trigger --name-match=cuse >/dev/null 2>&1 || true
		fi
		# udev only applies the modes it is told about, so dropping the rule
		# leaves the existing node as the rule made it.  Restore the kernel
		# default (0600, root) by hand.
		if [ -e /dev/cuse ]; then
			"${SUDO[@]}" chown root:root /dev/cuse 2>/dev/null || true
			"${SUDO[@]}" chmod 0600 /dev/cuse 2>/dev/null || true
		fi
	fi
	command -v ldconfig >/dev/null 2>&1 && "${SUDO[@]}" ldconfig 2>/dev/null || true
	say "done (the build directory was left in place: $BUILD_DIR)"
	exit 0
fi

# ------------------------------------------------------------------- check ---
detect_packages

if [ "$DO_CHECK" -eq 1 ]; then
	echo "distribution : $(distro_pretty)"
	echo "architecture : $(uname -m)"
	echo "libc         : $(libc_kind)"
	echo "kernel       : $(uname -r)"
	echo "package mgr  : ${PKG_INSTALL%% *}"
	[ -n "$PKG_PACKAGES" ] &&
		echo "build deps   : $PKG_PACKAGES" ||
		echo "build deps   : unknown package manager; install cmake, a C compiler, pkg-config, fuse3, glib2 and bluez development files"
	missing=$(missing_build_deps)
	[ -n "$missing" ] &&
		echo "missing now  : $missing" ||
		echo "missing now  : (none)"
	installed=$(command -v chuart || true)
	echo "installed    : ${installed:-(not in PATH)}"
	echo
	echo "platform verdict:"
	check_vendor_lib || true
	echo "  FUSE/CUSE: $([ -e /dev/cuse ] && echo "/dev/cuse present" || echo "no /dev/cuse (modprobe cuse; needs root to open)")"
	echo "  udev     : $([ -d "$RULE_DIR" ] && echo "$RULE_DIR" || echo "absent, --with-cuse-rule would be skipped")"
	echo "  bluez    : $(command -v bluetoothd >/dev/null 2>&1 && echo "bluetoothd present" || echo "bluetoothd missing (install bluez)")"
	runtime_report || true
	echo
	echo "note: libCH9140lib.so is a prebuilt WCH binary (x86-64, glibc)."
	echo "      It does not exist for musl, ARM or RISC-V, so those platforms"
	echo "      cannot run chuart even though it builds."
	exit 0
fi

# -------------------------------------------------------------- dependencies ---
missing=$(missing_build_deps)
if [ -n "$missing" ]; then
	if [ "$DO_DEPS" -eq 1 ]; then
		[ -n "$PKG_INSTALL" ] ||
			die "no supported package manager found; install these yourself: $missing"
		say "installing build dependencies ($PKG_INSTALL)"
		${SUDO[@]+"${SUDO[@]}"} ${PKG_INSTALL} ${PKG_PACKAGES}
	else
		die "missing: $missing (re-run with --install-deps, or install them yourself; \`$PROG --check\` explains more)"
	fi
fi
check_vendor_lib || warn "continuing anyway, but chuart will not be able to load the vendor library"

# --------------------------------------------------------------------- build ---
say "building in $BUILD_DIR"
# A build tree created by an earlier root run would block the user's build.
if [ "${#BUILD_AS[@]}" -gt 0 ] && [ -e "$BUILD_DIR" ] &&
	[ -n "$(find "$BUILD_DIR" -maxdepth 0 ! -user "$INVOKER" 2>/dev/null)" ]; then
	"${SUDO[@]}" chown -R "$INVOKER" "$BUILD_DIR"
fi
# shellcheck disable=SC2086  # CMAKE_ARGS is intentionally split
"${BUILD_AS[@]+"${BUILD_AS[@]}"}" cmake -S "$ROOT" -B "$BUILD_DIR" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo ${CMAKE_ARGS:-} >/dev/null
"${BUILD_AS[@]+"${BUILD_AS[@]}"}" cmake --build "$BUILD_DIR" --parallel >/dev/null

# ------------------------------------------------------------------- install ---
say "installing into $PREFIX"
"${SUDO[@]+"${SUDO[@]}"}" cmake --install "$BUILD_DIR" --prefix "$PREFIX" >/dev/null

LIB_DEST=$(find_installed_lib)
BIN_DEST="$PREFIX/bin/chuart"

# Refresh the loader cache; harmless on distributions that never need it.
command -v ldconfig >/dev/null 2>&1 && "${SUDO[@]+"${SUDO[@]}"}" ldconfig 2>/dev/null || true

# -------------------------------------------------------------------- cuse ---
if [ "$CUSE_RULE" -eq 1 ]; then
	if [ -d "$RULE_DIR" ] && command -v udevadm >/dev/null 2>&1; then
		say "installing udev rule for /dev/cuse (group $CUSE_GROUP)"
		printf '%s\n' \
			'# Allow a group to create CUSE character devices (chuart serve).' \
			'# Any process able to open /dev/cuse can create device nodes in /dev,' \
			'# so only add this for a group you trust.' \
			"KERNEL==\"cuse\", SUBSYSTEM==\"misc\", MODE=\"0660\", GROUP=\"$CUSE_GROUP\"" |
			"${SUDO[@]+"${SUDO[@]}"}" tee "$RULE_DIR/$RULE_NAME" >/dev/null
		"${SUDO[@]+"${SUDO[@]}"}" udevadm control --reload
		"${SUDO[@]+"${SUDO[@]}"}" udevadm trigger --name-match=cuse 2>/dev/null || true
		if ! { getent group "$CUSE_GROUP" >/dev/null 2>&1 || grep -q "^$CUSE_GROUP:" /etc/group 2>/dev/null; }; then
			warn "group '$CUSE_GROUP' does not exist; adjust $RULE_DIR/$RULE_NAME"
		elif ! id -Gn 2>/dev/null | tr ' ' '\n' | grep -qx "$CUSE_GROUP"; then
			warn "you are not in group '$CUSE_GROUP' yet: sudo usermod -aG $CUSE_GROUP $INVOKER  (then re-login)"
		fi
	else
		warn "--with-cuse-rule requested, but this system has no udev; \`serve\` keeps needing root"
	fi
fi

# -------------------------------------------------------------- sanity check ---
"$BIN_DEST" help >/dev/null 2>&1 || die "the installed binary does not run: $BIN_DEST"
notfound=$(ldd "$BIN_DEST" 2>/dev/null | grep 'not found' || true)
[ -n "$notfound" ] && warn "unresolved libraries after install:" && echo "$notfound" >&2

case ":$PATH:" in
*":$PREFIX/bin:"*) ;;
*) warn "$PREFIX/bin is not in your PATH; add it to use plain \`chuart\`" ;;
esac

say "installed $("$BIN_DEST" version 2>/dev/null | head -1 || echo chuart)"
echo "  binary:  $BIN_DEST"
echo "  library: ${LIB_DEST:-$PREFIX/<libdir>/libCH9140lib.so}"
echo
echo "next:"
echo "  chuart scan"
echo "  sudo chuart serve --mac <MAC> --owner \$USER --mode 0660"
if [ "$CUSE_RULE" -eq 0 ]; then
	echo
	echo "note: creating the device node needs root; re-run with"
	echo "      --with-cuse-rule to let group $CUSE_GROUP do it instead"
	echo "      (read the caveat in --help first)."
fi
