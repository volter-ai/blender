/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <Python.h>

#include "bpy_numpy_modules.hh"

extern "C" {
PyObject *PyInit__multiarray_umath();
PyObject *PyInit__pocketfft_umath();
PyObject *PyInit__umath_linalg();
PyObject *PyInit_lapack_lite();
PyObject *PyInit__bounded_integers();
PyObject *PyInit__common();
PyObject *PyInit__generator();
PyObject *PyInit__mt19937();
PyObject *PyInit__pcg64();
PyObject *PyInit__philox();
PyObject *PyInit__sfc64();
PyObject *PyInit_bit_generator();
PyObject *PyInit_mtrand();
}

int BPY_numpy_extend_inittab()
{
  static _inittab numpy_modules[] = {
      {"numpy._core._multiarray_umath", PyInit__multiarray_umath},
      {"numpy.fft._pocketfft_umath", PyInit__pocketfft_umath},
      {"numpy.linalg._umath_linalg", PyInit__umath_linalg},
      {"numpy.linalg.lapack_lite", PyInit_lapack_lite},
      {"numpy.random._bounded_integers", PyInit__bounded_integers},
      {"numpy.random._common", PyInit__common},
      {"numpy.random._generator", PyInit__generator},
      {"numpy.random._mt19937", PyInit__mt19937},
      {"numpy.random._pcg64", PyInit__pcg64},
      {"numpy.random._philox", PyInit__philox},
      {"numpy.random._sfc64", PyInit__sfc64},
      {"numpy.random.bit_generator", PyInit_bit_generator},
      {"numpy.random.mtrand", PyInit_mtrand},
      {nullptr, nullptr},
  };
  return PyImport_ExtendInittab(numpy_modules);
}
