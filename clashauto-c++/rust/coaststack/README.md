# coaststack —— Coast 用户态 TCP 数据面（smoltcp）

C++ 侧（`src/net/NetStack.cpp`）通过 `src/net/coaststack.h` 的 C ABI 调用本库。
**只做 TCP**；UDP/DNS、ARP/NDP 投毒、SOCKS 拨号与每设备身份全在 C++ 侧。

只在 **Windows** 上构建和链接 —— Linux 走 TPROXY、macOS 走 pf rdr，都不需要用户态栈。

## 构建

由 CMake 在 `if(WIN32)` 分支里自动调用（见根 `CMakeLists.txt`），target triple 按
**C++ 编译器**选：MSVC → `*-pc-windows-msvc`，MinGW → `x86_64-pc-windows-gnu`。
**staticlib 的 ABI/CRT 必须与 C++ 工具链一致**，否则链接期报一堆缺符号。

手动构建：

    cargo build --release --locked --target x86_64-pc-windows-msvc
    cargo test          # 单测（本仓库第一份）

## 关于依赖与离线构建

依赖树只有 8 个 crate（smoltcp + bitflags/byteorder/cfg-if/hash32/heapless/managed/
stable_deref_trait），全部纯 Rust、**零 build.rs 编 C**。这一点不是洁癖：staticlib 形态下
cargo 不调用链接器，所以 Windows ARM64 交叉编译不需要 ARM64 的 `cl.exe` —— 但前提是
依赖树里没有跑 `cc` crate 的包。**加依赖前先确认这一条。**

`Cargo.lock` 已提交，构建一律加 `--locked`，解析结果逐字节可复现。

**没有** vendor 依赖源码，因为 `cargo vendor` 会把 heapless 的可选 `defmt` feature 连同
`syn`/`quote`/`proc-macro2`/`thiserror` 一起拉进来（它不管 feature 是否启用）：
实际需要 ~3 MB，vendor 出来 11 MB，是整个 `third_party/lwip` 的两倍。
若将来 CI 真的被 crates.io 抖动咬到，再用 `cargo vendor` + `.cargo/config.toml`
（`rust/vendor/` 已在 `.gitignore` 里，本地随时可以 vendor）。
