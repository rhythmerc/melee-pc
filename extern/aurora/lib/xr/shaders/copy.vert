#version 450
// Full-screen triangle for the bridge's shader copy (xr.cpp, copy_latest).
void main() {
  vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
