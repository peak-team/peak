from spack.package import *


class Peak(CMakePackage):
    """PEAK lightweight HPC profiler."""

    homepage = "https://github.com/peak-team/peak"
    git = "https://github.com/peak-team/peak.git"

    version("develop", branch="main")
    depends_on("frida-gum")
    depends_on("otf2", when="+otf2")

    variant("otf2", default=False, description="Enable OTF2 memory-trace export")

    def cmake_args(self):
        return [
            self.define("PEAK_FETCH_DEPS", False),
            self.define("PEAK_ENABLE_OTF2", self.spec.satisfies("+otf2")),
            self.define("FRIDA_GUM_INCLUDE_DIRS", self.spec["frida-gum"].prefix.include),
            self.define("FRIDA_GUM_LIBRARIES", self.spec["frida-gum"].libs[0]),
        ]
