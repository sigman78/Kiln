# API friction log

**Purpose.** The owner integrates kiln into an external project in parallel with development.
Every place where the API is awkward, surprising, missing something, or forces a workaround goes
into this table. It is the input for the API review before each milestone closes and before v0.5
is tagged.

Add one row per friction point. Keep "Friction" to what happened, and "Proposed change" to one
concrete suggestion. Status is one of: Open, Accepted, Rejected (with reason), Done (with version).

| Date | Reporter | Area | Friction | Proposed change | Status |
|---|---|---|---|---|---|

Review this table at the end of every milestone; resolved rows that change the API get a
`CHANGELOG.md` entry with migration notes.
