# r8-dedup

A fast, zero-dependency **C11** cross-platform duplicate file finder engineered with a 4-stage filtering pipeline (**Size -> Edge Sample -> Full 128-bit Stream Digest -> Byte-for-Byte Verification**).

[![Download Release](https://img.shields.io/badge/Download-Linux_%26_Windows-A8B9CC?style=for-the-badge&logo=c&logoColor=black)](https://github.com/Amin8362/r8-dedup/releases/latest)

---

## Features

- **4-Stage High-Speed Pipeline:** Eliminates unique file sizes without disk reads, samples first/last 4 KB edges, streams full 128-bit digests with constant 32 KB RAM usage, and confirms duplicates byte-for-byte.
- **Zero False Positives:** Built-in exact byte verification (`memcmp`) ensures identical content before reporting or deleting duplicates (optional `--fast` mode skips final byte scan).
- **Safe Cleanup Modes:** Preview removable duplicates safely with `--dry-run` or reclaim disk space automatically with `--delete`.
- **Cross-Platform Native C11:** Uses POSIX `dirent/lstat` on Linux and Win32 `FindFirstFileA` on Windows with symlink-loop protection and zero external dependencies.

## Usage Examples

    # Scan the current folder for duplicates
    ./r8-dedup

    # Scan multiple directories with a minimum file size of 1 MB
    ./r8-dedup --min-size 1048576 ~/Downloads ~/Documents

    # Preview which duplicate files would be deleted (safe dry run)
    ./r8-dedup --dry-run ~/Downloads

    # Delete duplicate copies while keeping the first copy in each group
    ./r8-dedup --delete ~/Downloads

## Build from Source

    make
    ./r8-dedup -v

---

<div dir="rtl">

### معرفی فارسی

ابزار **`r8-dedup`** یک اسکنر فوق‌سریع و بدون وابستگی نوشته‌شده با **زبان C استاندارد (C11)** برای شناسایی و پاکسازی فایل‌های تکراری در لینوکس و ویندوز است. این ابزار با فیلتر چهارمرحله‌ای (حجم فایل، نمونه‌برداری ابتدایی و انتهایی، هش ۱۲۸ بیتی استریم‌شده و تطبیق قطعی بایت‌به‌بایت) بدون مصرف رم اضافی و بدون خطای مثبت کاذب، فضای هدررفته دیسک را گزارش و در صورت درخواست پاکسازی می‌کند.

</div>

---

**Author:** Amin Shirdeli (`R8Force`) • **License:** MIT
