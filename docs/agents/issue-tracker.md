# Issue tracker: GitHub

Issues and specs for this repo live as GitHub issues on `AlgusDark/oinkOS`. Use the `gh` CLI for all operations.

This clone has two GitHub remotes: `origin` (`AlgusDark/oinkOS`) and `upstream` (`crosspoint-reader/crosspoint-reader`). Always pass `--repo AlgusDark/oinkOS` so `gh` never prompts or targets upstream. Never create, comment on, label, or close issues in upstream.

## Conventions

- **Create an issue**: `gh issue create --repo AlgusDark/oinkOS --title "..." --body-file <file>`. Write multi-line bodies to a temporary file.
- **Read an issue**: `gh issue view <number> --repo AlgusDark/oinkOS --comments`, filtering comments by `jq` and also fetching labels.
- **List issues**: `gh issue list --repo AlgusDark/oinkOS --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'` with appropriate `--label` and `--state` filters.
- **Comment on an issue**: `gh issue comment <number> --repo AlgusDark/oinkOS --body "..."`
- **Apply / remove labels**: `gh issue edit <number> --repo AlgusDark/oinkOS --add-label "..."` / `--remove-label "..."`
- **Close**: `gh issue close <number> --repo AlgusDark/oinkOS --comment "..."`

## Pull requests as a triage surface

**PRs as a request surface: no.** _(Set to `yes` if this repo treats external PRs as feature requests; `/triage` reads this flag.)_

When set to `yes`, PRs run through the same labels and states as issues, using the `gh pr` equivalents (always with `--repo AlgusDark/oinkOS`):

- **Read a PR**: `gh pr view <number> --repo AlgusDark/oinkOS --comments` and `gh pr diff <number> --repo AlgusDark/oinkOS` for the diff.
- **List external PRs for triage**: `gh pr list --repo AlgusDark/oinkOS --state open --json number,title,body,labels,author,authorAssociation,comments` then keep only `authorAssociation` of `CONTRIBUTOR`, `FIRST_TIME_CONTRIBUTOR`, or `NONE` (drop `OWNER`/`MEMBER`/`COLLABORATOR`).
- **Comment / label**: `gh pr comment <number> --repo AlgusDark/oinkOS`, `gh pr edit <number> --repo AlgusDark/oinkOS --add-label`/`--remove-label`. Agents never open or close PRs (see `AGENTS.md`).

GitHub shares one number space across issues and PRs, so a bare `#42` may be either: resolve with `gh pr view 42 --repo AlgusDark/oinkOS` and fall back to `gh issue view 42 --repo AlgusDark/oinkOS`.

## When a skill says "publish to the issue tracker"

Create a GitHub issue on `AlgusDark/oinkOS`.

## When a skill says "fetch the relevant ticket"

Run `gh issue view <number> --repo AlgusDark/oinkOS --comments`.

## Wayfinding operations

Used by `/wayfinder`. The **map** is a single issue with **child** issues as tickets. All commands target `AlgusDark/oinkOS`.

- **Map**: a single issue labelled `wayfinder:map`, holding the Notes / Decisions-so-far / Fog body. `gh issue create --repo AlgusDark/oinkOS --label wayfinder:map`.
- **Child ticket**: an issue linked to the map as a GitHub sub-issue (`gh api` on the sub-issues endpoint). Where sub-issues aren't enabled, add the child to a task list in the map body and put `Part of #<map>` at the top of the child body. Labels: `wayfinder:<type>` (`research`/`prototype`/`grilling`/`task`). Once claimed, the ticket is assigned to the driving dev.
- **Blocking**: GitHub's **native issue dependencies**, the canonical, UI-visible representation. Add an edge with `gh api --method POST repos/AlgusDark/oinkOS/issues/<child>/dependencies/blocked_by -F issue_id=<blocker-db-id>`, where `<blocker-db-id>` is the blocker's numeric **database id** (`gh api repos/AlgusDark/oinkOS/issues/<n> --jq .id`, _not_ the `#number` or `node_id`). GitHub reports `issue_dependencies_summary.blocked_by` (open blockers only, the live gate). Where dependencies aren't available, fall back to a `Blocked by: #<n>, #<n>` line at the top of the child body. A ticket is unblocked when every blocker is closed.
- **Frontier query**: list the map's open children (`gh issue list --repo AlgusDark/oinkOS --state open`, scoped to the map's sub-issues / task list), drop any with an open blocker (`issue_dependencies_summary.blocked_by > 0`, or an open issue in the `Blocked by` line) or an assignee; first in map order wins.
- **Claim**: `gh issue edit <n> --repo AlgusDark/oinkOS --add-assignee @me`, the session's first write.
- **Resolve**: `gh issue comment <n> --repo AlgusDark/oinkOS --body "<answer>"`, then `gh issue close <n> --repo AlgusDark/oinkOS`, then append a context pointer (gist + link) to the map's Decisions-so-far.
