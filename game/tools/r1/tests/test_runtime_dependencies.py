import os
import hashlib
import zipfile
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from r1.runtime_packages import repair_wheel


class RuntimeDependenciesTests(unittest.TestCase):
    def test_partial_and_corrupt_packages_are_repaired_offline(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            archive = root/"package.whl"
            with zipfile.ZipFile(archive,"w") as wheel:
                wheel.writestr("shapely/__init__.py","# package\n")
                wheel.writestr("shapely/geometry/__init__.py","# geometry\n")
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            target = root/"installed"
            self.assertEqual(repair_wheel(archive,target,digest),2)
            geometry = target/"shapely/geometry/__init__.py"
            geometry.unlink()
            self.assertEqual(repair_wheel(archive,target,digest),1)
            geometry.write_text("# broken!!\n",encoding="utf-8")  # same size, wrong CRC
            self.assertEqual(repair_wheel(archive,target,digest),1)
            self.assertEqual(geometry.read_text(),"# geometry\n")
            self.assertEqual(repair_wheel(archive,target,digest),0)

    def test_bundled_runtime_ignores_terminal_python_configuration(self):
        game = Path(__file__).resolve().parents[3]
        runtime = game/"generated/python-runtime/python.exe"
        env = os.environ.copy()
        env.update(PYTHONHOME="C:/does-not-exist",PYTHONPATH="C:/does-not-exist",PATH="")
        result = subprocess.run([str(runtime),str(game/"tools/check_world_python.py")],
                                env=env,cwd=game.parent,capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)

    def test_incomplete_external_shapely_cannot_shadow_project_installation(self):
        game = Path(__file__).resolve().parents[3]
        with tempfile.TemporaryDirectory() as folder:
            package = Path(folder)/"shapely"
            package.mkdir()
            (package/"__init__.py").write_text("# incomplete installation\n",encoding="utf-8")
            env = os.environ.copy()
            env["PYTHONPATH"] = folder
            result = subprocess.run([sys.executable,str(game/"tools/check_world_python.py")],
                                    env=env,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
