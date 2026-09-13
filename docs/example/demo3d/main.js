// demo3d viewer
// Loads demo3d.wasm (STANDALONE_WASM) and copies the RGB565 frame buffer to a canvas.

'use strict';

const SCALE = 2;

// RGB565 -> 8-bit expansion tables
const LUT5 = new Uint8Array(32);
const LUT6 = new Uint8Array(64);
for (let i = 0; i < 32; i++) LUT5[i] = Math.round(i * 255 / 31);
for (let i = 0; i < 64; i++) LUT6[i] = Math.round(i * 255 / 63);

// Camera state (initial values and limits match example/demo3d/scene.hpp)
const cam = {
  yaw: 0.6,
  pitch: 0.35,
  dist: 7.0,
};
const PITCH_MIN = -0.2, PITCH_MAX = 1.4;
const DIST_MIN = 3.0, DIST_MAX = 20.0;

function clampCam() {
  cam.pitch = Math.min(PITCH_MAX, Math.max(PITCH_MIN, cam.pitch));
  cam.dist = Math.min(DIST_MAX, Math.max(DIST_MIN, cam.dist));
}

async function main() {
  const statusEl = document.getElementById('status');
  try {
    // WASI imports are unused, but stubs are needed to satisfy the linker
    const wasiStubs = new Proxy({}, {
      get: (_, name) => (name === 'proc_exit' ? () => { throw new Error('exit'); } : () => 0),
    });
    const resp = await fetch('demo3d.wasm');
    if (!resp.ok) throw new Error(`fetch failed: ${resp.status}`);
    const { instance } = await WebAssembly.instantiate(await resp.arrayBuffer(), {
      wasi_snapshot_preview1: wasiStubs,
    });
    const ex = instance.exports;
    if (ex._initialize) ex._initialize();
    ex.demo3d_init();

    const W = ex.demo3d_get_width();
    const H = ex.demo3d_get_height();
    const fbPtr = ex.demo3d_get_fb();

    const canvas = document.getElementById('screen');
    canvas.width = W;
    canvas.height = H;
    canvas.style.width = `${W * SCALE}px`;
    canvas.style.height = `${H * SCALE}px`;
    const ctx = canvas.getContext('2d');
    const imgData = ctx.createImageData(W, H);
    const rgba = imgData.data;

    setupInput(canvas);

    // FPS counter
    let frames = 0;
    let fpsTime = performance.now();
    const fpsEl = document.getElementById('fps');

    const t0 = performance.now();
    function frame() {
      const t = (performance.now() - t0) / 1000;
      ex.demo3d_frame(t, cam.yaw, cam.pitch, cam.dist);

      // Re-create the view every frame in case the memory grows
      const fb = new Uint16Array(ex.memory.buffer, fbPtr, W * H);
      for (let i = 0, j = 0; i < W * H; i++, j += 4) {
        const p = fb[i];
        rgba[j] = LUT5[(p >> 11) & 31];
        rgba[j + 1] = LUT6[(p >> 5) & 63];
        rgba[j + 2] = LUT5[p & 31];
        rgba[j + 3] = 255;
      }
      ctx.putImageData(imgData, 0, 0);

      frames++;
      const now = performance.now();
      if (now - fpsTime >= 1000) {
        fpsEl.textContent = `${(frames * 1000 / (now - fpsTime)).toFixed(1)} fps`;
        frames = 0;
        fpsTime = now;
      }
      requestAnimationFrame(frame);
    }
    requestAnimationFrame(frame);
    statusEl.textContent = '';
  } catch (e) {
    statusEl.textContent = `Error: ${e.message} - this page does not work from file://.` +
      ' Serve it over HTTP, e.g. "python3 -m http.server -d docs".';
    throw e;
  }
}

function setupInput(canvas) {
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

main();
