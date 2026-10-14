# Publish the Techmo source

Suggested repository name: **techmo-bloodborne**.

The source is prepared for a new repository on branch `main`. Since the original
local `.git` directory was absent, this repository starts fresh; upstream origins
and source revisions are recorded in `NOTICE.md` and `DEPENDENCIES.md`.
No GitHub remote is chosen automatically.

Create an empty repository in your GitHub account, then run from this folder:

```powershell
git status --short
git add .
git diff --cached --stat
git commit -m "Introduce Techmo Bloodborne Windows workspace"
git remote add origin YOUR_GITHUB_REPOSITORY_URL
git push -u origin main
```

Replace `YOUR_GITHUB_REPOSITORY_URL` with the URL GitHub provides. Use your own
Git author configuration for the commit. If `origin` already exists, inspect it
with `git remote -v` and update it deliberately.

The `.gitignore` excludes local build output, game folders, settings, saves,
account caches and downloaded DLLs/model sets. Launcher source art is explicitly
included. Inspect the staged file list before publishing any additional files.

CMake/Ninja output, .NET/Python caches, personal IDE files and local capture
directories are also ignored. Vendored shader blobs, header-generation scripts,
dependency source, licenses and launcher/documentation artwork remain included.

Dependencies are vendored, so a normal clone or GitHub source ZIP is complete.
The Windows build workflow runs on pushes to `main` and `experiment/**` when
native, launcher, test, tool or workflow files change. Documentation-only pushes
do not trigger a build. To build a chosen branch manually, open GitHub Actions,
select **Techmo Windows**, choose **Run workflow**, and select that branch.
Download the `Techmo-Bloodborne-Windows-<commit SHA>` artifact from a successful
run. A launcher verification result does not replace native engine or in-game
validation.

Compiled packages should include `LICENSE`, `LICENSING.md`, `NOTICE.md`,
`DEPENDENCIES.md`, the retained license directories and the corresponding source.
