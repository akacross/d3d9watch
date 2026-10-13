/* d3d9watch.asi - catch the code that misuses GTA's Direct3D 9 device.
 *
 * GTA creates its device without D3DCREATE_MULTITHREADED (RenderWare adds it
 * only when its "multithread safe" option is on, and GTA never sets it), so
 * every call from another thread races the game. Calling a method on a
 * texture or buffer that has already been deleted, or binding one, is just
 * as bad. Under DXVK both leave damage that surfaces much later and
 * somewhere else (a DXVK allocator free list off by one,
 * d3d9.dll+0x1A63E9, 2026-10-11/12). This plugin watches for both and names
 * the caller: module+offset and a short stack, once per call site, then at
 * counts 1, 2, 4, 8 and so on, in d3d9watch.log next to the game (the run
 * before is kept as d3d9watch.prev.log) and through OutputDebugString
 * (sampcmd.log).
 *
 * Every IDirect3DDevice9 method is hooked through the device's vtable,
 * chaining to whatever is there (SA-MP's, open.mp's and MoonLoader's hooks
 * included). A stub compares the calling thread with the game window's
 * thread and jumps straight on when they match. The methods that take a
 * resource also check it for DXVK's "deleted" mark: ComObject's private
 * refcount at +8 gets 0x80000000 added just before the object is freed.
 * Under DXVK (found by its RTTI), every texture, surface and buffer method
 * is hooked as well: called from another thread, or on a deleted object.
 *
 * Under DXVK it also names what makes Reset fail. Reset refuses while any
 * D3DPOOL_DEFAULT resource is still referenced, and RenderWare does not check
 * its result: when a window resize makes it reset the device, a failed Reset
 * leaves its camera raster released and the next clear reads a NULL texture
 * (gta_sa.exe+0x3F7C82, 2026-10-13). So DXVK's own device methods, beneath
 * every proxy, record who makes each D3DPOOL_DEFAULT resource and who fetches
 * surfaces, and a failed Reset lists what is still held.
 *
 * It only watches; every call goes on to the original unchanged.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define GTA_DEVICE_SLOT 0xC97C28u   /* RwD3D9's IDirect3DDevice9*: the address it passes to CreateDevice */
#define GTA_WINDOW_SLOT 0xC97C1Cu   /* the HWND it passes as the focus window */
#define GTA_CREATE_PUSH 0x7F6761u   /* push 0xC97C28 before IDirect3D9::CreateDevice, gta_sa.exe 1.0 US */

#define DEVICE_METHODS 119          /* IDirect3DDevice9; the Ex methods after it are not GTA's */
#define MAX_TABLES     32
#define MAX_METHODS    120
#define STUB_SIZE      40

enum { KIND_THREAD, KIND_DELETED_THIS, KIND_DELETED_ARG, KIND_COUNT };
static const char *const kind_text[KIND_COUNT] = {
    "called from another thread",
    "called on a deleted object",
    "handed a deleted object",
};

static const char *const device_methods[DEVICE_METHODS] = {
    "QueryInterface", "AddRef", "Release", "TestCooperativeLevel", "GetAvailableTextureMem",
    "EvictManagedResources", "GetDirect3D", "GetDeviceCaps", "GetDisplayMode", "GetCreationParameters",
    "SetCursorProperties", "SetCursorPosition", "ShowCursor", "CreateAdditionalSwapChain", "GetSwapChain",
    "GetNumberOfSwapChains", "Reset", "Present", "GetBackBuffer", "GetRasterStatus",
    "SetDialogBoxMode", "SetGammaRamp", "GetGammaRamp", "CreateTexture", "CreateVolumeTexture",
    "CreateCubeTexture", "CreateVertexBuffer", "CreateIndexBuffer", "CreateRenderTarget", "CreateDepthStencilSurface",
    "UpdateSurface", "UpdateTexture", "GetRenderTargetData", "GetFrontBufferData", "StretchRect",
    "ColorFill", "CreateOffscreenPlainSurface", "SetRenderTarget", "GetRenderTarget", "SetDepthStencilSurface",
    "GetDepthStencilSurface", "BeginScene", "EndScene", "Clear", "SetTransform",
    "GetTransform", "MultiplyTransform", "SetViewport", "GetViewport", "SetMaterial",
    "GetMaterial", "SetLight", "GetLight", "LightEnable", "GetLightEnable",
    "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState", "CreateStateBlock",
    "BeginStateBlock", "EndStateBlock", "SetClipStatus", "GetClipStatus", "GetTexture",
    "SetTexture", "GetTextureStageState", "SetTextureStageState", "GetSamplerState", "SetSamplerState",
    "ValidateDevice", "SetPaletteEntries", "GetPaletteEntries", "SetCurrentTexturePalette", "GetCurrentTexturePalette",
    "SetScissorRect", "GetScissorRect", "SetSoftwareVertexProcessing", "GetSoftwareVertexProcessing", "SetNPatchMode",
    "GetNPatchMode", "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP",
    "ProcessVertices", "CreateVertexDeclaration", "SetVertexDeclaration", "GetVertexDeclaration", "SetFVF",
    "GetFVF", "CreateVertexShader", "SetVertexShader", "GetVertexShader", "SetVertexShaderConstantF",
    "GetVertexShaderConstantF", "SetVertexShaderConstantI", "GetVertexShaderConstantI", "SetVertexShaderConstantB", "GetVertexShaderConstantB",
    "SetStreamSource", "GetStreamSource", "SetStreamSourceFreq", "GetStreamSourceFreq", "SetIndices",
    "GetIndices", "CreatePixelShader", "SetPixelShader", "GetPixelShader", "SetPixelShaderConstantF",
    "GetPixelShaderConstantF", "SetPixelShaderConstantI", "GetPixelShaderConstantI", "SetPixelShaderConstantB", "GetPixelShaderConstantB",
    "DrawRectPatch", "DrawTriPatch", "DeletePatch", "CreateQuery",
};

/* Device methods that take resources: bit n = argument n (after `this`) is one. */
static unsigned device_resource_args(unsigned method)
{
    switch (method) {
    case 30: return 0x5;   /* UpdateSurface(src, rect, dst, point) */
    case 31: return 0x3;   /* UpdateTexture(src, dst) */
    case 32: return 0x3;   /* GetRenderTargetData(rt, dst) */
    case 33: return 0x2;   /* GetFrontBufferData(swapchain, dst) */
    case 34: return 0x5;   /* StretchRect(src, rect, dst, rect, filter) */
    case 35: return 0x1;   /* ColorFill(surface, rect, colour) */
    case 37: return 0x2;   /* SetRenderTarget(index, surface) */
    case 39: return 0x1;   /* SetDepthStencilSurface(surface) */
    case 65: return 0x2;   /* SetTexture(stage, texture) */
    case 100: return 0x2;  /* SetStreamSource(stream, vb, offset, stride) */
    case 104: return 0x1;  /* SetIndices(ib) */
    default: return 0;
    }
}

static const char *const resource_common[] = {
    "QueryInterface", "AddRef", "Release", "GetDevice", "SetPrivateData", "GetPrivateData",
    "FreePrivateData", "SetPriority", "GetPriority", "PreLoad", "GetType",
};
static const char *const texture_methods[] = {
    "SetLOD", "GetLOD", "GetLevelCount", "SetAutoGenFilterType", "GetAutoGenFilterType",
    "GenerateMipSubLevels", "GetLevelDesc", "GetLevel", "Lock", "Unlock", "AddDirty",
};
static const char *const buffer_methods[] = { "Lock", "Unlock", "GetDesc" };
static const char *const surface_methods[] = { "GetContainer", "GetDesc", "LockRect", "UnlockRect", "GetDC", "ReleaseDC" };

enum { IF_TEXTURE, IF_BUFFER, IF_SURFACE };
static const struct { const char *rtti; const char *name; int iface; } dxvk_classes[] = {
    { "N4dxvk13D3D9Texture2DE", "Texture2D", IF_TEXTURE },
    { "N4dxvk15D3D9BaseTextureINS_11D3D9SurfaceEJ17IDirect3DTexture9EEE", "Texture2D (base)", IF_TEXTURE },
    { "N4dxvk12D3D9ResourceIJ17IDirect3DTexture9EEE", "Texture2D (resource)", IF_TEXTURE },
    { "N4dxvk15D3D9TextureCubeE", "TextureCube", IF_TEXTURE },
    { "N4dxvk15D3D9BaseTextureINS_11D3D9SurfaceEJ21IDirect3DCubeTexture9EEE", "TextureCube (base)", IF_TEXTURE },
    { "N4dxvk12D3D9ResourceIJ21IDirect3DCubeTexture9EEE", "TextureCube (resource)", IF_TEXTURE },
    { "N4dxvk13D3D9Texture3DE", "Texture3D", IF_TEXTURE },
    { "N4dxvk15D3D9BaseTextureINS_10D3D9VolumeEJ23IDirect3DVolumeTexture9EEE", "Texture3D (base)", IF_TEXTURE },
    { "N4dxvk12D3D9ResourceIJ23IDirect3DVolumeTexture9EEE", "Texture3D (resource)", IF_TEXTURE },
    { "N4dxvk16D3D9VertexBufferE", "VertexBuffer", IF_BUFFER },
    { "N4dxvk10D3D9BufferIJ22IDirect3DVertexBuffer9EEE", "VertexBuffer (base)", IF_BUFFER },
    { "N4dxvk12D3D9ResourceIJ22IDirect3DVertexBuffer9EEE", "VertexBuffer (resource)", IF_BUFFER },
    { "N4dxvk15D3D9IndexBufferE", "IndexBuffer", IF_BUFFER },
    { "N4dxvk10D3D9BufferIJ21IDirect3DIndexBuffer9EEE", "IndexBuffer (base)", IF_BUFFER },
    { "N4dxvk12D3D9ResourceIJ21IDirect3DIndexBuffer9EEE", "IndexBuffer (resource)", IF_BUFFER },
    { "N4dxvk11D3D9SurfaceE", "Surface", IF_SURFACE },
    { "N4dxvk15D3D9SubresourceIJ17IDirect3DSurface9EEE", "Surface (base)", IF_SURFACE },
    { "N4dxvk12D3D9ResourceIJ17IDirect3DSurface9EEE", "Surface (resource)", IF_SURFACE },
};

typedef struct {
    const char *name;
    int resource;              /* `this` is a DXVK resource: check it for the deleted mark */
    int iface;
    void **vtable;
    unsigned count;
    void *orig[MAX_METHODS];
} Table;

static Table tables[MAX_TABLES];
static unsigned ntables;
static volatile DWORD render_tid;   /* read by the stubs */
static unsigned char *stubs;
static unsigned nstubs;
static HMODULE self, d3d9_module, dxvk_module;
static int dxvk;                    /* resource vtables known: deleted checks are possible */

static CRITICAL_SECTION lock;
static FILE *log_file;
static LONG reports[KIND_COUNT];

static void log_line(const char *fmt, ...)
{
    char buf[1024] = "[d3d9watch] ";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + 12, sizeof buf - 14, fmt, ap);
    va_end(ap);
    strcat(buf, "\n");
    EnterCriticalSection(&lock);
    OutputDebugStringA(buf);
    if (log_file) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(log_file, "%02d:%02d:%02d.%03d %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf + 12);
        fflush(log_file);
    }
    LeaveCriticalSection(&lock);
}

/* "module+0xOFF" for a code address, or NULL when it is in no module. */
static const char *where(void *addr, char *buf, size_t n)
{
    HMODULE mod;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &mod))
        return NULL;
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(mod, path, sizeof path);
    const char *base = path;
    for (DWORD i = 0; i < len; i++)
        if (path[i] == '\\' || path[i] == '/') base = path + i + 1;
    snprintf(buf, n, "%s+0x%lX", base, (unsigned long)((char *)addr - (char *)mod));
    return buf;
}

/* Inside a module's executable section. */
static int in_code(void *addr)
{
    HMODULE mod;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &mod))
        return 0;
    unsigned char *base = (unsigned char *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    DWORD rva = (DWORD)((unsigned char *)addr - base);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) && rva >= sec->VirtualAddress + 8
            && rva < sec->VirtualAddress + sec->Misc.VirtualSize)
            return 1;
    return 0;
}

/* Whether a return address follows a call instruction. */
static int after_call(const unsigned char *r)
{
    if (r[-5] == 0xE8) return 1;                                        /* call rel32 */
    if (r[-2] == 0xFF && (r[-1] & 0x38) == 0x10) return 1;              /* call reg / [reg] */
    if (r[-3] == 0xFF && (r[-2] & 0x38) == 0x10) return 1;              /* call [reg+d8] */
    if (r[-6] == 0xFF && (r[-5] & 0x38) == 0x10) return 1;              /* call [d32] / [reg+d32] */
    if (r[-7] == 0xFF && (r[-6] & 0x38) == 0x10) return 1;              /* call [reg*s+d32] */
    return 0;
}

/* The caller (frame[0]) and the next return addresses on the stack, up to
 * `max`, outside `skip` and this plugin. */
static unsigned stack_sites(void **frame, HMODULE skip, void **out, unsigned max)
{
    unsigned found = 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(frame, &mbi, sizeof mbi)) return 0;
    void **top = (void **)((char *)mbi.BaseAddress + mbi.RegionSize);
    for (void **p = frame; p < top && p < frame + 2048 && found < max; p++) {
        void *v = *p;
        if (!in_code(v) || !after_call(v)) continue;
        HMODULE mod = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)v, &mod);
        if ((mod == skip || mod == self) && p != frame) continue;
        out[found++] = v;
    }
    return found;
}

/* "a < b < c" */
static void sites_text(void *const *sites, unsigned count, char *out, size_t n)
{
    char one[160];
    size_t used = 0;
    out[0] = 0;
    for (unsigned i = 0; i < count; i++) {
        if (!sites[i] || !where(sites[i], one, sizeof one)) continue;
        int w = snprintf(out + used, n - used, "%s%s", used ? " < " : "", one);
        if (w < 0 || (size_t)w >= n - used) break;
        used += (size_t)w;
    }
}

/* The caller and the next few return addresses on the stack, outside
 * d3d9.dll and this plugin. */
static void stack_text(void **frame, char *out, size_t n)
{
    void *sites[6];
    sites_text(sites, stack_sites(frame, d3d9_module, sites, 6), out, n);
}

/* One line per call site and kind, then at counts 1, 2, 4, 8... */
#define SITES 1024
static struct { unsigned key_id, kind; void *caller; LONG count; } sites[SITES];
static LONG sites_full;

static void report(int kind, unsigned id, void **frame, void *object, int arg)
{
    void *caller = frame[0];
    InterlockedIncrement(&reports[kind]);
    EnterCriticalSection(&lock);
    unsigned h = ((unsigned)(uintptr_t)caller * 2654435761u ^ id * 40503u ^ (unsigned)kind) % SITES;
    LONG count = 0;
    for (unsigned probe = 0; probe < SITES; probe++, h = (h + 1) % SITES) {
        if (sites[h].count == 0) {
            sites[h].key_id = id; sites[h].kind = (unsigned)kind; sites[h].caller = caller;
            count = sites[h].count = 1;
            break;
        }
        if (sites[h].key_id == id && sites[h].kind == (unsigned)kind && sites[h].caller == caller) {
            count = ++sites[h].count;
            break;
        }
    }
    LeaveCriticalSection(&lock);
    if (count == 0) {   /* table full: one line for that, then quiet */
        if (InterlockedIncrement(&sites_full) == 1) log_line("more than %d call sites; new ones are counted only", SITES);
        return;
    }
    if (count & (count - 1))
        return;

    Table *t = &tables[id >> 16];
    unsigned m = id & 0xFFFF;
    const char *method = "?";
    char numbered[16];
    if (!t->resource) {
        method = m < DEVICE_METHODS ? device_methods[m] : "?";
    } else if (m < 11) {
        method = resource_common[m];
    } else if (t->iface == IF_TEXTURE && m - 11 < sizeof texture_methods / sizeof *texture_methods) {
        method = texture_methods[m - 11];
    } else if (t->iface == IF_BUFFER && m - 11 < sizeof buffer_methods / sizeof *buffer_methods) {
        method = buffer_methods[m - 11];
    } else if (t->iface == IF_SURFACE && m - 11 < sizeof surface_methods / sizeof *surface_methods) {
        method = surface_methods[m - 11];
    } else {
        snprintf(numbered, sizeof numbered, "#%u", m);
        method = numbered;
    }
    char stack[900], what[48] = "";
    stack_text(frame, stack, sizeof stack);
    if (kind == KIND_DELETED_ARG)
        snprintf(what, sizeof what, " (argument %d: %p)", arg + 1, object);
    else if (kind == KIND_DELETED_THIS)
        snprintf(what, sizeof what, " (object %p)", object);
    log_line("%s::%s %s%s, thread %lu (game %lu), x%ld: %s", t->name, method, kind_text[kind], what,
             GetCurrentThreadId(), render_tid, count, stack[0] ? stack : "?");
}

/* DXVK's deleted mark: ComObject::ReleasePrivate adds 0x80000000 to the
 * private refcount (+8) right before it deletes the object. */
static int deleted(void *object)
{
    return object && (((volatile unsigned char *)object)[0xB] & 0x80);
}

void *__cdecl watch_check(unsigned id, void **frame);
void *__cdecl watch_check(unsigned id, void **frame)
{
    Table *t = &tables[id >> 16];
    unsigned m = id & 0xFFFF;
    if (GetCurrentThreadId() != render_tid)
        report(KIND_THREAD, id, frame, frame[1], -1);
    if (t->resource) {
        if (deleted(frame[1]))
            report(KIND_DELETED_THIS, id, frame, frame[1], -1);
    } else if (dxvk) {
        unsigned args = device_resource_args(m);
        for (int a = 0; args; a++, args >>= 1)
            if ((args & 1) && deleted(frame[2 + a]))
                report(KIND_DELETED_ARG, id, frame, frame[2 + a], a);
    }
    return t->orig[m];
}

/* The stubs jump here with the method id pushed over the caller's frame:
 * [esp] = id, [esp+4] = the caller's return address, [esp+8] = this, ...
 * watch_check returns the original; ret goes to it with the stack exactly as
 * the caller left it and every register restored. */
void watch_common(void);
__asm__(
    ".text\n"
    ".globl _watch_common\n"
    "_watch_common:\n"
    "    pushfl\n"
    "    pushal\n"
    "    mov  36(%esp), %eax\n"
    "    lea  40(%esp), %ecx\n"
    "    push %ecx\n"
    "    push %eax\n"
    "    call _watch_check\n"
    "    add  $8, %esp\n"
    "    mov  %eax, 36(%esp)\n"
    "    popal\n"
    "    popfl\n"
    "    ret\n"
);

static void put32(unsigned char *p, unsigned v) { memcpy(p, &v, 4); }

/* One stub. Every path but the slow one leaves eax changed only, which COM
 * methods (stdcall, arguments on the stack) do not read. */
static unsigned char *make_stub(unsigned id, void **orig_slot, int fast_thread, int fast_this)
{
    unsigned char *s = stubs + nstubs++ * STUB_SIZE, *p = s;
    if (fast_thread) {
        *p++ = 0x64; *p++ = 0xA1; put32(p, 0x24); p += 4;              /* mov eax, fs:[24h]  ; thread id */
        *p++ = 0x3B; *p++ = 0x05; put32(p, (unsigned)(uintptr_t)&render_tid); p += 4;   /* cmp eax, [render_tid] */
        *p++ = 0x75; unsigned char *to_slow1 = p++;                     /* jne slow */
        unsigned char *to_slow2 = NULL;
        if (fast_this) {
            *p++ = 0x8B; *p++ = 0x44; *p++ = 0x24; *p++ = 0x04;         /* mov eax, [esp+4]   ; this */
            *p++ = 0xF6; *p++ = 0x40; *p++ = 0x0B; *p++ = 0x80;         /* test byte [eax+0Bh], 80h */
            *p++ = 0x75; to_slow2 = p++;                                /* jnz slow */
        }
        *p++ = 0xFF; *p++ = 0x25; put32(p, (unsigned)(uintptr_t)orig_slot); p += 4;     /* jmp [orig] */
        *to_slow1 = (unsigned char)(p - (to_slow1 + 1));
        if (to_slow2) *to_slow2 = (unsigned char)(p - (to_slow2 + 1));
    }
    *p++ = 0x68; put32(p, id); p += 4;                                  /* slow: push id */
    *p = 0xE9; put32(p + 1, (unsigned)((uintptr_t)watch_common - (uintptr_t)(p + 5)));   /* jmp watch_common */
    return s;
}

static int hook_table(const char *name, void **vtable, unsigned count, int resource, int iface)
{
    if (ntables >= MAX_TABLES || count > MAX_METHODS)
        return 0;
    Table *t = &tables[ntables];
    t->name = name; t->vtable = vtable; t->count = count; t->resource = resource; t->iface = iface;
    /* A base class's vtable leaves the methods it does not implement empty
     * (MinGW writes 0, not a pure-virtual stub): those stay as they are. */
    unsigned hooked = 0;
    for (unsigned i = 0; i < count; i++)
        if ((t->orig[i] = vtable[i]) != NULL) hooked++;
    if (!hooked)
        return 0;
    void *new_entries[MAX_METHODS];
    for (unsigned i = 0; i < count; i++) {
        unsigned id = (ntables << 16) | i;
        int always_slow = !resource && device_resource_args(i) != 0;
        new_entries[i] = t->orig[i] ? make_stub(id, &t->orig[i], !always_slow, resource) : NULL;
    }
    DWORD old;
    if (!VirtualProtect(vtable, count * sizeof *vtable, PAGE_READWRITE, &old))
        return 0;
    for (unsigned i = 0; i < count; i++)
        if (new_entries[i])
            InterlockedExchange((LONG *)&vtable[i], (LONG)(uintptr_t)new_entries[i]);
    VirtualProtect(vtable, count * sizeof *vtable, old, &old);
    ntables++;
    return 1;
}

/* A vtable by its RTTI name (Itanium ABI, as MinGW builds DXVK): the name
 * string, the typeinfo whose name points at it, the vtable whose slot -1
 * points at the typeinfo with offset-to-top 0 before it. */
static void **find_vtable(HMODULE mod, const char *rtti)
{
    unsigned char *base = (unsigned char *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    size_t size = nt->OptionalHeader.SizeOfImage, len = strlen(rtti) + 1;
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    unsigned char *name = NULL;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !name; i++) {
        IMAGE_SECTION_HEADER *s = &sec[i];
        if (s->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        unsigned char *p = base + s->VirtualAddress, *end = p + s->Misc.VirtualSize;
        for (; p + len <= end; p++)
            if (*p == rtti[0] && memcmp(p, rtti, len) == 0 && (p == base || p[-1] == 0)) { name = p; break; }
    }
    if (!name) return NULL;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER *s = &sec[i];
        if (s->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        unsigned *p = (unsigned *)(base + s->VirtualAddress), *end = (unsigned *)(base + s->VirtualAddress + s->Misc.VirtualSize);
        for (; p + 1 < end; p++) {
            if (p[1] != (unsigned)(uintptr_t)name) continue;
            unsigned ti = (unsigned)(uintptr_t)p;          /* typeinfo: {vptr, name} */
            for (int j = 0; j < nt->FileHeader.NumberOfSections; j++) {
                IMAGE_SECTION_HEADER *s2 = &sec[j];
                if (s2->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
                unsigned *q = (unsigned *)(base + s2->VirtualAddress), *qend = (unsigned *)(base + s2->VirtualAddress + s2->Misc.VirtualSize);
                /* slot 0 may be empty in a base class's vtable (see hook_table) */
                for (q++; q + 2 < qend; q++)
                    if (q[0] == ti && q[-1] == 0 && (q[1] == 0 || (uintptr_t)q[1] - (uintptr_t)base < size)
                        && (uintptr_t)q[2] - (uintptr_t)base < size)
                        return (void **)(q + 1);
            }
        }
    }
    return NULL;
}

/* ---- Reset: who still holds a D3DPOOL_DEFAULT resource -------------------
 *
 * DXVK's Reset returns D3DERR_INVALIDCALL while any D3DPOOL_DEFAULT resource
 * still has a reference: a texture, buffer or surface made in that pool, a
 * second swap chain, or the back buffer or depth surface when something kept
 * what GetBackBuffer, GetRenderTarget or GetDepthStencilSurface handed it.
 * RenderWare never checks: a window resize makes _rwD3D9CameraClear release
 * its video memory and Reset (gta_sa.exe 0x7F795B, 0x7F7998, then 0x7F7AAC
 * with fallback parameters), and after a failed one it clears the camera
 * raster it has just released (gta_sa.exe+0x3F7C82, 2026-10-13).
 *
 * The wrappers sit on DXVK's own device vtable, beneath every proxy
 * (ProperShaders', MoonLoader's, SA-MP's), so they see the resources those
 * make for themselves as well as the game's, and Reset only after every
 * layer above has released what it was going to. A made resource keeps the
 * caller and three more return addresses; a fetched surface keeps its
 * distinct callers only, since GetRenderTarget runs every frame. When Reset
 * fails, everything recorded that is still referenced is listed. */
#define HELD_SLOTS 16384
#define HELD_SITES 4
enum { HELD_TEXTURE, HELD_VOLUME, HELD_CUBE, HELD_VB, HELD_IB, HELD_RT, HELD_DS, HELD_PLAIN, HELD_SWAPCHAIN, HELD_FETCHED };
static const char *const held_kinds[] = {
    "texture", "volume texture", "cube texture", "vertex buffer", "index buffer",
    "render target", "depth surface", "offscreen surface", "swap chain", "surface",
};

typedef struct {
    void *object;                       /* NULL: a free slot */
    void *vptr;                         /* the object's when recorded: gone once it differs */
    unsigned kind, w, h, format, usage; /* w = length for a buffer, format = FVF for a vertex buffer */
    void *made[HELD_SITES];             /* who made it: the caller, then up the stack */
    void *fetch[HELD_SITES];            /* distinct callers that fetched it from the device */
    unsigned fetches[HELD_SITES];
} Held;

static Held *held;                      /* HELD_SLOTS, open addressing; pruned, never deleted from */
static unsigned held_count;
static LONG held_full;
static int tracking;
static void *dev_orig[DEVICE_METHODS];  /* DXVK's device methods beneath the wrappers */

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return 0;
    return (const char *)p + n <= (const char *)mbi.BaseAddress + mbi.RegionSize;
}

/* The public reference count (ComObject's, at +4), or -1 once it is gone. */
static long held_refs(const Held *e)
{
    if (!readable(e->object, 12) || *(void *const volatile *)e->object != e->vptr || deleted(e->object))
        return -1;
    return (long)((const volatile unsigned *)e->object)[1];
}

static Held *held_find(void *object, int free_slot)
{
    unsigned h = (unsigned)(((uintptr_t)object >> 3) * 2654435761u) % HELD_SLOTS;
    for (unsigned probe = 0; probe < HELD_SLOTS; probe++, h = (h + 1) % HELD_SLOTS) {
        if (held[h].object == object) return &held[h];
        if (!held[h].object) return free_slot ? &held[h] : NULL;
    }
    return NULL;
}

/* Keep what is still alive, referenced or not. */
static void held_prune(void)
{
    Held *old = malloc(HELD_SLOTS * sizeof *old);
    if (!old) return;
    memcpy(old, held, HELD_SLOTS * sizeof *old);
    memset(held, 0, HELD_SLOTS * sizeof *held);
    held_count = 0;
    for (unsigned i = 0; i < HELD_SLOTS; i++)
        if (old[i].object && held_refs(&old[i]) >= 0) {
            *held_find(old[i].object, 1) = old[i];
            held_count++;
        }
    free(old);
}

/* The entry for `object`, cleared when it is new or its address was reused.
 * Under the lock. */
static Held *held_entry(void *object, unsigned kind)
{
    Held *e = held_find(object, 0);
    if (!e) {
        if (held_count >= HELD_SLOTS * 3 / 4) held_prune();
        if (held_count >= HELD_SLOTS * 3 / 4) {
            if (InterlockedIncrement(&held_full) == 1)
                log_line("more than %d D3DPOOL_DEFAULT resources alive; new ones are not recorded", HELD_SLOTS * 3 / 4);
            return NULL;
        }
        e = held_find(object, 1);
        held_count++;
    } else if (e->vptr == *(void **)object && kind == HELD_FETCHED) {
        return e;
    }
    memset(e, 0, sizeof *e);
    e->object = object;
    e->vptr = *(void **)object;
    e->kind = kind;
    return e;
}

static void held_made(void *object, unsigned kind, unsigned w, unsigned h, unsigned format, unsigned usage, void **frame)
{
    if (!object || !held) return;
    void *sites[HELD_SITES];
    unsigned n = stack_sites(frame, dxvk_module, sites, HELD_SITES);   /* slow: outside the lock */
    EnterCriticalSection(&lock);
    Held *e = held_entry(object, kind);
    if (e) {
        e->w = w; e->h = h; e->format = format; e->usage = usage;
        memcpy(e->made, sites, n * sizeof *sites);
    }
    LeaveCriticalSection(&lock);
}

static void held_fetched(void *object, void *caller)
{
    if (!object || !held) return;
    EnterCriticalSection(&lock);
    Held *e = held_entry(object, HELD_FETCHED);
    if (e) {
        for (unsigned i = 0; i < HELD_SITES; i++) {
            if (e->fetch[i] == caller) { e->fetches[i]++; break; }
            if (!e->fetch[i]) { e->fetch[i] = caller; e->fetches[i] = 1; break; }
        }
    }
    LeaveCriticalSection(&lock);
}

static const char *format_text(unsigned f, char *buf, size_t n)
{
    switch (f) {
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_A8: return "A8";
    case D3DFMT_L8: return "L8";
    case D3DFMT_A2R10G10B10: return "A2R10G10B10";
    case D3DFMT_A16B16G16R16: return "A16B16G16R16";
    case D3DFMT_R16F: return "R16F";
    case D3DFMT_G16R16F: return "G16R16F";
    case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
    case D3DFMT_R32F: return "R32F";
    case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
    case D3DFMT_D16: return "D16";
    case D3DFMT_D24S8: return "D24S8";
    case D3DFMT_D24X8: return "D24X8";
    case D3DFMT_D32: return "D32";
    case D3DFMT_INDEX16: return "16-bit";
    case D3DFMT_INDEX32: return "32-bit";
    }
    if (f >= 0x20202020u) {   /* a FOURCC: INTZ, RAWZ, DF24, NULL, ... */
        snprintf(buf, n, "%c%c%c%c", (char)f, (char)(f >> 8), (char)(f >> 16), (char)(f >> 24));
        return buf;
    }
    snprintf(buf, n, "format %u", f);
    return buf;
}

/* "512x512 A8R8G8B8 (render target, dynamic)" */
static void held_desc(const Held *e, char *out, size_t n)
{
    char fmt[24];
    unsigned w = e->w, h = e->h, format = e->format, usage = e->usage;
    if (e->kind == HELD_FETCHED) {   /* the device's own surfaces: ask them */
        D3DSURFACE_DESC d;
        if (SUCCEEDED(IDirect3DSurface9_GetDesc((IDirect3DSurface9 *)e->object, &d))) {
            w = d.Width; h = d.Height; format = d.Format; usage = d.Usage;
        }
    }
    int at;
    if (e->kind == HELD_VB)
        at = snprintf(out, n, "%u bytes, FVF 0x%X", w, format);
    else if (e->kind == HELD_IB)
        at = snprintf(out, n, "%u bytes, %s", w, format_text(format, fmt, sizeof fmt));
    else if (e->kind == HELD_SWAPCHAIN)
        at = snprintf(out, n, "%ux%u", w, h);
    else
        at = snprintf(out, n, "%ux%u %s", w, h, format_text(format, fmt, sizeof fmt));
    if (at < 0 || (size_t)at >= n) return;
    static const struct { DWORD bit; const char *name; } flags[] = {
        { D3DUSAGE_RENDERTARGET, "render target" }, { D3DUSAGE_DEPTHSTENCIL, "depth" },
        { D3DUSAGE_DYNAMIC, "dynamic" }, { D3DUSAGE_WRITEONLY, "write-only" },
        { D3DUSAGE_AUTOGENMIPMAP, "mipmaps" },
    };
    const char *sep = " (";
    for (size_t i = 0; i < sizeof flags / sizeof *flags; i++)
        if (usage & flags[i].bit) {
            int w2 = snprintf(out + at, n - (size_t)at, "%s%s", sep, flags[i].name);
            if (w2 < 0 || (size_t)w2 >= n - (size_t)at) return;
            at += w2; sep = ", ";
        }
    if (*sep == ',') snprintf(out + at, n - (size_t)at, ")");
}

static const char *hresult_text(HRESULT hr)
{
    switch ((unsigned)hr) {
    case 0x8876086Cu: return "D3DERR_INVALIDCALL";
    case 0x88760868u: return "D3DERR_DEVICELOST";
    case 0x88760869u: return "D3DERR_DEVICENOTRESET";
    case 0x8876017Cu: return "D3DERR_OUTOFVIDEOMEMORY";
    case 0x88760827u: return "D3DERR_DRIVERINTERNALERROR";
    case 0x8007000Eu: return "E_OUTOFMEMORY";
    }
    return "?";
}

/* After a failed Reset: everything recorded that is still referenced, alike
 * ones on one line. */
static void held_report(HRESULT hr)
{
    if (!held) return;
    EnterCriticalSection(&lock);
    Held *alive = malloc(HELD_SLOTS * sizeof *alive);
    long *refs = malloc(HELD_SLOTS * sizeof *refs);
    unsigned n = 0;
    for (unsigned i = 0; alive && refs && i < HELD_SLOTS; i++) {
        long r;
        if (held[i].object && (r = held_refs(&held[i])) > 0) {
            alive[n] = held[i];
            refs[n++] = r;
        }
    }
    LeaveCriticalSection(&lock);
    if (!n) {
        log_line("Reset failed (0x%08lX %s), and nothing d3d9watch saw made or fetched in D3DPOOL_DEFAULT is still "
                 "referenced: the holder made it before d3d9watch started, or through another device",
                 (unsigned long)hr, hresult_text(hr));
    } else {
        log_line("Reset failed (0x%08lX %s): %u D3DPOOL_DEFAULT resource(s) still referenced",
                 (unsigned long)hr, hresult_text(hr), n);
    }
    char desc[128], made[700], fetched[700], one[160];
    for (unsigned i = 0; i < n; i++) {
        if (!alive[i].object) continue;
        unsigned same = 1;
        long total = refs[i];
        for (unsigned j = i + 1; j < n; j++) {
            Held *a = &alive[i], *b = &alive[j];
            if (b->object && a->kind == b->kind && a->w == b->w && a->h == b->h && a->format == b->format
                && a->usage == b->usage && memcmp(a->made, b->made, sizeof a->made) == 0
                && memcmp(a->fetch, b->fetch, sizeof a->fetch) == 0) {
                same++;
                total += refs[j];
                b->object = NULL;
            }
        }
        held_desc(&alive[i], desc, sizeof desc);
        sites_text(alive[i].made, HELD_SITES, made, sizeof made);
        size_t used = 0;
        fetched[0] = 0;
        for (unsigned k = 0; k < HELD_SITES && alive[i].fetch[k]; k++) {
            if (!where(alive[i].fetch[k], one, sizeof one)) continue;
            int w = snprintf(fetched + used, sizeof fetched - used, "%s%s (x%u)", used ? ", " : "", one, alive[i].fetches[k]);
            if (w < 0 || (size_t)w >= sizeof fetched - used) break;
            used += (size_t)w;
        }
        char count[24] = "";
        if (same > 1) snprintf(count, sizeof count, " x%u", same);
        log_line("  %s %s%s, %ld reference(s)%s%s%s%s", held_kinds[alive[i].kind], desc, count, total,
                 made[0] ? ": made by " : "", made,
                 fetched[0] ? (made[0] ? "; fetched by " : ": fetched by ") : "", fetched);
    }
    free(alive);
    free(refs);
}

#define CALL_ORIG(slot, type, ...) (((type)dev_orig[slot])(__VA_ARGS__))
/* The wrapper's caller: [ebp+4] is the return address, then `this` and the arguments. */
#define FRAME() ((void **)__builtin_frame_address(0) + 1)

static HRESULT WINAPI w_CreateAdditionalSwapChain(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp, IDirect3DSwapChain9 **out)
{
    HRESULT hr = CALL_ORIG(13, HRESULT (WINAPI *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *, IDirect3DSwapChain9 **), d, pp, out);
    if (SUCCEEDED(hr) && out && pp)
        held_made(*out, HELD_SWAPCHAIN, pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, 0, FRAME());
    return hr;
}

static HRESULT WINAPI w_Reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp)
{
    char stack[900];
    void *sites[6];
    sites_text(sites, stack_sites(FRAME(), dxvk_module, sites, 6), stack, sizeof stack);
    if (pp)
        log_line("Reset to %ux%u %s, from %s", pp->BackBufferWidth, pp->BackBufferHeight,
                 pp->Windowed ? "windowed" : "full screen", stack[0] ? stack : "?");
    HRESULT hr = CALL_ORIG(16, HRESULT (WINAPI *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *), d, pp);
    if (SUCCEEDED(hr))
        log_line("Reset succeeded");
    else
        held_report(hr);
    return hr;
}

static HRESULT WINAPI w_GetBackBuffer(IDirect3DDevice9 *d, UINT chain, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **out)
{
    HRESULT hr = CALL_ORIG(18, HRESULT (WINAPI *)(IDirect3DDevice9 *, UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **), d, chain, index, type, out);
    if (SUCCEEDED(hr) && out) held_fetched(*out, __builtin_return_address(0));
    return hr;
}

static HRESULT WINAPI w_CreateTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
                                      D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *shared)
{
    HRESULT hr = CALL_ORIG(23, HRESULT (WINAPI *)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *),
                           d, w, h, levels, usage, fmt, pool, out, shared);
    if (SUCCEEDED(hr) && out && pool == D3DPOOL_DEFAULT) held_made(*out, HELD_TEXTURE, w, h, fmt, usage, FRAME());
    return hr;
}

static HRESULT WINAPI w_CreateVolumeTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT depth, UINT levels, DWORD usage,
                                            D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9 **out, HANDLE *shared)
{
    HRESULT hr = CALL_ORIG(24, HRESULT (WINAPI *)(IDirect3DDevice9 *, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9 **, HANDLE *),
                           d, w, h, depth, levels, usage, fmt, pool, out, shared);
    if (SUCCEEDED(hr) && out && pool == D3DPOOL_DEFAULT) held_made(*out, HELD_VOLUME, w, h, fmt, usage, FRAME());
    return hr;
}

static HRESULT WINAPI w_CreateCubeTexture(IDirect3DDevice9 *d, UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt,
                                          D3DPOOL pool, IDirect3DCubeTexture9 **out, HANDLE *shared)
{
    HRESULT hr = CALL_ORIG(25, HRESULT (WINAPI *)(IDirect3DDevice9 *, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9 **, HANDLE *),
                           d, edge, levels, usage, fmt, pool, out, shared);
    if (SUCCEEDED(hr) && out && pool == D3DPOOL_DEFAULT) held_made(*out, HELD_CUBE, edge, edge, fmt, usage, FRAME());
    return hr;
}

static HRESULT WINAPI w_CreateVertexBuffer(IDirect3DDevice9 *d, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                           IDirect3DVertexBuffer9 **out, HANDLE *shared)
{
    HRESULT hr = CALL_ORIG(26, HRESULT (WINAPI *)(IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **, HANDLE *),
                           d, length, usage, fvf, pool, out, shared);
    if (SUCCEEDED(hr) && out && pool == D3DPOOL_DEFAULT) held_made(*out, HELD_VB, length, 0, fvf, usage, FRAME());
    return hr;
}

static HRESULT WINAPI w_CreateIndexBuffer(IDirect3DDevice9 *d, UINT length, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                          IDirect3DIndexBuffer9 **out, HANDLE *shared)
{
    HRESULT hr = CALL_ORIG(27, HRESULT (WINAPI *)(IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **, HANDLE *),
                           d, length, usage, fmt, pool, out, shared);
    if (SUCCEEDED(hr) && out && pool == D3DPOOL_DEFAULT) held_made(*out, HELD_IB, length, 0, fmt, usage, FRAME());
    return hr;
}

static HRESULT WINAPI w_CreateRenderTarget(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                           DWORD quality, BOOL lockable, IDirect3DSurface9 **out, HANDLE *shared)
{
    HRESULT hr = CALL_ORIG(28, HRESULT (WINAPI *)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *),
                           d, w, h, fmt, ms, quality, lockable, out, shared);
    if (SUCCEEDED(hr) && out) held_made(*out, HELD_RT, w, h, fmt, D3DUSAGE_RENDERTARGET, FRAME());
    return hr;
}

static HRESULT WINAPI w_CreateDepthStencilSurface(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                                  DWORD quality, BOOL discard, IDirect3DSurface9 **out, HANDLE *shared)
{
    HRESULT hr = CALL_ORIG(29, HRESULT (WINAPI *)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *),
                           d, w, h, fmt, ms, quality, discard, out, shared);
    if (SUCCEEDED(hr) && out) held_made(*out, HELD_DS, w, h, fmt, D3DUSAGE_DEPTHSTENCIL, FRAME());
    return hr;
}

static HRESULT WINAPI w_CreateOffscreenPlainSurface(IDirect3DDevice9 *d, UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool,
                                                    IDirect3DSurface9 **out, HANDLE *shared)
{
    HRESULT hr = CALL_ORIG(36, HRESULT (WINAPI *)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9 **, HANDLE *),
                           d, w, h, fmt, pool, out, shared);
    if (SUCCEEDED(hr) && out && pool == D3DPOOL_DEFAULT) held_made(*out, HELD_PLAIN, w, h, fmt, 0, FRAME());
    return hr;
}

static HRESULT WINAPI w_GetRenderTarget(IDirect3DDevice9 *d, DWORD index, IDirect3DSurface9 **out)
{
    HRESULT hr = CALL_ORIG(38, HRESULT (WINAPI *)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 **), d, index, out);
    if (SUCCEEDED(hr) && out) held_fetched(*out, __builtin_return_address(0));
    return hr;
}

static HRESULT WINAPI w_GetDepthStencilSurface(IDirect3DDevice9 *d, IDirect3DSurface9 **out)
{
    HRESULT hr = CALL_ORIG(40, HRESULT (WINAPI *)(IDirect3DDevice9 *, IDirect3DSurface9 **), d, out);
    if (SUCCEEDED(hr) && out) held_fetched(*out, __builtin_return_address(0));
    return hr;
}

static const struct { unsigned slot; void *wrapper; } dev_wraps[] = {
    { 13, (void *)w_CreateAdditionalSwapChain }, { 16, (void *)w_Reset }, { 18, (void *)w_GetBackBuffer },
    { 23, (void *)w_CreateTexture }, { 24, (void *)w_CreateVolumeTexture }, { 25, (void *)w_CreateCubeTexture },
    { 26, (void *)w_CreateVertexBuffer }, { 27, (void *)w_CreateIndexBuffer }, { 28, (void *)w_CreateRenderTarget },
    { 29, (void *)w_CreateDepthStencilSurface }, { 36, (void *)w_CreateOffscreenPlainSurface },
    { 38, (void *)w_GetRenderTarget }, { 40, (void *)w_GetDepthStencilSurface },
};

/* Wrap DXVK's device vtable, once, as soon as DXVK is loaded: before GTA
 * creates its device, so nothing made in D3DPOOL_DEFAULT is missed. */
static int track_resets(void)
{
    if (tracking) return 1;
    HMODULE mod = GetModuleHandleA("d3d9.dll");
    void **vt = mod ? find_vtable(mod, "N4dxvk12D3D9DeviceExE") : NULL;
    if (!vt) return 0;
    if (!(held = calloc(HELD_SLOTS, sizeof *held))) return 0;
    dxvk_module = mod;
    DWORD old;
    if (!VirtualProtect(vt, DEVICE_METHODS * sizeof *vt, PAGE_READWRITE, &old)) {
        free(held);
        held = NULL;
        return 0;
    }
    for (size_t i = 0; i < sizeof dev_wraps / sizeof *dev_wraps; i++) {
        dev_orig[dev_wraps[i].slot] = vt[dev_wraps[i].slot];   /* before the swap: a call in flight finds it */
        InterlockedExchange((LONG *)&vt[dev_wraps[i].slot], (LONG)(uintptr_t)dev_wraps[i].wrapper);
    }
    VirtualProtect(vt, DEVICE_METHODS * sizeof *vt, old, &old);
    tracking = 1;
    char at[160];
    log_line("watching Reset on DXVK's device (its Reset entry was %s): a failed one lists who holds D3DPOOL_DEFAULT resources",
             where(dev_orig[16], at, sizeof at) ? at : "?");
    return 1;
}

/* Hook a device: every method from another thread; under DXVK, deleted
 * resources too. Exported so the test harness can hand it a device. */
__declspec(dllexport) int d3d9watch_attach(void *device, HWND window);
__declspec(dllexport) int d3d9watch_attach(void *device, HWND window)
{
    if (ntables || !device) return 0;
    track_resets();   /* normally done by the watcher thread before the device exists */
    render_tid = GetWindowThreadProcessId(window, NULL);
    if (!render_tid) render_tid = GetCurrentThreadId();
    stubs = VirtualAlloc(NULL, MAX_TABLES * MAX_METHODS * STUB_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stubs) { log_line("cannot allocate the stubs"); return 0; }

    void **device_vtable = *(void ***)device;
    char at[160];
    const char *vtable_at = where(device_vtable[0], at, sizeof at);   /* before the hooks replace it */
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)device_vtable[0], &d3d9_module);
    unsigned resources = 0;
    HMODULE dxvk_mod = GetModuleHandleA("d3d9.dll");
    if (dxvk_mod && find_vtable(dxvk_mod, "N4dxvk12D3D9DeviceExE")) {
        for (size_t i = 0; i < sizeof dxvk_classes / sizeof *dxvk_classes; i++) {
            void **vt = find_vtable(dxvk_mod, dxvk_classes[i].rtti);
            unsigned count = dxvk_classes[i].iface == IF_TEXTURE ? 22 : dxvk_classes[i].iface == IF_BUFFER ? 14 : 17;
            if (vt && hook_table(dxvk_classes[i].name, vt, count, 1, dxvk_classes[i].iface))
                resources++;
            else
                log_line("no vtable for %s (%s)", dxvk_classes[i].name, vt ? "hook failed" : "not found");
        }
        dxvk = resources > 0;
    }
    if (!hook_table("IDirect3DDevice9", device_vtable, DEVICE_METHODS, 0, 0)) {
        log_line("could not hook the device's vtable");
        return 0;
    }
    log_line("watching the D3D9 device %p (vtable in %s), game thread %lu: %u device methods%s; %u DXVK resource vtables",
             device, vtable_at ? vtable_at : "?", render_tid, DEVICE_METHODS,
             dxvk ? ", resources checked for DXVK's deleted mark" : " (not DXVK: thread checks only)", resources);
    return 1;
}

/* A hooked vtable by its table name ("Texture2D (resource)"), for the tests. */
__declspec(dllexport) void **d3d9watch_vtable(const char *name);
__declspec(dllexport) void **d3d9watch_vtable(const char *name)
{
    for (unsigned i = 0; i < ntables; i++)
        if (strcmp(tables[i].name, name) == 0) return tables[i].vtable;
    return NULL;
}

__declspec(dllexport) long d3d9watch_reports(int kind);
__declspec(dllexport) long d3d9watch_reports(int kind)
{
    return kind >= 0 && kind < KIND_COUNT ? reports[kind] : -1;
}

/* GTA creates the device during startup, after plugins load. */
static DWORD WINAPI watch(void *unused)
{
    (void)unused;
    const unsigned char want[] = { 0x68, 0x28, 0x7C, 0xC9, 0x00 };   /* push 0xC97C28 */
    if (IsBadReadPtr((void *)GTA_CREATE_PUSH, sizeof want) || memcmp((void *)GTA_CREATE_PUSH, want, sizeof want) != 0) {
        log_line("not gta_sa.exe 1.0 US (no push 0xC97C28 at 0x%X); not watching", GTA_CREATE_PUSH);
        return 0;
    }
    track_resets();   /* gta_sa.exe imports d3d9.dll: DXVK is already loaded */
    for (int i = 0; i < 6000; i++) {   /* ten minutes */
        void *device = *(void *volatile *)GTA_DEVICE_SLOT;
        HWND window = *(HWND volatile *)GTA_WINDOW_SLOT;
        if (device && window) {
            Sleep(200);   /* let the device finish its first steps */
            d3d9watch_attach(device, window);
            return 0;
        }
        Sleep(100);
    }
    log_line("no D3D9 device after ten minutes; not watching");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&lock);
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
        char *slash = n ? strrchr(path, '\\') : NULL;
        if (slash && (size_t)(slash - path) + 20 < sizeof path) {
            /* the last run's log stays, as d3d9watch.prev.log: a crash's is
             * still there after the game is started again */
            char prev[MAX_PATH];
            strcpy(slash + 1, "d3d9watch.prev.log");
            strcpy(prev, path);
            strcpy(slash + 1, "d3d9watch.log");
            MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
            log_file = fopen(path, "w");
        }
        char no_auto[4];
        if (!GetEnvironmentVariableA("D3D9WATCH_NO_AUTO", no_auto, sizeof no_auto)) {
            HANDLE t = CreateThread(NULL, 0, watch, NULL, 0, NULL);
            if (t) CloseHandle(t);
        }
    }
    return TRUE;
}
