import unittest
from r1.geodesy import Anchor
from r1.sources import Bounds, ElevationGrid, OsmWay, OsmNode
from r1.streets import build_streets, sidewalk_sides, width
from r1.buildings import build_buildings
from r1.atlas import CHAMONIX
from r1.terrain import ground_point, build_terrain
from r1.mesh import Mesh
from r1.streets import smooth_surface


class StreetsTests(unittest.TestCase):
    def setUp(self):
        self.anchor = Anchor.at(0,0)
        self.grid = ElevationGrid(Bounds(-.001,-.001,.001,.001),2,((0.,0.),(20.,20.)))
        self.road = OsmWay(1,((-.0005,0.),(.0005,0.)),
                           {"highway":"residential","width":"6","sidewalk":"both"})

    def test_explicit_absence_and_separate_mapping_prevent_duplicates(self):
        for value in ("no","none","separate"):
            self.assertEqual(sidewalk_sides({"highway":"residential","sidewalk":value}),[])
        self.assertEqual(sidewalk_sides({"highway":"residential","sidewalk":"left"}),[("left",False)])
        self.assertEqual(width({"highway":"primary","width":"4.2"}),4.2)

    def test_no_roads_is_valid_empty_street_geometry(self):
        parts,stats = build_streets((),(),self.grid,self.anchor)
        self.assertEqual(parts,[])
        self.assertEqual(stats["carriagewayAreaM2"],0)
        self.assertEqual(stats["sidewalkAreaM2"],0)

    def test_only_one_surface_has_no_kerbs(self):
        for tags,expected in (({"highway":"footway"},"Sidewalks"),
                              ({"highway":"residential","sidewalk":"no"},"Carriageway")):
            with self.subTest(tags=tags):
                road = OsmWay(2,self.road.points,tags)
                parts,_ = build_streets((road,),(),self.grid,self.anchor)
                self.assertEqual([p.name for p in parts],[expected])

    def test_only_grade_separated_roads_has_no_ground_kerbs(self):
        road = OsmWay(2,self.road.points,{"highway":"primary","bridge":"yes"})
        parts,stats = build_streets((road,),(),self.grid,self.anchor)
        self.assertEqual(parts,[])
        self.assertEqual(stats["unsupportedGradeSeparatedWays"],1)

    def test_road_and_kerb_follow_transverse_slope(self):
        parts,_ = build_streets((self.road,),(),self.grid,self.anchor)
        road = next(p.mesh for p in parts if p.name=="Carriageway")
        self.assertGreater(max(p[1] for p in road.positions)-min(p[1] for p in road.positions),.4)
        kerb = next(p.mesh for p in parts if p.name=="Kerbs")
        self.assertTrue(kerb.indices)
        for x,y,z in road.positions:
            lo,la,_ = self.anchor.engine_to_geodetic(x,0,z)
            expected = ground_point(lo,la,self.grid,self.anchor)[1]+.06
            self.assertAlmostEqual(y,expected,delta=.0001)  # float mesh precision, 0.1 mm

    def test_zebra_requires_surveyed_marking(self):
        for marking,expected in (("zebra",1),("no",0),("yes",0),("",0)):
            feature = OsmNode(2,0,0,{"highway":"crossing","crossing:markings":marking})
            parts,stats = build_streets((self.road,),(feature,),self.grid,self.anchor)
            self.assertEqual(stats["zebraCrossingsTagged"],expected)
            self.assertEqual(any(p.name=="Surveyed zebra crossings" for p in parts),bool(expected))

    def test_foundation_closes_downhill_gap_without_tilting_roof(self):
        way = OsmWay(1,((0.,0.),(20.,0.),(20.,20.),(0.,20.),(0.,0.)),
                     {"building":"yes","height":"12","roof:shape":"flat"})
        parts,_,_ = build_buildings((way,),lambda x,z:(x,x*.2,z),CHAMONIX,detail_radius=-1)
        foundations = [p for p in parts if p.name.startswith("Foundations")]
        self.assertTrue(foundations)
        self.assertLess(min(v[1] for p in foundations for v in p.mesh.positions),0.)
        roofs = [v[1] for p in parts if p.name.startswith("Roofs") for v in p.mesh.positions]
        self.assertLess(max(roofs)-min(roofs),.5)
        self.assertAlmostEqual(min(roofs),14.)  # mean ground 2 m + measured height 12 m

    def test_ground_matches_terrain_triangle_in_a_saddle(self):
        # The source is bilinear; the visible mesh uses SW-SE-NE / SW-NE-NW.
        grid = ElevationGrid(Bounds(0,0,.001,.001),2,((0.,0.),(0.,80.)))
        p = ground_point(.00001,.000005,grid,self.anchor)
        mesh = build_terrain(grid.bounds,grid,self.anchor)[""]
        ids = mesh.indices[:3]
        corners = [mesh.positions[i] for i in ids]
        # First cell at u=.4, v=.2; SW/SE/NE weights .6/.2/.2.
        expected = sum(w*v[1] for w,v in zip((.6,.2,.2),corners))
        self.assertAlmostEqual(p[1],expected,places=6)

    def test_ground_does_not_collapse_footprints_crossing_tile_boundary(self):
        p = ground_point(.002,0.,self.grid,self.anchor)
        edge = ground_point(.001,0.,self.grid,self.anchor)
        self.assertGreater(p[0]-edge[0],100.)

    def test_intersecting_streets_do_not_stack_sidewalks_on_carriageway(self):
        crossing = OsmWay(3,((0.,-.0005),(0.,.0005)),self.road.tags)
        parts,stats = build_streets((self.road,crossing),(),self.grid,self.anchor)
        self.assertLess(stats["surfaceOverlapM2"],.001)
        self.assertTrue(any(p.name=="Kerbs" for p in parts))

    def test_pavement_is_cut_out_of_building_footprints(self):
        # The building cuts a large hole out of the otherwise continuous strip.
        hole = [(-10,-10),(10,-10),(10,10),(-10,10)]
        parts,_ = build_streets((self.road,),(),self.grid,self.anchor,(hole,))
        for part in parts:
            if part.name not in {"Carriageway","Sidewalks"}:
                continue
            for i in range(0,len(part.mesh.indices),3):
                pts=[part.mesh.positions[j] for j in part.mesh.indices[i:i+3]]
                x,z = sum(p[0] for p in pts)/3,sum(p[2] for p in pts)/3
                self.assertFalse(-9.99<x<9.99 and -9.99<z<9.99)

    def test_smoothing_shares_vertices_without_removing_triangles(self):
        mesh=Mesh()
        mesh.add_up_quad((0,0,0),(1,0,0),(1,.2,1),(0,0,1))
        smooth=smooth_surface(mesh)
        self.assertEqual(len(smooth.positions),4)
        self.assertEqual([mesh.positions[i] for i in mesh.indices],
                         [smooth.positions[i] for i in smooth.indices])
