// Slint on the B4 (SGX530) hard-float GLES2 stack via a custom platform:
// a FLIPWSEGL window on the shim, driving Slint's FemtoVG OpenGL renderer.
// Milestone: a solid-fill window + a wall of rounded "tiles".

use std::cell::Cell;
use std::ffi::{c_void, CStr};
use std::num::NonZeroU32;
use std::rc::{Rc, Weak};

use i_slint_renderer_femtovg::FemtoVGOpenGLRenderer;
use i_slint_renderer_femtovg::opengl::OpenGLInterface;
use khronos_egl as egl;
use slint::platform::{Platform, WindowAdapter, WindowEvent};
use slint::platform::Renderer;
use slint::{LogicalSize, PhysicalSize, PlatformError, Window};

slint::slint! {
    export component AppWindow inherits Window {
        width: 1024px;
        height: 600px;
        background: #202020;
        in property <bool> cache-on: true;

        // A wall of tiles; a couple animate to exercise the cached composite.
        for tile[i] in [
            { c: #3366cc, x: 16px, y: 16px },
            { c: #cc6633, x: 252px, y: 16px },
            { c: #33cc66, x: 488px, y: 16px },
            { c: #cc33aa, x: 724px, y: 16px },
            { c: #6633cc, x: 16px, y: 168px },
            { c: #33aacc, x: 252px, y: 168px },
            { c: #aacc33, x: 488px, y: 168px },
            { c: #cc3366, x: 724px, y: 168px },
        ] : Rectangle {
            x: tile.x + (i == 1 || i == 6 ? 20px * sin(anim * 360deg) : 0px);
            y: tile.y;
            width: 220px;
            height: 136px;
            background: tile.c;
            border-radius: 12px;
            // Bake each tile to a texture once; moving it should just re-composite.
            cache-rendering-hint: root.cache-on;
        }

        property <float> anim;
        animate anim { duration: 2s; iteration-count: -1; }
        init => { anim = 1.0; }
    }
}

/// The shim EGL context, exposed to Slint's FemtoVG renderer as an OpenGL surface.
struct EglContext {
    egl: egl::DynamicInstance<egl::EGL1_4>,
    display: egl::Display,
    surface: egl::Surface,
    context: egl::Context,
}

impl Drop for EglContext {
    // Close the SGX session cleanly: an abrupt teardown can race pvrsrvkm's
    // command-queue timer and Oops the kernel.
    fn drop(&mut self) {
        let _ = self.egl.make_current(self.display, None, None, None);
        let _ = self.egl.destroy_surface(self.display, self.surface);
        let _ = self.egl.destroy_context(self.display, self.context);
        let _ = self.egl.terminate(self.display);
    }
}

#[allow(unsafe_code)]
unsafe impl OpenGLInterface for EglContext {
    fn ensure_current(&self) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        self.egl
            .make_current(self.display, Some(self.surface), Some(self.surface), Some(self.context))
            .map_err(|e| format!("eglMakeCurrent: {e:?}").into())
    }

    fn swap_buffers(&self) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        self.egl
            .swap_buffers(self.display, self.surface)
            .map_err(|e| format!("eglSwapBuffers: {e:?}").into())
    }

    fn resize(&self, _w: NonZeroU32, _h: NonZeroU32) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        Ok(()) // fixed 1024x600 FLIPWSEGL surface
    }

    fn get_proc_address(&self, name: &CStr) -> *const c_void {
        match self.egl.get_proc_address(name.to_str().unwrap_or("")) {
            Some(f) => f as usize as *const c_void,
            None => std::ptr::null(),
        }
    }
}

fn create_egl_context() -> EglContext {
    // Promote the shim libs to the global symbol scope so the shim's
    // eglGetProcAddress veneers (resolved via dlsym(RTLD_DEFAULT)) are visible.
    unsafe {
        for lib in [c"libGLESv2.so", c"libEGL.so"] {
            if libc::dlopen(lib.as_ptr(), libc::RTLD_NOW | libc::RTLD_GLOBAL).is_null() {
                panic!("dlopen {lib:?} failed");
            }
        }
    }

    let egl = unsafe { egl::DynamicInstance::<egl::EGL1_4>::load_required_from_filename("libEGL.so") }
        .expect("load libEGL.so");
    let display = unsafe { egl.get_display(egl::DEFAULT_DISPLAY) }.expect("eglGetDisplay");
    egl.initialize(display).expect("eglInitialize");

    let config_attribs = [
        egl::SURFACE_TYPE, egl::WINDOW_BIT,
        egl::RENDERABLE_TYPE, egl::OPENGL_ES2_BIT,
        egl::RED_SIZE, 8, egl::GREEN_SIZE, 8, egl::BLUE_SIZE, 8, egl::ALPHA_SIZE, 8,
        egl::STENCIL_SIZE, 8,
        egl::NONE,
    ];
    let config = egl
        .choose_first_config(display, &config_attribs)
        .expect("eglChooseConfig")
        .expect("no matching EGL config");
    egl.bind_api(egl::OPENGL_ES_API).expect("eglBindAPI");
    let ctx_attribs = [egl::CONTEXT_CLIENT_VERSION, 2, egl::NONE];
    let context = egl.create_context(display, config, None, &ctx_attribs).expect("eglCreateContext");
    // FLIPWSEGL native window handle is 0 (null).
    let surface = unsafe {
        egl.create_window_surface(display, config, std::ptr::null_mut(), None)
            .expect("eglCreateWindowSurface")
    };
    egl.make_current(display, Some(surface), Some(surface), Some(context)).expect("eglMakeCurrent");

    EglContext { egl, display, surface, context }
}

struct TileWindowAdapter {
    window: Window,
    renderer: FemtoVGOpenGLRenderer,
    needs_redraw: Cell<bool>,
    size: PhysicalSize,
}

impl TileWindowAdapter {
    fn new() -> Rc<Self> {
        let egl_context = create_egl_context();
        let renderer = FemtoVGOpenGLRenderer::new(egl_context).expect("FemtoVG renderer");
        Rc::new_cyclic(|weak: &Weak<Self>| TileWindowAdapter {
            window: Window::new(weak.clone()),
            renderer,
            needs_redraw: Cell::new(true),
            size: PhysicalSize::new(1024, 600),
        })
    }
}

impl WindowAdapter for TileWindowAdapter {
    fn window(&self) -> &Window {
        &self.window
    }
    fn size(&self) -> PhysicalSize {
        self.size
    }
    fn renderer(&self) -> &dyn Renderer {
        &self.renderer
    }
    fn request_redraw(&self) {
        self.needs_redraw.set(true);
    }
}

struct TilePlatform {
    adapter: Rc<TileWindowAdapter>,
}

impl Platform for TilePlatform {
    fn create_window_adapter(&self) -> Result<Rc<dyn WindowAdapter>, PlatformError> {
        Ok(self.adapter.clone())
    }

    fn run_event_loop(&self) -> Result<(), PlatformError> {
        let adapter = &self.adapter;
        adapter
            .window
            .dispatch_event(WindowEvent::ScaleFactorChanged { scale_factor: 1.0 });
        adapter
            .window
            .dispatch_event(WindowEvent::Resized { size: LogicalSize::new(1024.0, 600.0) });

        let seconds: u64 = std::env::var("SLINT_SECONDS").ok().and_then(|v| v.parse().ok()).unwrap_or(20);
        let start = std::time::Instant::now();
        let mut frames = 0u64;
        while start.elapsed().as_secs() < seconds {
            slint::platform::update_timers_and_animations();
            adapter.needs_redraw.set(false);
            adapter.renderer.render().map_err(|e| PlatformError::Other(format!("render: {e}")))?;
            frames += 1;
        }
        let secs = start.elapsed().as_secs_f32();
        println!("slint: {frames} frames in {secs:.1}s = {:.1} fps", frames as f32 / secs);
        Ok(())
    }
}

fn main() {
    let adapter = TileWindowAdapter::new();
    slint::platform::set_platform(Box::new(TilePlatform { adapter }))
        .expect("set_platform");

    let ui = AppWindow::new().expect("AppWindow");
    ui.set_cache_on(std::env::var("SLINT_CACHE").as_deref() != Ok("0"));
    ui.run().expect("run");
}
