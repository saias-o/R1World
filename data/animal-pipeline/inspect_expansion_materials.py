import sys,json,bpy
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from prepare_0ad import ROOT,QA
for n in ['shrike','bk_wasp_rigged_animated_fur_particles','bk_spider_rigged_animated_fur','bk_lizard_template']:
 r=json.loads((QA/'expansion'/(n+'.json')).read_text());bpy.ops.wm.open_mainfile(filepath=str(ROOT/r['source_file']),load_ui=False,use_scripts=False)
 print('SOURCE',n,flush=True)
 for o in bpy.data.objects:
  if o.type=='MESH':print('MESH',o.name,'UV',[u.name for u in o.data.uv_layers],flush=True)
 for m in bpy.data.materials:
  print('MATERIAL',m.name,m.diffuse_color[:],m.use_nodes,flush=True)
  if m.node_tree:
   for n in m.node_tree.nodes:
    print(n.name,n.type,[(i.name,list(i.default_value) if hasattr(i.default_value,'__iter__') else i.default_value) for i in n.inputs if hasattr(i,'default_value')][:3],getattr(n,'image',None),flush=True)
   print('LINKS',[(l.from_node.name,l.from_socket.name,l.to_node.name,l.to_socket.name) for l in m.node_tree.links],flush=True)
