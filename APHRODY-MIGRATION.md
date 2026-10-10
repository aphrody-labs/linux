# Aphrody Linux 7.2 migration

Base: upstream Linux 7.2.9, commit
`5fce161649b4d779d1b76d9fcd52dc77779774b8` (tag `v7.2.9`).
The existing 6.18.55 `aphrody-bun` checkout remains intact.

## Retained fork changes

- `c2201a070dd62`, `ab61c299be8b7`, `47f637c546bf4`: `bun_accel`, its
  `file_user_path` helper and userspace selftests. The helper now uses the 7.2
  `__rust_helper` annotation.
- `b7c539d96f220`, `6166e7207d14a`, `3db127080b814`, `1a414f242df93`:
  retain Zstd level 19, the kexec disable boot option, the PowerPC compiler
  guard and the AMD Xen reset-reporting correction.
- `defedc4f61082`: retain the overridable objtool AWK setting at its new location.
- `d3e787ecbac0a`, `22d9a34fa6733`, `41f78f90f5c96`, `95c10fdc6a9ee`,
  `952d4bb7adc3f`, `1ca900c3cfed9`, `a840ffe4b583d`, `1d842f9dd0f6b`:
  changes already present in the 7.2.9 source, verified with reverse patch checks.
- `bf71bd5f4816f`: all four SG2042 PCIe controller nodes are present upstream;
  7.2.9 additionally marks them DMA-coherent. Keep the newer upstream nodes.

## Profiles and qualification

Run `KBUILD_OUTPUT=/absolute/build/path bash scripts/aphrody-config.sh
generic` or `nvidia`, optionally with `ARCH=arm64`. The script filters the
architecture markers, checks the Rust toolchain and rejects every requested
setting that Kconfig drops. The native NVIDIA profile preserves Rust, DWARF,
BTF, sched_ext, MGLRU, madvise THP, device-memory hotplug and HMM helpers.
`APHRODY_NVIDIA_HMM` selects HMM without enabling Nova/Nouveau.

On the Ubuntu 26.04 VPS, the native x86 profile passed strict olddefconfig
with Rust 1.98.1, Clang 21.1.8, bindgen 0.73.2 and pahole 1.31. On 2026-10-10,
the bzImage and modules built, and `CLIPPY=1` compiled the Rust driver. A
diskless QEMU TCG guest booted Linux 7.2.9, loaded `bun_accel.ko` and passed
all seven static userspace selftests, including unprivileged access rejection.
The first combined build script returned 2 because the selftest output
directory was absent; the follow-up gates and boot scripts returned 0.

Qualified artifact SHA-256 values:

- bzImage: `29bc986aacdb83a49ea83847b8c8a678ab7bb37b29ce32cc6e2eb0e650c92244`
- bun_accel.ko: `cfe45dedfd14843639ac5d161dbca768e5bca88dbf2a75c55c0c837751628279`
- copy_batch: `4493f239267b8590e0fea604a524c4171e6aa56afd8b925a437bab53b9ccbb48`

Native NVIDIA module compatibility, hardware CUDA and before/after memory
measurements remain separate gates. No running host kernel was changed.

NVIDIA modules, GSP firmware and userspace must use the same NVIDIA release.
This profile does not package them. WSL requires a separate Microsoft dxgkrnl
port and the Windows NVIDIA driver; it must not load the native PCI driver.
Alpine's musl userspace needs separate D3D12/CUDA ABI qualification.
