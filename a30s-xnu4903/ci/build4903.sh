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
  grep -n "LDFLAGS_KERNEL_SDK\|INCFLAGS_SDK" makedefs/MakeInc.def | head
}

# ---------------------------------------------------------------- ferramentas
do_tools() {
  cd "$WORK/AvailabilityVersions"
  make install -j"$NCPU" OBJROOT="$BLD/" SYMROOT="$BLD/" DSTROOT="$FR"
  test -x "$FR/usr/local/libexec/availability.pl"
  for t in mig migcom ctfconvert ctfmerge ctf_insert unifdef; do
    printf '%-12s ' "$t"; xcrun -sdk "$SDK" -find "$t" 2>&1 || true
  done
  xcrun -sdk "$SDK" -find mig >/dev/null
  xcrun -sdk "$SDK" -find migcom >/dev/null
  echo "embedded_device_map:"; xcrun -sdk "$SDK" -find embedded_device_map 2>&1 || echo "(ausente: ok, usamos TARGET_CONFIGS explicito)"
  xcrun -sdk "$SDK" -show-sdk-path; xcrun -sdk "$SDK" -show-sdk-version
}

# ---------------------------------------------------------------- headers
do_xnu_headers() {
  cd "$WORK/xnu"
  make installhdrs SDKROOT="$SDK" ARCH_CONFIGS="ARM64" TARGET_CONFIGS="$KCFG ARM64 $MCFG" \
    ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 \
    SRCROOT="$WORK/xnu" OBJROOT="$BLD/xnu-hdrs.obj" SYMROOT="$BLD/xnu-hdrs.sym" DSTROOT="$FR" FAKEROOT_DIR="$FR" \
    MEMORY_SIZE="$MEM" SYSCTL_HW_PHYSICALCPU="$PCPU" SYSCTL_HW_LOGICALCPU="$NCPU" KERNEL_BUILDS_IN_PARALLEL=1 \
    RC_DARWIN_KERNEL_VERSION=18.7.0
}
do_libsystem_hdrs() {
  cd "$WORK/Libsystem"
  sed -i '' 's|^#include.*BSD.xcconfig.*||g' Libsystem.xcconfig
  env LD="$(xcrun -find clang)" LDPLUSPLUS="$(xcrun -find clang++)" \
    xcodebuild installhdrs -sdk "$SDK" ARCHS="arm64" VALID_ARCHS="arm64" \
    OBJROOT="$BLD/Libsystem.obj" SYMROOT="$BLD/Libsystem.sym" DSTROOT="$FR" FAKEROOT_DIR="$FR" CODE_SIGN_IDENTITY="-"
}
do_libsyscall_hdrs() {
  cd "$WORK/xnu/libsyscall"
  env LD="$(xcrun -find clang)" LDPLUSPLUS="$(xcrun -find clang++)" \
    xcodebuild installhdrs -sdk "$SDK" TARGET_CONFIGS="$KCFG ARM64 $MCFG" ARCHS="arm64" VALID_ARCHS="arm64" \
    OBJROOT="$BLD/libsyscall.obj" SYMROOT="$BLD/libsyscall.sym" DSTROOT="$FR" FAKEROOT_DIR="$FR" CODE_SIGN_IDENTITY="-"
}
do_libplatform() {
  cd "$WORK/libplatform"
  ditto include "$FR/usr/local/include"
  ditto private "$FR/usr/local/include"
}
do_firehose() {
  cd "$WORK/libdispatch"
  sed -i '' "s|\$(SDKROOT)/System/Library/Frameworks/Kernel.framework/PrivateHeaders|$FR/System/Library/Frameworks/Kernel.framework/PrivateHeaders|g" xcodeconfig/libfirehose_kernel.xcconfig
  sed -i '' "s|\$(SDKROOT)/usr/local/include|$FR/usr/local/include|g" xcodeconfig/libfirehose_kernel.xcconfig
  env LD="$(xcrun -find clang)" LDPLUSPLUS="$(xcrun -find clang++)" \
    xcodebuild install -target libfirehose_kernel -sdk "$SDK" ARCHS="arm64" VALID_ARCHS="arm64" \
    OBJROOT="$BLD/firehose.obj" SYMROOT="$BLD/firehose.sym" DSTROOT="$FR" FAKEROOT_DIR="$FR" CODE_SIGN_IDENTITY="-"
  mkdir -p "$FR/usr/local/lib/kernel"
  [ -f "$FR/usr/local/lib/kernel/liblibfirehose_kernel.a" ] && mv "$FR/usr/local/lib/kernel/liblibfirehose_kernel.a" "$FR/usr/local/lib/kernel/libfirehose_kernel.a"
  ls -la "$FR/usr/local/lib/kernel"
  test -f "$FR/usr/local/lib/kernel/libfirehose_kernel.a"
}

# ---------------------------------------------------------------- kernel
do_kernel() {
  cd "$WORK/xnu"
  make install -j"$NCPU" VERBOSE=YES CONCISE=0 SDKROOT="$SDK" TARGET_CONFIGS="$KCFG ARM64 $MCFG" \
    ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 BUILD_WERROR=0 BUILD_LTO=0 \
    SRCROOT="$WORK/xnu" OBJROOT="$BLD/xnu.obj" SYMROOT="$BLD/xnu.sym" DSTROOT="$FR" FAKEROOT_DIR="$FR" \
    MEMORY_SIZE="$MEM" SYSCTL_HW_PHYSICALCPU="$PCPU" SYSCTL_HW_LOGICALCPU="$NCPU" KERNEL_BUILDS_IN_PARALLEL=1 \
    RC_DARWIN_KERNEL_VERSION=18.7.0 ${MAKE_KEEP_GOING:+-k}
}
collect() {
  mkdir -p "$OUT/kernel"
  find "$WORK" -name "kernel.$KLC*" -type f \( -perm -u+x -o -name "*.unstripped" -o -name "*.dSYM" \) 2>/dev/null | while read -r f; do cp -R "$f" "$OUT/kernel/"; done
  find "$FR" -path "*Kernels*" -name "kernel.*" -type f -exec cp {} "$OUT/kernel/" \; 2>/dev/null || true
  ls -la "$OUT/kernel"
  ( cd "$OUT/kernel" && shasum -a 256 * > SHA256SUMS ) || true
  for f in "$OUT"/kernel/kernel.*; do [ -f "$f" ] && { file "$f"; xcrun lipo -info "$f" 2>&1 || true; }; done
}

say "xnu=$XNU_TAG kcfg=$KCFG machine=$MCFG sdk=$SDK ncpu=$NCPU"
sw_vers; xcodebuild -version
stage 01-fetch          do_fetch      || { echo "FATAL: sem fontes"; exit 2; }
stage 02-patches-nossos do_ours       || { echo "FATAL: patch nosso nao aplicou (veja out/02-patches-nossos.log)"; exit 3; }
stage 03-compat         do_compat
stage 04-ferramentas    do_tools
stage 05-xnu-headers    do_xnu_headers
stage 06-libsystem-hdrs do_libsystem_hdrs
stage 07-libsyscall-hdrs do_libsyscall_hdrs
stage 08-libplatform    do_libplatform
stage 09-firehose       do_firehose
stage 10-kernel         do_kernel
stage 11-coletar        collect || true
echo "---------- RELATORIO ----------"; cat "$REPORT"
[ -n "$(ls "$OUT"/kernel/kernel.* 2>/dev/null)" ] || { echo "SEM KERNEL"; exit 1; }
exit $FAILED
