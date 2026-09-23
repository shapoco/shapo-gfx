// ShapoGFX demo viewer (shared by the demos under docs/example/).
//
// Loads a STANDALONE_WASM module that exports
//   <prefix>_init()
//   <prefix>_frame(t [, yaw, pitch, dist])   -- camera arguments only when opts.camera is set
//   <prefix>_get_fb(), <prefix>_get_width(), <prefix>_get_height()
// and copies its RGB565_SWAPPED frame buffer to the <canvas id="screen"> every frame.
//
// startDemoViewer({ wasm: 'demo3d.wasm', prefix: 'demo3d', scale: 2,
//                   camera: { yaw, pitch, dist, pitchMin, pitchMax, distMin, distMax } })

'use strict';

// RGB565 -> 8-bit expansion tables
const LUT5 = new Uint8Array(32);
const LUT6 = new Uint8Array(64);
for (let i = 0; i < 32; i++) LUT5[i] = Math.round(i * 255 / 31);
for (let i = 0; i < 64; i++) LUT6[i] = Math.round(i * 255 / 63);

async function startDemoViewer(opts) {
  const statusEl = document.getElementById('status');
  const fpsEl = document.getElementById('fps');
  const scale = opts.scale || 2;
  const cam = opts.camera ? Object.assign({}, opts.camera) : null;

  function clampCam() {
    if (!cam) return;
    cam.pitch = Math.min(cam.pitchMax, Math.max(cam.pitchMin, cam.pitch));
    cam.dist = Math.min(cam.distMax, Math.max(cam.distMin, cam.dist));
  }

  try {
    // WASI imports are unused, but stubs are needed to satisfy the linker
    const wasiStubs = new Proxy({}, {
      get: (_, name) => (name === 'proc_exit' ? () => { throw new Error('exit'); } : () => 0),
    });
    const resp = await fetch(opts.wasm);
    if (!resp.ok) throw new Error(`fetch failed: ${resp.status}`);
    const { instance } = await WebAssembly.instantiate(await resp.arrayBuffer(), {
      wasi_snapshot_preview1: wasiStubs,
    });
    const ex = instance.exports;
    const fn = (name) => ex[`${opts.prefix}_${name}`];
    if (ex._initialize) ex._initialize();
    fn('init')();

    const W = fn('get_width')();
    const H = fn('get_height')();
    const fbPtr = fn('get_fb')();
    const frameFn = fn('frame');

    const canvas = document.getElementById('screen');
    canvas.width = W;
    canvas.height = H;
    // Display size: W*scale wide, but never wider than the viewport; the height
    // follows from the canvas aspect ratio (style.css sets height: auto).
    canvas.style.width = `min(${W * scale}px, 100%)`;
    canvas.style.height = 'auto';
    canvas.style.aspectRatio = `${W} / ${H}`;
    const ctx = canvas.getContext('2d');
    const imgData = ctx.createImageData(W, H);
    const rgba = imgData.data;

    if (cam) setupCameraInput(canvas, cam, clampCam);

    // FPS counter
    let frames = 0;
    let fpsTime = performance.now();

    const t0 = performance.now();
    function frame() {
      const t = (performance.now() - t0) / 1000;
      if (cam) {
        frameFn(t, cam.yaw, cam.pitch, cam.dist);
      } else {
        frameFn(t);
      }

      // Re-create the view every frame in case the memory grows.
      // RGB565_SWAPPED on little-endian WebAssembly: byte 0 = RRRRRGGG,
      // byte 1 = GGGBBBBB
      const fb = new Uint8Array(ex.memory.buffer, fbPtr, W * H * 2);
      for (let i = 0, j = 0; i < W * H * 2; i += 2, j += 4) {
        const b0 = fb[i], b1 = fb[i + 1];
        rgba[j] = LUT5[b0 >> 3];
        rgba[j + 1] = LUT6[((b0 & 7) << 3) | (b1 >> 5)];
        rgba[j + 2] = LUT5[b1 & 31];
        rgba[j + 3] = 255;
      }
      ctx.putImageData(imgData, 0, 0);

      frames++;
      const now = performance.now();
      if (now - fpsTime >= 1000) {
        if (fpsEl) fpsEl.textContent = `${(frames * 1000 / (now - fpsTime)).toFixed(1)} fps`;
        frames = 0;
        fpsTime = now;
      }
      requestAnimationFrame(frame);
    }
    requestAnimationFrame(frame);
    if (statusEl) statusEl.textContent = '';
  } catch (e) {
    if (statusEl) {
      statusEl.textContent = `Error: ${e.message} - this page does not work from file://.` +
        ' Serve it over HTTP, e.g. "python3 -m http.server -d docs".';
    }
    throw e;
  }
}

function setupCameraInput(canvas, cam, clampCam) {
  // Drag to rotate, wheel to zoom
  let dragging = false, lastX = 0, lastY = 0;
  canvas.addEventListener('pointerdown', (e) => {
    dragging = true;
    lastX = e.clientX;
    lastY = e.clientY;
    canvas.setPointerCapture(e.pointerId);
  });
  canvas.addEventListener('pointermove', (e) => {
    if (!dragging) return;
    cam.yaw += (e.clientX - lastX) * 0.008;
    cam.pitch += (e.clientY - lastY) * 0.008;
    lastX = e.clientX;
    lastY = e.clientY;
    clampCam();
  });
  canvas.addEventListener('pointerup', () => { dragging = false; });
  canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    cam.dist *= Math.exp(e.deltaY * 0.001);
    clampCam();
  }, { passive: false });

  // Keyboard: arrows to rotate, PageUp/PageDown or +/- to zoom
  window.addEventListener('keydown', (e) => {
    const ROT = 0.08, ZOOM = 1.1;
    switch (e.key) {
      case 'ArrowLeft': cam.yaw -= ROT; break;
      case 'ArrowRight': cam.yaw += ROT; break;
      case 'ArrowUp': cam.pitch += ROT; break;
      case 'ArrowDown': cam.pitch -= ROT; break;
      case 'PageUp': case '+': case '=': cam.dist /= ZOOM; break;
      case 'PageDown': case '-': cam.dist *= ZOOM; break;
      default: return;
    }
    e.preventDefault();
    clampCam();
  });
}
