"""Derived native Mikk frames, default render UV and cached export layouts."""
import bpy,json,struct,tempfile,os
import _blender_web
previous=bpy.context.window.scene
scene=bpy.data.scenes.new('WebTangentTest');bpy.context.window.scene=scene
mesh=bpy.data.meshes.new('TangentSharedMesh')
materials=[];objects=[]
try:
    mesh.from_pydata([(-1,-1,0),(0,-1,.3),(1,-1,0),(-1,1,0),(0,1,.3),(1,1,0)],[],[(0,1,4,3),(1,2,5,4)])
    for face in mesh.polygons:face.use_smooth=True
    for name,mirror in [('ActiveUV',False),('RenderUV',True)]:
        uv=mesh.uv_layers.new(name=name)
        for loop in mesh.loops:
            p=mesh.vertices[loop.vertex_index].co
            uv.data[loop.index].uv=((1-p.x if mirror else p.x+1)*.5,(p.y+1)*.5)
    mesh.uv_layers.active_index=0;mesh.uv_layers['RenderUV'].active_render=True
    for name in ['Plain','Mapped']:
        mat=bpy.data.materials.new(name);materials.append(mat)
    mesh.materials.append(materials[0])
    for name,material in [('NoTangent',materials[0]),('WithTangent',materials[1])]:
        obj=bpy.data.objects.new(name,mesh);scene.collection.objects.link(obj);objects.append(obj)
        obj.material_slots[0].link='OBJECT';obj.material_slots[0].material=material
    original_attributes=[a.name for a in mesh.attributes]
    with tempfile.TemporaryDirectory() as folder:
        path=os.path.join(folder,'arena.bin')
        def export(names,known=None,defer=True):
            return json.loads(_blender_web.export_frame(json.dumps({'session':'tangent-test',
                'evaluate':True,'defer':defer,'known':known or {},'graph_tangents':names,'buffer_path':path})))
        frame=export(['Mapped']);rows={r['name']:r for r in frame['objects']}
        plain_key=rows['NoTangent']['mesh'];mapped_key=rows['WithTangent']['mesh']
        assert plain_key!=mapped_key,(plain_key,mapped_key)
        pulled=json.loads(_blender_web.export_mesh(json.dumps({'key':plain_key,'buffer_path':path})))['mesh']
        assert not any(a['name'].startswith('.blender.tangent:') for a in pulled['attributes'])
        pulled=json.loads(_blender_web.export_mesh(json.dumps({'key':mapped_key,'buffer_path':path})))['mesh']
        layers={a['name']:a for a in pulled['attributes'] if a['name'].startswith('.blender.tangent:')}
        assert set(layers)=={'.blender.tangent:', '.blender.tangent:ActiveUV','.blender.tangent:RenderUV'},layers
        with open(path,'rb') as file:arena=file.read()
        def values(layer):
            c=layer['data'];assert c['dtype']=='f32' and c['stride']==4
            return struct.unpack_from('<%df'%(c['count']*4),arena,c['offset'])
        assert values(layers['.blender.tangent:'])==values(layers['.blender.tangent:RenderUV'])
        evaluated=objects[1].evaluated_get(bpy.context.evaluated_depsgraph_get())
        copy=evaluated.to_mesh(preserve_all_data_layers=True,depsgraph=bpy.context.evaluated_depsgraph_get())
        try:
            for uv in ['ActiveUV','RenderUV']:
                copy.calc_tangents(uvmap=uv)
                expected=[v for loop in copy.loops for v in (*loop.tangent,loop.bitangent_sign)]
                actual=values(layers['.blender.tangent:'+uv])
                assert len(actual)==len(expected)
                assert max(abs(a-b) for a,b in zip(actual,expected))<1e-6,(uv,actual,expected)
        finally:evaluated.to_mesh_clear()
        known={'mesh:'+key:value['revision'] for key,value in frame['meshes'].items()}
        same=export(['Mapped'],known)
        assert all(m.get('unchanged') for m in same['meshes'].values())
        off=export([],known);off_rows={r['name']:r for r in off['objects']}
        assert off_rows['WithTangent']['mesh']==plain_key
        # Inline and deferred exports must use the same derived layout.
        inline=export(['Mapped'],defer=False)
        assert any(a['name']=='.blender.tangent:' for a in inline['meshes'][mapped_key]['attributes'])
    assert [a.name for a in mesh.attributes]==original_attributes,'export authored an attribute'
    print('WEB_TANGENT_PASS',json.dumps({'nativeUVLayers':2,'sharedLayoutIsolation':True,'knownRevisions':True,'authoredAttributesPreserved':True}))
finally:
    bpy.context.window.scene=previous
    for obj in objects:bpy.data.objects.remove(obj,do_unlink=True)
    bpy.data.scenes.remove(scene)
    if mesh.users==0:bpy.data.meshes.remove(mesh)
    for mat in materials:
        if mat.users==0:bpy.data.materials.remove(mat)
