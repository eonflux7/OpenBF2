// [RENDER] Draw submission, see Submit.h. Not part of retail BF2.
#ifdef BF2_IMAGE_RENDDX9

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "../../RendDX9Abi/Frontier.h"
#include "RendDX9Base.h"
#include "DrawStats.h"
#include "Ra/RaShader.h"
#include "Submit.h"
#include "../Geom/XpLightManager.h"
#include "../Game/Objects/Visual/LightSource.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dice {
namespace hfe {

namespace geom { extern LightManager* g_LightManager; }

namespace dx9rend {

extern void* g_stateManager;
extern void* g_raShaderConstants;

namespace {

bool g_enabled = true;

// The render states a material decides (contract 2), by the semantic of their parameter.
enum StateSlot { kAlphaTest, kAlphaRef, kDepthWrite, kStateSlots };
const char* const kStateSemantics[kStateSlots] = { "RaAlphaTest", "RaAlphaRef", "RaDepthWrite" };
const unsigned int kStateRenderState[kStateSlots] = { 15u, 24u, 14u };   // ALPHATESTENABLE, ALPHAREF,
                                                                         // ZWRITEENABLE
const unsigned int kZEnable = 7u;
const unsigned int kZWriteEnable = 14u;

bool isTextureType(D3DXPARAMETER_TYPE type)
{
    return type >= D3DXPT_TEXTURE && type <= D3DXPT_TEXTURECUBE;
}

// A parameter's value is kept as 32-bit words in its own type (float bits, int or BOOL), laid out
// as D3DX lays it out for SetValue: elements, each rows x columns, row-major; a struct is its
// members' layouts in order.
std::uint32_t floatWord(float f)
{
    std::uint32_t w;
    std::memcpy(&w, &f, sizeof(w));
    return w;
}

float wordFloat(std::uint32_t w)
{
    float f;
    std::memcpy(&f, &w, sizeof(f));
    return f;
}

std::uint32_t wordFromFloat(D3DXPARAMETER_TYPE type, float f)
{
    if (type == D3DXPT_INT)
        return static_cast<std::uint32_t>(static_cast<int>(f));
    if (type == D3DXPT_BOOL)
        return f != 0.0f ? 1u : 0u;
    return floatWord(f);
}

std::uint32_t wordFromInt(D3DXPARAMETER_TYPE type, int i)
{
    if (type == D3DXPT_INT)
        return static_cast<std::uint32_t>(i);
    if (type == D3DXPT_BOOL)
        return i ? 1u : 0u;
    return floatWord(static_cast<float>(i));
}

float wordToFloat(D3DXPARAMETER_TYPE type, std::uint32_t w)
{
    if (type == D3DXPT_INT)
        return static_cast<float>(static_cast<int>(w));
    if (type == D3DXPT_BOOL)
        return w ? 1.0f : 0.0f;
    return wordFloat(w);
}

int wordToInt(D3DXPARAMETER_TYPE type, std::uint32_t w)
{
    if (type == D3DXPT_FLOAT)
        return static_cast<int>(wordFloat(w));
    return static_cast<int>(w);
}

// One place a parameter (or a member of a struct parameter) lives in a shader.
struct Target
{
    bool pixel;
    D3DXREGISTER_SET set;
    unsigned int reg;
    unsigned int regCount;
    D3DXPARAMETER_CLASS cls;
    D3DXPARAMETER_TYPE type;
    unsigned int rows;
    unsigned int cols;
    unsigned int elements;
    unsigned int offset;   // first word in the parameter's value
    // Per register component (per register for BOOL), the word of the parameter's value that
    // feeds it, or -1 for zero; computed once, so a draw only copies.
    std::vector<int> source;
    // Float registers filled from consecutive words starting at `offset`, none zero: the value is
    // uploaded in place, without the gather.
    bool identity;
};

// Fills t.source from the shape: either matrix packing, arrays, vectors and scalars.
void mapTarget(Target& t, unsigned int wordCount)
{
    const unsigned int stride = t.rows * t.cols;
    const unsigned int perElement = t.set == D3DXRS_BOOL ? stride
        : t.cls == D3DXPC_MATRIX_COLUMNS ? t.cols
        : t.cls == D3DXPC_MATRIX_ROWS ? t.rows : 1u;
    const unsigned int regCount = t.regCount > 256 ? 256 : t.regCount;
    const unsigned int width = t.set == D3DXRS_BOOL ? 1u : 4u;
    t.regCount = regCount;
    t.source.assign(width * regCount, -1);
    t.identity = false;
    if (!perElement)
        return;
    unsigned int out = 0;
    for (unsigned int e = 0; e < t.elements && out < regCount; ++e)
    {
        const unsigned int base = t.offset + e * stride;
        for (unsigned int j = 0; j < perElement && out < regCount; ++j, ++out)
        {
            if (t.set == D3DXRS_BOOL)
            {
                if (base + j < wordCount)
                    t.source[out] = static_cast<int>(base + j);
                continue;
            }
            const unsigned int comps = t.cls == D3DXPC_MATRIX_COLUMNS ? t.rows : t.cols;
            for (unsigned int i = 0; i < comps && i < 4; ++i)
            {
                const unsigned int at = base + (t.cls == D3DXPC_MATRIX_COLUMNS ? i * t.cols + j
                                              : t.cls == D3DXPC_MATRIX_ROWS ? j * t.cols + i : i);
                if (at < wordCount)
                    t.source[4 * out + i] = static_cast<int>(at);
            }
        }
    }
    if (t.set != D3DXRS_FLOAT4 || t.type != D3DXPT_FLOAT || t.source.empty())
        return;
    for (size_t k = 0; k < t.source.size(); ++k)
        if (t.source[k] != static_cast<int>(t.offset + k))
            return;
    t.identity = true;
}

struct Binding
{
    D3DXHANDLE handle;
    std::string name;
    D3DXPARAMETER_TYPE type;   // D3DXPT_VOID for a struct
    unsigned int rows;
    unsigned int cols;
    unsigned int elements;
    std::vector<std::uint32_t> words;
    std::vector<Target> targets;
    std::vector<unsigned int> stages;   // samplers a texture parameter feeds
    int stateSlot;
    bool held;                          // a Ra holder sets it
    std::vector<std::uint32_t> defaultWords;
    IDirect3DBaseTexture9* defaultTexture;
    // An effect program's binding reads its value from the effect's value store (`shared`); a
    // store entry holds a reference on its texture and is `dirty` until written to D3DX.
    Binding* shared;
    IDirect3DBaseTexture9* texture;
    bool stored;
    bool dirty;

    Binding() : handle(0), type(D3DXPT_VOID), rows(0), cols(0), elements(1), stateSlot(-1),
                held(false), defaultTexture(0), shared(0), texture(0), stored(false), dirty(false) {}
    ~Binding()
    {
        if (stored && texture)
            texture->Release();
    }
};

std::uint32_t g_registers[4 * 256];

void emitTarget(const Target& t, const std::uint32_t* words, unsigned int)
{
    const unsigned int count = static_cast<unsigned int>(t.source.size());
    if (!count)
        return;
    const int* const source = &t.source[0];
    if (t.identity)
    {
        const float* const data = reinterpret_cast<const float*>(words + t.offset);
        if (t.pixel)
            stateManagerSetPixelShaderConstantF(t.reg, data, t.regCount);
        else
            stateManagerSetVertexShaderConstantF(t.reg, data, t.regCount);
        return;
    }
    if (t.set == D3DXRS_FLOAT4 && t.type == D3DXPT_FLOAT)
    {
        for (unsigned int k = 0; k < count; ++k)
            g_registers[k] = source[k] >= 0 ? words[source[k]] : 0u;
    }
    else if (t.set == D3DXRS_FLOAT4)
    {
        for (unsigned int k = 0; k < count; ++k)
            g_registers[k] = source[k] >= 0 ? floatWord(wordToFloat(t.type, words[source[k]])) : 0u;
    }
    else if (t.set == D3DXRS_INT4)
    {
        for (unsigned int k = 0; k < count; ++k)
            g_registers[k] = source[k] >= 0
                ? static_cast<std::uint32_t>(wordToInt(t.type, words[source[k]])) : 0u;
    }
    else
    {
        for (unsigned int k = 0; k < count; ++k)
            g_registers[k] = source[k] >= 0 && wordToInt(t.type, words[source[k]]) ? 1u : 0u;
    }
    if (t.set == D3DXRS_FLOAT4)
    {
        const float* const data = reinterpret_cast<const float*>(g_registers);
        if (t.pixel)
            stateManagerSetPixelShaderConstantF(t.reg, data, t.regCount);
        else
            stateManagerSetVertexShaderConstantF(t.reg, data, t.regCount);
    }
    else if (t.set == D3DXRS_INT4)
        stateManagerSetShaderConstantI(t.pixel, t.reg, reinterpret_cast<const int*>(g_registers),
                                       t.regCount);
    else
        stateManagerSetShaderConstantB(t.pixel, t.reg, reinterpret_cast<const BOOL*>(g_registers),
                                       t.regCount);
}

void emitBinding(const Binding& b, std::uint32_t* stateValues)
{
    const std::vector<std::uint32_t>& value = b.shared ? b.shared->words : b.words;
    const unsigned int wordCount = static_cast<unsigned int>(value.size());
    const std::uint32_t* const words = wordCount ? &value[0] : 0;
    for (size_t i = 0; i < b.targets.size(); ++i)
        emitTarget(b.targets[i], words, wordCount);
    if (b.stateSlot >= 0 && wordCount)
        stateValues[b.stateSlot] = static_cast<std::uint32_t>(wordToInt(b.type, words[0]));
}

void bindTexture(const Binding& b, IDirect3DBaseTexture9* texture)
{
    for (size_t i = 0; i < b.stages.size(); ++i)
        stateManagerSetTexture(b.stages[i], texture);
}

// A bound holder's place: its block in the entry, its registry index, and the binding it feeds.
struct UsedParameter
{
    int registryOffset;
    Binding* binding;
    D3DXHANDLE handle;
};

} // namespace

struct SubmitProgram
{
    Ra::RaShader* shader;   // null for an effect program (SubmitEffect)
    void* effect;
    int buildSerial;
    unsigned int key;
    bool valid;
    std::string effectName;
    std::string techniqueName;
    unsigned int pass;
    IDirect3DVertexShader9* vs;
    IDirect3DPixelShader9* ps;
    std::vector<std::pair<unsigned int, unsigned int> > renderStates;
    struct SamplerState { unsigned int stage, type, value; };
    std::vector<SamplerState> samplerStates;
    std::vector<std::unique_ptr<Binding> > bindings;
    // [set][block]: set 0 instance, 1 global, 2 template; blocks as RaShader::applyParameterSet.
    std::vector<UsedParameter> used[3][5];
    std::vector<Binding*> unheld;
    std::uint32_t stateValues[kStateSlots];
    bool hasState[kStateSlots];
    // Contract 3: the vertex shader reads the instance stream (an input TEXCOORD8). An instance
    // record is taken from the holders of World and LightMapOffset, which the program's shaders
    // do not read: `recordSources` (block, holder, scratch binding the value lands in).
    bool instanced;
    struct RecordSource
    {
        unsigned int block;
        UsedParameter used;
        std::unique_ptr<Binding> value;
    };
    RecordSource recordSources[2];   // World, LightMapOffset; used.binding null when absent

    SubmitProgram() : shader(0), effect(0), buildSerial(-1), key(0), valid(false), pass(0), vs(0),
                      ps(0), instanced(false)
    {
        for (unsigned int n = 0; n < 2; ++n)
        {
            recordSources[n].block = 0;
            recordSources[n].used.registryOffset = 0;
            recordSources[n].used.binding = 0;
            recordSources[n].used.handle = 0;
        }
        std::memset(stateValues, 0, sizeof(stateValues));
        std::memset(hasState, 0, sizeof(hasState));
    }
    ~SubmitProgram()
    {
        if (vs)
            vs->Release();
        if (ps)
            ps->Release();
        for (size_t i = 0; i < bindings.size(); ++i)
            if (bindings[i]->defaultTexture)
                bindings[i]->defaultTexture->Release();
    }

    Binding* find(D3DXHANDLE handle)
    {
        for (size_t i = 0; i < bindings.size(); ++i)
            if (bindings[i]->handle == handle)
                return bindings[i].get();
        return 0;
    }
};

namespace {

// The holders' sink: every Ra holder applies its value through an ID3DXEffect, and this one turns
// it into register writes for the binding the caller selected, ignoring the handle. `current`
// takes the value; `target`, when set, is the binding of `program` uploaded from it (the same
// binding for a Ra program; an effect program's binding reading the store entry `current`).
// With `mirror` the call also goes to that D3DX effect (a D3DX pass of the effect is active).
class ParameterSink : public ID3DXEffect
{
public:
    Binding* current;
    Binding* target;
    SubmitProgram* program;
    ID3DXEffect* mirror;
    // When set, each value is appended here (a texture as its pointer) instead of uploaded.
    std::vector<std::uint32_t>* capture;

    ParameterSink() : current(0), target(0), program(0), mirror(0), capture(0) {}

    // IUnknown: the sink lives in static storage and is never handed out.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = 0; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE SetValue(D3DXHANDLE h, LPCVOID data, UINT bytes) override
    {
        if (mirror)
            mirror->SetValue(h, data, bytes);
        if (!current)
            return E_FAIL;
        std::vector<std::uint32_t>& w = current->words;
        const size_t count = (std::min<size_t>)(w.size(), bytes / 4u);
        if (data && count)
            std::memcpy(&w[0], data, count * 4u);
        return emit();
    }
    HRESULT STDMETHODCALLTYPE SetBool(D3DXHANDLE h, BOOL b) override
    {
        if (mirror)
            mirror->SetBool(h, b);
        return setInts(reinterpret_cast<const int*>(&b), 1u);
    }
    HRESULT STDMETHODCALLTYPE SetBoolArray(D3DXHANDLE h, const BOOL* b, UINT n) override
    {
        if (mirror)
            mirror->SetBoolArray(h, b, n);
        return setInts(reinterpret_cast<const int*>(b), n);
    }
    HRESULT STDMETHODCALLTYPE SetInt(D3DXHANDLE h, INT i) override
    {
        if (mirror)
            mirror->SetInt(h, i);
        return setInts(&i, 1u);
    }
    HRESULT STDMETHODCALLTYPE SetIntArray(D3DXHANDLE h, const INT* i, UINT n) override
    {
        if (mirror)
            mirror->SetIntArray(h, i, n);
        return setInts(i, n);
    }
    HRESULT STDMETHODCALLTYPE SetFloat(D3DXHANDLE h, FLOAT f) override
    {
        if (mirror)
            mirror->SetFloat(h, f);
        return setFloats(&f, 1u);
    }
    HRESULT STDMETHODCALLTYPE SetFloatArray(D3DXHANDLE h, const FLOAT* f, UINT n) override
    {
        if (mirror)
            mirror->SetFloatArray(h, f, n);
        return setFloats(f, n);
    }
    HRESULT STDMETHODCALLTYPE SetVector(D3DXHANDLE h, const D3DXVECTOR4* v) override
    {
        if (mirror)
            mirror->SetVector(h, v);
        return setVectors(v, 1u);
    }
    HRESULT STDMETHODCALLTYPE SetVectorArray(D3DXHANDLE h, const D3DXVECTOR4* v, UINT n) override
    {
        if (mirror)
            mirror->SetVectorArray(h, v, n);
        return setVectors(v, n);
    }
    HRESULT STDMETHODCALLTYPE SetMatrix(D3DXHANDLE h, const D3DXMATRIX* m) override
    {
        if (mirror)
            mirror->SetMatrix(h, m);
        return setMatrices(&m, 1u, false, false);
    }
    HRESULT STDMETHODCALLTYPE SetMatrixArray(D3DXHANDLE h, const D3DXMATRIX* m, UINT n) override
    {
        if (mirror)
            mirror->SetMatrixArray(h, m, n);
        return setMatrices(reinterpret_cast<const D3DXMATRIX* const*>(m), n, false, true);
    }
    HRESULT STDMETHODCALLTYPE SetMatrixPointerArray(D3DXHANDLE h, const D3DXMATRIX** m, UINT n) override
    {
        if (mirror)
            mirror->SetMatrixPointerArray(h, m, n);
        return setMatrices(m, n, false, false);
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTranspose(D3DXHANDLE h, const D3DXMATRIX* m) override
    {
        if (mirror)
            mirror->SetMatrixTranspose(h, m);
        return setMatrices(&m, 1u, true, false);
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransposeArray(D3DXHANDLE h, const D3DXMATRIX* m, UINT n) override
    {
        if (mirror)
            mirror->SetMatrixTransposeArray(h, m, n);
        return setMatrices(reinterpret_cast<const D3DXMATRIX* const*>(m), n, true, true);
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransposePointerArray(D3DXHANDLE h, const D3DXMATRIX** m, UINT n) override
    {
        if (mirror)
            mirror->SetMatrixTransposePointerArray(h, m, n);
        return setMatrices(m, n, true, false);
    }
    HRESULT STDMETHODCALLTYPE SetTexture(D3DXHANDLE h, IDirect3DBaseTexture9* texture) override
    {
        if (mirror)
            mirror->SetTexture(h, texture);
        if (!current)
            return E_FAIL;
        if (capture)
        {
            const std::uintptr_t bits = reinterpret_cast<std::uintptr_t>(texture);
            capture->push_back(static_cast<std::uint32_t>(bits));
            capture->push_back(static_cast<std::uint32_t>(static_cast<std::uint64_t>(bits) >> 32));
            return S_OK;
        }
        if (current->stored)
        {
            if (texture)
                texture->AddRef();
            if (current->texture)
                current->texture->Release();
            current->texture = texture;
            current->dirty |= !mirror;
        }
        if (target)
            bindTexture(*target, texture);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetArrayRange(D3DXHANDLE h, UINT start, UINT end) override
    {
        if (mirror)
            mirror->SetArrayRange(h, start, end);
        return S_OK;
    }

    // Nothing else is reached by a holder.
#define SUBMIT_SINK_NONE(name, args) \
    HRESULT STDMETHODCALLTYPE name args override { return E_NOTIMPL; }
    SUBMIT_SINK_NONE(GetDesc, (D3DXEFFECT_DESC*))
    SUBMIT_SINK_NONE(GetParameterDesc, (D3DXHANDLE, D3DXPARAMETER_DESC*))
    SUBMIT_SINK_NONE(GetTechniqueDesc, (D3DXHANDLE, D3DXTECHNIQUE_DESC*))
    SUBMIT_SINK_NONE(GetPassDesc, (D3DXHANDLE, D3DXPASS_DESC*))
    SUBMIT_SINK_NONE(GetFunctionDesc, (D3DXHANDLE, D3DXFUNCTION_DESC*))
    SUBMIT_SINK_NONE(GetValue, (D3DXHANDLE, LPVOID, UINT))
    SUBMIT_SINK_NONE(GetBool, (D3DXHANDLE, BOOL*))
    SUBMIT_SINK_NONE(GetBoolArray, (D3DXHANDLE, BOOL*, UINT))
    SUBMIT_SINK_NONE(GetInt, (D3DXHANDLE, INT*))
    SUBMIT_SINK_NONE(GetIntArray, (D3DXHANDLE, INT*, UINT))
    SUBMIT_SINK_NONE(GetFloat, (D3DXHANDLE, FLOAT*))
    SUBMIT_SINK_NONE(GetFloatArray, (D3DXHANDLE, FLOAT*, UINT))
    SUBMIT_SINK_NONE(GetVector, (D3DXHANDLE, D3DXVECTOR4*))
    SUBMIT_SINK_NONE(GetVectorArray, (D3DXHANDLE, D3DXVECTOR4*, UINT))
    SUBMIT_SINK_NONE(GetMatrix, (D3DXHANDLE, D3DXMATRIX*))
    SUBMIT_SINK_NONE(GetMatrixArray, (D3DXHANDLE, D3DXMATRIX*, UINT))
    SUBMIT_SINK_NONE(GetMatrixPointerArray, (D3DXHANDLE, D3DXMATRIX**, UINT))
    SUBMIT_SINK_NONE(GetMatrixTranspose, (D3DXHANDLE, D3DXMATRIX*))
    SUBMIT_SINK_NONE(GetMatrixTransposeArray, (D3DXHANDLE, D3DXMATRIX*, UINT))
    SUBMIT_SINK_NONE(GetMatrixTransposePointerArray, (D3DXHANDLE, D3DXMATRIX**, UINT))
    SUBMIT_SINK_NONE(SetString, (D3DXHANDLE, LPCSTR))
    SUBMIT_SINK_NONE(GetString, (D3DXHANDLE, LPCSTR*))
    SUBMIT_SINK_NONE(GetTexture, (D3DXHANDLE, IDirect3DBaseTexture9**))
    SUBMIT_SINK_NONE(GetPixelShader, (D3DXHANDLE, IDirect3DPixelShader9**))
    SUBMIT_SINK_NONE(GetVertexShader, (D3DXHANDLE, IDirect3DVertexShader9**))
    SUBMIT_SINK_NONE(GetPool, (ID3DXEffectPool**))
    SUBMIT_SINK_NONE(SetTechnique, (D3DXHANDLE))
    SUBMIT_SINK_NONE(ValidateTechnique, (D3DXHANDLE))
    SUBMIT_SINK_NONE(FindNextValidTechnique, (D3DXHANDLE, D3DXHANDLE*))
    SUBMIT_SINK_NONE(Begin, (UINT*, DWORD))
    SUBMIT_SINK_NONE(BeginPass, (UINT))
    SUBMIT_SINK_NONE(CommitChanges, ())
    SUBMIT_SINK_NONE(EndPass, ())
    SUBMIT_SINK_NONE(End, ())
    SUBMIT_SINK_NONE(GetDevice, (IDirect3DDevice9**))
    SUBMIT_SINK_NONE(OnLostDevice, ())
    SUBMIT_SINK_NONE(OnResetDevice, ())
    SUBMIT_SINK_NONE(SetStateManager, (ID3DXEffectStateManager*))
#undef SUBMIT_SINK_NONE
#define SUBMIT_SINK_NO_HANDLE(name, args) \
    D3DXHANDLE STDMETHODCALLTYPE name args override { return 0; }
    SUBMIT_SINK_NO_HANDLE(GetParameter, (D3DXHANDLE, UINT))
    SUBMIT_SINK_NO_HANDLE(GetParameterByName, (D3DXHANDLE, LPCSTR))
    SUBMIT_SINK_NO_HANDLE(GetParameterBySemantic, (D3DXHANDLE, LPCSTR))
    SUBMIT_SINK_NO_HANDLE(GetParameterElement, (D3DXHANDLE, UINT))
    SUBMIT_SINK_NO_HANDLE(GetTechnique, (UINT))
    SUBMIT_SINK_NO_HANDLE(GetTechniqueByName, (LPCSTR))
    SUBMIT_SINK_NO_HANDLE(GetPass, (D3DXHANDLE, UINT))
    SUBMIT_SINK_NO_HANDLE(GetPassByName, (D3DXHANDLE, LPCSTR))
    SUBMIT_SINK_NO_HANDLE(GetFunction, (UINT))
    SUBMIT_SINK_NO_HANDLE(GetFunctionByName, (LPCSTR))
    SUBMIT_SINK_NO_HANDLE(GetAnnotation, (D3DXHANDLE, UINT))
    SUBMIT_SINK_NO_HANDLE(GetAnnotationByName, (D3DXHANDLE, LPCSTR))
    SUBMIT_SINK_NO_HANDLE(GetCurrentTechnique, ())
#undef SUBMIT_SINK_NO_HANDLE
    BOOL STDMETHODCALLTYPE IsParameterUsed(D3DXHANDLE, D3DXHANDLE) override { return FALSE; }

private:
    HRESULT emit()
    {
        if (capture)
        {
            capture->insert(capture->end(), current->words.begin(), current->words.end());
            return S_OK;
        }
        if (current->stored)
            current->dirty |= !mirror;
        if (target)
            emitBinding(*target, program->stateValues);
        return S_OK;
    }
    HRESULT setInts(const int* values, unsigned int n)
    {
        if (!current)
            return E_FAIL;
        std::vector<std::uint32_t>& w = current->words;
        for (unsigned int i = 0; i < n && i < w.size(); ++i)
            w[i] = wordFromInt(current->type, values[i]);
        return emit();
    }
    HRESULT setFloats(const float* values, unsigned int n)
    {
        if (!current)
            return E_FAIL;
        std::vector<std::uint32_t>& w = current->words;
        if (current->type == D3DXPT_FLOAT)
        {
            const size_t count = (std::min<size_t>)(n, w.size());
            if (count)
                std::memcpy(&w[0], values, count * 4u);
            return emit();
        }
        for (unsigned int i = 0; i < n && i < w.size(); ++i)
            w[i] = wordFromFloat(current->type, values[i]);
        return emit();
    }
    // Each vector fills the first rows x columns (at most four) words of one element.
    HRESULT setVectors(const D3DXVECTOR4* v, unsigned int n)
    {
        if (!current)
            return E_FAIL;
        std::vector<std::uint32_t>& w = current->words;
        const unsigned int stride = (std::max)(1u, current->rows * current->cols);
        const unsigned int take = (std::min)(4u, stride);
        if (current->type == D3DXPT_FLOAT)
        {
            for (unsigned int e = 0; e < n && e * stride < w.size(); ++e)
                std::memcpy(&w[e * stride], &v[e].x,
                            (std::min<size_t>)(take, w.size() - e * stride) * 4u);
            return emit();
        }
        for (unsigned int e = 0; e < n; ++e)
        {
            const float* const f = &v[e].x;
            for (unsigned int i = 0; i < take && e * stride + i < w.size(); ++i)
                w[e * stride + i] = wordFromFloat(current->type, f[i]);
        }
        return emit();
    }
    // `contiguous`: m points at n matrices; otherwise at n matrix pointers.
    HRESULT setMatrices(const D3DXMATRIX* const* m, unsigned int n, bool transpose,
                        bool contiguous)
    {
        if (!current)
            return E_FAIL;
        std::vector<std::uint32_t>& w = current->words;
        const unsigned int rows = (std::min)(4u, current->rows);
        const unsigned int cols = (std::min)(4u, current->cols);
        const unsigned int stride = current->rows * current->cols;
        // A float matrix four columns wide, not transposed: its rows are the value's rows.
        const bool rowCopy = current->type == D3DXPT_FLOAT && !transpose && current->cols == 4;
        for (unsigned int e = 0; e < n; ++e)
        {
            const D3DXMATRIX* const matrix = contiguous
                ? reinterpret_cast<const D3DXMATRIX*>(m) + e : m[e];
            if (!matrix)
                continue;
            if (rowCopy)
            {
                if (e * stride < w.size())
                    std::memcpy(&w[e * stride], matrix,
                                (std::min<size_t>)(rows * 4u, w.size() - e * stride) * 4u);
                continue;
            }
            for (unsigned int r = 0; r < rows; ++r)
                for (unsigned int c = 0; c < cols; ++c)
                {
                    const unsigned int at = e * stride + r * current->cols + c;
                    if (at < w.size())
                        w[at] = wordFromFloat(current->type,
                                              transpose ? matrix->m[c][r] : matrix->m[r][c]);
                }
        }
        return emit();
    }
};

ParameterSink g_sink;

// Records what one BeginPass sets: the program's shaders and fixed states, and which sampler each
// texture parameter reaches (each texture parameter holds a distinct marker texture meanwhile).
class PassRecorder : public ID3DXEffectStateManager
{
public:
    std::vector<std::pair<unsigned int, unsigned int> > renderStates;
    std::vector<SubmitProgram::SamplerState> samplerStates;
    std::vector<std::pair<unsigned int, IDirect3DBaseTexture9*> > textures;
    IDirect3DVertexShader9* vs;
    IDirect3DPixelShader9* ps;
    std::string unsupported;
    unsigned int passes;
    // The constant registers the pass uploaded: (pixel, register set, first register, count).
    struct Constants { bool pixel; D3DXREGISTER_SET set; unsigned int reg, count; };
    std::vector<Constants> constants;

    PassRecorder() : vs(0), ps(0), passes(0) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = 0; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override
    {
        renderStates.push_back(std::make_pair(static_cast<unsigned int>(state),
                                              static_cast<unsigned int>(value)));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD stage, D3DSAMPLERSTATETYPE type, DWORD value) override
    {
        SubmitProgram::SamplerState s = { stage, static_cast<unsigned int>(type), value };
        samplerStates.push_back(s);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture9* texture) override
    {
        textures.push_back(std::make_pair(static_cast<unsigned int>(stage), texture));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* shader) override
    {
        vs = shader;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* shader) override
    {
        ps = shader;
        return S_OK;
    }
    // The constants D3DX uploads are the parameters' current values; the program sets its own.
    // Only where they go is kept, to check that the program's bindings cover them.
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT r, const FLOAT*, UINT n) override { return at(false, D3DXRS_FLOAT4, r, n); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT r, const INT*, UINT n) override { return at(false, D3DXRS_INT4, r, n); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT r, const BOOL*, UINT n) override { return at(false, D3DXRS_BOOL, r, n); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT r, const FLOAT*, UINT n) override { return at(true, D3DXRS_FLOAT4, r, n); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT r, const INT*, UINT n) override { return at(true, D3DXRS_INT4, r, n); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT r, const BOOL*, UINT n) override { return at(true, D3DXRS_BOOL, r, n); }
    // Fixed-function state has no place in a program.
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE, const D3DMATRIX*) override { return no("SetTransform"); }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9*) override { return no("SetMaterial"); }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD, const D3DLIGHT9*) override { return no("SetLight"); }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD, BOOL) override { return no("LightEnable"); }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) override { return no("SetTextureStageState"); }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(FLOAT) override { return no("SetNPatchMode"); }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD) override { return no("SetFVF"); }

private:
    HRESULT at(bool pixel, D3DXREGISTER_SET set, UINT reg, UINT count)
    {
        Constants c = { pixel, set, reg, count };
        constants.push_back(c);
        return S_OK;
    }
    HRESULT no(const char* what)
    {
        if (unsupported.empty())
            unsupported = what;
        return S_OK;
    }
};

void recordPass(ID3DXEffect* effect, D3DXHANDLE technique, unsigned int pass,
                PassRecorder& recorder)
{
    effect->SetStateManager(&recorder);
    effect->SetTechnique(technique);
    UINT passes = 0;
    if (effect->Begin(&passes, 7u) >= 0)
    {
        recorder.passes = passes;
        if (pass < passes)
        {
            effect->BeginPass(pass);
            effect->EndPass();
        }
        effect->End();
    }
    effect->SetStateManager(static_cast<ID3DXEffectStateManager*>(g_stateManager));
}

// The last value the pass set for render state `state`, or ~0u when it set none.
unsigned int recordedState(const PassRecorder& recorder, unsigned int state)
{
    unsigned int value = ~0u;
    for (size_t i = 0; i < recorder.renderStates.size(); ++i)
        if (recorder.renderStates[i].first == state)
            value = recorder.renderStates[i].second;
    return value;
}

// Marker textures for the texture-to-sampler mapping; managed, so they outlive device resets.
std::vector<IDirect3DTexture9*> g_markers;

IDirect3DTexture9* marker(IDirect3DDevice9* device, size_t index)
{
    while (g_markers.size() <= index)
    {
        IDirect3DTexture9* texture = 0;
        if (device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, 0) < 0)
            return 0;
        g_markers.push_back(texture);
    }
    return g_markers[index];
}

typedef std::map<std::pair<Ra::RaShader*, unsigned int>, std::unique_ptr<SubmitProgram> > ProgramMap;
ProgramMap g_programs;

void addTargets(SubmitProgram& program, Binding& binding, ID3DXConstantTable* table,
                D3DXHANDLE constant, const D3DXCONSTANT_DESC& desc, bool pixel,
                unsigned int offset)
{
    if (desc.Class == D3DXPC_STRUCT)
    {
        const unsigned int elements = (std::max)(1u, desc.Elements);
        const unsigned int structWords = desc.Bytes / 4u / elements;
        for (unsigned int e = 0; e < elements; ++e)
        {
            const D3DXHANDLE element = elements > 1 ? table->GetConstantElement(constant, e)
                                                    : constant;
            unsigned int memberOffset = offset + e * structWords;
            for (unsigned int m = 0; m < desc.StructMembers; ++m)
            {
                const D3DXHANDLE member = table->GetConstant(element, m);
                D3DXCONSTANT_DESC memberDesc;
                UINT count = 1;
                if (!member || table->GetConstantDesc(member, &memberDesc, &count) < 0)
                    continue;
                addTargets(program, binding, table, member, memberDesc, pixel, memberOffset);
                memberOffset += memberDesc.Bytes / 4u;
            }
        }
        return;
    }
    (void)program;
    Target t;
    t.pixel = pixel;
    t.set = desc.RegisterSet;
    t.reg = desc.RegisterIndex;
    t.regCount = desc.RegisterCount;
    t.cls = desc.Class;
    t.type = desc.Type;
    t.rows = (std::max)(1u, desc.Rows);
    t.cols = (std::max)(1u, desc.Columns);
    t.elements = (std::max)(1u, desc.Elements);
    t.offset = offset;
    mapTarget(t, static_cast<unsigned int>(binding.words.size()));
    binding.targets.push_back(t);
}

Binding* bindingFor(SubmitProgram& program, ID3DXEffect* effect, D3DXHANDLE handle)
{
    if (Binding* const existing = program.find(handle))
        return existing;
    D3DXPARAMETER_DESC desc;
    if (effect->GetParameterDesc(handle, &desc) < 0)
        return 0;
    std::unique_ptr<Binding> b(new Binding);
    b->handle = handle;
    b->name = desc.Name ? desc.Name : "";
    b->type = desc.Class == D3DXPC_STRUCT ? D3DXPT_VOID : desc.Type;
    b->rows = desc.Rows;
    b->cols = desc.Columns;
    b->elements = (std::max)(1u, desc.Elements);
    if (!isTextureType(desc.Type))
    {
        b->words.assign(desc.Bytes / 4u, 0u);
        if (!b->words.empty())
            effect->GetValue(handle, &b->words[0], desc.Bytes);
        b->defaultWords = b->words;
    }
    else
        effect->GetTexture(handle, &b->defaultTexture);
    program.bindings.push_back(std::move(b));
    return program.bindings.back().get();
}

// A shader's bytecode, into `out`; null when it has none.
template <class Shader>
const DWORD* shaderFunction(Shader* shader, std::vector<DWORD>& out)
{
    UINT bytes = 0;
    if (shader->GetFunction(0, &bytes) < 0 || bytes < sizeof(DWORD))
        return 0;
    out.assign(bytes / sizeof(DWORD), 0u);
    if (shader->GetFunction(&out[0], &bytes) < 0)
        return 0;
    return &out[0];
}

// Whether vertex shader bytecode declares the input TEXCOORD8 (contract 3: the instance stream).
// Walks the instruction stream for dcl (opcode 31): its usage token (usage in bits 0-4, index in
// bits 16-19), then its destination, an input register (type 1).
bool readsInstanceStream(const DWORD* function, size_t words)
{
    if (!function || words < 2)
        return false;
    size_t i = 1;   // past the version token
    while (i < words)
    {
        const DWORD token = function[i];
        if (token == 0x0000FFFFu)   // end
            break;
        const DWORD opcode = token & 0xFFFFu;
        if (opcode == 0xFFFEu)      // comment: its length in bits 16-30
        {
            i += 1 + ((token >> 16) & 0x7FFFu);
            continue;
        }
        const size_t length = (token >> 24) & 0xFu;   // shader model 2 and later
        if (opcode == 31u && length >= 2 && i + 2 < words)
        {
            const DWORD usage = function[i + 1];
            const DWORD dest = function[i + 2];
            const DWORD regType = ((dest >> 28) & 0x7u) | ((dest >> 8) & 0x18u);
            if (regType == 1u && (usage & 0x1Fu) == 5u && ((usage >> 16) & 0xFu) == 8u)
                return true;
        }
        i += 1 + length;
    }
    return false;
}

// The parameter each constant of `function` reads gets its targets. A constant with no effect
// parameter of its name is skipped for a Ra program; an effect program refuses it (`missing`).
bool addShaderConstants(SubmitProgram& program, ID3DXEffect* effect, const DWORD* function,
                        bool pixel, std::string* missing)
{
    if (!function)
        return false;
    ID3DXConstantTable* table = 0;
    if (D3DXGetShaderConstantTable(function, &table) < 0 || !table)
        return false;
    D3DXCONSTANTTABLE_DESC tableDesc;
    table->GetDesc(&tableDesc);
    for (unsigned int i = 0; i < tableDesc.Constants; ++i)
    {
        const D3DXHANDLE constant = table->GetConstant(0, i);
        D3DXCONSTANT_DESC desc;
        UINT count = 1;
        if (!constant || table->GetConstantDesc(constant, &desc, &count) < 0)
            continue;
        if (desc.RegisterSet == D3DXRS_SAMPLER)
            continue;                      // reached through the texture parameters
        const D3DXHANDLE parameter = effect->GetParameterByName(0, desc.Name);
        if (!parameter)
        {
            if (missing && missing->empty())
                *missing = desc.Name ? desc.Name : "?";
            continue;
        }
        Binding* const b = bindingFor(program, effect, parameter);
        if (b)
            addTargets(program, *b, table, constant, desc, pixel, 0u);
    }
    table->Release();
    return true;
}

// What a program records from its effect, either kind.
struct ProgramSource
{
    ID3DXEffect* effect;
    D3DXHANDLE technique;
    unsigned int pass;
    IDirect3DDevice9* device;
    bool ra;   // Ra contract: one pass, the material states (contract 2), constants by name
};

void logRefused(const SubmitProgram& program, const char* why)
{
    ::renddx9abi::logf("Submit: %s %s pass %u: not drawable here (%s)", program.effectName.c_str(),
                       program.techniqueName.c_str(), program.pass, why);
}

// Records the pass into `program`: shaders, fixed states, sampler stages, constant targets.
// False (logged) when it cannot be drawn here.
bool recordProgram(SubmitProgram& program, const ProgramSource& source, PassRecorder& recorder)
{
    ID3DXEffect* const effect = source.effect;
    const D3DXHANDLE technique = source.technique;
    D3DXTECHNIQUE_DESC techniqueDesc;
    if (effect->GetTechniqueDesc(technique, &techniqueDesc) >= 0 && techniqueDesc.Name)
        program.techniqueName = techniqueDesc.Name;
    for (unsigned int s = 0; source.ra && s < kStateSlots; ++s)
    {
        const D3DXHANDLE parameter = effect->GetParameterBySemantic(0, kStateSemantics[s]);
        if (!parameter)
            continue;
        Binding* const b = bindingFor(program, effect, parameter);
        if (!b || b->words.empty())
            continue;
        b->stateSlot = static_cast<int>(s);
        program.hasState[s] = true;
    }

    // Every texture parameter of the effect holds a marker while the pass is recorded.
    D3DXEFFECT_DESC effectDesc;
    effect->GetDesc(&effectDesc);
    std::vector<std::pair<D3DXHANDLE, IDirect3DBaseTexture9*> > saved;
    for (unsigned int i = 0; i < effectDesc.Parameters; ++i)
    {
        const D3DXHANDLE parameter = effect->GetParameter(0, i);
        D3DXPARAMETER_DESC desc;
        if (!parameter || effect->GetParameterDesc(parameter, &desc) < 0
            || !isTextureType(desc.Type) || desc.Elements > 1)
            continue;
        IDirect3DTexture9* const mark = marker(source.device, saved.size());
        if (!mark)
            break;
        IDirect3DBaseTexture9* previous = 0;
        effect->GetTexture(parameter, &previous);
        saved.push_back(std::make_pair(parameter, previous));
        effect->SetTexture(parameter, mark);
    }

    recordPass(effect, technique, source.pass, recorder);

    // A material state is per draw only where the pass's state follows its parameter: recorded
    // again with the parameter changed, the state must change with it. Otherwise the pass's own
    // value stands (leaves fix AlphaTestEnable TRUE while every effect declares AlphaTest).
    for (unsigned int s = 0; s < kStateSlots; ++s)
    {
        if (!program.hasState[s])
            continue;
        Binding* b = 0;
        for (size_t i = 0; i < program.bindings.size() && !b; ++i)
            if (program.bindings[i]->stateSlot == static_cast<int>(s))
                b = program.bindings[i].get();
        const std::uint32_t original = b->words[0];
        const std::uint32_t changed = b->type == D3DXPT_BOOL ? (original ? 0u : 1u)
            : b->type == D3DXPT_INT ? original + 1u : floatWord(wordFloat(original) + 1.0f);
        effect->SetValue(b->handle, &changed, 4u);
        PassRecorder probe;
        recordPass(effect, technique, source.pass, probe);
        effect->SetValue(b->handle, &original, 4u);
        if (recordedState(recorder, kStateRenderState[s]) == recordedState(probe, kStateRenderState[s]))
        {
            program.hasState[s] = false;
            b->stateSlot = -1;
        }
    }

    for (size_t i = 0; i < saved.size(); ++i)
    {
        effect->SetTexture(saved[i].first, saved[i].second);
        if (saved[i].second)
            saved[i].second->Release();
    }
    for (size_t i = 0; i < recorder.textures.size(); ++i)
    {
        for (size_t m = 0; m < saved.size(); ++m)
        {
            if (recorder.textures[i].second != g_markers[m])
                continue;
            Binding* const b = bindingFor(program, effect, saved[m].first);
            if (b)
                b->stages.push_back(recorder.textures[i].first);
        }
    }

    char why[160];
    if (source.ra ? recorder.passes != 1u : source.pass >= recorder.passes)
    {
        std::snprintf(why, sizeof(why), "%u passes", recorder.passes);
        logRefused(program, why);
        return false;
    }
    if (!recorder.unsupported.empty() || !recorder.vs)
    {
        logRefused(program, recorder.vs ? recorder.unsupported.c_str() : "no vertex shader");
        return false;
    }
    program.vs = recorder.vs;
    program.vs->AddRef();
    program.ps = recorder.ps;
    if (program.ps)
        program.ps->AddRef();

    // The constant tables of the shaders the pass bound. The effects' pass descriptions carry no
    // shader functions (GetPassDesc answers null), the shader objects keep theirs.
    std::vector<DWORD> function;
    std::string missing;
    std::string* const report = source.ra ? 0 : &missing;
    if (!addShaderConstants(program, effect, shaderFunction(program.vs, function), false, report)
        || (program.ps
            && !addShaderConstants(program, effect, shaderFunction(program.ps, function), true,
                                   report)))
    {
        logRefused(program, "no constant table");
        return false;
    }
    if (!missing.empty())
    {
        std::snprintf(why, sizeof(why), "constant %s is no parameter", missing.c_str());
        logRefused(program, why);
        return false;
    }
    for (size_t i = 0; i < recorder.renderStates.size(); ++i)
    {
        bool perDraw = false;
        for (unsigned int s = 0; s < kStateSlots; ++s)
            perDraw |= program.hasState[s] && recorder.renderStates[i].first == kStateRenderState[s];
        if (!perDraw)
            program.renderStates.push_back(recorder.renderStates[i]);
    }
    program.samplerStates = recorder.samplerStates;
    return true;
}

void logProgram(const SubmitProgram& program)
{
    if (!drawStatsOn())   // [DEV] BF2_DRAW_STATS: what each program reads, and from where
        return;
    ::renddx9abi::logf("Submit: %s %s pass %u: %u bindings, %u fixed states, %u sampler states",
                       program.effectName.c_str(), program.techniqueName.c_str(), program.pass,
                       static_cast<unsigned int>(program.bindings.size()),
                       static_cast<unsigned int>(program.renderStates.size()),
                       static_cast<unsigned int>(program.samplerStates.size()));
    for (size_t i = 0; i < program.bindings.size(); ++i)
    {
        const Binding& b = *program.bindings[i];
        std::string where;
        char text[96];
        for (size_t t = 0; t < b.targets.size(); ++t)
        {
            const Target& g = b.targets[t];
            std::snprintf(text, sizeof(text), " %s%c%u+%u(c%d %ux%u e%u @%u)",
                          g.pixel ? "ps" : "vs", "bifs"[g.set], g.reg, g.regCount,
                          static_cast<int>(g.cls), g.rows, g.cols, g.elements, g.offset);
            where += text;
        }
        for (size_t t = 0; t < b.stages.size(); ++t)
        {
            std::snprintf(text, sizeof(text), " s%u", b.stages[t]);
            where += text;
        }
        ::renddx9abi::logf("Submit:   %s %s state%d words%u%s", b.name.c_str(),
                           b.shared ? "store" : b.held ? "held" : "effect", b.stateSlot,
                           static_cast<unsigned int>(b.words.size()), where.c_str());
    }
}

void buildProgram(SubmitProgram& program)
{
    Ra::RaShader* const shader = program.shader;
    program.effectName = shader->name();
    const ProgramSource source = { static_cast<ID3DXEffect*>(shader->m_effect),
                                   ::renddx9abi::d3dxHandle(shader->m_activeTechnique), 0u,
                                   shader->m_device, true };
    PassRecorder recorder;
    if (!recordProgram(program, source, recorder))
        return;

    // The Ra holders the program reads; the rest of its parameters keep their effect values.
    std::vector<Ra::RaBoundParameter>* const sets[3] = {
        shader->m_instanceParameters, shader->m_globalParameters, shader->m_templateParameters };
    for (unsigned int set = 0; set < 3; ++set)
        for (unsigned int block = 0; block < 5; ++block)
        {
            const std::vector<Ra::RaBoundParameter>& bound = sets[set][block];
            for (size_t i = 0; i < bound.size(); ++i)
            {
                Binding* const b =
                    program.find(::renddx9abi::d3dxHandle(bound[i].handle));
                if (!b)
                    continue;
                b->held = true;
                UsedParameter used = { bound[i].offset, b,
                                       ::renddx9abi::d3dxHandle(bound[i].handle) };
                program.used[set][block].push_back(used);
            }
        }
    for (size_t i = 0; i < program.bindings.size(); ++i)
        if (!program.bindings[i]->held)
            program.unheld.push_back(program.bindings[i].get());

    // Contract 3: the holders an instance record is taken from.
    std::vector<DWORD> function;
    shaderFunction(program.vs, function);
    program.instanced = readsInstanceStream(function.empty() ? 0 : &function[0], function.size());
    if (program.instanced)
    {
        static const char* const kRecordNames[2] = { "World", "LightMapOffset" };
        for (unsigned int n = 0; n < 2; ++n)
        {
            const D3DXHANDLE handle = source.effect->GetParameterByName(0, kRecordNames[n]);
            D3DXPARAMETER_DESC desc;
            if (!handle || source.effect->GetParameterDesc(handle, &desc) < 0)
                continue;
            for (unsigned int block = 0; block < 5; ++block)
            {
                const std::vector<Ra::RaBoundParameter>& bound = shader->m_instanceParameters[block];
                for (size_t i = 0; i < bound.size(); ++i)
                {
                    if (::renddx9abi::d3dxHandle(bound[i].handle) != handle)
                        continue;
                    SubmitProgram::RecordSource& r = program.recordSources[n];
                    r.value.reset(new Binding);
                    r.value->handle = handle;
                    r.value->name = kRecordNames[n];
                    r.value->type = desc.Type;
                    r.value->rows = desc.Rows;
                    r.value->cols = desc.Columns;
                    r.value->words.assign(desc.Rows * desc.Columns, 0u);
                    r.block = block;
                    const UsedParameter used = { bound[i].offset, r.value.get(), handle };
                    r.used = used;
                }
            }
        }
    }
    program.valid = true;
    logProgram(program);
}

// RaShader::applyParameterVector with the sink in place of the effect, for one holder. Without
// `upload` the value only lands in the binding's words (or the sink's capture).
void applyOne(SubmitProgram* program, void* block, const UsedParameter& used, bool upload)
{
    typedef int (__thiscall* ApplyFn)(void*, void*, void*);
    struct BlockHeader { void* vptr; const std::vector<int>* registry; };
    unsigned char* const bytes = static_cast<unsigned char*>(block);
    const std::vector<int>& offsets = *reinterpret_cast<const BlockHeader*>(bytes)->registry;
    unsigned char* const holder = bytes + offsets[used.registryOffset];
    g_sink.program = program;
    g_sink.current = used.binding;
    g_sink.target = upload ? used.binding : 0;
    reinterpret_cast<ApplyFn>((*reinterpret_cast<void***>(holder))[0])(
        holder, static_cast<ID3DXEffect*>(&g_sink), const_cast<char*>(used.handle));
    g_sink.current = 0;
    g_sink.target = 0;
}

void applyUsed(SubmitProgram* program, void* block, const std::vector<UsedParameter>& used)
{
    for (size_t i = 0; i < used.size(); ++i)
        applyOne(program, block, used[i], true);
}

void applySet(SubmitProgram* program, unsigned int set, RenderQueueEntry* entry)
{
    applyUsed(program, entry->materialParameters, program->used[set][0]);
    applyUsed(program, entry->objectParameters,   program->used[set][1]);
    applyUsed(program, g_raShaderConstants,       program->used[set][2]);
    applyUsed(program, &entry->lightArray,        program->used[set][4]);
    applyUsed(program, &entry->lighting,          program->used[set][3]);
}

} // namespace

bool submitEnabled() { return g_enabled; }
void submitSetEnabled(bool on) { g_enabled = on; }

namespace {

// Uploads `count` floats to the parameter with `semantic`, when the program reads it.
void setSemanticFloats(SubmitProgram* program, const char* semantic, const float* values,
                       unsigned int count)
{
    ID3DXEffect* const effect = static_cast<ID3DXEffect*>(program->effect);
    const D3DXHANDLE handle = effect->GetParameterBySemantic(0, semantic);
    Binding* const b = handle ? program->find(handle) : 0;
    if (!b)
        return;
    g_sink.program = program;
    g_sink.current = b;
    g_sink.target = b;
    g_sink.SetFloatArray(handle, values, count);
    g_sink.current = 0;
    g_sink.target = 0;
}

// Contract 3: the sun in world space for the run, in place of each instance's object-space
// Lights[0] (the static mesh's sun record: the sky light's direction, the static sun colour).
void setSun(SubmitProgram* program)
{
    float direction[3] = { 0.0f, -1.0f, 0.0f };
    float color[3] = { 0.0f, 0.0f, 0.0f };
    if (geom::g_LightManager)
    {
        world::LightSource* const sky = geom::g_LightManager->getSkyLight();
        if (sky)
        {
            const Vec3& d = sky->getDirection();   // the direction the sunlight travels
            direction[0] = d.x;
            direction[1] = d.y;
            direction[2] = d.z;
        }
        const Vec3& c = geom::g_LightManager->getStaticSunColor();
        color[0] = c.x;
        color[1] = c.y;
        color[2] = c.z;
    }
    setSemanticFloats(program, "RaSunDirection", direction, 3);
    setSemanticFloats(program, "RaSunColor", color, 3);
}

} // namespace

SubmitProgram* submitProgramForWalk(Ra::RaShader* shader, unsigned int key, bool orDefault)
{
    SubmitProgram* const program = submitProgram(shader, key, orDefault);
    return program && (g_enabled || program->instanced) ? program : 0;
}

SubmitProgram* submitProgram(Ra::RaShader* shader, unsigned int key, bool orDefault)
{
    if ((!shader->m_built && !shader->slot04()) || !shader->m_effect)
        return 0;
    std::unique_ptr<SubmitProgram>& slot = g_programs[std::make_pair(shader, key)];
    if (slot && slot->effect == shader->m_effect && slot->buildSerial == shader->m_buildSerial)
        return slot->valid ? slot.get() : 0;
    slot.reset(new SubmitProgram);
    slot->shader = shader;
    slot->effect = shader->m_effect;
    slot->buildSerial = shader->m_buildSerial;
    slot->key = key;
    if (key && shader->selectTechnique(key))
        buildProgram(*slot);
    else if ((!key || orDefault) && shader->m_technique)
    {
        shader->m_activeTechnique = shader->m_technique;   // as RaShader::begin
        buildProgram(*slot);
    }
    return slot->valid ? slot.get() : 0;
}

void submitBegin(SubmitProgram* program, RenderQueueEntry* entry)
{
    stateManagerPush();
    stateManagerSetVertexShader(program->vs);
    stateManagerSetPixelShader(program->ps);
    for (size_t i = 0; i < program->renderStates.size(); ++i)
        stateManagerSetRenderState(program->renderStates[i].first, program->renderStates[i].second);
    for (size_t i = 0; i < program->samplerStates.size(); ++i)
        stateManagerSetSamplerState(program->samplerStates[i].stage,
                                    program->samplerStates[i].type,
                                    program->samplerStates[i].value);
    for (size_t i = 0; i < program->unheld.size(); ++i)
    {
        Binding& b = *program->unheld[i];
        if (!b.stages.empty())
            bindTexture(b, b.defaultTexture);
        else
        {
            b.words = b.defaultWords;
            emitBinding(b, program->stateValues);
        }
    }
    applySet(program, 1u, entry);
    if (program->instanced)
        setSun(program);
    if (drawStatsOn())   // [DEV] BF2_DRAW_STATS
        drawStatsBeginPass(program->effectName, program->techniqueName, 0);
}

void submitEnd(SubmitProgram*)
{
    drawStatsEndPass();   // [DEV] BF2_DRAW_STATS
    stateManagerPop();
}

bool submitHasParameter(SubmitProgram* program, const char* semantic)
{
    ID3DXEffect* const effect = static_cast<ID3DXEffect*>(program->effect);
    const D3DXHANDLE handle = effect->GetParameterBySemantic(0, semantic);
    return handle && program->find(handle);
}

bool submitSetMatrix(SubmitProgram* program, const char* semantic, const float* matrix)
{
    ID3DXEffect* const effect = static_cast<ID3DXEffect*>(program->effect);
    const D3DXHANDLE handle = effect->GetParameterBySemantic(0, semantic);
    Binding* const b = handle ? program->find(handle) : 0;
    if (!b)
        return false;
    g_sink.program = program;
    g_sink.current = b;
    g_sink.target = b;
    g_sink.SetMatrix(handle, reinterpret_cast<const D3DXMATRIX*>(matrix));
    g_sink.current = 0;
    g_sink.target = 0;
    return true;
}

bool submitEntry(SubmitProgram* program, RenderQueueEntry* entry)
{
    applySet(program, 2u, entry);
    applySet(program, 0u, entry);

    // The entry's clip planes, as RaShader::applyInstanceParameters.
    std::vector<unsigned char>& planes = entry->lighting.tail0FC;
    const unsigned int planeCount = static_cast<unsigned int>(planes.size()) / 16;
    for (unsigned int index = 0; index < planeCount; ++index)
        program->shader->m_device->SetClipPlane(
            index, reinterpret_cast<const float*>(&planes[16 * index]));

    for (unsigned int s = 0; s < kStateSlots; ++s)
        if (program->hasState[s])
            stateManagerSetRenderState(kStateRenderState[s],
                s == kAlphaRef ? program->stateValues[s] : (program->stateValues[s] ? 1u : 0u));
    return stateManagerWantedRenderState(kZEnable) && stateManagerWantedRenderState(kZWriteEnable);
}

// ---- Instancing (contract 3) ----

namespace {

bool g_instancing = true;

// One record per instance: the world transform's three columns, the lightmap offset.
const unsigned int kRecordFloats = 16;
const unsigned int kRecordBytes = kRecordFloats * 4;
const unsigned int kRecordUsageIndex = 8;    // TEXCOORD8..11
const unsigned int kInstanceBufferRecords = 65536;

IDirect3DVertexBuffer9* g_instanceBuffer = 0;
unsigned int g_instanceBufferNext = 0;   // the next free record

// The declarations a material binds, each with the instance elements added on stream 1. Both the
// material's declaration and the merged one map to the merged one (a run binds it again after its
// first draw). The keys hold a reference, so a freed declaration's address is never met again.
std::map<IDirect3DVertexDeclaration9*, IDirect3DVertexDeclaration9*> g_mergedDeclarations;

// The group being built: its key (the instance parameters its first entry captured) and records.
SubmitProgram* g_groupProgram = 0;
const RenderQueueEntry* g_groupFirst = 0;
std::vector<std::uint32_t> g_groupKey;
std::vector<std::uint32_t> g_candidateKey;
std::vector<float> g_groupRecords;

void* entryBlock(RenderQueueEntry* entry, unsigned int block)
{
    switch (block)
    {
    case 0: return entry->materialParameters;
    case 1: return entry->objectParameters;
    case 2: return g_raShaderConstants;
    case 3: return &entry->lighting;
    default: return &entry->lightArray;
    }
}

// The values the instance parameters of `entry` take in `program`, into `out` (not uploaded).
void captureInstanceKey(SubmitProgram* program, RenderQueueEntry* entry,
                        std::vector<std::uint32_t>& out)
{
    out.clear();
    g_sink.capture = &out;
    for (unsigned int block = 0; block < 5; ++block)
    {
        const std::vector<UsedParameter>& used = program->used[0][block];
        for (size_t i = 0; i < used.size(); ++i)
            applyOne(program, entryBlock(entry, block), used[i], false);
    }
    g_sink.capture = 0;
}

void appendRecord(SubmitProgram* program, RenderQueueEntry* entry)
{
    const size_t at = g_groupRecords.size();
    g_groupRecords.resize(at + kRecordFloats, 0.0f);
    float* const record = &g_groupRecords[at];
    SubmitProgram::RecordSource& world = program->recordSources[0];
    if (world.used.binding)
    {
        applyOne(program, entryBlock(entry, world.block), world.used, false);
        const std::vector<std::uint32_t>& w = world.value->words;
        if (w.size() >= 16)
        {
            // Row-vector world matrix (translation in row 3): column c is m[0..3][c].
            for (unsigned int c = 0; c < 3; ++c)
                for (unsigned int r = 0; r < 4; ++r)
                    record[4 * c + r] = wordFloat(w[4 * r + c]);
        }
    }
    SubmitProgram::RecordSource& lightMap = program->recordSources[1];
    if (lightMap.used.binding)
    {
        applyOne(program, entryBlock(entry, lightMap.block), lightMap.used, false);
        const std::vector<std::uint32_t>& w = lightMap.value->words;
        for (unsigned int k = 0; k < 4 && k < w.size(); ++k)
            record[12 + k] = wordFloat(w[k]);
    }
}

IDirect3DVertexDeclaration9* mergedDeclaration(IDirect3DDevice9* device,
                                               IDirect3DVertexDeclaration9* declaration)
{
    if (!declaration)
        return 0;
    std::map<IDirect3DVertexDeclaration9*, IDirect3DVertexDeclaration9*>::iterator it =
        g_mergedDeclarations.find(declaration);
    if (it != g_mergedDeclarations.end())
        return it->second;
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1];
    UINT count = 0;
    IDirect3DVertexDeclaration9* merged = 0;
    if (declaration->GetDeclaration(0, &count) >= 0 && count >= 1
        && count + 4 <= MAXD3DDECLLENGTH + 1
        && declaration->GetDeclaration(elements, &count) >= 0)
    {
        unsigned int n = count - 1;   // past the last element, before D3DDECL_END
        for (unsigned int k = 0; k < 4; ++k, ++n)
        {
            D3DVERTEXELEMENT9& e = elements[n];
            e.Stream = 1;
            e.Offset = static_cast<WORD>(16 * k);
            e.Type = D3DDECLTYPE_FLOAT4;
            e.Method = D3DDECLMETHOD_DEFAULT;
            e.Usage = D3DDECLUSAGE_TEXCOORD;
            e.UsageIndex = static_cast<BYTE>(kRecordUsageIndex + k);
        }
        const D3DVERTEXELEMENT9 end = D3DDECL_END();
        elements[n] = end;
        if (drawStatsOn())   // [DEV] BF2_DRAW_STATS: the merged declarations
        {
            std::string text;
            for (unsigned int k = 0; k < n; ++k)
            {
                char item[48];
                std::snprintf(item, sizeof(item), " s%u+%u t%u u%u.%u", elements[k].Stream,
                              elements[k].Offset, elements[k].Type, elements[k].Usage,
                              elements[k].UsageIndex);
                text += item;
            }
            ::renddx9abi::logf("Submit: instanced declaration%s", text.c_str());
        }
        if (device->CreateVertexDeclaration(elements, &merged) < 0)
            merged = 0;
    }
    if (!merged)
    {
        ::renddx9abi::logf("Submit: no instanced declaration for %p", declaration);
        return 0;
    }
    declaration->AddRef();
    g_mergedDeclarations[declaration] = merged;
    merged->AddRef();
    g_mergedDeclarations[merged] = merged;
    return merged;
}

bool ensureInstanceBuffer(IDirect3DDevice9* device)
{
    if (g_instanceBuffer)
        return true;
    const HRESULT hr = device->CreateVertexBuffer(kInstanceBufferRecords * kRecordBytes,
                                                  D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                                                  D3DPOOL_DEFAULT, &g_instanceBuffer, 0);
    if (hr < 0)
    {
        ::renddx9abi::logf("Submit: instance buffer not created (0x%08lX)",
                           static_cast<unsigned long>(hr));
        g_instanceBuffer = 0;
        return false;
    }
    g_instanceBufferNext = 0;
    return true;
}

} // namespace

bool submitInstancingEnabled() { return g_instancing; }
void submitSetInstancingEnabled(bool on) { g_instancing = on; }

bool submitInstanced(const SubmitProgram* program)
{
    return program && program->instanced;
}

void submitGroupBegin(SubmitProgram* program, RenderQueueEntry* entry)
{
    g_groupProgram = program;
    g_groupFirst = entry;
    g_groupRecords.clear();
    appendRecord(program, entry);
    if (g_instancing)
        captureInstanceKey(program, entry, g_groupKey);
}

bool submitGroupJoin(RenderQueueEntry* entry)
{
    SubmitProgram* const program = g_groupProgram;
    const RenderQueueEntry* const first = g_groupFirst;
    if (!g_instancing || !program || !first
        || g_groupRecords.size() >= kInstanceBufferRecords * kRecordFloats
        || entry->lodDither[0] != first->lodDither[0] || entry->lodDither[1] != first->lodDither[1]
        || entry->lighting.tail10C || !entry->lighting.tail0FC.empty()
        || first->lighting.tail10C || !first->lighting.tail0FC.empty())
        return false;
    captureInstanceKey(program, entry, g_candidateKey);
    if (g_candidateKey != g_groupKey)
        return false;
    appendRecord(program, entry);
    return true;
}

std::uint64_t submitInstanceKeyHash(SubmitProgram* program, RenderQueueEntry* entry)
{
    // FNV-1a over the instance parameters' values and the LOD dither. An entry with clip planes
    // never joins a group: its address makes its hash its own.
    std::uint64_t h = 14695981039346656037ull;
    const auto mix = [&h](std::uint32_t w) {
        for (unsigned int b = 0; b < 4; ++b)
        {
            h ^= (w >> (8 * b)) & 0xFFu;
            h *= 1099511628211ull;
        }
    };
    if (entry->lighting.tail10C || !entry->lighting.tail0FC.empty())
    {
        const std::uint64_t bits = reinterpret_cast<std::uintptr_t>(entry);
        mix(static_cast<std::uint32_t>(bits));
        mix(static_cast<std::uint32_t>(bits >> 32));
        return h;
    }
    captureInstanceKey(program, entry, g_candidateKey);
    for (size_t i = 0; i < g_candidateKey.size(); ++i)
        mix(g_candidateKey[i]);
    mix(floatWord(entry->lodDither[0]));
    mix(floatWord(entry->lodDither[1]));
    return h;
}

unsigned int submitGroupSize()
{
    return static_cast<unsigned int>(g_groupRecords.size() / kRecordFloats);
}

bool submitGroupBind()
{
    SubmitProgram* const program = g_groupProgram;
    if (!program || g_groupRecords.empty())
        return false;
    IDirect3DDevice9* const device = program->shader->m_device;
    const unsigned int count = submitGroupSize();
    IDirect3DVertexDeclaration9* const merged = mergedDeclaration(
        device, static_cast<IDirect3DVertexDeclaration9*>(stateManagerVertexDeclaration()));
    if (!merged || !ensureInstanceBuffer(device))
        return false;
    DWORD flags = D3DLOCK_NOOVERWRITE;
    if (g_instanceBufferNext + count > kInstanceBufferRecords)
    {
        g_instanceBufferNext = 0;
        flags = D3DLOCK_DISCARD;
    }
    void* data = 0;
    if (g_instanceBuffer->Lock(g_instanceBufferNext * kRecordBytes, count * kRecordBytes, &data,
                               flags) < 0 || !data)
        return false;
    std::memcpy(data, &g_groupRecords[0], count * kRecordBytes);
    g_instanceBuffer->Unlock();
    stateManagerSetVertexDeclaration(merged);
    stateManagerSetStreamSource(1, g_instanceBuffer, g_instanceBufferNext * kRecordBytes,
                                kRecordBytes);
    g_instanceBufferNext += count;
    device->SetStreamSourceFreq(0, D3DSTREAMSOURCE_INDEXEDDATA | count);
    device->SetStreamSourceFreq(1, D3DSTREAMSOURCE_INSTANCEDATA | 1u);
    return true;
}

void submitGroupEnd()
{
    if (g_groupProgram)
    {
        IDirect3DDevice9* const device = g_groupProgram->shader->m_device;
        device->SetStreamSourceFreq(0, 1u);
        device->SetStreamSourceFreq(1, 1u);
    }
    g_groupProgram = 0;
    g_groupFirst = 0;
    g_groupRecords.clear();
}

void submitInstancesRelease()
{
    if (g_instanceBuffer)
        g_instanceBuffer->Release();
    g_instanceBuffer = 0;
    g_instanceBufferNext = 0;
}

void submitReset()
{
    g_programs.clear();
    for (std::map<IDirect3DVertexDeclaration9*, IDirect3DVertexDeclaration9*>::iterator it =
             g_mergedDeclarations.begin(); it != g_mergedDeclarations.end(); ++it)
    {
        it->first->Release();
        if (it->second != it->first)
            it->second->Release();   // the reference CreateVertexDeclaration gave
    }
    g_mergedDeclarations.clear();
}

// ---- Effect programs (SubmitEffect) ----

struct SubmitEffect
{
    ID3DXEffect* fx;
    IDirect3DDevice9* device;
    std::string name;
    // The value store: every parameter written since the effect opted in, by handle (a
    // parameter reached through two handles maps both to one entry, owned once).
    std::map<D3DXHANDLE, Binding*> values;
    std::vector<std::unique_ptr<Binding> > owned;
    std::map<std::pair<D3DXHANDLE, unsigned int>, std::unique_ptr<SubmitProgram> > programs;
    SubmitProgram* active;
    bool d3dxPass;

    SubmitEffect() : fx(0), device(0), active(0), d3dxPass(false) {}
};

namespace {

ParameterSink g_effectSink;

void setEffect(SubmitEffect& e, ID3DXEffect* fx)
{
    e.fx = fx;
    e.device = 0;
    if (fx && fx->GetDevice(&e.device) >= 0 && e.device)
        e.device->Release();   // the effect keeps it
}

// The store entry for `handle`, read from D3DX when first reached; null for a parameter the
// store does not keep (strings, texture arrays, shaders).
Binding* storeValue(SubmitEffect& e, D3DXHANDLE handle)
{
    const std::map<D3DXHANDLE, Binding*>::const_iterator found = e.values.find(handle);
    if (found != e.values.end())
        return found->second;
    D3DXPARAMETER_DESC desc;
    if (!handle || e.fx->GetParameterDesc(handle, &desc) < 0)
        return e.values[handle] = 0;
    const D3DXHANDLE canonical = desc.Name ? e.fx->GetParameterByName(0, desc.Name) : 0;
    if (canonical && canonical != handle)
        return e.values[handle] = storeValue(e, canonical);
    const bool texture = isTextureType(desc.Type) && desc.Elements <= 1;
    const bool numeric = desc.Class == D3DXPC_SCALAR || desc.Class == D3DXPC_VECTOR
        || desc.Class == D3DXPC_MATRIX_ROWS || desc.Class == D3DXPC_MATRIX_COLUMNS
        || desc.Class == D3DXPC_STRUCT;
    if (!texture && (!numeric || !desc.Bytes || desc.Type == D3DXPT_STRING))
        return e.values[handle] = 0;
    std::unique_ptr<Binding> b(new Binding);
    b->handle = handle;
    b->name = desc.Name ? desc.Name : "";
    b->type = desc.Class == D3DXPC_STRUCT ? D3DXPT_VOID : desc.Type;
    b->rows = desc.Rows;
    b->cols = desc.Columns;
    b->elements = (std::max)(1u, desc.Elements);
    b->stored = true;
    if (texture)
        e.fx->GetTexture(handle, &b->texture);   // the reference the entry holds
    else
    {
        b->words.assign(desc.Bytes / 4u, 0u);
        e.fx->GetValue(handle, &b->words[0], desc.Bytes);
    }
    e.owned.push_back(std::move(b));
    return e.values[handle] = e.owned.back().get();
}

bool rangeCovered(const SubmitProgram& program, const PassRecorder::Constants& c)
{
    for (unsigned int r = c.reg; r < c.reg + c.count; ++r)
    {
        bool hit = false;
        for (size_t i = 0; i < program.bindings.size() && !hit; ++i)
        {
            const std::vector<Target>& targets = program.bindings[i]->targets;
            for (size_t t = 0; t < targets.size() && !hit; ++t)
                hit = targets[t].pixel == c.pixel && targets[t].set == c.set
                    && r >= targets[t].reg && r < targets[t].reg + targets[t].regCount;
        }
        if (!hit)
            return false;
    }
    return true;
}

// Whether a fixed state of the pass follows a parameter: recorded again with every numeric
// parameter changed, the states must stay as recorded.
bool statesFollowParameters(ID3DXEffect* effect, const ProgramSource& source,
                            const PassRecorder& recorder)
{
    D3DXEFFECT_DESC effectDesc;
    effect->GetDesc(&effectDesc);
    std::vector<std::pair<D3DXHANDLE, std::vector<std::uint32_t> > > saved;
    for (unsigned int i = 0; i < effectDesc.Parameters; ++i)
    {
        const D3DXHANDLE parameter = effect->GetParameter(0, i);
        D3DXPARAMETER_DESC desc;
        if (!parameter || effect->GetParameterDesc(parameter, &desc) < 0 || !desc.Bytes
            || desc.Bytes > 0x800u || desc.Class == D3DXPC_STRUCT || desc.Class == D3DXPC_OBJECT)
            continue;
        if (desc.Type != D3DXPT_FLOAT && desc.Type != D3DXPT_INT && desc.Type != D3DXPT_BOOL)
            continue;
        std::vector<std::uint32_t> words(desc.Bytes / 4u, 0u);
        effect->GetValue(parameter, &words[0], desc.Bytes);
        std::vector<std::uint32_t> changed(words);
        for (size_t w = 0; w < changed.size(); ++w)
            changed[w] = desc.Type == D3DXPT_BOOL ? (changed[w] ? 0u : 1u)
                : desc.Type == D3DXPT_INT ? changed[w] + 1u : floatWord(wordFloat(changed[w]) + 1.0f);
        effect->SetValue(parameter, &changed[0], desc.Bytes);
        saved.push_back(std::make_pair(parameter, words));
    }
    PassRecorder probe;
    recordPass(effect, source.technique, source.pass, probe);
    for (size_t i = 0; i < saved.size(); ++i)
        effect->SetValue(saved[i].first, &saved[i].second[0],
                         static_cast<UINT>(saved[i].second.size() * 4u));
    if (probe.renderStates != recorder.renderStates
        || probe.samplerStates.size() != recorder.samplerStates.size())
        return true;
    for (size_t i = 0; i < probe.samplerStates.size(); ++i)
        if (probe.samplerStates[i].stage != recorder.samplerStates[i].stage
            || probe.samplerStates[i].type != recorder.samplerStates[i].type
            || probe.samplerStates[i].value != recorder.samplerStates[i].value)
            return true;
    return false;
}

void buildEffectProgram(SubmitEffect& e, SubmitProgram& program, D3DXHANDLE technique)
{
    program.effectName = e.name;
    const ProgramSource source = { e.fx, technique, program.pass, e.device, false };
    PassRecorder recorder;
    if (!e.device || !recordProgram(program, source, recorder))
        return;
    for (size_t i = 0; i < recorder.constants.size(); ++i)
    {
        if (rangeCovered(program, recorder.constants[i]))
            continue;
        char why[96];
        const PassRecorder::Constants& c = recorder.constants[i];
        std::snprintf(why, sizeof(why), "%s %c%u+%u set by the effect, no parameter",
                      c.pixel ? "ps" : "vs", "bifs"[c.set], c.reg, c.count);
        logRefused(program, why);
        return;
    }
    if (statesFollowParameters(e.fx, source, recorder))
    {
        logRefused(program, "a state follows a parameter");
        return;
    }
    for (size_t i = 0; i < program.bindings.size(); ++i)
    {
        Binding& b = *program.bindings[i];
        b.shared = storeValue(e, b.handle);
        if (!b.shared)
        {
            logRefused(program, ("parameter " + b.name + " not storable").c_str());
            return;
        }
        b.handle = b.shared->handle;   // the store's handle, as the writes find it
    }
    program.valid = true;
    logProgram(program);
}

} // namespace

SubmitEffect* submitEffectCreate(void* d3dxEffect, const std::string& name)
{
    SubmitEffect* const e = new SubmitEffect;
    e->name = name;
    setEffect(*e, static_cast<ID3DXEffect*>(d3dxEffect));
    return e;
}

void submitEffectDestroy(SubmitEffect* e)
{
    delete e;
}

void submitEffectFlush(SubmitEffect* e)
{
    if (!e->fx)
        return;
    for (size_t i = 0; i < e->owned.size(); ++i)
    {
        Binding& b = *e->owned[i];
        if (!b.dirty)
            continue;
        b.dirty = false;
        if (b.words.empty())
            e->fx->SetTexture(b.handle, b.texture);
        else
            e->fx->SetValue(b.handle, &b.words[0], static_cast<UINT>(b.words.size() * 4u));
    }
}

void submitEffectRebind(SubmitEffect* e, void* d3dxEffect)
{
    submitEffectFlush(e);
    e->programs.clear();
    e->values.clear();
    e->owned.clear();
    e->active = 0;
    e->d3dxPass = false;
    setEffect(*e, static_cast<ID3DXEffect*>(d3dxEffect));
}

bool submitEffectBeginPass(SubmitEffect* e, void* technique, unsigned int pass)
{
    e->active = 0;
    e->d3dxPass = false;
    if (!submitEnabled() || !e->fx || !technique)
        return false;
    const D3DXHANDLE handle = ::renddx9abi::d3dxHandle(technique);
    std::unique_ptr<SubmitProgram>& slot = e->programs[std::make_pair(handle, pass)];
    if (!slot)
    {
        slot.reset(new SubmitProgram);
        slot->effect = e->fx;
        slot->pass = pass;
        buildEffectProgram(*e, *slot, handle);
    }
    SubmitProgram* const program = slot.get();
    if (!program->valid)
        return false;

    // As D3DX's BeginPass with the effect begun under flags 7: nothing saved, nothing restored.
    stateManagerSetVertexShader(program->vs);
    stateManagerSetPixelShader(program->ps);
    for (size_t i = 0; i < program->renderStates.size(); ++i)
        stateManagerSetRenderState(program->renderStates[i].first, program->renderStates[i].second);
    for (size_t i = 0; i < program->samplerStates.size(); ++i)
        stateManagerSetSamplerState(program->samplerStates[i].stage,
                                    program->samplerStates[i].type,
                                    program->samplerStates[i].value);
    for (size_t i = 0; i < program->bindings.size(); ++i)
    {
        Binding& b = *program->bindings[i];
        if (!b.stages.empty())
            bindTexture(b, b.shared->texture);
        else
            emitBinding(b, program->stateValues);
    }
    e->active = program;
    return true;
}

void submitEffectBeginD3dxPass(SubmitEffect* e)
{
    submitEffectFlush(e);
    e->active = 0;
    e->d3dxPass = true;
}

void submitEffectEndPass(SubmitEffect* e)
{
    e->active = 0;
    e->d3dxPass = false;
}

void* submitEffectParameterTarget(SubmitEffect* e, void* d3dxParameter)
{
    const D3DXHANDLE handle = ::renddx9abi::d3dxHandle(d3dxParameter);
    Binding* const value = e->fx ? storeValue(*e, handle) : 0;
    if (!value)
        return e->fx;
    g_effectSink.current = value;
    g_effectSink.program = e->active;
    g_effectSink.target = e->active ? e->active->find(value->handle) : 0;
    g_effectSink.mirror = e->d3dxPass ? e->fx : 0;
    return static_cast<ID3DXEffect*>(&g_effectSink);
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9
