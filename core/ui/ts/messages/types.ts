/**
 * The shape of an incoming message and of a handler for one.
 *
 * A payload arrives as untyped JSON from the host, so every handler narrows it
 * itself rather than trusting a declared type.
 */

export type SignalPathNodeConfigUpdateOptions = {
  markDirty?: boolean;
};

/** A decoded message from the backend. Shape depends on `type`. */
export type IncomingPayload = Record<string, unknown>;

export type MessageHandler = (payload: IncomingPayload) => void;
