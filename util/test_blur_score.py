# This file is part of the Astrometry.net suite.
# Licensed under a 3-clause BSD style license - see LICENSE
import os
import tempfile
import unittest
import numpy as np

from .blur_score import (
    InsufficientSources,
    compute_blur_score,
    measure_psf,
    score_from_psf,
)


def _gaussian_star(shape, x0, y0, fwhm, amp=2000.0, ellipticity=0.0):
    sigma = fwhm / 2.3548
    sx = sigma
    sy = sigma * (1.0 - ellipticity)
    y, x = np.mgrid[: shape[0], : shape[1]]
    return amp * np.exp(-(((x - x0) ** 2) / (2 * sx ** 2)
                         + ((y - y0) ** 2) / (2 * sy ** 2)))


def _starfield(n_stars, fwhm, shape=(512, 512), ellipticity=0.0,
               noise_sigma=5.0, sky=100.0, seed=0):
    rng = np.random.default_rng(seed)
    img = rng.normal(sky, noise_sigma, shape).astype(np.float32)
    margin = 25
    for _ in range(n_stars):
        x0 = rng.uniform(margin, shape[1] - margin)
        y0 = rng.uniform(margin, shape[0] - margin)
        amp = rng.uniform(800, 6000)
        img += _gaussian_star(shape, x0, y0, fwhm,
                              amp=amp, ellipticity=ellipticity).astype(np.float32)
    return img


class TestBlurScore(unittest.TestCase):

    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmpdir.cleanup)

    def _save(self, name, arr):
        from astropy.io import fits
        path = os.path.join(self.tmpdir.name, name + ".fits")
        fits.PrimaryHDU(arr.astype(np.float32)).writeto(path, overwrite=True)
        return path

    def test_score_in_range(self):
        path = self._save("starfield", _starfield(40, fwhm=3.0, seed=1))
        s = compute_blur_score(path)
        self.assertGreaterEqual(s, 0.0)
        self.assertLessEqual(s, 1.0)

    def test_sharp_better_than_blurred(self):
        sharp = self._save("sharp", _starfield(40, fwhm=3.0, seed=2))
        blurry = self._save("blurry", _starfield(40, fwhm=8.0, seed=2))
        self.assertGreater(compute_blur_score(sharp),
                           compute_blur_score(blurry))

    def test_round_better_than_streaked(self):
        round_p = self._save("round", _starfield(40, fwhm=3.0, ellipticity=0.0, seed=3))
        streaked = self._save("streaked", _starfield(40, fwhm=3.0, ellipticity=0.6, seed=3))
        self.assertGreater(compute_blur_score(round_p),
                           compute_blur_score(streaked))

    def test_determinism(self):
        path = self._save("det", _starfield(40, fwhm=3.0, seed=42))
        self.assertEqual(compute_blur_score(path), compute_blur_score(path))

    def test_insufficient_sources_raises(self):
        rng = np.random.default_rng(0)
        path = self._save("noise", rng.normal(100, 5, (256, 256)).astype(np.float32))
        with self.assertRaises(InsufficientSources):
            measure_psf(path)

    def test_score_from_psf_formula(self):
        # Perfect: target FWHM, round, saturated count
        self.assertEqual(score_from_psf(3.0, 0.0, 10, target_fwhm=3.0), 1.0)
        # FWHM 2x target → halves the score
        self.assertAlmostEqual(score_from_psf(6.0, 0.0, 10, target_fwhm=3.0), 0.5)
        # Heavy ellipticity → halves the score
        self.assertAlmostEqual(score_from_psf(3.0, 0.5, 10, target_fwhm=3.0), 0.5)
        # Few sources → linear penalty
        self.assertAlmostEqual(score_from_psf(3.0, 0.0, 5, target_fwhm=3.0), 0.5)
        # Degenerate FWHM
        self.assertEqual(score_from_psf(0.0, 0.0, 10, target_fwhm=3.0), 0.0)


if __name__ == "__main__":
    unittest.main()
