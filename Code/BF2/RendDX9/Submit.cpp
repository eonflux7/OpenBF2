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

    Binding() : handle(0), type(D3DXPT_VOID), rows(0), cols(0), elements(1), stateSlot(-1),
                held(false), defaultTexture(0) {}
};

std::uint32_t g_registers[4 * 256];

void emitTarget(const Target& t, const std::uint32_t* words, unsigned int)
{
    const unsigned int count = static_cast<unsigned int>(t.source.size());
    if (!count)
        return;
    const int* const source = &t.source[0];
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
    const unsigned int wordCount = static_cast<unsigned int>(b.words.size());
    const std::uint32_t* const words = wordCount ? &b.words[0] : 0;
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
    Ra::RaShader* shader;
    void* effect;
    int buildSerial;
    unsigned int key;
    bool valid;
    std::string techniqueName;
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

    SubmitProgram() : shader(0), effect(0), buildSerial(-1), key(0), valid(false), vs(0), ps(0)
    {
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
// it into register writes for the binding the caller selected (`current`), ignoring the handle.
class ParameterSink : public ID3DXEffect
{
public:
    Binding* current;
    SubmitProgram* program;

    ParameterSink() : current(0), program(0) {}

    // IUnknown: the sink lives in static storage and is never handed out.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = 0; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE SetValue(D3DXHANDLE, LPCVOID data, UINT bytes) override
    {
        if (!current)
            return E_FAIL;
        std::vector<std::uint32_t>& w = current->words;
        const size_t count = (std::min<size_t>)(w.size(), bytes / 4u);
        if (data && count)
            std::memcpy(&w[0], data, count * 4u);
        return emit();
    }
    HRESULT STDMETHODCALLTYPE SetBool(D3DXHANDLE, BOOL b) override
    {
        return setInts(reinterpret_cast<const int*>(&b), 1u);
    }
    HRESULT STDMETHODCALLTYPE SetBoolArray(D3DXHANDLE, const BOOL* b, UINT n) override
    {
        return setInts(reinterpret_cast<const int*>(b), n);
    }
    HRESULT STDMETHODCALLTYPE SetInt(D3DXHANDLE, INT i) override { return setInts(&i, 1u); }
    HRESULT STDMETHODCALLTYPE SetIntArray(D3DXHANDLE, const INT* i, UINT n) override
    {
        return setInts(i, n);
    }
    HRESULT STDMETHODCALLTYPE SetFloat(D3DXHANDLE, FLOAT f) override { return setFloats(&f, 1u); }
    HRESULT STDMETHODCALLTYPE SetFloatArray(D3DXHANDLE, const FLOAT* f, UINT n) override
    {
        return setFloats(f, n);
    }
    HRESULT STDMETHODCALLTYPE SetVector(D3DXHANDLE, const D3DXVECTOR4* v) override
    {
        return setVectors(v, 1u);
    }
    HRESULT STDMETHODCALLTYPE SetVectorArray(D3DXHANDLE, const D3DXVECTOR4* v, UINT n) override
    {
        return setVectors(v, n);
    }
    HRESULT STDMETHODCALLTYPE SetMatrix(D3DXHANDLE, const D3DXMATRIX* m) override
    {
        return setMatrices(&m, 1u, false, false);
    }
    HRESULT STDMETHODCALLTYPE SetMatrixArray(D3DXHANDLE, const D3DXMATRIX* m, UINT n) override
    {
        return setMatrices(reinterpret_cast<const D3DXMATRIX* const*>(m), n, false, true);
    }
    HRESULT STDMETHODCALLTYPE SetMatrixPointerArray(D3DXHANDLE, const D3DXMATRIX** m, UINT n) override
    {
        return setMatrices(m, n, false, false);
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTranspose(D3DXHANDLE, const D3DXMATRIX* m) override
    {
        return setMatrices(&m, 1u, true, false);
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransposeArray(D3DXHANDLE, const D3DXMATRIX* m, UINT n) override
    {
        return setMatrices(reinterpret_cast<const D3DXMATRIX* const*>(m), n, true, true);
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransposePointerArray(D3DXHANDLE, const D3DXMATRIX** m, UINT n) override
    {
        return setMatrices(m, n, true, false);
    }
    HRESULT STDMETHODCALLTYPE SetTexture(D3DXHANDLE, IDirect3DBaseTexture9* texture) override
    {
        if (!current)
            return E_FAIL;
        bindTexture(*current, texture);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetArrayRange(D3DXHANDLE, UINT, UINT) override { return S_OK; }

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
        emitBinding(*current, program->stateValues);
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
        for (unsigned int e = 0; e < n; ++e)
        {
            const D3DXMATRIX* const matrix = contiguous
                ? reinterpret_cast<const D3DXMATRIX*>(m) + e : m[e];
            if (!matrix)
                continue;
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

    PassRecorder() : vs(0), ps(0) {}

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
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT, const FLOAT*, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT, const INT*, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT, const BOOL*, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT, const FLOAT*, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT, const INT*, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT, const BOOL*, UINT) override { return S_OK; }
    // Fixed-function state has no place in a program.
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE, const D3DMATRIX*) override { return no("SetTransform"); }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9*) override { return no("SetMaterial"); }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD, const D3DLIGHT9*) override { return no("SetLight"); }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD, BOOL) override { return no("LightEnable"); }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) override { return no("SetTextureStageState"); }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(FLOAT) override { return no("SetNPatchMode"); }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD) override { return no("SetFVF"); }

private:
    HRESULT no(const char* what)
    {
        if (unsupported.empty())
            unsupported = what;
        return S_OK;
    }
};

void recordPass(ID3DXEffect* effect, D3DXHANDLE technique, PassRecorder& recorder)
{
    effect->SetStateManager(&recorder);
    effect->SetTechnique(technique);
    UINT passes = 0;
    if (effect->Begin(&passes, 7u) >= 0)
    {
        if (passes)
        {
            effect->BeginPass(0);
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

bool addShaderConstants(SubmitProgram& program, ID3DXEffect* effect, const DWORD* function,
                        bool pixel)
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
            continue;
        Binding* const b = bindingFor(program, effect, parameter);
        if (b)
            addTargets(program, *b, table, constant, desc, pixel, 0u);
    }
    table->Release();
    return true;
}

void buildProgram(SubmitProgram& program)
{
    Ra::RaShader* const shader = program.shader;
    ID3DXEffect* const effect = static_cast<ID3DXEffect*>(shader->m_effect);
    const D3DXHANDLE technique = ::renddx9abi::d3dxHandle(shader->m_activeTechnique);
    D3DXTECHNIQUE_DESC techniqueDesc;
    if (effect->GetTechniqueDesc(technique, &techniqueDesc) >= 0 && techniqueDesc.Name)
        program.techniqueName = techniqueDesc.Name;
    for (unsigned int s = 0; s < kStateSlots; ++s)
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
        IDirect3DTexture9* const mark = marker(shader->m_device, saved.size());
        if (!mark)
            break;
        IDirect3DBaseTexture9* previous = 0;
        effect->GetTexture(parameter, &previous);
        saved.push_back(std::make_pair(parameter, previous));
        effect->SetTexture(parameter, mark);
    }

    PassRecorder recorder;
    recordPass(effect, technique, recorder);

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
        recordPass(effect, technique, probe);
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

    if (!recorder.unsupported.empty() || !recorder.vs)
    {
        ::renddx9abi::logf("Submit: %s %s: not drawable here (%s)", shader->name().c_str(),
                           program.techniqueName.c_str(),
                           recorder.vs ? recorder.unsupported.c_str() : "no vertex shader");
        return;
    }
    program.vs = recorder.vs;
    program.vs->AddRef();
    program.ps = recorder.ps;
    if (program.ps)
        program.ps->AddRef();

    // The constant tables of the shaders the pass bound. The effects' pass descriptions carry no
    // shader functions (GetPassDesc answers null), the shader objects keep theirs.
    std::vector<DWORD> function;
    if (!addShaderConstants(program, effect, shaderFunction(program.vs, function), false)
        || (program.ps
            && !addShaderConstants(program, effect, shaderFunction(program.ps, function), true)))
    {
        ::renddx9abi::logf("Submit: %s %s: no constant table", shader->name().c_str(),
                           program.techniqueName.c_str());
        return;
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
    program.valid = true;

    if (drawStatsOn())   // [DEV] BF2_DRAW_STATS: what each program reads, and from where
    {
        ::renddx9abi::logf("Submit: %s %s: %u bindings, %u fixed states, %u sampler states",
                           shader->name().c_str(), program.techniqueName.c_str(),
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
                               b.held ? "held" : "effect", b.stateSlot,
                               static_cast<unsigned int>(b.words.size()), where.c_str());
        }
    }
}

// RaShader::applyParameterVector with the sink in place of the effect.
void applyUsed(SubmitProgram* program, void* block, const std::vector<UsedParameter>& used)
{
    if (used.empty())
        return;
    typedef int (__thiscall* ApplyFn)(void*, void*, void*);
    struct BlockHeader { void* vptr; const std::vector<int>* registry; };
    unsigned char* const bytes = static_cast<unsigned char*>(block);
    const std::vector<int>& offsets = *reinterpret_cast<const BlockHeader*>(bytes)->registry;
    g_sink.program = program;
    for (size_t i = 0; i < used.size(); ++i)
    {
        unsigned char* const holder = bytes + offsets[used[i].registryOffset];
        g_sink.current = used[i].binding;
        reinterpret_cast<ApplyFn>((*reinterpret_cast<void***>(holder))[0])(
            holder, static_cast<ID3DXEffect*>(&g_sink),
            const_cast<char*>(used[i].handle));
    }
    g_sink.current = 0;
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

SubmitProgram* submitProgram(Ra::RaShader* shader, unsigned int key)
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
    if (shader->selectTechnique(key))
        buildProgram(*slot);
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
    if (drawStatsOn())   // [DEV] BF2_DRAW_STATS
        drawStatsBeginPass(program->shader->name(), program->techniqueName, 0);
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
    g_sink.SetMatrix(handle, reinterpret_cast<const D3DXMATRIX*>(matrix));
    g_sink.current = 0;
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

void submitReset()
{
    g_programs.clear();
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9
