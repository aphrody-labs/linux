// SPDX-License-Identifier: GPL-2.0

//! `/dev/bun_accel`: batched file copies for package managers.
//!
//! Populating `node_modules` from a package cache opens, creates, copies and
//! closes every file, five system calls per file. io_uring batches `openat`
//! and `close` but has no clone or copy opcode, so each copy still costs a
//! round trip. `BUN_ACCEL_IOC_COPY_BATCH` performs up to [`MAX_BATCH`] copies
//! per call. `vfs_copy_file_range()` shares extents where the file system
//! supports it; between file systems the copy is retried as an in-kernel
//! splice.
//!
//! Every lookup and open runs in the caller's context with its credentials,
//! permission checks, LSM hooks and quotas, so the device grants nothing the
//! caller could not do with `openat()` and `copy_file_range()`. Confining
//! names to their directory is a convenience, not a security boundary.

use core::mem::{offset_of, size_of};

use kernel::{
    bindings, c_str,
    error::from_err_ptr,
    fs::{File, LocalFile},
    ioctl::_IOWR,
    miscdevice::{MiscDevice, MiscDeviceOptions, MiscDeviceRegistration},
    prelude::*,
    transmute::FromBytes,
    uaccess::{UserPtr, UserSlice},
};

module! {
    type: BunAccelModule,
    name: "bun_accel",
    authors: ["aphrody-dev"],
    description: "Batched file copies for package managers",
    license: "GPL",
}

/// `BUN_ACCEL_CLONE_ONLY`
const CLONE_ONLY: u32 = 1 << 0;
/// `BUN_ACCEL_SOURCE_MODE`
const SOURCE_MODE: u32 = 1 << 1;
/// `BUN_ACCEL_MAX_BATCH`
const MAX_BATCH: u32 = 1024;
/// Bytes requested per `vfs_copy_file_range()` call, below `MAX_RW_COUNT`.
const COPY_CHUNK: usize = 1 << 30;
/// `PATH_MAX`, including the terminating NUL.
const NAME_BUF: usize = bindings::PATH_MAX as usize;

/// `struct bun_accel_copy`
#[repr(C)]
#[derive(Clone, Copy)]
struct CopyEntry {
    src_dirfd: i32,
    dst_dirfd: i32,
    src_path: u64,
    dst_path: u64,
    mode: u32,
    flags: u32,
    result: i64,
}

// SAFETY: `CopyEntry` holds only integers and has no padding, so any bit pattern is valid.
unsafe impl FromBytes for CopyEntry {}

/// `struct bun_accel_batch`
#[repr(C)]
#[derive(Clone, Copy)]
struct Batch {
    entries: u64,
    count: u32,
    flags: u32,
    done: u32,
    reserved: u32,
}

// SAFETY: `Batch` holds only integers and has no padding, so any bit pattern is valid.
unsafe impl FromBytes for Batch {}

const IOC_COPY_BATCH: u32 = _IOWR::<Batch>(0xB9, 0x01);

/// A file reference returned by `file_open_root()`, dropped with `fput()`.
struct Opened(*mut bindings::file);

impl Opened {
    /// Opens `name` with `dir` as the root: `..` and absolute symbolic links
    /// stop at `dir`. Unlike `RESOLVE_IN_ROOT`, a concurrent rename can still
    /// move the walk outside; it then only reaches what the caller may open.
    fn beneath(dir: &LocalFile, name: &CStr, flags: u32, mode: u32) -> Result<Self> {
        // SAFETY: `dir` holds a reference to a valid file for the duration of the call.
        let root = unsafe { bindings::file_user_path(dir.as_ptr()) };
        // SAFETY: `root` stays valid while `dir` is held and `name` is NUL-terminated;
        // `file_open_root()` copies the name before returning.
        let file = from_err_ptr(unsafe {
            bindings::file_open_root(
                root,
                name.as_char_ptr(),
                flags as c_int,
                mode as bindings::umode_t,
            )
        })?;
        Ok(Self(file))
    }
}

impl Drop for Opened {
    fn drop(&mut self) {
        // SAFETY: `self.0` is a file reference owned by this value and released once.
        unsafe { bindings::fput(self.0) };
    }
}

/// Copies the NUL-terminated user string at `addr` into `buf`.
fn read_name(addr: u64, buf: &mut [u8]) -> Result<&CStr> {
    let addr = usize::try_from(addr).map_err(|_| EFAULT)?;
    let len = buf.len();
    let name = UserSlice::new(UserPtr::from_addr(addr), len)
        .reader()
        .strcpy_into_buf(buf)?;
    let name_len = name.to_bytes().len();
    if name_len == 0 {
        return Err(ENOENT);
    }
    // `strcpy_into_buf()` truncates silently: a name that filled the buffer may be cut.
    if name_len + 1 >= len {
        return Err(Error::from_errno(-(bindings::ENAMETOOLONG as c_int)));
    }
    Ok(name)
}

fn copy_one(entry: &CopyEntry, src_buf: &mut [u8], dst_buf: &mut [u8]) -> Result<i64> {
    if entry.flags & !(CLONE_ONLY | SOURCE_MODE) != 0 || entry.mode & !0o7777 != 0 {
        return Err(EINVAL);
    }
    if entry.flags & SOURCE_MODE != 0 && entry.mode != 0 {
        return Err(EINVAL);
    }
    let src_name = read_name(entry.src_path, src_buf)?;
    let dst_name = read_name(entry.dst_path, dst_buf)?;
    // A negative descriptor becomes a large `u32` that `fget()` rejects with `EBADF`.
    let src_dir = LocalFile::fget(entry.src_dirfd as u32)?;
    let dst_dir = LocalFile::fget(entry.dst_dirfd as u32)?;

    // `O_NONBLOCK` keeps a FIFO from blocking the open; only regular files are copied.
    let src = Opened::beneath(
        &src_dir,
        src_name,
        bindings::O_RDONLY | bindings::O_NOFOLLOW | bindings::O_NONBLOCK,
        0,
    )?;
    // SAFETY: an open file always has a valid inode.
    let src_mode = u32::from(unsafe { (*(*src.0).f_inode).i_mode });
    if src_mode & bindings::S_IFMT != bindings::S_IFREG {
        return Err(EINVAL);
    }
    let mode = if entry.flags & SOURCE_MODE != 0 {
        src_mode & 0o777
    } else {
        entry.mode
    };
    let dst = Opened::beneath(
        &dst_dir,
        dst_name,
        bindings::O_WRONLY | bindings::O_CREAT | bindings::O_EXCL | bindings::O_NOFOLLOW,
        mode,
    )?;

    if entry.flags & CLONE_ONLY != 0 {
        // SAFETY: both files are referenced until the end of the function; a zero
        // length clones up to the end of the source.
        let cloned = unsafe { bindings::vfs_clone_file_range(src.0, 0, dst.0, 0, 0, 0) };
        return if cloned < 0 {
            Err(Error::from_errno(cloned as c_int))
        } else {
            Ok(cloned)
        };
    }

    let mut copied: i64 = 0;
    let mut flags: c_uint = 0;
    loop {
        if kernel::current!().signal_pending() {
            return Err(EINTR);
        }
        // SAFETY: both files are referenced until the end of the function.
        let n = unsafe {
            bindings::vfs_copy_file_range(src.0, copied, dst.0, copied, COPY_CHUNK, flags)
        };
        if n == -(bindings::EXDEV as isize) && flags == 0 && copied == 0 {
            flags = bindings::COPY_FILE_SPLICE;
            continue;
        }
        if n < 0 {
            return Err(Error::from_errno(n as c_int));
        }
        if n == 0 {
            return Ok(copied);
        }
        copied += n as i64;
    }
}

fn copy_batch(arg: usize) -> Result<isize> {
    let batch: Batch = UserSlice::new(UserPtr::from_addr(arg), size_of::<Batch>())
        .reader()
        .read()?;
    if batch.flags != 0 || batch.reserved != 0 || batch.count > MAX_BATCH {
        return Err(EINVAL);
    }
    let entries = usize::try_from(batch.entries).map_err(|_| EFAULT)?;

    let mut src_buf = KVec::from_elem(0u8, NAME_BUF, GFP_KERNEL)?;
    let mut dst_buf = KVec::from_elem(0u8, NAME_BUF, GFP_KERNEL)?;

    let mut done: u32 = 0;
    while done < batch.count {
        if kernel::current!().signal_pending() {
            break;
        }
        let at = (done as usize)
            .checked_mul(size_of::<CopyEntry>())
            .and_then(|offset| entries.checked_add(offset))
            .ok_or(EFAULT)?;
        let entry: CopyEntry = UserSlice::new(UserPtr::from_addr(at), size_of::<CopyEntry>())
            .reader()
            .read()?;
        let result = match copy_one(&entry, &mut src_buf, &mut dst_buf) {
            Ok(bytes) => bytes,
            Err(err) => i64::from(err.to_errno()),
        };
        let result_at = at
            .checked_add(offset_of!(CopyEntry, result))
            .ok_or(EFAULT)?;
        UserSlice::new(UserPtr::from_addr(result_at), size_of::<i64>())
            .writer()
            .write(&result)?;
        done += 1;
    }

    let done_at = arg.checked_add(offset_of!(Batch, done)).ok_or(EFAULT)?;
    UserSlice::new(UserPtr::from_addr(done_at), size_of::<u32>())
        .writer()
        .write(&done)?;
    if done == 0 && batch.count > 0 {
        return Err(EINTR);
    }
    Ok(0)
}

struct BunAccel;

#[vtable]
impl MiscDevice for BunAccel {
    type Ptr = ();

    fn open(_file: &File, _misc: &MiscDeviceRegistration<Self>) -> Result<()> {
        Ok(())
    }

    fn ioctl(_device: (), _file: &File, cmd: u32, arg: usize) -> Result<isize> {
        match cmd {
            IOC_COPY_BATCH => copy_batch(arg),
            _ => Err(ENOTTY),
        }
    }
}

#[pin_data]
struct BunAccelModule {
    #[pin]
    _miscdev: MiscDeviceRegistration<BunAccel>,
}

impl kernel::InPlaceModule for BunAccelModule {
    fn init(_module: &'static ThisModule) -> impl PinInit<Self, Error> {
        let options = MiscDeviceOptions {
            name: c_str!("bun_accel"),
        };

        try_pin_init!(Self {
            _miscdev <- MiscDeviceRegistration::register(options),
        })
    }
}
