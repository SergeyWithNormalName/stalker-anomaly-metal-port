# Unchanged-stock empty-visual startup capture

## Result

The corrected WineDbg run stopped before the renderer existence-query call at `CGameObject::net_Spawn` RVA `0x1f9938` in the exact stock executable (`c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3`). The RIP assertion returned `0x1401f9938`. At that stop RDX was `0x354504`, and the explicitly evaluated predicate `*((char*)$rdx)==0` returned `0x1`. This confirms the incoming visual name is a non-null pointer to an empty C string at that stock callsite.

The caller-frame registers at the verified stop were R13=`0x95ee8b00` (the incoming entity pointer) and RSI=`0x99722cb0` (the CGameObject). The `bt` output independently showed `CLevel::g_sv_Spawn(..., E=0x95ee8b00)` directly below the net-spawn chain, confirming the entity-pointer correlation. The fallback section handle loaded from `this+0xec` was `0x04bdc980`; reading its string bytes beginning at `handle+0x14` produced `lights_hanging_lamp`.

This identifies the empty visual query and the section key used by the fallback path for this spawn. It does **not** establish the entity's numeric ID or server-side `s_name`; the section string is not assumed to be a unique entity identity. No CSE field offsets were guessed. The stop occurred before executing the query, so this capture does not independently prove that this same call proceeded into `CModelPool::Create`; the saved crash stack separately contains that path.

## Run discipline

The controller used one carriage return per command. An offline minidump test had shown a blank CR repeats the prior WineDbg command; the invalid debug03 attempt was marked unusable in `debug03/REVIEW.md`. Debug04 asserted RIP and the empty-byte predicate before reading R13, RSI, or the section handle. It then quit WineDbg and shut down the dedicated prefix. The full compact command transcript is in `transcript.txt`.
