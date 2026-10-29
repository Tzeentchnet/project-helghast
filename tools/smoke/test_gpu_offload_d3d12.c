/* Game-free D3D12 execution test for GPU vertex fetch and surface writeback packing. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsx_vtx_gpu.h"
#include "rsx_wb_gpu.h"

#define CHECK_HR(expr) do { \
    HRESULT check_hr = (expr); \
    if (FAILED(check_hr)) { \
        fprintf(stderr, "[FAIL] %s: 0x%08lX at line %d\n", #expr, (unsigned long)check_hr, __LINE__); \
        return 0; \
    } \
} while (0)

typedef struct gpu_context {
    ID3D12Device* dev;
    ID3D12CommandQueue* queue;
    ID3D12CommandAllocator* allocator;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence* fence;
    HANDLE event;
    UINT64 fence_value;
} gpu_context;

static int gpu_init(gpu_context* c)
{
    memset(c, 0, sizeof *c);
    IDXGIFactory4* factory = NULL;
    IDXGIAdapter* adapter = NULL;
    const char* force_warp = getenv("RSX_TEST_WARP");
    const char* adapter_env = getenv("RSX_TEST_ADAPTER");
    unsigned long adapter_index = 0;
    if (adapter_env && *adapter_env) {
        char* end = NULL;
        adapter_index = strtoul(adapter_env, &end, 10);
        if (*end || adapter_index > UINT_MAX) {
            fprintf(stderr, "[FAIL] invalid RSX_TEST_ADAPTER index: %s\n", adapter_env);
            return 0;
        }
    }
    CHECK_HR(CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory));
    if (force_warp && !strcmp(force_warp, "1")) {
        CHECK_HR(IDXGIFactory4_EnumWarpAdapter(factory, &IID_IDXGIAdapter, (void**)&adapter));
    } else {
        CHECK_HR(IDXGIFactory4_EnumAdapters(factory, (UINT)adapter_index, &adapter));
    }
    DXGI_ADAPTER_DESC desc;
    CHECK_HR(IDXGIAdapter_GetDesc(adapter, &desc));
    HRESULT hr = D3D12CreateDevice((IUnknown*)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&c->dev);
    IDXGIAdapter_Release(adapter);
    IDXGIFactory4_Release(factory);
    if (FAILED(hr)) {
        fprintf(stderr, "[FAIL] D3D12CreateDevice: 0x%08lX\n", (unsigned long)hr);
        return 0;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {0};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK_HR(ID3D12Device_CreateCommandQueue(c->dev, &qd, &IID_ID3D12CommandQueue, (void**)&c->queue));
    CHECK_HR(ID3D12Device_CreateCommandAllocator(c->dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                &IID_ID3D12CommandAllocator, (void**)&c->allocator));
    CHECK_HR(ID3D12Device_CreateCommandList(c->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->allocator, NULL,
                                           &IID_ID3D12GraphicsCommandList, (void**)&c->list));
    CHECK_HR(ID3D12Device_CreateFence(c->dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&c->fence));
    c->event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!c->event) {
        fprintf(stderr, "[FAIL] CreateEventA: %lu\n", (unsigned long)GetLastError());
        return 0;
    }
    printf("[INFO] D3D12 adapter: %ls\n", desc.Description);
    return 1;
}

static int gpu_submit(gpu_context* c)
{
    CHECK_HR(ID3D12GraphicsCommandList_Close(c->list));
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)c->list};
    ID3D12CommandQueue_ExecuteCommandLists(c->queue, 1, lists);
    CHECK_HR(ID3D12CommandQueue_Signal(c->queue, c->fence, ++c->fence_value));
    CHECK_HR(ID3D12Fence_SetEventOnCompletion(c->fence, c->fence_value, c->event));
    const DWORD wait = WaitForSingleObject(c->event, 30000);
    if (wait != WAIT_OBJECT_0) {
        fprintf(stderr, "[FAIL] GPU fence wait: %lu\n", (unsigned long)wait);
        return 0;
    }
    CHECK_HR(ID3D12CommandAllocator_Reset(c->allocator));
    CHECK_HR(ID3D12GraphicsCommandList_Reset(c->list, c->allocator, NULL));
    return 1;
}

static void gpu_shutdown(gpu_context* c)
{
    if (c->list) ID3D12GraphicsCommandList_Release(c->list);
    if (c->allocator) ID3D12CommandAllocator_Release(c->allocator);
    if (c->fence) ID3D12Fence_Release(c->fence);
    if (c->queue) ID3D12CommandQueue_Release(c->queue);
    if (c->dev) ID3D12Device_Release(c->dev);
    if (c->event) CloseHandle(c->event);
}

static int make_buffer(gpu_context* c, D3D12_HEAP_TYPE heap, UINT64 bytes, D3D12_RESOURCE_FLAGS flags,
                       D3D12_RESOURCE_STATES state, ID3D12Resource** out)
{
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = heap;
    D3D12_RESOURCE_DESC d = {0};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags;
    CHECK_HR(ID3D12Device_CreateCommittedResource(c->dev, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL,
                                                  &IID_ID3D12Resource, (void**)out));
    return 1;
}

static int make_texture(gpu_context* c, DXGI_FORMAT fmt, UINT width, UINT height, ID3D12Resource** out)
{
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {0};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width; d.Height = height; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    CHECK_HR(ID3D12Device_CreateCommittedResource(c->dev, &hp, D3D12_HEAP_FLAG_NONE, &d,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                  &IID_ID3D12Resource, (void**)out));
    return 1;
}

static int make_pipeline(gpu_context* c, const char* source, D3D12_ROOT_PARAMETER* params, UINT count,
                         ID3D12RootSignature** root, ID3D12PipelineState** pso)
{
    ID3DBlob *shader = NULL, *sig = NULL, *err = NULL;
    const char* stage = "shader";
    HRESULT hr = D3DCompile(source, strlen(source), "gpu-offload-test", NULL, NULL, "main", "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &err);
    if (FAILED(hr)) goto fail;
    if (err) { ID3D10Blob_Release(err); err = NULL; }
    D3D12_ROOT_SIGNATURE_DESC rd = {0};
    rd.NumParameters = count; rd.pParameters = params;
    stage = "root signature serialization";
    hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) goto fail;
    if (err) { ID3D10Blob_Release(err); err = NULL; }
    stage = "root signature";
    hr = ID3D12Device_CreateRootSignature(c->dev, 0, ID3D10Blob_GetBufferPointer(sig),
                                          ID3D10Blob_GetBufferSize(sig), &IID_ID3D12RootSignature, (void**)root);
    if (FAILED(hr)) goto fail;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = *root;
    pd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(shader);
    pd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(shader);
    stage = "compute pipeline";
    hr = ID3D12Device_CreateComputePipelineState(c->dev, &pd, &IID_ID3D12PipelineState, (void**)pso);
    if (FAILED(hr)) goto fail;
    ID3D10Blob_Release(sig); ID3D10Blob_Release(shader);
    return 1;
fail:
    fprintf(stderr, "[FAIL] %s: 0x%08lX %s\n", stage, (unsigned long)hr,
            err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "");
    if (err) ID3D10Blob_Release(err);
    if (sig) ID3D10Blob_Release(sig);
    if (shader) ID3D10Blob_Release(shader);
    if (*root) { ID3D12RootSignature_Release(*root); *root = NULL; }
    return 0;
}

static void transition(gpu_context* c, ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(c->list, 1, &b);
}

static uint32_t rng = 0x9E3779B9u;
static uint32_t next_u32(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static void reference_word(const uint8_t* src, uint8_t* dst, unsigned mode)
{
    if (mode == RSX_WB_GPU_RGBA8) {
        dst[0] = src[3]; dst[1] = src[0]; dst[2] = src[1]; dst[3] = src[2];
    } else if (mode == RSX_WB_GPU_HALF) {
        dst[0] = src[1]; dst[1] = src[0]; dst[2] = src[3]; dst[3] = src[2];
    } else {
        dst[0] = src[3]; dst[1] = src[2]; dst[2] = src[1]; dst[3] = src[0];
    }
}

static int test_packing(gpu_context* c)
{
    D3D12_ROOT_PARAMETER rp[2] = {0};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = 4;
    ID3D12RootSignature* root = NULL; ID3D12PipelineState* pso = NULL;
    if (!make_pipeline(c, RSX_WB_GPU_HLSL, rp, 2, &root, &pso)) return 0;
    enum { PITCH = 256, ROWS = 7, BYTES = PITCH * ROWS };
    uint8_t source[BYTES], expected[BYTES];
    for (unsigned mode = 0; mode < 3; mode++) {
        const unsigned words = 19u + mode;
        const unsigned width = mode == RSX_WB_GPU_HALF ? words / 2u : words;
        const DXGI_FORMAT fmt = mode == RSX_WB_GPU_RGBA8 ? DXGI_FORMAT_R8G8B8A8_UNORM :
                                mode == RSX_WB_GPU_HALF ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32_FLOAT;
        for (unsigned i = 0; i < BYTES; i++) source[i] = (uint8_t)next_u32();
        static const uint32_t special[] = {0, 0x80000000u, 0x7F800000u, 0xFF800000u,
                                           0x7FC01234u, 0x7F800001u, 0x7C00FC00u, 0x7E00FE00u};
        memcpy(source, special, sizeof special);
        memcpy(expected, source, BYTES);
        for (unsigned y = 0; y < ROWS; y++)
            for (unsigned x = 0; x < words; x++)
                reference_word(source + y * PITCH + x * 4u, expected + y * PITCH + x * 4u, mode);
        ID3D12Resource *upload = NULL, *work = NULL, *readback = NULL, *texture = NULL;
        if (!make_buffer(c, D3D12_HEAP_TYPE_UPLOAD, BYTES, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload) ||
            !make_buffer(c, D3D12_HEAP_TYPE_DEFAULT, BYTES, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_COPY_DEST, &work) ||
            !make_buffer(c, D3D12_HEAP_TYPE_READBACK, BYTES, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback) ||
            !make_texture(c, fmt, width, ROWS, &texture)) return 0;
        uint8_t* mapped = NULL;
        CHECK_HR(ID3D12Resource_Map(upload, 0, NULL, (void**)&mapped));
        memcpy(mapped, source, BYTES);
        ID3D12Resource_Unmap(upload, 0, NULL);
        ID3D12GraphicsCommandList_CopyBufferRegion(c->list, work, 0, upload, 0, BYTES);
        D3D12_TEXTURE_COPY_LOCATION texture_copy = {0}, buffer_copy = {0};
        texture_copy.pResource = texture; texture_copy.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        buffer_copy.pResource = upload; buffer_copy.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        buffer_copy.PlacedFootprint.Footprint.Format = fmt;
        buffer_copy.PlacedFootprint.Footprint.Width = width;
        buffer_copy.PlacedFootprint.Footprint.Height = ROWS;
        buffer_copy.PlacedFootprint.Footprint.Depth = 1;
        buffer_copy.PlacedFootprint.Footprint.RowPitch = PITCH;
        ID3D12GraphicsCommandList_CopyTextureRegion(c->list, &texture_copy, 0, 0, 0, &buffer_copy, NULL);
        transition(c, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(c, texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        buffer_copy.pResource = work;
        ID3D12GraphicsCommandList_CopyTextureRegion(c->list, &buffer_copy, 0, 0, 0, &texture_copy, NULL);
        transition(c, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(c, work, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12GraphicsCommandList_SetPipelineState(c->list, pso);
        ID3D12GraphicsCommandList_SetComputeRootSignature(c->list, root);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(c->list, 0, ID3D12Resource_GetGPUVirtualAddress(work));
        const UINT constants[4] = {words, ROWS, PITCH, mode};
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(c->list, 1, 4, constants, 0);
        ID3D12GraphicsCommandList_Dispatch(c->list, (words + 7u) / 8u, (ROWS + 7u) / 8u, 1);
        transition(c, work, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_CopyBufferRegion(c->list, readback, 0, work, 0, BYTES);
        if (!gpu_submit(c)) return 0;
        D3D12_RANGE range = {0, BYTES};
        CHECK_HR(ID3D12Resource_Map(readback, 0, &range, (void**)&mapped));
        int same = 1;
        for (unsigned y = 0; y < ROWS; y++)
            if (memcmp(mapped + y * PITCH, expected + y * PITCH, words * 4u)) same = 0;
        D3D12_RANGE no_write = {0, 0};
        ID3D12Resource_Unmap(readback, 0, &no_write);
        ID3D12Resource_Release(texture); ID3D12Resource_Release(readback);
        ID3D12Resource_Release(work); ID3D12Resource_Release(upload);
        if (!same) {
            fprintf(stderr, "[FAIL] GPU writeback packing mode %u\n", mode);
            return 0;
        }
    }
    ID3D12PipelineState_Release(pso); ID3D12RootSignature_Release(root);
    puts("[PASS] GPU writeback packing: pitched RGBA8, half and float textures");
    return 1;
}

#define SOURCE_BYTES (1u << 16)
#define MAX_REFS 32u
static uint8_t guest[SOURCE_BYTES];
static float lut[RSX_VG_LUT_WORDS];
static rsx_vertex_ref refs[MAX_REFS];
static uint8_t cpu_vertices[MAX_REFS * 16u * 16u];

u32 rsx_dsp_vertex_data_base_offset(const rsx_dispatch* rsx) { (void)rsx; return 0; }
void rsx_dsp_get_vertex_attr(const rsx_dispatch* rsx, u32 index, rsx_dsp_vertex_attr* out)
{ (void)rsx; (void)index; memset(out, 0, sizeof *out); }
void rsx_dsp_vertex_default(const rsx_dispatch* rsx, u32 attr, float out[4])
{ (void)rsx; (void)attr; out[0] = out[1] = out[2] = 0; out[3] = 1; }

static const u8* guest_ptr(void* user, u32 location, u32 offset, u32 bytes)
{
    (void)user;
    if (location || (u64)offset + bytes > SOURCE_BYTES) return NULL;
    return guest + offset;
}

static void set_attr(rsx_vertex_fetch_plan* p, u32 a, u32 type, u32 size, u32 elem, u32 stride,
                     u32 offset, u32 frequency)
{
    rsx_vertex_fetch_attr* f = &p->attr[a];
    f->desc.type = type; f->desc.size = size; f->desc.stride = stride;
    f->desc.offset = offset; f->desc.frequency = frequency;
    f->elem_size = elem; f->stride = stride;
    f->default_value[0] = 0.25f; f->default_value[1] = -2.0f;
    f->default_value[2] = 0.75f; f->default_value[3] = 1.0f;
}

static const char vertex_shader[] = RSX_VG_HLSL
    "ByteAddressBuffer test_refs : register(t22);\n"
    "RWByteAddressBuffer test_out : register(u0);\n"
    "cbuffer TestParams : register(b0) { uint test_count; uint test_attrs; };\n"
    "[numthreads(64, 1, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x < test_count * test_attrs) {\n"
    "        uint i = id.x / test_attrs; uint a = id.x % test_attrs;\n"
    "        uint2 r = uint2(test_refs.Load(i * 8u), test_refs.Load(i * 8u + 4u));\n"
    "        test_out.Store4(id.x * 16u, asuint(vg_attr(a, r)));\n"
    "    }\n"
    "}\n";

static UINT64 align256(UINT64 n) { return (n + 255u) & ~(UINT64)255u; }

static int test_vertex_draw(gpu_context* c, rsx_vertex_fetch_plan* p, u32 count, const char* label)
{
    rsx_vertex_fetch_plan_prepare(p, refs, count);
    rsx_vg_draw draw;
    const int reason = rsx_vg_plan(p, refs, count, 1u << 20, &draw);
    if (reason != RSX_VG_OK || !rsx_vertex_fetch_many(p, refs, count, cpu_vertices)) {
        fprintf(stderr, "[FAIL] %s: CPU planner or fetch failed (%d)\n", label, reason);
        return 0;
    }
    D3D12_ROOT_PARAMETER rp[5] = {0};
    for (u32 i = 0; i < 3; i++) {
        rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[i].Descriptor.ShaderRegister = 20u + i;
    }
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[4].Constants.Num32BitValues = 2;
    ID3D12RootSignature* root = NULL; ID3D12PipelineState* pso = NULL;
    if (!make_pipeline(c, vertex_shader, rp, 5, &root, &pso)) return 0;
    const UINT64 lut_off = align256(draw.block_bytes);
    const UINT64 refs_off = align256(lut_off + sizeof lut);
    const UINT64 upload_bytes = refs_off + (UINT64)count * sizeof refs[0];
    const UINT64 result_bytes = (UINT64)count * p->layout.stride;
    ID3D12Resource *upload = NULL, *output = NULL, *readback = NULL;
    if (!make_buffer(c, D3D12_HEAP_TYPE_UPLOAD, upload_bytes, 0, D3D12_RESOURCE_STATE_GENERIC_READ, &upload) ||
        !make_buffer(c, D3D12_HEAP_TYPE_DEFAULT, result_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output) ||
        !make_buffer(c, D3D12_HEAP_TYPE_READBACK, result_bytes, 0, D3D12_RESOURCE_STATE_COPY_DEST, &readback)) return 0;
    uint8_t* mapped = NULL;
    CHECK_HR(ID3D12Resource_Map(upload, 0, NULL, (void**)&mapped));
    rsx_vg_write_block(&draw, mapped);
    memcpy(mapped + lut_off, lut, sizeof lut);
    memcpy(mapped + refs_off, refs, (size_t)count * sizeof refs[0]);
    ID3D12Resource_Unmap(upload, 0, NULL);
    const D3D12_GPU_VIRTUAL_ADDRESS va = ID3D12Resource_GetGPUVirtualAddress(upload);
    ID3D12GraphicsCommandList_SetPipelineState(c->list, pso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(c->list, root);
    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(c->list, 0, va);
    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(c->list, 1, va + lut_off);
    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(c->list, 2, va + refs_off);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(c->list, 3, ID3D12Resource_GetGPUVirtualAddress(output));
    const UINT constants[2] = {count, p->layout.count};
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(c->list, 4, 2, constants, 0);
    ID3D12GraphicsCommandList_Dispatch(c->list, (count * p->layout.count + 63u) / 64u, 1, 1);
    transition(c, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(c->list, readback, 0, output, 0, result_bytes);
    if (!gpu_submit(c)) return 0;
    D3D12_RANGE range = {0, (SIZE_T)result_bytes};
    CHECK_HR(ID3D12Resource_Map(readback, 0, &range, (void**)&mapped));
    int same = 1;
    for (UINT64 i = 0; i < result_bytes; i += 4u) {
        if (memcmp(mapped + i, cpu_vertices + i, 4)) {
            u32 got, want;
            memcpy(&got, mapped + i, 4); memcpy(&want, cpu_vertices + i, 4);
            fprintf(stderr, "[FAIL] %s word %llu: got %08X want %08X\n", label,
                    (unsigned long long)(i / 4u), got, want);
            same = 0;
            break;
        }
    }
    D3D12_RANGE no_write = {0, 0};
    ID3D12Resource_Unmap(readback, 0, &no_write);
    ID3D12Resource_Release(readback); ID3D12Resource_Release(output); ID3D12Resource_Release(upload);
    ID3D12PipelineState_Release(pso); ID3D12RootSignature_Release(root);
    if (same) printf("[PASS] GPU vertex fetch: %s\n", label);
    return same;
}

static int test_vertices(gpu_context* c)
{
    for (u32 i = 0; i < SOURCE_BYTES; i++) guest[i] = (u8)next_u32();
    rsx_vg_lut_build(lut);
    rsx_vertex_fetch_plan p;
    memset(&p, 0, sizeof p);
    rsx_vertex_layout_plan_init(&p.layout, 0xFFu);
    p.guest_ptr = guest_ptr;
    static const u32 types[] = {2, 3, 4, 7, 1, 5, 6, 0};
    static const u32 sizes[] = {4, 2, 4, 4, 2, 3, 1, 0};
    static const u32 elems[] = {16, 4, 4, 4, 4, 6, 4, 0};
    static const u32 freqs[] = {0, 1, 1, 2, 3, 7, 65535, 0};
    p.divider_mask = (1u << 2) | (1u << 4) | (1u << 5) | (1u << 6);
    for (u32 a = 0; a < 8; a++)
        set_attr(&p, a, types[a], sizes[a], elems[a], elems[a] + (a % 3u), a * 7000u, freqs[a]);
    for (u32 i = 0; i < MAX_REFS; i++) {
        refs[i].vertex_id = (i * 3u) % 70u;
        refs[i].base_index = (i % 5u) * 7u;
    }
    if (!test_vertex_draw(c, &p, MAX_REFS, "formats and divider modes")) return 0;
    memset(&p, 0, sizeof p);
    rsx_vertex_layout_plan_init(&p.layout, 3u);
    set_attr(&p, 0, 2, 1, 4, 4, 0, 1);
    set_attr(&p, 1, 3, 2, 4, 4, 0, 0);
    p.divider_mask = 1u;
    p.inline_data = guest; p.inline_bytes = 16u; p.inline_stride = 8u;
    p.inline_off[0] = 0; p.inline_off[1] = 4;
    for (u32 i = 0; i < MAX_REFS; i++) { refs[i].vertex_id = i; refs[i].base_index = 0; }
    refs[2].vertex_id = 0xFFFFFFFFu;
    return test_vertex_draw(c, &p, MAX_REFS, "inline modulo and out-of-range defaults");
}

int main(void)
{
    gpu_context c;
    if (!gpu_init(&c)) return 1;
    const int ok = test_packing(&c) && test_vertices(&c);
    gpu_shutdown(&c);
    puts(ok ? "[PASS] test_gpu_offload_d3d12" : "[FAIL] test_gpu_offload_d3d12");
    return ok ? 0 : 1;
}
