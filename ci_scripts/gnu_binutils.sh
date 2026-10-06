# Sourced by the gates that read ELF files. It sets READELF and NM to the GNU
# binutils tools, whose options and output the gates parse. A Linux system
# has them as readelf and nm. FreeBSD's readelf and nm come from elftoolchain
# and take other options; there the binutils package installs the GNU tools
# under /usr/local/bin. A READELF or NM from the environment wins. The gate
# stops with a message when no GNU tool is found, because a gate that cannot
# read its artifact must fail and never pass.

ccol_find_gnu_tool() {
	# $1 is the tool name, $2 the value from the environment.
	if [ -n "$2" ]; then
		echo "$2"
		return 0
	fi
	for _cand in "$1" "/usr/local/bin/$1"; do
		if "$_cand" --version 2>/dev/null | head -n 1 | grep -q 'GNU'; then
			echo "$_cand"
			return 0
		fi
	done
	return 1
}

READELF=$(ccol_find_gnu_tool readelf "${READELF:-}") || {
	echo "$0: needs GNU readelf (on FreeBSD: pkg install binutils)" >&2
	exit 2
}
NM=$(ccol_find_gnu_tool nm "${NM:-}") || {
	echo "$0: needs GNU nm (on FreeBSD: pkg install binutils)" >&2
	exit 2
}
