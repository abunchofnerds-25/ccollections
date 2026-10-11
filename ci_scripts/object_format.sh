# Sourced by the gates that read the built library. It tells an ELF file from
# a Mach-O file by the first four bytes of the file itself, and never by the
# system that runs the gate: a cross-build makes a library for another system.
# For Mach-O it also lists the defined external symbols, because the gates read
# ELF with GNU readelf and nm, and those tools do not read Mach-O.

# Gives "elf" or "macho" for the file $1, or fails for any other file.
ccol_object_format() {
	_magic=$(od -An -tx1 -N4 "$1" 2>/dev/null | tr -d ' \n') || return 1
	case "$_magic" in
	7f454c46) echo elf ;;
	# A 64-bit and a 32-bit Mach-O file, in either byte order, and a
	# universal file that holds several architectures.
	cffaedfe | cefaedfe | feedfacf | feedface | cafebabe) echo macho ;;
	*) return 1 ;;
	esac
}

# Lists, one per line and sorted, the C names of the defined external
# symbols of the Mach-O dylib or archive $1. It reads them with the nm of the
# system (on macOS the LLVM nm of the developer tools). In an archive these
# include the private external symbols, which hidden visibility gives: a
# static link still resolves those across the objects of one program. In a
# linked dylib a private external symbol becomes local, so the list is the
# exported set. The symbol table of Mach-O adds one underscore in front of
# every C name, and this removes it. __mh_dylib_header is the header symbol
# that the linker defines in every dylib, and it is left out. The status of
# nm is checked on its own, so a file that nm cannot read fails and never
# gives an empty list.
ccol_macho_defined_globals() {
	_raw=$("${NM:-nm}" -gUj "$1") || return 1
	printf '%s\n' "$_raw" \
		| awk 'NF == 1 && $1 !~ /:$/ && $1 != "__mh_dylib_header"' \
		| sed 's/^_//' | sort -u
}
