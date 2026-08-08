#ifndef REMOTICA_POWER_INHIBIT_H
#define REMOTICA_POWER_INHIBIT_H

/*
 * power_inhibit.h
 * ================
 * Stops the machine suspending itself while a print is running.
 *
 * Why this exists: found the hard way (2026-08-08) on a real print. The
 * PC hit its idle timeout after an hour of nobody touching it and
 * suspended, which froze the backend mid-stream and stopped the print.
 * The print being ruined is the mild half of the problem — the printer
 * does NOT stop when the host goes away. It sits at whatever temperature
 * it was last told, holding a hot nozzle against the part, with nothing
 * supervising it until someone happens to wake the machine.
 *
 * A dedicated printer PC is exactly the machine nobody touches for hours,
 * so this is the default behaviour, not an edge case.
 *
 * How: takes a systemd logind inhibitor lock ("sleep:idle", mode=block)
 * for as long as a print is active, and drops it as soon as the print
 * ends. Deliberately scoped to while-printing rather than always — an
 * idle printer PC is welcome to sleep, and permanently disabling suspend
 * is the machine owner's decision, not ours to make silently.
 *
 * Implemented by running the `systemd-inhibit` tool and holding its child
 * process, rather than by talking to logind over D-Bus directly. That
 * avoids adding libsystemd as a dependency to a project that vendors
 * everything and compiles with no package manager (see the root
 * CLAUDE.md), at the cost of one cheap long-lived child process. On a
 * machine without systemd this degrades to doing nothing, with one log
 * line saying so.
 *
 * NOT a substitute for the machine's own power settings: this only blocks
 * *automatic* idle suspend. Closing a laptop lid, or a human choosing
 * "Suspend", is still honoured — blocking those would be hijacking the
 * machine. See the README for how to disable idle suspend outright.
 */

/* Acquires the lock if `active` and it isn't already held; releases it if
 * not `active` and it is. Idempotent and cheap when nothing changes, so
 * it's safe to call on every tick.
 *
 * Must only be called from one thread (main.c's tick thread) — it keeps
 * unguarded static state and reaps a child process. */
void power_inhibit_set(int active);

/* True while the lock is currently held — reported by GET /api/system so
 * the dashboard can show that sleep is actually being blocked, rather
 * than the user having to take it on trust. */
int power_inhibit_is_held(void);

/* Drops the lock if held. Called on shutdown so the child process never
 * outlives the backend. */
void power_inhibit_shutdown(void);

#endif /* REMOTICA_POWER_INHIBIT_H */
