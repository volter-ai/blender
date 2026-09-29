# SPDX-License-Identifier: GPL-2.0-or-later
"""Run with the built Blender Python door; compares native and exported triangles."""
import array
import json
import bpy
import _blender_web

bpy.ops.wm.read_factory_settings(use_empty=True)
mesh = bpy.data.meshes.new('concave')
mesh.from_pydata([(0,0,0),(2,0,0),(2,2,0),(1,1,0),(0,2,0)], [], [(0,1,2,3,4)])
obj = bpy.data.objects.new('concave', mesh)
bpy.context.collection.objects.link(obj)
for i in range(64):
    linked = bpy.data.objects.new('linked%d' % i, mesh)
    bpy.context.collection.objects.link(linked)
checkpoint_count = 0
def checkpoint():
    global checkpoint_count
    checkpoint_count += 1

def frame(known=None):
    return json.loads(_blender_web.export_frame_chunked(json.dumps({
        'session': 'native-columns-proof', 'defer': True, 'known': known or {}}), checkpoint))

def verify(f):
    key, notice = next(iter(f['meshes'].items()))
    assert notice['deferred']
    piece = json.loads(_blender_web.export_mesh(json.dumps({'key': key, 'buffer_path': '/tmp/columns.bin'})))['mesh']
    descriptor = piece['columns']['cornerTri']
    assert descriptor['dtype'] == 'u32' and descriptor['stride'] == 3
    raw = _blender_web.buffer()
    actual = array.array('I', raw[descriptor['offset']:descriptor['offset'] + descriptor['length']])
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get()).data
    evaluated.calc_loop_triangles()
    expected = array.array('I', [0]) * (len(evaluated.loop_triangles) * 3)
    evaluated.loop_triangles.foreach_get('loops', expected)
    assert actual == expected, (actual, expected)
    return key, notice['revision']

key, revision = verify(frame())
unchanged = frame({'mesh:' + key: revision})
assert unchanged['meshes'][key]['unchanged']
mesh.vertices[3].co.y = 0.5
mesh.update()
bpy.context.view_layer.update()
key2, revision2 = verify(frame({'mesh:' + key: revision}))
assert key2 == key and revision2 != revision
assert checkpoint_count >= 9
print('NATIVE_COLUMNS_OK: concave initial, unchanged delta, edited delta')
