# Agent Instructions

This project uses **bd** (beads) for issue tracking. Run `bd onboard` to get started.

## Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --status in_progress  # Claim work
bd close <id>         # Complete work
```

## Landing the Plane (Session Completion)

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   git add .beads/issues.jsonl && git commit -m "bd: update issues"
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds

## Nostr Protocol Guidelines

This project is Nostr-native — keep it event-driven, not request/response or polling:

- Subscribe with `REQ` filters and react to `EVENT`/`EOSE`; never poll relays on a timer or close a subscription after an arbitrary delay.
- Check every publish: don't ignore `OK`/`OK false`, `CLOSED` reasons, or `AUTH` (NIP-42) challenges — a successful websocket send is not a successful publish.
- Backfill with `since`/`until`/`limit` and a clear catch-up → live boundary instead of sleeping and hoping for history.
- Scope filters tightly (kinds/authors/tags) rather than pulling broad sets and filtering client-side.
- Dedupe by `event.id` across relays/reconnects; handlers must be idempotent on replay.
- Probe relay capabilities with NIP-11 instead of hardcoding single-relay assumptions; reconnect with backoff and re-issue subscriptions.
- Before merging Nostr-touching changes, run a **Nostr Protocol Review** (the `nostr-protocol-smells` skill / `nostr-protocol-review` workflow) to catch polling, timeout-closes, ignored relay frames, and other protocol smells.

## Git Worktree & Branch Hygiene

- Once a feature branch's changes are merged into `main`/`master`, remove the worktree and delete the branch. Don't leave merged worktrees or stale branches lying around.
- Before removing a worktree, run `git status` inside it to confirm there's nothing uncommitted you'd lose — commit, stash, or discard first.
- Clean up with `git worktree remove <path>` and `git branch -d <branch>` (add `git push origin --delete <branch>` if the branch was pushed).
