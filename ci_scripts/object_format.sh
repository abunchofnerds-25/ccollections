# Sourced by the gates that read the built library. It tells an ELF file from a
# Mach-O one by looking at the first four bytes of the file, never at the system
# that runs the gate, since a cross-build makes a library for another system.
# For Mach-O it also lists the defined external symbols, because the gates read
# ELF with GNU readelf and nm and those can't read Mach-O.

# Prints "elf" or "macho" for the file $1, and fails for anything else.
ccol_object_format() {
	_magic=$(od -An -tx1 -N4 "$1" 2>/dev/null | tr -d ' \n') || return 1
	case "$_magic" in
	7f454c46) echo elf ;;
	# A 64-bit or 32-bit Mach-O file in either byte order, or a universal
	# file holding several architectures.
	cffaedfe | cefaedfe | feedfacf | feedface | cafebabe) echo macho ;;
	*) return 1 ;;
	esac
}

# Prints the C names of the defined external symbols of the Mach-O dylib or
# archive $1, sorted, one per line. It uses the system's nm (on macOS that's the
# LLVM nm of the developer tools). In an archive the list also has the private
# external symbols that hidden visibility makes, since a static link still
# resolves those between the objects of one program. A linked dylib turns them
# into local symbols, so there the list is exactly the exported set. Mach-O puts
# an underscore in front of every C name in the symbol table and we strip it.
# __mh_dylib_header is the header symbol the linker defines in every dylib, so
# it's left out. We check nm's status separately, so a file that nm can't read
# fails instead of giving an empty list.
ccol_macho_defined_globals() {
	_raw=$("${NM:-nm}" -gUj "$1") || return 1
	printf '%s\n' "$_raw" \
		| awk 'NF == 1 && $1 !~ /:$/ && $1 != "__mh_dylib_header"' \
		| sed 's/^_//' | sort -u
}
