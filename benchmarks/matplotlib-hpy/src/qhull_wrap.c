/*-----------------------------------------------------------------------------
| Copyright (c) 2012- Matplotlib Development Team; All Rights Reserved
| Copyright (c) 2022, Oracle and/or its affiliates.
|
| The full license is in the file LICENSE, distributed with this software.
|----------------------------------------------------------------------------*/
/*
 * Wrapper module for libqhull, providing Delaunay triangulation.
 *
 * This module's methods should not be accessed directly.  To obtain a Delaunay
 * triangulation, construct an instance of the matplotlib.tri.Triangulation
 * class without specifying a triangles array.
 */
#define PY_SSIZE_T_CLEAN
#include "hpy.h"
#include "numpy/ndarrayobject.h"
#include "libqhull_r/qhull_ra.h"
#include <stdio.h>


#ifndef MPL_DEVNULL
#error "MPL_DEVNULL must be defined as the OS-equivalent of /dev/null"
#endif

#define STRINGIFY(x) STR(x)
#define STR(x) #x


static const char* qhull_error_msg[6] = {
    "",                     /* 0 = qh_ERRnone */
    "input inconsistency",  /* 1 = qh_ERRinput */
    "singular input data",  /* 2 = qh_ERRsingular */
    "precision error",      /* 3 = qh_ERRprec */
    "insufficient memory",  /* 4 = qh_ERRmem */
    "internal error"};      /* 5 = qh_ERRqhull */


/* Return the indices of the 3 vertices that comprise the specified facet (i.e.
 * triangle). */
static void
get_facet_vertices(qhT* qh, const facetT* facet, int indices[3])
{
    vertexT *vertex, **vertexp;
    FOREACHvertex_(facet->vertices)
        *indices++ = qh_pointid(qh, vertex->point);
}

/* Return the indices of the 3 triangles that are neighbors of the specified
 * facet (triangle). */
static void
get_facet_neighbours(const facetT* facet, const int* tri_indices,
                     int indices[3])
{
    facetT *neighbor, **neighborp;
    FOREACHneighbor_(facet)
        *indices++ = (neighbor->upperdelaunay ? -1 : tri_indices[neighbor->id]);
}

/* Return 1 if the specified points arrays contain at least 3 unique points,
 * or 0 otherwise. */
static int
at_least_3_unique_points(int npoints, const double* x, const double* y)
{
    int i;
    const int unique1 = 0;  /* First unique point has index 0. */
    int unique2 = 0;        /* Second unique point index is 0 until set. */

    if (npoints < 3)
        return 0;

    for (i = 1; i < npoints; ++i) {
        if (unique2 == 0) {
            /* Looking for second unique point. */
            if (x[i] != x[unique1] || y[i] != y[unique1])
                unique2 = i;
        }
        else {
            /* Looking for third unique point. */
            if ( (x[i] != x[unique1] || y[i] != y[unique1]) &&
                 (x[i] != x[unique2] || y[i] != y[unique2]) ) {
                /* 3 unique points found, with indices 0, unique2 and i. */
                return 1;
            }
        }
    }

    /* Run out of points before 3 unique points found. */
    return 0;
}

/* Delaunay implementation methyod.  If hide_qhull_errors is 1 then qhull error
 * messages are discarded; if it is 0 then they are written to stderr. */
static HPy
delaunay_impl(HPyContext *ctx, int npoints, const double* x, const double* y,
              int hide_qhull_errors)
{
	qhT qh_qh;                  /* qh variable type and name must be like */
	qhT* qh = &qh_qh;           /* this for Qhull macros to work correctly. */
    coordT* points = NULL;
    facetT* facet;
    int i, ntri, max_facet_id;
    FILE* error_file = NULL;    /* qhull expects a FILE* to write errors to. */
    int exitcode;               /* Value returned from qh_new_qhull(). */
    int* tri_indices = NULL;    /* Maps qhull facet id to triangle index. */
    int indices[3];
    int curlong, totlong;       /* Memory remaining after qh_memfreeshort. */
    const int ndim = 2;
    npy_intp dims[2];
    PyArrayObject* triangles = NULL;
    PyArrayObject* neighbors = NULL;
    int* triangles_ptr;
    int* neighbors_ptr;
    double x_mean = 0.0;
    double y_mean = 0.0;

    QHULL_LIB_CHECK

    /* Allocate points. */
    points = (coordT*)malloc(npoints*ndim*sizeof(coordT));
    if (points == NULL) {
        HPyErr_SetString(ctx, ctx->h_MemoryError,
                        "Could not allocate points array in qhull.delaunay");
        goto error_before_qhull;
    }

    /* Determine mean x, y coordinates. */
    for (i = 0; i < npoints; ++i) {
        x_mean += x[i];
        y_mean += y[i];
    }
    x_mean /= npoints;
    y_mean /= npoints;

    /* Prepare points array to pass to qhull. */
    for (i = 0; i < npoints; ++i) {
        points[2*i  ] = x[i] - x_mean;
        points[2*i+1] = y[i] - y_mean;
    }

    /* qhull expects a FILE* to write errors to. */
    if (hide_qhull_errors) {
        /* qhull errors are ignored by writing to OS-equivalent of /dev/null.
         * Rather than have OS-specific code here, instead it is determined by
         * setupext.py and passed in via the macro MPL_DEVNULL. */
        error_file = fopen(STRINGIFY(MPL_DEVNULL), "w");
        if (error_file == NULL) {
            HPyErr_SetString(ctx, ctx->h_RuntimeError,
                            "Could not open devnull in qhull.delaunay");
            goto error_before_qhull;
        }
    }
    else {
        /* qhull errors written to stderr. */
        error_file = stderr;
    }

    /* Perform Delaunay triangulation. */
    qh_zero(qh, error_file);
    exitcode = qh_new_qhull(qh, ndim, npoints, points, False,
                            "qhull d Qt Qbb Qc Qz", NULL, error_file);
    if (exitcode != qh_ERRnone) {
        PyErr_Format(PyExc_RuntimeError,
                     "Error in qhull Delaunay triangulation calculation: %s (exitcode=%d)%s",
                     qhull_error_msg[exitcode], exitcode,
                     hide_qhull_errors ? "; use python verbose option (-v) to see original qhull error." : "");
        goto error;
    }

    /* Split facets so that they only have 3 points each. */
    qh_triangulate(qh);

    /* Determine ntri and max_facet_id.
       Note that libqhull uses macros to iterate through collections. */
    ntri = 0;
    FORALLfacets {
        if (!facet->upperdelaunay)
            ++ntri;
    }

    max_facet_id = qh->facet_id - 1;

    /* Create array to map facet id to triangle index. */
    tri_indices = (int*)malloc((max_facet_id+1)*sizeof(int));
    if (tri_indices == NULL) {
        HPyErr_SetString(ctx, ctx->h_MemoryError,
                        "Could not allocate triangle map in qhull.delaunay");
        goto error;
    }

    /* Allocate python arrays to return. */
    dims[0] = ntri;
    dims[1] = 3;
    triangles = (PyArrayObject*)PyArray_SimpleNew(ndim, dims, NPY_INT);
    if (triangles == NULL) {
        HPyErr_SetString(ctx, ctx->h_MemoryError,
                        "Could not allocate triangles array in qhull.delaunay");
        goto error;
    }

    neighbors = (PyArrayObject*)PyArray_SimpleNew(ndim, dims, NPY_INT);
    if (neighbors == NULL) {
        HPyErr_SetString(ctx, ctx->h_MemoryError,
                        "Could not allocate neighbors array in qhull.delaunay");
        goto error;
    }

    triangles_ptr = (int*)PyArray_DATA(triangles);
    neighbors_ptr = (int*)PyArray_DATA(neighbors);

    /* Determine triangles array and set tri_indices array. */
    i = 0;
    FORALLfacets {
        if (!facet->upperdelaunay) {
            tri_indices[facet->id] = i++;
            get_facet_vertices(qh, facet, indices);
            *triangles_ptr++ = (facet->toporient ? indices[0] : indices[2]);
            *triangles_ptr++ = indices[1];
            *triangles_ptr++ = (facet->toporient ? indices[2] : indices[0]);
        }
        else
            tri_indices[facet->id] = -1;
    }

    /* Determine neighbors array. */
    FORALLfacets {
        if (!facet->upperdelaunay) {
            get_facet_neighbours(facet, tri_indices, indices);
            *neighbors_ptr++ = (facet->toporient ? indices[2] : indices[0]);
            *neighbors_ptr++ = (facet->toporient ? indices[0] : indices[2]);
            *neighbors_ptr++ = indices[1];
        }
    }

    /* Clean up. */
    qh_freeqhull(qh, !qh_ALL);
    qh_memfreeshort(qh, &curlong, &totlong);
    if (curlong || totlong)
        HPyErr_WarnEx(ctx, ctx->h_RuntimeWarning,
                     "Qhull could not free all allocated memory", 1);
    if (hide_qhull_errors)
        fclose(error_file);
    free(tri_indices);
    free(points);

    HPy tuple[] = {
        HPy_FromPyObject(ctx, (cpy_PyObject *)triangles), 
        HPy_FromPyObject(ctx, (cpy_PyObject *)neighbors)
    };
    
    return HPyTuple_FromArray(ctx, tuple, 2);

error:
    /* Clean up. */
    Py_XDECREF(triangles);
    Py_XDECREF(neighbors);
    qh_freeqhull(qh, !qh_ALL);
    qh_memfreeshort(qh, &curlong, &totlong);
    /* Don't bother checking curlong and totlong as raising error anyway. */
    if (hide_qhull_errors)
        fclose(error_file);
    free(tri_indices);

error_before_qhull:
    free(points);

    return HPy_NULL;
}

/* Process python arguments and call Delaunay implementation method. */
static HPy
delaunay(HPyContext *ctx, HPy h_self, HPy* args, HPy_ssize_t nargs)
{
    HPy xarg;
    HPy yarg;
    PyArrayObject* xarray;
    PyArrayObject* yarray;
    HPy ret;
    int npoints;
    const double* x;
    const double* y;

    if (!HPyArg_Parse(ctx, NULL, args, nargs, "OO", &xarg, &yarg)) {
        HPyErr_SetString(ctx, ctx->h_ValueError, "expecting x and y arrays");
        return HPy_NULL;
    }

    xarray = (PyArrayObject*)PyArray_ContiguousFromObject(HPy_AsPyObject(ctx, xarg), NPY_DOUBLE,
                                                          1, 1);
    yarray = (PyArrayObject*)PyArray_ContiguousFromObject(HPy_AsPyObject(ctx, yarg), NPY_DOUBLE,
                                                          1, 1);
    if (xarray == 0 || yarray == 0 ||
        PyArray_DIM(xarray,0) != PyArray_DIM(yarray, 0)) {
        Py_XDECREF(xarray);
        Py_XDECREF(yarray);
        HPyErr_SetString(ctx, ctx->h_ValueError,
                        "x and y must be 1D arrays of the same length");
        return HPy_NULL;
    }

    npoints = PyArray_DIM(xarray, 0);

    if (npoints < 3) {
        Py_XDECREF(xarray);
        Py_XDECREF(yarray);
        HPyErr_SetString(ctx, ctx->h_ValueError,
                        "x and y arrays must have a length of at least 3");
        return HPy_NULL;
    }

    x = (const double*)PyArray_DATA(xarray);
    y = (const double*)PyArray_DATA(yarray);

    if (!at_least_3_unique_points(npoints, x, y)) {
        Py_XDECREF(xarray);
        Py_XDECREF(yarray);
        HPyErr_SetString(ctx, ctx->h_ValueError,
                        "x and y arrays must consist of at least 3 unique points");
        return HPy_NULL;
    }

    ret = delaunay_impl(ctx, npoints, x, y, Py_VerboseFlag == 0);

    Py_XDECREF(xarray);
    Py_XDECREF(yarray);
    return ret;
}

/* Return qhull version string for assistance in debugging. */
static HPy
version(HPyContext *ctx, HPy module)
{
    return HPyBytes_FromString(ctx, qh_version);
}


HPyDef_METH(delaunay_def, "delaunay", delaunay, HPyFunc_VARARGS, .doc = "")
HPyDef_METH(version_def, "version", version, HPyFunc_NOARGS, .doc = "")
static HPyDef *module_defines[] = {
    &delaunay_def,
    &version_def,
    NULL
};

static HPyModuleDef moduledef = {
    .name = "_qhull",
    .doc = "Computing Delaunay triangulations.\n",
    .size = -1,
    .defines = module_defines,
};

// Logic is from NumPy's import_array()
static int npy_import_array_hpy(HPyContext *ctx) {
    if (_import_array() < 0) {
        // HPyErr_Print(ctx); TODO
        HPyErr_SetString(ctx, ctx->h_ImportError, "numpy.core.multiarray failed to import"); 
        return 0; 
    }
    return 1;
}
#ifdef __cplusplus
extern "C" {
#endif

#pragma GCC visibility push(default)
HPy_MODINIT(_qhull)
static HPy init__qhull_impl(HPyContext *ctx)
{
    if (!npy_import_array_hpy(ctx)) {
        return HPy_NULL;
    }

    HPy m = HPyModule_Create(ctx, &moduledef);
    if (HPy_IsNull(m)) {
        return HPy_NULL;
    }

    return m;
}

#pragma GCC visibility pop
#ifdef __cplusplus
}
#endif
