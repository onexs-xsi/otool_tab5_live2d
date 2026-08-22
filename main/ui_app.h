#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the Agent workspace and start its UI/audio interaction workers.
 *
 * @param tab5_comp Initialized m5::tab5::otool_tab5_component pointer. The C
 *                  boundary keeps the concrete C++ board type out of this header.
 */
void ui_app_start(void *tab5_comp);

#ifdef __cplusplus
}
#endif
