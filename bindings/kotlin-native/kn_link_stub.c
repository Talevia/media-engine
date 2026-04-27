/*
 * Trivial translation unit so add_library(SHARED) has at least
 * one input. The actual symbols come from the whole-archive
 * load of libmedia_engine.a (see the corresponding CMakeLists.txt
 * `-Wl,-force_load` / `--whole-archive` lines). On macOS / Linux
 * a shared library with zero TU inputs is rejected by the
 * linker, so this empty unit is the cheapest legal placeholder.
 */
void me_kn_link_marker(void) {}
