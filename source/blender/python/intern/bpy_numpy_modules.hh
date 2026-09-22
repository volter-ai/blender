/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** Register the statically linked NumPy extensions before Python initializes. */
int BPY_numpy_extend_inittab();
