# Sourced by the gates that read ELF files. It sets READELF and NM to the GNU
# binutils tools, since the gates parse their options and output. On Linux
# that's just readelf and nm, but FreeBSD's own readelf and nm come from
# elftoolchain and take other options, so there we use the GNU tools that the
# binutils package puts under /usr/local/bin. A READELF or NM from the
# environment wins. If no GNU tool is found the gate stops with a message,
# because a gate that can't read its artifact has to fail, never pass.

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
