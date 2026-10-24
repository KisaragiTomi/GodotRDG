shader_type spatial;
render_mode diffuse_lambert;

uniform vec4 base_color : source_color = vec4(1.0);
uniform sampler2D albedo_texture : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D normal_texture : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D metallic_texture : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D roughness_texture : filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool has_normal = false;
uniform bool has_metallic = false;
uniform bool has_roughness = false;
uniform float normal_scale = 1.0;
uniform float metallic_factor = 0.0;
uniform float roughness_factor = 1.0;
uniform int metallic_channel = 2;
uniform int roughness_channel = 1;
uniform float alpha_cutoff = -1.0;
uniform sampler2D indirect_atlas : filter_linear, repeat_disable;
// 0 direct, 1 mesh cache, 2 native VoxelGI, 3 mesh indirect, 4 TG, 5 TG indirect.
uniform int gi_mode = 1;
uniform sampler2D tg_sh : filter_nearest, repeat_disable;
uniform sampler2D tg_position : filter_nearest, repeat_disable;
uniform sampler2D tg_normal : filter_nearest, repeat_disable;
uniform bool tg_stable_denoise = true;

vec3 tg_irradiance(vec2 screen_uv, vec3 world_position, vec3 world_normal, vec3 surface_normal) {
    ivec2 size = textureSize(tg_position, 0);
    vec2 pixel = screen_uv * vec2(size) - (tg_stable_denoise ? 0.75 : 0.5);
    ivec2 base = ivec2(floor(pixel));
    vec2 fraction = fract(pixel);
    vec4 r = vec4(0.0), g = vec4(0.0), b = vec4(0.0);
    float weight_sum = 0.0;
    // The final receiver uses its actual position and normal, never a UV2 cell.
    for (int j = -1; j <= 2; j++) {
        for (int i = -1; i <= 2; i++) {
            ivec2 tap = clamp(base + ivec2(i, j), ivec2(0), size - ivec2(1));
            vec4 guide = texelFetch(tg_position, tap, 0);
            vec3 normal = texelFetch(tg_normal, tap, 0).xyz;
            vec3 delta = guide.xyz - world_position;
            vec3 guide_normal = tg_stable_denoise ? surface_normal : world_normal;
            float plane = max(abs(dot(delta, guide_normal)), abs(dot(delta, normal)));
            vec2 offset = vec2(float(i),float(j)) - fraction;
            float weight = exp(-dot(offset, offset));
            weight *= exp(-plane / (0.015 + 0.002 * guide.w));
            weight *= pow(max(dot(normal, guide_normal), 0.0), 8.0);
            if (guide.w <= 0.0) weight = 0.0;
            r += texelFetch(tg_sh, tap, 0) * weight;
            g += texelFetch(tg_sh, tap + ivec2(size.x,0), 0) * weight;
            b += texelFetch(tg_sh, tap + ivec2(size.x*2,0), 0) * weight;
            weight_sum += weight;
        }
    }
    vec4 diffuse_basis = vec4(0.282094792, world_normal * 0.325735008);
    return max(vec3(dot(r,diffuse_basis),dot(g,diffuse_basis),dot(b,diffuse_basis)) / max(weight_sum,0.00001), vec3(0.0));
}

float channel(vec4 value, int index) {
    if (index == 0) return value.r;
    if (index == 1) return value.g;
    if (index == 2) return value.b;
    return value.a;
}

void fragment() {
    vec4 base = texture(albedo_texture, UV) * base_color;
    if (base.a < alpha_cutoff) discard;
    ALBEDO = base.rgb;
    METALLIC = metallic_factor * (has_metallic ? channel(texture(metallic_texture, UV), metallic_channel) : 1.0);
    ROUGHNESS = roughness_factor * (has_roughness ? channel(texture(roughness_texture, UV), roughness_channel) : 1.0);
    if (has_normal) {
        NORMAL_MAP = texture(normal_texture, UV).rgb;
        NORMAL_MAP_DEPTH = normal_scale;
    }
    vec3 incoming = textureLod(indirect_atlas, UV2, 0.0).rgb;
    if (gi_mode >= 4) {
        vec3 receiver_normal = NORMAL;
        if (has_normal) {
            vec3 map = texture(normal_texture, UV).rgb;
            map.xy = map.xy * 2.0 - 1.0;
            map.z = sqrt(max(0.0, 1.0 - dot(map.xy,map.xy)));
            receiver_normal = normalize(mix(NORMAL, TANGENT * map.x + BINORMAL * map.y + NORMAL * map.z, normal_scale));
        }
        incoming = tg_irradiance(SCREEN_UV, (INV_VIEW_MATRIX * vec4(VERTEX,1.0)).xyz, normalize(mat3(INV_VIEW_MATRIX) * receiver_normal), normalize(mat3(INV_VIEW_MATRIX) * NORMAL));
    }
    IRRADIANCE = vec4((gi_mode == 1 || gi_mode == 4) ? incoming : vec3(0.0), gi_mode == 2 ? 0.0 : 1.0);
    if (gi_mode == 3 || gi_mode == 5) {
        ALBEDO = vec3(0.0);
        EMISSION = base.rgb * (1.0 - METALLIC) * incoming;
        METALLIC = 0.0;
        SPECULAR = 0.0;
    }
}
