"""Finish only Island Explorer's generated scenes; run inside Blender."""
import bpy
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
for act in range(2):
    scene = bpy.data.scenes.get(f'Island Explorer - Emerald Coast {act + 1}')
    if scene is None:
        continue
    world = scene.world.node_tree
    world.nodes.clear()
    output = world.nodes.new('ShaderNodeOutputWorld')
    ambient = world.nodes.new('ShaderNodeBackground')
    ambient.inputs[0].default_value = (.8, .86, 1, 1)
    ambient.inputs[1].default_value = .55
    sky = world.nodes.new('ShaderNodeBackground')
    sky.inputs[0].default_value = (.25, .52, .8, 1)
    path = world.nodes.new('ShaderNodeLightPath')
    mix = world.nodes.new('ShaderNodeMixShader')
    world.links.new(path.outputs['Is Camera Ray'], mix.inputs[0])
    world.links.new(ambient.outputs[0], mix.inputs[1])
    world.links.new(sky.outputs[0], mix.inputs[2])
    world.links.new(mix.outputs[0], output.inputs[0])
    for obj in scene.objects:
        if obj.type == 'LIGHT' and obj.data.type == 'SUN':
            obj.data.energy = 1.4
            obj.data.color = (1, .96, .86)
    source = json.loads((root / f'assets/extracted/coast{act}.json').read_text())
    # Scene iteration visits the visual collection before collision-only meshes.
    source.sort(key=lambda m: not bool(m['surface'] & 0x80000000))
    objects = [o for o in scene.objects if 'surface_flags' in o]
    if len(objects) != len(source):
        raise ValueError('Scene object count differs from the import')
    for obj, record in zip(objects, source):
        if len(obj.data.vertices) != len(record['vertices']):
            raise ValueError('Scene no longer matches the import; preserve edits before regenerating')
        attr = obj.data.color_attributes.get('Dreamcast color')
        if attr is None:
            attr = obj.data.color_attributes.new(name='Dreamcast color', type='BYTE_COLOR', domain='POINT')
        colors = []
        for v in record['vertices']:
            c = v[5]
            colors.extend(((c >> 16 & 255) / 255, (c >> 8 & 255) / 255,
                           (c & 255) / 255, (c >> 24 & 255) / 255))
        attr.data.foreach_set('color_srgb', colors)
    ocean = next(o for o in scene.objects if o.name.startswith('Ocean - runtime'))
    mesh = ocean.data
    uv = mesh.uv_layers.get('Ocean UV') or mesh.uv_layers.new(name='Ocean UV')
    for loop in mesh.loops:
        p = mesh.vertices[loop.vertex_index].co
        uv.data[loop.index].uv = (p.x / 180, p.y / 180)
    material = mesh.materials[0]
    material.use_nodes = True
    nodes = material.node_tree.nodes
    shader = nodes.get('Principled BSDF')
    shader.inputs['Roughness'].default_value = .28
    image = nodes.get('Ocean texture') or nodes.new('ShaderNodeTexImage')
    image.name = 'Ocean texture'
    path = root / 'assets/extracted/textures/BEACH_SEA_000_m128_sea001.png'
    image.image = bpy.data.images.load(str(path), check_existing=True)
    image.image.pack()
    material.node_tree.links.new(image.outputs['Color'], shader.inputs['Base Color'])
    scene.camera.data.clip_end = 40000

for area in bpy.context.screen.areas:
    if area.type == 'VIEW_3D':
        space = area.spaces.active
        space.clip_end = 40000
        space.region_3d.view_perspective = 'CAMERA'
        space.shading.type = 'MATERIAL'
        space.shading.use_scene_world = True
        space.shading.use_scene_lights = True
        space.overlay.show_overlays = False
bpy.ops.wm.save_as_mainfile(filepath=str(root / 'assets/island-explorer.blend'))
print('Saved textured water, original vertex colors and the clean Blender preview')
