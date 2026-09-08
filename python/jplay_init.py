import sys

# The naming-convention logic (metadata pickers + CREATE FROM DIRECTORY) lives in
# jplay_naming_core.py. This file claims its callbacks, then imports the
# site-owned jplay_naming_convention.py: registration is last-wins, so a callback
# that file registers replaces the shipped one, and everything it leaves alone
# stays on the shipped implementation (and keeps picking up its fixes). Sites
# customise in naming_convention.conf where they can and in that file where the
# config isn't enough.
#
# The host execs EVERY jplay_init.py it finds on sys.path (see PythonStartup.cpp),
# so a site deploying its own init alongside this one may already have established
# a convention by the time this runs. Claiming the callbacks then would land AFTER
# the site's registrations and silently replace them — hence the guard: if a
# convention module is already loaded, someone has set this up and this file has
# nothing to add.
if "jplay_naming_convention" not in sys.modules:
    import jplay_naming_core
    jplay_naming_core.register_defaults()
    import jplay_naming_convention  # noqa: F401

# Project lookup and naming-convention search, for the local control channel /
# Claude Code plugin. Same customisation story: override a callback, don't edit
# the module.
import jplay_discovery  # noqa: F401,E402
