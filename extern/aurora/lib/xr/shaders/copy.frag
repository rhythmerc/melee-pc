#version 450
// Copies the shared image into an *_SRGB swapchain image texel for texel.
// The shared image holds already sRGB-encoded bytes in a UNORM image, and the
// sRGB attachment encodes what we write, so decode first: the 8-bit round
// trip returns the original bytes. Alpha is linear and passes straight
// through.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(location = 0) out vec4 color;
vec3 to_linear(vec3 c) {
  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}
void main() {
  vec4 c = texelFetch(src, ivec2(gl_FragCoord.xy), 0);
  color = vec4(to_linear(c.rgb), c.a);
}
