//! Prevents automatic system sleep while a GUI transfer is active.

/// A platform-specific inhibitor, active for this value's lifetime.
///
/// Failure to acquire an inhibitor never interrupts a transfer. It is a convenience feature and
/// can be unavailable, for example when a Linux desktop does not use systemd-logind.
#[cfg(target_os = "macos")]
pub struct SleepInhibitor {
    assertion_id: Option<u32>,
}

#[cfg(target_os = "windows")]
pub struct SleepInhibitor {
    active: bool,
}

#[cfg(target_os = "linux")]
pub struct SleepInhibitor {
    /// The logind inhibitor fd; closing it (on drop or process exit) releases the lock.
    _lock: Option<zbus::zvariant::OwnedFd>,
}

#[cfg(not(any(target_os = "macos", target_os = "windows", target_os = "linux")))]
pub struct SleepInhibitor;

#[cfg(target_os = "macos")]
impl SleepInhibitor {
    /// Creates the macOS idle-system-sleep assertion when the preference is enabled.
    pub fn new(enabled: bool) -> Self {
        if enabled {
            macos::create()
        } else {
            Self { assertion_id: None }
        }
    }
}

#[cfg(target_os = "macos")]
impl Drop for SleepInhibitor {
    fn drop(&mut self) {
        if let Some(assertion_id) = self.assertion_id.take() {
            // SAFETY: `assertion_id` was returned by IOPMAssertionCreateWithName and has not yet
            // been released.
            unsafe {
                macos::release(assertion_id);
            }
        }
    }
}

#[cfg(target_os = "windows")]
impl SleepInhibitor {
    /// Sets the execution state on the batch controller thread when the preference is enabled.
    pub fn new(enabled: bool) -> Self {
        let active = enabled
            && unsafe {
                // SAFETY: SetThreadExecutionState has no pointer arguments and is valid on this
                // controller thread. It is cleared by this same thread in `Drop`.
                windows::set_execution_state(windows::ES_CONTINUOUS | windows::ES_SYSTEM_REQUIRED)
                    != 0
            };
        Self { active }
    }
}

#[cfg(target_os = "windows")]
impl Drop for SleepInhibitor {
    fn drop(&mut self) {
        if self.active {
            // SAFETY: this runs on the same controller thread that set the requirement.
            unsafe {
                windows::set_execution_state(windows::ES_CONTINUOUS);
            }
        }
    }
}

#[cfg(target_os = "linux")]
impl SleepInhibitor {
    /// Takes a `sleep` inhibitor lock from systemd-logind over the system D-Bus.
    ///
    /// logind returns a file descriptor; the lock lasts as long as it stays open, so the kernel
    /// releases it when this process ends for any reason (crash, SIGKILL, forced quit).
    pub fn new(enabled: bool) -> Self {
        let lock = if enabled {
            match linux::inhibit_sleep() {
                Ok(fd) => Some(fd),
                Err(error) => {
                    eprintln!("Sleep inhibitor unavailable (systemd-logind): {error}");
                    None
                }
            }
        } else {
            None
        };
        Self { _lock: lock }
    }
}

#[cfg(not(any(target_os = "macos", target_os = "windows", target_os = "linux")))]
impl SleepInhibitor {
    pub fn new(_enabled: bool) -> Self {
        Self
    }
}

#[cfg(target_os = "macos")]
mod macos {
    use std::ffi::{c_char, c_void};
    use std::ptr;

    use super::SleepInhibitor;

    type CFStringRef = *const c_void;

    const K_CF_STRING_ENCODING_UTF8: u32 = 0x0800_0100;
    const K_IOPM_ASSERTION_LEVEL_ON: u32 = 255;
    const K_IO_RETURN_SUCCESS: i32 = 0;

    #[link(name = "CoreFoundation", kind = "framework")]
    unsafe extern "C" {
        fn CFStringCreateWithCString(
            allocator: *const c_void,
            source: *const c_char,
            encoding: u32,
        ) -> CFStringRef;
        fn CFRelease(value: *const c_void);
    }

    #[link(name = "IOKit", kind = "framework")]
    unsafe extern "C" {
        fn IOPMAssertionCreateWithName(
            assertion_type: CFStringRef,
            assertion_level: u32,
            assertion_name: CFStringRef,
            assertion_id: *mut u32,
        ) -> i32;
        fn IOPMAssertionRelease(assertion_id: u32) -> i32;
    }

    pub(super) fn create() -> SleepInhibitor {
        // Both strings are valid UTF-8 C strings. The references are only needed while creating
        // the assertion, and are released immediately afterwards.
        let assertion_type = unsafe {
            CFStringCreateWithCString(
                ptr::null(),
                c"PreventUserIdleSystemSleep".as_ptr(),
                K_CF_STRING_ENCODING_UTF8,
            )
        };
        let assertion_name = unsafe {
            CFStringCreateWithCString(
                ptr::null(),
                c"StreamExtract transfer".as_ptr(),
                K_CF_STRING_ENCODING_UTF8,
            )
        };
        if assertion_type.is_null() || assertion_name.is_null() {
            // SAFETY: Core Foundation accepts the non-null objects created above.
            unsafe {
                if !assertion_type.is_null() {
                    CFRelease(assertion_type);
                }
                if !assertion_name.is_null() {
                    CFRelease(assertion_name);
                }
            }
            return SleepInhibitor { assertion_id: None };
        }

        let mut assertion_id = 0;
        // SAFETY: both Core Foundation string references are valid for this call and the output
        // points to writable storage.
        let result = unsafe {
            IOPMAssertionCreateWithName(
                assertion_type,
                K_IOPM_ASSERTION_LEVEL_ON,
                assertion_name,
                &mut assertion_id,
            )
        };
        // SAFETY: the assertion API has copied the strings it needs; both references are live.
        unsafe {
            CFRelease(assertion_type);
            CFRelease(assertion_name);
        }

        SleepInhibitor {
            assertion_id: (result == K_IO_RETURN_SUCCESS).then_some(assertion_id),
        }
    }

    pub(super) unsafe fn release(assertion_id: u32) {
        // SAFETY: the caller guarantees that this is a live IOKit assertion ID.
        unsafe {
            IOPMAssertionRelease(assertion_id);
        }
    }
}

#[cfg(target_os = "linux")]
mod linux {
    use zbus::blocking::Connection;
    use zbus::zvariant::OwnedFd;

    /// Calls `org.freedesktop.login1.Manager.Inhibit("sleep", ..., "block")` and returns the
    /// lock file descriptor.
    pub(super) fn inhibit_sleep() -> zbus::Result<OwnedFd> {
        let connection = Connection::system()?;
        let reply = connection.call_method(
            Some("org.freedesktop.login1"),
            "/org/freedesktop/login1",
            Some("org.freedesktop.login1.Manager"),
            "Inhibit",
            &(
                "sleep",
                "StreamExtract",
                "Archive transfer in progress",
                "block",
            ),
        )?;
        reply.body().deserialize::<OwnedFd>()
    }
}

#[cfg(target_os = "windows")]
mod windows {
    pub(super) const ES_SYSTEM_REQUIRED: u32 = 0x0000_0001;
    pub(super) const ES_CONTINUOUS: u32 = 0x8000_0000;

    #[link(name = "Kernel32")]
    unsafe extern "system" {
        fn SetThreadExecutionState(flags: u32) -> u32;
    }

    pub(super) unsafe fn set_execution_state(flags: u32) -> u32 {
        // SAFETY: forwarded from the platform-specific caller with valid execution-state flags.
        unsafe { SetThreadExecutionState(flags) }
    }
}
