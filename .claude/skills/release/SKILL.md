---
name: release
description: Close and publish a SavanXP version (release commit, release ISO, checks, tag, GitHub release) following docs/build/RELEASING.md. Use only when the user asks to release a version.
disable-model-invocation: true
---

# Releasing a SavanXP version

The procedure is [`docs/build/RELEASING.md`](../../../docs/build/RELEASING.md).
Read it whole before the first command; this skill only adds how an agent
runs it.

1. **Ask for what was not given:** the version `X.Y.Z`, which must be the one
   `include/shared/version.h` already carries, and the release name.
2. **Check the preconditions** at the top of `RELEASING.md`: a full clone, on
   `master`, a clean tree that matches `origin/master`, and the area checks of
   `AGENTS.md` passed on it. If one fails, say which and stop.
3. **Run steps 1 to 3 locally:** the release commit, with the changelog move
   snippet; the release image in `build/release-X.Y.Z`; and every check of
   step 3. A failing check stops the release: report it as it printed, and
   the fix goes to `master` before anything else.
4. **Stop before step 4** and show the user the release commit, each check as
   run with what it printed, and the release notes drafted per step 6 from
   `docs/changelog/vX.Y.Z.md`. The tag, the push and `gh release create` are
   public and cannot be taken back: run them only when the user says so, in
   the order of steps 4, 5 and 7.
5. **After publishing**, confirm that `gh release view vX.Y.Z` lists the four
   assets and that the downloaded `SHA256SUMS` verifies them. Then remind the
   user about the website: its changelog pages follow `docs/changelog/` by
   themselves, but the highlight line and the status data are written by
   hand for each release.
