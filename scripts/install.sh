#!/usr/bin/env bash
#
# Install chuart into $PREFIX so it can be run as plain `chuart` instead of
# ./build/chuart, together with the vendor library it needs.
#
#   scripts/install.sh [--prefix DIR] [--install-deps] [--with-cuse-rule[=GROUP]]
#   scripts/install.sh --uninstall [--prefix DIR]
#
# Building is done as the calling user; only the copy into $PREFIX uses sudo
# (and only when $PREFIX is not writable by the caller).
set -euo pipefail

PROG=$(basename "$0")
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$ROOT/build"

PREFIX=/usr/local
DO_UNINSTALL=0
DO_DEPS=0
CUSE_RULE=0
CUSE_GROUP=dialout
RULE_PATH=/etc/udev/rules.d/70-chuart-cuse.rules

usage() {
	cat <<EOF
usage: [sudo] $PROG [options]

Builds chuart and installs it into \$PREFIX/bin (with libCH9140lib.so in
\$PREFIX/lib), so that users can type

    chuart scan
    sudo chuart serve --mac <MAC> --owner \$USER --mode 0660

instead of spelling out the build directory each time.

options:
  --prefix <dir>          where to install (default $PREFIX)
  --install-deps          apt-get install the build dependencies first
  --with-cuse-rule[=GRP]  also install a udev rule letting group GRP (default
                          $CUSE_GROUP) open /dev/cuse, so \`serve\` no longer
                          needs root.  See the caveat below.
  --uninstall             remove a previous installation
  -h, --help              this text

notes:
  * \`serve\` needs the cuse kernel module; the installer tries \`modprobe cuse\`.
  * Caveat for --with-cuse-rule: /dev/cuse is root-only by default because any
    process that can open it may create character device nodes in /dev.  Only
    add this rule if you accept that for members of GRP.
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
	--prefix) PREFIX="${2:?--prefix needs a directory}"; shift ;;
	--prefix=*) PREFIX="${1#*=}" ;;
	--install-deps) DO_DEPS=1 ;;
	--with-cuse-rule) CUSE_RULE=1 ;;
	--with-cuse-rule=*) CUSE_RULE=1; CUSE_GROUP="${1#*=}" ;;
	--uninstall) DO_UNINSTALL=1 ;;
	-h|--help) usage; exit 0 ;;
	*) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
	esac
	shift
done

BIN_DEST="$PREFIX/bin/chuart"
LIB_DEST="$PREFIX/lib/libCH9140lib.so"

INVOKER="${SUDO_USER:-$(id -un)}"

# Privileged steps (copying into $PREFIX, udev) go through sudo; the build does
# not, so that a `sudo scripts/install.sh` does not leave root-owned files in
# the source tree.
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

# ---------------------------------------------------------------- uninstall ---
if [ "$DO_UNINSTALL" -eq 1 ]; then
	say "uninstalling from $PREFIX"
	for f in "$BIN_DEST" "$LIB_DEST"; do
		if [ -e "$f" ]; then
			"${SUDO[@]}" rm -f "$f"
			echo "  removed $f"
		fi
	done
	if [ -e "$RULE_PATH" ]; then
		"${SUDO[@]}" rm -f "$RULE_PATH"
		echo "  removed $RULE_PATH"
		if command -v udevadm >/dev/null 2>&1; then
			"${SUDO[@]}" udevadm control --reload >/dev/null 2>&1 || true
			"${SUDO[@]}" udevadm trigger --name-match=cuse >/dev/null 2>&1 || true
		fi
		# udev only applies the modes it is told about, so dropping the rule
		# leaves the existing node as the rule made it.  Put the kernel's
		# default (0600, root) back by hand.
		if [ -e /dev/cuse ]; then
			"${SUDO[@]}" chown root:root /dev/cuse 2>/dev/null || true
			"${SUDO[@]}" chmod 0600 /dev/cuse 2>/dev/null || true
		fi
	fi
	if [ -d "$PREFIX/lib" ] && command -v ldconfig >/dev/null 2>&1 &&
		[ "${PREFIX#/usr}" != "$PREFIX" ]; then
		"${SUDO[@]}" ldconfig
	fi
	say "done (the build directory was left in place: $BUILD_DIR)"
	exit 0
fi

# -------------------------------------------------------------- dependencies ---
missing=()
command -v cmake >/dev/null 2>&1 || missing+=(cmake)
command -v cc >/dev/null 2>&1 || missing+=(a C compiler)
command -v pkg-config >/dev/null 2>&1 || missing+=(pkg-config)
if command -v pkg-config >/dev/null 2>&1; then
	pkg-config --exists fuse3 || missing+=(libfuse3-dev)
	pkg-config --exists glib-2.0 gio-2.0 gobject-2.0 || missing+=(libglib2.0-dev)
	[ -f /usr/include/bluetooth/bluetooth.h ] || missing+=(libbluetooth-dev)
fi

if [ "${#missing[@]}" -gt 0 ]; then
	if [ "$DO_DEPS" -eq 1 ]; then
		command -v apt-get >/dev/null 2>&1 ||
			die "--install-deps only knows about apt-get; install manually: ${missing[*]}"
		say "installing build dependencies"
		"${SUDO[@]}" apt-get update -qq
		"${SUDO[@]}" apt-get install -y build-essential cmake pkg-config \
			libfuse3-dev libglib2.0-dev libbluetooth-dev
	elif [ "$DO_UNINSTALL" -eq 0 ]; then
		die "missing: ${missing[*]} (re-run with --install-deps, or install them yourself)"
	fi
fi

# --------------------------------------------------------------------- build ---
say "building in $BUILD_DIR"
# A build tree created by an earlier root run would block the user's build.
if [ "${#BUILD_AS[@]}" -gt 0 ] && [ -e "$BUILD_DIR" ] &&
	[ "$(stat -c %U "$BUILD_DIR" 2>/dev/null || echo "$INVOKER")" != "$INVOKER" ]; then
	"${SUDO[@]}" chown -R "$INVOKER" "$BUILD_DIR"
fi
"${BUILD_AS[@]}" cmake -S "$ROOT" -B "$BUILD_DIR" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
"${BUILD_AS[@]}" cmake --build "$BUILD_DIR" --parallel >/dev/null

# ------------------------------------------------------------------- install ---
say "installing into $PREFIX"
"${SUDO[@]}" cmake --install "$BUILD_DIR" --prefix "$PREFIX" >/dev/null

# /usr/local/lib is a default search path, but refreshing the cache makes the
# library available even to processes started before the install.
case "$PREFIX" in
/usr|/usr/local)
	command -v ldconfig >/dev/null 2>&1 && "${SUDO[@]}" ldconfig
	;;
esac

# ------------------------------------------------------------------- cuse ---
if [ "$CUSE_RULE" -eq 1 ]; then
	say "installing udev rule for /dev/cuse (group $CUSE_GROUP)"
	printf '%s\n' \
		'# Allow a group to create CUSE character devices (chuart serve).' \
		'# Any process able to open /dev/cuse can create device nodes in /dev,' \
		'# so only add this for a group you trust.' \
		"KERNEL==\"cuse\", SUBSYSTEM==\"misc\", MODE=\"0660\", GROUP=\"$CUSE_GROUP\"" |
		"${SUDO[@]}" tee "$RULE_PATH" >/dev/null
	"${SUDO[@]}" udevadm control --reload
	"${SUDO[@]}" udevadm trigger --name-match=cuse 2>/dev/null || true
	if ! getent group "$CUSE_GROUP" >/dev/null; then
		warn "group '$CUSE_GROUP' does not exist; adjust $RULE_PATH"
	elif ! id -nG | tr ' ' '\n' | grep -qx "$CUSE_GROUP"; then
		warn "you are not in group '$CUSE_GROUP' yet: sudo usermod -aG $CUSE_GROUP $INVOKER  (then re-login)"
	fi
fi

# -------------------------------------------------------------- sanity check ---
case ":$PATH:" in
*":$PREFIX/bin:"*) ;;
*) warn "$PREFIX/bin is not in your PATH; add it to use plain \`chuart\`" ;;
esac

"$BIN_DEST" help >/dev/null 2>&1 || die "the installed binary does not run: $BIN_DEST"
say "installed $("$BIN_DEST" version 2>/dev/null | head -1 || echo chuart)"
echo "  binary: $BIN_DEST"
echo "  library: $LIB_DEST"
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
