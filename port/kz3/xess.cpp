#include "xess.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <d3dcompiler.h>

#include "xess_api.h"

namespace {

constexpr wchar_t kRuntimeDll[] = L"libxess.dll";
constexpr uint16_t kApiMajor = 2;
constexpr uint16_t kApiMinor = 0;
constexpr uint16_t kApiPatch = 2;

struct XessState {
    std::mutex mutex;
    HMODULE module = nullptr;
    kz3_xess_functions functions{};
    kz3_xess_context_handle context = nullptr;
    ID3D12Device* device = nullptr;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    enum kz3_xess_quality quality = KZ3_XESS_QUALITY_AA;
    ID3D12Resource* motion_vectors = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* opaque_color = nullptr;
    ID3D12Resource* pre_upscale_color = nullptr;
    ID3D12Resource* responsive_mask = nullptr;
    ID3D12DescriptorHeap* motion_heap = nullptr;
    ID3D12RootSignature* motion_root_signature = nullptr;
    ID3D12PipelineState* motion_pipeline = nullptr;
    ID3D12DescriptorHeap* responsive_heap = nullptr;
    ID3D12RootSignature* responsive_root_signature = nullptr;
    ID3D12PipelineState* responsive_pipeline = nullptr;
    D3D12_RESOURCE_STATES opaque_color_state =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES pre_upscale_color_state =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool motion_is_compute_read = false;
    bool output_is_pixel_read = false;
    bool responsive_is_compute_read = false;
    uint64_t capture_serial = 0;
    uint64_t opaque_capture_serial = 0;
    uint64_t pre_upscale_capture_serial = 0;
    uint64_t dispatch_count = 0;
    uint64_t history_reset_count = 0;
    ID3D12Resource* capture_source = nullptr;
    std::string status = "XeSS-SR has not been initialized";
};

XessState g_xess;

struct CameraBridge {
    std::mutex mutex;
    kz3_xess_camera_state state{};
    kz3_xess_camera_state pending{};
    uint64_t plane_flip = UINT64_MAX;
    float near_plane = 0.0f;
    float far_plane = 0.0f;
    bool planes_ambiguous = false;
};

CameraBridge g_camera;

void set_status(const char* format, ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    g_xess.status = message;
}

void set_win32_status(const char* action)
{
    const DWORD error = GetLastError();
    char system_message[256] = {};
    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        system_message,
        static_cast<DWORD>(sizeof(system_message)),
        nullptr);
    size_t length = strlen(system_message);
    while (length && (system_message[length - 1] == '\r' ||
                      system_message[length - 1] == '\n')) {
        system_message[--length] = '\0';
    }
    set_status(
        "%s failed (Win32 error %lu%s%s)",
        action,
        static_cast<unsigned long>(error),
        system_message[0] ? ": " : "",
        system_message);
}

const char* result_name(int result)
{
    switch (result) {
    case KZ3_XESS_SUCCESS:
        return "success";
    case KZ3_XESS_WARNING_OLD_DRIVER:
        return "old driver";
    case KZ3_XESS_ERROR_UNSUPPORTED_DEVICE:
        return "unsupported device";
    case KZ3_XESS_ERROR_UNSUPPORTED_DRIVER:
        return "unsupported driver";
    case KZ3_XESS_ERROR_UNINITIALIZED:
        return "uninitialized";
    case KZ3_XESS_ERROR_INVALID_ARGUMENT:
        return "invalid argument";
    case KZ3_XESS_ERROR_DEVICE_OUT_OF_MEMORY:
        return "device out of memory";
    case KZ3_XESS_ERROR_DEVICE:
        return "device error";
    case KZ3_XESS_ERROR_NOT_IMPLEMENTED:
        return "not implemented";
    case KZ3_XESS_ERROR_INVALID_CONTEXT:
        return "invalid context";
    case KZ3_XESS_ERROR_OPERATION_IN_PROGRESS:
        return "operation in progress";
    case KZ3_XESS_ERROR_UNSUPPORTED:
        return "unsupported";
    case KZ3_XESS_ERROR_CANT_LOAD_LIBRARY:
        return "cannot load library";
    case KZ3_XESS_ERROR_WRONG_CALL_ORDER:
        return "wrong call order";
    default:
        return "unknown";
    }
}

template <typename T>
void release(T*& object)
{
    if (object) {
        object->Release();
        object = nullptr;
    }
}

void destroy_resources_locked()
{
    release(g_xess.responsive_pipeline);
    release(g_xess.responsive_root_signature);
    release(g_xess.responsive_heap);
    release(g_xess.motion_pipeline);
    release(g_xess.motion_root_signature);
    release(g_xess.motion_heap);
    release(g_xess.responsive_mask);
    release(g_xess.pre_upscale_color);
    release(g_xess.opaque_color);
    release(g_xess.output);
    release(g_xess.motion_vectors);
    g_xess.opaque_color_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_xess.pre_upscale_color_state =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_xess.motion_is_compute_read = false;
    g_xess.output_is_pixel_read = false;
    g_xess.responsive_is_compute_read = false;
    g_xess.capture_serial = 0;
    g_xess.opaque_capture_serial = 0;
    g_xess.pre_upscale_capture_serial = 0;
    g_xess.dispatch_count = 0;
    g_xess.history_reset_count = 0;
    g_xess.capture_source = nullptr;
}

void destroy_context_locked()
{
    destroy_resources_locked();
    if (g_xess.context && g_xess.functions.destroy_context) {
        const int result =
            g_xess.functions.destroy_context(g_xess.context);
        if (result != KZ3_XESS_SUCCESS) {
            fprintf(
                stderr,
                "[xess] xessDestroyContext failed (%d: %s)\n",
                result,
                result_name(result));
        }
        g_xess.context = nullptr;
    }
    if (g_xess.device) {
        g_xess.device->Release();
        g_xess.device = nullptr;
    }
    g_xess.render_width = 0;
    g_xess.render_height = 0;
    g_xess.output_width = 0;
    g_xess.output_height = 0;
    g_xess.quality = KZ3_XESS_QUALITY_AA;
}

bool create_texture(
    ID3D12Device* device,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    ID3D12Resource** out_resource)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = width;
    resource.Height = height;
    resource.DepthOrArraySize = 1;
    resource.MipLevels = 1;
    resource.Format = format;
    resource.SampleDesc.Count = 1;
    resource.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const HRESULT hr = device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &resource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_ID3D12Resource,
        reinterpret_cast<void**>(out_resource));
    if (FAILED(hr)) {
        set_status(
            "creating a XeSS-SR texture failed (HRESULT 0x%08lX)",
            static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool compile_compute_shader(
    const char* source,
    const char* name,
    ID3DBlob** out_shader)
{
    ID3DBlob* errors = nullptr;
    const HRESULT hr = D3DCompile(
        source,
        strlen(source),
        name,
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        out_shader,
        &errors);
    if (errors) {
        if (FAILED(hr)) {
            set_status(
                "compiling the XeSS-SR %s shader failed: %s",
                name,
                static_cast<const char*>(errors->GetBufferPointer()));
        }
        errors->Release();
    }
    return SUCCEEDED(hr);
}

bool create_compute_pipeline(
    ID3D12Device* device,
    ID3DBlob* shader,
    D3D12_DESCRIPTOR_RANGE* ranges,
    UINT range_count,
    UINT constant_count,
    ID3D12RootSignature** out_root_signature,
    ID3D12PipelineState** out_pipeline)
{
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = range_count;
    parameters[0].DescriptorTable.pDescriptorRanges = ranges;
    parameters[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0;
    parameters[1].Constants.Num32BitValues = constant_count;
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 2;
    root_desc.pParameters = parameters;

    ID3DBlob* signature = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(
        &root_desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &signature,
        &errors);
    if (errors) {
        if (FAILED(hr)) {
            set_status(
                "serializing a XeSS-SR root signature failed: %s",
                static_cast<const char*>(errors->GetBufferPointer()));
        }
        errors->Release();
    }
    if (SUCCEEDED(hr)) {
        hr = device->CreateRootSignature(
            0,
            signature->GetBufferPointer(),
            signature->GetBufferSize(),
            IID_ID3D12RootSignature,
            reinterpret_cast<void**>(out_root_signature));
    }
    release(signature);
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = *out_root_signature;
        pipeline.CS.pShaderBytecode = shader->GetBufferPointer();
        pipeline.CS.BytecodeLength = shader->GetBufferSize();
        hr = device->CreateComputePipelineState(
            &pipeline,
            IID_ID3D12PipelineState,
            reinterpret_cast<void**>(out_pipeline));
    }
    if (FAILED(hr)) {
        set_status(
            "creating a XeSS-SR compute pipeline failed "
            "(HRESULT 0x%08lX)",
            static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool create_motion_pipeline_locked(ID3D12Device* device)
{
    static constexpr char source[] =
        "cbuffer Camera : register(b0) {\n"
        "  float4 currentRight; float4 currentForward; float4 currentUp;\n"
        "  float4 previousRight; float4 previousForward; float4 previousUp;\n"
        "  float4 currentPosition; float4 previousPosition;\n"
        "  float4 fovTangent; float4 sizeJitter; float4 depthPlanes;\n"
        "};\n"
        "RWTexture2D<float2> motion : register(u0);\n"
        "Texture2D<float> sceneDepth : register(t0);\n"
        "[numthreads(8, 8, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) {\n"
        "  uint2 size = (uint2)sizeJitter.xy;\n"
        "  if (id.x >= size.x || id.y >= size.y) return;\n"
        "  float depth = sceneDepth.Load(int3(id.xy, 0));\n"
        "  if (depth < 0.0 || depth > 1.0) {\n"
        "    motion[id.xy] = 0.0; return;\n"
        "  }\n"
        "  float denominator = depthPlanes.y -\n"
        "      depth * (depthPlanes.y - depthPlanes.x);\n"
        "  if (!(denominator > 0.0)) { motion[id.xy] = 0.0; return; }\n"
        "  float viewDepth = depthPlanes.x * depthPlanes.y / denominator;\n"
        "  float2 ndc = float2((id.x + 0.5) * 2.0 / size.x - 1.0,\n"
        "                      1.0 - (id.y + 0.5) * 2.0 / size.y);\n"
        "  float2 jitter = float2(2.0 * sizeJitter.z / size.x,\n"
        "                         -2.0 * sizeJitter.w / size.y);\n"
        "  float2 currentNdc = ndc - jitter;\n"
        "  float3 ray = currentForward.xyz +\n"
        "      currentRight.xyz * currentNdc.x * fovTangent.x +\n"
        "      currentUp.xyz * currentNdc.y * fovTangent.y;\n"
        "  float3 world = currentPosition.xyz + ray * viewDepth;\n"
        "  float3 previousRelative = world - previousPosition.xyz;\n"
        "  float forward = dot(previousRelative, previousForward.xyz);\n"
        "  if (forward <= 1e-5) { motion[id.xy] = 0.0; return; }\n"
        "  float2 previousNdc = float2(\n"
        "      dot(previousRelative, previousRight.xyz) /\n"
        "          (forward * fovTangent.z),\n"
        "      dot(previousRelative, previousUp.xyz) /\n"
        "          (forward * fovTangent.w));\n"
        "  motion[id.xy] = previousNdc - currentNdc;\n"
        "}\n";

    ID3DBlob* shader = nullptr;
    if (!compile_compute_shader(
            source, "kz3_xess_motion", &shader)) {
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    const bool pipeline_ok = create_compute_pipeline(
        device,
        shader,
        ranges,
        2,
        44,
        &g_xess.motion_root_signature,
        &g_xess.motion_pipeline);
    release(shader);
    if (!pipeline_ok) {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 2;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const HRESULT hr = device->CreateDescriptorHeap(
        &heap,
        IID_ID3D12DescriptorHeap,
        reinterpret_cast<void**>(&g_xess.motion_heap));
    if (FAILED(hr)) {
        set_status(
            "creating the XeSS-SR motion descriptor heap failed "
            "(HRESULT 0x%08lX)",
            static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool create_responsive_pipeline_locked(ID3D12Device* device)
{
    static constexpr char source[] =
        "cbuffer Responsive : register(b0) {\n"
        "  uint width; uint height; uint enabled;\n"
        "};\n"
        "RWTexture2D<float> responsive : register(u0);\n"
        "Texture2D<float4> opaqueColor : register(t0);\n"
        "Texture2D<float4> completeColor : register(t1);\n"
        "[numthreads(8, 8, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) {\n"
        "  if (id.x >= width || id.y >= height) return;\n"
        "  if (!enabled) { responsive[id.xy] = 0.0; return; }\n"
        "  float3 a = opaqueColor.Load(int3(id.xy, 0)).rgb;\n"
        "  float3 b = completeColor.Load(int3(id.xy, 0)).rgb;\n"
        "  float3 d = abs(a - b);\n"
        "  float difference = max(d.r, max(d.g, d.b));\n"
        "  responsive[id.xy] = difference >= 0.2 ? 0.8 : 0.0;\n"
        "}\n";

    ID3DBlob* shader = nullptr;
    if (!compile_compute_shader(
            source, "kz3_xess_responsive", &shader)) {
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 2;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    const bool pipeline_ok = create_compute_pipeline(
        device,
        shader,
        ranges,
        2,
        3,
        &g_xess.responsive_root_signature,
        &g_xess.responsive_pipeline);
    release(shader);
    if (!pipeline_ok) {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 3;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const HRESULT hr = device->CreateDescriptorHeap(
        &heap,
        IID_ID3D12DescriptorHeap,
        reinterpret_cast<void**>(&g_xess.responsive_heap));
    if (FAILED(hr)) {
        set_status(
            "creating the XeSS-SR responsive descriptor heap failed "
            "(HRESULT 0x%08lX)",
            static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool create_resources_locked(
    ID3D12Device* device,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height)
{
    if (!create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R16G16_FLOAT,
            &g_xess.motion_vectors) ||
        !create_texture(
            device,
            output_width,
            output_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            &g_xess.output) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            &g_xess.opaque_color) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            &g_xess.pre_upscale_color) ||
        !create_texture(
            device,
            render_width,
            render_height,
            DXGI_FORMAT_R8_UNORM,
            &g_xess.responsive_mask) ||
        !create_motion_pipeline_locked(device) ||
        !create_responsive_pipeline_locked(device)) {
        destroy_resources_locked();
        return false;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC motion_uav{};
    motion_uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    motion_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(
        g_xess.motion_vectors,
        nullptr,
        &motion_uav,
        g_xess.motion_heap->GetCPUDescriptorHandleForHeapStart());

    const UINT descriptor_size =
        device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        g_xess.responsive_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_UNORDERED_ACCESS_VIEW_DESC responsive_uav{};
    responsive_uav.Format = DXGI_FORMAT_R8_UNORM;
    responsive_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(
        g_xess.responsive_mask, nullptr, &responsive_uav, handle);

    D3D12_SHADER_RESOURCE_VIEW_DESC color_srv{};
    color_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    color_srv.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    color_srv.Texture2D.MipLevels = 1;
    handle.ptr += descriptor_size;
    device->CreateShaderResourceView(
        g_xess.opaque_color, &color_srv, handle);
    handle.ptr += descriptor_size;
    device->CreateShaderResourceView(
        g_xess.pre_upscale_color, &color_srv, handle);
    return true;
}

bool texture_matches(
    ID3D12Resource* texture,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format)
{
    if (!texture) {
        return false;
    }
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           desc.Width == width && desc.Height == height &&
           desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
           desc.Format == format && desc.SampleDesc.Count == 1;
}

bool capture_color_locked(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height,
    bool opaque)
{
    if (!g_xess.context || !command_list ||
        !texture_matches(
            color, width, height, DXGI_FORMAT_R8G8B8A8_UNORM) ||
        width != g_xess.render_width ||
        height != g_xess.render_height) {
        set_status("XeSS-SR scene capture received invalid state or color");
        return false;
    }
    if (!opaque &&
        (!g_xess.opaque_capture_serial ||
         g_xess.capture_source != color ||
         g_xess.pre_upscale_capture_serial ==
             g_xess.opaque_capture_serial)) {
        set_status(
            "XeSS-SR pre-upscale capture has no matching opaque color");
        return false;
    }

    ID3D12Resource* destination =
        opaque ? g_xess.opaque_color : g_xess.pre_upscale_color;
    D3D12_RESOURCE_STATES& destination_state =
        opaque ? g_xess.opaque_color_state :
                 g_xess.pre_upscale_color_state;
    D3D12_RESOURCE_BARRIER before[2]{};
    before[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    before[0].Transition.pResource = color;
    before[0].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    before[0].Transition.StateBefore =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    before[0].Transition.StateAfter =
        D3D12_RESOURCE_STATE_COPY_SOURCE;
    before[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    before[1].Transition.pResource = destination;
    before[1].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    before[1].Transition.StateBefore = destination_state;
    before[1].Transition.StateAfter =
        D3D12_RESOURCE_STATE_COPY_DEST;
    command_list->ResourceBarrier(2, before);
    command_list->CopyResource(destination, color);

    before[0].Transition.StateBefore =
        D3D12_RESOURCE_STATE_COPY_SOURCE;
    before[0].Transition.StateAfter =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    before[1].Transition.StateBefore =
        D3D12_RESOURCE_STATE_COPY_DEST;
    before[1].Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    command_list->ResourceBarrier(2, before);
    destination_state =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if (opaque) {
        if (!++g_xess.capture_serial) {
            ++g_xess.capture_serial;
        }
        g_xess.opaque_capture_serial = g_xess.capture_serial;
        g_xess.pre_upscale_capture_serial = 0;
        g_xess.capture_source = color;
    } else {
        g_xess.pre_upscale_capture_serial =
            g_xess.opaque_capture_serial;
    }
    return true;
}

bool generate_responsive_locked(
    ID3D12GraphicsCommandList* command_list,
    uint32_t width,
    uint32_t height,
    bool enabled)
{
    if (!g_xess.context || !command_list ||
        !g_xess.responsive_mask || !g_xess.opaque_color ||
        !g_xess.pre_upscale_color || !g_xess.responsive_heap ||
        !g_xess.responsive_root_signature ||
        !g_xess.responsive_pipeline) {
        set_status("XeSS-SR responsive-mask resources are unavailable");
        return false;
    }
    if (enabled &&
        (!g_xess.opaque_capture_serial ||
         g_xess.pre_upscale_capture_serial !=
             g_xess.opaque_capture_serial)) {
        set_status(
            "XeSS-SR responsive-mask dispatch has no current scene captures");
        return false;
    }

    if (g_xess.responsive_is_compute_read) {
        D3D12_RESOURCE_BARRIER writable{};
        writable.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        writable.Transition.pResource = g_xess.responsive_mask;
        writable.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        writable.Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        writable.Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        command_list->ResourceBarrier(1, &writable);
        g_xess.responsive_is_compute_read = false;
    }

    const UINT constants[3] = {
        width, height, enabled ? 1u : 0u};
    ID3D12DescriptorHeap* heaps[] = {g_xess.responsive_heap};
    command_list->SetDescriptorHeaps(1, heaps);
    command_list->SetPipelineState(g_xess.responsive_pipeline);
    command_list->SetComputeRootSignature(
        g_xess.responsive_root_signature);
    command_list->SetComputeRootDescriptorTable(
        0,
        g_xess.responsive_heap->GetGPUDescriptorHandleForHeapStart());
    command_list->SetComputeRoot32BitConstants(
        1, 3, constants, 0);
    command_list->Dispatch(
        (width + 7u) / 8u,
        (height + 7u) / 8u,
        1);

    D3D12_RESOURCE_BARRIER readable{};
    readable.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    readable.Transition.pResource = g_xess.responsive_mask;
    readable.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    readable.Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    readable.Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    command_list->ResourceBarrier(1, &readable);
    g_xess.responsive_is_compute_read = true;
    return true;
}

template <typename T>
bool load_export(T* out_function, const char* name)
{
    const FARPROC address = GetProcAddress(g_xess.module, name);
    static_assert(sizeof(*out_function) == sizeof(address));
    memcpy(out_function, &address, sizeof(address));
    if (*out_function) {
        return true;
    }
    set_status("libxess.dll is missing the required %s export", name);
    return false;
}

bool load_runtime_locked()
{
    if (g_xess.module) {
        return true;
    }

    std::vector<wchar_t> path(32768);
    const DWORD length =
        GetModuleFileNameW(
            nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) {
        set_win32_status("GetModuleFileNameW");
        return false;
    }
    wchar_t* slash = wcsrchr(path.data(), L'\\');
    wchar_t* forward_slash = wcsrchr(path.data(), L'/');
    if (!slash || (forward_slash && forward_slash > slash)) {
        slash = forward_slash;
    }
    if (!slash) {
        set_status("the executable path has no directory");
        return false;
    }
    const size_t directory_length =
        static_cast<size_t>(slash - path.data() + 1);
    if (directory_length + std::size(kRuntimeDll) > path.size()) {
        set_status("the XeSS runtime path is too long");
        return false;
    }
    memcpy(
        path.data() + directory_length,
        kRuntimeDll,
        sizeof(kRuntimeDll));

    g_xess.module = LoadLibraryExW(
        path.data(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_xess.module) {
        set_win32_status("loading libxess.dll beside the executable");
        return false;
    }

    bool complete =
        load_export(
            &g_xess.functions.get_version, "xessGetVersion") &&
        load_export(
            &g_xess.functions.create_context,
            "xessD3D12CreateContext") &&
        load_export(
            &g_xess.functions.get_optimal_input_resolution,
            "xessGetOptimalInputResolution") &&
        load_export(&g_xess.functions.init, "xessD3D12Init") &&
        load_export(
            &g_xess.functions.execute, "xessD3D12Execute") &&
        load_export(
            &g_xess.functions.destroy_context,
            "xessDestroyContext");
    const FARPROC optimal_driver =
        GetProcAddress(g_xess.module, "xessIsOptimalDriver");
    static_assert(
        sizeof(g_xess.functions.is_optimal_driver) ==
        sizeof(optimal_driver));
    memcpy(
        &g_xess.functions.is_optimal_driver,
        &optimal_driver,
        sizeof(optimal_driver));
    if (!complete) {
        FreeLibrary(g_xess.module);
        g_xess.module = nullptr;
        g_xess.functions = {};
        return false;
    }

    kz3_xess_version version{};
    const int result = g_xess.functions.get_version(&version);
    if (result != KZ3_XESS_SUCCESS ||
        version.major != kApiMajor ||
        version.minor != kApiMinor ||
        version.patch != kApiPatch ||
        version.reserved != 0) {
        set_status(
            "libxess.dll exposes API %u.%u.%u (result %d); "
            "XeSS SDK 3.0.2 API 2.0.2 is required",
            version.major,
            version.minor,
            version.patch,
            result);
        FreeLibrary(g_xess.module);
        g_xess.module = nullptr;
        g_xess.functions = {};
        return false;
    }
    return true;
}

bool dimensions_valid(
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height)
{
    if (!render_width || !render_height ||
        !output_width || !output_height ||
        render_width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        render_height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        output_width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        output_height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        output_width < render_width ||
        output_height < render_height) {
        return false;
    }
    return static_cast<uint64_t>(render_width) * output_height ==
           static_cast<uint64_t>(render_height) * output_width;
}

enum kz3_xess_quality quality_for_scale(
    uint32_t render_width,
    uint32_t output_width)
{
    const double scale =
        static_cast<double>(output_width) /
        static_cast<double>(render_width);
    if (scale <= 1.001) {
        return KZ3_XESS_QUALITY_AA;
    }
    if (scale <= 1.301) {
        return KZ3_XESS_QUALITY_ULTRA_QUALITY_PLUS;
    }
    if (scale <= 1.501) {
        return KZ3_XESS_QUALITY_ULTRA_QUALITY;
    }
    if (scale <= 1.701) {
        return KZ3_XESS_QUALITY_QUALITY;
    }
    if (scale <= 2.001) {
        return KZ3_XESS_QUALITY_BALANCED;
    }
    if (scale <= 2.301) {
        return KZ3_XESS_QUALITY_PERFORMANCE;
    }
    return KZ3_XESS_QUALITY_ULTRA_PERFORMANCE;
}

const char* quality_name(enum kz3_xess_quality quality)
{
    switch (quality) {
    case KZ3_XESS_QUALITY_ULTRA_PERFORMANCE:
        return "Ultra Performance";
    case KZ3_XESS_QUALITY_PERFORMANCE:
        return "Performance";
    case KZ3_XESS_QUALITY_BALANCED:
        return "Balanced";
    case KZ3_XESS_QUALITY_QUALITY:
        return "Quality";
    case KZ3_XESS_QUALITY_ULTRA_QUALITY:
        return "Ultra Quality";
    case KZ3_XESS_QUALITY_ULTRA_QUALITY_PLUS:
        return "Ultra Quality Plus";
    case KZ3_XESS_QUALITY_AA:
        return "Native AA";
    default:
        return "unknown";
    }
}

bool pose_valid(const float pose[16])
{
    for (unsigned i = 0; i < 16; ++i) {
        if (!std::isfinite(pose[i])) {
            return false;
        }
    }
    for (unsigned row = 0; row < 3; ++row) {
        const float* v = pose + row * 4;
        const float length_squared =
            v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (!(length_squared > 0.81f && length_squared < 1.21f)) {
            return false;
        }
    }
    return true;
}

bool camera_cut(
    const float previous[16],
    const float current[16])
{
    const float dx = current[12] - previous[12];
    const float dy = current[13] - previous[13];
    const float dz = current[14] - previous[14];
    if (dx * dx + dy * dy + dz * dz > 25.0f) {
        return true;
    }
    const float forward_dot = std::clamp(
        previous[4] * current[4] +
            previous[5] * current[5] +
            previous[6] * current[6],
        -1.0f,
        1.0f);
    return forward_dot < std::cos(1.5f);
}

bool projection_plane_matches(float left, float right)
{
    const float scale =
        std::max(1.0f, std::max(std::fabs(left), std::fabs(right)));
    return std::fabs(left - right) <= scale * 1.0e-4f;
}

float halton(uint64_t index, uint32_t base)
{
    float value = 0.0f;
    float fraction = 1.0f;
    while (index) {
        fraction /= static_cast<float>(base);
        value += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return value;
}

void publish_camera_locked(bool force)
{
    const uint32_t ready =
        KZ3_XESS_CAMERA_CURRENT_VALID |
        KZ3_XESS_CAMERA_PLANES_VALID;
    if ((g_camera.pending.flags & ready) == ready ||
        (force &&
         (g_camera.pending.flags &
          KZ3_XESS_CAMERA_CURRENT_VALID))) {
        g_camera.state = g_camera.pending;
    }
}

}  // namespace

extern "C" int kz3_xess_initialize(
    ID3D12Device* device,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t output_width,
    uint32_t output_height)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    if (!device ||
        !dimensions_valid(
            render_width,
            render_height,
            output_width,
            output_height)) {
        set_status(
            "XeSS-SR requires non-decreasing input/output dimensions "
            "with the same aspect ratio");
        return 0;
    }
    if (static_cast<uint64_t>(output_width) >
            static_cast<uint64_t>(render_width) * 3u ||
        static_cast<uint64_t>(output_height) >
            static_cast<uint64_t>(render_height) * 3u) {
        set_status("XeSS-SR output exceeds the supported 3x scale");
        return 0;
    }
    if (g_xess.context && g_xess.device == device &&
        g_xess.render_width == render_width &&
        g_xess.render_height == render_height &&
        g_xess.output_width == output_width &&
        g_xess.output_height == output_height) {
        return 1;
    }

    destroy_context_locked();
    if (!load_runtime_locked()) {
        return 0;
    }

    int result =
        g_xess.functions.create_context(device, &g_xess.context);
    if (result != KZ3_XESS_SUCCESS || !g_xess.context) {
        g_xess.context = nullptr;
        set_status(
            "creating the XeSS-SR context failed (%d: %s)",
            result,
            result_name(result));
        return 0;
    }
    if (g_xess.functions.is_optimal_driver &&
        g_xess.functions.is_optimal_driver(g_xess.context) ==
            KZ3_XESS_WARNING_OLD_DRIVER) {
        fprintf(
            stderr,
            "[xess] warning: the graphics driver is not optimal for "
            "XeSS-SR\n");
    }

    const enum kz3_xess_quality quality =
        quality_for_scale(render_width, output_width);
    const kz3_xess_2d output = {output_width, output_height};
    kz3_xess_2d optimal{}, minimum{}, maximum{};
    result = g_xess.functions.get_optimal_input_resolution(
        g_xess.context,
        &output,
        quality,
        &optimal,
        &minimum,
        &maximum);
    if (result != KZ3_XESS_SUCCESS ||
        render_width < minimum.x || render_width > maximum.x ||
        render_height < minimum.y || render_height > maximum.y) {
        g_xess.functions.destroy_context(g_xess.context);
        g_xess.context = nullptr;
        set_status(
            "XeSS-SR %s rejects %ux%u input for %ux%u output "
            "(result %d, optimal %ux%u, range %ux%u..%ux%u)",
            quality_name(quality),
            render_width,
            render_height,
            output_width,
            output_height,
            result,
            optimal.x,
            optimal.y,
            minimum.x,
            minimum.y,
            maximum.x,
            maximum.y);
        return 0;
    }

    kz3_xess_d3d12_init_params init{};
    init.output_resolution = output;
    init.quality_setting = quality;
    init.init_flags =
        KZ3_XESS_INIT_RESPONSIVE_PIXEL_MASK |
        KZ3_XESS_INIT_USE_NDC_VELOCITY |
        KZ3_XESS_INIT_LDR_INPUT_COLOR;
    result = g_xess.functions.init(g_xess.context, &init);
    if (result != KZ3_XESS_SUCCESS) {
        g_xess.functions.destroy_context(g_xess.context);
        g_xess.context = nullptr;
        set_status(
            "initializing XeSS-SR failed (%d: %s)",
            result,
            result_name(result));
        return 0;
    }
    if (!create_resources_locked(
            device,
            render_width,
            render_height,
            output_width,
            output_height)) {
        g_xess.functions.destroy_context(g_xess.context);
        g_xess.context = nullptr;
        return 0;
    }

    device->AddRef();
    g_xess.device = device;
    g_xess.render_width = render_width;
    g_xess.render_height = render_height;
    g_xess.output_width = output_width;
    g_xess.output_height = output_height;
    g_xess.quality = quality;
    set_status(
        "XeSS-SR API 2.0.2 %s initialized for %ux%u -> %ux%u",
        quality_name(quality),
        render_width,
        render_height,
        output_width,
        output_height);
    return 1;
}

extern "C" int kz3_xess_get_jitter(
    uint64_t frame_index,
    uint32_t render_width,
    uint32_t output_width,
    float* out_x,
    float* out_y)
{
    if (!out_x || !out_y || !render_width ||
        output_width <= render_width) {
        std::lock_guard<std::mutex> lock(g_xess.mutex);
        set_status("XeSS-SR jitter query received invalid arguments");
        return 0;
    }
    const double scale =
        static_cast<double>(output_width) /
        static_cast<double>(render_width);
    const uint32_t phase_count = std::clamp(
        static_cast<uint32_t>(
            std::ceil(8.0 * scale * scale)),
        8u,
        64u);
    const uint64_t index = frame_index % phase_count + 1u;
    *out_x = halton(index, 2) - 0.5f;
    *out_y = halton(index, 3) - 0.5f;
    return 1;
}

extern "C" void kz3_xess_note_camera(
    uint64_t guest_flip,
    uint64_t view_id,
    const float pose[16],
    float horizontal_fov,
    float vertical_fov)
{
    if (!pose || !pose_valid(pose) ||
        !std::isfinite(horizontal_fov) ||
        !std::isfinite(vertical_fov) ||
        horizontal_fov <= 0.0f || horizontal_fov >= 3.13f ||
        vertical_fov <= 0.0f || vertical_fov >= 3.13f) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_camera.mutex);
    kz3_xess_camera_state& pending = g_camera.pending;
    if ((pending.flags & KZ3_XESS_CAMERA_CURRENT_VALID) &&
        guest_flip < pending.guest_flip) {
        return;
    }
    if ((pending.flags & KZ3_XESS_CAMERA_CURRENT_VALID) &&
        pending.guest_flip == guest_flip) {
        if (pending.view_id != view_id) {
            pending.flags |= KZ3_XESS_CAMERA_MULTIPLE_VIEWS |
                             KZ3_XESS_CAMERA_RESET;
            publish_camera_locked(true);
            return;
        }
        memcpy(
            pending.current_pose,
            pose,
            sizeof(pending.current_pose));
        pending.current_horizontal_fov = horizontal_fov;
        pending.current_vertical_fov = vertical_fov;
        publish_camera_locked(false);
        return;
    }

    kz3_xess_camera_state next{};
    next.guest_flip = guest_flip;
    next.view_id = view_id;
    memcpy(next.current_pose, pose, sizeof(next.current_pose));
    next.current_horizontal_fov = horizontal_fov;
    next.current_vertical_fov = vertical_fov;
    next.flags =
        KZ3_XESS_CAMERA_CURRENT_VALID |
        KZ3_XESS_CAMERA_RESET;

    const kz3_xess_camera_state& state = g_camera.state;
    if ((state.flags & KZ3_XESS_CAMERA_CURRENT_VALID) &&
        state.guest_flip + 1u == guest_flip &&
        state.view_id == view_id &&
        !(state.flags & KZ3_XESS_CAMERA_MULTIPLE_VIEWS)) {
        memcpy(
            next.previous_pose,
            state.current_pose,
            sizeof(next.previous_pose));
        next.previous_horizontal_fov =
            state.current_horizontal_fov;
        next.previous_vertical_fov =
            state.current_vertical_fov;
        next.flags |= KZ3_XESS_CAMERA_PREVIOUS_VALID;
        if (!camera_cut(next.previous_pose, next.current_pose)) {
            next.flags &= ~KZ3_XESS_CAMERA_RESET;
        }
    }

    if (g_camera.plane_flip == guest_flip &&
        !g_camera.planes_ambiguous) {
        next.near_plane = g_camera.near_plane;
        next.far_plane = g_camera.far_plane;
        next.flags |= KZ3_XESS_CAMERA_PLANES_VALID;
    }
    pending = next;
    publish_camera_locked(false);
}

extern "C" void kz3_xess_note_camera_planes(
    uint64_t guest_flip,
    float near_plane,
    float far_plane)
{
    if (!std::isfinite(near_plane) ||
        !std::isfinite(far_plane) ||
        near_plane < 0.001f || near_plane > 10.0f ||
        far_plane <= 10.0f || far_plane <= near_plane) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_camera.mutex);
    if (g_camera.plane_flip != UINT64_MAX &&
        guest_flip < g_camera.plane_flip) {
        return;
    }
    if (g_camera.plane_flip != guest_flip) {
        g_camera.plane_flip = guest_flip;
        g_camera.near_plane = near_plane;
        g_camera.far_plane = far_plane;
        g_camera.planes_ambiguous = false;
    } else if (!projection_plane_matches(
                   g_camera.near_plane, near_plane) ||
               !projection_plane_matches(
                   g_camera.far_plane, far_plane)) {
        g_camera.planes_ambiguous = true;
    }
    if ((g_camera.pending.flags &
         KZ3_XESS_CAMERA_CURRENT_VALID) &&
        g_camera.pending.guest_flip == guest_flip) {
        if (g_camera.planes_ambiguous) {
            g_camera.pending.flags &=
                ~KZ3_XESS_CAMERA_PLANES_VALID;
        } else {
            g_camera.pending.near_plane = g_camera.near_plane;
            g_camera.pending.far_plane = g_camera.far_plane;
            g_camera.pending.flags |=
                KZ3_XESS_CAMERA_PLANES_VALID;
        }
        publish_camera_locked(g_camera.planes_ambiguous);
    }
}

extern "C" int kz3_xess_get_camera(
    kz3_xess_camera_state* out_state)
{
    if (!out_state) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_camera.mutex);
    *out_state = g_camera.state;
    return
        (out_state->flags & KZ3_XESS_CAMERA_CURRENT_VALID) != 0;
}

extern "C" int kz3_xess_capture_opaque_color(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    return capture_color_locked(
        command_list, color, width, height, true);
}

extern "C" int kz3_xess_capture_pre_upscale_color(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    uint32_t width,
    uint32_t height)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    return capture_color_locked(
        command_list, color, width, height, false);
}

extern "C" ID3D12Resource* kz3_xess_dispatch_camera(
    const kz3_xess_camera_dispatch_desc* desc)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    if (!desc || !desc->command_list || !desc->color ||
        !desc->depth || !g_xess.context ||
        !g_xess.motion_vectors || !g_xess.output ||
        !g_xess.motion_heap || !g_xess.motion_root_signature ||
        !g_xess.motion_pipeline) {
        set_status(
            "XeSS-SR camera dispatch received invalid state or resources");
        return nullptr;
    }
    const uint32_t required_camera =
        KZ3_XESS_CAMERA_CURRENT_VALID |
        KZ3_XESS_CAMERA_PLANES_VALID;
    if ((desc->camera.flags & required_camera) != required_camera ||
        (desc->camera.flags & KZ3_XESS_CAMERA_MULTIPLE_VIEWS)) {
        set_status(
            "XeSS-SR camera dispatch has no coherent single-view camera");
        return nullptr;
    }
    if (desc->render_width != g_xess.render_width ||
        desc->render_height != g_xess.render_height ||
        desc->output_width != g_xess.output_width ||
        desc->output_height != g_xess.output_height) {
        set_status(
            "XeSS-SR camera dispatch dimensions do not match its context");
        return nullptr;
    }
    if (!texture_matches(
            desc->color,
            desc->render_width,
            desc->render_height,
            DXGI_FORMAT_R8G8B8A8_UNORM) ||
        !texture_matches(
            desc->depth,
            desc->render_width,
            desc->render_height,
            DXGI_FORMAT_R32_FLOAT)) {
        set_status(
            "XeSS-SR camera dispatch color or depth format is incompatible");
        return nullptr;
    }

    const bool has_previous =
        (desc->camera.flags &
         KZ3_XESS_CAMERA_PREVIOUS_VALID) != 0;
    if (!pose_valid(desc->camera.current_pose) ||
        (has_previous &&
         !pose_valid(desc->camera.previous_pose)) ||
        !std::isfinite(desc->camera.near_plane) ||
        !std::isfinite(desc->camera.far_plane) ||
        !(desc->camera.near_plane > 0.0f) ||
        !(desc->camera.far_plane >
          desc->camera.near_plane)) {
        set_status(
            "XeSS-SR camera dispatch has an invalid pose or projection");
        return nullptr;
    }
    const float* previous_pose = has_previous ?
        desc->camera.previous_pose :
        desc->camera.current_pose;
    const float previous_horizontal_fov = has_previous ?
        desc->camera.previous_horizontal_fov :
        desc->camera.current_horizontal_fov;
    const float previous_vertical_fov = has_previous ?
        desc->camera.previous_vertical_fov :
        desc->camera.current_vertical_fov;
    const float tangents[4] = {
        std::tan(desc->camera.current_horizontal_fov * 0.5f),
        std::tan(desc->camera.current_vertical_fov * 0.5f),
        std::tan(previous_horizontal_fov * 0.5f),
        std::tan(previous_vertical_fov * 0.5f)};
    if (!std::isfinite(tangents[0]) ||
        !std::isfinite(tangents[1]) ||
        !std::isfinite(tangents[2]) ||
        !std::isfinite(tangents[3]) ||
        !(tangents[0] > 0.0f) ||
        !(tangents[1] > 0.0f) ||
        !(tangents[2] > 0.0f) ||
        !(tangents[3] > 0.0f)) {
        set_status(
            "XeSS-SR camera dispatch has an invalid field of view");
        return nullptr;
    }

    D3D12_RESOURCE_BARRIER before[2]{};
    UINT before_count = 0;
    if (g_xess.motion_is_compute_read) {
        D3D12_RESOURCE_BARRIER& barrier = before[before_count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_xess.motion_vectors;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (g_xess.output_is_pixel_read) {
        D3D12_RESOURCE_BARRIER& barrier = before[before_count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_xess.output;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (before_count) {
        desc->command_list->ResourceBarrier(
            before_count, before);
    }
    g_xess.motion_is_compute_read = false;
    g_xess.output_is_pixel_read = false;

    float constants[44]{};
    memcpy(
        constants,
        desc->camera.current_pose,
        3 * 4 * sizeof(float));
    memcpy(
        constants + 12,
        previous_pose,
        3 * 4 * sizeof(float));
    memcpy(
        constants + 24,
        desc->camera.current_pose + 12,
        3 * sizeof(float));
    memcpy(
        constants + 28,
        previous_pose + 12,
        3 * sizeof(float));
    memcpy(constants + 32, tangents, sizeof(tangents));
    constants[36] =
        static_cast<float>(desc->render_width);
    constants[37] =
        static_cast<float>(desc->render_height);
    constants[38] = desc->jitter_x;
    constants[39] = desc->jitter_y;
    constants[40] = desc->camera.near_plane;
    constants[41] = desc->camera.far_plane;

    D3D12_SHADER_RESOURCE_VIEW_DESC depth_srv{};
    depth_srv.Format = DXGI_FORMAT_R32_FLOAT;
    depth_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depth_srv.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depth_srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE depth_handle =
        g_xess.motion_heap->GetCPUDescriptorHandleForHeapStart();
    depth_handle.ptr +=
        g_xess.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_xess.device->CreateShaderResourceView(
        desc->depth, &depth_srv, depth_handle);

    ID3D12DescriptorHeap* heaps[] = {g_xess.motion_heap};
    desc->command_list->SetDescriptorHeaps(1, heaps);
    desc->command_list->SetPipelineState(g_xess.motion_pipeline);
    desc->command_list->SetComputeRootSignature(
        g_xess.motion_root_signature);
    desc->command_list->SetComputeRootDescriptorTable(
        0,
        g_xess.motion_heap->GetGPUDescriptorHandleForHeapStart());
    desc->command_list->SetComputeRoot32BitConstants(
        1, 44, constants, 0);
    desc->command_list->Dispatch(
        (desc->render_width + 7u) / 8u,
        (desc->render_height + 7u) / 8u,
        1);

    D3D12_RESOURCE_BARRIER motion_ready{};
    motion_ready.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    motion_ready.Transition.pResource = g_xess.motion_vectors;
    motion_ready.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    motion_ready.Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    motion_ready.Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    desc->command_list->ResourceBarrier(1, &motion_ready);
    g_xess.motion_is_compute_read = true;

    if (!generate_responsive_locked(
            desc->command_list,
            desc->render_width,
            desc->render_height,
            desc->use_responsive_mask != 0)) {
        g_xess.opaque_capture_serial = 0;
        g_xess.pre_upscale_capture_serial = 0;
        g_xess.capture_source = nullptr;
        return nullptr;
    }
    g_xess.opaque_capture_serial = 0;
    g_xess.pre_upscale_capture_serial = 0;
    g_xess.capture_source = nullptr;

    kz3_xess_d3d12_execute_params execute{};
    execute.color_texture = desc->color;
    execute.velocity_texture = g_xess.motion_vectors;
    execute.depth_texture = desc->depth;
    execute.responsive_pixel_mask_texture =
        g_xess.responsive_mask;
    execute.output_texture = g_xess.output;
    execute.jitter_offset_x = desc->jitter_x;
    execute.jitter_offset_y = desc->jitter_y;
    execute.exposure_scale = 1.0f;
    execute.reset_history =
        desc->reset ||
        (desc->camera.flags & KZ3_XESS_CAMERA_RESET) ||
        !has_previous;
    execute.input_width = desc->render_width;
    execute.input_height = desc->render_height;
    if (g_xess.dispatch_count < 4u) {
        fprintf(
            stderr,
            "[xess] camera: guest flip %llu, view 0x%llX, flags 0x%X\n",
            static_cast<unsigned long long>(desc->camera.guest_flip),
            static_cast<unsigned long long>(desc->camera.view_id),
            desc->camera.flags);
    }
    const int result = g_xess.functions.execute(
        g_xess.context, desc->command_list, &execute);
    if (result != KZ3_XESS_SUCCESS) {
        set_status(
            "XeSS-SR dispatch failed (%d: %s)",
            result,
            result_name(result));
        return nullptr;
    }
    ++g_xess.dispatch_count;
    if (execute.reset_history) {
        ++g_xess.history_reset_count;
    }
    if (g_xess.dispatch_count == 120u ||
        g_xess.dispatch_count % 600u == 0u) {
        fprintf(
            stderr,
            "[xess] history: %llu dispatches, %llu resets\n",
            static_cast<unsigned long long>(g_xess.dispatch_count),
            static_cast<unsigned long long>(
                g_xess.history_reset_count));
    }

    D3D12_RESOURCE_BARRIER output_ready{};
    output_ready.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    output_ready.Transition.pResource = g_xess.output;
    output_ready.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    output_ready.Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    output_ready.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    desc->command_list->ResourceBarrier(1, &output_ready);
    g_xess.output_is_pixel_read = true;
    return g_xess.output;
}

extern "C" void kz3_xess_shutdown(void)
{
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    destroy_context_locked();
    if (g_xess.module) {
        FreeLibrary(g_xess.module);
        g_xess.module = nullptr;
        g_xess.functions = {};
    }
    set_status("XeSS-SR is shut down");
}

extern "C" const char* kz3_xess_status(void)
{
    thread_local std::string copy;
    std::lock_guard<std::mutex> lock(g_xess.mutex);
    copy = g_xess.status;
    return copy.c_str();
}
