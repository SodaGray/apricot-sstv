/* SPDX-License-Identifier: GPL-3.0-only */
/* version.h -- the app's name, version and website: change them HERE.
 *
 * APP_NAME     strip title (falls back to APP_SHORT if it doesn't fit) and splash
 * APP_VERSION  shown on the splash ("Apricot SSTV RX v1.0"); keep Info.plist's
 *              CFBundleShortVersionString in step with it
 * APP_WEBSITE  second line of the splash
 *
 * The home-screen name and icon are in Info.plist (CFBundleName, HBIconGlyph,
 * HBIconColor). */
#ifndef APP_VERSION_H
#define APP_VERSION_H

#define APP_NAME     "Apricot SSTV"
#define APP_SHORT    "Apricot"
#define APP_VERSION  "1.0"
#define APP_WEBSITE  "https://bd1fpn.cn"

#define APP_SPLASH   APP_NAME " RX v" APP_VERSION        /* "Apricot SSTV RX v1.0" */

#endif
