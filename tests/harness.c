/* Runs d3d9watch.asi against a real DXVK device under Wine.
 * usage: harness <folder holding d3d9watch.asi> <path to DXVK's d3d9.dll> */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

typedef int (*attach_fn)(void *, HWND);
typedef long (*reports_fn)(int);
typedef void **(*vtable_fn)(const char *);
enum { KIND_THREAD, KIND_DELETED_THIS, KIND_DELETED_ARG };

static int passed, failed;
static void ok(int cond, const char *what)
{
    if (cond) passed++; else { failed++; printf("  FAIL: %s\n", what); }
}

static IDirect3DDevice9 *dev;
static IDirect3DTexture9 *tex;

static DWORD WINAPI other_thread(void *unused)
{
    (void)unused;
    DWORD v;
    for (int i = 0; i < 5; i++) IDirect3DDevice9_GetRenderState(dev, D3DRS_ZENABLE, &v);
    for (int i = 0; i < 3; i++) IDirect3DTexture9_GetType(tex);
    return 0;
}

static double ns_per_call(void)
{
    LARGE_INTEGER f, a, b;
    DWORD v;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 1000000; i++) IDirect3DDevice9_GetRenderState(dev, D3DRS_ZENABLE, &v);
    QueryPerformanceCounter(&b);
    return (double)(b.QuadPart - a.QuadPart) * 1e9 / (double)f.QuadPart / 1e6;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: harness <dir> <d3d9.dll>\n"); return 2; }
    SetEnvironmentVariableA("D3D9WATCH_NO_AUTO", "1");
    HMODULE d3d9 = LoadLibraryA(argv[2]);
    if (!d3d9) { printf("cannot load %s (%lu)\n", argv[2], GetLastError()); return 2; }
    IDirect3D9 *(WINAPI *create)(UINT) = (void *)GetProcAddress(d3d9, "Direct3DCreate9");
    IDirect3D9 *d3d = create ? create(D3D_SDK_VERSION) : NULL;
    HWND w = CreateWindowA("STATIC", "d3d9watch test", WS_POPUP | WS_VISIBLE, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.hDeviceWindow = w;
    HRESULT hr = d3d ? IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev) : E_FAIL;
    if (FAILED(hr)) { printf("no D3D9 device (%08lX): is Vulkan available in this prefix?\n", (unsigned long)hr); return 2; }

    double before = ns_per_call();
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\d3d9watch.asi", argv[1]);
    HMODULE asi = LoadLibraryA(path);
    attach_fn attach = asi ? (attach_fn)GetProcAddress(asi, "d3d9watch_attach") : NULL;
    reports_fn reports = asi ? (reports_fn)GetProcAddress(asi, "d3d9watch_reports") : NULL;
    ok(attach && reports, "the plugin loads and exports its entry points");
    if (!attach || !reports) return 2;

    printf("== attach to a real DXVK device\n");
    ok(attach(dev, w) == 1, "attached");
    ok(attach(dev, w) == 0, "a second attach does nothing");

    printf("== ordinary use on the game's thread: nothing to report\n");
    ok(SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL)), "CreateTexture");
    IDirect3DVertexBuffer9 *vb = NULL;
    ok(SUCCEEDED(IDirect3DDevice9_CreateVertexBuffer(dev, 4096, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, NULL)), "CreateVertexBuffer");
    void *data;
    for (int i = 0; i < 100; i++) {
        IDirect3DVertexBuffer9_Lock(vb, 0, 0, &data, D3DLOCK_DISCARD);
        memset(data, i, 4096);
        IDirect3DVertexBuffer9_Unlock(vb);
        D3DLOCKED_RECT lr;
        IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0);
        IDirect3DTexture9_UnlockRect(tex, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000u | (DWORD)i, 1.0f, 0);
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex);
        IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, 16);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
        IDirect3DDevice9_EndScene(dev);
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    }
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DDevice9_SetStreamSource(dev, 0, NULL, 0, 0);
    ok(reports(KIND_THREAD) == 0 && reports(KIND_DELETED_THIS) == 0 && reports(KIND_DELETED_ARG) == 0,
       "100 frames of locks, binds and presents: no reports");

    printf("== calls from another thread\n");
    HANDLE t = CreateThread(NULL, 0, other_thread, NULL, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    ok(reports(KIND_THREAD) == 8, "5 device and 3 texture calls from another thread: 8 reports");

    printf("== a deleted texture: bound, and called\n");
    /* a texture as DXVK leaves it when it deletes one: the live object's
     * bytes with the deleted mark on its private refcount */
    static unsigned char ghost[0x400];
    memcpy(ghost, tex, sizeof ghost);
    ghost[0xB] |= 0x80;
    IDirect3DTexture9 *dead = (IDirect3DTexture9 *)ghost;
    IDirect3DDevice9_SetTexture(dev, 1000, (IDirect3DBaseTexture9 *)dead);   /* an invalid stage: DXVK returns at once */
    ok(reports(KIND_DELETED_ARG) == 1, "SetTexture handed a deleted texture: reported");
    ok(IDirect3DTexture9_GetType(dead) == D3DRTYPE_TEXTURE, "a method on it still reaches DXVK");
    ok(reports(KIND_DELETED_THIS) == 1, "and is reported as called on a deleted object");
    IDirect3DDevice9_SetTexture(dev, 1000, (IDirect3DBaseTexture9 *)tex);
    ok(reports(KIND_DELETED_ARG) == 1, "a live texture is not reported");

    printf("== a deleted texture whose vptr fell back to its base class, as DXVK's destructors leave it\n");
    vtable_fn vtable = (vtable_fn)GetProcAddress(asi, "d3d9watch_vtable");
    void **base_vt = vtable ? vtable("Texture2D (resource)") : NULL;
    ok(base_vt != NULL, "the base class's vtable is hooked");
    if (base_vt) {
        *(void ***)ghost = base_vt;
        IDirect3DTexture9_AddRef(dead);       /* ComObject's AddRef: only counts, on our copy */
        ok(reports(KIND_DELETED_THIS) == 2, "AddRef through it is reported as called on a deleted object");
    }

    double after = ns_per_call();
    printf("   GetRenderState: %.1f ns a call before, %.1f ns with the watchdog\n", before, after);

    IDirect3DVertexBuffer9_Release(vb);
    IDirect3DTexture9_Release(tex);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);

    /* the log names the caller */
    snprintf(path, sizeof path, "%s\\..\\d3d9watch.log", argv[1]);
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    strcpy(strrchr(exe, '\\') + 1, "d3d9watch.log");
    FILE *f = fopen(exe, "r");
    char line[2048];
    int named = 0, thread_lines = 0, deleted_lines = 0;
    while (f && fgets(line, sizeof line, f)) {
        printf("   log: %s", line);
        if (strstr(line, "another thread") && strstr(line, "harness.exe+0x")) { named++; thread_lines++; }
        if (strstr(line, "deleted") && strstr(line, "harness.exe+0x")) deleted_lines++;
    }
    if (f) fclose(f);
    ok(named > 0, "the log names the caller (harness.exe+offset)");
    ok(thread_lines == 2 + 2 + 1, "per call site at counts 1, 2, 4: 3 lines for the device call, 2 for the texture's");
    ok(deleted_lines == 3, "one line for each deleted-object report");

    printf("\nd3d9watch: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
