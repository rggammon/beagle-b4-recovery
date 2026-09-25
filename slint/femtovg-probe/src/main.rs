// Minimal FemtoVG GLES2 solid-fill probe for the B4 (SGX530) hard-float shim.
//
// Default (headless): create an EGL pbuffer, build the FemtoVG OpenGl renderer
// through the shim's eglGetProcAddress, fill the canvas with a solid colour, and
// read back the centre pixel. Expected ~ (51, 102, 153) for Color::rgb(51,102,153).
//
// FVG_WINDOW=1: render into a FLIPWSEGL window surface (native handle 0) at
// 1024x600 and swap for FVG_SECONDS (default 10) so the dc_nohw present=2 path
// scans it out onto the panel.
//
// Run on-board via: sgx-ddk16-hf-run ./femtovg-probe   (after pvrsrvinit).

use std::ffi::{c_void, CStr};
use std::ptr;

use femtovg::{renderer::OpenGl, Canvas, Color, ImageFlags, Paint, Path, PixelFormat, RenderTarget};
use khronos_egl as egl;

// Grid position for tile i at time t (4 columns) with a small sinusoidal drift.
fn tile_pos(i: u32, t: f32) -> (f32, f32) {
    let col = i % 4;
    let row = i / 4;
    let base_x = 16.0 + col as f32 * 236.0;
    let base_y = 16.0 + row as f32 * 152.0;
    let dx = 18.0 * (t * 1.5 + i as f32 * 0.7).sin();
    let dy = 10.0 * (t * 1.2 + i as f32 * 0.5).cos();
    (base_x + dx, base_y + dy)
}

// Composite N cached tile textures via a trivial texture-copy shader (NO scissor).
fn run_tiles_gl(
    egl: &egl::DynamicInstance<egl::EGL1_4>,
    display: egl::Display,
    surface: egl::Surface,
    w: u32,
    h: u32,
    ntiles: u32,
    seconds: u32,
) {
    use glow::HasContext;
    let gl = unsafe {
        glow::Context::from_loader_function(|s| {
            egl.get_proc_address(s).map_or(std::ptr::null(), |f| f as usize as *const c_void)
        })
    };
    let (tw, th): (u32, u32) = (220, 136);
    unsafe {
        let vs_src = "#version 100\nattribute vec2 pos;\nattribute vec2 uv;\nvarying vec2 vuv;\nvoid main(){ vuv = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
        let fs_src = "#version 100\nprecision mediump float;\nvarying vec2 vuv;\nuniform sampler2D tex;\nvoid main(){ gl_FragColor = texture2D(tex, vuv); }\n";
        let vs = gl.create_shader(glow::VERTEX_SHADER).unwrap();
        gl.shader_source(vs, vs_src);
        gl.compile_shader(vs);
        assert!(gl.get_shader_compile_status(vs), "VS: {}", gl.get_shader_info_log(vs));
        let fs = gl.create_shader(glow::FRAGMENT_SHADER).unwrap();
        gl.shader_source(fs, fs_src);
        gl.compile_shader(fs);
        assert!(gl.get_shader_compile_status(fs), "FS: {}", gl.get_shader_info_log(fs));
        let prog = gl.create_program().unwrap();
        gl.attach_shader(prog, vs);
        gl.attach_shader(prog, fs);
        gl.link_program(prog);
        assert!(gl.get_program_link_status(prog), "link: {}", gl.get_program_info_log(prog));
        gl.use_program(Some(prog));
        let loc_pos = gl.get_attrib_location(prog, "pos").unwrap();
        let loc_uv = gl.get_attrib_location(prog, "uv").unwrap();
        let loc_tex = gl.get_uniform_location(prog, "tex");

        let mut data = vec![0u8; (tw * th * 4) as usize];
        for y in 0..th {
            for x in 0..tw {
                let i = ((y * tw + x) * 4) as usize;
                let t = y as f32 / th as f32;
                data[i] = (60.0 * (1.0 - t) + 30.0 * t) as u8;
                data[i + 1] = (120.0 * (1.0 - t) + 70.0 * t) as u8;
                data[i + 2] = (190.0 * (1.0 - t) + 120.0 * t) as u8;
                data[i + 3] = 255;
                if y >= th - 40 && y < th - 20 && x >= 16 && x < tw - 16 {
                    data[i] = 230;
                    data[i + 1] = 230;
                    data[i + 2] = 240;
                }
            }
        }
        let tex = gl.create_texture().unwrap();
        gl.active_texture(glow::TEXTURE0);
        gl.bind_texture(glow::TEXTURE_2D, Some(tex));
        gl.tex_image_2d(glow::TEXTURE_2D, 0, glow::RGBA as i32, tw as i32, th as i32, 0, glow::RGBA, glow::UNSIGNED_BYTE, glow::PixelUnpackData::Slice(Some(&data)));
        gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MIN_FILTER, glow::LINEAR as i32);
        gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_MAG_FILTER, glow::LINEAR as i32);
        gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_S, glow::CLAMP_TO_EDGE as i32);
        gl.tex_parameter_i32(glow::TEXTURE_2D, glow::TEXTURE_WRAP_T, glow::CLAMP_TO_EDGE as i32);
        if let Some(l) = loc_tex.as_ref() {
            gl.uniform_1_i32(Some(l), 0);
        }

        let vbo = gl.create_buffer().unwrap();
        gl.bind_buffer(glow::ARRAY_BUFFER, Some(vbo));
        gl.vertex_attrib_pointer_f32(loc_pos, 2, glow::FLOAT, false, 16, 0);
        gl.enable_vertex_attrib_array(loc_pos);
        gl.vertex_attrib_pointer_f32(loc_uv, 2, glow::FLOAT, false, 16, 8);
        gl.enable_vertex_attrib_array(loc_uv);

        gl.viewport(0, 0, w as i32, h as i32);
        gl.disable(glow::DEPTH_TEST);
        gl.disable(glow::SCISSOR_TEST);

        let start = std::time::Instant::now();
        let mut frames = 0u64;
        while start.elapsed().as_secs() < seconds as u64 {
            gl.clear_color(0.0, 0.0, 0.0, 1.0);
            gl.clear(glow::COLOR_BUFFER_BIT);
            let t = start.elapsed().as_secs_f32();
            for i in 0..ntiles {
                let (px, py) = tile_pos(i, t);
                let x0 = px / w as f32 * 2.0 - 1.0;
                let x1 = (px + tw as f32) / w as f32 * 2.0 - 1.0;
                let y0 = 1.0 - py / h as f32 * 2.0;
                let y1 = 1.0 - (py + th as f32) / h as f32 * 2.0;
                let verts: [f32; 16] = [x0, y0, 0.0, 0.0, x1, y0, 1.0, 0.0, x0, y1, 0.0, 1.0, x1, y1, 1.0, 1.0];
                let bytes = core::slice::from_raw_parts(verts.as_ptr() as *const u8, 64);
                gl.buffer_data_u8_slice(glow::ARRAY_BUFFER, bytes, glow::DYNAMIC_DRAW);
                gl.draw_arrays(glow::TRIANGLE_STRIP, 0, 4);
            }
            egl.swap_buffers(display, surface).expect("swap");
            frames += 1;
        }
        let secs = start.elapsed().as_secs_f32();
        println!("window[tilesgl]: {frames} frames in {secs:.1}s = {:.1} fps", frames as f32 / secs);
    }
}

fn env_flag(name: &str) -> bool {
    std::env::var(name).map(|v| v != "0" && !v.is_empty()).unwrap_or(false)
}

fn env_u32(name: &str, default: u32) -> u32 {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

fn main() {
    let window_mode = env_flag("FVG_WINDOW");
    let (w, h): (u32, u32) = if window_mode { (1024, 600) } else { (64, 64) };

    // The shim's eglGetProcAddress returns its veneers via dlsym(RTLD_DEFAULT),
    // so the shim libs must sit in the GLOBAL symbol scope. libloading (used by
    // khronos-egl dynamic) dlopens RTLD_LOCAL, so promote them explicitly first.
    unsafe {
        for name in [c"libGLESv2.so", c"libEGL.so"] {
            if libc::dlopen(name.as_ptr(), libc::RTLD_NOW | libc::RTLD_GLOBAL).is_null() {
                let e = CStr::from_ptr(libc::dlerror());
                panic!("dlopen {name:?} (RTLD_GLOBAL) failed: {}", e.to_string_lossy());
            }
        }
    }

    // dlopen the shim's libEGL.so (found via LD_LIBRARY_PATH set by sgx-ddk16-hf-run).
    let egl = unsafe {
        egl::DynamicInstance::<egl::EGL1_4>::load_required_from_filename("libEGL.so")
    }
    .expect("load libEGL.so");

    let display = unsafe { egl.get_display(egl::DEFAULT_DISPLAY) }.expect("eglGetDisplay");
    let (major, minor) = egl.initialize(display).expect("eglInitialize");
    eprintln!("EGL {major}.{minor}  mode={}", if window_mode { "window" } else { "pbuffer" });

    let surface_bit = if window_mode { egl::WINDOW_BIT } else { egl::PBUFFER_BIT };
    let config_attribs = [
        egl::SURFACE_TYPE,
        surface_bit,
        egl::RENDERABLE_TYPE,
        egl::OPENGL_ES2_BIT,
        egl::RED_SIZE,
        8,
        egl::GREEN_SIZE,
        8,
        egl::BLUE_SIZE,
        8,
        egl::ALPHA_SIZE,
        8,
        egl::STENCIL_SIZE,
        8,
        egl::NONE,
    ];
    let config = egl
        .choose_first_config(display, &config_attribs)
        .expect("eglChooseConfig")
        .expect("no matching EGL config");

    egl.bind_api(egl::OPENGL_ES_API).expect("eglBindAPI");

    let ctx_attribs = [egl::CONTEXT_CLIENT_VERSION, 2, egl::NONE];
    let context = egl
        .create_context(display, config, None, &ctx_attribs)
        .expect("eglCreateContext");

    let surface = if window_mode {
        // FLIPWSEGL native window handle is 0 (null).
        unsafe {
            egl.create_window_surface(display, config, ptr::null_mut(), None)
                .expect("eglCreateWindowSurface")
        }
    } else {
        let surf_attribs = [egl::WIDTH, w as i32, egl::HEIGHT, h as i32, egl::NONE];
        egl.create_pbuffer_surface(display, config, &surf_attribs)
            .expect("eglCreatePbufferSurface")
    };

    egl.make_current(display, Some(surface), Some(surface), Some(context))
        .expect("eglMakeCurrent");

    // Raw-GL unclipped textured-quad composite (no scissor) — the ceiling test
    // for the Maemo composite-cached-texture path, bypassing femtovg entirely.
    if window_mode && std::env::var("FVG_MODE").as_deref() == Ok("tilesgl") {
        run_tiles_gl(&egl, display, surface, w, h, env_u32("FVG_TILES", 12), env_u32("FVG_SECONDS", 10));
        return;
    }

    // Build the FemtoVG GLES2 renderer using the shim's eglGetProcAddress. This is
    // where the shader programs get compiled by the SGX530 GLSL compiler.
    let renderer = unsafe {
        OpenGl::new_from_function_cstr(|s: &CStr| match egl.get_proc_address(s.to_str().unwrap()) {
            Some(f) => f as usize as *const c_void,
            None => ptr::null(),
        })
    }
    .expect("FemtoVG OpenGl renderer (shader compile)");

    let mut canvas = Canvas::new(renderer).expect("FemtoVG Canvas");
    canvas.set_size(w, h, 1.0);

    let fill = Color::rgb(51, 102, 153);

    if window_mode {
        let seconds = env_u32("FVG_SECONDS", 10);
        // FVG_MODE: "fill" (femtovg full-screen, default) | "fillsmall" (femtovg
        // 128x128) | "clear" (raw glClear, no femtovg) | "tiles" (composite a cached
        // tile texture) | "tilesredraw" (re-render tile content each frame) —
        // isolates shader vs present and caching vs re-rasterization.
        let mode = std::env::var("FVG_MODE").unwrap_or_else(|_| "fill".into());
        let ntiles = env_u32("FVG_TILES", 12);
        let (tw, th): (u32, u32) = (220, 136);

        // Bake the tile content into a texture ONCE (Maemo composite-cached-texture).
        let tile_img = if mode == "tiles" {
            let img = canvas
                .create_image_empty(tw as usize, th as usize, PixelFormat::Rgba8, ImageFlags::FLIP_Y)
                .expect("create tile image");
            canvas.set_render_target(RenderTarget::Image(img));
            canvas.set_size(tw, th, 1.0);
            canvas.clear_rect(0, 0, tw, th, Color::rgba(0, 0, 0, 0));
            let mut p = Path::new();
            p.rounded_rect(2.0, 2.0, (tw - 4) as f32, (th - 4) as f32, 12.0);
            let grad =
                Paint::linear_gradient(0.0, 0.0, 0.0, th as f32, Color::rgb(60, 120, 190), Color::rgb(30, 70, 120));
            canvas.fill_path(&p, &grad);
            let mut bar = Path::new();
            bar.rect(16.0, (th - 40) as f32, (tw - 32) as f32, 20.0);
            canvas.fill_path(&bar, &Paint::color(Color::rgb(230, 230, 240)));
            canvas.flush();
            canvas.set_render_target(RenderTarget::Screen);
            canvas.set_size(w, h, 1.0);
            Some(img)
        } else {
            None
        };

        // Raw GL entry points for the plain-clear baseline.
        type ClearColor = unsafe extern "C" fn(f32, f32, f32, f32);
        type ClearFn = unsafe extern "C" fn(u32);
        const GL_COLOR_BUFFER_BIT: u32 = 0x4000;
        let gl_clear_color: ClearColor = unsafe {
            std::mem::transmute(egl.get_proc_address("glClearColor").expect("glClearColor"))
        };
        let gl_clear: ClearFn =
            unsafe { std::mem::transmute(egl.get_proc_address("glClear").expect("glClear")) };

        let start = std::time::Instant::now();
        let mut frames = 0u64;
        while start.elapsed().as_secs() < seconds as u64 {
            match mode.as_str() {
                "clear" => unsafe {
                    gl_clear_color(0.20, 0.40, 0.60, 1.0);
                    gl_clear(GL_COLOR_BUFFER_BIT);
                },
                "fillsmall" => {
                    canvas.clear_rect(0, 0, w, h, Color::black());
                    let mut path = Path::new();
                    path.rect(0.0, 0.0, 128.0, 128.0);
                    canvas.fill_path(&path, &Paint::color(fill));
                    canvas.flush();
                }
                "fillhalf" => {
                    // Render at 1/4 resolution (512x300) to gauge fill-rate scaling.
                    canvas.set_size(512, 300, 1.0);
                    canvas.clear_rect(0, 0, 512, 300, Color::black());
                    let mut path = Path::new();
                    path.rect(0.0, 0.0, 512.0, 300.0);
                    canvas.fill_path(&path, &Paint::color(fill));
                    canvas.flush();
                }
                "fillrect" => {
                    // Fill an env-sized rect on the FULL canvas (no per-frame set_size),
                    // to measure clean partial-coverage fill-rate. FVG_RW x FVG_RH.
                    let rw = env_u32("FVG_RW", 512) as f32;
                    let rh = env_u32("FVG_RH", 300) as f32;
                    canvas.clear_rect(0, 0, w, h, Color::black());
                    let mut path = Path::new();
                    path.rect(0.0, 0.0, rw, rh);
                    canvas.fill_path(&path, &Paint::color(fill));
                    canvas.flush();
                }
                "tiles" => {
                    // Composite N cached tile textures at animated positions.
                    // anti_alias(false) lets femtovg route the rect+image fill to
                    // its unclipped TextureCopyUnclipped fast path (as Slint does).
                    let img = tile_img.unwrap();
                    let t = start.elapsed().as_secs_f32();
                    canvas.clear_rect(0, 0, w, h, Color::black());
                    for i in 0..ntiles {
                        let (x, y) = tile_pos(i, t);
                        let mut p = Path::new();
                        p.rect(x, y, tw as f32, th as f32);
                        canvas.fill_path(
                            &p,
                            &Paint::image(img, x, y, tw as f32, th as f32, 0.0, 1.0)
                                .with_anti_alias(false),
                        );
                    }
                    canvas.flush();
                }
                "tilesredraw" => {
                    // Re-render each tile's content every frame (no caching).
                    let t = start.elapsed().as_secs_f32();
                    canvas.clear_rect(0, 0, w, h, Color::black());
                    for i in 0..ntiles {
                        let (x, y) = tile_pos(i, t);
                        let mut p = Path::new();
                        p.rounded_rect(x + 2.0, y + 2.0, (tw - 4) as f32, (th - 4) as f32, 12.0);
                        let grad = Paint::linear_gradient(
                            x,
                            y,
                            x,
                            y + th as f32,
                            Color::rgb(60, 120, 190),
                            Color::rgb(30, 70, 120),
                        );
                        canvas.fill_path(&p, &grad);
                        let mut bar = Path::new();
                        bar.rect(x + 16.0, y + (th - 40) as f32, (tw - 32) as f32, 20.0);
                        canvas.fill_path(&bar, &Paint::color(Color::rgb(230, 230, 240)));
                    }
                    canvas.flush();
                }
                "tilesrsolid" => {
                    // Isolate multi-tile ROUNDED FRINGE: N rounded rects, SOLID paint (no gradient).
                    let t = start.elapsed().as_secs_f32();
                    canvas.clear_rect(0, 0, w, h, Color::black());
                    for i in 0..ntiles {
                        let (x, y) = tile_pos(i, t);
                        let mut p = Path::new();
                        p.rounded_rect(x + 2.0, y + 2.0, (tw - 4) as f32, (th - 4) as f32, 12.0);
                        canvas.fill_path(&p, &Paint::color(fill));
                    }
                    canvas.flush();
                }
                "tilesrsolidnoaa" => {
                    // Same as tilesrsolid but AA off — pins storm to femtovg's rounded fringe path.
                    let t = start.elapsed().as_secs_f32();
                    canvas.clear_rect(0, 0, w, h, Color::black());
                    for i in 0..ntiles {
                        let (x, y) = tile_pos(i, t);
                        let mut p = Path::new();
                        p.rounded_rect(x + 2.0, y + 2.0, (tw - 4) as f32, (th - 4) as f32, 12.0);
                        canvas.fill_path(&p, &Paint::color(fill).with_anti_alias(false));
                    }
                    canvas.flush();
                }
                "tilesgrect" => {
                    // Isolate multi-tile GRADIENT: N plain rects (no rounding), gradient paint.
                    let t = start.elapsed().as_secs_f32();
                    canvas.clear_rect(0, 0, w, h, Color::black());
                    for i in 0..ntiles {
                        let (x, y) = tile_pos(i, t);
                        let mut p = Path::new();
                        p.rect(x, y, tw as f32, th as f32);
                        let grad = Paint::linear_gradient(
                            x,
                            y,
                            x,
                            y + th as f32,
                            Color::rgb(60, 120, 190),
                            Color::rgb(30, 70, 120),
                        );
                        canvas.fill_path(&p, &grad);
                    }
                    canvas.flush();
                }
                "fillnoaa" => {
                    canvas.clear_rect(0, 0, w, h, Color::black());
                    let mut path = Path::new();
                    path.rect(0.0, 0.0, w as f32, h as f32);
                    canvas.fill_path(&path, &Paint::color(fill).with_anti_alias(false));
                    canvas.flush();
                }
                "tri" | "trinoaa" => {
                    // Single filled triangle A/B for AA fringe isolation (mirrors tri-inline).
                    let aa = mode == "tri";
                    canvas.clear_rect(0, 0, w, h, Color::black());
                    let cx = w as f32 * 0.5;
                    let cy = h as f32 * 0.5;
                    let r = (w.min(h) as f32) * 0.35;
                    let mut path = Path::new();
                    path.move_to(cx, cy - r);
                    path.line_to(cx + r, cy + r);
                    path.line_to(cx - r, cy + r);
                    path.close();
                    canvas.fill_path(&path, &Paint::color(fill).with_anti_alias(aa));
                    canvas.flush();
                }
                "gradrect" => {
                    // Isolate GRADIENT: plain (non-rounded) rect, linear-gradient paint.
                    // Shares plain-rect + default-AA with "fill" (which is clean), so a
                    // reset here implicates the gradient (texture) path, not AA/rounding.
                    canvas.clear_rect(0, 0, w, h, Color::black());
                    let mut path = Path::new();
                    path.rect(0.0, 0.0, w as f32, h as f32);
                    let grad = Paint::linear_gradient(
                        0.0,
                        0.0,
                        0.0,
                        h as f32,
                        Color::rgb(60, 120, 190),
                        Color::rgb(30, 70, 120),
                    );
                    canvas.fill_path(&path, &grad);
                    canvas.flush();
                }
                "roundsolid" => {
                    // Isolate AA rounded corners: rounded_rect, SOLID paint (no gradient).
                    // A reset here implicates the AA fringe / rounded tessellation path.
                    canvas.clear_rect(0, 0, w, h, Color::black());
                    let mut path = Path::new();
                    path.rounded_rect(2.0, 2.0, (w - 4) as f32, (h - 4) as f32, 24.0);
                    canvas.fill_path(&path, &Paint::color(fill));
                    canvas.flush();
                }
                _ => {
                    canvas.clear_rect(0, 0, w, h, Color::black());
                    let mut path = Path::new();
                    path.rect(0.0, 0.0, w as f32, h as f32);
                    canvas.fill_path(&path, &Paint::color(fill));
                    canvas.flush();
                }
            }
            egl.swap_buffers(display, surface).expect("eglSwapBuffers");
            frames += 1;
        }
        let secs = start.elapsed().as_secs_f32();
        println!(
            "window[{mode}]: {frames} frames in {secs:.1}s = {:.1} fps",
            frames as f32 / secs
        );
    } else {
        canvas.clear_rect(0, 0, w, h, Color::black());
        let mut path = Path::new();
        path.rect(0.0, 0.0, w as f32, h as f32);
        canvas.fill_path(&path, &Paint::color(fill));
        canvas.flush();

        // glReadPixels through the shim to verify the fill landed.
        type ReadPixels = unsafe extern "C" fn(i32, i32, i32, i32, u32, u32, *mut c_void);
        const GL_RGBA: u32 = 0x1908;
        const GL_UNSIGNED_BYTE: u32 = 0x1401;
        let rp: ReadPixels = unsafe {
            std::mem::transmute::<_, ReadPixels>(
                egl.get_proc_address("glReadPixels").expect("glReadPixels"),
            )
        };
        let mut px = [0u8; 4];
        unsafe {
            rp(
                (w / 2) as i32,
                (h / 2) as i32,
                1,
                1,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                px.as_mut_ptr() as *mut c_void,
            );
        }
        println!("centre RGBA = {} {} {} {}", px[0], px[1], px[2], px[3]);
        let ok = px[0].abs_diff(51) <= 2 && px[1].abs_diff(102) <= 2 && px[2].abs_diff(153) <= 2;
        println!("RESULT: {}", if ok { "PASS" } else { "FAIL" });
        if !ok {
            std::process::exit(1);
        }
    }
}
