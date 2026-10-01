import csv
import datetime
import json
from pathlib import Path
import tempfile
import unittest
from summarize_run import summarize, distribution


class MeasurementTests(unittest.TestCase):
    def fixture(self, out, gap=False, pid=123):
        fields = ['engine_frame','wall_ms','epoch_s','phase','camera_x','camera_y','camera_z',
                  'dir_x','dir_y','dir_z','device_time_factor','game_time_factor','width','height',
                  'level','weather','calendar']
        with (out/'phases.csv.device.csv').open('w') as f:
            w=csv.writer(f)
            w.writerow(fields)
            for i in range(301):
                w.writerow([i,round(i*1000/30),100+i//30,1,0,0,0,0,0,1,1,6,1920,1200,'test','clear','fixed'])
        (out/'owned-pid.txt').write_text('123')
        def stamp(t):
            return datetime.datetime.fromtimestamp(t,datetime.timezone.utc).isoformat()
        rows=[]
        for i in range(300):
            if gap and i == 120:
                continue
            rows.append({'Start Date':stamp(100+i/30),'End Date':stamp(100+(i+1)/30),
                         'Duration MCT Seconds':1/30,'In-Flight Drawable Info':
                         {'MetalFX Enabled':False,'MetalFX Frame Interpolator Enabled':False}})
        (out/'metal-timeline.json').write_text(json.dumps([{'PID':pid,'Layers':
                                             [{'On-Glass Drawable Timeline':rows}]}]))

    def test_common_display_fps_and_callback_are_separate(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)
            self.fixture(out)
            r=summarize(out)
            self.assertTrue(r['displayed']['valid'])
            self.assertAlmostEqual(r['displayed']['mean_fps'],30,places=6)
            self.assertAlmostEqual(r['displayed']['intervals_ms']['p50'],1000/30,places=6)
            self.assertEqual(r['callback']['frame_gap_count'],0)

    def test_missing_display_frame_and_wrong_process_reject_claim(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)
            self.fixture(out,gap=True)
            self.assertFalse(summarize(out)['displayed']['valid'])
            self.fixture(out,pid=999)
            self.assertFalse(summarize(out)['displayed']['valid'])

    def test_one_percent_low_includes_long_stall(self):
        stats=distribution([10]*99+[1000])
        self.assertEqual(stats['one_percent_low_fps'],1)
        self.assertEqual(stats['p50'],10)


if __name__ == '__main__':
    unittest.main()
