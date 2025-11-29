#ifndef RECORDER_H
#define RECORDER_H

#include "config.h"

/*
 * Start the recording process.
 * data: Pointer to the CustomData structure.
 * Returns: TRUE if successful, FALSE otherwise.
 */
gboolean start_recording(CustomData *data);

/*
 * Stop the recording process.
 * data: Pointer to the CustomData structure.
 * Returns: TRUE if successful, FALSE otherwise.
 */
gboolean stop_recording(CustomData *data);

/*
 * Helper function to clean up recording branch GStreamer elements asynchronously.
 * user_data: Pointer to the CustomData structure (used in g_idle_add).
 * Returns: G_SOURCE_REMOVE to stop the idle source.
 */
gboolean cleanup_recording_async(gpointer user_data);

#endif // RECORDER_H
