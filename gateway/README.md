Raspberry Pi gateway. Mosquitto broker configured and tested.

## Files

| File | Role |
|------|------|
| `fusion.py` | MQTT subscriber, majority-vote fusion, publishes fused result |
| `db.py` | SQLite persistence; owns the session open/close transitions |
| `schema.sql` | Table definitions — the read-only contract with the dashboard |
| `inspect_db.py` | Read-only console view of a live or sample database |
| `tools/gen_sample_db.py` | 12 h of synthetic history for dashboard development |

## Run

```bash
pip3 install paho-mqtt
python3 fusion.py [db_path]      # defaults to gateway/parking.db
python3 inspect_db.py --watch    # read-only live view, separate shell
```

`schema.sql` must sit next to `db.py` — it is read and replayed on every
startup, not only when the database is created. Every statement is
`IF NOT EXISTS`, so replaying is a no-op and adding a table to the schema
takes effect on restart with no migration step.

The corollary is that `IF NOT EXISTS` creates but never alters: a column
added to an existing table will not appear in a `parking.db` created before
it. Delete and recreate the file in that case.

## Notes

Voting is a sliding window: each node's latest reading is kept and readings
older than `STALE_SEC` are dropped from the vote, so a dead node degrades
`votes_total` from 3 to 2 instead of stalling the vote.

An even split (only reachable at 2 live nodes) holds the previous state
rather than resolving to EMPTY. Resolving to EMPTY let a single misread on
one survivor cut one parking session into two, and the demo deliberately
unplugs a node, so the 2-node path is a normal operating mode.

Timestamps are gateway receive time — Phase 6 node firmware has no RTC or
NTP, so nodes cannot stamp their own reports.

The dashboard should open the database read-only:

```python
sqlite3.connect(f"file:{path}?mode=ro", uri=True)
```

WAL is enabled, so those reads are not blocked by gateway writes.
