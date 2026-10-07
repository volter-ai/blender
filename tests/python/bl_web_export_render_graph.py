"""A render export follows native render visibility/evaluation and owns its state.

Run with the source-built Blender containing _blender_web. The render-engine
callback supplies the genuine RENDER graph; no authored visibility is changed
to make viewport evaluation resemble it.
"""
import bpy
import json
import _blender_web

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
camera = bpy.data.objects.new('Camera', bpy.data.cameras.new('Camera'))
scene.collection.objects.link(camera)
scene.camera = camera
camera.location = (0, -8, 3)
scene.render.resolution_x = scene.render.resolution_y = 8
scene.render.resolution_percentage = 100
collection = bpy.data.collections.new('RenderOnly')
scene.collection.children.link(collection)
collection.hide_viewport = True
mesh = bpy.data.meshes.new('NativeTriangle')
mesh.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
hidden = bpy.data.objects.new('RenderOnly', mesh)
collection.objects.link(hidden)
hidden.location = (13, 7, 5)
modified = bpy.data.objects.new('RenderModifier', mesh)
scene.collection.objects.link(modified)
array = modified.modifiers.new('RenderOnlyArray', 'ARRAY')
array.count = 3
array.show_viewport = False
array.show_render = True
modified.location = (2, 3, 4)

def export(known=None):
    return json.loads(_blender_web.export_frame(json.dumps({
        'evaluate': True, 'defer': True, 'known': known or {}, 'session': 'viewport-test'})))

before = export()
known = {'mesh:' + key: value['revision'] for key, value in before['meshes'].items()}
rows = {row['name']: row for row in before['objects']}
viewport_key = rows['RenderModifier']['mesh']
assert json.loads(_blender_web.export_mesh(json.dumps({'key': viewport_key})))['mesh']['counts']['verts'] == 3

try:
    _blender_web.begin_render_export(bpy.context.evaluated_depsgraph_get())
    raise AssertionError('viewport graph accepted as a render graph')
except ValueError:
    pass

captures = []
class RenderGraphTest(bpy.types.RenderEngine):
    bl_idname = 'WEB_RENDER_GRAPH_TEST'
    bl_label = 'Web Render Graph Test'
    def render(self, depsgraph):
        assert depsgraph.mode == 'RENDER'
        _blender_web.begin_render_export(depsgraph)
        try:
            try:
                _blender_web.begin_render_export(depsgraph)
                raise AssertionError('nested export accepted')
            except RuntimeError:
                pass
            frame = export()
            rows = {row['name']: row for row in frame['objects']}
            row = rows['RenderOnly']
            assert [row['matrix'][i][3] for i in range(3)] == [13, 7, 5], row
            assert row['render_visible'] is True, row
            native_count = len(modified.evaluated_get(depsgraph).data.vertices)
            key = rows['RenderModifier']['mesh']
            pulled = json.loads(_blender_web.export_mesh(json.dumps({'key': key})))['mesh']
            assert pulled['counts']['verts'] == native_count == 9, pulled
            captures.append({'hiddenMatrix': row['matrix'], 'renderVertices': native_count})
            # An exceptional owner still ends the scope in finally.
            raise LookupError('expected capture failure')
        except LookupError:
            pass
        finally:
            _blender_web.end_render_export()
        result = self.begin_result(0, 0, 8, 8)
        self.end_result(result)

bpy.utils.register_class(RenderGraphTest)
scene.render.engine = RenderGraphTest.bl_idname
for _ in range(2):
    bpy.ops.render.render()
    after = export(known)
    assert all(mesh.get('unchanged') for mesh in after['meshes'].values()), after['meshes']
    assert _blender_web.session_undo() is False, 'render replaced viewport undo graph'
assert collection.hide_viewport and not collection.hide_render
assert array.show_render and not array.show_viewport
assert len(captures) == 2
print('WEB_RENDER_GRAPH_PASS', json.dumps(captures))
