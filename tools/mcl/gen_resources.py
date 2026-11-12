"""Regenerate MCL's compressed resource tables (menus, menu options, machine names, icons) for the Blackbox build.

MCL's own build does this in a PlatformIO extra script (tools/platform/generate_resources.py in the MCL checkout) and commits the
output per platform (src/resources/avr, src/resources/rp2040). A branch that only regenerated one platform's tables leaves the other
stale: the menus then show the wrong labels and options. This runs that same script with our toolchain and compile flags (the
"rp2040" layout: ARM, 4-byte alignment), writing src/resources/rp2040 in the checkout; it skips the work when nothing changed.
usage: python3 tools/mcl/gen_resources.py <MCL checkout> <build dir>   (compile flags in $MCLRES_FLAGS)
"""
import os
import sys

mcl, build = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
flags = os.environ.get("MCLRES_FLAGS", "")


class Env:
    def subst(self, s):
        return {"$PIOENV": "rp2040", "$CC": "arm-none-eabi-gcc", "$PROJECT_DIR": mcl, "$BUILD_DIR": build,
                "$CCFLAGS $CXXFLAGS $_CPPDEFFLAGS $_CPPINCFLAGS": flags, "$CUSTOM_PRIVATE_KEY": ""}.get(s, s.replace("$PROJECT_DIR", mcl))

    def Exit(self, code):
        sys.exit(code)

    def AddPostAction(self, *a):
        pass


env = Env()
os.makedirs(build, exist_ok=True)
script = os.path.join(mcl, "tools", "platform", "generate_resources.py")
src = open(script).read()
exec(compile(src, script, "exec"), {"Import": lambda name: None, "env": env, "__name__": "generate_resources"})
