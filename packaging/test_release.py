"""Release gates: all platforms, hashes and safe filenames, using unittest only."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("release", Path(__file__).with_name("release.py"))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_tampered_download_is_rejected(self):
        file = self.root / "package.zip"
        file.write_bytes(b"tested")
        release.checksums(self.root)
        self.assertIn(file.name, release.verify(self.root))
        file.write_bytes(b"other")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            release.verify(self.root)

    def test_path_and_duplicate_entries_are_rejected(self):
        for name in ("../outside", "C:outside", "sub/file", "sub\\file", "SHA256SUMS"):
            (self.root / "SHA256SUMS").write_text("0" * 64 + "  " + name + "\n")
            with self.assertRaises(ValueError):
                release.verify(self.root)
        (self.root / "ok").write_text("ok")
        release.checksums(self.root)
        manifest = self.root / "SHA256SUMS"
        manifest.write_text(manifest.read_text() * 2)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            release.verify(self.root)

    def test_missing_platform_is_rejected_before_output(self):
        with self.assertRaises(FileNotFoundError):
            release.assemble(self.root, self.root / "release", "a" * 40)
        self.assertFalse((self.root / "release").exists())

    def test_full_bundle_and_download_verification(self):
        for target, extension in release.TARGETS.items():
            directory = self.root / f"music-service-{target}"
            directory.mkdir()
            (directory / f"music-service-{release.version()}-{target}{extension}").write_bytes(b"tested")
            release.checksums(directory)
        output = self.root / "release"
        release.assemble(self.root, output, "b" * 40)
        self.assertEqual(len(release.verify(output)), 7)
        with self.assertRaisesRegex(ValueError, "empty"):
            release.assemble(self.root, output, "b" * 40)


if __name__ == "__main__":
    unittest.main()
