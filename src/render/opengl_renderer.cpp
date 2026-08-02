#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "heartstead/render/opengl_renderer.hpp"
#include "heartstead/voxel/block_registry.hpp"
#include "heartstead/voxel/chunk.hpp"

#include "ui/pixel_ui.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <array>
#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace heartstead {
namespace {

#if defined(_WIN32)
#define HS_GL_CALL __stdcall
#else
#define HS_GL_CALL
#endif

using GlEnum = std::uint32_t;
using GlUInt = std::uint32_t;
using GlUInt64 = std::uint64_t;
using GlInt = std::int32_t;
using GlSize = std::ptrdiff_t;
using GlChar = char;
using GlBoolean = std::uint8_t;
using GlFloat = float;

constexpr GlEnum gl_array_buffer = 0x8892;
constexpr GlEnum gl_element_array_buffer = 0x8893;
constexpr GlEnum gl_copy_write_buffer = 0x8F37;
constexpr GlEnum gl_static_draw = 0x88E4;
constexpr GlEnum gl_dynamic_draw = 0x88E8;
constexpr GlEnum gl_short = 0x1402;
constexpr GlEnum gl_unsigned_short = 0x1403;
constexpr GlEnum gl_byte = 0x1400;
constexpr GlEnum gl_unsigned_int = 0x1405;
constexpr GlEnum gl_vertex_shader = 0x8B31;
constexpr GlEnum gl_fragment_shader = 0x8B30;
constexpr GlEnum gl_compile_status = 0x8B81;
constexpr GlEnum gl_link_status = 0x8B82;
constexpr GlEnum gl_depth_test = 0x0B71;
constexpr GlEnum gl_cull_face = 0x0B44;
constexpr GlEnum gl_sample_alpha_to_coverage = 0x809E;
constexpr GlEnum gl_blend = 0x0BE2;
constexpr GlEnum gl_src_alpha = 0x0302;
constexpr GlEnum gl_one_minus_src_alpha = 0x0303;
constexpr GlEnum gl_texture_2d = 0x0DE1;
constexpr GlEnum gl_texture0 = 0x84C0;
constexpr GlEnum gl_texture1 = 0x84C1;
constexpr GlEnum gl_texture2 = 0x84C2;
constexpr GlEnum gl_rgba = 0x1908;
constexpr GlEnum gl_depth_component = 0x1902;
constexpr GlEnum gl_depth_component24 = 0x81A6;
constexpr GlEnum gl_float = 0x1406;
constexpr GlEnum gl_unsigned_byte = 0x1401;
constexpr GlEnum gl_texture_min_filter = 0x2801;
constexpr GlEnum gl_texture_mag_filter = 0x2800;
constexpr GlEnum gl_texture_wrap_s = 0x2802;
constexpr GlEnum gl_texture_wrap_t = 0x2803;
constexpr GlEnum gl_nearest = 0x2600;
constexpr GlEnum gl_clamp_to_edge = 0x812F;
constexpr GlEnum gl_back = 0x0405;
constexpr GlEnum gl_ccw = 0x0901;
constexpr GlEnum gl_color_buffer_bit = 0x00004000;
constexpr GlEnum gl_depth_buffer_bit = 0x00000100;
constexpr GlEnum gl_triangles = 0x0004;
constexpr GlEnum gl_framebuffer = 0x8D40;
constexpr GlEnum gl_depth_attachment = 0x8D00;
constexpr GlEnum gl_framebuffer_complete = 0x8CD5;
constexpr GlEnum gl_none = 0;
constexpr GlEnum gl_any_samples_passed = 0x8C2F;
constexpr GlEnum gl_time_elapsed = 0x88BF;
constexpr GlEnum gl_query_result = 0x8866;
constexpr GlEnum gl_query_result_available = 0x8867;
constexpr GlBoolean gl_false = 0;
constexpr GlBoolean gl_true = 1;

struct GlApi {
    void (HS_GL_CALL *gen_vertex_arrays)(GlInt, GlUInt*){};
    void (HS_GL_CALL *bind_vertex_array)(GlUInt){};
    void (HS_GL_CALL *delete_vertex_arrays)(GlInt, const GlUInt*){};
    void (HS_GL_CALL *gen_buffers)(GlInt, GlUInt*){};
    void (HS_GL_CALL *bind_buffer)(GlEnum, GlUInt){};
    void (HS_GL_CALL *buffer_data)(GlEnum, GlSize, const void*, GlEnum){};
    void (HS_GL_CALL *buffer_sub_data)(GlEnum, GlSize, GlSize, const void*){};
    void (HS_GL_CALL *delete_buffers)(GlInt, const GlUInt*){};
    void (HS_GL_CALL *enable_vertex_attrib_array)(GlUInt){};
    void (HS_GL_CALL *vertex_attrib_i_pointer)(GlUInt, GlInt, GlEnum, GlInt, const void*){};
    GlUInt (HS_GL_CALL *create_shader)(GlEnum){};
    void (HS_GL_CALL *shader_source)(GlUInt, GlInt, const GlChar* const*, const GlInt*){};
    void (HS_GL_CALL *compile_shader)(GlUInt){};
    void (HS_GL_CALL *get_shader_iv)(GlUInt, GlEnum, GlInt*){};
    void (HS_GL_CALL *get_shader_info_log)(GlUInt, GlInt, GlInt*, GlChar*){};
    void (HS_GL_CALL *delete_shader)(GlUInt){};
    GlUInt (HS_GL_CALL *create_program)(){};
    void (HS_GL_CALL *attach_shader)(GlUInt, GlUInt){};
    void (HS_GL_CALL *link_program)(GlUInt){};
    void (HS_GL_CALL *get_program_iv)(GlUInt, GlEnum, GlInt*){};
    void (HS_GL_CALL *get_program_info_log)(GlUInt, GlInt, GlInt*, GlChar*){};
    void (HS_GL_CALL *delete_program)(GlUInt){};
    void (HS_GL_CALL *use_program)(GlUInt){};
    GlInt (HS_GL_CALL *get_uniform_location)(GlUInt, const GlChar*){};
    void (HS_GL_CALL *uniform_matrix_4fv)(GlInt, GlInt, GlBoolean, const GlFloat*){};
    void (HS_GL_CALL *uniform_3f)(GlInt, GlFloat, GlFloat, GlFloat){};
    void (HS_GL_CALL *uniform_2f)(GlInt, GlFloat, GlFloat){};
    void (HS_GL_CALL *uniform_1f)(GlInt, GlFloat){};
    void (HS_GL_CALL *enable)(GlEnum){};
    void (HS_GL_CALL *disable)(GlEnum){};
    void (HS_GL_CALL *blend_func)(GlEnum, GlEnum){};
    void (HS_GL_CALL *cull_face)(GlEnum){};
    void (HS_GL_CALL *front_face)(GlEnum){};
    void (HS_GL_CALL *clear_color)(GlFloat, GlFloat, GlFloat, GlFloat){};
    void (HS_GL_CALL *clear)(GlEnum){};
    void (HS_GL_CALL *viewport)(GlInt, GlInt, GlInt, GlInt){};
    void (HS_GL_CALL *draw_elements)(GlEnum, GlInt, GlEnum, const void*){};
    void (HS_GL_CALL *multi_draw_elements)(GlEnum, const GlInt*, GlEnum, const void* const*, GlInt){};
    void (HS_GL_CALL *draw_arrays)(GlEnum, GlInt, GlInt){};
    void (HS_GL_CALL *draw_arrays_instanced)(GlEnum, GlInt, GlInt, GlInt){};
    void (HS_GL_CALL *color_mask)(GlBoolean, GlBoolean, GlBoolean, GlBoolean){};
    void (HS_GL_CALL *depth_mask)(GlBoolean){};
    void (HS_GL_CALL *gen_queries)(GlInt, GlUInt*){};
    void (HS_GL_CALL *delete_queries)(GlInt, const GlUInt*){};
    void (HS_GL_CALL *begin_query)(GlEnum, GlUInt){};
    void (HS_GL_CALL *end_query)(GlEnum){};
    void (HS_GL_CALL *get_query_object_uiv)(GlUInt, GlEnum, GlUInt*){};
    void (HS_GL_CALL *get_query_object_ui64v)(GlUInt, GlEnum, GlUInt64*){};
    void (HS_GL_CALL *gen_textures)(GlInt, GlUInt*){};
    void (HS_GL_CALL *delete_textures)(GlInt, const GlUInt*){};
    void (HS_GL_CALL *bind_texture)(GlEnum, GlUInt){};
    void (HS_GL_CALL *tex_image_2d)(GlEnum, GlInt, GlInt, GlInt, GlInt, GlInt, GlEnum, GlEnum, const void*){};
    void (HS_GL_CALL *tex_parameter_i)(GlEnum, GlEnum, GlInt){};
    void (HS_GL_CALL *active_texture)(GlEnum){};
    void (HS_GL_CALL *uniform_1i)(GlInt, GlInt){};
    void (HS_GL_CALL *gen_framebuffers)(GlInt, GlUInt*){};
    void (HS_GL_CALL *bind_framebuffer)(GlEnum, GlUInt){};
    void (HS_GL_CALL *framebuffer_texture_2d)(GlEnum, GlEnum, GlEnum, GlUInt, GlInt){};
    GlEnum (HS_GL_CALL *check_framebuffer_status)(GlEnum){};
    void (HS_GL_CALL *delete_framebuffers)(GlInt, const GlUInt*){};
    void (HS_GL_CALL *draw_buffer)(GlEnum){};
    void (HS_GL_CALL *read_buffer)(GlEnum){};
};

GlApi gl;

template <typename Function>
Function load(const char* name) {
    const auto address = glfwGetProcAddress(name);
    if (address == nullptr) {
        throw std::runtime_error(std::string("OpenGL function unavailable: ") + name);
    }
    return reinterpret_cast<Function>(address);
}

void load_api() {
#define HS_LOAD(member, name) gl.member = load<decltype(gl.member)>(name)
    HS_LOAD(gen_vertex_arrays, "glGenVertexArrays");
    HS_LOAD(bind_vertex_array, "glBindVertexArray");
    HS_LOAD(delete_vertex_arrays, "glDeleteVertexArrays");
    HS_LOAD(gen_buffers, "glGenBuffers");
    HS_LOAD(bind_buffer, "glBindBuffer");
    HS_LOAD(buffer_data, "glBufferData");
    HS_LOAD(buffer_sub_data, "glBufferSubData");
    HS_LOAD(delete_buffers, "glDeleteBuffers");
    HS_LOAD(enable_vertex_attrib_array, "glEnableVertexAttribArray");
    HS_LOAD(vertex_attrib_i_pointer, "glVertexAttribIPointer");
    HS_LOAD(create_shader, "glCreateShader");
    HS_LOAD(shader_source, "glShaderSource");
    HS_LOAD(compile_shader, "glCompileShader");
    HS_LOAD(get_shader_iv, "glGetShaderiv");
    HS_LOAD(get_shader_info_log, "glGetShaderInfoLog");
    HS_LOAD(delete_shader, "glDeleteShader");
    HS_LOAD(create_program, "glCreateProgram");
    HS_LOAD(attach_shader, "glAttachShader");
    HS_LOAD(link_program, "glLinkProgram");
    HS_LOAD(get_program_iv, "glGetProgramiv");
    HS_LOAD(get_program_info_log, "glGetProgramInfoLog");
    HS_LOAD(delete_program, "glDeleteProgram");
    HS_LOAD(use_program, "glUseProgram");
    HS_LOAD(get_uniform_location, "glGetUniformLocation");
    HS_LOAD(uniform_matrix_4fv, "glUniformMatrix4fv");
    HS_LOAD(uniform_3f, "glUniform3f");
    HS_LOAD(uniform_2f, "glUniform2f");
    HS_LOAD(uniform_1f, "glUniform1f");
    HS_LOAD(enable, "glEnable");
    HS_LOAD(disable, "glDisable");
    HS_LOAD(blend_func, "glBlendFunc");
    HS_LOAD(cull_face, "glCullFace");
    HS_LOAD(front_face, "glFrontFace");
    HS_LOAD(clear_color, "glClearColor");
    HS_LOAD(clear, "glClear");
    HS_LOAD(viewport, "glViewport");
    HS_LOAD(draw_elements, "glDrawElements");
    HS_LOAD(multi_draw_elements, "glMultiDrawElements");
    HS_LOAD(draw_arrays, "glDrawArrays");
    HS_LOAD(draw_arrays_instanced, "glDrawArraysInstanced");
    HS_LOAD(color_mask, "glColorMask");
    HS_LOAD(depth_mask, "glDepthMask");
    HS_LOAD(gen_queries, "glGenQueries");
    HS_LOAD(delete_queries, "glDeleteQueries");
    HS_LOAD(begin_query, "glBeginQuery");
    HS_LOAD(end_query, "glEndQuery");
    HS_LOAD(get_query_object_uiv, "glGetQueryObjectuiv");
    HS_LOAD(get_query_object_ui64v, "glGetQueryObjectui64v");
    HS_LOAD(gen_textures, "glGenTextures");
    HS_LOAD(delete_textures, "glDeleteTextures");
    HS_LOAD(bind_texture, "glBindTexture");
    HS_LOAD(tex_image_2d, "glTexImage2D");
    HS_LOAD(tex_parameter_i, "glTexParameteri");
    HS_LOAD(active_texture, "glActiveTexture");
    HS_LOAD(uniform_1i, "glUniform1i");
    HS_LOAD(gen_framebuffers, "glGenFramebuffers");
    HS_LOAD(bind_framebuffer, "glBindFramebuffer");
    HS_LOAD(framebuffer_texture_2d, "glFramebufferTexture2D");
    HS_LOAD(check_framebuffer_status, "glCheckFramebufferStatus");
    HS_LOAD(delete_framebuffers, "glDeleteFramebuffers");
    HS_LOAD(draw_buffer, "glDrawBuffer");
    HS_LOAD(read_buffer, "glReadBuffer");
#undef HS_LOAD
}

GlUInt compile_shader(GlEnum type, const char* source) {
    const auto shader = gl.create_shader(type);
    gl.shader_source(shader, 1, &source, nullptr);
    gl.compile_shader(shader);
    GlInt succeeded = 0;
    gl.get_shader_iv(shader, gl_compile_status, &succeeded);
    if (succeeded == 0) {
        std::string message(2048, '\0');
        GlInt length = 0;
        gl.get_shader_info_log(shader, static_cast<GlInt>(message.size()), &length, message.data());
        gl.delete_shader(shader);
        message.resize(static_cast<std::size_t>(length));
        throw std::runtime_error("OpenGL shader compilation failed: " + message);
    }
    return shader;
}

struct FrustumPlane {
    float x{};
    float y{};
    float z{};
    float distance{};
};

[[nodiscard]] std::array<FrustumPlane, 6> extract_frustum(const Matrix4& matrix) noexcept {
    const auto& m = matrix.values;
    const auto plane = [&](std::size_t row, float sign) noexcept {
        return FrustumPlane{
            m[3] + sign * m[row],
            m[7] + sign * m[4U + row],
            m[11] + sign * m[8U + row],
            m[15] + sign * m[12U + row],
        };
    };
    return {
        plane(0U, 1.0F), plane(0U, -1.0F),
        plane(1U, 1.0F), plane(1U, -1.0F),
        plane(2U, 1.0F), plane(2U, -1.0F),
    };
}

enum class FrustumRelation { outside, intersecting, inside };

[[nodiscard]] FrustumRelation box_frustum_relation(
    float minimum_x,
    float maximum_x,
    float minimum_z,
    float maximum_z,
    const std::array<FrustumPlane, 6>& planes) noexcept {
    constexpr float minimum_y = 0.0F;
    constexpr float maximum_y = 64.0F;
    auto relation = FrustumRelation::inside;
    for (const auto& plane : planes) {
        const auto positive_x = plane.x >= 0.0F ? maximum_x : minimum_x;
        const auto positive_y = plane.y >= 0.0F ? maximum_y : minimum_y;
        const auto positive_z = plane.z >= 0.0F ? maximum_z : minimum_z;
        if (plane.x * positive_x + plane.y * positive_y + plane.z * positive_z + plane.distance < 0.0F)
            return FrustumRelation::outside;

        const auto negative_x = plane.x >= 0.0F ? minimum_x : maximum_x;
        const auto negative_y = plane.y >= 0.0F ? minimum_y : maximum_y;
        const auto negative_z = plane.z >= 0.0F ? minimum_z : maximum_z;
        if (plane.x * negative_x + plane.y * negative_y + plane.z * negative_z + plane.distance < 0.0F)
            relation = FrustumRelation::intersecting;
    }
    return relation;
}

[[nodiscard]] bool chunk_intersects_frustum(
    const ChunkDrawRange& range,
    Float3 world_origin,
    const std::array<FrustumPlane, 6>& planes) noexcept {
    return box_frustum_relation(
        world_origin.x + static_cast<float>(range.minimum_x),
        world_origin.x + static_cast<float>(range.maximum_x),
        world_origin.z + static_cast<float>(range.minimum_z),
        world_origin.z + static_cast<float>(range.maximum_z),
        planes) != FrustumRelation::outside;
}

GlUInt create_program() {
    constexpr auto vertex_source = R"glsl(
#version 330 core
layout(location = 0) in ivec3 vertex_position;
layout(location = 1) in uint vertex_block;
layout(location = 2) in ivec3 vertex_normal;
layout(location = 3) in uvec2 vertex_uv;

uniform mat4 view_projection;
uniform mat4 light_view_projection;
uniform mat4 player_light_view_projection;
uniform vec3 world_origin;
uniform vec3 camera_position;
flat out uint block_id;
flat out vec3 surface_normal;
out vec3 world_position;
out vec4 shadow_position;
out vec4 player_shadow_position;

void main() {
    world_position = vec3(vertex_position) + world_origin;
    gl_Position = view_projection * vec4(world_position, 1.0);
    block_id = vertex_block;
    surface_normal = normalize(vec3(vertex_normal));
    shadow_position = light_view_projection * vec4(world_position, 1.0);
    player_shadow_position = player_light_view_projection * vec4(world_position, 1.0);
}
)glsl";

    constexpr auto fragment_source = R"glsl(
#version 330 core
flat in uint block_id;
flat in vec3 surface_normal;
in vec3 world_position;
in vec4 shadow_position;
in vec4 player_shadow_position;
uniform vec3 camera_position;
uniform vec3 shadow_caster_position;
uniform sampler2D shadow_map;
uniform sampler2D player_shadow_map;
uniform float distance_smoothing_start;
uniform float fog_start_fraction;
uniform float render_distance_blocks;
uniform float shadow_distance;
out vec4 fragment_color;

float pixel_hash(vec2 position, float seed) {
    uvec2 coordinate = uvec2(ivec2(floor(position)));
    uint value = coordinate.x * 0x9E3779B1u;
    value ^= coordinate.y * 0x85EBCA77u;
    value ^= uint(seed) * 0xC2B2AE3Du;
    value ^= value >> 16u;
    value *= 0x7FEB352Du;
    value ^= value >> 15u;
    value *= 0x846CA68Bu;
    value ^= value >> 16u;
    return float(value & 0x00FFFFFFu) * (1.0 / 16777215.0);
}

vec3 simple_block_color(uint id) {
    if (id == 1u) return vec3(0.43, 0.46, 0.51);
    if (id == 2u) return vec3(0.43, 0.26, 0.13);
    if (id == 3u) return vec3(0.23, 0.61, 0.19);
    if (id == 4u) return vec3(0.45, 0.78, 0.92);
    if (id == 5u) return vec3(0.39, 0.22, 0.09);
    if (id == 6u) return vec3(0.16, 0.49, 0.13);
    if (id == 7u) return vec3(0.74, 0.66, 0.40);
    if (id == 8u) return vec3(0.91, 0.94, 0.96);
    return vec3(0.85, 0.20, 0.65);
}

vec3 pixel_block_color(uint id, vec2 texel, float noise_value) {
    vec3 base = simple_block_color(id);
    if (id == 1u) {
        float chips = noise_value > 0.82 ? 0.11 : (noise_value < 0.16 ? -0.09 : 0.0);
        return base + vec3(chips);
    }
    if (id == 2u) {
        return base + vec3(0.07, 0.045, 0.018) * (noise_value > 0.58 ? 1.0 : -0.35);
    }
    if (id == 3u) {
        if (surface_normal.y < 0.5) {
            float grass_pixel = noise_value > 0.62 ? 1.0 : 0.0;
            return mix(vec3(0.39, 0.235, 0.11), base, 0.35 + grass_pixel * 0.42);
        }
        return base + vec3(-0.04, 0.09, -0.025) * (noise_value - 0.45);
    }
    if (id == 5u) {
        if (abs(surface_normal.y) > 0.5) {
            float ring = mod(max(abs(texel.x - 3.5), abs(texel.y - 3.5)), 2.0);
            return mix(vec3(0.28, 0.14, 0.055), vec3(0.53, 0.32, 0.13), ring);
        }
        float bark = mod(texel.x + floor(noise_value * 3.0), 3.0) == 0.0 ? -0.10 : 0.05;
        return base + vec3(bark, bark * 0.62, bark * 0.25);
    }
    if (id == 6u) {
        return base + vec3(-0.035, 0.12, -0.02) * (noise_value > 0.48 ? 1.0 : -0.5);
    }
    if (id == 7u) {
        return base + vec3(0.075, 0.055, 0.018) * (noise_value > 0.65 ? 1.0 : -0.45);
    }
    if (id == 8u) {
        return base - vec3(0.10, 0.075, 0.035) * (noise_value < 0.22 ? 1.0 : 0.0);
    }
    return base;
}

float shadow_comparison(vec2 coordinate, float receiver_depth, float bias) {
    float stored_depth = texture(shadow_map, coordinate).r;
    return receiver_depth - bias > stored_depth ? 1.0 : 0.0;
}

float sunlight_visibility(vec3 normal, vec3 light_direction, float camera_distance) {
    if (camera_distance >= shadow_distance) return 1.0;
    vec3 projected = shadow_position.xyz / shadow_position.w * 0.5 + 0.5;
    if (projected.x <= 0.0 || projected.x >= 1.0 || projected.y <= 0.0 || projected.y >= 1.0 ||
        projected.z <= 0.0 || projected.z >= 1.0) return 1.0;
    vec2 texel_size = 1.0 / vec2(textureSize(shadow_map, 0));
    float bias = max(0.00035, 0.0015 * (1.0 - max(dot(normal, light_direction), 0.0)));
    float shadowed;
    if (camera_distance > shadow_distance * 0.58) {
        shadowed = shadow_comparison(projected.xy, projected.z, bias);
    } else {
        const vec2 offsets[4] = vec2[4](
            vec2(-0.75, -0.75), vec2(0.75, -0.75),
            vec2(-0.75, 0.75), vec2(0.75, 0.75));
        shadowed = 0.0;
        for (int sample_index = 0; sample_index < 4; ++sample_index) {
            shadowed += shadow_comparison(
                projected.xy + offsets[sample_index] * texel_size, projected.z, bias);
        }
        shadowed *= 0.25;
    }
    float distance_fade = 1.0 - smoothstep(shadow_distance * 0.78, shadow_distance, camera_distance);
    return 1.0 - shadowed * 0.62 * distance_fade;
}

float player_sunlight_visibility(vec3 normal, vec3 light_direction) {
    float receiver_distance = distance(world_position.xz, shadow_caster_position.xz);
    if (receiver_distance >= 12.0) return 1.0;
    vec3 projected = player_shadow_position.xyz / player_shadow_position.w * 0.5 + 0.5;
    if (projected.x <= 0.0 || projected.x >= 1.0 || projected.y <= 0.0 || projected.y >= 1.0 ||
        projected.z <= 0.0 || projected.z >= 1.0) return 1.0;

    vec2 texel_size = 1.0 / vec2(textureSize(player_shadow_map, 0));
    float bias = max(0.00018, 0.00075 * (1.0 - max(dot(normal, light_direction), 0.0)));
    float shadowed = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float stored_depth = texture(
                player_shadow_map, projected.xy + vec2(float(x), float(y)) * texel_size).r;
            shadowed += projected.z - bias > stored_depth ? 1.0 : 0.0;
        }
    }
    shadowed *= 1.0 / 9.0;
    float distance_fade = 1.0 - smoothstep(8.0, 12.0, receiver_distance);
    return 1.0 - shadowed * 0.62 * distance_fade;
}

void main() {
    vec3 light_direction = normalize(vec3(0.45, 0.85, 0.30));
    float diffuse = max(dot(surface_normal, light_direction), 0.0);
    float camera_distance = distance(world_position, camera_position);

    // One-block terrace faces eventually become smaller than a pixel. Fade
    // their lighting contrast before that point to prevent radial moire dots.
    float geometric_detail = 1.0 - smoothstep(
        distance_smoothing_start,
        distance_smoothing_start + 320.0,
        camera_distance);
    float sun_visibility = min(
        sunlight_visibility(surface_normal, light_direction, camera_distance),
        player_sunlight_visibility(surface_normal, light_direction));
    float directional_light = 0.42 + diffuse * 0.58 * sun_visibility;
    float lighting = mix(0.84, directional_light, geometric_detail);
    vec2 surface_coordinate = abs(surface_normal.x) > 0.5
        ? world_position.zy
        : (abs(surface_normal.y) > 0.5 ? world_position.xz : world_position.xy);
    vec2 tile = floor(surface_coordinate);
    vec2 texel = floor(fract(surface_coordinate) * 8.0);
    float texture_noise = pixel_hash(texel + tile * 11.0, float(block_id) * 17.0);
    vec3 base_color = simple_block_color(block_id);
    vec3 detailed_color = pixel_block_color(block_id, texel, texture_noise);
    vec3 color = mix(base_color, detailed_color, geometric_detail) * lighting;

    float alpha = 1.0;
    if (block_id == 6u) {
        float leaf_mask = pixel_hash(texel + tile * 7.0, 91.0);
        if (geometric_detail > 0.2 && leaf_mask < 0.22) discard;
        alpha = mix(1.0, 0.70, geometric_detail);
    }

    // The streamed circle is centered ahead of the camera. Finish fog before
    // its nearer rear edge, with extra room for an in-flight streaming shift.
    float look_ahead_blocks = clamp(render_distance_blocks / 32.0 - 2.0, 0.0, 8.0) * 32.0;
    float streaming_safety = clamp(render_distance_blocks * 0.125, 32.0, 128.0);
    float fog_end = max(24.0, render_distance_blocks - look_ahead_blocks - streaming_safety);
    float fog_start = min(fog_end - 16.0, fog_end * fog_start_fraction);
    fog_start = max(0.0, fog_start);
    float fog = smoothstep(fog_start, fog_end, camera_distance);
    vec3 sky_color = vec3(0.48, 0.72, 0.92);
    fragment_color = vec4(mix(color, sky_color, fog), mix(alpha, 1.0, fog));
}
)glsl";

    const auto vertex = compile_shader(gl_vertex_shader, vertex_source);
    const auto fragment = compile_shader(gl_fragment_shader, fragment_source);
    const auto program = gl.create_program();
    gl.attach_shader(program, vertex);
    gl.attach_shader(program, fragment);
    gl.link_program(program);
    gl.delete_shader(vertex);
    gl.delete_shader(fragment);

    GlInt succeeded = 0;
    gl.get_program_iv(program, gl_link_status, &succeeded);
    if (succeeded == 0) {
        std::string message(2048, '\0');
        GlInt length = 0;
        gl.get_program_info_log(program, static_cast<GlInt>(message.size()), &length, message.data());
        gl.delete_program(program);
        message.resize(static_cast<std::size_t>(length));
        throw std::runtime_error("OpenGL shader linking failed: " + message);
    }
    return program;
}

GlUInt create_ui_program() {
    constexpr auto vertex_source = R"glsl(
#version 330 core
out vec2 texture_coordinate;
void main() {
    const vec2 positions[6] = vec2[6](
        vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    const vec2 coordinates[6] = vec2[6](
        vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(1.0, 0.0),
        vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(0.0, 0.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
    texture_coordinate = coordinates[gl_VertexID];
}
)glsl";
    constexpr auto fragment_source = R"glsl(
#version 330 core
in vec2 texture_coordinate;
uniform sampler2D interface_texture;
out vec4 fragment_color;
void main() {
    fragment_color = texture(interface_texture, texture_coordinate);
}
)glsl";
    const auto vertex = compile_shader(gl_vertex_shader, vertex_source);
    const auto fragment = compile_shader(gl_fragment_shader, fragment_source);
    const auto program = gl.create_program();
    gl.attach_shader(program, vertex);
    gl.attach_shader(program, fragment);
    gl.link_program(program);
    gl.delete_shader(vertex);
    gl.delete_shader(fragment);
    GlInt succeeded = 0;
    gl.get_program_iv(program, gl_link_status, &succeeded);
    if (succeeded == 0) {
        gl.delete_program(program);
        throw std::runtime_error("Settings interface shader linking failed");
    }
    return program;
}

GlUInt create_occlusion_program() {
    constexpr auto vertex_source = R"glsl(
#version 330 core
uniform mat4 view_projection;
uniform vec3 bounds_minimum;
uniform int active_mask;
void main() {
    const vec3 corners[8] = vec3[8](
        vec3(0.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0),
        vec3(1.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0),
        vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 1.0),
        vec3(1.0, 1.0, 1.0), vec3(0.0, 1.0, 1.0));
    const int indices[36] = int[36](
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
        0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
        0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5);
    if ((active_mask & (1 << gl_InstanceID)) == 0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    int local_x = gl_InstanceID & 3;
    int local_z = gl_InstanceID >> 2;
    vec3 chunk_minimum = bounds_minimum + vec3(float(local_x * 32), 0.0, float(local_z * 32));
    vec3 chunk_maximum = chunk_minimum + vec3(32.0);
    vec3 position = mix(chunk_minimum, chunk_maximum, corners[indices[gl_VertexID]]);
    gl_Position = view_projection * vec4(position, 1.0);
}
)glsl";
    constexpr auto fragment_source = R"glsl(
#version 330 core
out vec4 fragment_color;
void main() { fragment_color = vec4(0.0); }
)glsl";
    const auto vertex = compile_shader(gl_vertex_shader, vertex_source);
    const auto fragment = compile_shader(gl_fragment_shader, fragment_source);
    const auto program = gl.create_program();
    gl.attach_shader(program, vertex);
    gl.attach_shader(program, fragment);
    gl.link_program(program);
    gl.delete_shader(vertex);
    gl.delete_shader(fragment);
    GlInt succeeded = 0;
    gl.get_program_iv(program, gl_link_status, &succeeded);
    if (succeeded == 0) {
        std::string message(2048, '\0');
        GlInt length = 0;
        gl.get_program_info_log(program, static_cast<GlInt>(message.size()), &length, message.data());
        gl.delete_program(program);
        message.resize(static_cast<std::size_t>(length));
        throw std::runtime_error("Occlusion shader linking failed: " + message);
    }
    return program;
}

GlUInt create_player_program() {
    constexpr auto vertex_source = R"glsl(
#version 330 core
uniform mat4 view_projection;
uniform vec3 player_position;
uniform float player_yaw;
flat out vec3 part_color;
void main() {
    const vec3 corners[8] = vec3[8](
        vec3(-0.5, -0.5, -0.5), vec3(0.5, -0.5, -0.5),
        vec3(0.5, 0.5, -0.5), vec3(-0.5, 0.5, -0.5),
        vec3(-0.5, -0.5, 0.5), vec3(0.5, -0.5, 0.5),
        vec3(0.5, 0.5, 0.5), vec3(-0.5, 0.5, 0.5));
    const int indices[36] = int[36](
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
        0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
        0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5);
    const vec3 centers[6] = vec3[6](
        vec3(0.0, 1.20, 0.0), vec3(0.0, 1.75, 0.0),
        vec3(-0.49, 1.18, 0.0), vec3(0.49, 1.18, 0.0),
        vec3(-0.21, 0.43, 0.0), vec3(0.21, 0.43, 0.0));
    const vec3 sizes[6] = vec3[6](
        vec3(0.74, 0.82, 0.40), vec3(0.50, 0.50, 0.50),
        vec3(0.25, 0.82, 0.27), vec3(0.25, 0.82, 0.27),
        vec3(0.31, 0.86, 0.33), vec3(0.31, 0.86, 0.33));
    const vec3 colors[6] = vec3[6](
        vec3(0.20, 0.34, 0.27), vec3(0.72, 0.52, 0.35),
        vec3(0.17, 0.29, 0.23), vec3(0.17, 0.29, 0.23),
        vec3(0.24, 0.18, 0.14), vec3(0.24, 0.18, 0.14));
    vec3 local_position = centers[gl_InstanceID] + corners[indices[gl_VertexID]] * sizes[gl_InstanceID];
    // Movement yaw uses +X as forward, while the model was authored with +Z
    // as forward. Rotate the model basis by 90 degrees so its chest, arms and
    // legs face the direction of travel instead of moving shoulder-first.
    float model_yaw = player_yaw - 1.57079632679;
    float cosine_yaw = cos(model_yaw);
    float sine_yaw = sin(model_yaw);
    vec3 rotated_position = vec3(
        local_position.x * cosine_yaw - local_position.z * sine_yaw,
        local_position.y,
        local_position.x * sine_yaw + local_position.z * cosine_yaw);
    gl_Position = view_projection * vec4(player_position + rotated_position, 1.0);
    part_color = colors[gl_InstanceID];
}
)glsl";
    constexpr auto fragment_source = R"glsl(
#version 330 core
flat in vec3 part_color;
out vec4 fragment_color;
void main() { fragment_color = vec4(part_color, 1.0); }
)glsl";
    const auto vertex = compile_shader(gl_vertex_shader, vertex_source);
    const auto fragment = compile_shader(gl_fragment_shader, fragment_source);
    const auto program = gl.create_program();
    gl.attach_shader(program, vertex);
    gl.attach_shader(program, fragment);
    gl.link_program(program);
    gl.delete_shader(vertex);
    gl.delete_shader(fragment);
    GlInt succeeded = 0;
    gl.get_program_iv(program, gl_link_status, &succeeded);
    if (succeeded == 0) {
        std::string message(2048, '\0');
        GlInt length = 0;
        gl.get_program_info_log(program, static_cast<GlInt>(message.size()), &length, message.data());
        gl.delete_program(program);
        message.resize(static_cast<std::size_t>(length));
        throw std::runtime_error("Player shader linking failed: " + message);
    }
    return program;
}

GlUInt create_shadow_program() {
    constexpr auto vertex_source = R"glsl(
#version 330 core
layout(location = 0) in ivec3 vertex_position;
uniform mat4 light_view_projection;
uniform vec3 world_origin;
void main() {
    gl_Position = light_view_projection * vec4(vec3(vertex_position) + world_origin, 1.0);
}
)glsl";
    constexpr auto fragment_source = R"glsl(
#version 330 core
void main() {}
)glsl";
    const auto vertex = compile_shader(gl_vertex_shader, vertex_source);
    const auto fragment = compile_shader(gl_fragment_shader, fragment_source);
    const auto program = gl.create_program();
    gl.attach_shader(program, vertex);
    gl.attach_shader(program, fragment);
    gl.link_program(program);
    gl.delete_shader(vertex);
    gl.delete_shader(fragment);
    GlInt succeeded = 0;
    gl.get_program_iv(program, gl_link_status, &succeeded);
    if (succeeded == 0) {
        gl.delete_program(program);
        throw std::runtime_error("Shadow shader linking failed");
    }
    return program;
}

GlUInt create_crosshair_program() {
    constexpr auto vertex_source = R"glsl(
#version 330 core
uniform vec2 framebuffer_size;
flat out vec3 crosshair_color;
void main() {
    const vec2 corners[6] = vec2[6](
        vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    const vec2 half_sizes[4] = vec2[4](
        vec2(7.0, 2.0), vec2(2.0, 7.0), vec2(6.0, 0.75), vec2(0.75, 6.0));
    int rectangle = gl_VertexID / 6;
    vec2 pixel_position = corners[gl_VertexID % 6] * half_sizes[rectangle];
    gl_Position = vec4(pixel_position * 2.0 / framebuffer_size, 0.0, 1.0);
    crosshair_color = rectangle < 2 ? vec3(0.05, 0.07, 0.08) : vec3(0.95);
}
)glsl";
    constexpr auto fragment_source = R"glsl(
#version 330 core
flat in vec3 crosshair_color;
out vec4 fragment_color;
void main() { fragment_color = vec4(crosshair_color, 1.0); }
)glsl";
    const auto vertex = compile_shader(gl_vertex_shader, vertex_source);
    const auto fragment = compile_shader(gl_fragment_shader, fragment_source);
    const auto program = gl.create_program();
    gl.attach_shader(program, vertex);
    gl.attach_shader(program, fragment);
    gl.link_program(program);
    gl.delete_shader(vertex);
    gl.delete_shader(fragment);
    GlInt succeeded = 0;
    gl.get_program_iv(program, gl_link_status, &succeeded);
    if (succeeded == 0) {
        gl.delete_program(program);
        throw std::runtime_error("Crosshair shader linking failed");
    }
    return program;
}

struct GpuSlice {
    std::size_t offset{};
    std::size_t size{};
};

class FreeListArena {
public:
    explicit FreeListArena(std::size_t capacity = 0) : capacity_(capacity) {
        if (capacity != 0) free_.push_back({0, capacity});
    }

    [[nodiscard]] std::optional<GpuSlice> allocate(std::size_t size, std::size_t alignment) {
        if (size == 0) return GpuSlice{};
        for (std::size_t index = 0; index < free_.size(); ++index) {
            const auto block = free_[index];
            const auto aligned = (block.offset + alignment - 1U) / alignment * alignment;
            const auto padding = aligned - block.offset;
            if (padding + size > block.size) continue;
            free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(index));
            if (padding != 0) free_.push_back({block.offset, padding});
            const auto tail = block.size - padding - size;
            if (tail != 0) free_.push_back({aligned + size, tail});
            return GpuSlice{aligned, size};
        }
        return std::nullopt;
    }

    void release(GpuSlice slice) {
        if (slice.size == 0) return;
        free_.push_back(slice);
        std::sort(free_.begin(), free_.end(), [](GpuSlice lhs, GpuSlice rhs) {
            return lhs.offset < rhs.offset;
        });
        std::size_t output = 0;
        for (const auto block : free_) {
            if (output != 0 && free_[output - 1U].offset + free_[output - 1U].size == block.offset) {
                free_[output - 1U].size += block.size;
            } else {
                free_[output++] = block;
            }
        }
        free_.resize(output);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    std::size_t capacity_{};
    std::vector<GpuSlice> free_;
};

} // namespace

struct OpenGlRenderer::StreamingStorage {
    struct Entry {
        GpuSlice vertices;
        GpuSlice opaque_indices;
        GpuSlice cutout_indices;
        ChunkDrawRange range;
    };

    StreamingStorage(std::size_t vertex_capacity, std::size_t opaque_capacity, std::size_t cutout_capacity)
        : vertex_arena(vertex_capacity), opaque_arena(opaque_capacity), cutout_arena(cutout_capacity) {}

    FreeListArena vertex_arena;
    FreeListArena opaque_arena;
    FreeListArena cutout_arena;
    std::unordered_map<Int3, Entry> entries;
    std::vector<MeshVertex> vertex_scratch;
    std::vector<std::uint32_t> opaque_scratch;
    std::vector<std::uint32_t> cutout_scratch;
};

struct OpenGlRenderer::OcclusionStorage {
    static constexpr std::int32_t chunks_per_group = 4;
    static constexpr std::size_t chunks_in_group =
        static_cast<std::size_t>(chunks_per_group * chunks_per_group);
    static constexpr std::uint32_t invalid_range = std::numeric_limits<std::uint32_t>::max();

    struct Group {
        Group() { range_indices.fill(invalid_range); }

        GlUInt query{};
        std::array<std::uint32_t, chunks_in_group> range_indices{};
        bool visible{true};
        bool pending{};
        bool candidate{};
        std::uint8_t occluded_confirmations{};
        std::uint32_t active_mask{};
        std::uint64_t issued_view_revision{};
    };

    GlUInt program{};
    GlUInt vertex_array{};
    GlInt view_projection_location{-1};
    GlInt bounds_minimum_location{-1};
    GlInt active_mask_location{-1};
    std::unordered_map<Int3, Group> groups;
    std::vector<Int3> query_candidates;
    std::vector<Int3> distance_order;
    std::size_t query_cursor{};
    Float3 reference_camera{};
    Float3 order_reference_camera{};
    std::uint64_t view_revision{1};
    std::uint32_t stable_frames{};
    bool has_reference_camera{};
    bool order_dirty{true};
    bool groups_dirty{true};
};

struct OpenGlRenderer::PlayerStorage {
    GlUInt program{};
    GlUInt vertex_array{};
    GlInt view_projection_location{-1};
    GlInt position_location{-1};
    GlInt yaw_location{-1};
};

struct OpenGlRenderer::ShadowStorage {
    static constexpr GlInt texture_size = 2048;
    static constexpr GlInt player_texture_size = 1024;
    GlUInt program{};
    GlUInt framebuffer{};
    GlUInt depth_texture{};
    GlUInt player_framebuffer{};
    GlUInt player_depth_texture{};
    GlInt light_view_projection_location{-1};
    GlInt world_origin_location{-1};
};

struct OpenGlRenderer::CrosshairStorage {
    GlUInt program{};
    GlUInt vertex_array{};
    GlInt framebuffer_size_location{-1};
};

struct OpenGlRenderer::GpuTimingStorage {
    static constexpr std::size_t query_count = 4;
    std::array<GlUInt, query_count> queries{};
    std::array<bool, query_count> pending{};
    std::size_t cursor{};
    std::size_t active_query{};
    double milliseconds{};
    bool measuring{};
};

VideoSettingsLayout VideoSettingsLayout::from_framebuffer(std::int32_t width, std::int32_t height) noexcept {
    const auto panel_width = std::clamp(width - 40, 360, 680);
    const auto panel_height = std::clamp(height - 40, 480, 560);
    const auto left = (width - panel_width) / 2;
    const auto top = (height - panel_height) / 2;
    return {
        .panel_left = left,
        .panel_top = top,
        .panel_right = left + panel_width,
        .panel_bottom = top + panel_height,
        .slider_left = left + 48,
        .slider_right = left + panel_width - 48,
        .render_distance_y = top + 190,
        .smoothing_y = top + 260,
        .fog_start_y = top + 330,
        .shadow_distance_y = top + 400,
        .vsync_y = top + 450,
        .fullscreen_y = top + 498,
        .toggle_left = left + panel_width - 158,
        .toggle_right = left + panel_width - 48,
        .render_value_left = left + panel_width - 210,
        .render_value_top = top + 128,
        .render_value_right = left + panel_width - 48,
        .render_value_bottom = top + 162,
    };
}

OpenGlRenderer::OpenGlRenderer(const WorldMesh& mesh, Float3 world_origin)
    : world_origin_(world_origin) {
    load_api();
    program_ = create_program();
    ui_program_ = create_ui_program();
    view_projection_location_ = gl.get_uniform_location(program_, "view_projection");
    light_view_projection_location_ = gl.get_uniform_location(program_, "light_view_projection");
    player_light_view_projection_location_ =
        gl.get_uniform_location(program_, "player_light_view_projection");
    world_origin_location_ = gl.get_uniform_location(program_, "world_origin");
    camera_position_location_ = gl.get_uniform_location(program_, "camera_position");
    shadow_caster_position_location_ =
        gl.get_uniform_location(program_, "shadow_caster_position");
    distance_smoothing_start_location_ = gl.get_uniform_location(program_, "distance_smoothing_start");
    fog_start_fraction_location_ = gl.get_uniform_location(program_, "fog_start_fraction");
    render_distance_blocks_location_ = gl.get_uniform_location(program_, "render_distance_blocks");
    shadow_distance_location_ = gl.get_uniform_location(program_, "shadow_distance");
    shadow_map_location_ = gl.get_uniform_location(program_, "shadow_map");
    player_shadow_map_location_ = gl.get_uniform_location(program_, "player_shadow_map");
    ui_sampler_location_ = gl.get_uniform_location(ui_program_, "interface_texture");

    const auto reserve_capacity = [](std::size_t used, std::size_t minimum_slack) {
        const auto capacity = used + std::max(used / 2U, minimum_slack);
        return (capacity + 4095U) / 4096U * 4096U;
    };
    const auto vertex_capacity = reserve_capacity(mesh.vertices.size() * sizeof(MeshVertex), 32U * 1024U * 1024U);
    const auto opaque_capacity = reserve_capacity(mesh.opaque_indices.size() * sizeof(std::uint32_t), 16U * 1024U * 1024U);
    const auto cutout_capacity = reserve_capacity(mesh.cutout_indices.size() * sizeof(std::uint32_t), 8U * 1024U * 1024U);
    streaming_storage_ = std::make_unique<StreamingStorage>(
        vertex_capacity, opaque_capacity, cutout_capacity);
    occlusion_storage_ = std::make_unique<OcclusionStorage>();
    occlusion_storage_->program = create_occlusion_program();
    occlusion_storage_->view_projection_location =
        gl.get_uniform_location(occlusion_storage_->program, "view_projection");
    occlusion_storage_->bounds_minimum_location =
        gl.get_uniform_location(occlusion_storage_->program, "bounds_minimum");
    occlusion_storage_->active_mask_location =
        gl.get_uniform_location(occlusion_storage_->program, "active_mask");
    gl.gen_vertex_arrays(1, &occlusion_storage_->vertex_array);
    player_storage_ = std::make_unique<PlayerStorage>();
    player_storage_->program = create_player_program();
    player_storage_->view_projection_location =
        gl.get_uniform_location(player_storage_->program, "view_projection");
    player_storage_->position_location =
        gl.get_uniform_location(player_storage_->program, "player_position");
    player_storage_->yaw_location = gl.get_uniform_location(player_storage_->program, "player_yaw");
    gl.gen_vertex_arrays(1, &player_storage_->vertex_array);
    shadow_storage_ = std::make_unique<ShadowStorage>();
    shadow_storage_->program = create_shadow_program();
    shadow_storage_->light_view_projection_location =
        gl.get_uniform_location(shadow_storage_->program, "light_view_projection");
    shadow_storage_->world_origin_location =
        gl.get_uniform_location(shadow_storage_->program, "world_origin");
    gl.gen_textures(1, &shadow_storage_->depth_texture);
    gl.bind_texture(gl_texture_2d, shadow_storage_->depth_texture);
    gl.tex_image_2d(gl_texture_2d, 0, static_cast<GlInt>(gl_depth_component24),
        ShadowStorage::texture_size, ShadowStorage::texture_size, 0,
        gl_depth_component, gl_float, nullptr);
    gl.tex_parameter_i(gl_texture_2d, gl_texture_min_filter, static_cast<GlInt>(gl_nearest));
    gl.tex_parameter_i(gl_texture_2d, gl_texture_mag_filter, static_cast<GlInt>(gl_nearest));
    gl.tex_parameter_i(gl_texture_2d, gl_texture_wrap_s, static_cast<GlInt>(gl_clamp_to_edge));
    gl.tex_parameter_i(gl_texture_2d, gl_texture_wrap_t, static_cast<GlInt>(gl_clamp_to_edge));
    gl.gen_framebuffers(1, &shadow_storage_->framebuffer);
    gl.bind_framebuffer(gl_framebuffer, shadow_storage_->framebuffer);
    gl.framebuffer_texture_2d(gl_framebuffer, gl_depth_attachment, gl_texture_2d,
        shadow_storage_->depth_texture, 0);
    gl.draw_buffer(gl_none);
    gl.read_buffer(gl_none);
    if (gl.check_framebuffer_status(gl_framebuffer) != gl_framebuffer_complete)
        throw std::runtime_error("Could not create the sun shadow framebuffer");

    gl.gen_textures(1, &shadow_storage_->player_depth_texture);
    gl.bind_texture(gl_texture_2d, shadow_storage_->player_depth_texture);
    gl.tex_image_2d(gl_texture_2d, 0, static_cast<GlInt>(gl_depth_component24),
        ShadowStorage::player_texture_size, ShadowStorage::player_texture_size, 0,
        gl_depth_component, gl_float, nullptr);
    gl.tex_parameter_i(gl_texture_2d, gl_texture_min_filter, static_cast<GlInt>(gl_nearest));
    gl.tex_parameter_i(gl_texture_2d, gl_texture_mag_filter, static_cast<GlInt>(gl_nearest));
    gl.tex_parameter_i(gl_texture_2d, gl_texture_wrap_s, static_cast<GlInt>(gl_clamp_to_edge));
    gl.tex_parameter_i(gl_texture_2d, gl_texture_wrap_t, static_cast<GlInt>(gl_clamp_to_edge));
    gl.gen_framebuffers(1, &shadow_storage_->player_framebuffer);
    gl.bind_framebuffer(gl_framebuffer, shadow_storage_->player_framebuffer);
    gl.framebuffer_texture_2d(gl_framebuffer, gl_depth_attachment, gl_texture_2d,
        shadow_storage_->player_depth_texture, 0);
    gl.draw_buffer(gl_none);
    gl.read_buffer(gl_none);
    if (gl.check_framebuffer_status(gl_framebuffer) != gl_framebuffer_complete)
        throw std::runtime_error("Could not create the player shadow framebuffer");
    gl.bind_framebuffer(gl_framebuffer, 0);
    crosshair_storage_ = std::make_unique<CrosshairStorage>();
    crosshair_storage_->program = create_crosshair_program();
    crosshair_storage_->framebuffer_size_location =
        gl.get_uniform_location(crosshair_storage_->program, "framebuffer_size");
    gl.gen_vertex_arrays(1, &crosshair_storage_->vertex_array);
    gpu_timing_storage_ = std::make_unique<GpuTimingStorage>();
    gl.gen_queries(static_cast<GlInt>(GpuTimingStorage::query_count),
        gpu_timing_storage_->queries.data());

    gl.gen_buffers(1, &vertex_buffer_);
    gl.bind_buffer(gl_array_buffer, vertex_buffer_);
    gl.buffer_data(gl_array_buffer, static_cast<GlSize>(vertex_capacity), nullptr, gl_dynamic_draw);

    gl.gen_buffers(1, &opaque_index_buffer_);
    gl.bind_buffer(gl_element_array_buffer, opaque_index_buffer_);
    gl.buffer_data(gl_element_array_buffer, static_cast<GlSize>(opaque_capacity), nullptr, gl_dynamic_draw);

    gl.gen_buffers(1, &cutout_index_buffer_);
    gl.bind_buffer(gl_element_array_buffer, cutout_index_buffer_);
    gl.buffer_data(gl_element_array_buffer, static_cast<GlSize>(cutout_capacity), nullptr, gl_dynamic_draw);

    constexpr auto stride = static_cast<GlInt>(sizeof(MeshVertex));
    const auto configure_vertex_array = [&](std::uint32_t& vertex_array, std::uint32_t index_buffer) {
        gl.gen_vertex_arrays(1, &vertex_array);
        gl.bind_vertex_array(vertex_array);
        gl.bind_buffer(gl_array_buffer, vertex_buffer_);
        gl.bind_buffer(gl_element_array_buffer, index_buffer);
        gl.enable_vertex_attrib_array(0);
        gl.vertex_attrib_i_pointer(0, 3, gl_short, stride, reinterpret_cast<const void*>(offsetof(MeshVertex, x)));
        gl.enable_vertex_attrib_array(1);
        gl.vertex_attrib_i_pointer(1, 1, gl_unsigned_short, stride, reinterpret_cast<const void*>(offsetof(MeshVertex, block)));
        gl.enable_vertex_attrib_array(2);
        gl.vertex_attrib_i_pointer(2, 3, gl_byte, stride, reinterpret_cast<const void*>(offsetof(MeshVertex, normal_x)));
        gl.enable_vertex_attrib_array(3);
        gl.vertex_attrib_i_pointer(3, 2, gl_unsigned_short, stride, reinterpret_cast<const void*>(offsetof(MeshVertex, texture_u)));
    };
    configure_vertex_array(opaque_vertex_array_, opaque_index_buffer_);
    configure_vertex_array(cutout_vertex_array_, cutout_index_buffer_);

    auto& storage = *streaming_storage_;
    for (const auto& source_range : mesh.chunks) {
        const auto vertex_bytes = static_cast<std::size_t>(source_range.vertex_count) * sizeof(MeshVertex);
        const auto opaque_bytes = static_cast<std::size_t>(source_range.opaque_index_count) * sizeof(std::uint32_t);
        const auto cutout_bytes = static_cast<std::size_t>(source_range.cutout_index_count) * sizeof(std::uint32_t);
        const auto vertex_slice = storage.vertex_arena.allocate(vertex_bytes, alignof(MeshVertex));
        const auto opaque_slice = storage.opaque_arena.allocate(opaque_bytes, alignof(std::uint32_t));
        const auto cutout_slice = storage.cutout_arena.allocate(cutout_bytes, alignof(std::uint32_t));
        if (!vertex_slice || !opaque_slice || !cutout_slice) {
            throw std::runtime_error("Initial streaming GPU arena is too small");
        }
        if (vertex_bytes != 0) {
            gl.bind_buffer(gl_array_buffer, vertex_buffer_);
            gl.buffer_sub_data(gl_array_buffer, static_cast<GlSize>(vertex_slice->offset),
                static_cast<GlSize>(vertex_bytes), mesh.vertices.data() + source_range.first_vertex);
        }
        const auto vertex_base = static_cast<std::uint32_t>(vertex_slice->offset / sizeof(MeshVertex));
        storage.opaque_scratch.resize(source_range.opaque_index_count);
        for (std::size_t index = 0; index < storage.opaque_scratch.size(); ++index) {
            storage.opaque_scratch[index] = vertex_base +
                mesh.opaque_indices[source_range.opaque_first_index + index] - source_range.first_vertex;
        }
        if (opaque_bytes != 0) {
            gl.bind_buffer(gl_copy_write_buffer, opaque_index_buffer_);
            gl.buffer_sub_data(gl_copy_write_buffer, static_cast<GlSize>(opaque_slice->offset),
                static_cast<GlSize>(opaque_bytes), storage.opaque_scratch.data());
        }
        storage.cutout_scratch.resize(source_range.cutout_index_count);
        for (std::size_t index = 0; index < storage.cutout_scratch.size(); ++index) {
            storage.cutout_scratch[index] = vertex_base +
                mesh.cutout_indices[source_range.cutout_first_index + index] - source_range.first_vertex;
        }
        if (cutout_bytes != 0) {
            gl.bind_buffer(gl_copy_write_buffer, cutout_index_buffer_);
            gl.buffer_sub_data(gl_copy_write_buffer, static_cast<GlSize>(cutout_slice->offset),
                static_cast<GlSize>(cutout_bytes), storage.cutout_scratch.data());
        }
        auto range = source_range;
        range.first_vertex = vertex_base;
        range.opaque_first_index = static_cast<std::uint32_t>(opaque_slice->offset / sizeof(std::uint32_t));
        range.cutout_first_index = static_cast<std::uint32_t>(cutout_slice->offset / sizeof(std::uint32_t));
        storage.entries.emplace(source_range.coordinates, StreamingStorage::Entry{
            *vertex_slice, *opaque_slice, *cutout_slice, range});
        draw_ranges_.push_back(range);
    }

    visible_ranges_.reserve(draw_ranges_.size());
    opaque_draw_counts_.reserve(draw_ranges_.size());
    opaque_draw_offsets_.reserve(draw_ranges_.size());
    cutout_draw_counts_.reserve(draw_ranges_.size());
    cutout_draw_offsets_.reserve(draw_ranges_.size());
    shadow_opaque_draw_counts_.reserve(draw_ranges_.size());
    shadow_opaque_draw_offsets_.reserve(draw_ranges_.size());
    shadow_cutout_draw_counts_.reserve(draw_ranges_.size());
    shadow_cutout_draw_offsets_.reserve(draw_ranges_.size());

    gl.gen_textures(1, &ui_texture_);
    gl.bind_texture(gl_texture_2d, ui_texture_);
    gl.tex_parameter_i(gl_texture_2d, gl_texture_min_filter, static_cast<GlInt>(gl_nearest));
    gl.tex_parameter_i(gl_texture_2d, gl_texture_mag_filter, static_cast<GlInt>(gl_nearest));
    gl.tex_parameter_i(gl_texture_2d, gl_texture_wrap_s, static_cast<GlInt>(gl_clamp_to_edge));
    gl.tex_parameter_i(gl_texture_2d, gl_texture_wrap_t, static_cast<GlInt>(gl_clamp_to_edge));

    gl.gen_textures(1, &menu_texture_);
    gl.bind_texture(gl_texture_2d, menu_texture_);
    gl.tex_parameter_i(gl_texture_2d, gl_texture_min_filter, static_cast<GlInt>(gl_nearest));
    gl.tex_parameter_i(gl_texture_2d, gl_texture_mag_filter, static_cast<GlInt>(gl_nearest));
    gl.tex_parameter_i(gl_texture_2d, gl_texture_wrap_s, static_cast<GlInt>(gl_clamp_to_edge));
    gl.tex_parameter_i(gl_texture_2d, gl_texture_wrap_t, static_cast<GlInt>(gl_clamp_to_edge));

    gl.gen_textures(1, &debug_texture_);
    gl.bind_texture(gl_texture_2d, debug_texture_);
    gl.tex_parameter_i(gl_texture_2d, gl_texture_min_filter, static_cast<GlInt>(gl_nearest));
    gl.tex_parameter_i(gl_texture_2d, gl_texture_mag_filter, static_cast<GlInt>(gl_nearest));
    gl.tex_parameter_i(gl_texture_2d, gl_texture_wrap_s, static_cast<GlInt>(gl_clamp_to_edge));
    gl.tex_parameter_i(gl_texture_2d, gl_texture_wrap_t, static_cast<GlInt>(gl_clamp_to_edge));

    gl.enable(gl_depth_test);
    gl.enable(gl_cull_face);
    gl.enable(gl_sample_alpha_to_coverage);
    gl.cull_face(gl_back);
    gl.front_face(gl_ccw);
    gl.clear_color(0.48F, 0.72F, 0.92F, 1.0F);
}

OpenGlRenderer::~OpenGlRenderer() {
    if (gpu_timing_storage_)
        gl.delete_queries(static_cast<GlInt>(GpuTimingStorage::query_count),
            gpu_timing_storage_->queries.data());
    if (crosshair_storage_) {
        if (crosshair_storage_->vertex_array != 0)
            gl.delete_vertex_arrays(1, &crosshair_storage_->vertex_array);
        if (crosshair_storage_->program != 0) gl.delete_program(crosshair_storage_->program);
    }
    if (shadow_storage_) {
        if (shadow_storage_->player_framebuffer != 0)
            gl.delete_framebuffers(1, &shadow_storage_->player_framebuffer);
        if (shadow_storage_->player_depth_texture != 0)
            gl.delete_textures(1, &shadow_storage_->player_depth_texture);
        if (shadow_storage_->framebuffer != 0)
            gl.delete_framebuffers(1, &shadow_storage_->framebuffer);
        if (shadow_storage_->depth_texture != 0)
            gl.delete_textures(1, &shadow_storage_->depth_texture);
        if (shadow_storage_->program != 0) gl.delete_program(shadow_storage_->program);
    }
    if (player_storage_) {
        if (player_storage_->vertex_array != 0) gl.delete_vertex_arrays(1, &player_storage_->vertex_array);
        if (player_storage_->program != 0) gl.delete_program(player_storage_->program);
    }
    if (occlusion_storage_) {
        std::vector<GlUInt> queries;
        queries.reserve(occlusion_storage_->groups.size());
        for (const auto& [coordinate, group] : occlusion_storage_->groups) {
            (void)coordinate;
            if (group.query != 0) queries.push_back(group.query);
        }
        if (!queries.empty()) gl.delete_queries(static_cast<GlInt>(queries.size()), queries.data());
        if (occlusion_storage_->vertex_array != 0)
            gl.delete_vertex_arrays(1, &occlusion_storage_->vertex_array);
        if (occlusion_storage_->program != 0) gl.delete_program(occlusion_storage_->program);
    }
    if (debug_texture_ != 0) gl.delete_textures(1, &debug_texture_);
    if (menu_texture_ != 0) gl.delete_textures(1, &menu_texture_);
    if (ui_texture_ != 0) gl.delete_textures(1, &ui_texture_);
    if (cutout_index_buffer_ != 0) gl.delete_buffers(1, &cutout_index_buffer_);
    if (opaque_index_buffer_ != 0) gl.delete_buffers(1, &opaque_index_buffer_);
    if (vertex_buffer_ != 0) gl.delete_buffers(1, &vertex_buffer_);
    if (cutout_vertex_array_ != 0) gl.delete_vertex_arrays(1, &cutout_vertex_array_);
    if (opaque_vertex_array_ != 0) gl.delete_vertex_arrays(1, &opaque_vertex_array_);
    if (program_ != 0) gl.delete_program(program_);
    if (ui_program_ != 0) gl.delete_program(ui_program_);
}

bool OpenGlRenderer::apply_chunk_updates(
    const std::vector<ChunkMeshUpdate>& updates,
    const std::vector<Int3>& removals) {
    auto& storage = *streaming_storage_;
    const auto release_entry = [&](const Int3 coordinate) {
        const auto found = storage.entries.find(coordinate);
        if (found == storage.entries.end()) return;
        storage.vertex_arena.release(found->second.vertices);
        storage.opaque_arena.release(found->second.opaque_indices);
        storage.cutout_arena.release(found->second.cutout_indices);
        storage.entries.erase(found);
    };
    const auto rebuild_draw_ranges = [&] {
        draw_ranges_.clear();
        draw_ranges_.reserve(storage.entries.size());
        for (const auto& [coordinate, entry] : storage.entries) {
            (void)coordinate;
            draw_ranges_.push_back(entry.range);
        }
        std::sort(draw_ranges_.begin(), draw_ranges_.end(), [](const ChunkDrawRange& lhs, const ChunkDrawRange& rhs) {
            if (lhs.coordinates.z != rhs.coordinates.z) return lhs.coordinates.z < rhs.coordinates.z;
            return lhs.coordinates.x < rhs.coordinates.x;
        });
        visible_ranges_.reserve(draw_ranges_.size());
        opaque_draw_counts_.reserve(draw_ranges_.size());
        opaque_draw_offsets_.reserve(draw_ranges_.size());
        cutout_draw_counts_.reserve(draw_ranges_.size());
        cutout_draw_offsets_.reserve(draw_ranges_.size());
        shadow_opaque_draw_counts_.reserve(draw_ranges_.size());
        shadow_opaque_draw_offsets_.reserve(draw_ranges_.size());
        shadow_cutout_draw_counts_.reserve(draw_ranges_.size());
        shadow_cutout_draw_offsets_.reserve(draw_ranges_.size());
        occlusion_storage_->groups_dirty = true;
    };

    for (const auto coordinate : removals) release_entry(coordinate);

    const auto blocks = BlockRegistry::defaults();
    for (const auto& update : updates) {
        release_entry(update.coordinates);

        const auto offset_x = update.coordinates.x * Chunk::edge -
            static_cast<std::int32_t>(std::lround(world_origin_.x));
        const auto offset_z = update.coordinates.z * Chunk::edge -
            static_cast<std::int32_t>(std::lround(world_origin_.z));
        const auto minimum_coordinate = static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::min());
        const auto maximum_coordinate = static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::max());
        if (offset_x < minimum_coordinate || offset_z < minimum_coordinate ||
            offset_x + Chunk::edge > maximum_coordinate || offset_z + Chunk::edge > maximum_coordinate) {
            rebuild_draw_ranges();
            return false;
        }

        storage.vertex_scratch = update.mesh.vertices;
        for (auto& vertex : storage.vertex_scratch) {
            const auto x = static_cast<std::int32_t>(vertex.x) + offset_x;
            const auto z = static_cast<std::int32_t>(vertex.z) + offset_z;
            if (x < minimum_coordinate || x > maximum_coordinate ||
                z < minimum_coordinate || z > maximum_coordinate) {
                rebuild_draw_ranges();
                return false;
            }
            vertex.x = static_cast<std::int16_t>(x);
            vertex.z = static_cast<std::int16_t>(z);
        }

        storage.opaque_scratch.clear();
        storage.cutout_scratch.clear();
        storage.opaque_scratch.reserve(update.mesh.indices.size());
        storage.cutout_scratch.reserve(update.mesh.indices.size() / 8U);
        for (std::size_t index_offset = 0; index_offset + 5U < update.mesh.indices.size(); index_offset += 6U) {
            const auto first_vertex = update.mesh.indices[index_offset];
            if (first_vertex >= update.mesh.vertices.size()) {
                rebuild_draw_ranges();
                return false;
            }
            auto& destination = blocks.is_occluding(update.mesh.vertices[first_vertex].block)
                ? storage.opaque_scratch
                : storage.cutout_scratch;
            destination.insert(destination.end(),
                update.mesh.indices.begin() + static_cast<std::ptrdiff_t>(index_offset),
                update.mesh.indices.begin() + static_cast<std::ptrdiff_t>(index_offset + 6U));
        }

        const auto vertex_bytes = storage.vertex_scratch.size() * sizeof(MeshVertex);
        const auto opaque_bytes = storage.opaque_scratch.size() * sizeof(std::uint32_t);
        const auto cutout_bytes = storage.cutout_scratch.size() * sizeof(std::uint32_t);
        const auto vertex_slice = storage.vertex_arena.allocate(vertex_bytes, alignof(MeshVertex));
        const auto opaque_slice = storage.opaque_arena.allocate(opaque_bytes, alignof(std::uint32_t));
        const auto cutout_slice = storage.cutout_arena.allocate(cutout_bytes, alignof(std::uint32_t));
        if (!vertex_slice || !opaque_slice || !cutout_slice) {
            if (vertex_slice) storage.vertex_arena.release(*vertex_slice);
            if (opaque_slice) storage.opaque_arena.release(*opaque_slice);
            if (cutout_slice) storage.cutout_arena.release(*cutout_slice);
            rebuild_draw_ranges();
            return false;
        }

        const auto vertex_base = static_cast<std::uint32_t>(vertex_slice->offset / sizeof(MeshVertex));
        for (auto& index : storage.opaque_scratch) index += vertex_base;
        for (auto& index : storage.cutout_scratch) index += vertex_base;
        if (vertex_bytes != 0) {
            gl.bind_buffer(gl_array_buffer, vertex_buffer_);
            gl.buffer_sub_data(gl_array_buffer, static_cast<GlSize>(vertex_slice->offset),
                static_cast<GlSize>(vertex_bytes), storage.vertex_scratch.data());
        }
        if (opaque_bytes != 0) {
            gl.bind_buffer(gl_copy_write_buffer, opaque_index_buffer_);
            gl.buffer_sub_data(gl_copy_write_buffer, static_cast<GlSize>(opaque_slice->offset),
                static_cast<GlSize>(opaque_bytes), storage.opaque_scratch.data());
        }
        if (cutout_bytes != 0) {
            gl.bind_buffer(gl_copy_write_buffer, cutout_index_buffer_);
            gl.buffer_sub_data(gl_copy_write_buffer, static_cast<GlSize>(cutout_slice->offset),
                static_cast<GlSize>(cutout_bytes), storage.cutout_scratch.data());
        }

        ChunkDrawRange range{
            .coordinates = update.coordinates,
            .first_vertex = vertex_base,
            .vertex_count = static_cast<std::uint32_t>(storage.vertex_scratch.size()),
            .opaque_first_index = static_cast<std::uint32_t>(opaque_slice->offset / sizeof(std::uint32_t)),
            .opaque_index_count = static_cast<std::uint32_t>(storage.opaque_scratch.size()),
            .cutout_first_index = static_cast<std::uint32_t>(cutout_slice->offset / sizeof(std::uint32_t)),
            .cutout_index_count = static_cast<std::uint32_t>(storage.cutout_scratch.size()),
            .minimum_x = static_cast<std::int16_t>(offset_x),
            .minimum_z = static_cast<std::int16_t>(offset_z),
            .maximum_x = static_cast<std::int16_t>(offset_x + Chunk::edge),
            .maximum_z = static_cast<std::int16_t>(offset_z + Chunk::edge),
        };
        storage.entries.emplace(update.coordinates, StreamingStorage::Entry{
            *vertex_slice, *opaque_slice, *cutout_slice, range});
    }

    rebuild_draw_ranges();
    return true;
}

bool OpenGlRenderer::has_chunk(Int3 coordinates) const noexcept {
    return streaming_storage_ && streaming_storage_->entries.contains(coordinates);
}

std::size_t OpenGlRenderer::gpu_storage_bytes() const noexcept {
    if (!streaming_storage_) return 0;
    return streaming_storage_->vertex_arena.capacity() +
        streaming_storage_->opaque_arena.capacity() +
        streaming_storage_->cutout_arena.capacity();
}

double OpenGlRenderer::gpu_frame_milliseconds() const noexcept {
    return gpu_timing_storage_ ? gpu_timing_storage_->milliseconds : 0.0;
}

void OpenGlRenderer::render(
    const Matrix4& view_projection,
    Float3 camera_position,
    const VideoSettings& settings,
    std::int32_t width,
    std::int32_t height,
    bool show_player,
    bool show_crosshair,
    Float3 player_position,
    float player_yaw) {
    auto& timing = *gpu_timing_storage_;
    for (std::size_t index = 0; index < GpuTimingStorage::query_count; ++index) {
        if (!timing.pending[index]) continue;
        GlUInt available = 0;
        gl.get_query_object_uiv(timing.queries[index], gl_query_result_available, &available);
        if (available == 0) continue;
        GlUInt64 nanoseconds = 0;
        gl.get_query_object_ui64v(timing.queries[index], gl_query_result, &nanoseconds);
        const auto sample_milliseconds = static_cast<double>(nanoseconds) / 1'000'000.0;
        timing.milliseconds = timing.milliseconds == 0.0
            ? sample_milliseconds
            : timing.milliseconds * 0.80 + sample_milliseconds * 0.20;
        timing.pending[index] = false;
    }
    timing.measuring = false;
    for (std::size_t offset = 0; offset < GpuTimingStorage::query_count; ++offset) {
        const auto index = (timing.cursor + offset) % GpuTimingStorage::query_count;
        if (timing.pending[index]) continue;
        timing.active_query = index;
        timing.cursor = (index + 1U) % GpuTimingStorage::query_count;
        gl.begin_query(gl_time_elapsed, timing.queries[index]);
        timing.measuring = true;
        break;
    }

    auto& occlusion = *occlusion_storage_;
    const auto group_coordinate = [](Int3 coordinate) noexcept {
        return Int3{
            floor_div(coordinate.x, OcclusionStorage::chunks_per_group), 0,
            floor_div(coordinate.z, OcclusionStorage::chunks_per_group)};
    };
    if (occlusion.groups_dirty) {
        std::unordered_map<Int3, std::uint32_t> required_groups;
        required_groups.reserve(draw_ranges_.size() / 8U + 8U);
        for (const auto& range : draw_ranges_) {
            const auto group = group_coordinate(range.coordinates);
            const auto local_x = floor_mod(range.coordinates.x, OcclusionStorage::chunks_per_group);
            const auto local_z = floor_mod(range.coordinates.z, OcclusionStorage::chunks_per_group);
            required_groups[group] |= 1U << static_cast<std::uint32_t>(
                local_x + local_z * OcclusionStorage::chunks_per_group);
        }
        for (auto iterator = occlusion.groups.begin(); iterator != occlusion.groups.end();) {
            if (required_groups.contains(iterator->first)) {
                ++iterator;
            } else {
                if (iterator->second.query != 0) gl.delete_queries(1, &iterator->second.query);
                iterator = occlusion.groups.erase(iterator);
            }
        }
        for (const auto& [coordinate, active_mask] : required_groups) {
            auto [iterator, inserted] = occlusion.groups.try_emplace(coordinate);
            if (!inserted && iterator->second.active_mask != active_mask) {
                iterator->second.visible = true;
                iterator->second.occluded_confirmations = 0;
            }
            iterator->second.active_mask = active_mask;
            iterator->second.range_indices.fill(OcclusionStorage::invalid_range);
            if (inserted) gl.gen_queries(1, &iterator->second.query);
        }
        for (std::uint32_t index = 0; index < draw_ranges_.size(); ++index) {
            const auto& range = draw_ranges_[index];
            const auto coordinate = group_coordinate(range.coordinates);
            const auto local_x = floor_mod(range.coordinates.x, OcclusionStorage::chunks_per_group);
            const auto local_z = floor_mod(range.coordinates.z, OcclusionStorage::chunks_per_group);
            const auto local_index = static_cast<std::size_t>(
                local_x + local_z * OcclusionStorage::chunks_per_group);
            occlusion.groups.at(coordinate).range_indices[local_index] = index;
        }
        occlusion.distance_order.clear();
        occlusion.distance_order.reserve(occlusion.groups.size());
        for (const auto& [coordinate, group] : occlusion.groups) {
            (void)group;
            occlusion.distance_order.push_back(coordinate);
        }
        occlusion.query_candidates.reserve(occlusion.groups.size());
        occlusion.query_cursor = 0;
        occlusion.order_dirty = true;
        occlusion.groups_dirty = false;
    }

    const auto camera_delta = camera_position - occlusion.reference_camera;
    const auto camera_moved = !occlusion.has_reference_camera ||
        dot(camera_delta, camera_delta) > 0.5625F;
    if (camera_moved) {
        occlusion.reference_camera = camera_position;
        occlusion.has_reference_camera = true;
        occlusion.stable_frames = 0;
        ++occlusion.view_revision;
        for (auto& [coordinate, group] : occlusion.groups) {
            (void)coordinate;
            group.visible = true;
            group.occluded_confirmations = 0;
        }
    } else {
        // Camera rotation changes which groups enter the frustum, but it does
        // not change line-of-sight occlusion through a static world from the
        // same camera position. Preserve those results while turning instead
        // of making every group visible and disabling queries for 20 frames.
        occlusion.stable_frames = std::min(occlusion.stable_frames + 1U, 1'000U);
    }

    const auto order_delta = camera_position - occlusion.order_reference_camera;
    if (occlusion.order_dirty || dot(order_delta, order_delta) > 64.0F) {
        constexpr auto group_size_blocks = OcclusionStorage::chunks_per_group * Chunk::edge;
        std::sort(occlusion.distance_order.begin(), occlusion.distance_order.end(),
            [&](Int3 lhs, Int3 rhs) noexcept {
                const auto distance_squared = [&](Int3 coordinate) noexcept {
                    const auto center_x = static_cast<float>(coordinate.x * group_size_blocks) +
                        static_cast<float>(group_size_blocks) * 0.5F;
                    const auto center_z = static_cast<float>(coordinate.z * group_size_blocks) +
                        static_cast<float>(group_size_blocks) * 0.5F;
                    const auto delta_x = center_x - camera_position.x;
                    const auto delta_z = center_z - camera_position.z;
                    return delta_x * delta_x + delta_z * delta_z;
                };
                return distance_squared(lhs) < distance_squared(rhs);
            });
        occlusion.order_reference_camera = camera_position;
        occlusion.order_dirty = false;
    }

    occlusion.query_candidates.clear();
    for (auto& [coordinate, group] : occlusion.groups) {
        (void)coordinate;
        group.candidate = false;
        if (!group.pending) continue;
        GlUInt available = 0;
        gl.get_query_object_uiv(group.query, gl_query_result_available, &available);
        if (available == 0) continue;
        GlUInt samples_passed = 1;
        gl.get_query_object_uiv(group.query, gl_query_result, &samples_passed);
        group.pending = false;
        if (group.issued_view_revision == occlusion.view_revision) {
            if (samples_passed != 0) {
                group.visible = true;
                group.occluded_confirmations = 0;
            } else {
                group.occluded_confirmations = static_cast<std::uint8_t>(
                    std::min<std::uint32_t>(group.occluded_confirmations + 1U, 2U));
                if (group.occluded_confirmations >= 2U) group.visible = false;
            }
        }
    }

    const auto frustum = extract_frustum(view_projection);
    visible_ranges_.clear();
    occluded_chunk_count_ = 0;
    constexpr auto near_occlusion_distance =
        static_cast<float>(OcclusionStorage::chunks_per_group * Chunk::edge);
    constexpr auto near_occlusion_distance_squared = near_occlusion_distance * near_occlusion_distance;
    constexpr auto group_size_blocks = OcclusionStorage::chunks_per_group * Chunk::edge;
    for (const auto coordinate : occlusion.distance_order) {
        auto& group = occlusion.groups.at(coordinate);
        const auto minimum_x = static_cast<float>(coordinate.x * group_size_blocks);
        const auto minimum_z = static_cast<float>(coordinate.z * group_size_blocks);
        const auto relation = box_frustum_relation(
            minimum_x, minimum_x + static_cast<float>(group_size_blocks),
            minimum_z, minimum_z + static_cast<float>(group_size_blocks), frustum);
        if (relation == FrustumRelation::outside) continue;

        group.candidate = true;
        occlusion.query_candidates.push_back(coordinate);
        for (const auto index : group.range_indices) {
            if (index == OcclusionStorage::invalid_range) continue;
            const auto& range = draw_ranges_[index];
            if (relation == FrustumRelation::intersecting &&
                !chunk_intersects_frustum(range, world_origin_, frustum)) continue;
            const auto center_x = world_origin_.x +
                (static_cast<float>(range.minimum_x) + static_cast<float>(range.maximum_x)) * 0.5F;
            const auto center_z = world_origin_.z +
                (static_cast<float>(range.minimum_z) + static_cast<float>(range.maximum_z)) * 0.5F;
            const auto delta_x = center_x - camera_position.x;
            const auto delta_z = center_z - camera_position.z;
            if (group.visible || delta_x * delta_x + delta_z * delta_z < near_occlusion_distance_squared) {
                visible_ranges_.push_back(index);
            } else {
                ++occluded_chunk_count_;
            }
        }
    }
    visible_chunk_count_ = static_cast<std::uint32_t>(visible_ranges_.size());
    opaque_draw_counts_.clear();
    opaque_draw_offsets_.clear();
    cutout_draw_counts_.clear();
    cutout_draw_offsets_.clear();
    for (const auto index : visible_ranges_) {
        const auto& range = draw_ranges_[index];
        if (range.opaque_index_count != 0U) {
            opaque_draw_counts_.push_back(static_cast<std::int32_t>(range.opaque_index_count));
            opaque_draw_offsets_.push_back(reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(range.opaque_first_index) * sizeof(std::uint32_t)));
        }
        if (range.cutout_index_count != 0U) {
            cutout_draw_counts_.push_back(static_cast<std::int32_t>(range.cutout_index_count));
            cutout_draw_offsets_.push_back(reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(range.cutout_first_index) * sizeof(std::uint32_t)));
        }
    }

    const auto light_direction = normalize(Float3{0.45F, 0.85F, 0.30F});
    const Float3 shadow_target{
        camera_position.x,
        16.0F,
        camera_position.z,
    };
    const auto light_position = shadow_target + light_direction * 420.0F;
    const auto light_view = Matrix4::look_at(light_position, shadow_target, {0.0F, 1.0F, 0.0F});
    const auto shadow_radius = std::clamp(
        static_cast<float>(settings.shadow_distance_blocks), 64.0F, 256.0F);
    auto light_projection = Matrix4::orthographic(
        -shadow_radius, shadow_radius,
        -shadow_radius, shadow_radius, 1.0F, 760.0F);

    // Anchor the orthographic projection to the shadow-map texel grid in
    // light space. Moving the player now scrolls the sun camera in exact
    // one-texel increments instead of the old two-world-block (~9 texel)
    // jumps, keeping shadows from stationary trees fixed on the terrain.
    const auto unstabilized_light_view_projection = light_projection * light_view;
    const auto half_shadow_resolution = static_cast<float>(ShadowStorage::texture_size) * 0.5F;
    const auto origin_texel_x = unstabilized_light_view_projection.values[12] * half_shadow_resolution;
    const auto origin_texel_y = unstabilized_light_view_projection.values[13] * half_shadow_resolution;
    const auto texel_offset_x = std::round(origin_texel_x) - origin_texel_x;
    const auto texel_offset_y = std::round(origin_texel_y) - origin_texel_y;
    light_projection.values[12] += texel_offset_x / half_shadow_resolution;
    light_projection.values[13] += texel_offset_y / half_shadow_resolution;
    const auto light_view_projection = light_projection * light_view;
    const auto player_shadow_target = player_position + Float3{0.0F, 1.0F, 0.0F};
    const auto player_shadow_light_position =
        player_shadow_target + light_direction * 48.0F;
    const auto player_shadow_view = Matrix4::look_at(
        player_shadow_light_position, player_shadow_target, {0.0F, 1.0F, 0.0F});
    const auto player_shadow_projection = Matrix4::orthographic(
        -12.0F, 12.0F, -12.0F, 12.0F, 0.1F, 96.0F);
    const auto player_light_view_projection = player_shadow_projection * player_shadow_view;
    shadow_opaque_draw_counts_.clear();
    shadow_opaque_draw_offsets_.clear();
    shadow_cutout_draw_counts_.clear();
    shadow_cutout_draw_offsets_.clear();
    const auto shadow_selection_radius = shadow_radius + static_cast<float>(Chunk::edge);
    const auto shadow_selection_radius_squared = shadow_selection_radius * shadow_selection_radius;
    for (const auto& range : draw_ranges_) {
        const auto center_x = world_origin_.x +
            (static_cast<float>(range.minimum_x) + static_cast<float>(range.maximum_x)) * 0.5F;
        const auto center_z = world_origin_.z +
            (static_cast<float>(range.minimum_z) + static_cast<float>(range.maximum_z)) * 0.5F;
        const auto delta_x = center_x - camera_position.x;
        const auto delta_z = center_z - camera_position.z;
        if (delta_x * delta_x + delta_z * delta_z > shadow_selection_radius_squared) continue;
        if (range.opaque_index_count != 0U) {
            shadow_opaque_draw_counts_.push_back(static_cast<std::int32_t>(range.opaque_index_count));
            shadow_opaque_draw_offsets_.push_back(reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(range.opaque_first_index) * sizeof(std::uint32_t)));
        }
        if (range.cutout_index_count != 0U) {
            shadow_cutout_draw_counts_.push_back(static_cast<std::int32_t>(range.cutout_index_count));
            shadow_cutout_draw_offsets_.push_back(reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(range.cutout_first_index) * sizeof(std::uint32_t)));
        }
    }

    const auto& shadow = *shadow_storage_;
    gl.bind_framebuffer(gl_framebuffer, shadow.framebuffer);
    gl.viewport(0, 0, ShadowStorage::texture_size, ShadowStorage::texture_size);
    gl.clear(gl_depth_buffer_bit);
    gl.use_program(shadow.program);
    gl.uniform_matrix_4fv(
        shadow.light_view_projection_location, 1, gl_false, light_view_projection.values.data());
    gl.uniform_3f(shadow.world_origin_location, world_origin_.x, world_origin_.y, world_origin_.z);
    gl.bind_vertex_array(opaque_vertex_array_);
    if (!shadow_opaque_draw_counts_.empty()) {
        gl.multi_draw_elements(gl_triangles, shadow_opaque_draw_counts_.data(), gl_unsigned_int,
            shadow_opaque_draw_offsets_.data(), static_cast<GlInt>(shadow_opaque_draw_counts_.size()));
    }
    gl.bind_vertex_array(cutout_vertex_array_);
    if (!shadow_cutout_draw_counts_.empty()) {
        gl.multi_draw_elements(gl_triangles, shadow_cutout_draw_counts_.data(), gl_unsigned_int,
            shadow_cutout_draw_offsets_.data(), static_cast<GlInt>(shadow_cutout_draw_counts_.size()));
    }
    // The large world map has only a few texels across the player. Render the
    // dynamic caster into a small player-centered map instead, keeping its
    // silhouette stable while the player moves through world-space texels.
    const auto& shadow_player = *player_storage_;
    gl.bind_framebuffer(gl_framebuffer, shadow.player_framebuffer);
    gl.viewport(0, 0, ShadowStorage::player_texture_size, ShadowStorage::player_texture_size);
    gl.clear(gl_depth_buffer_bit);
    gl.disable(gl_cull_face);
    gl.use_program(shadow_player.program);
    gl.uniform_matrix_4fv(shadow_player.view_projection_location, 1, gl_false,
        player_light_view_projection.values.data());
    gl.uniform_3f(
        shadow_player.position_location, player_position.x, player_position.y, player_position.z);
    gl.uniform_1f(shadow_player.yaw_location, player_yaw);
    gl.bind_vertex_array(shadow_player.vertex_array);
    gl.draw_arrays_instanced(gl_triangles, 0, 36, 6);
    gl.enable(gl_cull_face);
    gl.bind_framebuffer(gl_framebuffer, 0);

    gl.viewport(0, 0, width, height);
    gl.clear(gl_color_buffer_bit | gl_depth_buffer_bit);
    gl.use_program(program_);
    gl.uniform_matrix_4fv(view_projection_location_, 1, gl_false, view_projection.values.data());
    gl.uniform_matrix_4fv(
        light_view_projection_location_, 1, gl_false, light_view_projection.values.data());
    gl.uniform_matrix_4fv(player_light_view_projection_location_, 1, gl_false,
        player_light_view_projection.values.data());
    gl.uniform_3f(world_origin_location_, world_origin_.x, world_origin_.y, world_origin_.z);
    gl.uniform_3f(camera_position_location_, camera_position.x, camera_position.y, camera_position.z);
    gl.uniform_3f(shadow_caster_position_location_,
        player_position.x, player_position.y, player_position.z);
    gl.uniform_1f(distance_smoothing_start_location_, settings.distance_smoothing_start);
    gl.uniform_1f(fog_start_fraction_location_, settings.fog_start_fraction);
    gl.uniform_1f(render_distance_blocks_location_,
        static_cast<float>(settings.render_distance_chunks) * 16.0F);
    gl.uniform_1f(shadow_distance_location_, static_cast<float>(settings.shadow_distance_blocks));
    gl.active_texture(gl_texture1);
    gl.bind_texture(gl_texture_2d, shadow.depth_texture);
    gl.uniform_1i(shadow_map_location_, 1);
    gl.active_texture(gl_texture2);
    gl.bind_texture(gl_texture_2d, shadow.player_depth_texture);
    gl.uniform_1i(player_shadow_map_location_, 2);
    gl.disable(gl_sample_alpha_to_coverage);
    gl.bind_vertex_array(opaque_vertex_array_);

    if (!opaque_draw_counts_.empty()) {
        // Integer-position, neighbor-aware meshes share exact chunk boundaries,
        // so opaque terrain needs only one raster pass.
        gl.multi_draw_elements(gl_triangles, opaque_draw_counts_.data(), gl_unsigned_int,
            opaque_draw_offsets_.data(), static_cast<GlInt>(opaque_draw_counts_.size()));
    }

    if (!cutout_draw_counts_.empty()) {
        gl.enable(gl_sample_alpha_to_coverage);
        gl.bind_vertex_array(cutout_vertex_array_);
        gl.multi_draw_elements(gl_triangles, cutout_draw_counts_.data(), gl_unsigned_int,
            cutout_draw_offsets_.data(), static_cast<GlInt>(cutout_draw_counts_.size()));
    }

    if (show_player) {
        const auto& player = *player_storage_;
        gl.disable(gl_sample_alpha_to_coverage);
        gl.disable(gl_cull_face);
        gl.use_program(player.program);
        gl.uniform_matrix_4fv(player.view_projection_location, 1, gl_false, view_projection.values.data());
        gl.uniform_3f(player.position_location, player_position.x, player_position.y, player_position.z);
        gl.uniform_1f(player.yaw_location, player_yaw);
        gl.bind_vertex_array(player.vertex_array);
        gl.draw_arrays_instanced(gl_triangles, 0, 36, 6);
        gl.enable(gl_cull_face);
    }

    if (occlusion.stable_frames >= 20U && !occlusion.query_candidates.empty()) {
        // Ask the GPU whether far 4x4 chunk regions survive the depth buffer.
        // A fixed budget keeps query work even from frame to frame, and results
        // are consumed only after availability is reported, so the CPU never waits.
        gl.disable(gl_sample_alpha_to_coverage);
        gl.disable(gl_cull_face);
        gl.color_mask(gl_false, gl_false, gl_false, gl_false);
        gl.depth_mask(gl_false);
        gl.use_program(occlusion.program);
        gl.uniform_matrix_4fv(
            occlusion.view_projection_location, 1, gl_false, view_projection.values.data());
        gl.bind_vertex_array(occlusion.vertex_array);
        constexpr auto group_size_blocks = OcclusionStorage::chunks_per_group * Chunk::edge;
        constexpr std::size_t query_budget_per_frame = 4;
        std::size_t queried_groups = 0;
        std::size_t scanned_groups = 0;
        while (queried_groups < query_budget_per_frame &&
            scanned_groups < occlusion.query_candidates.size()) {
            const auto candidate_index = occlusion.query_cursor % occlusion.query_candidates.size();
            const auto coordinate = occlusion.query_candidates[candidate_index];
            ++occlusion.query_cursor;
            ++scanned_groups;
            auto& group = occlusion.groups.at(coordinate);
            if (group.pending) continue;
            const auto minimum_x = static_cast<float>(coordinate.x * group_size_blocks);
            const auto minimum_z = static_cast<float>(coordinate.z * group_size_blocks);
            const auto center_x = minimum_x + static_cast<float>(group_size_blocks) * 0.5F;
            const auto center_z = minimum_z + static_cast<float>(group_size_blocks) * 0.5F;
            const auto delta_x = center_x - camera_position.x;
            const auto delta_z = center_z - camera_position.z;
            if (delta_x * delta_x + delta_z * delta_z < near_occlusion_distance_squared) continue;
            gl.uniform_3f(occlusion.bounds_minimum_location, minimum_x, 0.0F, minimum_z);
            gl.uniform_1i(occlusion.active_mask_location, static_cast<GlInt>(group.active_mask));
            gl.begin_query(gl_any_samples_passed, group.query);
            gl.draw_arrays_instanced(gl_triangles, 0, 36,
                OcclusionStorage::chunks_per_group * OcclusionStorage::chunks_per_group);
            gl.end_query(gl_any_samples_passed);
            group.pending = true;
            group.issued_view_revision = occlusion.view_revision;
            ++queried_groups;
        }
        occlusion.query_cursor %= occlusion.query_candidates.size();
        gl.depth_mask(gl_true);
        gl.color_mask(gl_true, gl_true, gl_true, gl_true);
        gl.enable(gl_cull_face);
    }

    if (show_crosshair) {
        const auto& crosshair = *crosshair_storage_;
        gl.disable(gl_depth_test);
        gl.disable(gl_cull_face);
        gl.disable(gl_sample_alpha_to_coverage);
        gl.use_program(crosshair.program);
        gl.uniform_2f(
            crosshair.framebuffer_size_location, static_cast<float>(width), static_cast<float>(height));
        gl.bind_vertex_array(crosshair.vertex_array);
        gl.draw_arrays(gl_triangles, 0, 24);
        gl.enable(gl_depth_test);
        gl.enable(gl_cull_face);
    }
    if (timing.measuring) {
        gl.end_query(gl_time_elapsed);
        timing.pending[timing.active_query] = true;
        timing.measuring = false;
    }
}

void OpenGlRenderer::render_menu(
    const MenuUiState& state,
    const VideoSettings& settings,
    std::int32_t width,
    std::int32_t height) {
    if (width != menu_cached_width_ || height != menu_cached_height_ ||
        state.screen != menu_cached_screen_ ||
        state.hovered_control != menu_cached_hovered_ ||
        state.selected_world != menu_cached_selected_world_ ||
        state.editing_world_name != menu_cached_editing_ ||
        state.world_name != menu_cached_world_name_ ||
        state.creation_date != menu_cached_creation_date_ ||
        state.multiplayer_status != menu_cached_multiplayer_status_ ||
        state.saved_worlds != menu_cached_saved_worlds_ ||
        settings.render_distance_chunks != menu_cached_render_distance_) {
        render::ui::build_menu_pixels(menu_pixels_, width, height, state, settings);
        gl.active_texture(gl_texture0);
        gl.bind_texture(gl_texture_2d, menu_texture_);
        gl.tex_image_2d(gl_texture_2d, 0, static_cast<GlInt>(gl_rgba), width, height, 0,
            gl_rgba, gl_unsigned_byte, menu_pixels_.data());
        menu_cached_width_ = width;
        menu_cached_height_ = height;
        menu_cached_screen_ = state.screen;
        menu_cached_hovered_ = state.hovered_control;
        menu_cached_selected_world_ = state.selected_world;
        menu_cached_editing_ = state.editing_world_name;
        menu_cached_world_name_ = state.world_name;
        menu_cached_creation_date_ = state.creation_date;
        menu_cached_multiplayer_status_ = state.multiplayer_status;
        menu_cached_saved_worlds_ = state.saved_worlds;
        menu_cached_render_distance_ = settings.render_distance_chunks;
    }

    gl.disable(gl_depth_test);
    gl.disable(gl_cull_face);
    gl.disable(gl_sample_alpha_to_coverage);
    gl.enable(gl_blend);
    gl.blend_func(gl_src_alpha, gl_one_minus_src_alpha);
    gl.use_program(ui_program_);
    gl.active_texture(gl_texture0);
    gl.bind_texture(gl_texture_2d, menu_texture_);
    gl.uniform_1i(ui_sampler_location_, 0);
    gl.bind_vertex_array(opaque_vertex_array_);
    gl.draw_arrays(gl_triangles, 0, 6);
    gl.disable(gl_blend);
    gl.enable(gl_depth_test);
    gl.enable(gl_cull_face);
}

void OpenGlRenderer::render_video_settings(
    const VideoSettings& settings,
    const VideoSettingsUiState& ui_state,
    std::int32_t width,
    std::int32_t height) {
    if (width != ui_cached_width_ || height != ui_cached_height_ ||
        settings.render_distance_chunks != ui_cached_render_distance_ ||
        settings.distance_smoothing_start != ui_cached_smoothing_ ||
        settings.fog_start_fraction != ui_cached_fog_start_ ||
        settings.shadow_distance_blocks != ui_cached_shadow_distance_ ||
        settings.render_distance_scale_max != ui_cached_scale_max_ ||
        settings.vsync != ui_cached_vsync_ ||
        settings.fullscreen != ui_cached_fullscreen_ ||
        ui_state.editing_render_distance != ui_cached_editing_ ||
        ui_state.numeric_text != ui_cached_numeric_text_) {
        render::ui::build_settings_pixels(ui_pixels_, width, height, settings, ui_state);
        gl.active_texture(gl_texture0);
        gl.bind_texture(gl_texture_2d, ui_texture_);
        gl.tex_image_2d(gl_texture_2d, 0, static_cast<GlInt>(gl_rgba), width, height, 0,
            gl_rgba, gl_unsigned_byte, ui_pixels_.data());
        ui_cached_width_ = width;
        ui_cached_height_ = height;
        ui_cached_render_distance_ = settings.render_distance_chunks;
        ui_cached_smoothing_ = settings.distance_smoothing_start;
        ui_cached_fog_start_ = settings.fog_start_fraction;
        ui_cached_shadow_distance_ = settings.shadow_distance_blocks;
        ui_cached_scale_max_ = settings.render_distance_scale_max;
        ui_cached_vsync_ = settings.vsync;
        ui_cached_fullscreen_ = settings.fullscreen;
        ui_cached_editing_ = ui_state.editing_render_distance;
        ui_cached_numeric_text_ = ui_state.numeric_text;
    }

    gl.disable(gl_depth_test);
    gl.disable(gl_cull_face);
    gl.disable(gl_sample_alpha_to_coverage);
    gl.enable(gl_blend);
    gl.blend_func(gl_src_alpha, gl_one_minus_src_alpha);
    gl.use_program(ui_program_);
    gl.active_texture(gl_texture0);
    gl.bind_texture(gl_texture_2d, ui_texture_);
    gl.uniform_1i(ui_sampler_location_, 0);
    gl.bind_vertex_array(opaque_vertex_array_);
    gl.draw_arrays(gl_triangles, 0, 6);
    gl.disable(gl_blend);
    gl.enable(gl_depth_test);
    gl.enable(gl_cull_face);
    gl.enable(gl_sample_alpha_to_coverage);
}

void OpenGlRenderer::render_debug_overlay(
    const DebugStats& stats,
    std::int32_t width,
    std::int32_t height) {
    const auto text = render::ui::debug_text(stats);
    if (width != debug_cached_width_ || height != debug_cached_height_ ||
        text != debug_cached_text_) {
        render::ui::build_debug_pixels(debug_pixels_, width, height, text);
        gl.active_texture(gl_texture0);
        gl.bind_texture(gl_texture_2d, debug_texture_);
        gl.tex_image_2d(gl_texture_2d, 0, static_cast<GlInt>(gl_rgba), width, height, 0,
            gl_rgba, gl_unsigned_byte, debug_pixels_.data());
        debug_cached_width_ = width;
        debug_cached_height_ = height;
        debug_cached_text_ = text;
    }

    gl.disable(gl_depth_test);
    gl.disable(gl_cull_face);
    gl.disable(gl_sample_alpha_to_coverage);
    gl.enable(gl_blend);
    gl.blend_func(gl_src_alpha, gl_one_minus_src_alpha);
    gl.use_program(ui_program_);
    gl.active_texture(gl_texture0);
    gl.bind_texture(gl_texture_2d, debug_texture_);
    gl.uniform_1i(ui_sampler_location_, 0);
    gl.bind_vertex_array(opaque_vertex_array_);
    gl.draw_arrays(gl_triangles, 0, 6);
    gl.disable(gl_blend);
    gl.enable(gl_depth_test);
    gl.enable(gl_cull_face);
    gl.enable(gl_sample_alpha_to_coverage);
}

} // namespace heartstead
