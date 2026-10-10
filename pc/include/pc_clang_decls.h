/* pc_clang_decls.h - forced into every TU of the Apple Clang build
 * (pc/CMakeLists.txt, -include). The decomp calls bcopy/bzero/bcmp without
 * declaring them; GCC tolerated the implicit declarations, Clang refuses
 * them. Same idea as the Android port's android_ansi_decls.h. */
#ifndef PC_CLANG_DECLS_H
#define PC_CLANG_DECLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int bcmp(const void* s1, const void* s2, size_t n);
void bcopy(const void* src, void* dst, size_t n);
void bzero(void* s, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* PC_CLANG_DECLS_H */
