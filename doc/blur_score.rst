.. _blur_score:

Blur Score
==========

The ``util.blur_score`` module produces a single scalar in ``[0, 1]`` that
quantifies how sharp an astronomical image is. ``1.0`` corresponds to
pinpoint stars at the target seeing, ``0.0`` corresponds to a severely
blurred or unusable frame.

Source code: :file:`util/blur_score.py`.

Approach
--------

Blur is measured **indirectly via the stellar PSF**, not by running an
edge filter (Laplacian, Sobel, FFT, ...) on the image pixels. The
rationale is that astronomical frames are dominated by sky background
with sparse point sources, so the relevant question is "how wide and how
elongated are the stars?", which a generic image-sharpness operator
answers poorly.

The pipeline is:

1. **Source extraction.** ``source-extractor`` (SExtractor) is invoked
   with a 5x5 Gaussian convolution filter
   (``/usr/share/source-extractor/gauss_3.0_5x5.conv``) at
   ``DETECT_THRESH = 5.0 sigma`` and ``DETECT_MINAREA = 5`` pixels.
   For each detection it returns ``FWHM_IMAGE``, ``ELLIPTICITY``,
   ``FLAGS``, ``FLUX_AUTO``.
2. **Cleaning.** Keep only detections with ``FLAGS == 0`` (no blending,
   no saturation, no truncation) and with FWHM in
   ``FWHM_RANGE = (1.0, 30.0)`` pixels. Below 1 px is undersampled
   noise; above 30 px is almost certainly a galaxy, hot pixel cluster,
   or a satellite trail residue.
3. **Bright subset.** Take the brightest ``BRIGHT_QUANTILE = 0.7``
   (top 30% by ``FLUX_AUTO``). Faint detections sit close to the noise
   floor and their FWHM is dominated by photon noise rather than by
   the true PSF.
4. **Aggregation.** Take the **median** FWHM and the **median**
   ellipticity over the bright subset.
5. **Scoring.** Map the three numbers ``(median_fwhm,
   median_ellipticity, n_bright)`` to the final score.

A frame is rejected with :class:`InsufficientSources` when fewer than
``MIN_SOURCES = 10`` clean detections or fewer than ``MIN_BRIGHT = 4``
bright detections are found. This is a "cannot decide" outcome, not
"score = 0".

Calculation rule
----------------

The score is the product of three independent factors, each clipped to
``[0, 1]``::

    score = fwhm_score * shape_weight * count_weight

where

============= ==================================================== ==================================
Factor        Formula                                              What it penalises
============= ==================================================== ==================================
fwhm_score    ``min(1.0, target_fwhm / median_fwhm)``              Wide PSFs from defocus or seeing
shape_weight  ``max(0.0, 1.0 - median_ellipticity)``               Tracking-error streaks
count_weight  ``min(1.0, n_bright / COUNT_SATURATION)``            Loss of faint stars from blur
============= ==================================================== ==================================

Default constants:

* ``target_fwhm = DEFAULT_TARGET_FWHM = 3.0`` pixels - the FWHM at which
  ``fwhm_score`` saturates. Any image sharper than 3 px gets the full
  weight; doubling the FWHM (6 px) halves the score; tripling it (9 px)
  drops it to one third.
* ``COUNT_SATURATION = 10`` - the number of bright sources at which
  ``count_weight`` saturates. With only 5 bright sources the score is
  multiplied by 0.5.
* If the measured FWHM is ``0`` (degenerate), ``fwhm_score`` is set to
  ``0`` to avoid a divide-by-zero.

Worked examples (from :file:`util/test_blur_score.py`):

* Perfect frame: ``fwhm = 3.0``, ``ecc = 0.0``, ``n_bright = 10``
  -> ``1.0 * 1.0 * 1.0 = 1.0``.
* Double FWHM: ``fwhm = 6.0``, ``ecc = 0.0``, ``n_bright = 10``
  -> ``0.5 * 1.0 * 1.0 = 0.5``.
* Streaked: ``fwhm = 3.0``, ``ecc = 0.5``, ``n_bright = 10``
  -> ``1.0 * 0.5 * 1.0 = 0.5``.
* Sparse: ``fwhm = 3.0``, ``ecc = 0.0``, ``n_bright = 5``
  -> ``1.0 * 1.0 * 0.5 = 0.5``.

Because the factors multiply, *any* single bad factor pulls the whole
score down: a frame with razor-sharp stars but ``ecc = 0.9`` (heavy
trailing) cannot score above ``0.1``.

Outlier behaviour
-----------------

**The aggregator is the median, so a single outlier cannot dominate the
score.** This is the central robustness property of the metric.

Concretely:

* The median of the bright subset is unaffected by up to ~50% of
  contaminating values - even a very wide blob or a single hot pixel
  cluster that slips past the ``FLAGS == 0`` filter shifts the median by
  at most one rank position.
* Satellite trails, cosmic rays, edge artefacts and galaxies normally
  fail one of the SExtractor flags (blended, saturated, truncated) or
  fall outside ``FWHM_RANGE``, so they never enter the median in the
  first place. Even if one slips through, it is one sample among
  ``n_bright >= 4``.
* The ``BRIGHT_QUANTILE`` cut also removes the long tail of faint,
  noisy detections whose FWHM scatter is large - those are not outliers
  but a noise-driven population that would otherwise bias an arithmetic
  mean upward.

What the metric is **not** robust to:

* **Global blur** affecting all stars equally - this is what the metric
  is *supposed* to detect, not an outlier.
* **A frame that is mostly blurred plus a few sharp stars in one
  corner.** The median tracks the majority, so the score will reflect
  the blurred majority, not the sharp minority. This is intentional.
* **Fewer than** ``MIN_SOURCES = 10`` **clean detections.** In that
  regime the median is computed on too few samples to be statistically
  stable, and the module raises :class:`InsufficientSources` rather
  than returning a number that one outlier could swing.

If a mean were used instead of the median, a single 30 px FWHM blob
among nine 3 px stars would inflate the average FWHM from 3.0 to 5.7 px
and roughly halve the score; with the median it stays at 3.0 and the
score is unchanged. This is the concrete reason for the design choice.

Public API
----------

.. py:function:: compute_blur_score(image, target_fwhm=3.0, **kwargs)

   Return the blur score in ``[0, 1]``. ``image`` is a path
   (FITS / PNG / JPEG / TIFF) or a 2-D / 3-D numpy array.
   Raises :class:`InsufficientSources` if the frame has too few clean
   detections.

.. py:function:: measure_psf(image, bright_quantile=0.7, **kwargs)

   Return ``(median_fwhm, median_ellipticity, n_bright_sources)`` for
   the bright subset. Useful when the caller wants the raw PSF numbers
   instead of (or alongside) the score.

.. py:function:: score_from_psf(fwhm, ecc, n_bright, target_fwhm)

   Apply the scoring formula above to already-measured numbers. Pure
   function, no I/O.

.. py:exception:: InsufficientSources

   Raised by :func:`measure_psf` and :func:`compute_blur_score` when
   fewer than ``MIN_SOURCES`` clean detections or fewer than
   ``MIN_BRIGHT`` bright detections are available.

Command-line use
----------------

::

    python -m util.blur_score path/to/image.fits
    # -> 0.873

    python -m util.blur_score path/to/image.fits --raw
    # -> fwhm=3.142 ellipticity=0.087 n_sources=27

Exit code ``2`` is returned (with an explanatory message on stderr)
when too few sources are detected.

Batch benchmarking is available via :file:`util/blur_score_benchmark.py`,
which walks a directory tree and writes a CSV with columns
``image, fwhm, ellipticity, n_sources, score``. Frames that raise
:class:`InsufficientSources` produce ``nan`` rows rather than aborting
the run.
