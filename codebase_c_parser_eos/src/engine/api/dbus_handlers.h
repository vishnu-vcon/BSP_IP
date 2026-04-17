/*
 * dbus_service.h — D-Bus Service for Unified Engine
 * ===================================================
 * Port of: unified_engine.py — UnifiedEngineDBus class
 */

#ifndef SMARTIP_DBUS_SERVICE_H
#define SMARTIP_DBUS_SERVICE_H

#include <gio/gio.h>

typedef struct _UnifiedEngine UnifiedEngine;

/* Start the D-Bus service and register methods */
guint dbus_service_start(UnifiedEngine *engine);

#endif /* SMARTIP_DBUS_SERVICE_H */
