#include "stdafx.h"
#include "TemporalShaders.h"
#include "TemporalShaderSource.h"
#include "DlssIntegration.h"
#include "../../xrRenderDX10/dx10ConstantBuffer.h"
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace aoe_temporal
{
using Microsoft::WRL::ComPtr;
namespace
{
struct Copy
{
    unsigned source_slot, source_offset, destination_offset, bytes;
    bool instances = false;
};
std::atomic<u64> next_program_identity{1};
struct Program
{
    std::mutex preparation;
    const u64 identity = next_program_identity.fetch_add(1, std::memory_order_relaxed);
    std::string name, target, entry, source;
    std::vector<std::pair<std::string, std::string>> definitions;
    unsigned flags = 0;
    ComPtr<ID3D11DeviceChild> native;
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11Buffer> history_buffer;
    std::vector<Copy> copies;
    unsigned history_size = 0;
    bool camera_motion = false;
    unsigned frame_animation = 0;
    unsigned camera_offset = unsigned(-1), wave_offset = unsigned(-1), wind_offset = unsigned(-1);
    u64 animation_epoch = 0;
    Fvector4 animation_wave{}, animation_wind{};
    std::vector<u8> frame_values;
    unsigned motion_register = unsigned(-1);
    std::map<u64, ComPtr<ID3D11PixelShader>> linked_pixels;
    std::atomic<bool> ready{false};
    bool attempted = false;
    bool original_legacy_output = false;
    bool vertex_resources = false;
    // Alpha-tested cut-outs drawn with colour blending: opaque for history, weighted by alpha at the edges.
    bool cutout = false;
};
std::mutex programs_mutex;
std::unordered_map<ID3D11DeviceChild *, std::shared_ptr<Program>> programs;
std::set<std::pair<ID3D11DeviceChild *, ID3D11DeviceChild *>> passes;
std::atomic<bool> enabled{false};
u64 epoch = 0;
bool scene_geometry = false;
const void *current_visual = nullptr;
const void *current_object = nullptr;
ComPtr<ID3D11Buffer> settings[16];
ComPtr<ID3D11Buffer> raster_settings, camera_history;
struct AnimationState
{
    Fvector4 wave{}, wind{};
    u32 frame = u32(-1);
} animations[2];
Fmatrix packed_camera;
bool current_hud = false;
std::shared_ptr<Program> cached_vertex;
std::shared_ptr<Program> cached_pixel;
u64 linked_vertex_identity = 0, linked_pixel_identity = 0;
ComPtr<ID3D11PixelShader> linked_pixel;
const CTexture *display_position_source = nullptr;
const CTexture *display_heat_source = nullptr;
ID3D11ShaderResourceView *display_position = nullptr;
ID3D11ShaderResourceView *display_heat = nullptr;
ID3D11VertexShader *cached_native_vertex = nullptr;
ID3D11PixelShader *cached_native_pixel = nullptr;
constexpr size_t history_budget = 64u * 1024u * 1024u;
size_t history_bytes = 0;
u32 frame_attempts = 0, frame_draws = 0, frame_cold = 0;
u64 frame_uploaded = 0;
using Key = std::array<uintptr_t, 10>;
struct History
{
    u64 epoch = 0;
    std::vector<u8> previous, current;
    u64 instance_identity = 0;
};
std::map<Key, History> histories;

class Include : public ID3DInclude
{
  public:
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID *data, UINT *bytes) override
    {
        string_path path;
        strconcat(sizeof(path), path, ::Render->getShaderPath(), name);
        IReader *reader = FS.r_open("$game_shaders$", path);
        if (!reader)
            reader = FS.r_open("$game_shaders$", name);
        if (!reader)
            return E_FAIL;
        *bytes = reader->length();
        auto *copy = xr_alloc<u8>(*bytes + 1);
        memcpy(copy, reader->pointer(), *bytes);
        copy[*bytes] = 0;
        FS.r_close(reader);
        *data = copy;
        return S_OK;
    }
    HRESULT __stdcall Close(LPCVOID data) override
    {
        xr_free(data);
        return S_OK;
    }
};

std::string Declaration(ID3D11ShaderReflectionType *type, const std::string &name)
{
    D3D11_SHADER_TYPE_DESC desc{};
    if (FAILED(type->GetDesc(&desc)))
        return {};
    std::string result;
    switch (desc.Type)
    {
    case D3D_SVT_FLOAT:
        result = "float";
        break;
    case D3D_SVT_INT:
        result = "int";
        break;
    case D3D_SVT_UINT:
        result = "uint";
        break;
    case D3D_SVT_BOOL:
        result = "bool";
        break;
    default:
        return {};
    }
    if (desc.Class == D3D_SVC_VECTOR)
        result += std::to_string(desc.Columns);
    else if (desc.Class == D3D_SVC_MATRIX_ROWS || desc.Class == D3D_SVC_MATRIX_COLUMNS)
        result = (desc.Class == D3D_SVC_MATRIX_ROWS ? "row_major " : "column_major ") + result +
                 std::to_string(desc.Rows) + "x" + std::to_string(desc.Columns);
    else if (desc.Class != D3D_SVC_SCALAR)
        return {};
    result += " aoe_previous_" + name;
    if (desc.Elements)
        result += "[" + std::to_string(desc.Elements) + "]";
    return result;
}

std::string PixelPadding(ID3D11ShaderReflection *output, unsigned first, unsigned end)
{
    std::string padding;
    D3D11_SHADER_DESC desc{};
    output->GetDesc(&desc);
    for (unsigned i = 0; i < desc.OutputParameters; ++i)
    {
        D3D11_SIGNATURE_PARAMETER_DESC parameter{};
        output->GetOutputParameterDesc(i, &parameter);
        if (parameter.Register < first || parameter.Register >= end)
            continue;
        unsigned components = 0;
        for (unsigned mask = parameter.Mask; mask; mask >>= 1)
            ++components;
        padding += parameter.ComponentType == D3D_REGISTER_COMPONENT_UINT32   ? "uint"
                   : parameter.ComponentType == D3D_REGISTER_COMPONENT_SINT32 ? "int"
                                                                              : "float";
        padding += std::to_string(components) + " aoe_unused_" + std::to_string(i) + " : " + parameter.SemanticName;
        if (parameter.SemanticIndex)
            padding += std::to_string(parameter.SemanticIndex);
        padding += ", ";
    }
    return padding;
}

struct OriginalVariable
{
    unsigned slot, offset, bytes;
};

bool SameBindings(ID3D11ShaderReflection *original, ID3D11ShaderReflection *adapted)
{
    D3D11_SHADER_DESC desc{};
    if (FAILED(adapted->GetDesc(&desc)))
        return false;
    const auto owned = [](const char *name)
    {
        return !strcmp(name, "AOE_PreviousDraw") || !strcmp(name, "AOE_MotionSettings") ||
               !strcmp(name, "AOE_RasterSettings") || !strcmp(name, "AOE_CameraHistory");
    };
    // The compiler may discard the old material's MV-only calculations.
    // Every original resource still read by the owned variant retains its ABI.
    for (unsigned i = 0; i < desc.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC a{}, b{};
        if (FAILED(adapted->GetResourceBindingDesc(i, &b)))
            return false;
        if (owned(b.Name))
            continue;
        if (FAILED(original->GetResourceBindingDescByName(b.Name, &a)) || a.Type != b.Type ||
            a.BindPoint != b.BindPoint || a.BindCount != b.BindCount)
            return false;
    }
    for (unsigned i = 0; i < desc.ConstantBuffers; ++i)
    {
        auto *b = adapted->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC buffer{};
        if (FAILED(b->GetDesc(&buffer)))
            return false;
        if (owned(buffer.Name))
            continue;
        auto *a = original->GetConstantBufferByName(buffer.Name);
        for (unsigned j = 0; j < buffer.Variables; ++j)
        {
            D3D11_SHADER_VARIABLE_DESC av{}, bv{};
            if (FAILED(b->GetVariableByIndex(j)->GetDesc(&bv)))
                return false;
            if (!(bv.uFlags & D3D_SVF_USED))
                continue;
            if (FAILED(a->GetVariableByName(bv.Name)->GetDesc(&av)) || av.StartOffset != bv.StartOffset ||
                av.Size != bv.Size)
                return false;
        }
    }
    return true;
}

bool Prepare(Program &program)
{
    std::lock_guard<std::mutex> preparation(program.preparation);
    if (program.attempted)
        return program.ready.load(std::memory_order_acquire);
    program.attempted = true;
    PROF_EVENT("Render.Upscaler.PrepareMaterialMotion");
    ComPtr<ID3D11ShaderReflection> original;
    if (FAILED(D3DReflect(program.bytecode->GetBufferPointer(), program.bytecode->GetBufferSize(),
                          IID_PPV_ARGS(&original))))
        return false;
    D3D11_SHADER_DESC desc{};
    original->GetDesc(&desc);
    const bool vertex = program.target[0] == 'v';
    std::map<std::string, OriginalVariable> source_variables;
    std::vector<Variable> variables;
    for (unsigned i = 0; i < desc.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        original->GetResourceBindingDesc(i, &binding);
        if (vertex && binding.Type != D3D_SIT_CBUFFER && binding.Type != D3D_SIT_SAMPLER)
            program.vertex_resources = true;
        if (binding.Type == D3D_SIT_CBUFFER &&
            (binding.BindPoint == (vertex ? 13u : 12u) || (vertex && binding.BindPoint == 12u)))
            return false;
        if (!vertex && binding.Type != D3D_SIT_CBUFFER && binding.Type != D3D_SIT_TEXTURE &&
            binding.Type != D3D_SIT_SAMPLER && binding.Type != D3D_SIT_STRUCTURED &&
            binding.Type != D3D_SIT_BYTEADDRESS)
            return false;
    }
    if (vertex)
    {
        for (unsigned i = 0; i < desc.ConstantBuffers; ++i)
        {
            auto *buffer = original->GetConstantBufferByIndex(i);
            D3D11_SHADER_BUFFER_DESC bd{};
            D3D11_SHADER_INPUT_BIND_DESC binding{};
            buffer->GetDesc(&bd);
            if (FAILED(original->GetResourceBindingDescByName(bd.Name, &binding)))
                return false;
            for (unsigned j = 0; j < bd.Variables; ++j)
            {
                auto *value = buffer->GetVariableByIndex(j);
                D3D11_SHADER_VARIABLE_DESC vd{};
                value->GetDesc(&vd);
                if (!(vd.uFlags & D3D_SVF_USED))
                    continue;
                const std::string declaration = Declaration(value->GetType(), vd.Name);
                if (declaration.empty())
                    return false;
                variables.push_back({vd.Name, declaration});
                source_variables.emplace(vd.Name, OriginalVariable{binding.BindPoint, vd.StartOffset, vd.Size});
            }
        }
        if (variables.empty())
            return false;
    }
    else
        for (unsigned i = 0; i < desc.OutputParameters; ++i)
        {
            D3D11_SIGNATURE_PARAMETER_DESC output{};
            original->GetOutputParameterDesc(i, &output);
            if (output.SystemValueType == D3D_NAME_TARGET && output.SemanticIndex == 3)
                program.original_legacy_output = true;
            if (output.SystemValueType == D3D_NAME_TARGET && output.SemanticIndex >= 4)
                return false;
        }

    std::vector<D3D_SHADER_MACRO> defines;
    for (const auto &definition : program.definitions)
        defines.push_back({definition.first.c_str(), definition.second.c_str()});
    defines.push_back({nullptr, nullptr});
    Include include;
    ComPtr<ID3DBlob> preprocessed, error;
    if (FAILED(D3DPreprocess(program.source.data(), program.source.size(), program.name.c_str(), defines.data(),
                             &include, &preprocessed, &error)))
        return false;
    std::string source(static_cast<const char *>(preprocessed->GetBufferPointer()), preprocessed->GetBufferSize());
    while (!source.empty() && source.back() == '\0')
        source.pop_back();
    if (vertex && !program.vertex_resources &&
        (!program.name.compare(0, 11, "deffer_tree") || !program.name.compare(0, 13, "deffer_detail") ||
         !program.name.compare(0, 7, "details")))
    {
        // Stock wind uses per-frame parameters already provided by trees/details.
        // Other contracts keep the general capture path.
        const std::set<std::string> known{"m_WVP",   "m_WV",      "m_VP",   "m_V",     "m_P",       "m_W",
                                          "m_xform", "m_xform_v", "consts", "c_scale", "c_bias",    "wind",
                                          "wave",    "c_sun",     "dir2D",  "array",   "dt_params", "L_material"};
        bool supported = true;
        for (const auto &variable : variables)
        {
            if (!known.count(variable.name))
                supported = false;
            if ((variable.name == "wave" || variable.name == "wind" || variable.name == "dir2D") &&
                source_variables.at(variable.name).bytes != sizeof(Fvector4))
                supported = false;
        }
        if (supported)
            program.frame_animation = !program.name.compare(0, 11, "deffer_tree") ? 1 : 2;
    }
    std::string adapted;
    if (!(vertex ? VertexSource(source, program.entry, variables, adapted, !program.name.compare(0, 11, "deffer_base"),
                                program.frame_animation != 0)
                 : PixelSource(source, program.entry, adapted)))
        return false;

    // Cache the complete generated source, including the owned wrapper version.
    const u32 hash = crc32(adapted.data(), u32(adapted.size())) ^
                     crc32(program.target.c_str(), u32(program.target.size())) ^ program.flags;
    string_path cache;
    FS.update_path(cache, "$app_data_root$", "shaders_cache\\aoe_temporal\\");
    string64 filename;
    xr_sprintf(filename, "%08x_%c.dxbc", hash, vertex ? 'v' : 'p');
    xr_strcat(cache, filename);
    ComPtr<ID3DBlob> code;
    if (IReader *reader = FS.r_open(cache))
    {
        if (reader->length() > 8)
        {
            const u32 saved = reader->r_u32();
            if (saved == crc32(reader->pointer(), reader->elapsed()) &&
                SUCCEEDED(D3DCreateBlob(reader->elapsed(), &code)))
                memcpy(code->GetBufferPointer(), reader->pointer(), reader->elapsed());
        }
        FS.r_close(reader);
    }
    if (!code)
    {
        error.Reset();
        if (FAILED(D3DCompile(adapted.data(), adapted.size(), program.name.c_str(), nullptr, nullptr,
                              "aoe_temporal_main", program.target.c_str(), program.flags, 0, &code, &error)))
        {
            if (error)
                Msg("! [AOE temporal] %s: %s", program.name.c_str(),
                    static_cast<const char *>(error->GetBufferPointer()));
            return false;
        }
        if (IWriter *writer = FS.w_open(cache))
        {
            writer->w_u32(crc32(code->GetBufferPointer(), u32(code->GetBufferSize())));
            writer->w(code->GetBufferPointer(), u32(code->GetBufferSize()));
            FS.w_close(writer);
        }
    }
    ComPtr<ID3D11ShaderReflection> reflected;
    if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&reflected))) ||
        !SameBindings(original.Get(), reflected.Get()))
        return false;
    D3D11_SHADER_DESC signature{};
    reflected->GetDesc(&signature);
    for (unsigned i = 0; i < (vertex ? signature.OutputParameters : signature.InputParameters); ++i)
    {
        D3D11_SIGNATURE_PARAMETER_DESC parameter{};
        if (vertex)
            reflected->GetOutputParameterDesc(i, &parameter);
        else
            reflected->GetInputParameterDesc(i, &parameter);
        if (SemanticEqual(parameter.SemanticName, "AOE_CURRENT"))
            program.motion_register = parameter.Register;
    }
    if (vertex)
    {
        D3D11_SHADER_DESC adapted_desc{};
        reflected->GetDesc(&adapted_desc);
        if (adapted_desc.InputParameters != desc.InputParameters)
            return false;
        for (unsigned i = 0; i < desc.InputParameters; ++i)
        {
            D3D11_SIGNATURE_PARAMETER_DESC a{}, b{};
            original->GetInputParameterDesc(i, &a);
            reflected->GetInputParameterDesc(i, &b);
            if (_stricmp(a.SemanticName, b.SemanticName) || a.SemanticIndex != b.SemanticIndex ||
                a.ComponentType != b.ComponentType || a.Mask != b.Mask || a.Register != b.Register ||
                a.SystemValueType != b.SystemValueType)
                return false;
        }
        auto *buffer = reflected->GetConstantBufferByName("AOE_PreviousDraw");
        D3D11_SHADER_BUFFER_DESC bd{};
        D3D11_SHADER_INPUT_BIND_DESC camera_binding{};
        program.camera_motion =
            SUCCEEDED(reflected->GetResourceBindingDescByName("AOE_CameraHistory", &camera_binding));
        if (SUCCEEDED(buffer->GetDesc(&bd)))
        {
            if (!bd.Size || bd.Size > 65536)
                return false;
            program.history_size = bd.Size;
            for (unsigned i = 0; i < bd.Variables; ++i)
            {
                D3D11_SHADER_VARIABLE_DESC value{};
                buffer->GetVariableByIndex(i)->GetDesc(&value);
                if (!(value.uFlags & D3D_SVF_USED))
                    continue;
                if (program.frame_animation)
                {
                    if (!strcmp(value.Name, "aoe_camera_reprojection") && value.Size == sizeof(Fmatrix))
                        program.camera_offset = value.StartOffset;
                    else if (!strcmp(value.Name, "aoe_previous_wave"))
                        program.wave_offset = value.StartOffset;
                    else if (!strcmp(value.Name, "aoe_previous_wind") || !strcmp(value.Name, "aoe_previous_dir2D"))
                        program.wind_offset = value.StartOffset;
                    else
                        return false;
                    continue;
                }
                const auto found = source_variables.find(std::string(value.Name).substr(13)); // aoe_previous_
                if (found == source_variables.end() || found->second.bytes != value.Size)
                    return false;
                program.copies.push_back({found->second.slot, found->second.offset, value.StartOffset, value.Size,
                                          found->first == "array" && value.Size % 64 == 0});
            }
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = bd.Size;
            cb.Usage = D3D11_USAGE_DYNAMIC;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (program.frame_animation)
            {
                if (program.camera_offset == unsigned(-1))
                    return false;
                program.frame_values.resize(bd.Size, 0);
            }
            if (FAILED(HW.pDevice->CreateBuffer(&cb, nullptr, &program.history_buffer)))
                return false;
        }
        if (FAILED(HW.pDevice->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                  &program.vertex)))
            return false;
    }
    else if (FAILED(HW.pDevice->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                                  &program.pixel)))
        return false;
    original.Reset();
    reflected.Reset();
    if (vertex)
    {
        program.bytecode = code;
        std::string().swap(program.source);
        std::vector<std::pair<std::string, std::string>>().swap(program.definitions);
    }
    program.ready.store(true, std::memory_order_release);
    return true;
}

bool Candidate(const char *name)
{
    return !strncmp(name, "deffer_", 7) || !strncmp(name, "details", 7) || !strncmp(name, "models_", 7) ||
           !strncmp(name, "model_", 6) || !strncmp(name, "lod", 3) || !strncmp(name, "water", 5) ||
           !strncmp(name, "sky", 3) || !strncmp(name, "clouds", 6) || !strncmp(name, "particle", 8) ||
           !strncmp(name, "effects_", 8) || !strncmp(name, "lmapE", 5) || !strcmp(name, "vert") ||
           // GAMMA self-light scripts draw emissive overlays with the shadow vertex shader.
           !strncmp(name, "shadow_direct_model", 19);
}

void PreparePass(ID3D11DeviceChild *vertex_shader, ID3D11DeviceChild *pixel_shader)
{
    std::shared_ptr<Program> vertex, pixel;
    {
        std::lock_guard<std::mutex> guard(programs_mutex);
        const auto v = programs.find(vertex_shader), p = programs.find(pixel_shader);
        if (v == programs.end() || p == programs.end())
            return;
        vertex = v->second;
        pixel = p->second;
    }
    if (!Prepare(*vertex) || !Prepare(*pixel) || vertex->motion_register == pixel->motion_register)
        return;
    std::lock_guard<std::mutex> guard(pixel->preparation);
    if (pixel->linked_pixels.count(vertex->identity))
        return;
    auto &result = pixel->linked_pixels[vertex->identity];
    if (vertex->motion_register < pixel->motion_register)
        return;
    ComPtr<ID3D11ShaderReflection> output, original;
    if (FAILED(D3DReflect(vertex->bytecode->GetBufferPointer(), vertex->bytecode->GetBufferSize(),
                          IID_PPV_ARGS(&output))) ||
        FAILED(
            D3DReflect(pixel->bytecode->GetBufferPointer(), pixel->bytecode->GetBufferSize(), IID_PPV_ARGS(&original))))
        return;
    const std::string padding = PixelPadding(output.Get(), pixel->motion_register, vertex->motion_register);
    std::vector<D3D_SHADER_MACRO> defines;
    for (const auto &definition : pixel->definitions)
        defines.push_back({definition.first.c_str(), definition.second.c_str()});
    defines.push_back({nullptr, nullptr});
    Include include;
    ComPtr<ID3DBlob> source, error, code;
    if (FAILED(D3DPreprocess(pixel->source.data(), pixel->source.size(), pixel->name.c_str(), defines.data(), &include,
                             &source, &error)))
        return;
    std::string body(static_cast<const char *>(source->GetBufferPointer()), source->GetBufferSize()), adapted;
    while (!body.empty() && !body.back())
        body.pop_back();
    if (!PixelSource(body, pixel->entry, adapted, padding) ||
        FAILED(D3DCompile(adapted.data(), adapted.size(), pixel->name.c_str(), nullptr, nullptr, "aoe_temporal_main",
                          pixel->target.c_str(), pixel->flags, 0, &code, &error)))
        return;
    ComPtr<ID3D11ShaderReflection> input;
    if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&input))) ||
        !SameBindings(original.Get(), input.Get()))
        return;
    D3D11_SHADER_DESC inputs{};
    input->GetDesc(&inputs);
    for (unsigned i = 0; i < inputs.InputParameters; ++i)
    {
        D3D11_SIGNATURE_PARAMETER_DESC parameter{};
        input->GetInputParameterDesc(i, &parameter);
        if (SemanticEqual(parameter.SemanticName, "AOE_CURRENT") && parameter.Register != vertex->motion_register)
            return;
    }
    R_CHK(HW.pDevice->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &result));
}
} // namespace

void Register(const char *name, const char *target, const char *entry, const void *source, unsigned source_size,
              const D3D_SHADER_MACRO *defines, unsigned flags, ID3D11DeviceChild *shader, const void *bytecode,
              size_t bytes)
{
    if (!shader || !Candidate(name) || (target[0] != 'v' && target[0] != 'p'))
        return;
    auto program = std::make_shared<Program>();
    program->name = name;
    program->target = target;
    program->entry = entry;
    program->flags = flags;
    program->cutout = !strncmp(name, "lmapE", 5) || !strcmp(name, "vert");
    program->source.assign(static_cast<const char *>(source), source_size);
    program->native = shader;
    for (; defines && defines->Name; ++defines)
        program->definitions.emplace_back(defines->Name, defines->Definition ? defines->Definition : "");
    if (FAILED(D3DCreateBlob(bytes, &program->bytecode)))
        return;
    memcpy(program->bytecode->GetBufferPointer(), bytecode, bytes);
    {
        std::lock_guard<std::mutex> guard(programs_mutex);
        programs[shader] = program;
    }
    if (enabled && !Prepare(*program))
        Msg("! [AOE temporal] Material history unavailable: %s (%s)", name, target);
}

void Initialize()
{
    enabled = true;
    ResetHistory();
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 16;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    for (auto &setting : settings)
        R_CHK(HW.pDevice->CreateBuffer(&desc, nullptr, setting.ReleaseAndGetAddressOf()));
    R_CHK(HW.pDevice->CreateBuffer(&desc, nullptr, raster_settings.ReleaseAndGetAddressOf()));
    desc.ByteWidth = sizeof(Fmatrix);
    R_CHK(HW.pDevice->CreateBuffer(&desc, nullptr, camera_history.ReleaseAndGetAddressOf()));
    std::vector<std::shared_ptr<Program>> pending;
    {
        std::lock_guard<std::mutex> guard(programs_mutex);
        for (const auto &entry : programs)
            pending.push_back(entry.second);
    }
    unsigned prepared = 0;
    for (const auto &program : pending)
        if (Prepare(*program))
            ++prepared;
        else
            Msg("! [AOE temporal] Material history unavailable: %s (%s)", program->name.c_str(),
                program->target.c_str());
    Msg("- [AOE temporal] Owned material motion: %u/%u programs ready", prepared, u32(pending.size()));
    std::vector<std::pair<ID3D11DeviceChild *, ID3D11DeviceChild *>> pending_passes;
    {
        std::lock_guard<std::mutex> guard(programs_mutex);
        pending_passes.assign(passes.begin(), passes.end());
    }
    for (const auto &pass : pending_passes)
        PreparePass(pass.first, pass.second);
}

void RegisterPass(ID3D11VertexShader *vertex, ID3D11PixelShader *pixel)
{
    if (!vertex || !pixel)
        return;
    {
        std::lock_guard<std::mutex> guard(programs_mutex);
        if (!programs.count(vertex) || !programs.count(pixel))
            return;
        passes.emplace(vertex, pixel);
    }
    if (enabled)
        PreparePass(vertex, pixel);
}

void ResetHistory()
{
    scene_geometry = false;
    histories.clear();
    history_bytes = 0;
    current_visual = current_object = nullptr;
    for (auto &animation : animations)
        animation.frame = u32(-1);
    DisplayInputs(nullptr, nullptr, nullptr, nullptr);
}
void Release()
{
    enabled = false;
    ResetHistory();
    for (auto &setting : settings)
        setting.Reset();
    raster_settings.Reset();
    camera_history.Reset();
    linked_pixel.Reset();
    linked_vertex_identity = linked_pixel_identity = 0;
}
void Shutdown()
{
    Release();
    std::lock_guard<std::mutex> guard(programs_mutex);
    cached_vertex = cached_pixel = nullptr;
    cached_native_vertex = nullptr;
    cached_native_pixel = nullptr;
    programs.clear();
    passes.clear();
    linked_pixel.Reset();
    linked_vertex_identity = linked_pixel_identity = 0;
}

void SetIdentity(const void *visual, const void *object, bool hud)
{
    if (RenderContextId())
        return;
    current_visual = visual;
    current_object = object;
    current_hud = hud;
}

void SetSceneGeometry(bool enabled)
{
    if (!RenderContextId())
        scene_geometry = enabled;
}

void AnimationHistory(bool grass, const Fvector4 &previous_wave, const Fvector4 &previous_wind)
{
    if (!enabled || RenderContextId())
        return;
    auto &animation = animations[grass ? 1 : 0];
    animation.wave = previous_wave;
    animation.wind = previous_wind;
    animation.frame = Device.dwFrame;
}

bool Geometry(ID3D11VertexShader *shader)
{
    if (shader != cached_native_vertex)
    {
        cached_native_vertex = shader;
        std::lock_guard<std::mutex> guard(programs_mutex);
        const auto found = programs.find(shader);
        cached_vertex = found == programs.end() ? nullptr : found->second;
    }
    return cached_vertex != nullptr;
}

void Projection(Fmatrix &matrix, bool previous)
{
    const auto pixel = previous ? dlss::PreviousRasterJitter() : dlss::RasterJitter();
    const auto offset = dlss::ClipJitter(pixel, {dlss::RenderWidth(), dlss::RenderHeight()});
    float *values = &matrix._11;
    for (unsigned row = 0; row < 4; ++row)
    {
        values[row * 4] += offset.x * values[row * 4 + 3];
        values[row * 4 + 1] += offset.y * values[row * 4 + 3];
    }
}

namespace
{
// Row-major D3D layout, row-vector convention, like Fmatrix storage.
using Matrix = std::array<double, 16>;
Matrix FromFloat(const Fmatrix &matrix)
{
    Matrix result;
    for (unsigned i = 0; i < 16; ++i)
        result[i] = (&matrix._11)[i];
    return result;
}
Matrix Multiply(const Matrix &a, const Matrix &b)
{
    Matrix result{};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j)
            for (unsigned k = 0; k < 4; ++k)
                result[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return result;
}
Matrix Identity()
{
    Matrix result{};
    result[0] = result[5] = result[10] = result[15] = 1;
    return result;
}
Matrix Invert(Matrix a)
{
    Matrix inverse = Identity();
    for (unsigned column = 0; column < 4; ++column)
    {
        unsigned pivot = column;
        for (unsigned row = column + 1; row < 4; ++row)
            if (std::abs(a[row * 4 + column]) > std::abs(a[pivot * 4 + column]))
                pivot = row;
        if (a[pivot * 4 + column] == 0)
            return Identity();
        for (unsigned i = 0; i < 4; ++i)
        {
            std::swap(a[column * 4 + i], a[pivot * 4 + i]);
            std::swap(inverse[column * 4 + i], inverse[pivot * 4 + i]);
        }
        const double scale = 1 / a[column * 4 + column];
        for (unsigned i = 0; i < 4; ++i)
        {
            a[column * 4 + i] *= scale;
            inverse[column * 4 + i] *= scale;
        }
        for (unsigned row = 0; row < 4; ++row)
        {
            const double factor = a[row * 4 + column];
            if (row == column || factor == 0)
                continue;
            for (unsigned i = 0; i < 4; ++i)
            {
                a[row * 4 + i] -= factor * a[column * 4 + i];
                inverse[row * 4 + i] -= factor * inverse[column * 4 + i];
            }
        }
    }
    return inverse;
}
Matrix Jitter(dlss::Offset pixels)
{
    const auto clip = dlss::ClipJitter(pixels, {dlss::RenderWidth(), dlss::RenderHeight()});
    Matrix result = Identity();
    result[12] = clip.x;
    result[13] = clip.y;
    return result;
}
} // namespace

void Reprojection(Fmatrix &result, bool sky, bool jittered)
{
    if (!jittered && !memcmp(&Device.mView, &Device.mView_prev, sizeof(Fmatrix)) &&
        !memcmp(&Device.mProject, &Device.mProject_prev, sizeof(Fmatrix)))
    {
        result.identity();
        return;
    }
    // World-space translations reach thousands of metres: float32 inverse products
    // are off by whole pixels, so the camera change is computed in double precision.
    auto view = FromFloat(Device.mView), view_prev = FromFloat(Device.mView_prev);
    auto project = FromFloat(Device.mProject), project_prev = FromFloat(Device.mProject_prev);
    if (sky)
        for (unsigned i = 12; i < 15; ++i)
            view[i] = view_prev[i] = 0;
    if (jittered)
    {
        project = Multiply(project, Jitter(dlss::RasterJitter()));
        project_prev = Multiply(project_prev, Jitter(dlss::PreviousRasterJitter()));
    }
    const auto matrix = Multiply(Invert(Multiply(view, project)), Multiply(view_prev, project_prev));
    for (unsigned i = 0; i < 16; ++i)
        (&result._11)[i] = float(matrix[i]);
}

void SetMatrix(CBackend &backend, R_constant *constant, const Fmatrix &value)
{
    if (dlss::TemporalScene() && backend.RenderPhase(RImplementation.phase) == CRender::PHASE_NORMAL)
    {
        const char *name = constant->name.c_str();
        // Keep native material/light matrices unjittered. Only explicit
        // screen-space history projections include raster offsets.
        const bool previous = !strcmp(name, "m_previous");
        if (previous || !strcmp(name, "m_current"))
        {
            Fmatrix projected = value;
            Projection(projected, previous);
            backend.constants.set(constant, projected);
            return;
        }
    }
    backend.constants.set(constant, value);
}

void BeginFrame()
{
    ++epoch;
    frame_attempts = frame_draws = frame_cold = 0;
    frame_uploaded = 0;
    current_visual = current_object = nullptr;
    current_hud = false;
    scene_geometry = false;
    DisplayInputs(nullptr, nullptr, nullptr, nullptr);
    for (auto it = histories.begin(); it != histories.end();)
        if (it->second.epoch + 2 < epoch)
        {
            history_bytes -= it->second.current.size() + it->second.previous.size();
            it = histories.erase(it);
        }
        else
            ++it;
    if (!enabled || !RImplementation.Target || !RImplementation.Target->rt_aoe_motion)
        return;
    const float empty[4]{};
    HW.pContext->ClearRenderTargetView(RImplementation.Target->rt_aoe_motion->pRT, empty);
    if (!RImplementation.o.ssfx_motionvectors)
        HW.pContext->ClearRenderTargetView(RImplementation.Target->rt_ssfx_motion_vectors->pRT, empty);
    const auto current = dlss::RasterJitter(), previous = dlss::PreviousRasterJitter();
    const auto clip = dlss::ClipJitter(current, {dlss::RenderWidth(), dlss::RenderHeight()});
    D3D11_MAPPED_SUBRESOURCE mapped_frame{};
    if (SUCCEEDED(HW.pContext->Map(raster_settings.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_frame)))
    {
        const Fvector4 value{clip.x, clip.y, 0, 0};
        memcpy(mapped_frame.pData, &value, sizeof(value));
        HW.pContext->Unmap(raster_settings.Get(), 0);
    }
    Fmatrix reprojection;
    Reprojection(reprojection, false, false);
    packed_camera.transpose(reprojection);
    if (SUCCEEDED(HW.pContext->Map(camera_history.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_frame)))
    {
        memcpy(mapped_frame.pData, &packed_camera, sizeof(packed_camera));
        HW.pContext->Unmap(camera_history.Get(), 0);
    }
    for (unsigned i = 0; i < 16; ++i)
    {
        if (!settings[i])
            continue;
        const Fvector4 value{(current.x - previous.x) / dlss::RenderWidth(),
                             (current.y - previous.y) / dlss::RenderHeight(), float(i & 1),
                             float((i >> 1) & 1) + (i & 4 ? 2.f : 0.f) + (i & 8 ? 4.f : 0.f)};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(HW.pContext->Map(settings[i].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            memcpy(mapped.pData, &value, sizeof(value));
            HW.pContext->Unmap(settings[i].Get(), 0);
        }
    }
}

bool Bind(CBackend &backend, unsigned base_vertex, unsigned first, unsigned count, bool indexed)
{
    if (!enabled || !scene_geometry || !dlss::TemporalScene() || RenderContextId())
        return false;
    const auto reject = [](const char *) { return false; };
    if (backend.gs || backend.hs || backend.ds)
        return reject("NO JITTER: geometry/tessellation stage");
    if (backend.RenderPhase(RImplementation.phase) != CRender::PHASE_NORMAL)
        return reject("NO JITTER: not normal phase");
    if (backend.pZB != dlss::SceneDepth())
        return reject("NO JITTER: other depth buffer");
    if (!backend.pRT[0] || !backend.colorwrite_mask)
        return reject("NO JITTER: colour writes off");
    if (!Geometry(backend.vs))
        return reject("NO JITTER: vertex shader not owned");
    ++frame_attempts;
    if (!cached_vertex->ready.load(std::memory_order_acquire))
        return reject("NO JITTER: vertex program not ready");
    if (backend.ps != cached_native_pixel)
    {
        cached_native_pixel = backend.ps;
        std::lock_guard<std::mutex> guard(programs_mutex);
        const auto found = programs.find(backend.ps);
        cached_pixel = found == programs.end() ? nullptr : found->second;
    }
    if (!raster_settings || !settings[0] || !settings[15] || !RImplementation.Target->rt_aoe_motion)
        return reject("NO JITTER: motion resources not ready");
    auto &vertex = *cached_vertex;
    // The pixel shader cannot carry motion output (not an owned material, or no linked variant):
    // still jitter the geometry, as FSR assumes every scene draw is, and leave motion to the resolve pass.
    const auto jitter_only = [&](const char *)
    {
        auto *context = RenderDeviceContext();
        ID3D11Buffer *constants = raster_settings.Get();
        context->VSSetConstantBuffers(12, 1, &constants);
        if (backend.temporal_bound_vertex != vertex.vertex.Get())
        {
            context->VSSetShader(vertex.vertex.Get(), nullptr, 0);
            backend.temporal_bound_vertex = vertex.vertex.Get();
        }
        if (backend.temporal_bound_pixel)
        {
            context->PSSetShader(backend.ps, nullptr, 0);
            backend.temporal_bound_pixel = nullptr;
        }
        backend.set_RT(nullptr, 4);
        StateManager.SetTemporalTarget(false);
        return true;
    };
    if (!cached_pixel || !cached_pixel->ready.load(std::memory_order_acquire))
        return jitter_only("jitter only: pixel shader not owned");
    if (linked_vertex_identity != vertex.identity || linked_pixel_identity != cached_pixel->identity)
    {
        linked_vertex_identity = vertex.identity;
        linked_pixel_identity = cached_pixel->identity;
        linked_pixel = cached_pixel->pixel;
        if (vertex.motion_register != cached_pixel->motion_register)
        {
            std::lock_guard<std::mutex> guard(cached_pixel->preparation);
            const auto found = cached_pixel->linked_pixels.find(vertex.identity);
            linked_pixel = found == cached_pixel->linked_pixels.end() ? nullptr : found->second;
        }
    }
    if (!linked_pixel)
        return jitter_only("jitter only: no linked pixel variant");
    auto *context = RenderDeviceContext();
    // Camera and wind history are valid because their previous state is kept
    // explicitly. FSR already resets its own history on the first frame after
    // a level reset. The general capture path still rejects cold/stale entries
    // below through its per-draw epoch and identity checks.
    bool valid = true;
    const char *cold = "reactive: ";
    const auto invalidate = [&](const char *reason)
    {
        if (valid)
            cold = reason;
        valid = false;
    };
    ID3D11Buffer *constants = nullptr;
    if (vertex.camera_motion)
    {
        if (current_hud)
            invalidate("reactive: HUD");
        else if (vertex.vertex_resources)
            invalidate("reactive: vertex resources");
        constants = camera_history.Get();
    }
    else if (vertex.frame_animation && vertex.history_size)
    {
        const auto &animation = animations[vertex.frame_animation - 1];
        if (animation.frame != Device.dwFrame)
            invalidate("reactive: no animation history this frame");
        else if (current_hud)
            invalidate("reactive: HUD");
        if (vertex.animation_epoch != epoch || memcmp(&vertex.animation_wave, &animation.wave, sizeof(Fvector4)) ||
            memcmp(&vertex.animation_wind, &animation.wind, sizeof(Fvector4)))
        {
            auto *data = vertex.frame_values.data();
            memcpy(data + vertex.camera_offset, &packed_camera, sizeof(packed_camera));
            if (vertex.wave_offset != unsigned(-1))
                memcpy(data + vertex.wave_offset, &animation.wave, sizeof(Fvector4));
            if (vertex.wind_offset != unsigned(-1))
                memcpy(data + vertex.wind_offset, &animation.wind, sizeof(Fvector4));
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(vertex.history_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                return reject("NO JITTER: constant map failed");
            memcpy(mapped.pData, data, vertex.frame_values.size());
            context->Unmap(vertex.history_buffer.Get(), 0);
            vertex.animation_epoch = epoch;
            vertex.animation_wave = animation.wave;
            vertex.animation_wind = animation.wind;
            frame_uploaded += vertex.frame_values.size();
        }
        constants = vertex.history_buffer.Get();
    }
    else if (vertex.history_size)
    {
        static std::vector<u8> current;
        current.assign(vertex.history_size, 0);
        u64 instance_identity = 0;
        for (const auto &copy : vertex.copies)
        {
            if (copy.source_slot >= CBackend::MaxCBuffers || !backend.m_aVertexConstants[copy.source_slot] ||
                !backend.m_aVertexConstants[copy.source_slot]->ReadBytes(
                    copy.source_offset, current.data() + copy.destination_offset, copy.bytes))
                return false;
            if (copy.instances)
            {
                // Four float4 rows per detail instance: identity uses its three
                // transform rows, excluding colour/fade data in the fourth row.
                instance_identity = 1469598103934665603ull;
                for (unsigned offset = 0; offset < copy.bytes; offset += 64)
                    for (unsigned byte = 0; byte < 48; ++byte)
                        instance_identity =
                            (instance_identity ^ current[copy.destination_offset + offset + byte]) * 1099511628211ull;
            }
        }
        const Key key{reinterpret_cast<uintptr_t>(backend.vb),
                      indexed ? reinterpret_cast<uintptr_t>(backend.ib) : 0,
                      uintptr_t(vertex.identity),
                      reinterpret_cast<uintptr_t>(current_visual),
                      reinterpret_cast<uintptr_t>(current_object),
                      base_vertex,
                      first,
                      count,
                      uintptr_t(instance_identity),
                      uintptr_t(cached_pixel->identity)};
        auto found = histories.find(key);
        if (found == histories.end())
        {
            if (history_bytes + current.size() * 2 > history_budget)
                return reject("NO JITTER: history budget exhausted");
            found = histories.emplace(key, History{}).first;
        }
        auto &history = found->second;
        if (!(history.epoch == epoch || history.epoch + 1 == epoch))
            invalidate("reactive: no history from previous frame");
        if (history.epoch == epoch && history.current != current)
            invalidate("reactive: constants changed within frame");
        if (history.epoch != epoch)
        {
            const size_t before = history.previous.size() + history.current.size();
            history.previous.swap(history.current);
            history.current = current;
            history.epoch = epoch;
            history_bytes += history.previous.size() + history.current.size() - before;
        }
        if (history.previous.size() != current.size() || (!vertex.cutout && !StateManager.TemporalHistorySafe()))
            invalidate("reactive: history size/state unsafe");
        // CPU-generated vertex streams do not have stable per-vertex history.
        // Mark them reactive instead of mistaking their changing positions for rigid geometry.
        D3D11_BUFFER_DESC desc{};
        if (backend.vb)
            backend.vb->GetDesc(&desc);
        if (desc.Usage == D3D11_USAGE_DYNAMIC || vertex.vertex_resources)
            invalidate("reactive: dynamic vertex buffer or vertex resources");
        const auto &previous = valid ? history.previous : current;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(vertex.history_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return reject("NO JITTER: constant map failed");
        memcpy(mapped.pData, previous.data(), previous.size());
        context->Unmap(vertex.history_buffer.Get(), 0);
        constants = vertex.history_buffer.Get();
        frame_uploaded += previous.size();
    }
    if (constants)
        context->VSSetConstantBuffers(13, 1, &constants);
    constants = raster_settings.Get();
    context->VSSetConstantBuffers(12, 1, &constants);
    const bool legacy_target = backend.pRT[3] == RImplementation.Target->rt_ssfx_motion_vectors->pRT;
    const bool blended = StateManager.TemporalBlended();
    constants = settings[(legacy_target ? 0 : 4) + (current_hud ? 2 : 0) + (valid ? 1 : 0) + (blended ? 8 : 0)].Get();
    context->PSSetConstantBuffers(12, 1, &constants);
    if (backend.temporal_bound_vertex != vertex.vertex.Get())
    {
        context->VSSetShader(vertex.vertex.Get(), nullptr, 0);
        backend.temporal_bound_vertex = vertex.vertex.Get();
    }
    if (backend.temporal_bound_pixel != linked_pixel.Get())
    {
        context->PSSetShader(linked_pixel.Get(), nullptr, 0);
        backend.temporal_bound_pixel = linked_pixel.Get();
    }
    backend.set_RT(RImplementation.Target->rt_aoe_motion->pRT, 4);
    // Screen-space sky/cloud/combine draws stay jittered, but must not overwrite
    // scene motion. Their pixels use the camera-depth fallback in resolve.
    const bool write_motion = current_visual != nullptr;
    StateManager.SetTemporalTarget(true, !legacy_target && !cached_pixel->original_legacy_output, write_motion);
    ++frame_draws;
    frame_cold += valid ? 0 : 1;
    return true;
}

void Restore(CBackend &backend)
{
    if (backend.temporal_bound_vertex)
    {
        RenderDeviceContext()->VSSetShader(backend.vs, nullptr, 0);
        backend.temporal_bound_vertex = nullptr;
    }
    if (backend.temporal_bound_pixel)
    {
        RenderDeviceContext()->PSSetShader(backend.ps, nullptr, 0);
        backend.temporal_bound_pixel = nullptr;
    }
    backend.set_RT(nullptr, 4);
    StateManager.SetTemporalTarget(false);
}

void DisplayInputs(CTexture *position, ID3D11ShaderResourceView *position_view, CTexture *heat,
                   ID3D11ShaderResourceView *heat_view)
{
    display_position_source = position;
    display_position = position_view;
    display_heat_source = heat;
    display_heat = heat_view;
}
ID3D11ShaderResourceView *TextureView(CTexture *source, ID3D11ShaderResourceView *native)
{
    if (RenderContextId())
        return native;
    if (source && source == display_position_source)
        return display_position;
    if (source && source == display_heat_source)
        return display_heat;
    return native;
}
void Forget(ID3D11DeviceChild *shader)
{
    std::lock_guard<std::mutex> guard(programs_mutex);
    programs.erase(shader);
    for (auto it = passes.begin(); it != passes.end();)
        if (it->first == shader || it->second == shader)
            it = passes.erase(it);
        else
            ++it;
}

void ProfileFrame()
{
    PROF_EVENT("Render.Upscaler.OwnMotionStats");
    PROF_TAG("Motion candidate draws", frame_attempts);
    PROF_TAG("Motion captured draws", frame_draws);
    PROF_TAG("Motion reactive draws", frame_cold);
    PROF_TAG("Motion previous-constant bytes", frame_uploaded);
    PROF_TAG("Motion cached bytes", u64(history_bytes));
}
} // namespace aoe_temporal
