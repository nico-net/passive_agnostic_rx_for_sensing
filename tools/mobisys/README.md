# tools/mobisys

Offline telemetry exporter for the passive receiver (stdlib + numpy; pyarrow and matplotlib optional).

    nr-uesoftmodem ... 2>&1 | python3 tools/mobisys/export.py --experiment-mode commercial --out DIR --host-label "DGX aarch64"
    python3 tools/mobisys/export.py --experiment-mode lab --out DIR --input run.log [--line-time unix]
    python3 tools/mobisys/groundtruth.py --rx LABDIR --gnb-log gnb.log        # lab only; gNB needs mac log level debug
    python3 tools/mobisys/plots.py --lab LABDIR --commercial COMDIR --out PLOTS [--sessions DIR DIR ...]
    python3 tools/mobisys/make_synthetic.py --work SCRATCH --examples         # SYNTHETIC fixture -> examples/
    cd tools/mobisys && python3 -m unittest -v

Commercial (default): stdin pipe only, 60-s windows, K=10, no RNTI/per-grant rows. Exit 0 ok, 2 input refused, 3 policy violation, 4 internal error.
