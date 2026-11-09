/* Link this instead of the generated file to run the same host on the interpreter. */
#include "cpu8080.h"
void cpu_run(uint64_t limit) { interp_run(limit); }
const recomp_sym_t recomp_symbols[] = { { 0, 0 } };
const unsigned recomp_symbol_count = 0;
