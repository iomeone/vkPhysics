#include "dr_rsc.hpp"
#include <common/files.hpp>
#include <common/constant.hpp>
#include <common/serialiser.hpp>
#include <renderer/renderer.hpp>
#include <common/chunk.hpp>
#include <common/allocators.hpp>
#include <vulkan/vulkan_core.h>

#include "dr_chunk.hpp"

static mesh_t meshes[GM_INVALID];

static struct animations_t {
    skeleton_t player_sk;

    animation_cycles_t player_cyc;
} animations;

static shader_t shaders[GS_INVALID];

chunk_color_mem_t dr_chunk_colors_g;

static struct tmp_t {
    // These are used temporarily to generate a chunk's mesh
    compressed_chunk_mesh_vertex_t *mesh_vertices;
} tmp;

static void s_create_player_shaders_and_meshes() {
    shader_binding_info_t bullet_sbi = {};
    load_mesh_external(dr_get_mesh_rsc(GM_BULLET), &bullet_sbi, "assets/models/bullet.mesh");

    // Load the animation data for the player "person" mesh
    load_skeleton(&animations.player_sk, "assets/models/player.skeleton");
    load_animation_cycles(&animations.player_cyc, "assets/models/player.animations.link", "assets/models/player.animations");

    shader_binding_info_t player_sbi, ball_sbi, merged_sbi;

    // Create meshes also for transition effect between both models
    create_player_merged_mesh(
        &meshes[GM_PLAYER], &player_sbi,
        &meshes[GM_BALL], &ball_sbi,
        &meshes[GM_MERGED], &merged_sbi);

    // Create shaders for the ball mesh
    const char *static_shader_paths[] =
        {"shaders/SPV/mesh.vert.spv", "shaders/SPV/mesh.geom.spv", "shaders/SPV/mesh.frag.spv"};
    const char *static_shadow_shader_paths[] =
        {"shaders/SPV/mesh_shadow.vert.spv", "shaders/SPV/shadow.frag.spv"};

    shaders[GS_BALL] = create_mesh_shader_color(
        &ball_sbi,
        static_shader_paths,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_CULL_MODE_NONE,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        MT_STATIC);

    shaders[GS_BALL_SHADOW] = create_mesh_shader_shadow(
        &ball_sbi,
        static_shadow_shader_paths,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        MT_STATIC);

    // Create shaders for the "person" mesh
    const char *player_shader_paths[] =
        {"shaders/SPV/skeletal.vert.spv", "shaders/SPV/skeletal.geom.spv", "shaders/SPV/skeletal.frag.spv"};
    const char *player_shadow_shader_paths[] =
        {"shaders/SPV/skeletal_shadow.vert.spv", "shaders/SPV/shadow.frag.spv"};

    shaders[GS_PLAYER] = create_mesh_shader_color(
        &player_sbi,
        player_shader_paths,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_CULL_MODE_NONE, 
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        MT_ANIMATED);

    shaders[GS_PLAYER_SHADOW] = create_mesh_shader_shadow(
        &player_sbi,
        player_shadow_shader_paths,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        MT_ANIMATED);

    // Create shaders for the transition effect between person and ball
    const char *merged_shader_paths[] =
        {"shaders/SPV/morph.vert.spv", "shaders/SPV/morph_ball.geom.spv", "shaders/SPV/morph.frag.spv"};
    const char *merged_shadow_shader_paths[] =
        {"shaders/SPV/morph.vert.spv", "shaders/SPV/morph_ball_shadow.geom.spv", "shaders/SPV/shadow.frag.spv"};

    shaders[GS_MERGED_BALL] = create_mesh_shader_color(
        &merged_sbi,
        merged_shader_paths,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_CULL_MODE_NONE, 
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
        MT_ANIMATED | MT_MERGED_MESH);

#if 1
    shaders[GS_MERGED_BALL_SHADOW] = create_mesh_shader_shadow(
        &merged_sbi,
        merged_shadow_shader_paths,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
        MT_ANIMATED | MT_MERGED_MESH);
#endif

    merged_shader_paths[1] = "shaders/SPV/morph_dude.geom.spv";
    merged_shadow_shader_paths[1] = "shaders/SPV/morph_dude_shadow.geom.spv";

    shaders[GS_MERGED_PLAYER] = create_mesh_shader_color(
        &merged_sbi,
        merged_shader_paths,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_CULL_MODE_NONE, 
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
        MT_ANIMATED | MT_MERGED_MESH);

#if 1
    shaders[GS_MERGED_PLAYER_SHADOW] = create_mesh_shader_shadow(
        &merged_sbi,
        merged_shadow_shader_paths,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
        MT_ANIMATED | MT_MERGED_MESH);
#endif
}

static void s_create_chunk_shaders() {
    // mesh_t chunk_mesh_prototype = {};
    // push_buffer_to_mesh(
    //     BT_VERTEX,
    //     &chunk_mesh_prototype);

    // shader_binding_info_t binding_info = create_mesh_binding_info(
    //     &chunk_mesh_prototype);

    // Need to create special binding for chunk mesh rendering
    shader_binding_info_t binding_info = {};
    binding_info.binding_count = 1;
    binding_info.binding_descriptions = FL_MALLOC(VkVertexInputBindingDescription, binding_info.binding_count);

    binding_info.attribute_count = 2;
    binding_info.attribute_descriptions = FL_MALLOC(VkVertexInputAttributeDescription, binding_info.attribute_count);

    binding_info.binding_descriptions[0].binding = 0;
    binding_info.binding_descriptions[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    binding_info.binding_descriptions[0].stride = sizeof(uint32_t) + sizeof(uint32_t);
    
    binding_info.attribute_descriptions[0].binding = 0;
    binding_info.attribute_descriptions[0].location = 0;
    binding_info.attribute_descriptions[0].format = VK_FORMAT_R32_UINT;
    binding_info.attribute_descriptions[0].offset = 0;

    binding_info.attribute_descriptions[1].binding = 0;
    binding_info.attribute_descriptions[1].location = 1;
    binding_info.attribute_descriptions[1].format = VK_FORMAT_R32_UINT;
    binding_info.attribute_descriptions[1].offset = sizeof(uint32_t);

    const char *shader_paths[] = {
        "shaders/SPV/chunk_mesh.vert.spv",
        "shaders/SPV/chunk_mesh.geom.spv",
        "shaders/SPV/chunk_mesh.frag.spv"
    };

    const char *shadow_shader_paths[] = {
        "shaders/SPV/chunk_mesh_shadow.vert.spv",
        "shaders/SPV/shadow.frag.spv"
    };
    
    shaders[GS_CHUNK] = create_mesh_shader_color(
        &binding_info,
        shader_paths,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_CULL_MODE_FRONT_BIT,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        MT_STATIC | MT_PASS_EXTRA_UNIFORM_BUFFER);

    shaders[GS_CHUNK_SHADOW] = create_mesh_shader_shadow(
        &binding_info,
        shadow_shader_paths,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        MT_STATIC);

    dr_chunk_colors_g.chunk_color.pointer_position = vector4_t(0.0f);
    dr_chunk_colors_g.chunk_color.pointer_color = vector4_t(0.0f);
    dr_chunk_colors_g.chunk_color.pointer_radius = 0.0f;

    dr_chunk_colors_g.chunk_color_ubo = create_gpu_buffer(
        sizeof(chunk_color_data_t),
        &dr_chunk_colors_g.chunk_color,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    dr_chunk_colors_g.chunk_color_set = create_buffer_descriptor_set(
        dr_chunk_colors_g.chunk_color_ubo.buffer,
        sizeof(chunk_color_data_t),
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
}

void dr_resources_init() {
    s_create_player_shaders_and_meshes();
    s_create_chunk_shaders();

    tmp.mesh_vertices = FL_MALLOC(compressed_chunk_mesh_vertex_t, CHUNK_MAX_VERTICES_PER_CHUNK);
}

void dr_player_animated_instance_init(animated_instance_t *instance) {
    animated_instance_init(instance, &animations.player_sk, &animations.player_cyc);
}

compressed_chunk_mesh_vertex_t *dr_get_tmp_mesh_verts() {
    return tmp.mesh_vertices;
}


fixed_premade_scene_t dr_read_premade_rsc(const char *path) {
    fixed_premade_scene_t res = {};

    file_handle_t input = create_file(path, FLF_BINARY);

    file_contents_t contents = read_file(
        input);

    serialiser_t serialiser = {};
    serialiser.data_buffer = (uint8_t *)contents.data;
    serialiser.data_buffer_size = contents.size;

    vector3_t ws_position = serialiser.deserialise_vector3();
    vector3_t ws_view_direction = serialiser.deserialise_vector3();

    uint32_t vertex_count = serialiser.deserialise_uint32();

    compressed_chunk_mesh_vertex_t *vertices = LN_MALLOC(compressed_chunk_mesh_vertex_t, vertex_count);

    for (uint32_t i = 0; i < vertex_count; ++i) {
        vector3_t vertex = serialiser.deserialise_vector3();
        uint8_t color = 0;

        vector3_t floor_of_vertex = glm::min(glm::floor(vertex), vector3_t(15.0f));
        ivector3_t ifloor_of_vertex = ivector3_t(floor_of_vertex);

        vector3_t normalized_vertex = (vertex - floor_of_vertex);
        ivector3_t inormalized_vertex = ivector3_t((vertex - floor_of_vertex) * 255.0f);
    
        uint32_t low = 0;
        uint32_t high = 0;

        low += (ifloor_of_vertex.x << 28);
        low += (ifloor_of_vertex.y << 24);
        low += (ifloor_of_vertex.z << 20);
        low += (inormalized_vertex.x << 12);
        low += (inormalized_vertex.y << 4);
        low += inormalized_vertex.z >> 4;

        high += inormalized_vertex.z << 28;
        high += color << 20;

        vertices[i].low = low;
        vertices[i].high = high;
    }

    push_buffer_to_mesh(BT_VERTEX, &res.world_mesh);
    mesh_buffer_t *vtx_buffer = get_mesh_buffer(BT_VERTEX, &res.world_mesh);
    vtx_buffer->gpu_buffer = create_gpu_buffer(
        sizeof(compressed_chunk_mesh_vertex_t) * vertex_count,
        vertices,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    res.world_mesh.vertex_offset = 0;
    res.world_mesh.vertex_count = vertex_count;
    res.world_mesh.first_index = 0;
    res.world_mesh.index_offset = 0;
    res.world_mesh.index_count = 0;
    res.world_mesh.index_type = VK_INDEX_TYPE_UINT32;

    create_mesh_vbo_final_list(&res.world_mesh);

    res.world_render_data.model = matrix4_t(1.0f);
    res.world_render_data.pbr_info.x = 0.1f;
    res.world_render_data.pbr_info.y = 0.1f;
    res.world_render_data.color = vector4_t(0.0f);

    res.position = ws_position;
    res.view_direction = ws_view_direction;
    res.up_vector = vector3_t(0.0f, 1.0f, 0.0f);

    return res;
}

mesh_t *dr_get_mesh_rsc(game_mesh_t mesh) {
    return &meshes[mesh];
}

shader_t *dr_get_shader_rsc(game_shader_t shader) {
    return &shaders[shader];
}
