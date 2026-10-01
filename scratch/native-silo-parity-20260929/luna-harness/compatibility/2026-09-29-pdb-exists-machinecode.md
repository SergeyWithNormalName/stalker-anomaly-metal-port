# Windows CModelPool::Exists machine-code audit (offline/read-only)

## Result

The pinned May 15 Silo executable has no empty/null visual guard in its `CModelPool::Exists` machine code. Its empty-string path reaches the `CModelPool::Create` call with the original `N` argument. This is a concrete compatibility fault path consistent with the failed `FileDownload` empty-path stack. It does not prove the unnamed failing entity supplied the empty name; that argument is absent from the fatal log.

## Identity and method

Used a standard-library-only reader at `read_pdb_pub32.py` to parse MSF 7.00 block/directory data, DBI stream 3, CodeView `S_PUB32` records, and PE RSDS debug identity. No packages were installed. Reader found PDB GUID `b29860d9-ae5d-45d5-a5e0-16d672256fc0`, age `1`, and matching values in the EXE CodeView record. The PDB info header is VC70 (`20000404`); the DBI header identifies public stream 2220 and symbol-record stream 2221. It extracted these public symbols from section 1 (`.text`):

- `?Exists@CModelPool@@QEAA_NPEBD@Z`: RVA `0x00cd5e60`, VA `0x140cd5e60`
- `?Create@CModelPool@@QEAAPEAVdxRender_Visual@@PEBDPEAVIReader@@_N@Z`: RVA `0x00cd4fb0`, VA `0x140cd4fb0`
- `?g_sv_Spawn@CLevel@@QEAAXPEAVCSE_Abstract@@@Z`: RVA `0x004352d0`, VA `0x1404352d0`
- `?cl_Process_Spawn@CLevel@@QEAAXAEAVNET_Packet@@@Z`: RVA `0x00434e00`, VA `0x140434e00`
- `?net_Spawn@CGameObject@@UEAAHPEAVCSE_Abstract@@@Z`: RVA `0x001f9730`, VA `0x1401f9730`

The prior native-PDB LLDB symbol lookup crashed in LLVM's PDB inline-site parser. To avoid that parser, I did not load the PDB into LLDB again. I used the verified public VAs and ran LLDB batch disassembly against the PE only. No process was started/attached and no binary was modified.

## Machine-code evidence

`CModelPool::Exists` at `0x140cd5e60` starts by saving the Windows x64 `N` argument from RDX into R12, then copies/normalizes it, strips an extension if any, hashes/looks up the resulting string and probes the model maps. In this 0x390-byte function range there is no test of the incoming pointer or `N[0]` before those operations. At `0x140cd6171` it zeroes the `IReader*` and assert arguments, reloads original `N` from R12 into RDX, sets RCX to the model pool (`this`), and calls `0x140cd4fb0` (`CModelPool::Create`) at `0x140cd6177`. That matches the source `Exists()` miss path's `Create(N, 0, false)` call and shows an empty original name is not rejected in this Windows binary.

The `CLevel::g_sv_Spawn` function begins at `0x1404352d0`; it calls an object-creation routine and then invokes the created object's `net_Spawn` through its vtable at `0x140435374` (`call *0x60(%rax)`). That matches source `Level_network_spawn.cpp`, where `g_sv_Spawn` does `Objects.Create(*E->s_name)` then `O->net_Spawn(E)`. Current `CGameObject::net_Spawn()` calls `Render->models_Exists(visualName)` before the configured fallback at `src/xrGame/GameObject.cpp:281-288`. Thus the path exposed by the failure is: spawned object dispatch -> visual existence query -> empty-name fallthrough in old model pool -> file open from `Create`.

Current dirty source has the explicit guard `if (!N || !N[0]) return false;` in `src/Layers/xrRender/ModelPool.cpp:519-529`; it is absent from the pinned May 15 executable's disassembly. The full old and current-clone Windows executable hashes were previously verified identical (`c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3`).

## Limits

The fatal log identifies `FileDownload` with an empty filename and `CModelPool::Create -> CLevel::g_sv_Spawn -> CLevel::cl_Process_Spawn`, but supplies no model string/entity ID. This audit proves an unsafe `Exists(empty) -> Create(empty)` code path exists; it does not prove that exact input triggered this particular crash. No candidate entity name is inferred. No runtime attempt is warranted from this evidence alone.

## Format references (primary LLVM documentation)

- [MSF file format and stream directory](https://llvm.org/docs/PDB/MsfFile.html)
- [PDB/DBI stream fields and public/symbol-record stream indices](https://llvm.org/docs/PDB/DbiStream.html)
- [PDB GUID/age matching to PE CodeView identity](https://llvm.org/docs/PDB/PdbStream.html)
- [CodeView S_PUB32 record definition](https://llvm.org/docs/PDB/CodeViewSymbols.html)
