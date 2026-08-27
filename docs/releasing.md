# Release Checklist

PEAK releases use annotated `vMAJOR.MINOR.PATCH` tags and attach a deterministic
source archive plus its SHA-256 checksum. GitHub's generated source links are
convenience downloads; package recipes must use the attached archive and
recorded checksum. Repository release immutability must be enabled before the
first published release so its tag and assets are locked on publication. See
[GitHub's immutable release documentation](https://docs.github.com/en/code-security/how-tos/secure-your-supply-chain/establish-provenance-and-integrity/prevent-release-changes).

For a release version `1.0.0`:

1. Set `project(peak VERSION 1.0.0 ...)`, update release-facing documentation,
   and merge a PR whose complete CI is green.
2. From the exact merged commit, verify a production `Release` install with
   `BUILD_TESTING=OFF`, then configure, build, and run `test/package_consumer`
   using only that install prefix.
3. Verify dependency-disabled and enabled builds do not export source paths,
   headers, or private MPI, CUDA, OTF2, or Frida Gum requirements.
4. In repository settings, confirm that **Enable release immutability** is
   enabled. This setting protects only releases published after it is enabled.
5. Create and push the annotated tag:

   ```bash
   expected_commit="$(git rev-parse HEAD)"
   git tag -a v1.0.0 -m "PEAK 1.0.0"
   test "$(git rev-parse 'v1.0.0^{commit}')" = "$expected_commit"
   git push origin v1.0.0
   remote_commit="$(
     git ls-remote origin 'refs/tags/v1.0.0^{}' | awk '{print $1}'
   )"
   test "$remote_commit" = "$expected_commit"
   ```

6. Produce the deterministic release assets from the tag:

   ```bash
   git archive --format=tar --prefix=peak-1.0.0/ v1.0.0 \
     | gzip -n > peak-1.0.0.tar.gz
   sha256sum peak-1.0.0.tar.gz > peak-1.0.0.tar.gz.sha256
   ```

7. Create a draft release, attach both files, and record the checksum in the
   release notes:

   ```bash
   gh release create v1.0.0 --draft --verify-tag \
     --title "PEAK 1.0.0" --notes-file release-notes.md \
     peak-1.0.0.tar.gz peak-1.0.0.tar.gz.sha256
   ```

8. Download the draft assets into a clean directory, compare them byte for byte
   with the locally verified assets, verify the checksum, and repeat the
   controlled/offline production install and external preload consumer test.
   Reconfirm both the local and remote tag commits immediately before
   publication. Do not publish a release whose downloaded assets or tag differ.

   ```bash
   mkdir release-verify
   gh release download v1.0.0 --dir release-verify \
     --pattern 'peak-1.0.0.tar.gz*'
   cmp peak-1.0.0.tar.gz release-verify/peak-1.0.0.tar.gz
   cmp peak-1.0.0.tar.gz.sha256 \
     release-verify/peak-1.0.0.tar.gz.sha256
   (cd release-verify && sha256sum --check peak-1.0.0.tar.gz.sha256)
   expected_commit="$(git rev-parse HEAD)"
   test "$(git rev-parse 'v1.0.0^{commit}')" = "$expected_commit"
   remote_commit="$(
     git ls-remote origin 'refs/tags/v1.0.0^{}' | awk '{print $1}'
   )"
   test "$remote_commit" = "$expected_commit"
   ```

9. Publish the fully verified draft. Release immutability takes effect only at
   publication:

   ```bash
   gh release edit v1.0.0 --draft=false
   test "$(gh release view v1.0.0 --json isImmutable --jq .isImmutable)" = true
   gh release verify v1.0.0
   gh release verify-asset v1.0.0 peak-1.0.0.tar.gz
   gh release verify-asset v1.0.0 peak-1.0.0.tar.gz.sha256
   ```

   These commands use GitHub's signed release attestations to verify the
   published release and local asset bytes. Only after they pass should Spack
   and EasyBuild recipes use the recorded checksum.

Do not tag a PR head or mutable branch. A release tag must identify the exact
commit on the protected main branch that produced the attached archive.
