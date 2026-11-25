import os

from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain
from conan.tools.files import copy


class DaoConan(ConanFile):
    name = "dao"
    license = "Apache-2.0 WITH LLVM-exception"
    exports = "LICENSE", "THIRD_PARTY_NOTICES.md"
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

    def package(self):
        licenses_dir = os.path.join(self.package_folder, "licenses")
        copy(self, "LICENSE", self.source_folder, licenses_dir)
        copy(self, "THIRD_PARTY_NOTICES.md", self.source_folder, licenses_dir)
