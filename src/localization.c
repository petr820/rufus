/*
 * Rufus: The Reliable USB Formatting Utility
 * Localization functions, a.k.a. "Everybody is doing it wrong but me!"
 * Copyright © 2013-2014 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Memory leaks detection - define _CRTDBG_MAP_ALLOC as preprocessor macro */
#ifdef _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#endif

#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stddef.h>

#include "rufus.h"
#include "resource.h"
#include "msapi_utf8.h"
#include "localization.h"
#include "localization_data.h"

/* 
 * List of supported locale commands, with their parameter syntax:
 *   c control ID (no space, no quotes)
 *   s: quoted string
 *   i: 32 bit signed integer
 *   u: 32 bit unsigned CSV list
 * Remember to update the size of the array in localization.h when adding/removing elements
 */
const loc_parse parse_cmd[9] = {
	// Translation name and Windows LCIDs it should apply to
	{ 'l', LC_LOCALE, "ssu" },	// l "en_US" "English (US)" 0x0009,0x1009
	// Base translation to add on top of (eg. "English (UK)" can be used to build on top of "English (US)"
	{ 'b', LC_BASE, "s" },		// b "en_US"
	// Version to use for the localization commandset and API
	{ 'v', LC_VERSION, "u" },	// v 1.0.2
	// Translate the text control associated with an ID
	{ 't', LC_TEXT, "cs" },		// t IDC_CONTROL "Translation"
	// Set the section/dialog to which the next commands should apply
	{ 'g', LC_GROUP, "c" },		// g IDD_DIALOG
	// Resize a dialog (dx dy pixel increment)
	{ 's', LC_SIZE, "cii" },	// s IDC_CONTROL +10 +10
	// Move a dialog (dx dy pixed displacement)
	{ 'm', LC_MOVE, "cii" },	// m IDC_CONTROL -5 0
	// Set the font to use for the text controls that follow
	// Use f "Default" 0 to reset the font
	{ 'f', LC_FONT, "si" },		// f "MS Dialog" 10
	// Set translations attributes such as right-to-left, numerals to use, etc
	{ 'a', LC_ATTRIBUTES, "s" },	// a "ra"
};

/* Hash table for reused translation commands */
static htab_table htab_loc = HTAB_EMPTY;

/* Globals */
int    loc_line_nr;
struct list_head locale_list = {NULL, NULL};
char   *loc_filename = NULL, *embedded_loc_filename = "embedded.loc";

/* Message table */
char* default_msg_table[MSG_MAX-MSG_000] = {0};
char* current_msg_table[MSG_MAX-MSG_000] = {0};
char** msg_table = NULL;

static void mtab_destroy(BOOL reinit)
{
	size_t j;
	for (j=0; j<MSG_MAX-MSG_000; j++) {
		safe_free(current_msg_table[j]);
		if (!reinit)
			safe_free(default_msg_table[j]);
	}
}

/*
 * Add a localization command to a dialog/section
 */
void add_dialog_command(int index, loc_cmd* lcmd)
{
	char str[128];
	loc_cmd* htab_lcmd;
	uint32_t i;
	if ((lcmd == NULL) || (lcmd->txt[0] == NULL) || (index < 0) || (index >= ARRAYSIZE(loc_dlg))) {
		uprintf("localization: invalid parameter for add_dialog_command\n");
		return;
	}

	// A dialog command must be unique, so we use a hash to identify any
	// command that may already have been populated, and ensure it is replaced
	// with the new one.
	// Two dialogs may have different "m IDC_CONTROL" lines, and also
	// "m IDC_CONTROL" and "t IDC_CONTROL" are separate, so we compute two more
	// unique identifiers for dialog and command at the beginning of our string
	str[0] = index + 0x30;
	str[1] = lcmd->command + 0x30;
	safe_strcpy(&str[2], sizeof(str)-2, lcmd->txt[0]);
	i = htab_hash(str, &htab_loc);
	if (i != 0) {
		htab_lcmd = (loc_cmd*)(htab_loc.table[i].data);
		if (htab_lcmd != NULL) {
			list_del(&(htab_lcmd->list));
			free_loc_cmd(htab_lcmd);
		}
		htab_loc.table[i].data = (void*)lcmd;
	}
	list_add(&lcmd->list, &loc_dlg[index].list);
}

/*
 * Add a translated message to a direct lookup table
 */
void add_message_command(loc_cmd* lcmd)
{
	if (lcmd == NULL) {
		uprintf("localization: invalid parameter for add_message_command\n");
		return;
	}

	if ((lcmd->ctrl_id <= MSG_000) || (lcmd->ctrl_id >= MSG_MAX)) {
		uprintf("localization: invalid MSG_ index\n");
		return;
	}
	
	safe_free(msg_table[lcmd->ctrl_id-MSG_000]);
	msg_table[lcmd->ctrl_id-MSG_000] = lcmd->txt[1];
	lcmd->txt[1] = NULL;	// String would be freed after this call otherwise
}

void free_loc_cmd(loc_cmd* lcmd)
{
	if (lcmd == NULL)
		return;
	safe_free(lcmd->txt[0]);
	safe_free(lcmd->txt[1]);
	safe_free(lcmd->unum);
	free(lcmd);
}

void free_dialog_list(void)
{
	size_t i = 0;
	loc_cmd *lcmd, *next;

	for (i=0; i<ARRAYSIZE(loc_dlg); i++) {
		if (list_empty(&loc_dlg[i].list))
			continue;
		list_for_each_entry_safe(lcmd, next, &loc_dlg[i].list, loc_cmd, list) {
			list_del(&lcmd->list);
			free_loc_cmd(lcmd);
		}
	}
}

void free_locale_list(void)
{
	loc_cmd *lcmd, *next;

	list_for_each_entry_safe(lcmd, next, &locale_list, loc_cmd, list) {
		list_del(&lcmd->list);
		free_loc_cmd(lcmd);
	}
}

/*
 * Init/destroy our various localization lists
 * keep the locale list and filename on reinit
 */
void _init_localization(BOOL reinit) {
	size_t i;
	for (i=0; i<ARRAYSIZE(loc_dlg); i++)
		list_init(&loc_dlg[i].list);
	if (!reinit)
		list_init(&locale_list);
	htab_create(LOC_HTAB_SIZE, &htab_loc);
}

void _exit_localization(BOOL reinit) {
	if (!reinit) {
		free_locale_list();
		if (loc_filename != embedded_loc_filename)
			safe_free(loc_filename);
	}
	free_dialog_list();
	mtab_destroy(reinit);
	htab_destroy(&htab_loc);
}

/*
 * Validate and store localization command data
 */
BOOL dispatch_loc_cmd(loc_cmd* lcmd)
{
	size_t i;
	static int dlg_index = 0;
	loc_cmd* base_locale = NULL;
	const char* msg_prefix = "MSG_";

	if (lcmd == NULL)
		return FALSE;

	if (lcmd->command <= LC_TEXT) {
		// Any command up to LC_TEXT takes a control ID in text[0]
		if (safe_strncmp(lcmd->txt[0], msg_prefix, 4) == 0) {
			// The unneeded NULL check is to silence a VS warning
			if ((lcmd->txt[0] == NULL) || (lcmd->command != LC_TEXT)) {
				luprint("only the [t]ext command can be applied to a message (MSG_###)\n");
				goto err;
			}
			// Try to convert the numeric part of a MSG_#### to a numeric
			lcmd->ctrl_id = MSG_000 + atoi(&(lcmd->txt[0][4]));
			if (lcmd->ctrl_id == MSG_000) {
				// Conversion could not be performed
				luprintf("failed to convert the numeric value in '%'\n", lcmd->txt[0]);
				goto err;
			}
			add_message_command(lcmd);
			free_loc_cmd(lcmd);
			return TRUE;
		}
		for (i=0; i<ARRAYSIZE(control_id); i++) {
			if (safe_strcmp(lcmd->txt[0], control_id[i].name) == 0) {
				lcmd->ctrl_id = control_id[i].id;
				break;
			}
		}
		if (lcmd->ctrl_id < 0) {
			luprintf("unknown control '%s'\n", lcmd->txt[0]);
			goto err;
		}
	}

	// Don't process UI commands when we're dealing with the default
	if (msg_table == default_msg_table) {
		free_loc_cmd(lcmd);
		return TRUE;
	}

	switch(lcmd->command) {
	// NB: For commands that take an ID, ctrl_id is always a valid index at this stage
	case LC_TEXT:
	case LC_MOVE:
	case LC_SIZE:
		add_dialog_command(dlg_index, lcmd);
		break;
	case LC_GROUP:
		if ((lcmd->ctrl_id-IDD_DIALOG) > ARRAYSIZE(loc_dlg)) {
			luprintf("'%s' is not a group ID\n", lcmd->txt[0]);
			goto err;
		}
		dlg_index = lcmd->ctrl_id - IDD_DIALOG;
		free_loc_cmd(lcmd);
		break;
	case LC_BASE:
		base_locale = get_locale_from_name(lcmd->txt[0], FALSE);
		if (base_locale != NULL) {
			uprintf("localization: using locale base '%s'\n", lcmd->txt[0]);
			get_loc_data_file(NULL, base_locale);
		} else {
			luprintf("locale base '%s' not found - ignoring", lcmd->txt[0]);
		}
		free_loc_cmd(lcmd);
		break;
	default:
		free_loc_cmd(lcmd);
		break;
	}
	return TRUE;

err:
	free_loc_cmd(lcmd);
	return FALSE;
}

/*
 * Apply stored localization commands to a specific dialog
 * If hDlg is NULL, apply the commands against an active Window
 */
void apply_localization(int dlg_id, HWND hDlg)
{
	loc_cmd* lcmd;
	HWND hCtrl = NULL;
	int id_start = IDD_DIALOG, id_end = IDD_DIALOG + ARRAYSIZE(loc_dlg);

	if ((dlg_id >= id_start) && (dlg_id < id_end)) {
		// If we have a valid dialog_id, just process that one dialog
		id_start = dlg_id;
		id_end = dlg_id + 1;
		if (hDlg != NULL) {
			loc_dlg[dlg_id-IDD_DIALOG].hDlg = hDlg;
		}
	}

	for (dlg_id = id_start; dlg_id < id_end; dlg_id++) {
		hDlg = loc_dlg[dlg_id-IDD_DIALOG].hDlg;
		if ((!IsWindow(hDlg)) || (list_empty(&loc_dlg[dlg_id-IDD_DIALOG].list)))
			continue;

		list_for_each_entry(lcmd, &loc_dlg[dlg_id-IDD_DIALOG].list, loc_cmd, list) {
			if (lcmd->command <= LC_TEXT) {
				if (lcmd->ctrl_id == dlg_id) {
					if ((dlg_id == IDD_DIALOG) && (lcmd->txt[1] != NULL) && (lcmd->txt[1][0] != 0)) {
						loc_line_nr = lcmd->line_nr;
						luprint("operation forbidden (main dialog title cannot be changed)");
						continue;
					}
					hCtrl = hDlg;
					if (dlg_id == IDD_DIALOG)
						hDlg = NULL;
				} else {
					hCtrl = GetDlgItem(hDlg, lcmd->ctrl_id);
				}
				if (hCtrl == NULL) {
					loc_line_nr = lcmd->line_nr;
					luprintf("control '%s' is not part of dialog '%s'\n",
						lcmd->txt[0], control_id[dlg_id-IDD_DIALOG].name);
				}
			}

			switch(lcmd->command) {
			case LC_TEXT:
				if (hCtrl != NULL) {
					if ((lcmd->txt[1] != NULL) && (lcmd->txt[1][0] != 0))
						SetWindowTextU(hCtrl, lcmd->txt[1]);
				}
				break;
			case LC_MOVE:
				if (hCtrl != NULL) {
					ResizeMoveCtrl(hDlg, hCtrl, lcmd->num[0], lcmd->num[1], 0, 0);
				}
				break;
			case LC_SIZE:
				if (hCtrl != NULL) {
					ResizeMoveCtrl(hDlg, hCtrl, 0, 0, lcmd->num[0], lcmd->num[1]);
				}
				break;
			}
		}
	}
}

/*
 * This function should be called when a localized dialog is destroyed
 * NB: we can't use isWindow() against our existing HWND to avoid this call
 * as handles are recycled.
 */
void reset_localization(int dlg_id)
{
	loc_dlg[dlg_id-IDD_DIALOG].hDlg = NULL;
}

/*
 * Produce a formatted localized message.
 * Like printf, this call takes a variable number of argument, and uses
 * the message ID to identify the formatted message to use.
 * Uses a rolling list of buffers to allow concurrency
 * TODO: use dynamic realloc'd buffer in case LOC_MESSAGE_SIZE is not enough
 */
char* lmprintf(int msg_id, ...)
{
	static int buf_id = 0;
	static char buf[LOC_MESSAGE_NB][LOC_MESSAGE_SIZE];
	char *format = NULL;
	va_list args;
	buf_id %= LOC_MESSAGE_NB;
	buf[buf_id][0] = 0;

	if ((msg_id > MSG_000) && (msg_id < MSG_MAX)) {
		format = msg_table[msg_id - MSG_000];
	}

	if (format == NULL) {
		safe_sprintf(buf[buf_id], LOC_MESSAGE_SIZE-1, "MSG_%03d UNTRANSLATED", msg_id - MSG_000);
	} else {
		va_start(args, msg_id);
		safe_vsnprintf(buf[buf_id], LOC_MESSAGE_SIZE-1, format, args);
		va_end(args);
		buf[buf_id][LOC_MESSAGE_SIZE-1] = '\0';
	}
	return buf[buf_id++];
}

/*
 * Display a localized message on the status bar as well as its English counterpart in the
 * log (if debug is set). If duration is non zero, ensures that message is displayed for at
 * least duration ms, regardless of any other incoming message
 */
static BOOL bStatusTimerArmed = FALSE;
char szStatusMessage[256] = { 0 };
static void CALLBACK PrintStatusTimeout(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	bStatusTimerArmed = FALSE;
	// potentially display lower priority message that was overridden
	SendMessageLU(GetDlgItem(hMainDialog, IDC_STATUS), SB_SETTEXTW, SBT_OWNERDRAW, szStatusMessage);
	KillTimer(hMainDialog, TID_MESSAGE);
}

void PrintStatus(unsigned int duration, BOOL debug, int msg_id, ...)
{
	char *format = NULL, buf[sizeof(szStatusMessage)];
	va_list args;

	if (msg_id < 0) {
		//A negative msg_id clears the status
		SendMessageLU(GetDlgItem(hMainDialog, IDC_STATUS), SB_SETTEXTW, SBT_OWNERDRAW, "");
		return;
	}

	if ((msg_id <= MSG_000) || (msg_id >= MSG_MAX)) {
		uprintf("PrintStatus: invalid MSG_ID\n");
		return;
	}

	format = msg_table[msg_id - MSG_000];
	if (format == NULL) {
		safe_sprintf(szStatusMessage, sizeof(szStatusMessage), "MSG_%03d UNTRANSLATED", msg_id - MSG_000);
		return;
	}

	va_start(args, msg_id);
	safe_vsnprintf(szStatusMessage, sizeof(szStatusMessage), format, args);
	va_end(args);
	szStatusMessage[sizeof(szStatusMessage)-1] = '\0';

	if ((duration) || (!bStatusTimerArmed)) {
		SendMessageLU(GetDlgItem(hMainDialog, IDC_STATUS), SB_SETTEXTW, SBT_OWNERDRAW, szStatusMessage);
	}

	if (duration) {
		SetTimer(hMainDialog, TID_MESSAGE, duration, PrintStatusTimeout);
		bStatusTimerArmed = TRUE;
	}

	if (debug) {
		format = default_msg_table[msg_id - MSG_000];
		if (format == NULL) {
			safe_sprintf(buf, sizeof(szStatusMessage), "(default) MSG_%03d UNTRANSLATED", msg_id - MSG_000);
			return;
		}
		va_start(args, msg_id);
		safe_vsnprintf(buf, sizeof(szStatusMessage)-1, format, args);
		va_end(args);
		uprintf(buf);
	}
}

/*
 * These 2 functions are used to set the current locale
 * If fallback is true, the call will fall back to use the first
 * translation listed in the loc file
 */
loc_cmd* get_locale_from_lcid(int lcid, BOOL fallback)
{
	loc_cmd* lcmd = NULL;
	int i;

	if (list_empty(&locale_list)) {
		uprintf("localization: the locale list is empty!\n");
		return NULL;
	}

	list_for_each_entry(lcmd, &locale_list, loc_cmd, list) {
		for (i=0; i<lcmd->unum_size; i++) {
			if (lcmd->unum[i] == lcid) {
				return lcmd;
			}
		}
	}

	if (!fallback)
		return NULL;

	lcmd = list_entry(locale_list.next, loc_cmd, list);
	// If we couldn't find a supported locale, just pick the first one (usually English)
	uprintf("localization: could not find locale for LCID: 0x%04X. Will default to '%s'\n", lcid, lcmd->txt[0]);
	return lcmd;
}

loc_cmd* get_locale_from_name(char* locale_name, BOOL fallback)
{
	loc_cmd* lcmd = NULL;

	if (list_empty(&locale_list)) {
		uprintf("localization: the locale list is empty!\n");
		return NULL;
	}

	list_for_each_entry(lcmd, &locale_list, loc_cmd, list) {
		if (safe_strcmp(lcmd->txt[0], locale_name) == 0)
			return lcmd;
	}

	if (!fallback)
		return NULL;

	lcmd = list_entry(locale_list.next, loc_cmd, list);
	uprintf("localization: could not find locale for name '%s'. Will default to '%s'\n", locale_name, lcmd->txt[0]);
	return lcmd;
}

/* 
 * This call is used to toggle the issuing of messages with the default locale
 * (usually en-US) instead of the current (usually non en) one.
 */
void toggle_default_locale(void)
{
	static char** old_msg_table = NULL;

	if (old_msg_table == NULL) {
		old_msg_table = msg_table;
		msg_table = default_msg_table;
	} else {
		msg_table = old_msg_table;
		old_msg_table = NULL;
	}
}
