# a30s-xnu4903

Porte do iOS 12.5.8 (iPhone 6) para o Galaxy A30s (Exynos 7904).

- `shim/`  : loader bare-metal do A30s (M3b: botoes, menu, touch). Compila com clang+lld no WSL2.
- `ci/build4903.sh` + `.github/workflows/xnu4903.yml` : compila o XNU 4903 (config T7000) num Mac virtual do GitHub.
- `xnu/patches/` : nossos patches (GICv2, timer, console, stubs de plataforma...). Vazio por enquanto.

Nenhum codigo da Apple e guardado aqui: o workflow baixa tudo de github.com/apple-oss-distributions a cada build.
