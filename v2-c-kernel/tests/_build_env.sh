# mini-os/v2-c-kernel/tests/_build_env.sh
# BUG-053 接线原语：令 QEMU shell 脚本的构建/录制产物根目录可经 BUILD 环境变量覆盖（缺省 build/）。
# 并发重负荷 harness 各以 `make BUILD=<私有目录>` / `BUILD=<私有目录> bash tests/xxx.sh`
# 运行，即可在互不污染（kernel.elf / transcripts / *.log / *.fifo / *.pcap）的前提下共享同一源码树。
# 用法：在每个 QEMU shell 脚本 `cd "$(dirname "$0")/.."` 之后立即 `source tests/_build_env.sh`。
# 约定：脚本内所有字面 `build/xxx` 一律改为 `$BUILD/xxx`，每次 make 调用一律传 `BUILD="$BUILD"`。
[ -n "${BUILD:-}" ] || BUILD="build"
# S-1（评审）：export 使 BUILD 成为"进程环境变量"宽松可见——任何未来以子进程方式调用本库
# （如 `bash tests/transcript.sh` 之类）的脚本内 `${BUILD:-build}` 也能拿到外层接受的 BUILD，
# 不会因 source 作用域边界而静默回退共享 build/ 造成并发污染。当前无此路径（GNU make 命令行变量
# 会 export 到 recipe、库脚本均由调用方 source 引入），此行是面向未来的隐性契约。
export BUILD
mkdir -p "$BUILD"