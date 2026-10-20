/* Stand-in for <x86intrin.h> on non-x86 hosts (Apple silicon), found before the
 * compiler's own through the include path (cmake/mvme_parts.cmake). The pinned
 * external/mvme/src/analysis/a2/util/bits.h:32 includes <x86intrin.h> without a
 * guard but only uses it under __BMI2__ (:42), which x86 compilers define only
 * with -mbmi2 and which is never defined here. Both builds take the portable
 * branches (:44-54, :89-93), so the header has nothing to provide. */
#ifndef FLIRT_NON_X86_X86INTRIN_H
#define FLIRT_NON_X86_X86INTRIN_H
#endif
