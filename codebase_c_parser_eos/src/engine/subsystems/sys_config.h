/*
 * sys_config.h — Configuration Subsystem
 * ========================================
 * Handles 3-tier fallback loading and lens-level persistence.
 */

#ifndef SMARTIP_SYS_CONFIG_H
#define SMARTIP_SYS_CONFIG_H

#include "engine_types.h"

/**
 * sys_config_load_and_apply:
 * @e: The UnifiedEngine instance.
 *
 * Implements the 3-tier fallback logic:
 *   1. CLI Provided Path (-c)
 *   2. User Persistence (config/user_config.json)
 *   3. Manufacturer Defaults (config/manufacturer_defaults.json)
 *
 * It also ensures that top-level lens attributes (cairo, overlay) 
 * are correctly forwarded to the configuration engine.
 */
void sys_config_load_and_apply(UnifiedEngine *e);

#endif /* SMARTIP_SYS_CONFIG_H */
