# SPDX-License-Identifier: GPL-2.0-or-later
"""Run with a large CHECKPOINT_SCENE (default /work/scene.blend).
A rejected native read checkpoint must preserve the previous document."""
import os
import bpy
import _blender_web

before = sorted(obj.name for obj in bpy.data.objects)
called = 0

def refuse():
    global called
    called += 1
    raise RuntimeError('intentional read checkpoint refusal')

_blender_web.set_read_checkpoint(refuse)
refused = False
try:
    bpy.ops.wm.open_mainfile(filepath=os.environ.get('CHECKPOINT_SCENE', '/work/scene.blend'))
except RuntimeError as error:
    assert 'checkpoint' in str(error), str(error)
    refused = True
finally:
    try:
        _blender_web.set_read_checkpoint(None)
    except RuntimeError as error:
        assert 'intentional read checkpoint refusal' in str(error), str(error)
        refused = True
assert called == 1 and refused
assert sorted(obj.name for obj in bpy.data.objects) == before
print('READ_CHECKPOINT_OK: refused read retained previous document and removed callback')
