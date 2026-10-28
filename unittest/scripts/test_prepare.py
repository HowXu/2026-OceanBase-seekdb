"""Host dependency profile selection; run with unittest discovery."""

import unittest
from unittest.mock import patch

from deps.init import prepare


class HostProfileTest(unittest.TestCase):
    def host_profile(self, distribution, version, machine):
        release = 'ID=%s\nVERSION_ID="%s"\nID_LIKE=debian\n' % (
            distribution, version)
        with patch.object(prepare.platform, "system", return_value="Linux"), \
                patch.object(prepare.platform, "machine", return_value=machine), \
                patch.object(prepare.Path, "read_text", return_value=release):
            return prepare.host_profile()

    def test_kali_uses_el9_for_rolling_releases(self):
        for version in ("2026.1", "2025.4", "rolling", ""):
            for machine, arch in (("x86_64", "x86_64"), ("aarch64", "aarch64")):
                with self.subTest(version=version, machine=machine):
                    profile = self.host_profile("kali", version, machine)
                    self.assertEqual(
                        profile, prepare.PROFILES / ("oceanbase.el9.%s.deps" % arch))

    def test_debian_and_ubuntu_profiles_are_unchanged(self):
        for distribution, version, family in (
            ("debian", "9", "el7"),
            ("debian", "11", "el7"),
            ("debian", "12", "el9"),
            ("debian", "13", "el9"),
            ("ubuntu", "16.04", "el7"),
            ("ubuntu", "20.04", "el7"),
            ("ubuntu", "22.04", "el9"),
            ("ubuntu", "24.04", "el9"),
        ):
            for machine in ("x86_64", "aarch64"):
                with self.subTest(distribution=distribution, version=version, machine=machine):
                    self.assertEqual(
                        self.host_profile(distribution, version, machine),
                        prepare.PROFILES / ("oceanbase.%s.%s.deps" % (family, machine)))

    def test_unknown_debian_derivative_is_still_rejected(self):
        with self.assertRaisesRegex(ValueError, "No dependencies profile"):
            self.host_profile("unknown", "2026.1", "x86_64")


if __name__ == "__main__":
    unittest.main()
