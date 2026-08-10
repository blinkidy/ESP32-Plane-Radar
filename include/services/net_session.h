/*
 * Mutual exclusion for outbound TLS.
 *
 * Two mbedTLS contexts plus the 115 KB frame sprite do not fit in the C3's
 * heap, so the ADS-B poll (main task) and the route lookup (background task)
 * must never have a session open at the same time.
 */
#pragma once

namespace services::net {

/** Create the lock. Call once in setup(), before any task that uses it. */
void sessionInit();

/**
 * Take the lock without blocking. For the main task: the UI redraws from the
 * poll hook, so it must never wait here — skip the cycle and retry instead.
 */
bool trySession();

/** Take the lock, waiting as long as needed. For background tasks only. */
void acquireSession();

void releaseSession();

/** Advisory: true if someone holds the lock. Racy by nature — scheduling only. */
bool sessionBusy();

/** Scope guard for a lock already taken by trySession()/acquireSession(). */
class SessionLease {
 public:
  SessionLease() = default;
  ~SessionLease() { releaseSession(); }
  SessionLease(const SessionLease&) = delete;
  SessionLease& operator=(const SessionLease&) = delete;
};

}  // namespace services::net
