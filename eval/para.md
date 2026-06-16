  Phase 2 ist implementiert. In solver/engine.c:46 gibt es jetzt ASTROMETRY_INDEX_SHARD_TRACE; aktiv ist alles ausser leer/0/false/no/off. Nach der Indexauswahl loggt engine_run_job() Kandidatenzahl, finale Kandidatenreihenfolge,
  Indexname/-datei, ID, Scale-Range, Healpix/Himmelsabdeckung und selected/selection ab solver/engine.c:602.

  In solver/onefield.c:60 nutzt onefield_run() dieselbe Env-Option und loggt Kandidatenzahl, indexes_inparallel, mode=off sowie geplante und tatsaechliche Verarbeitungsreihenfolge ab solver/onefield.c:375. Der Shard-Modus selbst
  ist noch nicht implementiert; deshalb steht dort bewusst mode=off.

  Verifiziert:
```bash
  make -C solver astrometry-engine solve-field
  ASTROMETRY_INDEX_SHARD_TRACE=1 ./solver/solve-field --config demo/cfg --no-plots --overwrite --dir /tmp/solve-field-trace --out apod5 --scale-low 30 demo/apod5.xyls
  ASTROMETRY_INDEX_SHARD_TRACE=off ./solver/solve-field ...
  ./solver/solve-field ...
```
  Mit Trace erscheinen [index-shard]-Zeilen; ohne Trace und mit off erscheinen keine. make laeuft erfolgreich, gibt aber weiterhin die vorhandene git describe-Warnung fatal: No names found, cannot describe anything. aus.
  Unrelated untracked files aus dem vorherigen Stand sind weiter vorhanden und unveraendert.

    Phase 3 ist umgesetzt in solver/onefield.c:52:

  - Neuer interner Helper solve_fields_with_solver(onefield_t* bp, solver_t* sp, sip_t* verify_wcs, void* callback_userdata).
  - Bestehendes solve_fields(bp, verify_wcs) bleibt als Wrapper erhalten und ruft weiter den Originalpfad mit &(bp->solver) und bp als userdata auf.
  - Der verschobene Body nutzt fuer den aktiven Solver konsequent sp, inklusive solver_run(sp), solver_verify_sip_wcs(sp, ...), Callback-Feldern und Unsolved-Logging.
  - Gezielte Pruefung: im neuen Helper-Body gibt es keinen bp->solver-Zugriff mehr.

  Verifiziert:
```bash
  make -C solver astrometry-engine solve-field
  python3 util/compare_solve_field_runs.py /tmp/phase3-solve-fields-base /tmp/phase3-solve-fields-candidate --base apod5 --include-rdls --log-a /tmp/phase3-solve-fields-base/solve.log --log-b /tmp/phase3-solve-fields-candidate/
  solve.log
```
  Ergebnis: .solved, .wcs, .match, .corr, .rdls und normalisierte Logs matchen. Kein CLI-Verhalten geaendert, keine neue Parallelitaet. make zeigt weiterhin die bestehende git describe-Warnung fatal: No names found, cannot
  describe anything.

  ```bash
  docker run --rm \
    -v "$PWD:/work" \
    -v /tmp:/hosttmp \
    -w /work \
    astrometrynet/solver:test \
    bash -lc '
      set -e
      printf "%s\n" \
        "add_path /usr/local/data" \
        "index index-4119.fits" \
        "index index-4118.fits" \
        "index index-4117.fits" \
        "index index-4116.fits" \
        "index index-4115.fits" \
        "index index-4114.fits" \
        "index index-4113.fits" \
        "index index-4112.fits" \
        "index index-4111.fits" \
        "index index-4110.fits" \
        "index index-4109.fits" \
        "index index-4108.fits" \
        "index index-4107.fits" \
        > /tmp/indexes.cfg

      rm -rf /hosttmp/best-frame-solve

      eval/solve_best_blur_frame.sh \
        --images-dir /src/Astrometry-testing-data/data/20260303200006652 \
        --out-dir /hosttmp/best-frame-solve \
        --solve-workers 4 \
        --allow-best-below-threshold \
        --solve-field solve-field \
        -- \
        --config /tmp/indexes.cfg \
        --depth 10-20 \
        -L 2.5 -H 3.2 -u arcsecperpix
    '


  ```