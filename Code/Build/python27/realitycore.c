/* ===========================================================================
 * SCAFFOLDING - NOT RECOVERED SOURCE.
 * _realitycore: the math helper module Project Reality 1.9 builds into its dice_py.dll
 * (realitycore.py, realitylaser.py, realityutilities.py and realityzones.py import it).
 * Function names, argument formats and docstrings are taken from PR's dice_py.dll.
 * calcDistance*, calcHoriz* (x/z plane, BF2 is Y-up), calcPosFromPosRot (yaw in degrees,
 * x += sin(yaw)*d, z += cos(yaw)*d, y and pitch unused) and isClose (squared compare)
 * follow its disassembly.
 * [COMPAT] isCloseHoriz mirrors isClose on x/z; the polygon functions use
 * pos[0]/pos[2] against the polygon's (a, b) pairs, pnpoly (W. R. Franklin) for inside, and
 * the minimum distance to the polygon's edges, negated when inside.
 * =========================================================================== */
#include "Python.h"
#include <math.h>

#define DEG2RAD (3.141592653589793 / 180.0)

static int get3(PyObject *t, double v[3])
{
    int i;
    PyObject *o;
    if (!PySequence_Check(t) || PySequence_Size(t) < 3) {
        PyErr_SetString(PyExc_TypeError, "expected a tuple containing 3 floats");
        return 0;
    }
    for (i = 0; i < 3; ++i) {
        o = PySequence_GetItem(t, i);
        if (o == NULL)
            return 0;
        v[i] = PyFloat_AsDouble(o);
        Py_DECREF(o);
        if (v[i] == -1.0 && PyErr_Occurred())
            return 0;
    }
    return 1;
}

static int parse2(PyObject *args, const char *name, double a[3], double b[3])
{
    PyObject *p1, *p2;
    if (!PyArg_ParseTuple(args, "OO", &p1, &p2))
        return 0;
    (void)name;
    return get3(p1, a) && get3(p2, b);
}

static double dist2(const double a[3], const double b[3])
{
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

static double hdist2(const double a[3], const double b[3])
{
    double dx = a[0] - b[0], dz = a[2] - b[2];
    return dx * dx + dz * dz;
}

static PyObject *rc_calcDistance(PyObject *self, PyObject *args)
{
    double a[3], b[3];
    if (!parse2(args, "calcDistance", a, b)) return NULL;
    return PyFloat_FromDouble(sqrt(dist2(a, b)));
}

static PyObject *rc_calcDistanceSquared(PyObject *self, PyObject *args)
{
    double a[3], b[3];
    if (!parse2(args, "calcDistanceSquared", a, b)) return NULL;
    return PyFloat_FromDouble(dist2(a, b));
}

static PyObject *rc_calcHorizDistance(PyObject *self, PyObject *args)
{
    double a[3], b[3];
    if (!parse2(args, "calcHorizDistance", a, b)) return NULL;
    return PyFloat_FromDouble(sqrt(hdist2(a, b)));
}

static PyObject *rc_calcHorizDistanceSquared(PyObject *self, PyObject *args)
{
    double a[3], b[3];
    if (!parse2(args, "calcHorizDistanceSquared", a, b)) return NULL;
    return PyFloat_FromDouble(hdist2(a, b));
}

static PyObject *rc_calcPosFromPosRot(PyObject *self, PyObject *args)
{
    PyObject *p, *r, *d;
    double pos[3], rot[3], dist, yaw;
    if (!PyArg_ParseTuple(args, "OOO", &p, &r, &d)) return NULL;
    if (!get3(p, pos) || !get3(r, rot)) return NULL;
    dist = PyFloat_AsDouble(d);
    if (dist == -1.0 && PyErr_Occurred()) return NULL;
    yaw = rot[0] * DEG2RAD;
    return Py_BuildValue("(ddd)", pos[0] + sin(yaw) * dist, pos[1], pos[2] + cos(yaw) * dist);
}

static PyObject *close_impl(PyObject *args, int horiz)
{
    PyObject *p1, *p2, *d;
    double a[3], b[3], dist;
    if (!PyArg_ParseTuple(args, "OOO", &p1, &p2, &d)) return NULL;
    if (!get3(p1, a) || !get3(p2, b)) return NULL;
    dist = PyFloat_AsDouble(d);
    if (dist == -1.0 && PyErr_Occurred()) return NULL;
    return PyBool_FromLong((horiz ? hdist2(a, b) : dist2(a, b)) <= dist * dist);
}

static PyObject *rc_isClose(PyObject *self, PyObject *args) { return close_impl(args, 0); }
static PyObject *rc_isCloseHoriz(PyObject *self, PyObject *args) { return close_impl(args, 1); }

/* polygon: sequence of 2-sequences -> malloc'd array of n*2 doubles */
static double *get_polygon(PyObject *poly, Py_ssize_t *n)
{
    Py_ssize_t i, count;
    double *pts;
    if (!PySequence_Check(poly)) {
        PyErr_SetString(PyExc_TypeError, "polygon must be a list of 2-tuples");
        return NULL;
    }
    count = PySequence_Size(poly);
    if (count < 0) return NULL;
    pts = (double *)PyMem_Malloc(sizeof(double) * 2 * (count ? count : 1));
    if (pts == NULL) { PyErr_NoMemory(); return NULL; }
    for (i = 0; i < count; ++i) {
        PyObject *pt = PySequence_GetItem(poly, i), *o;
        int k;
        if (pt == NULL) { PyMem_Free(pts); return NULL; }
        for (k = 0; k < 2; ++k) {
            o = PySequence_GetItem(pt, k);
            if (o == NULL) { Py_DECREF(pt); PyMem_Free(pts); return NULL; }
            pts[2 * i + k] = PyFloat_AsDouble(o);
            Py_DECREF(o);
            if (pts[2 * i + k] == -1.0 && PyErr_Occurred()) { Py_DECREF(pt); PyMem_Free(pts); return NULL; }
        }
        Py_DECREF(pt);
    }
    *n = count;
    return pts;
}

static int pnpoly(const double *pts, Py_ssize_t n, double x, double y)
{
    Py_ssize_t i, j;
    int c = 0;
    for (i = 0, j = n - 1; i < n; j = i++) {
        double xi = pts[2 * i], yi = pts[2 * i + 1], xj = pts[2 * j], yj = pts[2 * j + 1];
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi))
            c = !c;
    }
    return c;
}

static PyObject *rc_isPointInPolygon(PyObject *self, PyObject *args)
{
    PyObject *p, *poly;
    double pos[3], *pts;
    Py_ssize_t n;
    int in;
    if (!PyArg_ParseTuple(args, "OO", &p, &poly)) return NULL;
    if (!get3(p, pos)) return NULL;
    if ((pts = get_polygon(poly, &n)) == NULL) return NULL;
    in = n >= 3 && pnpoly(pts, n, pos[0], pos[2]);
    PyMem_Free(pts);
    return PyBool_FromLong(in);
}

static PyObject *rc_calcDistanceToPolygon(PyObject *self, PyObject *args)
{
    PyObject *p, *poly;
    double pos[3], *pts, best = -1.0, x, y;
    Py_ssize_t n, i, j;
    if (!PyArg_ParseTuple(args, "OO", &p, &poly)) return NULL;
    if (!get3(p, pos)) return NULL;
    if ((pts = get_polygon(poly, &n)) == NULL) return NULL;
    x = pos[0];
    y = pos[2];
    for (i = 0, j = n - 1; i < n; j = i++) {
        double ax = pts[2 * j], ay = pts[2 * j + 1], bx = pts[2 * i], by = pts[2 * i + 1];
        double ex = bx - ax, ey = by - ay, len2 = ex * ex + ey * ey, t = 0.0, dx, dy, d2;
        if (len2 > 0.0) {
            t = ((x - ax) * ex + (y - ay) * ey) / len2;
            if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
        }
        dx = x - (ax + t * ex);
        dy = y - (ay + t * ey);
        d2 = dx * dx + dy * dy;
        if (best < 0.0 || d2 < best) best = d2;
    }
    best = best < 0.0 ? 0.0 : sqrt(best);
    if (n >= 3 && pnpoly(pts, n, x, y))
        best = -best;
    PyMem_Free(pts);
    return PyFloat_FromDouble(best);
}

static PyMethodDef rc_methods[] = {
    {"calcDistance", rc_calcDistance, METH_VARARGS,
     "calcDistance(pos1, pos2)\npos1, pos2 - a tuple containing 3 floats\nReturn the distance between two points."},
    {"calcDistanceSquared", rc_calcDistanceSquared, METH_VARARGS,
     "calcDistanceSquared(pos1, pos2)\npos1, pos2 - a tuple containing 3 floats\nReturns the squared distance between two points."},
    {"calcHorizDistance", rc_calcHorizDistance, METH_VARARGS,
     "calcHorizDistance(pos1, pos2)\npos1, pos2 - a tuple containing 3 floats\nReturns the horizontal distance between two points."},
    {"calcHorizDistanceSquared", rc_calcHorizDistanceSquared, METH_VARARGS,
     "calcHorizDistanceSquared(pos1, pos2)\npos1, pos2 - tuples containing 3 floats\nReturns the squared horizontal distance between two points."},
    {"calcPosFromPosRot", rc_calcPosFromPosRot, METH_VARARGS,
     "calcPosFromPosRot(pos1, rot1, distance)\nReturns a new position based on postion, rotation and distance."},
    {"isClose", rc_isClose, METH_VARARGS,
     "isClose(pos1, pos2, distance)\nChecks if pos2 is within the distance of pos1"},
    {"isCloseHoriz", rc_isCloseHoriz, METH_VARARGS,
     "isCloseHoriz(pos1, pos2, distance)\nChecks if pos2 is within the distance of pos1 horizontally"},
    {"isPointInPolygon", rc_isPointInPolygon, METH_VARARGS,
     "isPointInPolygon(pos, polygon)\nChecks if pos is inside the given polygon. Based on the W. Randolph Franklin method."},
    {"calcDistanceToPolygon", rc_calcDistanceToPolygon, METH_VARARGS,
     "calcDistanceToPolygon(pos, polygon)\nCalculates the distance from a given position to a given polygon. "
     "Negative distance is returned if it's from within the polygon"},
    {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC init_realitycore(void)
{
    Py_InitModule3("_realitycore", rc_methods,
                   "This module provides core functions used by project\nreality's python modules that required optimizations.");
}
