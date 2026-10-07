"""Run in a source build with _blender_web; isolate scene and restore context.

Linked input meshes must share resources until their evaluated geometry differs.
The export's arena must match each object's native evaluated vertex coordinates.
"""
import bpy
import json
import os
import struct
import tempfile
import _blender_web

previous = bpy.context.window.scene
scene = bpy.data.scenes.new('WebGeometryIdentity')
objects = []
mesh = bpy.data.meshes.new('Collision')
mesh.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
try:
    bpy.context.window.scene = scene
    for name in ['SharedA', 'SharedB', 'ArrayA', 'ArrayB', 'Collision']:
        obj = bpy.data.objects.new(name, mesh)
        scene.collection.objects.link(obj)
        objects.append(obj)
    for obj, count in zip(objects[2:4], [2, 4]):
        modifier = obj.modifiers.new('DifferentArray', 'ARRAY')
        modifier.count = count
    collision = objects[4].modifiers.new('NameCollision', 'ARRAY')
    collision.count = 3
    bpy.context.view_layer.update()
    graph = bpy.context.evaluated_depsgraph_get()
    frame = json.loads(_blender_web.export_frame(json.dumps({'evaluate': True, 'defer': True})))
    rows = {row['name']: row for row in frame['objects']}
    assert rows['SharedA']['mesh'] == rows['SharedB']['mesh']
    assert len({rows[name]['mesh'] for name in ['SharedA', 'ArrayA', 'ArrayB', 'Collision']}) == 4
    expected = {}
    for obj in objects:
        key = rows[obj.name]['mesh']
        positions = [tuple(vertex.co) for vertex in obj.evaluated_get(graph).data.vertices]
        if key in expected:
            assert positions == expected[key]
        expected[key] = positions
    with tempfile.TemporaryDirectory() as directory:
        buffer_path = os.path.join(directory, 'mesh.bin')
        for key, positions in expected.items():
            resource = json.loads(_blender_web.export_mesh(json.dumps({'key': key, 'buffer_path': buffer_path})))['mesh']
            column = resource['columns']['co']
            raw = open(buffer_path, 'rb').read()
            coordinates = struct.unpack_from('<%df' % (column['count'] * 3), raw, column['offset'])
            assert [tuple(coordinates[i:i + 3]) for i in range(0, len(coordinates), 3)] == positions, key
    print('WEB_GEOMETRY_IDENTITY_PASS', len(expected))
finally:
    bpy.context.window.scene = previous
    for obj in objects:
        bpy.data.objects.remove(obj, do_unlink=True)
    bpy.data.scenes.remove(scene)
    bpy.data.meshes.remove(mesh)
