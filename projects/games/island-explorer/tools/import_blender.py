"""Run inside Blender. Creates new editable scenes without removing existing ones."""
import bpy, json, math, runpy
from pathlib import Path
from mathutils import Vector

root=Path(__file__).resolve().parents[1]
assets=root/'assets/extracted'
manifest=json.loads((assets/'manifest.json').read_text())
for old in list(bpy.data.scenes):
    if old.name.startswith('Island Explorer - Emerald Coast'):
        bpy.data.scenes.remove(old)
materials=[]
for tex in manifest['textures']:
    mat=bpy.data.materials.new('IE / '+tex['name']);mat.use_nodes=True
    nt=mat.node_tree;nt.nodes.clear()
    output=nt.nodes.new('ShaderNodeOutputMaterial')
    shader=nt.nodes.new('ShaderNodeBsdfPrincipled');shader.inputs['Roughness'].default_value=0.9
    image=nt.nodes.new('ShaderNodeTexImage');image.image=bpy.data.images.load(str(assets/'textures'/tex['image']),check_existing=True)
    image.image.pack();image.interpolation='Linear'
    nt.links.new(image.outputs['Color'],shader.inputs['Base Color'])
    nt.links.new(image.outputs['Alpha'],shader.inputs['Alpha'])
    nt.links.new(shader.outputs['BSDF'],output.inputs['Surface'])
    materials.append(mat)
for act in range(2):
    scene=bpy.data.scenes.new(f'Island Explorer - Emerald Coast {act+1}')
    scene.world=bpy.data.worlds.new(f'Coastal sky {act+1}');scene.world.use_nodes=True
    scene.world.node_tree.nodes['Background'].inputs[0].default_value=(0.32,0.55,0.8,1)
    scene.world.node_tree.nodes['Background'].inputs[1].default_value=0.6
    visual=bpy.data.collections.new('World geometry and original props');scene.collection.children.link(visual)
    collision=bpy.data.collections.new('Collision only');scene.collection.children.link(collision)
    collision.hide_render=True;collision.hide_viewport=True
    for m in json.loads((assets/f'coast{act}.json').read_text()):
        mesh=bpy.data.meshes.new(m['name'])
        mesh.from_pydata([(v[0],-v[2],v[1]) for v in m['vertices']],[],m['triangles']);mesh.update()
        uv=mesh.uv_layers.new(name='Dreamcast UV')
        for loop in mesh.loops:
            vert=m['vertices'][loop.vertex_index];uv.data[loop.index].uv=(vert[3],1-vert[4])
        obj=bpy.data.objects.new(m['name'],mesh)
        (visual if m['surface']&0x80000000 else collision).objects.link(obj)
        obj['surface_flags']=hex(m['surface']);obj['material_flags']=hex(m['material_flags']);obj['texture_index']=m['texture']
        obj['source']='Sonic Adventure STG01; local GDI extraction'
        if m['texture']>=0:mesh.materials.append(materials[m['texture']])
    watermesh=bpy.data.meshes.new('Ocean plane')
    watermesh.from_pydata([(-4000,5000,-0.2),(12000,5000,-0.2),(12000,-4000,-0.2),(-4000,-4000,-0.2)],[],[(0,1,2,3)])
    obj=bpy.data.objects.new('Ocean - runtime animation is in KOS',watermesh);visual.objects.link(obj)
    water=bpy.data.materials.new('Lagoon turquoise');water.diffuse_color=(0.015,0.42,0.53,1);watermesh.materials.append(water)
    sun=bpy.data.lights.new('Late morning sun','SUN');sun.energy=2.2;sun.angle=math.radians(15)
    light=bpy.data.objects.new('Late morning sun',sun);scene.collection.objects.link(light);light.rotation_euler=(0.55,-0.4,-0.5)
    cam=bpy.data.cameras.new('Exploration camera');camera=bpy.data.objects.new('Exploration camera',cam);scene.collection.objects.link(camera)
    position=(-120,-280,90) if act==0 else (-1050,2920,1190)
    target=(110,-120,25) if act==0 else (-650,2700,1080)
    camera.location=position;camera.rotation_euler=(Vector(target)-camera.location).to_track_quat('-Z','Y').to_euler()
    cam.clip_end=20000;cam.lens=27;scene.camera=camera
    scene.render.engine='CYCLES';scene.cycles.samples=16
    scene.render.resolution_x=960;scene.render.resolution_y=720;scene.render.resolution_percentage=100
    scene.view_settings.view_transform='Standard'
    if act==0:bpy.context.window.scene=scene
runpy.run_path(str(root/'tools/finish_blender.py'))
print('Saved both editable Emerald Coast scenes:',root/'assets/island-explorer.blend')
