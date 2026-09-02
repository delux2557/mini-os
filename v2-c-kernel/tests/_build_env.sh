# mini-os/v2-c-kernel/tests/_build_env.sh
# BUG-053 接线原语：令 QEMU shell 脚本的构建/录制产物根目录可经 BUILD 环境变量覆盖（缺省 build/）。
# 并发重负荷 harness 各以 `make BUILD=<私有目录>` / `BUILD=<私有目录> bash tests/xxx.sh`
# 运行，即可在互不污染（kernel.elf / transcripts / *.log / *.fifo / *.pcap）的前提下共享同一源码树。
# 用法：在每个 QEMU shell 脚本 `cd "$(dirname "$0")/.."` 之后立即 `source tests/_build_env.sh`。
# 约定：脚本内所有字面 `build/xxx` 一律改为 `$BUILD/xxx`，每次 make 调用一律传 `BUILD="$BUILD"`。
[ -n "${BUILD:-}" ] || BUILD="build"
mkdir -p "$BUILD"