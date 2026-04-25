/*
 * VersionAssert — pin JNI nativeVersion() to the build-time C
 * version macros. Exit 0 when MediaEngine.version() matches the
 * 4 args (major, minor, patch, gitSha); exit 1 with a diff
 * otherwise. Drives the jni_version_assert_smoke ctest, which
 * passes the same values that feed src/core/version.inl.in.
 *
 * Why this is needed: cycle 70's jni_load_smoke proved the
 * nativeVersion symbol resolves; cycle 74's jni_passthrough_smoke
 * proved the render path round-trips. Neither asserts the
 * *values* nativeVersion returns. A future engine-version bump
 * (or a refactor that drops the git_sha field) could leave the
 * Java side returning stale data without any test catching it.
 *
 * Usage:
 *   java -Djava.library.path=<libdir> \
 *        -cp <classes-dir> \
 *        io.mediaengine.example.VersionAssert \
 *        <expectedMajor> <expectedMinor> <expectedPatch> <expectedGitSha>
 *
 * gitSha is compared verbatim (including any "-dirty" suffix).
 * Pass the empty string to skip the gitSha check (useful when
 * the build was configured outside a git checkout).
 */
package io.mediaengine.example;

import io.mediaengine.MediaEngine;

public final class VersionAssert {

    public static void main(String[] args) {
        if (args.length < 4) {
            System.err.println("usage: VersionAssert <major> <minor> <patch> <gitSha>");
            System.exit(2);
        }
        final int    expMajor = Integer.parseInt(args[0]);
        final int    expMinor = Integer.parseInt(args[1]);
        final int    expPatch = Integer.parseInt(args[2]);
        final String expSha   = args[3];

        final MediaEngine.Version v = MediaEngine.version();

        boolean ok = true;
        if (v.major() != expMajor) {
            System.err.printf("major mismatch: expected=%d got=%d%n", expMajor, v.major());
            ok = false;
        }
        if (v.minor() != expMinor) {
            System.err.printf("minor mismatch: expected=%d got=%d%n", expMinor, v.minor());
            ok = false;
        }
        if (v.patch() != expPatch) {
            System.err.printf("patch mismatch: expected=%d got=%d%n", expPatch, v.patch());
            ok = false;
        }
        /* Empty expSha == caller chose to skip; otherwise compare verbatim. */
        if (!expSha.isEmpty() && !expSha.equals(v.gitSha())) {
            System.err.printf("gitSha mismatch: expected='%s' got='%s'%n", expSha, v.gitSha());
            ok = false;
        }

        if (!ok) {
            System.err.println("VersionAssert FAILED");
            System.exit(1);
        }
        System.out.printf("VersionAssert OK: %d.%d.%d (%s)%n",
                v.major(), v.minor(), v.patch(),
                v.gitSha().isEmpty() ? "<unknown>" : v.gitSha());
    }
}
