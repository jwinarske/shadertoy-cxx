// Default shadertoy-cxx shader — animated kaleidoscopic fractal (iTime only).
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = (2.0 * fragCoord - iResolution.xy) / iResolution.y;
    float t = iTime * 0.4;
    vec3 col = vec3(0.0);
    float d = 0.0;
    for (int i = 0; i < 6; i++) {
        float fi = float(i);
        uv = abs(uv) / dot(uv, uv) - 0.9;
        d += length(uv);
        col += (0.5 + 0.5 * cos(vec3(0.0, 1.0, 2.0) + fi * 0.6 + t + d)) * 0.12;
    }
    float v = 1.0 - 0.3 * length((fragCoord - 0.5 * iResolution.xy) / iResolution.y);
    fragColor = vec4(col * v, 1.0);
}
