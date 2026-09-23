"""The world scene ships the animated player, and regenerating it keeps it."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from r1 import prepare_world


GAME = Path(__file__).resolve().parents[3]


class PlayerScene(unittest.TestCase):
    PLAYER_MODEL = 'assets/models/characters/player.glb'

    def check_player(self, scene):
        player = next(n for n in scene['scene']['children'] if 'player' in n.get('groups', []))
        body = next(n for n in player['children'] if n.get('importedFrom'))
        self.assertEqual(body['importedFrom'], self.PLAYER_MODEL)
        self.assertTrue((GAME/body['importedFrom']).is_file())
        return body

    def test_shipped_scene_has_player(self):
        self.check_player(json.loads((GAME/'scenes/earth.scene').read_text(encoding='utf-8')))

    def test_regenerating_world_keeps_original_player(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'scenes').mkdir()
            with patch.object(prepare_world, 'GAME', root), patch.object(prepare_world, 'basemap'):
                prepare_world.main()
            regenerated = self.check_player(json.loads((root/'scenes/earth.scene').read_text(encoding='utf-8')))
        shipped = self.check_player(json.loads((GAME/'scenes/earth.scene').read_text(encoding='utf-8')))
        self.assertEqual(regenerated['transform']['rotation'], shipped['transform']['rotation'])
