/**
 * The event the designer fires when it has changed the layout library, so the
 * settings list and anything else showing layouts can redraw.
 *
 * It lives here rather than as a static on the modal because the store fires it
 * too, and a store importing the modal for one string would be a cycle.
 */

export const LAYOUT_LIBRARY_CHANGED_EVENT = "layout-library-changed";
