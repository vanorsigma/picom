#version 330

// Automatic Variables
in vec2 texcoord;             // The current texture coordinate
uniform sampler2D tex;        // The window texture map

// Required internal Picom function signature for fallback/blending
vec4 default_post_processing(vec4 c);

vec4 window_shader() {
    vec2 texsize = textureSize(tex, 0);
    vec4 color = texture2D(tex, texcoord / texsize, 0);

    float gray = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
    return default_post_processing(vec4(vec3(gray), color.a));
}
