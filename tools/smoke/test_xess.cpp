#include "xess.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <d3d12.h>

namespace {

template <typename T>
void release(T*& object)
{
    if (object) {
        object->Release();
        object = nullptr;
    }
}

bool check(HRESULT result, const char* action)
{
    if (SUCCEEDED(result)) {
        return true;
    }
    fprintf(
        stderr,
        "[test_xess] %s failed (HRESULT 0x%08lx)\n",
        action,
        result);
    return false;
}

bool create_device(ID3D12Device** out_device)
{
    const HRESULT result = D3D12CreateDevice(
        nullptr,
        D3D_FEATURE_LEVEL_12_0,
        IID_ID3D12Device,
        reinterpret_cast<void**>(out_device));
    if (!check(result, "D3D12CreateDevice")) {
        return false;
    }
    puts("[test_xess] using the default hardware adapter");
    return true;
}

bool create_texture(
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial_state,
    ID3D12Resource** out_resource)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return check(
        device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            initial_state,
            nullptr,
            IID_ID3D12Resource,
            reinterpret_cast<void**>(out_resource)),
        "CreateCommittedResource(texture)");
}

bool upload_input(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* list,
    ID3D12Resource* texture,
    std::vector<ID3D12Resource*>& uploads,
    bool alternate_color = false)
{
    const D3D12_RESOURCE_DESC texture_desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(
        &texture_desc,
        0,
        1,
        0,
        &footprint,
        &rows,
        &row_bytes,
        &total_bytes);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total_bytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* upload = nullptr;
    if (!check(
            device->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &buffer,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_ID3D12Resource,
                reinterpret_cast<void**>(&upload)),
            "CreateCommittedResource(upload)")) {
        return false;
    }

    void* mapped = nullptr;
    if (!check(upload->Map(0, nullptr, &mapped), "Map(upload)")) {
        release(upload);
        return false;
    }
    memset(mapped, 0, static_cast<size_t>(total_bytes));
    unsigned char* pixels =
        static_cast<unsigned char*>(mapped) + footprint.Offset;
    if (texture_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        for (UINT y = 0; y < texture_desc.Height; ++y) {
            unsigned char* row =
                pixels + y * footprint.Footprint.RowPitch;
            for (UINT64 x = 0; x < texture_desc.Width; ++x) {
                row[x * 4 + 0] = alternate_color ? 224 : 48;
                row[x * 4 + 1] = alternate_color ? 48 : 112;
                row[x * 4 + 2] = alternate_color ? 80 : 208;
                row[x * 4 + 3] = 255;
            }
        }
    } else if (texture_desc.Format == DXGI_FORMAT_R32_FLOAT) {
        const float scene_depth = 0.5f;
        for (UINT y = 0; y < texture_desc.Height; ++y) {
            unsigned char* row =
                pixels + y * footprint.Footprint.RowPitch;
            for (UINT64 x = 0; x < texture_desc.Width; ++x) {
                memcpy(
                    row + x * sizeof(float),
                    &scene_depth,
                    sizeof(float));
            }
        }
    }
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture;
    destination.Type =
        D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(
        &destination, 0, 0, 0, &source, nullptr);
    uploads.push_back(upload);
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    constexpr uint32_t render_width = 1280;
    constexpr uint32_t render_height = 720;
    constexpr uint32_t output_width = 1920;
    constexpr uint32_t output_height = 1080;
    const bool expect_unsupported =
        argc == 2 &&
        strcmp(argv[1], "--expect-unsupported") == 0;

    {
        float pose[16] = {};
        for (int i = 0; i < 4; ++i) {
            pose[i * 4 + i] = 1.0f;
        }
        kz3_xess_camera_state camera{};
        kz3_xess_note_camera_planes(100, 0.1f, 1000.0f);
        kz3_xess_note_camera(100, 1, pose, 1.4f, 0.9f);
        if (!kz3_xess_get_camera(&camera) ||
            !(camera.flags & KZ3_XESS_CAMERA_PLANES_VALID)) {
            fputs(
                "[test_xess] camera did not accept coherent "
                "projection planes\n",
                stderr);
            return 1;
        }
        kz3_xess_note_camera_planes(100, 0.2f, 1000.0f);
        if (!kz3_xess_get_camera(&camera) ||
            (camera.flags & KZ3_XESS_CAMERA_PLANES_VALID)) {
            fputs(
                "[test_xess] camera accepted conflicting "
                "projection planes\n",
                stderr);
            return 1;
        }
        kz3_xess_note_camera(101, 1, pose, 1.4f, 0.9f);
        kz3_xess_note_camera_planes(101, 0.1f, 1000.0f);
        if (!kz3_xess_get_camera(&camera) ||
            !(camera.flags & KZ3_XESS_CAMERA_PLANES_VALID)) {
            fputs(
                "[test_xess] camera did not recover on the next frame\n",
                stderr);
            return 1;
        }
        pose[12] = 1.0f;
        kz3_xess_note_camera(102, 1, pose, 1.4f, 0.9f);
        if (!kz3_xess_get_camera(&camera) ||
            camera.guest_flip != 101 ||
            !(camera.flags & KZ3_XESS_CAMERA_PLANES_VALID)) {
            fputs(
                "[test_xess] partial camera replaced the last coherent frame\n",
                stderr);
            return 1;
        }
        kz3_xess_note_camera_planes(102, 0.1f, 1000.0f);
        if (!kz3_xess_get_camera(&camera) ||
            camera.guest_flip != 102 ||
            !(camera.flags & KZ3_XESS_CAMERA_PREVIOUS_VALID) ||
            !(camera.flags & KZ3_XESS_CAMERA_PLANES_VALID)) {
            fputs(
                "[test_xess] camera did not publish atomically after planes\n",
                stderr);
            return 1;
        }
        puts("[test_xess] projection-plane coherence: ok");
    }

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* scene = nullptr;
    ID3D12Resource* overlay = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12Resource* xess_output = nullptr;
    std::vector<ID3D12Resource*> uploads;
    HANDLE completion_event = nullptr;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    int result = 1;

    if (!create_device(&device)) {
        puts("[test_xess] SKIP: a D3D12 hardware device is unavailable");
        return 0;
    }
    if (kz3_xess_initialize(
            device,
            render_width,
            render_height,
            output_width,
            1200)) {
        fputs(
            "[test_xess] XeSS accepted mismatched aspect ratios\n",
            stderr);
        goto done;
    }
    if (!strstr(kz3_xess_status(), "same aspect ratio")) {
        fprintf(
            stderr,
            "[test_xess] unexpected aspect-ratio status: %s\n",
            kz3_xess_status());
        goto done;
    }
    puts("[test_xess] unsupported-aspect fallback: ok");

    if (!kz3_xess_initialize(
            device,
            render_width,
            render_height,
            output_width,
            output_height)) {
        if (expect_unsupported) {
            printf(
                "[test_xess] unavailable-runtime fallback: ok (%s)\n",
                kz3_xess_status());
            result = 0;
            goto done;
        }
        fprintf(stderr, "[test_xess] %s\n", kz3_xess_status());
        goto done;
    }
    if (expect_unsupported) {
        fputs(
            "[test_xess] XeSS unexpectedly initialized with an "
            "invalid adjacent runtime\n",
            stderr);
        goto done;
    }
    if (!strstr(kz3_xess_status(), "API 2.0.2")) {
        fprintf(
            stderr,
            "[test_xess] exact API version was not reported: %s\n",
            kz3_xess_status());
        goto done;
    }

    if (!check(
            device->CreateCommandQueue(
                &queue_desc,
                IID_ID3D12CommandQueue,
                reinterpret_cast<void**>(&queue)),
            "CreateCommandQueue") ||
        !check(
            device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_ID3D12CommandAllocator,
                reinterpret_cast<void**>(&allocator)),
            "CreateCommandAllocator") ||
        !check(
            device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                allocator,
                nullptr,
                IID_ID3D12GraphicsCommandList,
                reinterpret_cast<void**>(&list)),
            "CreateCommandList") ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST,
            &color) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST,
            &depth) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_DEST,
            &scene) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST,
            &overlay) ||
        !upload_input(device, list, color, uploads) ||
        !upload_input(device, list, depth, uploads) ||
        !upload_input(device, list, scene, uploads) ||
        !upload_input(device, list, overlay, uploads, true)) {
        goto done;
    }

    {
        D3D12_RESOURCE_BARRIER barriers[4]{};
        ID3D12Resource* resources[] = {
            color, depth, scene, overlay};
        const D3D12_RESOURCE_STATES states[] = {
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE};
        for (size_t i = 0; i < 4; ++i) {
            barriers[i].Type =
                D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[i].Transition.pResource = resources[i];
            barriers[i].Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[i].Transition.StateBefore =
                D3D12_RESOURCE_STATE_COPY_DEST;
            barriers[i].Transition.StateAfter = states[i];
        }
        list->ResourceBarrier(4, barriers);
    }

    if (!kz3_xess_capture_opaque_color(
            list, scene, render_width, render_height)) {
        fprintf(stderr, "[test_xess] %s\n", kz3_xess_status());
        goto done;
    }
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = scene;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &barrier);
        list->CopyResource(scene, overlay);
        barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
    }
    if (!kz3_xess_capture_pre_upscale_color(
            list, scene, render_width, render_height)) {
        fprintf(stderr, "[test_xess] %s\n", kz3_xess_status());
        goto done;
    }
    if (!kz3_xess_get_jitter(
            0,
            render_width,
            output_width,
            &jitter_x,
            &jitter_y)) {
        fprintf(stderr, "[test_xess] %s\n", kz3_xess_status());
        goto done;
    }

    {
        kz3_xess_camera_dispatch_desc dispatch{};
        dispatch.command_list = list;
        dispatch.color = color;
        dispatch.depth = depth;
        dispatch.camera.guest_flip = 1;
        dispatch.camera.view_id = 1;
        dispatch.camera.flags =
            KZ3_XESS_CAMERA_CURRENT_VALID |
            KZ3_XESS_CAMERA_PREVIOUS_VALID |
            KZ3_XESS_CAMERA_PLANES_VALID |
            KZ3_XESS_CAMERA_RESET;
        for (int i = 0; i < 4; ++i) {
            dispatch.camera.current_pose[i * 4 + i] = 1.0f;
            dispatch.camera.previous_pose[i * 4 + i] = 1.0f;
        }
        dispatch.camera.current_pose[13] = 0.25f;
        dispatch.camera.current_horizontal_fov = 1.4f;
        dispatch.camera.current_vertical_fov = 0.9f;
        dispatch.camera.previous_horizontal_fov = 1.4f;
        dispatch.camera.previous_vertical_fov = 0.9f;
        dispatch.camera.near_plane = 0.1f;
        dispatch.camera.far_plane = 1000.0f;
        dispatch.render_width = render_width;
        dispatch.render_height = render_height;
        dispatch.output_width = output_width;
        dispatch.output_height = output_height;
        dispatch.jitter_x = jitter_x;
        dispatch.jitter_y = jitter_y;
        dispatch.reset = 1;
        dispatch.use_responsive_mask = 1;
        xess_output = kz3_xess_dispatch_camera(&dispatch);
    }
    if (!xess_output) {
        fprintf(stderr, "[test_xess] %s\n", kz3_xess_status());
        goto done;
    }

    {
        const D3D12_RESOURCE_DESC desc = xess_output->GetDesc();
        if (desc.Width != output_width ||
            desc.Height != output_height ||
            desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            fprintf(
                stderr,
                "[test_xess] unexpected output %llux%u format %u\n",
                static_cast<unsigned long long>(desc.Width),
                desc.Height,
                static_cast<unsigned>(desc.Format));
            goto done;
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 row_bytes = 0;
        UINT64 total_bytes = 0;
        device->GetCopyableFootprints(
            &desc,
            0,
            1,
            0,
            &footprint,
            &rows,
            &row_bytes,
            &total_bytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total_bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (!check(
                device->CreateCommittedResource(
                    &heap,
                    D3D12_HEAP_FLAG_NONE,
                    &buffer,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_ID3D12Resource,
                    reinterpret_cast<void**>(&readback)),
                "CreateCommittedResource(readback)")) {
            goto done;
        }
        D3D12_RESOURCE_BARRIER ready{};
        ready.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        ready.Transition.pResource = xess_output;
        ready.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ready.Transition.StateBefore =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ready.Transition.StateAfter =
            D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &ready);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = xess_output;
        source.Type =
            D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback;
        destination.Type =
            D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        list->CopyTextureRegion(
            &destination, 0, 0, 0, &source, nullptr);
    }

    if (!check(list->Close(), "Close(command list)")) {
        goto done;
    }
    {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
    }
    if (!check(
            device->CreateFence(
                0,
                D3D12_FENCE_FLAG_NONE,
                IID_ID3D12Fence,
                reinterpret_cast<void**>(&fence)),
            "CreateFence")) {
        goto done;
    }
    completion_event =
        CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completion_event) {
        fprintf(
            stderr,
            "[test_xess] CreateEventW failed (%lu)\n",
            GetLastError());
        goto done;
    }
    if (!check(queue->Signal(fence, 1), "Signal(fence)") ||
        !check(
            fence->SetEventOnCompletion(1, completion_event),
            "SetEventOnCompletion(fence)") ||
        WaitForSingleObject(completion_event, 60000) !=
            WAIT_OBJECT_0 ||
        !check(
            device->GetDeviceRemovedReason(),
            "XeSS dispatch completion")) {
        goto done;
    }

    {
        const D3D12_RESOURCE_DESC desc = xess_output->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 row_bytes = 0;
        UINT64 total_bytes = 0;
        device->GetCopyableFootprints(
            &desc,
            0,
            1,
            0,
            &footprint,
            &rows,
            &row_bytes,
            &total_bytes);
        D3D12_RANGE range{
            0, static_cast<SIZE_T>(total_bytes)};
        void* mapped = nullptr;
        if (!check(
                readback->Map(0, &range, &mapped),
                "Map(readback)")) {
            goto done;
        }
        uint64_t nonblack = 0;
        const unsigned char* pixels =
            static_cast<const unsigned char*>(mapped) +
            footprint.Offset;
        for (UINT y = 0; y < output_height; ++y) {
            const unsigned char* row =
                pixels + y * footprint.Footprint.RowPitch;
            for (UINT x = 0; x < output_width; ++x) {
                if (row[x * 4] || row[x * 4 + 1] ||
                    row[x * 4 + 2]) {
                    nonblack++;
                }
            }
        }
        readback->Unmap(0, nullptr);
        if (nonblack <=
            static_cast<uint64_t>(output_width) *
                output_height / 2u) {
            fprintf(
                stderr,
                "[test_xess] output is unexpectedly empty "
                "(%llu/%u non-black pixels)\n",
                static_cast<unsigned long long>(nonblack),
                output_width * output_height);
            goto done;
        }
        printf(
            "[test_xess] output readback: %llu/%u "
            "non-black pixels\n",
            static_cast<unsigned long long>(nonblack),
            output_width * output_height);
    }
    printf(
        "[test_xess] XeSS-SR 2.0.2 depth-motion and responsive-mask "
        "dispatch: ok (jitter %.6f, %.6f)\n",
        jitter_x,
        jitter_y);
    result = 0;

done:
    if (completion_event) {
        CloseHandle(completion_event);
    }
    kz3_xess_shutdown();
    for (ID3D12Resource*& upload : uploads) {
        release(upload);
    }
    release(readback);
    release(overlay);
    release(scene);
    release(depth);
    release(color);
    release(fence);
    release(list);
    release(allocator);
    release(queue);
    release(device);
    return result;
}
