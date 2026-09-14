#pragma once

#define IDI_APPICON 101

#define IDM_ENABLED 40001
#define IDM_NAVIGATE_UP 40002
#define IDM_RUN_AT_STARTUP 40003
#define IDM_OPEN_LOG 40004
#define IDM_DIAGNOSTICS 40005
#define IDM_EXIT 40006
#define IDM_COPY_LOG_PATH 40007
#define IDM_SUBFOLDER_TIPS 40008
#define IDM_FILE_PREVIEWS 40009
#define IDM_MEDIA_PLAYBACK 40010
#define IDM_RESET_PREVIEWS 40011
#define IDM_MEDIA_AUTOPLAY 40012
#define IDM_FOLDER_COUNTS 40013
#define IDM_REMEMBER_RECENT 40014
#define IDM_CLEAR_RECENT 40015
#define IDM_SETTINGS 40016

// The recent folders themselves, one command each. A range rather than an id
// per entry, since the list is built fresh every time the menu opens.
#define IDM_RECENT_FIRST 40100
#define IDM_RECENT_LAST 40131
