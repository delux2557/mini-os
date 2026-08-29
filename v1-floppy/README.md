# v1-floppy（v0.1，已冻结）

早期原型：软盘引导扇区，进入保护模式并打印 "Hello Micro-OS!"。

- `boot.asm`：引导扇区源码（唯一需要保留的内容）
- `boot.bin` / `os.img`：构建产物（由 boot.asm 生成，可忽略）

> 该版本已被 v2-c-kernel 取代，仅作历史留存，不再开发。
> 运行截图见 [../docs/screenshots/v1_protected_mode.png](../docs/screenshots/v1_protected_mode.png)。
