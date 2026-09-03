# cc500 自举编译器

`cc500.c` 是 [Edmund GRIMLEY EVANS](http://homepage.ntlworld.com/edmund.grimley-evans/cc500/)
的 cc500（Copyright © 2006）的**移植 / 衍生** C 子集编译器。本仓库内按 **GPL-2.0-or-later**
提供，许可证全文见本目录 [`LICENSE`](LICENSE)。

## 在本项目中的角色
mini-os 的自举工具链（v0.27 引入）：guest 内用 cc500 把 C 子集源码编译为 ELF 并运行，
支撑"写-编-跑 / 编译器自编译"闭环（shell `ccrun` / `ccboot`）。

## 与内核的集成
- 源码 `cc500.c` 以原始字节 blob 嵌入内核（Makefile `cc500_csrc.o`），boot 时写入
  initramfs `/cc500.c`；
- 编译产物可经 shell `ccrun <src> <out>` 调用，`ccboot` 做二次自举字节一致性校验。

## 许可证边界
本组件为独立 GPL 组件，与仓库其余部分（MIT）分开授权：**使用 / 修改 / 再分发** 本组件请按
GPL-2.0-or-later 履行义务（随衍生作品提供一份 GPL 许可副本等），勿将本组件当作仓库根
`LICENSE`（MIT）覆盖的代码。