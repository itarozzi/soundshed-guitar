/**
 * The app-setting keys the browser persists under, and the sizing constants
 * behind the virtualised folder list.
 */

export const RESOURCE_FAVORITES_SETTING = "resources.favorites";

export const FOLDER_ROOTS_SETTING = "resources.folderBrowser.roots";

export const FOLDER_ACTIVE_ROOT_SETTING = "resources.folderBrowser.activeRootId";

// Last browsed folder per effect role (e.g. "ir-cab", "nam-amp") so an IR Cab
// picker reopens where IR Cab browsing left off, independent of NAM browsing.
export const FOLDER_LAST_LOCATIONS_SETTING = "resources.folderBrowser.lastLocationByContext";

export const DEFAULT_RESOURCE_CONTEXT_KEY = "default";

export const FOLDER_VIRTUAL_GAP = 6;

export const FOLDER_VIRTUAL_OVERSCAN = 6;

export const FOLDER_VIRTUAL_ESTIMATED_DIR_HEIGHT = 44;

export const FOLDER_VIRTUAL_ESTIMATED_FILE_HEIGHT = 62;
