# UI observation overlaps this screen

CUA selected the already-running candidate-v9-r1 application during this run. The getApp operation took about 37 seconds, and a subsequent screenshot caught the window while the harness was quitting. No extra Anomaly process remained after the owned run.

This UI observation overlaps the measurement window, so on-glass results from this run are excluded from performance acceptance despite a continuous OS timeline. Callback cadence was 19.0179 FPS and has no useful positive signal versus the earlier ordinary same-binary 19.1458 FPS control. No gain or statistically reliable loss is claimed.

Resource dedup remains off. This repeats the earlier S2 absence of useful signal; do not build more cache logic around this first screen. Any later performance qualification must use an ordinary pair without CUA, CPU sampling, builds or other hardware profiling during its measurement window.

Future visual observations should occur only in an extended post-measurement tail. The captured closing-window image is not visual or motion acceptance.
