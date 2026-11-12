//! OS integration for the desktop; no mining or process lifecycle operations.
use std::path::{Path, PathBuf};
use tauri::Manager;

pub fn paths(app: &tauri::AppHandle) -> Result<(PathBuf, PathBuf, PathBuf), String> {
    let source = match std::env::var_os("NEXUS_SOURCE_DIR") {
        Some(value) => PathBuf::from(value),
        None => app
            .path()
            .resource_dir()
            .map_err(|_| "Application resources unavailable")?
            .join("bridge"),
    };
    let root = match std::env::var_os("NEXUS_DATA_DIR") {
        Some(value) => PathBuf::from(value),
        None => {
            #[cfg(windows)]
            let path =
                PathBuf::from(std::env::var_os("LOCALAPPDATA").ok_or("LOCALAPPDATA is required")?)
                    .join("nexus-keryx-miner");
            #[cfg(not(windows))]
            let path = app
                .path()
                .app_local_data_dir()
                .map_err(|_| "Application data directory unavailable")?;
            path
        }
    };
    if !source.is_absolute()
        || !root.is_absolute()
        || !source.join("scripts/nexus_tauri_bridge.py").is_file()
    {
        return Err("Desktop resources or data path invalid".into());
    }
    std::fs::create_dir_all(&root).map_err(|_| "Application data directory unavailable")?;
    let python = python(&source)?;
    Ok((source, root, python))
}

pub fn python(_source: &Path) -> Result<PathBuf, String> {
    if let Some(value) = std::env::var_os("NEXUS_PYTHON") {
        let path = PathBuf::from(value);
        if path.is_absolute() && path.is_file() {
            return Ok(path);
        }
        return Err("NEXUS_PYTHON must name an existing absolute interpreter path".into());
    }
    #[cfg(windows)]
    let path = _source.join("python/python.exe");
    #[cfg(not(windows))]
    let path = PathBuf::from("/usr/bin/python3");
    if !path.is_file() {
        return Err("Desktop Python runtime missing; reinstall the desktop package".into());
    }
    Ok(path)
}

#[cfg(target_os = "linux")]
pub async fn clipboard(_app: &tauri::AppHandle, text: String) -> Result<(), String> {
    let connection = zbus::Connection::session()
        .await
        .map_err(|_| "Clipboard unavailable: check KDE Klipper")?;
    let proxy = zbus::Proxy::new(
        &connection,
        "org.kde.klipper",
        "/klipper",
        "org.kde.klipper.klipper",
    )
    .await
    .map_err(|_| "Clipboard unavailable: check KDE Klipper")?;
    proxy
        .call::<_, _, ()>("setClipboardContents", &(text,))
        .await
        .map_err(|_| "Clipboard unavailable: check KDE Klipper".into())
}

#[cfg(windows)]
pub async fn clipboard(_app: &tauri::AppHandle, text: String) -> Result<(), String> {
    use windows_sys::Win32::{
        Foundation::*,
        System::{DataExchange::*, Memory::*},
    };
    // Clipboard ownership transfers only after SetClipboardData succeeds.
    unsafe {
        let owner = _app
            .get_webview_window("main")
            .ok_or("Desktop window unavailable")?
            .hwnd()
            .map_err(|_| "Desktop window unavailable")?
            .0;
        if OpenClipboard(owner) == 0 {
            return Err("Clipboard is busy".into());
        }
        struct Close;
        impl Drop for Close {
            fn drop(&mut self) {
                unsafe {
                    CloseClipboard();
                }
            }
        }
        let _close = Close;
        let encoded: Vec<u16> = text.encode_utf16().chain(Some(0)).collect();
        let memory = GlobalAlloc(GMEM_MOVEABLE, encoded.len() * 2);
        if memory.is_null() {
            return Err("Clipboard allocation failed".into());
        }
        let target = GlobalLock(memory) as *mut u16;
        if target.is_null() {
            GlobalFree(memory);
            return Err("Clipboard allocation failed".into());
        }
        std::ptr::copy_nonoverlapping(encoded.as_ptr(), target, encoded.len());
        GlobalUnlock(memory);
        if EmptyClipboard() == 0 || SetClipboardData(13, memory).is_null() {
            GlobalFree(memory);
            return Err("Clipboard unavailable".into());
        }
    }
    Ok(())
}

#[cfg(not(any(windows, target_os = "linux")))]
pub async fn clipboard(_app: &tauri::AppHandle, _text: String) -> Result<(), String> {
    Err("Clipboard not supported on this platform".into())
}

pub fn open_api() -> Result<(), String> {
    #[cfg(windows)]
    {
        use windows_sys::Win32::UI::Shell::ShellExecuteW;
        let verb: Vec<u16> = "open\0".encode_utf16().collect();
        let url: Vec<u16> = "http://127.0.0.1:3340/\0".encode_utf16().collect();
        let result = unsafe {
            ShellExecuteW(
                std::ptr::null_mut(),
                verb.as_ptr(),
                url.as_ptr(),
                std::ptr::null(),
                std::ptr::null(),
                1,
            )
        };
        if result as isize <= 32 {
            return Err("Browser unavailable".into());
        }
        Ok(())
    }
    #[cfg(not(windows))]
    {
        std::process::Command::new("xdg-open")
            .arg("http://127.0.0.1:3340/")
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn()
            .map_err(|_| "Browser unavailable".to_string())?;
        Ok(())
    }
}
