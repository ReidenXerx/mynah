<!-- gitnexus:start -->
# GitNexus — Code Intelligence

This project is indexed by GitNexus as **mynah** (2186 symbols, 5055 relationships, 143 execution flows). Use the GitNexus MCP tools to understand code, assess impact, and navigate safely.

> Index stale? Run `node .gitnexus/run.cjs analyze` from the project root — it auto-selects an available runner. No `.gitnexus/run.cjs` yet? `npx gitnexus analyze` (npm 11 crash → `npm i -g gitnexus`; #1939).

## Always Do

- **MUST run impact analysis before editing any symbol.** Before modifying a function, class, or method, run `impact({target: "symbolName", direction: "upstream"})` and report the blast radius (direct callers, affected processes, risk level) to the user.
- **MUST run `detect_changes()` before committing** to verify your changes only affect expected symbols and execution flows. For regression review, compare against the default branch: `detect_changes({scope: "compare", base_ref: "main"})`.
- **MUST warn the user** if impact analysis returns HIGH or CRITICAL risk before proceeding with edits.
- When exploring unfamiliar code, use `query({search_query: "concept"})` to find execution flows instead of grepping. It returns process-grouped results ranked by relevance.
- When you need full context on a specific symbol — callers, callees, which execution flows it participates in — use `context({name: "symbolName"})`.
- For security review, `explain({target: "fileOrSymbol"})` lists taint findings (source→sink flows; needs `analyze --pdg`).

## Never Do

- NEVER edit a function, class, or method without first running `impact` on it.
- NEVER ignore HIGH or CRITICAL risk warnings from impact analysis.
- NEVER rename symbols with find-and-replace — use `rename` which understands the call graph.
- NEVER commit changes without running `detect_changes()` to check affected scope.

## Resources

| Resource | Use for |
|----------|---------|
| `gitnexus://repo/mynah/context` | Codebase overview, check index freshness |
| `gitnexus://repo/mynah/clusters` | All functional areas |
| `gitnexus://repo/mynah/processes` | All execution flows |
| `gitnexus://repo/mynah/process/{name}` | Step-by-step execution trace |

## CLI

| Task | Read this skill file |
|------|---------------------|
| Understand architecture / "How does X work?" | `.claude/skills/gitnexus/gitnexus-exploring/SKILL.md` |
| Blast radius / "What breaks if I change X?" | `.claude/skills/gitnexus/gitnexus-impact-analysis/SKILL.md` |
| Trace bugs / "Why is X failing?" | `.claude/skills/gitnexus/gitnexus-debugging/SKILL.md` |
| Rename / extract / split / refactor | `.claude/skills/gitnexus/gitnexus-refactoring/SKILL.md` |
| Tools, resources, schema reference | `.claude/skills/gitnexus/gitnexus-guide/SKILL.md` |
| Index, status, clean, wiki CLI commands | `.claude/skills/gitnexus/gitnexus-cli/SKILL.md` |
| Work in the Session area (52 symbols) | `.claude/skills/generated/session/SKILL.md` |
| Work in the Scripts area (49 symbols) | `.claude/skills/generated/scripts/SKILL.md` |
| Work in the Config area (42 symbols) | `.claude/skills/generated/config/SKILL.md` |
| Work in the Tests area (32 symbols) | `.claude/skills/generated/tests/SKILL.md` |
| Work in the Mynah area (22 symbols) | `.claude/skills/generated/mynah/SKILL.md` |
| Work in the Models area (15 symbols) | `.claude/skills/generated/models/SKILL.md` |
| Work in the Cli area (15 symbols) | `.claude/skills/generated/cli/SKILL.md` |
| Work in the MynahAppTests area (15 symbols) | `.claude/skills/generated/mynahapptests/SKILL.md` |
| Work in the UI area (12 symbols) | `.claude/skills/generated/ui/SKILL.md` |
| Work in the Golden area (12 symbols) | `.claude/skills/generated/golden/SKILL.md` |
| Work in the Api area (10 symbols) | `.claude/skills/generated/api/SKILL.md` |
| Work in the Bearing-teaching area (9 symbols) | `.claude/skills/generated/bearing-teaching/SKILL.md` |
| Work in the Cluster_65 area (8 symbols) | `.claude/skills/generated/cluster-65/SKILL.md` |
| Work in the Cluster_69 area (8 symbols) | `.claude/skills/generated/cluster-69/SKILL.md` |
| Work in the Audio area (8 symbols) | `.claude/skills/generated/audio/SKILL.md` |
| Work in the Stt area (8 symbols) | `.claude/skills/generated/stt/SKILL.md` |
| Work in the Input area (8 symbols) | `.claude/skills/generated/input/SKILL.md` |
| Work in the Cluster_22 area (6 symbols) | `.claude/skills/generated/cluster-22/SKILL.md` |
| Work in the Cluster_67 area (6 symbols) | `.claude/skills/generated/cluster-67/SKILL.md` |
| Work in the Cluster_73 area (6 symbols) | `.claude/skills/generated/cluster-73/SKILL.md` |

<!-- gitnexus:end -->
