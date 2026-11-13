# shellcheck shell=bash
# Zip downloads that are checked before they are trusted (sourced by fetch_opencore.sh).

# kext_in_zip FILE NAMES NAME: the zip holds NAME.kext with a readable Contents/Info.plist and the executable that plist names
# (Contents/MacOS/<CFBundleExecutable>). NAMES is the zip's entry list. The outermost NAME.kext counts (not one inside a PlugIns folder).
kext_in_zip() {
    local f="$1" names="$2" name="$3" plist prefix exe
    plist="$(grep -E -- "(^|/)${name}\.kext/Contents/Info\.plist$" <<<"$names" | awk '{ print length, $0 }' | sort -n | head -1 | cut -d' ' -f2-)"
    [ -n "$plist" ] || return 1
    prefix="${plist%/Contents/Info.plist}"
    exe="$(unzip -p "$f" "$plist" 2>/dev/null | python3 -I -c 'import plistlib, sys
try:
    print(plistlib.loads(sys.stdin.buffer.read()).get("CFBundleExecutable", ""))
except Exception:
    pass')"
    [ -n "$exe" ] || return 1
    grep -qxF -- "$prefix/Contents/MacOS/$exe" <<<"$names"
}

# zip_ok FILE CHECK...: the archive passes unzip's integrity test, has no entry with an absolute path or a .. part (it would land
# outside the folder it is extracted into), and passes every CHECK: "kext:NAME" = a complete NAME.kext (kext_in_zip), anything else =
# a regex at least one entry name must match
zip_ok() {
    local f="$1"; shift
    [ -s "$f" ] || return 1
    unzip -tqq "$f" >/dev/null 2>&1 || return 1
    local names c
    names="$(unzip -Z1 "$f" 2>/dev/null)" || return 1
    # (a backslash too: unzip may turn it into a path separator)
    local bad=0
    grep -qE -- '^/|(^|/)\.\.(/|$)|[\]' <<<"$names" || bad=$?
    [ "$bad" -eq 1 ] || return 1                       # 0 = an unsafe name was found, 2 = grep itself failed: reject both
    for c in "$@"; do
        case "$c" in
            kext:*) kext_in_zip "$f" "$names" "${c#kext:}" || return 1;;
            *) grep -qE -- "$c" <<<"$names" || return 1;;
        esac
    done
}

# fetch_zip URL OUT CHECK...: keep OUT only if zip_ok passes with those CHECKs. A cached OUT is checked too and fetched
# again when it fails. The download goes to OUT.part and is moved into place only after the check.
fetch_zip() {
    local url="$1" out="$2"; shift 2
    if [ -f "$out" ]; then
        if zip_ok "$out" "$@"; then echo "  [✓] $(basename "$out") ya descargado (verificado)"; return 0; fi
        echo "  [!] $(basename "$out") en caché está dañado o incompleto: se descarga otra vez"
        rm -f "$out"
    fi
    rm -f "$out.part"
    if ! curl -fsSL -o "$out.part" "$url"; then
        rm -f "$out.part"; echo "  [!] fallo al descargar $url" >&2; return 1
    fi
    if ! zip_ok "$out.part" "$@"; then
        rm -f "$out.part"; echo "  [!] $(basename "$out"): el zip descargado está dañado o no contiene $*" >&2; return 1
    fi
    mv "$out.part" "$out"
    echo "  [✓] $(basename "$out") ($(du -h "$out" | cut -f1))"
}
