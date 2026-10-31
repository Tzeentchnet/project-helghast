#pragma once

#include <stdint.h>

#if defined(_WIN32)
#include <d3d12.h>
#else
typedef struct ID3D12Device ID3D12Device;
typedef struct ID3D12GraphicsCommandList ID3D12GraphicsCommandList;
typedef struct ID3D12Resource ID3D12Resource;
#endif

#if defined(__cplusplus)
extern "C" {
#endif

enum kz3_xess_camera_flags {
    KZ3_XESS_CAMERA_CURRENT_VALID = 1u << 0,
    KZ3_XESS_CAMERA_PREVIOUS_VALID = 1u << 1,
    KZ3_XESS_CAMERA_PLANES_VALID = 1u << 2,
    KZ3_XESS_CAMERA_RESET = 1u << 3,
    KZ3_XESS_CAMERA_MULTIPLE_VIEWS = 1u << 4,
};

typedef struct kz3_xess_camera_state {
    uint64_t guest_flip;
    uint64_t view_id;
    float current_pose[16];
    float previous_pose[16];
    float current_horizontal_fov;
    float current_vertical_fov;
    float previous_horizontal_fov;
    float previous_vertical_fov;
    float near_plane;
    float far_plane;
    uint32_t flags;
} kz3_xess_camera_state;

typedef struct kz3_xess_camera_dispatch_desc {
    ID3D12GraphicsCommandList* command_list;
    ID3D12Resource* color;
    ID3D12Resource* depth;
    kz3_xess_camera_state camera;
    uint32_t render_width;
    uint32_t render_height;
    uint32_t output_width;
    uint32_t output_height;
    float jitter_x;
    float jitter_y;
    uint32_t reset;
    uint32_t use_responsive_mask;
} kz3_xess_camera_dispatch_desc;

int kz3_xess_initialize(
    ID3D12Device* device,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height);
int kz3_xess_get_jitter(
    uint64_t frame_index,
    uint32_t render_width,
    uint32_t output_width,
    float* out_x,
    float* out_y);
void kz3_xess_note_camera(
    uint64_t guest_flip,
    uint64_t view_id,
    const float pose[16],
    float horizontal_fov,
    float vertical_fov);
void kz3_xess_note_camera_planes(
    uint64_t guest_flip,
    float near_plane,
    float far_plane);
int kz3_xess_get_camera(kz3_xess_camera_state* out_state);
int kz3_xess_capture_opaque_color(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height);
int kz3_xess_capture_pre_upscale_color(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height);
ID3D12Resource* kz3_xess_dispatch_camera(
    const kz3_xess_camera_dispatch_desc* desc);
void kz3_xess_shutdown(void);
const char* kz3_xess_status(void);

#if defined(__cplusplus)
}
#endif
