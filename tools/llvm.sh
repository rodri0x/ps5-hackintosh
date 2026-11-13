# shellcheck shell=bash
# llvm_bin: the directory with clang, ld.lld and llvm-objcopy (same rules as tools/toolpaths.py): LLVM_BIN if set, else Homebrew llvm@18
# on Apple Silicon, then on Intel. Prints an error and fails when none has them.
llvm_bin() {
    local d cands
    if [ -n "${LLVM_BIN:-}" ]; then cands=("$LLVM_BIN"); else cands=(/opt/homebrew/opt/llvm@18/bin /usr/local/opt/llvm@18/bin); fi
    for d in "${cands[@]}"; do
        if [ -x "$d/clang" ] && [ -x "$d/ld.lld" ] && [ -x "$d/llvm-objcopy" ]; then echo "$d"; return 0; fi
    done
    echo "no LLVM (clang, ld.lld, llvm-objcopy) in ${cands[*]}: brew install llvm@18, or set LLVM_BIN" >&2
    return 1
}
