#ifndef ARCHOS_LIB_BUILD_COMMIT_H
#define ARCHOS_LIB_BUILD_COMMIT_H

/* The git commit this kernel was built from (M1-04c): a 12-digit hash, plus
 * "-dirty" when tracked files had changes, or "unknown" outside git. Written
 * on every build by cmake/build-commit.cmake. The boot report's first line
 * and every QR page header carry it, so a hardware result names its build. */
extern const char arc_build_commit[];

#endif /* ARCHOS_LIB_BUILD_COMMIT_H */
