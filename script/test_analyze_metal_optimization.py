import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from analyze_metal_optimization import compare_runs, binding_summary

class AdmissionTest(unittest.TestCase):
    def result(self, **changes):
        value = dict(engine_exit_status=0, scenario_result='OK', binary_sha256='binary',
                     shader_receipt_set_sha256='shaders', save_sha256='save',
                     engine_extent=[1920,1080], fixture='S4', run='test')
        value.update(changes)
        return value

    def test_rejects_failed_or_incomparable_runs(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            paths = [root/'a',root/'b']
            for p in paths:
                p.mkdir();(p/'provenance.json').write_text(json.dumps({'profile': {'user_ltx_sha256': 'config'}}))
            for difference in [dict(engine_exit_status=139), dict(scenario_result='FAILED'),
                               dict(binary_sha256='different'), dict(engine_extent=[960,540]),
                               dict(save_sha256=''), dict(shader_receipt_set_sha256='other')]:
                with self.subTest(difference=difference), patch('analyze_metal_optimization.summarize_run', side_effect=[self.result(), self.result(**difference)]):
                    with self.assertRaises(ValueError):compare_runs(paths,30)
            (paths[1]/'provenance.json').write_text(json.dumps({'profile': {'user_ltx_sha256': 'different-config'}}))
            with patch('analyze_metal_optimization.summarize_run', side_effect=[self.result(),self.result()]):
                with self.assertRaisesRegex(ValueError,'configuration'):compare_runs(paths,30)

    def test_failed_profile_never_used(self):
        with patch('analyze_metal_optimization.summarize_run', return_value=self.result(engine_exit_status=134)):
            with self.assertRaises(ValueError):binding_summary(Path('missing'),Path('missing'),30)

if __name__ == '__main__':unittest.main()
