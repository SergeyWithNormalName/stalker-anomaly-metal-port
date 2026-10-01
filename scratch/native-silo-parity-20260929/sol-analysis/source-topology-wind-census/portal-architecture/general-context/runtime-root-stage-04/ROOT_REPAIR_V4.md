# Runtime observer stage v4 repair

Stage v4 is copied from the immutable v3 stage and applies only compile-boundary/context ABI fixes. In the ObjC++ scene translation unit it imports Cocoa first, then temporarily hides Cocoa's BSD macro and Carbon ThreadID typedef while xrCore and portal context headers are parsed. The legacy integer BOOL is token-renamed to XRAYPortalLegacyBOOL for that private include scope; interface/TRUE/FALSE are pushed and restored. This keeps Objective-C declarations outside the legacy macro state. The same Fvector/Fmatrix/u32 ABI is used by the R4 C++ path; the renamed aliases have the same underlying types.

The build03 log also showed `R4PortalContextFrameInput` was incomplete at `CRender::Calculate`, and scene code used fields absent from that context definition. The C++ Calculate TU now includes the full context header after `stdafx.h`; the value input adds view/projection, dimensions and captured SSA settings consumed by scene/Flora validation.

No build/game was run by the stage author. Remaining semantic risk: the ObjC++ TU must not rely on unremapped engine `BOOL` or `ThreadID` names after the private include block; verify full target compile for ODR/layout diagnostics. Runtime payload remains observer-only and does not change admission.
