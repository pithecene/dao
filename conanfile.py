from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain


class DaoConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        self.requires("boost-ext-ut/2.3.1")
        self.requires("cpp-httplib/0.18.3")
        self.requires("nlohmann_json/3.11.3")
        self.requires("llvm-core/19.1.7")

    def configure(self):
        self.options["llvm-core"].with_z3 = False
        self.options["llvm-core"].exceptions = True
        self.options["llvm-core"].rtti = True
        self.options["llvm-core"].targets = "AArch64" if self.settings.arch == "armv8" else "X86"
        self.options["llvm-core"].with_ffi = False
        self.options["llvm-core"].with_libedit = False
        self.options["llvm-core"].with_xml2 = False

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.generate()
