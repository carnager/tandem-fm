# Contributing to Tandem

Thanks for helping! This page is the whole workflow, step by step, for
people new to Git and GitHub, and for coding agents (Codex, Claude, …) too.
The rules for the code itself are in [AGENTS.md](AGENTS.md); read both.

## The short version

- **Never push to `main`.** Only the maintainer does. You work on branches
  in your own fork.
- **One feature = one branch = one pull request**, based on the newest
  `main`. Small pull requests get merged; huge ones don't.
- **Ask first** (open an issue) before big or visible changes.
- Before you open the pull request: it builds, all tests pass, German is
  complete, the diff contains nothing else.

## Once: your fork

1. On GitHub, open <https://github.com/carnager/tandem-fm> and click
   **Fork**. You now have your own copy, `github.com/<you>/tandem-fm`.
2. Get it onto your computer and tell Git where the original is:

   ```sh
   git clone git@github.com:<you>/tandem-fm.git
   cd tandem-fm
   git remote add upstream https://github.com/carnager/tandem-fm.git
   ```

   `origin` is your fork (you push there), `upstream` is the original (you
   only read from it).

## Every feature: a new branch

Start every feature from the newest `main` of the original, never from
another feature branch:

```sh
git fetch upstream
git switch -c arrow-key-navigation upstream/main
```

Name the branch after the feature, in a few words with dashes.

Then work: change code, build, test (see below), and commit in small steps:

```sh
git add -A
git commit -m "Arrow keys walk folders: → into a folder, ← up"
```

A commit message says what changed for the user and why, in plain words.

### If you use Codex or another agent

- Give it **one feature per task**, and tell it to read `AGENTS.md` and
  `CONTRIBUTING.md` first.
- Before you commit anything it wrote, look at what it changed:

  ```sh
  git status            # which files
  git diff              # every changed line
  ```

  Throw away what doesn't belong to the feature
  (`git restore <file>` for a whole file). Typical unwanted extras: version
  bumps, notes or "validation" files, reformatting of untouched code,
  changes to how Linux copies or stores files.
- Ask it to explain any change you don't understand. If it can't say why
  a line is needed, the line probably isn't.

## Build and test

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target update_translations
grep -c 'type="unfinished"' translations/tandem-fm_de.ts   # must print 0
```

All tests must pass on your machine. If you can only test on macOS, say so
in the pull request; the maintainer tests on Linux.

## Keep the branch up to date

When `main` moved on while you worked, put your commits on top of it again
(rebase, don't merge):

```sh
git fetch upstream
git rebase upstream/main
```

If Git reports a conflict, open the files it names, keep the right parts
(remove the `<<<<<<<`, `=======`, `>>>>>>>` lines), then:

```sh
git add <files>
git rebase --continue
```

Unsure? `git rebase --abort` puts everything back as it was, and you can
ask in the pull request.

## Open the pull request

1. Check the whole change once more:

   ```sh
   git diff upstream/main --stat    # only files this feature needs?
   git diff upstream/main           # every line, one last time
   ```

2. Push the branch to your fork:

   ```sh
   git push -u origin arrow-key-navigation
   ```

3. GitHub shows a **Compare & pull request** button (or go to your fork →
   **Pull requests** → **New pull request**). Base: `carnager/tandem-fm`
   `main`. Compare: your branch.
4. Describe it:
   - **What** it does for the user, in two or three sentences.
   - **Why** it's useful.
   - **How you tested it** (which tests, which system: Linux or macOS).
   - **Screenshots** for anything visible.
   - Anything you weren't sure about.

   Not ready yet but want feedback? Choose **Create draft pull request**.

## After the review

Change what was asked on the same branch, commit, and push again; the pull
request updates by itself:

```sh
git add -A
git commit -m "Arrow keys: off for the thumbnail grid"
git push
```

After a rebase, a plain push is refused because the history changed; then:

```sh
git push --force-with-lease
```

Never open a second pull request for the same feature.

## Several features that build on each other

Better avoided: each pull request should stand alone, based on `main`. If
feature B really needs feature A, open A first, mention in B's
description that it depends on A, and keep B as a draft until A is merged.
Then rebase B on the new `main` (see above).

## When it was merged

```sh
git switch main
git fetch upstream
git reset --hard upstream/main     # your main = the original's main
git branch -d arrow-key-navigation
git push origin --delete arrow-key-navigation
```

Then start the next feature with a new branch, as above.
