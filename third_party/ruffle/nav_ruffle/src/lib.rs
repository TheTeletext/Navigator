//! nav_ruffle: a thin C ABI over Ruffle for Teletext Navigator's Retro Web
//! Browser tab.
//!
//! The C side owns a child HWND; this library owns one Ruffle Player per
//! embed, renders it into that HWND through wgpu, and plays its sound through
//! cpal. Every call happens on the thread that created the embed (the GUI
//! thread). No call may unwind into C: each export runs under catch_unwind,
//! and an embed that panicked is marked poisoned and only ever freed.
//!
//! The SWF bytes are handed in by the caller, which has already fetched them.
//! Ruffle itself is given a navigator that refuses every fetch, socket and
//! page navigation, so a movie can neither reach the network nor read local
//! files.
//!
//! Copyright (C) 2026 Dimitri Papadopoulos and The Teletext.
//! Licensed under the MIT license or the Apache License 2.0, at your option,
//! the same terms as Ruffle. The audio backend below is adapted from Ruffle's
//! frontend-utils/src/backends/audio.rs (Ruffle LLC, MIT OR Apache-2.0).

use std::ffi::{CStr, c_char, c_void};
use std::num::NonZeroIsize;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use ruffle_core::backend::audio::{
    AudioBackend, AudioMixer, DecodeError, RegisterError, SoundHandle, SoundInstanceHandle,
    SoundStreamInfo, SoundTransform, swf,
};
use ruffle_core::backend::navigator::{
    ErrorResponse, NavigationMethod, NavigatorBackend, NullExecutor, NullSpawner, OwnedFuture,
    Request, SuccessResponse, async_return, create_specific_fetch_error,
};
use ruffle_core::indexmap::IndexMap;
use ruffle_core::events::{
    KeyDescriptor, KeyLocation, LogicalKey, MouseButton, MouseWheelDelta, NamedKey, PhysicalKey,
};
use ruffle_core::loader::Error as LoaderError;
use ruffle_core::socket::{ConnectionState, SocketAction, SocketHandle};
use ruffle_core::tag_utils::SwfMovieData;
use ruffle_core::{FloatDuration, Player, PlayerBuilder, PlayerEvent, ViewportDimensions};
use ruffle_core::impl_audio_mixer_backend;
use ruffle_render_wgpu::backend::{WgpuRenderBackend, create_wgpu_instance, request_adapter_and_device};
use ruffle_render_wgpu::descriptors::Descriptors;
use ruffle_render_wgpu::target::SwapChainTarget;
use ruffle_render_wgpu::utils::supported_sample_count;
use url::Url;

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};

/// A script may run this long in one go before Ruffle stops it (AVM1 then
/// halts for good; AVM2 only abandons that one script).
const SCRIPT_LIMIT: Duration = Duration::from_millis(1000);

/// Any single call into the player that takes longer than this poisons the
/// embed, so that a movie whose every frame runs into SCRIPT_LIMIT (an AS3
/// enterFrame loop, say) cannot keep the GUI thread stalled. The C side then
/// tears it down and the page shows the movie's fallback content.
const CALL_LIMIT: Duration = Duration::from_millis(1500);

/// Largest uncompressed SWF accepted. Ruffle inflates a compressed SWF up to
/// the length its header declares (up to 4 GB), and running out of memory
/// aborts the process, so a "zip bomb" is refused before Ruffle sees it.
const MAX_UNCOMPRESSED: u32 = 128 * 1024 * 1024;

/// Refuse anything that is not an FWS/CWS/ZWS header within the size limit.
fn check_header(swf: &[u8]) -> Result<(), String> {
    if swf.len() < 8 {
        return Err("not a Flash movie (too short)".into());
    }
    if !matches!(&swf[0..3], b"FWS" | b"CWS" | b"ZWS") {
        return Err("not a Flash movie".into());
    }
    let declared = u32::from_le_bytes([swf[4], swf[5], swf[6], swf[7]]);
    if declared > MAX_UNCOMPRESSED {
        return Err(format!("the movie declares {declared} bytes, more than the limit"));
    }
    Ok(())
}

/// Bumped whenever an exported signature changes. The C side refuses a DLL
/// whose version differs from the one it was built against.
pub const NAVRFL_ABI_VERSION: u32 = 1;

/// navrfl_create flags.
pub const NAVRFL_FLAG_NO_AUDIO: u32 = 1;

/* ------------------------------------------------------------------ */
/* Audio (adapted from Ruffle frontend-utils, MIT OR Apache-2.0).      */
/* ------------------------------------------------------------------ */

struct CpalAudio {
    stream: cpal::Stream,
    mixer: AudioMixer,
}

impl CpalAudio {
    fn new() -> Result<Self, String> {
        let host = cpal::default_host();
        let device = host
            .default_output_device()
            .ok_or_else(|| "no audio output device".to_string())?;
        let config = device
            .default_output_config()
            .map_err(|e| format!("audio config: {e}"))?;
        let sample_format = config.sample_format();
        let config = cpal::StreamConfig::from(config);
        let mixer = AudioMixer::new(config.channels as u8, config.sample_rate.0);
        let proxy = mixer.proxy();
        let on_err = |_e| {};
        let stream = match sample_format {
            cpal::SampleFormat::F32 => device.build_output_stream(
                &config,
                move |buf: &mut [f32], _| proxy.mix::<f32>(buf),
                on_err,
                None,
            ),
            cpal::SampleFormat::I16 => device.build_output_stream(
                &config,
                move |buf: &mut [i16], _| proxy.mix::<i16>(buf),
                on_err,
                None,
            ),
            other => return Err(format!("unsupported sample format {other:?}")),
        }
        .map_err(|e| format!("audio stream: {e}"))?;
        stream.play().map_err(|e| format!("audio play: {e}"))?;
        Ok(Self { stream, mixer })
    }
}

impl AudioBackend for CpalAudio {
    impl_audio_mixer_backend!(mixer);

    fn play(&mut self) {
        let _ = self.stream.play();
    }

    fn pause(&mut self) {
        let _ = self.stream.pause();
    }
}

/* ------------------------------------------------------------------ */
/* Navigator: refuses everything.                                     */
/* ------------------------------------------------------------------ */

struct RefusingNavigator {
    spawner: NullSpawner,
}

impl NavigatorBackend for RefusingNavigator {
    fn navigate_to_url(
        &self,
        _url: &str,
        _target: &str,
        _vars_method: Option<(NavigationMethod, IndexMap<String, String>)>,
    ) {
    }

    fn fetch(&self, request: Request) -> OwnedFuture<Box<dyn SuccessResponse>, ErrorResponse> {
        let url = request.url().to_string();
        async_return(Err(create_specific_fetch_error(
            "Navigator does not load files from a movie:",
            &url,
            "",
        )))
    }

    fn resolve_url(&self, url: &str) -> Result<Url, url::ParseError> {
        Url::parse(url)
    }

    fn spawn_future(&mut self, future: OwnedFuture<(), LoaderError>) {
        self.spawner.spawn_local(future);
    }

    fn pre_process_url(&self, url: Url) -> Url {
        url
    }

    fn connect_socket(
        &mut self,
        _host: String,
        _port: u16,
        _timeout: Duration,
        handle: SocketHandle,
        _receiver: async_channel::Receiver<Vec<u8>>,
        sender: async_channel::Sender<SocketAction>,
    ) {
        let _ = sender.try_send(SocketAction::Connect(handle, ConnectionState::Failed));
    }
}

/* ------------------------------------------------------------------ */
/* One graphics device per session, shared by every embed.            */
/* ------------------------------------------------------------------ */

const GWLP_HINSTANCE: i32 = -6;
unsafe extern "system" {
    fn GetWindowLongPtrW(hwnd: isize, index: i32) -> isize;
}

#[derive(Debug)]
struct WinDisplay;
impl wgpu::rwh::HasDisplayHandle for WinDisplay {
    fn display_handle(&self) -> Result<wgpu::rwh::DisplayHandle<'_>, wgpu::rwh::HandleError> {
        Ok(wgpu::rwh::DisplayHandle::windows())
    }
}

static SHARED: Mutex<Option<Arc<Descriptors>>> = Mutex::new(None);

/// The graphics APIs tried, in order. (flags >> 4) & 7 forces one: 1 DX12,
/// 2 Vulkan, 3 OpenGL. 0 tries DX12, then Vulkan, then OpenGL.
fn backend_order(flags: u32) -> Vec<wgpu::Backends> {
    match (flags >> 4) & 7 {
        1 => vec![wgpu::Backends::DX12],
        2 => vec![wgpu::Backends::VULKAN],
        3 => vec![wgpu::Backends::GL],
        _ => vec![wgpu::Backends::DX12, wgpu::Backends::VULKAN, wgpu::Backends::GL],
    }
}

fn shared_descriptors(flags: u32) -> Result<Arc<Descriptors>, String> {
    let mut slot = SHARED.lock().map_err(|_| "graphics device lock poisoned".to_string())?;
    if let Some(d) = slot.as_ref() {
        return Ok(d.clone());
    }
    let mut why = Vec::new();
    for b in backend_order(flags) {
        let instance = create_wgpu_instance(b, wgpu::BackendOptions::default(), Some(Box::new(WinDisplay)));
        match futures::executor::block_on(request_adapter_and_device(
            b,
            &instance,
            None,
            wgpu::PowerPreference::LowPower,
        )) {
            Ok((adapter, device, queue)) => {
                let d = Arc::new(Descriptors::new(instance, adapter, device, queue));
                // Compile the pipelines the first frame will ask for: the
                // backend starts at Low quality (no MSAA) and the player then
                // switches to High (4x where supported). Windows swapchains
                // report Bgra8Unorm first; another format just compiles on
                // first use instead.
                let fmt = wgpu::TextureFormat::Bgra8Unorm;
                let msaa = supported_sample_count(&d.adapter, 4, fmt);
                let _ = d.pipelines(1, fmt);
                let _ = d.pipelines(msaa, fmt);
                let _ = d.copy_pipeline(fmt, 1);
                let _ = d.copy_pipeline(fmt, msaa);
                *slot = Some(d.clone());
                return Ok(d);
            }
            Err(e) => why.push(format!("{b:?}: {e}")),
        }
    }
    Err(format!("no usable graphics device ({})", why.join("; ")))
}

/* ------------------------------------------------------------------ */
/* The embed.                                                         */
/* ------------------------------------------------------------------ */

pub struct Embed {
    player: Option<Arc<Mutex<Player>>>,
    executor: NullExecutor,
    last_tick: Instant,
    poisoned: bool,
}

fn write_err(err: *mut c_char, errlen: usize, msg: &str) {
    if err.is_null() || errlen == 0 {
        return;
    }
    let bytes = msg.as_bytes();
    let n = bytes.len().min(errlen - 1);
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), err as *mut u8, n);
        *err.add(n) = 0;
    }
}

fn panic_text(p: &(dyn std::any::Any + Send)) -> String {
    if let Some(s) = p.downcast_ref::<&str>() {
        (*s).to_string()
    } else if let Some(s) = p.downcast_ref::<String>() {
        s.clone()
    } else {
        "panic".to_string()
    }
}

unsafe fn create_inner(
    hwnd: *mut c_void,
    width: u32,
    height: u32,
    swf: &[u8],
    url: &str,
    flags: u32,
) -> Result<Box<Embed>, String> {
    let hwnd = NonZeroIsize::new(hwnd as isize).ok_or("null window")?;
    let width = width.max(1);
    let height = height.max(1);

    check_header(swf)?;
    let movie = SwfMovieData::from_data(swf, url.to_string(), None, None)
        .map_err(|e| format!("not a playable SWF: {e}"))?;

    let descriptors = shared_descriptors(flags)?;
    let mut window = wgpu::rwh::Win32WindowHandle::new(hwnd);
    window.hinstance = NonZeroIsize::new(unsafe { GetWindowLongPtrW(hwnd.get(), GWLP_HINSTANCE) });
    let target = wgpu::SurfaceTargetUnsafe::RawHandle {
        raw_display_handle: Some(wgpu::rwh::RawDisplayHandle::Windows(
            wgpu::rwh::WindowsDisplayHandle::new(),
        )),
        raw_window_handle: wgpu::rwh::RawWindowHandle::Win32(window),
    };
    let surface = unsafe { descriptors.wgpu_instance.create_surface_unsafe(target) }
        .map_err(|e| format!("graphics surface: {e}"))?;
    let target = SwapChainTarget::new(surface, &descriptors.adapter, (width, height), &descriptors.device);
    let renderer = WgpuRenderBackend::new(descriptors, target)
        .map_err(|e| format!("graphics: {e}"))?;

    let executor = NullExecutor::new();
    let navigator = RefusingNavigator {
        spawner: executor.spawner(),
    };

    let mut builder = PlayerBuilder::new()
        .with_renderer(renderer)
        .with_navigator(navigator)
        .with_video(ruffle_video_software::backend::SoftwareVideoBackend::new())
        .with_movie(movie)
        .with_page_url(Some(url.to_string()))
        .with_viewport_dimensions(width, height, 1.0)
        .with_autoplay(true)
        .with_max_execution_duration(SCRIPT_LIMIT);
    if flags & NAVRFL_FLAG_NO_AUDIO == 0 {
        if let Ok(audio) = CpalAudio::new() {
            builder = builder.with_audio(audio);
        }
    }
    let player = builder.build();

    Ok(Box::new(Embed {
        player: Some(player),
        executor,
        last_tick: Instant::now(),
        poisoned: false,
    }))
}

/// ABI version of this DLL.
#[unsafe(no_mangle)]
pub extern "C" fn navrfl_abi_version() -> u32 {
    NAVRFL_ABI_VERSION
}

/// Create an embed that plays `swf` into `hwnd` at `width` x `height`.
/// Returns NULL on failure with a reason in `err` (UTF-8, NUL-terminated).
///
/// # Safety
/// `hwnd` must be a live window owned by the calling thread that outlives the
/// embed. `swf` must point at `swf_len` readable bytes (copied before return).
/// `url` must be NUL-terminated UTF-8. `err` must hold `errlen` bytes or be
/// NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn navrfl_create(
    hwnd: *mut c_void,
    width: u32,
    height: u32,
    swf: *const u8,
    swf_len: usize,
    url: *const c_char,
    flags: u32,
    err: *mut c_char,
    errlen: usize,
) -> *mut Embed {
    if swf.is_null() || swf_len == 0 || url.is_null() {
        write_err(err, errlen, "bad arguments");
        return std::ptr::null_mut();
    }
    let swf = unsafe { std::slice::from_raw_parts(swf, swf_len) };
    let url = match unsafe { CStr::from_ptr(url) }.to_str() {
        Ok(s) => s,
        Err(_) => {
            write_err(err, errlen, "url is not UTF-8");
            return std::ptr::null_mut();
        }
    };
    match catch_unwind(AssertUnwindSafe(|| unsafe {
        create_inner(hwnd, width, height, swf, url, flags)
    })) {
        Ok(Ok(e)) => Box::into_raw(e),
        Ok(Err(msg)) => {
            write_err(err, errlen, &msg);
            std::ptr::null_mut()
        }
        Err(p) => {
            write_err(err, errlen, &format!("player panicked: {}", panic_text(&*p)));
            std::ptr::null_mut()
        }
    }
}

/// Free an embed: stops its sound and releases its surface. Safe on NULL.
///
/// # Safety
/// `e` must come from navrfl_create and not be used again. The window passed
/// to navrfl_create must still exist.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn navrfl_destroy(e: *mut Embed) {
    if e.is_null() {
        return;
    }
    let boxed = unsafe { Box::from_raw(e) };
    if catch_unwind(AssertUnwindSafe(move || drop(boxed))).is_err() {
        // A panicking drop has already unwound out of the player; nothing
        // further can be released safely.
    }
}

/// Run `f` against a live, unpoisoned player. Returns None if the embed is
/// poisoned or panicked (and poisons it).
fn with_player<R>(e: *mut Embed, f: impl FnOnce(&mut Embed, &mut Player) -> R) -> Option<R> {
    if e.is_null() {
        return None;
    }
    let embed = unsafe { &mut *e };
    if embed.poisoned {
        return None;
    }
    let player = embed.player.as_ref()?.clone();
    let started = Instant::now();
    let r = catch_unwind(AssertUnwindSafe(|| {
        let mut guard = player.lock().ok()?;
        Some(f(embed, &mut guard))
    }));
    match r {
        Ok(Some(_)) if started.elapsed() > CALL_LIMIT => {
            embed.poisoned = true;
            None
        }
        Ok(Some(v)) => Some(v),
        _ => {
            embed.poisoned = true;
            None
        }
    }
}

/// Advance the movie by the wall time since the last tick and draw it if it
/// changed. Returns the milliseconds until the next tick is due (1..=100), or
/// -1 if the embed is dead and must be destroyed.
///
/// # Safety
/// `e` must be a live embed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn navrfl_tick(e: *mut Embed) -> i32 {
    let r = with_player(e, |embed, p| {
        let now = Instant::now();
        let dt = now.duration_since(embed.last_tick).min(Duration::from_millis(250));
        embed.last_tick = now;
        p.tick(FloatDuration::from_std(dt));
        if p.needs_render() {
            p.render();
        }
        p.time_til_next_frame().as_millis().clamp(1, 100) as i32
    });
    if let Some(ms) = r {
        let embed = unsafe { &mut *e };
        let _ = catch_unwind(AssertUnwindSafe(|| embed.executor.run()));
        ms
    } else {
        -1
    }
}

/// Draw the current frame again (after WM_PAINT). Returns 0, or -1 if dead.
///
/// # Safety
/// `e` must be a live embed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn navrfl_render(e: *mut Embed) -> i32 {
    with_player(e, |_, p| p.render()).map_or(-1, |_| 0)
}

/// Resize the drawing surface. Returns 0, or -1 if dead.
///
/// # Safety
/// `e` must be a live embed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn navrfl_resize(e: *mut Embed, width: u32, height: u32) -> i32 {
    with_player(e, |_, p| {
        p.set_viewport_dimensions(ViewportDimensions {
            width: width.max(1),
            height: height.max(1),
            scale_factor: 1.0,
        });
        p.render();
    })
    .map_or(-1, |_| 0)
}

/// Pause (1) or resume (0) the movie and its sound.
///
/// # Safety
/// `e` must be a live embed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn navrfl_set_paused(e: *mut Embed, paused: i32) -> i32 {
    with_player(e, |embed, p| {
        p.set_is_playing(paused == 0);
        embed.last_tick = Instant::now();
    })
    .map_or(-1, |_| 0)
}

/// Mouse kinds for navrfl_mouse.
pub const NAVRFL_MOUSE_MOVE: i32 = 0;
pub const NAVRFL_MOUSE_DOWN: i32 = 1;
pub const NAVRFL_MOUSE_UP: i32 = 2;
pub const NAVRFL_MOUSE_LEAVE: i32 = 3;
pub const NAVRFL_MOUSE_WHEEL: i32 = 4;

/// Forward a mouse event in client pixels. `button`: 0 left, 1 right,
/// 2 middle. For MOUSE_WHEEL, `button` carries the wheel delta in lines
/// times 120 (WHEEL_DELTA units). Returns 0, or -1 if dead.
///
/// # Safety
/// `e` must be a live embed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn navrfl_mouse(e: *mut Embed, kind: i32, x: i32, y: i32, button: i32) -> i32 {
    let (x, y) = (x as f64, y as f64);
    let b = match button {
        0 => MouseButton::Left,
        1 => MouseButton::Right,
        2 => MouseButton::Middle,
        _ => MouseButton::Unknown,
    };
    let ev = match kind {
        NAVRFL_MOUSE_MOVE => PlayerEvent::MouseMove { x, y },
        NAVRFL_MOUSE_DOWN => PlayerEvent::MouseDown { x, y, button: b, index: None },
        NAVRFL_MOUSE_UP => PlayerEvent::MouseUp { x, y, button: b },
        NAVRFL_MOUSE_LEAVE => PlayerEvent::MouseLeave,
        NAVRFL_MOUSE_WHEEL => PlayerEvent::MouseWheel {
            delta: MouseWheelDelta::Lines(button as f64 / 120.0),
        },
        _ => return 0,
    };
    with_player(e, |_, p| {
        // As Ruffle's own front ends do (desktop/src/app.rs CursorEntered /
        // CursorLeft): the stage must know whether the pointer is in it,
        // or buttons never roll out when the pointer leaves the movie.
        match kind {
            NAVRFL_MOUSE_LEAVE => p.set_mouse_in_stage(false),
            NAVRFL_MOUSE_MOVE | NAVRFL_MOUSE_DOWN => {
                if !p.mouse_in_stage() {
                    p.set_mouse_in_stage(true);
                }
            }
            _ => {}
        }
        p.handle_event(ev);
    })
    .map_or(-1, |_| 0)
}

fn vk_to_physical(vk: u32, extended: bool) -> PhysicalKey {
    use PhysicalKey as P;
    match vk {
        0x08 => P::Backspace,
        0x09 => P::Tab,
        0x0D => if extended { P::NumpadEnter } else { P::Enter },
        0x10 | 0xA0 => P::ShiftLeft,
        0xA1 => P::ShiftRight,
        0x11 | 0xA2 => if extended { P::ControlRight } else { P::ControlLeft },
        0xA3 => P::ControlRight,
        0x12 | 0xA4 => if extended { P::AltRight } else { P::AltLeft },
        0xA5 => P::AltRight,
        0x13 => P::Pause,
        0x14 => P::CapsLock,
        0x1B => P::Escape,
        0x20 => P::Space,
        0x21 => P::PageUp,
        0x22 => P::PageDown,
        0x23 => P::End,
        0x24 => P::Home,
        0x25 => P::ArrowLeft,
        0x26 => P::ArrowUp,
        0x27 => P::ArrowRight,
        0x28 => P::ArrowDown,
        0x2D => P::Insert,
        0x2E => P::Delete,
        0x30 => P::Digit0, 0x31 => P::Digit1, 0x32 => P::Digit2, 0x33 => P::Digit3,
        0x34 => P::Digit4, 0x35 => P::Digit5, 0x36 => P::Digit6, 0x37 => P::Digit7,
        0x38 => P::Digit8, 0x39 => P::Digit9,
        0x41 => P::KeyA, 0x42 => P::KeyB, 0x43 => P::KeyC, 0x44 => P::KeyD,
        0x45 => P::KeyE, 0x46 => P::KeyF, 0x47 => P::KeyG, 0x48 => P::KeyH,
        0x49 => P::KeyI, 0x4A => P::KeyJ, 0x4B => P::KeyK, 0x4C => P::KeyL,
        0x4D => P::KeyM, 0x4E => P::KeyN, 0x4F => P::KeyO, 0x50 => P::KeyP,
        0x51 => P::KeyQ, 0x52 => P::KeyR, 0x53 => P::KeyS, 0x54 => P::KeyT,
        0x55 => P::KeyU, 0x56 => P::KeyV, 0x57 => P::KeyW, 0x58 => P::KeyX,
        0x59 => P::KeyY, 0x5A => P::KeyZ,
        0x60 => P::Numpad0, 0x61 => P::Numpad1, 0x62 => P::Numpad2, 0x63 => P::Numpad3,
        0x64 => P::Numpad4, 0x65 => P::Numpad5, 0x66 => P::Numpad6, 0x67 => P::Numpad7,
        0x68 => P::Numpad8, 0x69 => P::Numpad9,
        0x6A => P::NumpadMultiply,
        0x6B => P::NumpadAdd,
        0x6D => P::NumpadSubtract,
        0x6E => P::NumpadDecimal,
        0x6F => P::NumpadDivide,
        0x70 => P::F1, 0x71 => P::F2, 0x72 => P::F3, 0x73 => P::F4,
        0x74 => P::F5, 0x75 => P::F6, 0x76 => P::F7, 0x77 => P::F8,
        0x78 => P::F9, 0x79 => P::F10, 0x7A => P::F11, 0x7B => P::F12,
        0x90 => P::NumLock,
        0x91 => P::ScrollLock,
        0xBA => P::Semicolon,
        0xBB => P::Equal,
        0xBC => P::Comma,
        0xBD => P::Minus,
        0xBE => P::Period,
        0xBF => P::Slash,
        0xC0 => P::Backquote,
        0xDB => P::BracketLeft,
        0xDC => P::Backslash,
        0xDD => P::BracketRight,
        0xDE => P::Quote,
        _ => P::Unknown,
    }
}

fn vk_to_named(vk: u32) -> Option<NamedKey> {
    use NamedKey as N;
    Some(match vk {
        0x08 => N::Backspace,
        0x09 => N::Tab,
        0x0D => N::Enter,
        0x10 | 0xA0 | 0xA1 => N::Shift,
        0x11 | 0xA2 | 0xA3 => N::Control,
        0x12 | 0xA4 | 0xA5 => N::Alt,
        0x13 => N::Pause,
        0x14 => N::CapsLock,
        0x1B => N::Escape,
        0x21 => N::PageUp,
        0x22 => N::PageDown,
        0x23 => N::End,
        0x24 => N::Home,
        0x25 => N::ArrowLeft,
        0x26 => N::ArrowUp,
        0x27 => N::ArrowRight,
        0x28 => N::ArrowDown,
        0x2D => N::Insert,
        0x2E => N::Delete,
        0x70 => N::F1, 0x71 => N::F2, 0x72 => N::F3, 0x73 => N::F4,
        0x74 => N::F5, 0x75 => N::F6, 0x76 => N::F7, 0x77 => N::F8,
        0x78 => N::F9, 0x79 => N::F10, 0x7A => N::F11, 0x7B => N::F12,
        0x90 => N::NumLock,
        0x91 => N::ScrollLock,
        _ => return None,
    })
}

/// Forward a key press (down = 1) or release (down = 0). `vk` is the
/// Windows virtual key, `extended` is lParam bit 24, `ch` is the character
/// the key produces in the current layout (0 if none). Returns 0, or -1 if
/// dead.
///
/// # Safety
/// `e` must be a live embed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn navrfl_key(e: *mut Embed, down: i32, vk: u32, extended: i32, ch: u32) -> i32 {
    let ext = extended != 0;
    let physical_key = vk_to_physical(vk, ext);
    let logical_key = if let Some(n) = vk_to_named(vk) {
        LogicalKey::Named(n)
    } else if let Some(c) = char::from_u32(ch).filter(|c| !c.is_control()) {
        LogicalKey::Character(c)
    } else {
        LogicalKey::Unknown
    };
    let key_location = match vk {
        0xA1 | 0xA3 | 0xA5 => KeyLocation::Right,
        0xA0 | 0xA2 | 0xA4 => KeyLocation::Left,
        0x60..=0x6F => KeyLocation::Numpad,
        0x0D if ext => KeyLocation::Numpad,
        _ => KeyLocation::Standard,
    };
    let key = KeyDescriptor { physical_key, logical_key, key_location };
    let ev = if down != 0 { PlayerEvent::KeyDown { key } } else { PlayerEvent::KeyUp { key } };
    with_player(e, |_, p| {
        p.handle_event(ev);
    })
    .map_or(-1, |_| 0)
}

/// Forward a typed character (WM_CHAR, UTF-16 already combined). Control
/// characters are ignored. Returns 0, or -1 if dead.
///
/// # Safety
/// `e` must be a live embed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn navrfl_char(e: *mut Embed, ch: u32) -> i32 {
    let Some(c) = char::from_u32(ch).filter(|c| !c.is_control()) else {
        return 0;
    };
    with_player(e, |_, p| {
        p.handle_event(PlayerEvent::TextInput { codepoint: c });
    })
    .map_or(-1, |_| 0)
}

/// Tell the movie it gained (1) or lost (0) keyboard focus.
///
/// # Safety
/// `e` must be a live embed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn navrfl_focus(e: *mut Embed, gained: i32) -> i32 {
    let ev = if gained != 0 { PlayerEvent::FocusGained } else { PlayerEvent::FocusLost };
    with_player(e, |_, p| {
        p.handle_event(ev);
    })
    .map_or(-1, |_| 0)
}

/// Write the name of the default audio output device into `buf` (for
/// diagnostics). Returns 1 if a device exists, 0 if none.
///
/// # Safety
/// `buf` must hold `len` bytes or be NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn navrfl_default_audio_device(buf: *mut c_char, len: usize) -> i32 {
    let r = catch_unwind(|| {
        cpal::default_host()
            .default_output_device()
            .and_then(|d| d.name().ok())
    });
    match r {
        Ok(Some(name)) => {
            write_err(buf, len, &name);
            1
        }
        _ => {
            write_err(buf, len, "");
            0
        }
    }
}

/// Parse `swf` and report its stage size in pixels and its SWF version,
/// without creating a player. Returns 1 if it parsed, 0 if not.
///
/// # Safety
/// `swf` must point at `swf_len` readable bytes; out pointers may be NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn navrfl_probe(
    swf: *const u8,
    swf_len: usize,
    out_w: *mut u32,
    out_h: *mut u32,
    out_version: *mut u32,
) -> i32 {
    if swf.is_null() || swf_len == 0 {
        return 0;
    }
    let data = unsafe { std::slice::from_raw_parts(swf, swf_len) };
    if check_header(data).is_err() {
        return 0;
    }
    let r = catch_unwind(|| {
        SwfMovieData::from_data(data, "about:blank".to_string(), None, None).ok().map(|m| {
            (m.width().to_pixels() as u32, m.height().to_pixels() as u32, m.version() as u32)
        })
    });
    match r {
        Ok(Some((w, h, v))) => {
            unsafe {
                if !out_w.is_null() { *out_w = w; }
                if !out_h.is_null() { *out_h = h; }
                if !out_version.is_null() { *out_version = v; }
            }
            1
        }
        _ => 0,
    }
}

/// Create the shared graphics device now, on the calling thread (any
/// thread). Call it from a worker before the first navrfl_create so that
/// creation on the GUI thread is quick. Returns 1 if a device is ready, 0 if
/// none could be made (reason in `err`).
///
/// # Safety
/// `err` must hold `errlen` bytes or be NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn navrfl_prewarm(flags: u32, err: *mut c_char, errlen: usize) -> i32 {
    match catch_unwind(|| shared_descriptors(flags)) {
        Ok(Ok(d)) => {
            let info = d.adapter.get_info();
            write_err(err, errlen, &format!("{:?} {}", info.backend, info.name));
            1
        }
        Ok(Err(msg)) => {
            write_err(err, errlen, &msg);
            0
        }
        Err(p) => {
            write_err(err, errlen, &format!("graphics panicked: {}", panic_text(&*p)));
            0
        }
    }
}
