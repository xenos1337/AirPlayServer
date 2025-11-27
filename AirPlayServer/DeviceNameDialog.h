#pragma once

// Function to show a dialog and get the device name for AirPlay
// Returns the entered name, or NULL if cancelled. Caller must free the returned string.
// If defaultName is provided, it will be pre-filled in the dialog
char* ShowDeviceNameDialog(const char* defaultName);

