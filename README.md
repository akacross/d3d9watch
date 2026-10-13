# d3d9watch

An ASI that catches code misusing GTA San Andreas's Direct3D 9 device, and
names it.

GTA creates its device without `D3DCREATE_MULTITHREADED`. RenderWare adds
the flag only when its "multithread safe" option is on (`gta_sa.exe`
0x7F6710), GTA never sets it, and the device has flags `0x50`. So any D3D9
call from another thread races the game. Calling a method on a texture or
buffer that has been deleted, or binding one, is just as bad. Under DXVK both
leave damage that surfaces much later and elsewhere: the DXVK allocator's free
list off by one, `d3d9.dll+0x1A63E9`, three times on 2026-10-11 and 10-12
(the analysis is in
[samp-openmp-linux](https://github.com/akacross/samp-openmp-linux/blob/main/docs/DXVK-CRASH.md)).
The dump shows the aftermath, never the culprit; this plugin is there for the
next time.

## What it reports

| report | meaning |
|---|---|
| `called from another thread` | a device, texture, surface or buffer method on a thread other than the game window's |
| `called on a deleted object` | a texture, surface or buffer method on an object DXVK has already deleted |
| `handed a deleted object` | `SetTexture`, `SetStreamSource`, `SetIndices`, `SetRenderTarget`, `SetDepthStencilSurface`, `StretchRect`, `UpdateSurface`, `UpdateTexture`, `ColorFill`, `GetRenderTargetData` or `GetFrontBufferData` given a deleted resource |

Each line names the method, the thread, the caller's `module+offset` and the
next few return addresses outside `d3d9.dll`. It is written once per call
site and kind, then again at counts 2, 4, 8 and so on:

```
IDirect3DDevice9::SetTexture handed a deleted object (argument 2: 0A1B2C3D), thread 1468 (game 1468), x1: somemod.asi+0x1234 < samp.dll+0x7E0D < ...
```

Lines go to `d3d9watch.log` next to `gta_sa.exe`. The previous run's log is
kept as `d3d9watch.prev.log`, so a crash's log survives starting the game
again. They also go through `OutputDebugString`, which sampcmd
`--monitor` writes to `sampcmd.log`. Nothing is changed: every call goes on to
the original.

## How

A watcher thread waits for the device RenderWare stores at `0xC97C28` (the
address `gta_sa.exe` 1.0 US passes to `CreateDevice`; checked by the
`push 0xC97C28` at 0x7F6761). The game thread is the one that owns the
window at `0xC97C1C`.

- **Device.** Every one of the 119 `IDirect3DDevice9` methods is hooked
  through the device's vtable, chaining to whatever was there (SA-MP's,
  open.mp's and MoonLoader's hooks included). Each entry gets a stub:
  `mov eax, fs:[24h]` (the thread id), compared with the game thread, then
  `jmp` to the original. About 0.3 ns a call in the harness, against a real
  DXVK device. Only the methods that take resources always go to the C check.
- **Resources (DXVK only).** DXVK keeps RTTI, so its vtables are found by
  type name (`N4dxvk13D3D9Texture2DE`, ...). That covers the six classes
  (2D, cube and volume textures, surfaces, vertex and index buffers) and
  their base classes `D3D9BaseTexture<>`, `D3D9Buffer<>`, `D3D9Subresource<>`
  and `D3D9Resource<>`: 18 vtables. A base class's vtable matters because
  DXVK's destructors leave a deleted object's vptr on it. Its empty slots
  (MinGW writes 0 for methods it does not implement) are left alone.
  - The stub also tests `this` for the deleted mark: DXVK's
    `ComObject::ReleasePrivate` adds 0x80000000 to the private refcount (+8)
    just before it deletes the object.
- Without DXVK (wined3d, native) only the device's thread checks run.

## Build, test, deploy

```bash
./build.sh                                  # build/d3d9watch.asi (i686 MinGW)
WINEPREFIX=... ./build.sh test [d3d9.dll]   # harness against a real DXVK d3d9.dll (needs Vulkan)
./build.sh --deploy /path/to/game           # copy the ASI into the game folder
```

The harness loads DXVK's `d3d9.dll` (by default the game folder's), creates
a device and attaches the plugin through its `d3d9watch_attach` export. It
checks 16 things:

- 100 frames of locks, binds and presents on the game thread: nothing
  reported;
- device and texture calls from another thread: caught and attributed to
  `harness.exe+offset`, with lines at counts 1, 2 and 4;
- a deleted texture handed to `SetTexture`, or one with a method called on it:
  caught;
- the same through the base-class vtable a destructor leaves: caught;
- a live texture: not reported.

## License

MIT.
