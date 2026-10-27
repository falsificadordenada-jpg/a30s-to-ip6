#!/bin/bash
# Compila o XNU 4903 (macOS 10.14.6 / era do iOS 12) para ARM64 T7000 (A8, iPhone 6)
# num runner macOS do GitHub Actions. Compativel com o bash 3.2 do macOS.
#
# Filosofia: cada etapa grava seu proprio log em out/<etapa>.log e o resultado em
# out/report.txt. Etapas que nao dependem uma da outra continuam rodando mesmo se
# uma falhar, para um unico run mostrar o maximo de problemas de uma vez.
# NADA do codigo da Apple e guardado no repo: tudo e baixado a cada build.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/work"; FR="$WORK/fakeroot"; BLD="$WORK/build"; OUT="$ROOT/out"
mkdir -p "$WORK" "$FR" "$BLD" "$OUT"
REPORT="$OUT/report.txt"; : > "$REPORT"

APPLE=https://github.com/apple-oss-distributions
XNU_TAG="${XNU_TAG:-xnu-4903.270.47}"
KCFG="${KERNEL_CONFIG:-DEVELOPMENT}"
SDK="${SDK:-iphoneos}"
MCFG="${MACHINE_CONFIG:-T7000}"        # T7000 = Apple A8 (iPhone 6)
KLC="$(echo "$KCFG" | tr 'A-Z' 'a-z').$(echo "$MCFG" | tr 'A-Z' 'a-z')"
# Tags do release macOS 10.14.6 (distribution-macOS @ macos-10146)
T_AVAIL=AvailabilityVersions-33.200.7
T_LIBSYS=Libsystem-1252.250.1
T_LIBPLAT=libplatform-177.270.1
T_LIBDISP=libdispatch-1008.270.1
T_BOOT=bootstrap_cmds-99.260.1
T_DTRACE=dtrace-284.250.4
NCPU="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
PCPU="$(sysctl -n hw.physicalcpu 2>/dev/null || echo 2)"
MEM="$(sysctl -n hw.memsize 2>/dev/null || echo 8589934592)"

# Xcode: o workflow ja fez xcode-select -s para um caminho unico (realpath). Nao mexer.
if XP="$(xcode-select -p 2>/dev/null)" && [ -d "$XP" ]; then DEVELOPER_DIR="$XP"; export DEVELOPER_DIR; fi
# Ferramentas Apple internas que nao existem no Xcode publico: stubs no PATH e em variaveis do make
STUBS="$WORK/stubs"; mkdir -p "$STUBS"
cat > "$STUBS/embedded_device_map" <<'EOS'
#!/bin/bash
# stub: so conhece o alvo iPhone 6 (n61 / t7000 / arm64)
q="$*"
case "$q" in
  *"KernelMachOArchitecture LIKE \"arm64\""*) echo n61 ;;
  *"KernelMachOArchitecture LIKE"*) ;;
  *"SELECT DISTINCT TargetType, KernelMachOArchitecture, KernelPlatform FROM Targets"*) echo "n61|arm64|t7000|" ;;
  *"KernelPlatform"*"FROM Targets WHERE"*) echo arm64 ;;
esac
exit 0
EOS
for t in ctfconvert ctfmerge ctfdump; do printf '#!/bin/sh\nexit 0\n' > "$STUBS/$t"; done
chmod +x "$STUBS"/*
STUBARGS="EMBEDDED_DEVICE_MAP=$STUBS/embedded_device_map EDM_DBPATH=/dev/null CTFCONVERT=$STUBS/ctfconvert CTFMERGE=$STUBS/ctfmerge CTFDUMP=$STUBS/ctfdump"
export PATH="$STUBS:$PATH"

# Flags que rebaixam para AVISO erros "novos" do clang moderno em codigo C de 2018
# (declaracao implicita, int<->ponteiro, return em funcao void, etc.). Nao mudam o codigo gerado.
COMPAT_WARN="-Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=incompatible-function-pointer-types -Wno-error=return-type -Wno-error=deprecated-non-prototype -Wno-error=strict-prototypes -Wno-error=int-to-pointer-cast -Wno-error=pointer-to-int-cast -Wno-return-type -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types"
SCRIPT_VERSION="v4 (2026-10-27)"

say() { echo "[$(date +%H:%M:%S)] $*"; }
FAILED=0
# stage NOME comando... : roda numa subshell com set -e, log em out/NOME.log
stage() {
  local name="$1"; shift
  say "=== ETAPA $name"
  ( set -e; "$@" ) > "$OUT/$name.log" 2>&1
  local rc=$?
  if [ $rc -eq 0 ]; then echo "PASS  $name" | tee -a "$REPORT"
  else
    echo "FAIL  $name (rc=$rc)" | tee -a "$REPORT"
    echo "----- ultimas 60 linhas de $name.log -----"; tail -n 60 "$OUT/$name.log"; echo "-----"
    grep -E "error:|\*\*\*|fatal:|No such file|not found" "$OUT/$name.log" | head -n 40 | sed "s/^/  [$name] /" >> "$REPORT"
    FAILED=1
  fi
  return $rc
}
clone() { # clone URL TAG DIR
  [ -d "$WORK/$3/.git" ] || git clone -q --depth 1 --branch "$2" "$1" "$WORK/$3"
}

# ---------------------------------------------------------------- fetch
do_fetch() {
  clone $APPLE/xnu.git                 "$XNU_TAG"   xnu
  clone $APPLE/AvailabilityVersions.git "$T_AVAIL"  AvailabilityVersions
  clone $APPLE/Libsystem.git           "$T_LIBSYS"  Libsystem
  clone $APPLE/libplatform.git         "$T_LIBPLAT" libplatform
  clone $APPLE/libdispatch.git         "$T_LIBDISP" libdispatch
  git -C "$WORK/xnu" log -1 --format='xnu %h %cd'
}

# ---------------------------------------------------------------- patches
# Nossos patches (xnu/patches/*.sh e *.patch) rodam em ordem. Falha = erro DURO
# (o script do blacktop pula patch que nao aplica em silencio; aqui nao).
do_ours() {
  cd "$WORK/xnu"
  for s in "$ROOT"/xnu/patches/*.sh; do [ -f "$s" ] || continue; echo "script $s"; bash "$s"; done
  for p in "$ROOT"/xnu/patches/*.patch; do [ -f "$p" ] || continue
    echo "patch $p"; git apply --check "$p"; git apply "$p"
  done
  echo "patches nossos: ok"
}
# Ajustes de compatibilidade com Xcode/SDK modernos (nao tocam na logica do kernel).
do_compat() {
  cd "$WORK/xnu"
  # availability.pl vem do AvailabilityVersions (instalado no fakeroot), nao do SDK
  sed -i '' "s|\${SDKROOT}/usr/local/libexec/availability.pl|$FR/usr/local/libexec/availability.pl|g" bsd/sys/make_symbol_aliasing.sh
  # libfirehose_kernel e headers do libdispatch/libplatform vem do fakeroot
  sed -i '' "s|^LDFLAGS_KERNEL_SDK[[:space:]]*= -L\$(SDKROOT)/usr/local/lib/kernel|LDFLAGS_KERNEL_SDK = -L$FR/usr/local/lib/kernel|" makedefs/MakeInc.def
  sed -i '' "s|^INCFLAGS_SDK[[:space:]]*= -I\$(SDKROOT)/usr/local/include/kernel|INCFLAGS_SDK = -I$FR/usr/local/include/kernel|" makedefs/MakeInc.def
  sed -i '' 's|^#include.*BSD.xcconfig.*||g' libsyscall/Libsyscall.xcconfig
  # Scripts Python 2 do xnu (o macOS 14 nao tem /usr/bin/python): usamos reescritas nossas em py3
  cat > bsd/kern/makekdebugevents.py <<'PYEOF1'
#!/usr/bin/env python3
# Reimplementacao em Python 3 (escrita por nos) do gerador kdebugevents.h:
# le trace.codes (linhas "0xID NOME") e imprime a tabela kd_events[] em C.
import re
import sys

if len(sys.argv) < 2:
    sys.exit(1)

pat = re.compile(r'0x([0-9a-fA-F]+)\s+([^\s]*)')
table = []
with open(sys.argv[1], 'rt') as f:
    for line in f:
        m = pat.match(line)
        if m:
            table.append((int(m.group(1), 16), m.group(2)))

print("typedef struct {")
print("        uint32_t   id;")
print("        const char *name;")
print("} kd_event_t;")
print("kd_event_t kd_events[] = {")
for ev_id, name in sorted(table, key=lambda x: x[0]):
    print('        {0x%x, "%s"},' % (ev_id, name))
print("};")
PYEOF1
  cat > san/tools/generate_dynamic_blacklist.py <<'PYEOF2'
#!/usr/bin/env python3
# Reimplementacao em Python 3 (escrita por nos) do gerador da blacklist dinamica do KASAN/san:
# entrada: linhas "kext:funcao:tipo" (comentarios com #); saida: array C blacklist[].
import re
import sys

EXTRA = 5
comment = re.compile(r'#.*$')
entries = []
with open(sys.argv[1]) as f:
    for line in f:
        line = comment.sub("", line).strip()
        if not line:
            continue
        fields = line.split(":")
        if len(fields) != 3:
            continue
        kext, func, ty = fields
        kext = '"%s"' % kext if kext else "NULL"
        func = '"%s"' % func if func else "NULL"
        entries.append((kext, func, "TYPE_" + (ty or "normal").upper()))

def entry(k, fn, t):
    return "\t{\n\t\t.kext_name = %s,\n\t\t.func_name = %s,\n\t\t.type_mask = %s,\n\t}," % (k, fn, t)

print("struct blacklist_entry blacklist[] = {")
for e in entries:
    print(entry(*e))
print("")
print("\t/* Unused entries that can be populated at runtime */")
for _ in range(EXTRA):
    print(entry("NULL", "NULL", 0))
print("};")
print("")
print("static size_t blacklist_entries = %d;" % len(entries))
print("static const size_t blacklist_max_entries = %d;" % (len(entries) + EXTRA))
PYEOF2
  chmod +x bsd/kern/makekdebugevents.py san/tools/generate_dynamic_blacklist.py
  # o alvo "install" completo chama checkstyle (tools/uncrustify.sh nao existe no fonte publico)
  sed -i '' 's/install_aliases checkstyle$/install_aliases/' makedefs/MakeInc.top
  grep -n "LDFLAGS_KERNEL_SDK\|INCFLAGS_SDK" makedefs/MakeInc.def | head
  python3 -I bsd/kern/makekdebugevents.py /dev/null >/dev/null && echo 'py3 makekdebugevents ok'
  grep -n "^install:" makedefs/MakeInc.top
}

# ---------------------------------------------------------------- ferramentas
do_tools() {
  mkdir -p "$FR/usr/local/libexec"
  cp "$WORK/AvailabilityVersions/availability.pl" "$FR/usr/local/libexec/availability.pl"
  chmod +x "$FR/usr/local/libexec/availability.pl"
  test -x "$FR/usr/local/libexec/availability.pl"
  for t in mig migcom ctfconvert ctfmerge ctf_insert unifdef python3; do
    printf '%-12s ' "$t"; xcrun -sdk "$SDK" -find "$t" 2>&1 || true
  done
  xcrun -sdk "$SDK" -find mig >/dev/null
  xcrun -sdk "$SDK" -find migcom >/dev/null
  xcrun -sdk "$SDK" -show-sdk-path; xcrun -sdk "$SDK" -show-sdk-version
}

# ctfconvert/ctfmerge nao vem no Xcode: compilamos do projeto dtrace (opcional)
do_ctf() {
  clone $APPLE/dtrace.git "$T_DTRACE" dtrace
  cd "$WORK/dtrace"
  env LD="$(xcrun -find clang)" LDPLUSPLUS="$(xcrun -find clang++)" \
    xcodebuild install -sdk macosx -target ctfconvert -target ctfdump -target ctfmerge ARCHS="arm64" \
    CODE_SIGN_IDENTITY="-" OBJROOT="$BLD/dtrace.obj" SYMROOT="$BLD/dtrace.sym" DSTROOT="$FR"
  test -x "$FR/usr/local/bin/ctfconvert"
}
CTFARGS=""
false && CTFARGS="CTFCONVERT=$FR/usr/local/bin/ctfconvert CTFMERGE=$FR/usr/local/bin/ctfmerge CTFDUMP=$FR/usr/local/bin/ctfdump"

# ---------------------------------------------------------------- headers
do_xnu_headers() {
  cd "$WORK/xnu"
  make installhdrs $STUBARGS SDKROOT="$SDK" ARCH_CONFIGS="ARM64" TARGET_CONFIGS="$KCFG ARM64 $MCFG" \
    ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 \
    SRCROOT="$WORK/xnu" OBJROOT="$BLD/xnu-hdrs.obj" SYMROOT="$BLD/xnu-hdrs.sym" DSTROOT="$FR" FAKEROOT_DIR="$FR" \
    MEMORY_SIZE="$MEM" SYSCTL_HW_PHYSICALCPU="$PCPU" SYSCTL_HW_LOGICALCPU="$NCPU" KERNEL_BUILDS_IN_PARALLEL=1 \
    RC_DARWIN_KERNEL_VERSION=18.7.0
}
do_libsystem_hdrs() {
  cd "$WORK/Libsystem"
  sed -i '' 's|^#include.*BSD.xcconfig.*||g' Libsystem.xcconfig
  env LD="$(xcrun -find clang)" LDPLUSPLUS="$(xcrun -find clang++)" \
    xcodebuild installhdrs -sdk macosx ARCHS="arm64" VALID_ARCHS="arm64" \
    OBJROOT="$BLD/Libsystem.obj" SYMROOT="$BLD/Libsystem.sym" DSTROOT="$FR" FAKEROOT_DIR="$FR" CODE_SIGN_IDENTITY="-"
}
do_libsyscall_hdrs() {
  cd "$WORK/xnu/libsyscall"
  env LD="$(xcrun -find clang)" LDPLUSPLUS="$(xcrun -find clang++)" \
    xcodebuild installhdrs -sdk macosx TARGET_CONFIGS="$KCFG ARM64 $MCFG" ARCHS="arm64" VALID_ARCHS="arm64" ONLY_ACTIVE_ARCH=YES \
    MACOSX_DEPLOYMENT_TARGET=10.14 IPHONEOS_DEPLOYMENT_TARGET=12.0 \
    OBJROOT="$BLD/libsyscall.obj" SYMROOT="$BLD/libsyscall.sym" DSTROOT="$FR" FAKEROOT_DIR="$FR" CODE_SIGN_IDENTITY="-"
}
do_libplatform() {
  cd "$WORK/libplatform"
  ditto include "$FR/usr/local/include"
  ditto private "$FR/usr/local/include"
}
do_firehose() {
  cd "$WORK/libdispatch"
  mkdir -p "$DEVELOPER_DIR/Makefiles/CoreOS/Xcode" 2>/dev/null || true
  # includes de xcconfig internos da Apple (BSD.xcconfig, PlatformSupport.xcconfig) nao existem no Xcode publico
  sed -i '' -e '/BSD.xcconfig/d' -e '/PlatformSupport.xcconfig/d' xcodeconfig/*.xcconfig
  sed -i '' "s|\$(SDKROOT)/System/Library/Frameworks/Kernel.framework/PrivateHeaders|$FR/System/Library/Frameworks/Kernel.framework/PrivateHeaders|g" xcodeconfig/libfirehose_kernel.xcconfig
  sed -i '' "s|\$(SDKROOT)/usr/local/include|$FR/usr/local/include|g" xcodeconfig/libfirehose_kernel.xcconfig
  # includes de xcconfig internos da Apple que nao existem no Xcode publico
  sed -i '' -e '/#include.*BSD.xcconfig/d' -e '/#include.*PlatformSupport.xcconfig/d' xcodeconfig/*.xcconfig
  printf '\nOTHER_CFLAGS = $(inherited) %s\n' "$COMPAT_WARN" >> xcodeconfig/libfirehose_kernel.xcconfig
  env LD="$(xcrun -find clang)" LDPLUSPLUS="$(xcrun -find clang++)" \
    xcodebuild install -target libfirehose_kernel -sdk macosx ARCHS="arm64" VALID_ARCHS="arm64" \
    GCC_TREAT_IMPLICIT_FUNCTION_DECLARATIONS_AS_ERRORS=NO GCC_TREAT_WARNINGS_AS_ERRORS=NO \
    OBJROOT="$BLD/firehose.obj" SYMROOT="$BLD/firehose.sym" DSTROOT="$FR" FAKEROOT_DIR="$FR" CODE_SIGN_IDENTITY="-"
  mkdir -p "$FR/usr/local/lib/kernel"
  [ -f "$FR/usr/local/lib/kernel/liblibfirehose_kernel.a" ] && mv "$FR/usr/local/lib/kernel/liblibfirehose_kernel.a" "$FR/usr/local/lib/kernel/libfirehose_kernel.a"
  ls -la "$FR/usr/local/lib/kernel"
  test -f "$FR/usr/local/lib/kernel/libfirehose_kernel.a"
}

# ---------------------------------------------------------------- kernel
do_kernel() {
  cd "$WORK/xnu"
  make "${KTARGET:-install_development_embedded}" $STUBARGS OTHER_CFLAGS="$COMPAT_WARN" -j"$NCPU" VERBOSE=YES CONCISE=0 $CTFARGS SDKROOT="$SDK" TARGET_CONFIGS="$KCFG ARM64 $MCFG" \
    ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 BUILD_WERROR=0 BUILD_LTO=0 DO_CTFCONVERT=0 DO_CTFMERGE=0 DO_CTFMACHO=0 \
    SRCROOT="$WORK/xnu" OBJROOT="$BLD/xnu.obj" SYMROOT="$BLD/xnu.sym" DSTROOT="$FR" FAKEROOT_DIR="$FR" \
    MEMORY_SIZE="$MEM" SYSCTL_HW_PHYSICALCPU="$PCPU" SYSCTL_HW_LOGICALCPU="$NCPU" KERNEL_BUILDS_IN_PARALLEL=1 \
    RC_DARWIN_KERNEL_VERSION=18.7.0 ${MAKE_KEEP_GOING:+-k}
}
collect() {
  mkdir -p "$OUT/kernel"
  find "$WORK" -name "kernel.$KLC*" -type f \( -perm -u+x -o -name "*.unstripped" -o -name "*.dSYM" \) 2>/dev/null | while read -r f; do cp -R "$f" "$OUT/kernel/"; done
  find "$FR" -path "*Kernels*" -name "kernel.*" -type f -exec cp {} "$OUT/kernel/" \; 2>/dev/null || true
  ls -la "$OUT/kernel"
  ( cd "$OUT/kernel" && ls kernel.* >/dev/null 2>&1 && shasum -a 256 kernel.* > SHA256SUMS ) || true
  true
  for f in "$OUT"/kernel/kernel.*; do [ -f "$f" ] && { file "$f"; xcrun lipo -info "$f" 2>&1 || true; }; done
}

say "script $SCRIPT_VERSION"; say "xnu=$XNU_TAG kcfg=$KCFG machine=$MCFG sdk=$SDK ncpu=$NCPU"
sw_vers; xcodebuild -version
stage 01-fetch          do_fetch      || { echo "FATAL: sem fontes"; exit 2; }
stage 02-patches-nossos do_ours       || { echo "FATAL: patch nosso nao aplicou (veja out/02-patches-nossos.log)"; exit 3; }
stage 03-compat         do_compat
stage 04-ferramentas    do_tools
CTFARGS=""; false && CTFARGS="CTFCONVERT=$FR/usr/local/bin/ctfconvert CTFMERGE=$FR/usr/local/bin/ctfmerge CTFDUMP=$FR/usr/local/bin/ctfdump"
stage 05-xnu-headers    do_xnu_headers
stage 06-libsystem-hdrs do_libsystem_hdrs
stage 07-libsyscall-hdrs do_libsyscall_hdrs
stage 08-libplatform    do_libplatform
stage 09-firehose       do_firehose
stage 10-kernel         do_kernel
stage 11-coletar        collect || true
{
  echo ""; echo "=== ERROS MAIS FREQUENTES (todas as etapas; sem ruido de xcrun) ==="
  cat "$OUT"/*.log 2>/dev/null | grep -E "error:" | grep -v "xcrun: error" | sed -E 's#/Users/runner/work/[^ ]*/work/##g; s#^[^ ]* ##' | sed -E 's/:[0-9]+:[0-9]+:/:/' | sort | uniq -c | sort -rn | head -50
  echo ""; echo "=== ARQUIVOS COM ERRO (kernel) ==="
  grep -E "error:" "$OUT/10-kernel.log" 2>/dev/null | grep -v "xcrun: error" | sed -E 's#/Users/runner/work/[^ ]*/work/##; s/:[0-9]+:[0-9]+:.*//' | sort | uniq -c | sort -rn | head -40
} >> "$REPORT" 2>&1
echo "---------- RELATORIO ----------"; cat "$REPORT"
[ -n "$(ls "$OUT"/kernel/kernel.* 2>/dev/null)" ] || { echo "SEM KERNEL"; exit 1; }
exit $FAILED
