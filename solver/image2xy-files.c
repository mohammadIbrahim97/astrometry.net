/*
 # This file is part of the Astrometry.net suite.
 # Licensed under a 3-clause BSD style license - see LICENSE
 */

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <math.h>

#include "os-features.h"
#include "image2xy-files.h"
#include "image2xy.h"
#include "fitsio.h"
#include "ioutils.h"
#include "simplexy.h"
#include "errors.h"
#include "log.h"
#include "cfitsutils.h"
#include "dimage.h"
#include "mathutil.h"

#ifdef HAVE_SEP
#include <sep.h>
#endif

static int source_table_check_status(int status, const char* msg) {
    if (status) {
        cfitserr(status);
        ERROR("%s", msg);
        return -1;
    }
    return 0;
}

#define SOURCE_TABLE_CHECK(msg)                         \
do {                                                    \
    if (source_table_check_status(*status, msg))        \
        return -1;                                      \
} while (0)

static int write_source_table(fitsfile* ofptr, const simplexy_t* params,
                              int src_ext, int imagew, int imageh,
                              const char* backend, int* status) {
    char* ttype[] = {"X", "Y", "FLUX", "BACKGROUND",
                     "FWHM_IMAGE", "ELLIPTICITY", "LFLUX", "LBG"};
    char* tform[] = {"E", "E", "E", "E", "E", "E", "E", "E"};
    char* tunit[] = {"pix", "pix", "unknown", "unknown",
                     "pix", "", "unknown", "unknown"};
    int ncols = params->Lorder ? 8 : 6;
    int npeaks = params->npeaks;
    float sigma = params->sigma;
    float dpsf = params->dpsf;
    float plim = params->plim;
    float dlim = params->dlim;
    float saddle = params->saddle;
    int maxper = params->maxper;
    int maxnpeaks = params->maxnpeaks;
    int maxsize = params->maxsize;
    int halfbox = params->halfbox;

    fits_create_tbl(ofptr, BINARY_TBL, params->npeaks, ncols, ttype, tform,
                    tunit, "SOURCES", status);
    SOURCE_TABLE_CHECK("Failed to create output table");

    if (params->npeaks) {
        fits_write_col(ofptr, TFLOAT, 1, 1, 1, params->npeaks, params->x, status);
        SOURCE_TABLE_CHECK("Failed to write X column");

        fits_write_col(ofptr, TFLOAT, 2, 1, 1, params->npeaks, params->y, status);
        SOURCE_TABLE_CHECK("Failed to write Y column");

        fits_write_col(ofptr, TFLOAT, 3, 1, 1, params->npeaks, params->flux, status);
        SOURCE_TABLE_CHECK("Failed to write FLUX column");

        fits_write_col(ofptr, TFLOAT, 4, 1, 1, params->npeaks, params->background, status);
        SOURCE_TABLE_CHECK("Failed to write BACKGROUND column");

        fits_write_col(ofptr, TFLOAT, 5, 1, 1, params->npeaks, params->fwhm, status);
        SOURCE_TABLE_CHECK("Failed to write FWHM_IMAGE column");

        fits_write_col(ofptr, TFLOAT, 6, 1, 1, params->npeaks, params->ellipticity, status);
        SOURCE_TABLE_CHECK("Failed to write ELLIPTICITY column");

        if (params->Lorder) {
            fits_write_col(ofptr, TFLOAT, 7, 1, 1, params->npeaks, params->fluxL, status);
            SOURCE_TABLE_CHECK("Failed to write LFLUX column");

            fits_write_col(ofptr, TFLOAT, 8, 1, 1, params->npeaks, params->backgroundL, status);
            SOURCE_TABLE_CHECK("Failed to write LBG column");
        }
    }

    fits_modify_comment(ofptr, "TTYPE1", "X coordinate", status);
    SOURCE_TABLE_CHECK("Failed to set X TTYPE");

    fits_modify_comment(ofptr, "TTYPE2", "Y coordinate", status);
    SOURCE_TABLE_CHECK("Failed to set Y TTYPE");

    fits_modify_comment(ofptr, "TTYPE3", "Flux of source", status);
    SOURCE_TABLE_CHECK("Failed to set FLUX TTYPE");

    fits_modify_comment(ofptr, "TTYPE4", "Sky background of source", status);
    SOURCE_TABLE_CHECK("Failed to set BACKGROUND TTYPE");

    fits_modify_comment(ofptr, "TTYPE5", "Moment-based source FWHM estimate", status);
    SOURCE_TABLE_CHECK("Failed to set FWHM_IMAGE TTYPE");

    fits_modify_comment(ofptr, "TTYPE6", "Moment-based source ellipticity estimate", status);
    SOURCE_TABLE_CHECK("Failed to set ELLIPTICITY TTYPE");

    fits_write_key(ofptr, TINT, "SRCEXT", &src_ext,
                   "Extension number in src image", status);
    SOURCE_TABLE_CHECK("Failed to write SRCEXT");

    fits_write_key(ofptr, TSTRING, "SRCBACK", (char*)backend,
                   "Source extraction backend", status);
    SOURCE_TABLE_CHECK("Failed to write SRCBACK");

    fits_write_key(ofptr, TINT, "IMAGEW", &imagew, "Input image width", status);
    SOURCE_TABLE_CHECK("Failed to write IMAGEW");

    fits_write_key(ofptr, TINT, "IMAGEH", &imageh, "Input image height", status);
    SOURCE_TABLE_CHECK("Failed to write IMAGEH");

    fits_write_key(ofptr, TFLOAT, "ESTSIGMA", &sigma,
                   "Estimated source image variance", status);
    SOURCE_TABLE_CHECK("Failed to write ESTSIGMA");

    fits_write_key(ofptr, TINT, "NPEAKS", &npeaks, "image2xy Number of peaks found", status);
    fits_write_key(ofptr, TFLOAT, "DPSF", &dpsf, "image2xy Assumed gaussian psf width", status);
    fits_write_key(ofptr, TFLOAT, "PLIM", &plim, "image2xy Significance to keep", status);
    fits_write_key(ofptr, TFLOAT, "DLIM", &dlim, "image2xy Closest two peaks can be", status);
    fits_write_key(ofptr, TFLOAT, "SADDLE", &saddle, "image2xy Saddle difference (in sig)", status);
    fits_write_key(ofptr, TINT, "MAXPER", &maxper, "image2xy Max num of peaks per object", status);
    fits_write_key(ofptr, TINT, "MAXPEAKS", &maxnpeaks, "image2xy Max num of peaks total", status);
    fits_write_key(ofptr, TINT, "MAXSIZE", &maxsize, "image2xy Max size for extended objects", status);
    fits_write_key(ofptr, TINT, "HALFBOX", &halfbox, "image2xy Half-size for sliding sky window", status);
    SOURCE_TABLE_CHECK("Failed to write source extraction parameters");

    fits_write_comment(ofptr,
                       "The X and Y points are specified assuming 1,1 is "
                       "the center of the leftmost bottom pixel of the "
                       "image in accordance with the FITS standard.", status);
    SOURCE_TABLE_CHECK("Failed to write comments");

    return 0;
}

#undef SOURCE_TABLE_CHECK

int image2xy_files(const char* infn, const char* outfn,
                   anbool do_u8, int downsample, int downsample_as_required,
                   int extension, int plane,
                   simplexy_t* params) {
    fitsfile *fptr = NULL;
    fitsfile *ofptr = NULL;
    int status = 0; // FIXME should have ostatus too
    int naxis;
    long naxisn[3];
    int kk;
    int nhdus, hdutype, nimgs;
    char* str;
    simplexy_t myparams;

    if (params == NULL) {
        memset(&myparams, 0, sizeof(simplexy_t));
        params = &myparams;
    }

    // QFITS to CFITSIO extension convention switch
    extension++;

    fits_open_file(&fptr, infn, READONLY, &status);
    CFITS_CHECK("Failed to open FITS input file %s", infn);

    // Are there multiple HDU's?
    fits_get_num_hdus(fptr, &nhdus, &status);
    CFITS_CHECK("Failed to read number of HDUs for input file %s", infn);
    logverb("nhdus=%d\n", nhdus);

    if (extension > nhdus) {
        logerr("Requested extension %i is greater than number of extensions (%i) in file %s\n",
               extension, nhdus, infn);
        return -1;
    }

    // Create output file
    fits_create_file(&ofptr, outfn, &status);
    CFITS_CHECK("Failed to open FITS output file %s", outfn);

    fits_create_img(ofptr, 8, 0, NULL, &status);
    CFITS_CHECK("Failed to create output image");

    fits_write_key(ofptr, TSTRING, "SRCFN", (char*)infn, "Source image", &status);
    if (extension)
        fits_write_key(ofptr, TINT, "SRCEXT", &extension, "Source image extension (1=primary)", &status);

    /* Parameters for simplexy; save for debugging */
    fits_write_comment(ofptr, "Parameters used for source extraction", &status);

    fits_write_history(ofptr, "Created by Astrometry.net's image2xy program.", &status);
    CFITS_CHECK("Failed to write HISTORY headers");

    asprintf_safe(&str, "GIT URL: %s", AN_GIT_URL);
    fits_write_history(ofptr, str, &status);
    CFITS_CHECK("Failed to write GIT HISTORY headers");
    free(str);
    asprintf_safe(&str, "GIT Rev: %s", AN_GIT_REVISION);
    fits_write_history(ofptr, str, &status);
    CFITS_CHECK("Failed to write GIT HISTORY headers");
    free(str);
    asprintf_safe(&str, "GIT Date: %s", AN_GIT_DATE);
    fits_write_history(ofptr, str, &status);
    CFITS_CHECK("Failed to write GIT HISTORY headers");
    free(str);
    fits_write_history(ofptr, "Visit us on the web at http://astrometry.net/", &status);
    CFITS_CHECK("Failed to write GIT HISTORY headers");

    nimgs = 0;

    // Run simplexy on each HDU
    for (kk=1; kk <= nhdus; kk++) {
        long* fpixel;
        int a;
        int w, h;
        int bitpix;

        if (extension && kk != extension)
            continue;

        fits_movabs_hdu(fptr, kk, &hdutype, &status);
        fits_get_hdu_type(fptr, &hdutype, &status);

        if (hdutype != IMAGE_HDU) {
            if (extension)
                logerr("Requested extension %i in file %s is not an image.\n", extension, infn);
            continue;
        }

        fits_get_img_dim(fptr, &naxis, &status);
        CFITS_CHECK("Failed to find image dimensions for HDU %i", kk);

        fits_get_img_size(fptr, 2, naxisn, &status);
        CFITS_CHECK("Failed to find image dimensions for HDU %i", kk);

        nimgs++;

        logverb("Got naxis=%d, na1=%lu, na2=%lu\n", naxis, naxisn[0], naxisn[1]);

        fits_get_img_type(fptr, &bitpix, &status);
        CFITS_CHECK("Failed to get FITS image type");

        fpixel = malloc(naxis * sizeof(long));
        for (a=0; a<naxis; a++)
            fpixel[a] = 1;

        if (plane && naxis == 3) {
            if (plane <= naxisn[2]) {
                logmsg("Grabbing image plane %i\n", plane);
                fpixel[2] = plane;
            } else
                logerr("Requested plane %i but only %i are available.\n", plane, (int)naxisn[2]);
        } else if (plane)
            logmsg("Plane %i requested but this image has NAXIS = %i (not 3).\n", plane, naxis);
        else if (naxis > 2)
            logmsg("This looks like a multi-color image: processing the first image plane only.  (NAXIS=%i)\n", naxis);
		
        if (bitpix == 8 && do_u8 && !downsample) {
            simplexy_fill_in_defaults_u8(params);

            // u8 image.
            params->image_u8 = malloc(naxisn[0] * naxisn[1]);
            if (!params->image_u8) {
                SYSERROR("Failed to allocate u8 image array");
                goto bailout;
            }
            fits_read_pix(fptr, TBYTE, fpixel, naxisn[0]*naxisn[1], NULL,
                          params->image_u8, NULL, &status);

        } else {
            simplexy_fill_in_defaults(params);

            params->image = malloc(naxisn[0] * naxisn[1] * sizeof(float));
            if (!params->image) {
                SYSERROR("Failed to allocate image array");
                goto bailout;
            }
            fits_read_pix(fptr, TFLOAT, fpixel, naxisn[0]*naxisn[1], NULL,
                          params->image, NULL, &status);
        }
        free(fpixel);
        CFITS_CHECK("Failed to read image pixels");

        params->nx = naxisn[0];
        params->ny = naxisn[1];

        image2xy_run(params, downsample, downsample_as_required);

        w = naxisn[0];
        h = naxisn[1];
        if (write_source_table(ofptr, params, kk, w, h, "simplexy", &status))
            goto bailout;

        simplexy_free_contents(params);
    }

    // Put in the optional NEXTEND keywoard
    fits_movabs_hdu(ofptr, 1, &hdutype, &status);
    assert(hdutype == IMAGE_HDU);
    fits_write_key(ofptr, TINT, "NEXTEND", &nimgs, "Number of extensions", &status);
    if (status == END_OF_FILE)
        status = 0; /* Reset after normal error */
    CFITS_CHECK("Failed to write NEXTEND");

    fits_close_file(fptr, &status);
    CFITS_CHECK("Failed to close FITS input file");
    fptr = NULL;

    fits_close_file(ofptr, &status);
    CFITS_CHECK("Failed to close FITS output file");

    // for valgrind
    simplexy_clean_cache();

    return 0;

 bailout:
    if (fptr)
        fits_close_file(fptr, &status);
    if (ofptr)
        fits_close_file(ofptr, &status);
    return -1;
}

#ifdef HAVE_SEP

static int sep_check_status(int status, const char* msg) {
    char errtext[128];
    char detail[512];

    if (!status)
        return 0;

    errtext[0] = '\0';
    detail[0] = '\0';
    sep_get_errmsg(status, errtext);
    sep_get_errdetail(detail);
    if (detail[0])
        ERROR("%s: %s (%s)", msg, errtext, detail);
    else
        ERROR("%s: %s", msg, errtext);
    return -1;
}

static void sep_make_filter(float* conv) {
    static const float raw[] = {
        0.006319f, 0.040599f, 0.075183f, 0.040599f, 0.006319f,
        0.040599f, 0.260856f, 0.483068f, 0.260856f, 0.040599f,
        0.075183f, 0.483068f, 0.894573f, 0.483068f, 0.075183f,
        0.040599f, 0.260856f, 0.483068f, 0.260856f, 0.040599f,
        0.006319f, 0.040599f, 0.075183f, 0.040599f, 0.006319f
    };
    double sum = 0.0;
    int i;

    for (i=0; i<25; i++)
        sum += raw[i];
    for (i=0; i<25; i++)
        conv[i] = raw[i] / sum;
}

static int sep_downsample_image(float** image, int* W, int* H, int S) {
    int newW, newH;

    if (S <= 1)
        return 0;

    get_output_image_size(*W, *H, S, EDGE_AVERAGE, &newW, &newH);
    dsmooth2(*image, *W, *H, (float)S, *image);
    if (!average_image_f(*image, *W, *H, S, EDGE_AVERAGE, &newW, &newH, *image)) {
        ERROR("Averaging the image for SEP downsampling failed.");
        return -1;
    }

    *W = newW;
    *H = newH;
    return 0;
}

static int sep_fill_output(simplexy_t* params, sep_catalog* catalog,
                           const sep_bkg* bkg, int scale) {
    const double fwhm_factor = 2.3548200450309493;
    int i;
    int n;

    n = catalog->nobj;
    if (params->maxnpeaks && n > params->maxnpeaks)
        n = params->maxnpeaks;

    params->npeaks = n;
    if (!n)
        return 0;

    params->x = calloc(n, sizeof(float));
    params->y = calloc(n, sizeof(float));
    params->flux = calloc(n, sizeof(float));
    params->background = calloc(n, sizeof(float));
    params->fwhm = calloc(n, sizeof(float));
    params->ellipticity = calloc(n, sizeof(float));
    if (!params->x || !params->y || !params->flux || !params->background ||
        !params->fwhm || !params->ellipticity) {
        SYSERROR("Failed to allocate SEP source output arrays");
        return -1;
    }

    for (i=0; i<n; i++) {
        double a = catalog->a[i];
        double b = catalog->b[i];
        int64_t bx = catalog->xpeak[i];
        int64_t by = catalog->ypeak[i];

        params->x[i] = (float)((catalog->x[i] + 0.5) * (double)scale + 0.5);
        params->y[i] = (float)((catalog->y[i] + 0.5) * (double)scale + 0.5);
        params->flux[i] = catalog->flux[i];

        if (bkg) {
            bx = MAX(0, MIN((int64_t)params->nx - 1, bx));
            by = MAX(0, MIN((int64_t)params->ny - 1, by));
            params->background[i] = sep_bkg_pix(bkg, bx, by);
        } else
            params->background[i] = 0.0f;

        if (a < b) {
            double tmp = a;
            a = b;
            b = tmp;
        }

        params->fwhm[i] = 0.0f;
        params->ellipticity[i] = 1.0f;
        if (a > 0.0 && b >= 0.0) {
            params->fwhm[i] = (float)(fwhm_factor *
                                      sqrt((a * a + b * b) / 2.0) *
                                      (double)scale);
            params->ellipticity[i] = (float)(1.0 - b / a);
        }
    }

    return 0;
}

static int sep_extract_image(simplexy_t* params, int scale) {
    sep_image image;
    sep_bkg* bkg = NULL;
    sep_catalog* catalog = NULL;
    float* work = NULL;
    float conv[25];
    float threshold;
    float globalbg = 0.0f;
    int status;
    int i;
    int rtn = -1;

    memset(&image, 0, sizeof(sep_image));

    work = malloc((size_t)params->nx * (size_t)params->ny * sizeof(float));
    if (!work) {
        SYSERROR("Failed to allocate SEP working image");
        goto bailout;
    }
    memcpy(work, params->image, (size_t)params->nx * (size_t)params->ny * sizeof(float));

    if (params->invert) {
        for (i=0; i<(params->nx * params->ny); i++)
            work[i] = -work[i];
    }

    image.data = work;
    image.dtype = SEP_TFLOAT;
    image.w = params->nx;
    image.h = params->ny;
    image.noise_type = SEP_NOISE_STDDEV;

    status = sep_background(&image, 64, 64, 3, 3, 0.0, &bkg);
    if (sep_check_status(status, "SEP background estimation failed"))
        goto bailout;

    globalbg = sep_bkg_global(bkg);
    if (params->sigma == 0.0f)
        params->sigma = sep_bkg_globalrms(bkg);
    params->globalbg = globalbg;

    if (!params->nobgsub) {
        status = sep_bkg_subarray(bkg, work, SEP_TFLOAT);
        if (sep_check_status(status, "SEP background subtraction failed"))
            goto bailout;
    }

    threshold = params->plim * params->sigma;
    if (!isfinite(threshold) || threshold <= 0.0f)
        threshold = params->plim;

    sep_make_filter(conv);
    status = sep_extract(&image, threshold, SEP_THRESH_ABS, 5,
                         conv, 5, 5, SEP_FILTER_CONV,
                         32, 0.005, 1, 1.0, &catalog);
    if (sep_check_status(status, "SEP source extraction failed"))
        goto bailout;

    if (sep_fill_output(params, catalog, bkg, scale))
        goto bailout;

    logverb("SEP: found %i sources.\n", params->npeaks);
    rtn = 0;

 bailout:
    if (catalog)
        sep_catalog_free(catalog);
    if (bkg)
        sep_bkg_free(bkg);
    free(work);
    return rtn;
}

int image2xy_files_sep(const char* infn, const char* outfn,
                       int downsample, int downsample_as_required,
                       int extension, int plane,
                       simplexy_t* params) {
    fitsfile *fptr = NULL;
    fitsfile *ofptr = NULL;
    int status = 0;
    int naxis;
    long naxisn[3];
    int kk;
    int nhdus, hdutype, nimgs;
    char* str;
    simplexy_t myparams;

    if (params == NULL) {
        memset(&myparams, 0, sizeof(simplexy_t));
        params = &myparams;
    }

    // QFITS to CFITSIO extension convention switch
    extension++;

    fits_open_file(&fptr, infn, READONLY, &status);
    CFITS_CHECK("Failed to open FITS input file %s", infn);

    fits_get_num_hdus(fptr, &nhdus, &status);
    CFITS_CHECK("Failed to read number of HDUs for input file %s", infn);
    logverb("nhdus=%d\n", nhdus);

    if (extension > nhdus) {
        logerr("Requested extension %i is greater than number of extensions (%i) in file %s\n",
               extension, nhdus, infn);
        goto bailout;
    }

    fits_create_file(&ofptr, outfn, &status);
    CFITS_CHECK("Failed to open FITS output file %s", outfn);

    fits_create_img(ofptr, 8, 0, NULL, &status);
    CFITS_CHECK("Failed to create output image");

    fits_write_key(ofptr, TSTRING, "SRCFN", (char*)infn, "Source image", &status);
    if (extension)
        fits_write_key(ofptr, TINT, "SRCEXT", &extension, "Source image extension (1=primary)", &status);

    fits_write_comment(ofptr, "Parameters used for SEP source extraction", &status);

    fits_write_history(ofptr, "Created by Astrometry.net's image2xy program.", &status);
    CFITS_CHECK("Failed to write HISTORY headers");

    asprintf_safe(&str, "GIT URL: %s", AN_GIT_URL);
    fits_write_history(ofptr, str, &status);
    CFITS_CHECK("Failed to write GIT HISTORY headers");
    free(str);
    asprintf_safe(&str, "GIT Rev: %s", AN_GIT_REVISION);
    fits_write_history(ofptr, str, &status);
    CFITS_CHECK("Failed to write GIT HISTORY headers");
    free(str);
    asprintf_safe(&str, "GIT Date: %s", AN_GIT_DATE);
    fits_write_history(ofptr, str, &status);
    CFITS_CHECK("Failed to write GIT HISTORY headers");
    free(str);
    fits_write_history(ofptr, "Visit us on the web at http://astrometry.net/", &status);
    CFITS_CHECK("Failed to write GIT HISTORY headers");

    nimgs = 0;

    for (kk=1; kk <= nhdus; kk++) {
        long* fpixel;
        int a;
        int w, h;
        int scale = downsample ? downsample : 1;
        int ds_required = downsample_as_required;
        anbool tryagain;

        if (extension && kk != extension)
            continue;

        fits_movabs_hdu(fptr, kk, &hdutype, &status);
        fits_get_hdu_type(fptr, &hdutype, &status);

        if (hdutype != IMAGE_HDU) {
            if (extension)
                logerr("Requested extension %i in file %s is not an image.\n", extension, infn);
            continue;
        }

        fits_get_img_dim(fptr, &naxis, &status);
        CFITS_CHECK("Failed to find image dimensions for HDU %i", kk);

        fits_get_img_size(fptr, 2, naxisn, &status);
        CFITS_CHECK("Failed to find image dimensions for HDU %i", kk);

        nimgs++;

        logverb("Got naxis=%d, na1=%lu, na2=%lu\n", naxis, naxisn[0], naxisn[1]);

        fpixel = malloc(naxis * sizeof(long));
        if (!fpixel) {
            SYSERROR("Failed to allocate FITS pixel coordinate array");
            goto bailout;
        }
        for (a=0; a<naxis; a++)
            fpixel[a] = 1;

        if (plane && naxis == 3) {
            if (plane <= naxisn[2]) {
                logmsg("Grabbing image plane %i\n", plane);
                fpixel[2] = plane;
            } else
                logerr("Requested plane %i but only %i are available.\n", plane, (int)naxisn[2]);
        } else if (plane)
            logmsg("Plane %i requested but this image has NAXIS = %i (not 3).\n", plane, naxis);
        else if (naxis > 2)
            logmsg("This looks like a multi-color image: processing the first image plane only.  (NAXIS=%i)\n", naxis);

        simplexy_fill_in_defaults(params);

        params->image = malloc(naxisn[0] * naxisn[1] * sizeof(float));
        if (!params->image) {
            SYSERROR("Failed to allocate image array");
            free(fpixel);
            goto bailout;
        }
        fits_read_pix(fptr, TFLOAT, fpixel, naxisn[0]*naxisn[1], NULL,
                      params->image, NULL, &status);
        free(fpixel);
        CFITS_CHECK("Failed to read image pixels");

        params->nx = naxisn[0];
        params->ny = naxisn[1];

        if (downsample && downsample > 1) {
            logmsg("Downsampling by %i...\n", downsample);
            if (sep_downsample_image(&params->image, &params->nx, &params->ny, downsample))
                goto bailout;
        }

        do {
            tryagain = FALSE;
            if (sep_extract_image(params, scale))
                goto bailout;
            if (params->npeaks == 0 && ds_required) {
                logmsg("Downsampling by 2...\n");
                if (sep_downsample_image(&params->image, &params->nx, &params->ny, 2))
                    goto bailout;
                scale *= 2;
                ds_required--;
                tryagain = TRUE;
            }
        } while (tryagain);

        w = naxisn[0];
        h = naxisn[1];
        if (write_source_table(ofptr, params, kk, w, h, "sep", &status))
            goto bailout;

        simplexy_free_contents(params);
    }

    fits_movabs_hdu(ofptr, 1, &hdutype, &status);
    assert(hdutype == IMAGE_HDU);
    fits_write_key(ofptr, TINT, "NEXTEND", &nimgs, "Number of extensions", &status);
    if (status == END_OF_FILE)
        status = 0;
    CFITS_CHECK("Failed to write NEXTEND");

    fits_close_file(fptr, &status);
    CFITS_CHECK("Failed to close FITS input file");
    fptr = NULL;

    fits_close_file(ofptr, &status);
    CFITS_CHECK("Failed to close FITS output file");

    return 0;

 bailout:
    simplexy_free_contents(params);
    if (fptr)
        fits_close_file(fptr, &status);
    if (ofptr)
        fits_close_file(ofptr, &status);
    return -1;
}

#else

int image2xy_files_sep(const char* infn, const char* outfn,
                       int downsample, int downsample_as_required,
                       int extension, int plane,
                       simplexy_t* params) {
    (void)infn;
    (void)outfn;
    (void)downsample;
    (void)downsample_as_required;
    (void)extension;
    (void)plane;
    (void)params;
    ERROR("SEP source extraction requested, but this binary was built without SEP support.  Rebuild with HAVE_SEP=yes SEP_INC=... SEP_LIB=...");
    return -1;
}

#endif
