#!/usr/bin/env python3
"""Join captured source-camera survivors to frame-700 portal candidate flags."""
import csv, hashlib, json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[7]
STAGE=ROOT/'scratch/native-silo-parity-20260929'
OUT=Path(__file__).resolve().parent
ARMS=('indoor','outdoor')
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    rows={}
    inputs=[]
    for arm in ARMS:
        run=STAGE/f'runs/portal-pixel-oracle-{arm}-07/native/output'
        topology=run/'source-topology.json'
        pairp=run/'main-flora-pixel-pair/main-flora-source-vs-portals-frame-700.json'
        idp=run/'main-flora-ids/main-flora-ids-frame-700.csv'
        top=json.loads(topology.read_text()); pair=json.loads(pairp.read_text())
        with idp.open(newline='') as f: idrows=list(csv.DictReader(line for line in f if not line.startswith(('schema,','engine_frame,','extent,','admitted,','submitted,','culled,','view_bits,','projection_bits,','eye_bits,','ssa_settings_bits,','encode_complete,'))))
        # Restrict to ordinary source survivors: adapter writes IDs after its exact camera clip.
        ids=[int(r['visual_id']) for r in idrows]
        topo=top['flora_ids']; topo_ids=[int(r['id']) for r in topo]
        if len(ids)!=pair['source_draws'] or len(topo_ids)!=len(ids) or len(set(ids))!=len(ids) or set(ids)!=set(topo_ids):
            raise ValueError(f'{arm}: source survivor ID join is not exact')
        if pair['frame']!=700 or top['frame']!=700 or top['generation']!=pair['portal_generation'] or not top['view_match'] or not pair['source_order_full_transform_match']:
            raise ValueError(f'{arm}: frame/view/generation qualification failed')
        if top['geometry_joined_ids']!=len(topo_ids) or top['geometry_join_unknown_ids']!=0:
            raise ValueError(f'{arm}: source graph join does not match observed survivors')
        survivor_raw=sum(int(r['raw_visible'])==0 for r in topo)
        raw=int(pair['raw_portal_candidates'])
        unknown=int(pair['candidate_wind_bound_unknown'])
        owner_unknown=int(pair['candidate_owner_frustum_unknown'])
        excluded=int(pair['candidate_excluded_id_count'])
        skipped=int(pair['candidate_actually_skipped'])
        admitted=int(idp.read_text().splitlines()[3].split(',')[1])
        culled=int(idp.read_text().splitlines()[5].split(',')[1])
        submitted=int(idp.read_text().splitlines()[4].split(',')[1])
        if submitted!=len(ids) or admitted!=submitted+culled or admitted!=11663:
            raise ValueError(f'{arm}: prepared/source inventory accounting mismatch')
        offscreen_raw=raw-survivor_raw
        if offscreen_raw<0 or excluded<skipped or skipped>survivor_raw:
            raise ValueError(f'{arm}: impossible subset counts')
        rows[arm]={
            'prepared_rest_records':admitted,'camera_survivors':submitted,'camera_culled':culled,
            'raw_portal_candidates_wind_compute_calls':raw,
            'raw_portal_candidates_among_camera_survivors':survivor_raw,
            'raw_portal_candidates_camera_clipped_before_draw':offscreen_raw,
            'wind_compute_calls_saved_by_lazy_after_clip':offscreen_raw,
            'wind_compute_calls_after_lazy_camera_survivor_filter':survivor_raw,
            'wind_compute_calls_reduction_fraction':offscreen_raw/raw,
            'candidate_prepared_exclusions':excluded,
            'prepared_exclusions_already_camera_clipped':excluded-skipped,
            'actual_post_clip_skips':skipped,
            'wind_unknown_total':unknown,'owner_unknown_total':owner_unknown,
            'owner_loop_record_count_current_lower_bound':max(0,raw-unknown-owner_unknown),
            'owner_loop_record_count_current_upper_bound':raw,
            'owner_loop_record_count_offscreen_lower_bound':max(0,offscreen_raw-unknown-owner_unknown),
            'owner_loop_record_count_offscreen_upper_bound':offscreen_raw,
            'observer_complete_population':bool(top['complete'] and top['geometry_join_complete']),
            'observer_joined_survivors':top['geometry_joined_ids'],
            'observer_contract_expected_ids':5316,
            'observer_contract_gap_from_pinned_5316':max(0,5316-top['geometry_joined_ids']),
        }
        inputs += [topology,pairp,idp]
    inputs.append(Path(__file__))
    result={'schema':'perframe-portal-camera-clip-opportunity/v1','method':'exact join of stage-07 frame-700 source camera-survivor IDs to same-frame source-topology raw_visible flags; raw candidate count is immediately before one wind compute in stage-10 helper','poses':rows,'inputs_sha256':{str(p.relative_to(ROOT)):sha(p) for p in inputs}}
    source7=STAGE/'sol-analysis/source-topology-wind-census/portal-architecture/general-context/runtime-root-stage-07/stage/src/Layers/xrRenderMetal/MetalR4EngineScene.mm'
    source10=STAGE/'sol-analysis/source-topology-wind-census/portal-architecture/general-context/runtime-root-stage-10/stage/src/Layers/xrRenderMetal/MetalR4EngineScene.mm'
    result['candidate_source_sha256']={str(p.relative_to(ROOT)):sha(p) for p in (source7,source10)}
    out=OUT/'camera-clip-opportunity.json'; out.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(rows,indent=2))
if __name__=='__main__': main()
