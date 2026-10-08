// SPDX-License-Identifier: GPL-3.0-or-later
// Phone test: compiles each pipeline on this worker's own device first, so the
// page's device finds the compiled Metal shader in the system cache.
let device;
const modules = new Map(); // WGSL source -> module
const layouts = new Map(); // JSON of bind group layout descriptors -> pipeline layout
function moduleFor(code) {
  let module = modules.get(code);
  if (!module) modules.set(code, (module = device.createShaderModule({ code })));
  return module;
}
function layoutFor(layout) {
  if (layout === 'auto') return 'auto';
  const key = JSON.stringify(layout);
  let result = layouts.get(key);
  if (!result) {
    result = device.createPipelineLayout({
      bindGroupLayouts: layout.map((entries) => device.createBindGroupLayout(entries)),
    });
    layouts.set(key, result);
  }
  return result;
}
onmessage = async ({ data: m }) => {
  if (m.type === 'init') {
    try {
      if (!navigator.gpu) throw Error('no WebGPU in workers');
      const adapter = await navigator.gpu.requestAdapter(m.adapterOptions);
      if (!adapter) throw Error('no adapter');
      device = await adapter.requestDevice(m.deviceDescriptor);
      device.lost.then((info) => postMessage({ type: 'lost', message: info.message }));
      postMessage({ type: 'ready' });
    } catch (error) {
      postMessage({ type: 'failed', message: String(error) });
    }
    return;
  }
  const t0 = performance.now();
  try {
    const d = m.descriptor;
    await device.createRenderPipelineAsync({
      ...d,
      layout: layoutFor(d.layout),
      vertex: { ...d.vertex, module: moduleFor(d.vertex.module) },
      fragment: d.fragment && { ...d.fragment, module: moduleFor(d.fragment.module) },
    });
    postMessage({ type: 'done', id: m.id, ms: performance.now() - t0 });
  } catch (error) {
    postMessage({ type: 'done', id: m.id, ms: performance.now() - t0, error: String(error) });
  }
};
