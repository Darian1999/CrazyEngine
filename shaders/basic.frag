#version 330 core

in vec3 vertexColor;
in vec2 vertexUV;

uniform sampler2D uTexture;

out vec4 FragColor;

void main() {
    vec4 tex = texture(uTexture, vertexUV);
    // Mix vertex color with texture so each mesh keeps its tint while showing the texture.
    FragColor = mix(tex, vec4(vertexColor, 1.0), 0.25);
}
