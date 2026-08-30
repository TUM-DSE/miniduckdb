# DuckDB as the miniOSv application.
#
# The kernel's top-level Makefile includes this file -- `include $(app)/Makefile`
# -- and links the objects it names in $(app-objects) into the kernel image.
# It is not a build driver: running `make` here does nothing useful.
#
#   make app=app/miniduckdb    from the miniOSv root, with this tree in place
#   make                       with this tree copied to the root's app/
#
# miniosv/miniosv.mk is the port; it locates the tree relative to itself, so
# either layout works. Upstream's own CMake driver is Makefile.upstream.
include $(dir $(lastword $(MAKEFILE_LIST)))miniosv/miniosv.mk
