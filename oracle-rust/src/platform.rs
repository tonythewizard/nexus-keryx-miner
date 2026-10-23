use std::{fs::{File, OpenOptions}, path::Path};

pub fn open_model(path: &Path) -> std::io::Result<File> {
    let mut options = OpenOptions::new();
    options.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.custom_flags(libc::O_CLOEXEC | libc::O_NOATIME);
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        use windows_sys::Win32::Storage::FileSystem::{FILE_SHARE_READ, FILE_WRITE_ATTRIBUTES};
        options.access_mode(0x80000000 | FILE_WRITE_ATTRIBUTES).share_mode(FILE_SHARE_READ);
    }
    let file = options.open(path)?;
    #[cfg(windows)]
    {
        use std::os::windows::io::AsRawHandle;
        use windows_sys::Win32::{Foundation::FILETIME, Storage::FileSystem::SetFileTime};
        let unchanged = FILETIME { dwLowDateTime: u32::MAX, dwHighDateTime: u32::MAX };
        if unsafe { SetFileTime(file.as_raw_handle(), std::ptr::null(), &unchanged, std::ptr::null()) } == 0 {
            return Err(std::io::Error::last_os_error());
        }
    }
    Ok(file)
}

#[cfg(unix)]
pub fn cpu_ns() -> u64 {
    let mut usage: libc::rusage = unsafe { std::mem::zeroed() };
    assert_eq!(unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut usage) }, 0);
    ((usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) as u64) * 1_000_000_000
        + ((usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) as u64) * 1_000
}

#[cfg(windows)]
pub fn cpu_ns() -> u64 {
    use windows_sys::Win32::{Foundation::FILETIME, System::Threading::{GetCurrentProcess, GetProcessTimes}};
    let mut created: FILETIME = unsafe { std::mem::zeroed() };
    let mut exited = created;
    let mut kernel = created;
    let mut user = created;
    assert_ne!(unsafe { GetProcessTimes(GetCurrentProcess(), &mut created, &mut exited, &mut kernel, &mut user) }, 0,
        "process CPU measurement failed");
    let ticks = |t: FILETIME| (u64::from(t.dwHighDateTime) << 32) | u64::from(t.dwLowDateTime);
    (ticks(kernel) + ticks(user)) * 100
}
