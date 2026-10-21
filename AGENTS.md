# Project workflow

## Commit and push after firmware builds

The user has authorized the following workflow for future sessions:

- After every successful firmware build, review the changes and check results, commit the intended source changes, and push to `origin/main` without asking for confirmation again.
- Include the J Poi build identifier in the commit message.
- Stage only intended project changes. Preserve unrelated user edits and do not commit secrets or ignored/generated firmware binaries unless explicitly requested.
- If there are no intended source changes, do not create an empty commit; report the new local build instead.
- Report the build identifier, commit hash, push outcome, and any failed checks. Do not describe a failing check as passed.
- A failed firmware build does not trigger this automatic commit-and-push workflow.
