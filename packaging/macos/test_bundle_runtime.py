"""Portable boundary checks; actual Mach-O relocation is verified on macOS CI."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("bundle_runtime", Path(__file__).with_name("bundle_runtime.py"))
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


class RuntimeResolutionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.llvm = self.root / "LLVM with spaces"
        self.library = self.llvm / "lib/c++/libc++.1.dylib"
        self.library.parent.mkdir(parents=True)
        self.library.write_bytes(b"fixture")
        self.executable = self.root / "package with spaces/bin/music-service"

    def resolve(self, name, rpaths=(), executable_rpaths=()):
        return bundle.resolve_dependency(name, self.library, self.executable, rpaths, executable_rpaths, self.llvm)

    def test_absolute_runtime_with_spaces(self):
        self.assertEqual(self.resolve(str(self.library)), self.library)

    def test_loader_relative_runtime(self):
        self.assertEqual(self.resolve("@loader_path/libc++.1.dylib"), self.library)

    def test_rpath_from_executable_is_available_to_dependency(self):
        self.assertEqual(self.resolve("@rpath/libc++.1.dylib", executable_rpaths=[str(self.library.parent)]), self.library)

    def test_bare_loader_path_in_llvm_runtime_rpath(self):
        self.assertEqual(self.resolve("@rpath/libc++.1.dylib", rpaths=["@loader_path"]), self.library)

    def test_bare_executable_path_is_a_directory(self):
        self.assertEqual(bundle.expand_path("@executable_path", self.library, self.executable), self.executable.parent)

    def test_unknown_runtime_is_not_silently_bundled(self):
        with self.assertRaisesRegex(ValueError, "Unreviewed"):
            self.resolve(str(self.llvm / "lib/libzstd.1.dylib"))

    def test_absolute_runtime_outside_selected_toolchain_is_rejected(self):
        outside = self.root / "libc++abi.1.dylib"
        outside.write_bytes(b"fixture")
        with self.assertRaisesRegex(ValueError, "outside"):
            self.resolve(str(outside))

    def test_missing_runtime_is_not_ignored(self):
        with self.assertRaisesRegex(ValueError, "Cannot resolve"):
            self.resolve("@rpath/libc++abi.1.dylib", rpaths=[str(self.library.parent)])

    def test_macho_identity_is_not_a_dependency(self):
        commands = """
Load command 1
          cmd LC_ID_DYLIB
      cmdsize 64
         name @rpath/libc++.1.dylib (offset 24)
Load command 2
          cmd LC_RPATH
      cmdsize 40
         path @loader_path/../unwind (offset 12)
Load command 3
          cmd LC_BUILD_VERSION
      cmdsize 32
     platform 1
        minos 15.0
          sdk 15.5
"""
        libraries = """fixture:
    @rpath/libc++.1.dylib (compatibility version 1.0.0, current version 1.0.0)
    @rpath/libc++abi.1.dylib (compatibility version 1.0.0, current version 1.0.0)
    /usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1.0.0)
"""
        with patch.object(bundle, "run", side_effect=[commands, libraries]):
            dependencies, rpaths, minimum = bundle.inspect(self.library)
        self.assertEqual(dependencies, ["@rpath/libc++abi.1.dylib", "/usr/lib/libSystem.B.dylib"])
        self.assertEqual(rpaths, ["@loader_path/../unwind"])
        self.assertEqual(minimum, "15.0")


if __name__ == "__main__":
    unittest.main()
