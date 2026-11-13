"""Where the build tools live, shared by make_tscfix/make_cachefix/make_ierfix/make_ktty.
  LLVM_BIN  directory with clang, ld.lld and llvm-objcopy (default: Homebrew llvm@18, Apple Silicon or Intel prefix)
  NM_BIN    the nm that reads the Monterey kernel collection (default: /usr/bin/nm, Xcode's; it prints the kernel's symbols and then
            stops at the first kext it cannot parse, which the callers rely on)"""
import os
import sys
from pathlib import Path

_LLVM_DEFAULTS = ("/opt/homebrew/opt/llvm@18/bin", "/usr/local/opt/llvm@18/bin")


def llvm_bin():
    env = os.environ.get("LLVM_BIN")
    if env:
        cands = (env,)
    else:
        cands = _LLVM_DEFAULTS
    for d in cands:
        if all(os.access(Path(d) / t, os.X_OK) for t in ("clang", "ld.lld", "llvm-objcopy")):      # the same three tools/llvm.sh checks
            return d
    sys.exit(f"no LLVM (clang, ld.lld, llvm-objcopy) in {' or '.join(cands)}: brew install llvm@18, or set LLVM_BIN")


def nm_bin():
    nm = os.environ.get("NM_BIN", "/usr/bin/nm")
    if not Path(nm).exists():
        sys.exit(f"no nm at {nm}: install the Xcode command line tools, or set NM_BIN")
    return nm


def require_kc(kc):
    if not Path(kc).exists():
        sys.exit(f"missing {kc}: the macOS 12.7.6 kernel collection, see 'The macOS kernel collection' in INSTALL.md")
