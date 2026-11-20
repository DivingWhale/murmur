# Claude Usage Page — Data Contract

The handheld has a **Claude** page that shows the owner's Claude plan usage: the **5-hour session limit** and the **weekly limit**. It shows the same numbers Claude Code's `/usage` command shows. This is plan usage, not API spending.

```
Computer (Claude Code logged in) --collector--> Raspberry Pi --GET--> Handheld
```

| Piece | Location | Status |
|---|---|---|
| Page UI | `prototype/index.html`, spec in `docs/ui-spec.md` §4.10 | Done |
| Mock response | `prototype/api/v1/pages/claude-usage.json` | Static file. The real endpoint must return this shape. |
| Collector | — | **To build** |
| Pi endpoint | — | **To build** |

## Where the numbers come from (open)

The owner does **not** use the Claude API, so the Usage and Cost Analytics API doesn't apply. As far as we know, there is **no documented public API** for personal plan limits. The only known source is Claude Code's `/usage` screen.

The collector's implementer should:

1. **Find a supported, non-interactive way** to read the same numbers on the computer where Claude Code is logged in. Check the current Claude Code docs and CLI options first.
2. **Avoid anything fragile or against the terms of service.** Don't scrape claude.ai with stored browser cookies, and don't call undocumented endpoints with the user's OAuth token. If no supported way exists, say so rather than shipping something fragile.
3. **Keep credentials on the computer.** Only the numbers below are sent to the Pi.

## `GET /v1/pages/claude-usage` (handheld → Pi)

The handheld calls this during each sync. The Pi returns the last values it received.

```json
{
  "updated_at": "2026-11-20T10:40:00+01:00",
  "session": { "used_pct": 42, "resets_at": "2026-11-20T14:30:00+01:00" },
  "week":    { "used_pct": 67, "resets_at": "2026-11-23T09:00:00+01:00" }
}
```

| Field | Type | Notes |
|---|---|---|
| `updated_at` | ISO 8601 with offset | When the collector read the numbers. Shown as "Updated 10:40". |
| `session.used_pct` | number 0–100 | Percent of the 5-hour limit used. |
| `session.resets_at` | ISO 8601 with offset | When the 5-hour window resets. |
| `week.used_pct` | number 0–100 | Percent of the weekly limit used. |
| `week.resets_at` | ISO 8601 with offset | When the weekly limit resets. |

- **Missing block:** either block may be left out if it's unknown, and the device hides that row.
- **No data:** an empty response or `404` shows "No usage data yet".
- **Times:** the device uses the wall-clock time written in each string. It doesn't convert time zones.
- **Countdowns:** the device works out "in 3 h 50 m" from its own clock. The prototype uses `updated_at` instead.

## Display rules (already built)

- **Colour:** the percentage and bar turn orange (due) at **≥ 80%** and red (rec) at **≥ 95%**.
- **Freshness:** the data only refreshes when the device syncs, so the page always shows `Updated HH:MM`. See the sync note in `docs/ui-spec.md`.
